# ALH Frame

手机上的补帧工具。选一个视频，按 2× / 3× / 4× 输出高帧率版本，全程在本机跑，不联网、不上传。

24fps 的素材补到 48 / 72 / 96fps，30fps 的补到 60 / 90 / 120fps。1080p 的片子按 128 ms/输出帧算，十分钟的 2× 成品在八核手机上大约二十多分钟，具体看运动复杂度。

- 包名 `com.alh.frame`，版本 1.1，minSdk 24，targetSdk 28
- 单 Activity，无第三方 Java 依赖，界面是代码搭的
- 引擎自带 ffmpeg 9.0.2，切成进程池并行跑，段编完立刻并合，不等全片

## 算法

补帧本身用的是 FFmpeg 的 `minterpolate`，工作模式选 MCI（运动补偿插值）：

```
mi_mode=mci:mc_mode=aobmc:me_mode=bilat:me=epzs:search_param=32
:mb_size=16:vsbmc=1:scd=fdiff:scd_threshold=8
```

一帧插值分四步。先在前后两个关键帧之间按 16×16 宏块做双向运动搜索（`epzs`，搜索半径 32），得到每块的前向/后向运动矢量；`aobmc` 用相邻块的运动矢量做重叠补偿，把块边界抹平；`bilat` 决定中间帧每个像素取哪个时间位置的运动矢量，比 `bidir` 少一半匹配开销；最后 `scd=fdiff` 盯帧差，突跳就整帧退回混合，避免场景切换时撕裂。

为什么选 `bilat` 而不是默认的 `bidir`，是量出来的。720p、30→60、60 帧真值的对照：

| 模式 | 耗时 | SSIM | PSNR |
| --- | --- | --- | --- |
| 直接复制帧 | 350 ms | 0.9754 | 30.45 |
| 两帧混合 | 389 ms | 0.9758 | 32.96 |
| mci bidir | 6466 ms | 0.9883 | 36.94 |
| mci bilat | 3855 ms | 0.9875 | 36.80 |

SSIM 少 0.0008，时间少 40%。4× 的差距更大：bidir 8214 ms / 0.9692，bilat 5225 ms / 0.9768，又快又好。所以默认走 `bilat`，`search_param` 保持 32。

### 并行只能靠多进程

`minterpolate` 自己基本不吃多线程。同一段 720p 素材，`-threads 1` 跑 10148 ms，`-threads 8` 跑 9602 ms，八倍的线程只换来 5%。所以想快，只能把视频切成几段，起几个 ffmpeg 同时跑。

切法按时间轴，不按帧数：

```
s0 = dur * i / N
s1 = dur * (i + 1) / N
本段输出帧数 = round(s1 * outFps) - round(s0 * outFps)
```

按帧数切会因为源帧率取整而整体错位，一错就表现为「时长变短、画面变快」。

每段的命令里有两处容易踩的坑，都是实测踩出来的：

- 时间戳用滤镜级偏移 `setpts=PTS-STARTPTS+F0/(fps*TB)`，不用 muxer 的 `-output_ts_offset`。后者会让第一段从 0.033333 s 起步、段边界重复一帧，600 帧的片子只剩 9.9667 s。
- 去掉 `-r outFps`，加 `-bf 0`。`minterpolate` 已经定好输出帧率，再给 `-r` 会为时间空洞补帧；有 B 帧时，TS 会把首帧时间戳按重排延迟抬起，各段抬的量还不一样，拼起来就错位。

### 段文件与并合

段文件写 `mpegts`（`c000.ts`、`c001.ts`…），不用 mp4。TS 可以按字节直接接起来，省掉一次 concat 解封装。试过 `-f h264` 裸流，600 帧的东西 remux 之后只剩 598 帧，H.264 解复用器在 SPS 重复处丢帧，弃用。

并合是流式的。`Assembler` 用 `TreeMap` 存已完成的段，只把 `next` 序号那一段追加进 `acc.ts`，追加完立刻删掉段文件。所以任何一个时刻，盘上只有「已并合的部分」加「正在跑的几段」，不需要攒齐全片再拼。

### 并发数怎么定

单路内存预算按 1080p ≈ 790 MB 估：`250 + 260 × 百万像素`。再用可用内存的 35% 去除，得到能开几路：

```
workers = min(段数, 可用内存 × 0.35 / 单路预算)
segments = max(workers, ceil(时长 / 30))
```

段数和并发是分开的。段数按 20 秒一段切，可以远多于并发，这样每一段结束就能立刻还掉内存和磁盘。1080p 长片在有 5 GB 可用内存的机器上开 2 到 3 路，2K 素材一路要 1.2 GB，通常只开得动 2 路。

宽高不是 16 的整数倍时不要补边。我试过 `pad` 到 16 的倍数再 `crop` 回来，在 1080×1620 上 6 段挂 5 段，退出码 139；`pad=1080:1620`（原尺寸空操作）也崩。结论是 `pad` 和 `minterpolate` 一起用就会踩坏内存，跟宽高对不齐没关系，所以这条链上不补边，直接按原始尺寸算。

### 时间戳

每段前面加 `-ss` 只是裁剪，时间戳必须重建，否则段与段之间会归零，播放器上就是每秒抖一下。做法是算完之后 `setpts=N/(输出帧率*TB) + 首帧序号/(输出帧率*TB)`，按帧序号硬算，不用 `PTS-STARTPTS` 加偏移（那个写法下片尾克隆帧的时间戳会被拉开，实测出现过 0.075 s 的毛刺间隔）。

### 失败之后

单段失败会自动降级重跑，一级比一级保守：原参数 → `-threads 1` → `-threads 1 -x264-params asm=c -cpuflags 0`。第三级连 ffmpeg 自身的 SIMD 都关掉，慢，但保证能出片。

### 精度核对

10 秒、300 帧、6 段的 2× 测试：输出 600 帧、10.000 s、60/1 fps，音轨 10.000 s。另用 8 位条码在每帧里编上源帧号做逐帧校验，第 2k 个输出帧正好是源第 k 帧，中间没有漂移。

## ALH Frame Standard 内核

`app/assets/alh/alhift-<abi>` 是自己写的插帧器，单文件 C++17，不依赖任何推理框架。它跟 Lite 的区别是运动估计和合成全部自己实现：

1. 2×2 盒采样建 3 到 4 层金字塔，每层做 8×8 块的 SAD 匹配，粗层搜索半径 8，往下逐层收窄到 2，最细层 3，两遍搜索，矢量幅度按 `0.02*(|dx|+|dy|)` 罚一下。
2. 矢量场上采样后做 3×3 分量中值，去掉孤立错矢量。
3. 按保留下来的匹配代价算置信度 `c = 1/(1+meanSAD²/900)`，合成时 `p0 = 双线性(a, x-vt)`、`p1 = 双线性(b, x+v(1-t))`，权重 `w0 = (1-t)(0.35+0.65c)`、`w1 = t(0.35+0.65c)`。
4. 去重复帧：相邻帧按 4 像素采样算平均绝对差，小于 0.8 就判定重复，直接复制当前帧跳过一次估计。

管线是 `ffmpeg 解码 → alhift → ffmpeg 编码`，三段用 `sh` 管道串起来，中间不落盘：

```
ffmpeg -ss .. -t .. -i src -an -f rawvideo -pix_fmt yuv420p -
  | alhift - W H N - 2 4
  | ffmpeg -f rawvideo -s WxH -r <输出帧率> -i - -c:v libx264 ... -f mpegts out.ts
```

实测（720×1280 与 360×640 合成真值，只算插帧不算编解码）：

| 素材 | 做法 | SSIM | PSNR | 耗时 |
| --- | --- | --- | --- | --- |
| 360×640 | Pro | 0.9907 | 39.79 | 0.55 s |
| 360×640 | Lite | 0.9864 | 36.01 | 1.03 s |
| 1280×720 | Pro | 0.9837 | 40.58 | 2.07 s |
| 1280×720 | Lite | 0.9864 | 43.62 | 2.30 s |

单文件 C++17，可以直接拿去当命令行工具用，接口与编译方式见 [native/README.md](native/README.md)。

低分辨率下 Pro 领先约 3.8 dB，速度还快一倍；720p 上还差 Lite 约 3 dB，差距集中在运动区的矢量精度（运动区平均误差 3.64 对 1.86，静止区反而更好：0.67 对 0.84）。试过双向场、多假设、4×4 精修、16px 块、更大搜索半径，全部没有正收益，所以这一版把 Pro 定位成「低分辨率与老片更清楚、更快」，高分辨率继续用 Lite。Pro 某一段跑不动时会自动退到 Lite 出片，不让任务整体失败。

### 第三档：Standard v2（稠密光流）

同一份核心里多一条逐像素路线：先用块匹配拿到大位移，再在 1/8→1/2 分辨率上做高斯-牛顿光流迭代（每轮 [1 2 1] 平滑一次），最后回到全分辨率细化两轮。矢量场连续，边缘和渐变的过渡更顺，代价是细节略软。

| 素材 | Standard（块匹配） | Standard v2（光流） | Lite | 耗时 v2 / Standard |
| --- | --- | --- | --- | --- |
| foreman 352×288 实拍 15→30 | 0.9497 | 0.9433 | 0.9486 | 4.4 s / 2.1 s |

在真实拍摄的 foreman 上，光流档比块匹配低 0.006 SSIM，速度慢一倍多。按「效果够好且慢得不多才升 Pro」的标准，它还不够格，所以挂成第三档实验项，界面上如实标注。想试的话：下拉 → `ALH Frame Standard v2`。

## 功能

| 项 | 说明 |
| --- | --- |
| 输入 | MP4 / MOV / MKV 等 ffmpeg 能解的容器，横竖屏都行（按 rotation 元数据交换宽高） |
| 倍率 | 2× / 3× / 4×，输出帧率 = 源帧率 × 倍率 |
| 音轨 | 优先 `-c:a copy`；失败重试 AAC 160k；再失败输出无音轨。绝不加 `-shortest`，源音轨短一截不会把画面截掉 |
| 输出 | `Movies/ALH Frame/ALHFrame_<时间戳>_<倍率>x.mp4`，H.264 High，CRF 19，yuv420p，`+faststart` |
| 进度 | 逐帧显示「第 N / 总数 帧」，多路并发时按已完成帧数累计 |
| 日志 | 右上角「日志」，滚动保留最近 5 MB，可复制、可清空 |
| 引擎 | `ALH Frame Lite` 走 ffmpeg `minterpolate`；`ALH Frame Standard` 走自研块匹配内核；`ALH Frame Standard v2` 是同一内核的稠密光流档（见下节）|
| 去重复帧 | 仅 Pro 可用。相邻帧 luma 采样差 < 0.8 判为重复，直接复制当前帧，省掉一次运动估计 |

## 构建

不用 Gradle，链路是 `aapt2 → javac → d8 → zipalign → apksigner`：

```bash
./build.sh          # 产物：build/ALHFrame-1.1.apk
```

依赖都在 `tools/`：JDK 21、Android SDK build-tools 35.0.0、platforms/android-33。中间文件写 `build/.tmp`，脚本结束自动清掉，`build/` 里只留签名好的 APK。

两个坑记一下：

- `ROOT` 要用 `pwd -W` 取成 Windows 路径，否则 javac 把 `/d/deep/...` 当相对路径，报「找不到文件」。
- d8 必须用 build-tools 35.0.0。33.0.2 带的 R8 3.3.20 碰到含匿名内部类的 class 会抛 `NullPointerException: Cannot invoke "String.length()" because "<parameter1>" is null`。

## 工程结构

```
app/
  AndroidManifest.xml
  src/com/alh/frame/
    MainActivity.java             程序化 UI、选片、进度、日志入口
    Engine.java                   探测 / 切段 / 并行 / 流式并合 / 回封 / 落盘
    Ff.java                       assets 里的 ffmpeg 落地到 files/bin 并加执行位
    Logs.java                     滚动日志（5 MB 上限，只留尾 3 MB）
    Bar.java                      自绘进度条
    Sheet.java                    底部面板（错误详情 / 日志 / 条款）
    Terms.java                    免责声明文本与同意状态
    Pro.java                      自研内核落地到 files/bin/alhift
  res/                            自适应图标与颜色
  assets/ff/ffmpeg                39 MB 静态 ffmpeg（GPLv3，见 docs/THIRD-PARTY.md）
  assets/alh/alhift-*             自研内核二进制（arm64-v8a / x86_64）
native/alhift.cpp                 自研内核源码，可独立编译使用（见 native/README.md）
docs/算法说明.md                  管线、参数、耗时拆解
docs/THIRD-PARTY.md               第三方组件与许可
tools/                            图标、界面预览的生成脚本
build/ALHFrame-1.1.apk
```

## 安装

```bash
adb install -r build/ALHFrame-1.1.apk
```

签名证书 SHA-256 是 `0c38ea…`，与任何旧版本都不同，装之前先卸载旧包，否则报 `INSTALL_FAILED_UPDATE_INCOMPATIBLE`。

## 临时文件与磁盘

中间产物只写在应用私有缓存，不碰相册以外的公共目录。

| 文件 | 位置 | 大小 | 何时删 |
| --- | --- | --- | --- |
| 源文件副本 | `cache/in/` | 等于源文件 | 换片时只保留一份；处理结束、下次启动清掉 |
| 分段 `c000.ts…` | `cache/alhframe/` | 每段 ≈ 成品 / 段数 | 并进累加文件的那一刻 |
| 累加流 `acc.ts` | `cache/alhframe/` | ≈ 成品 | 封装落盘后整目录删 |
| 成品 | `cache/alhframe/` → 相册 | ≈ 成品 | 拷进相册后删缓存副本 |

峰值是「累加流 + 成品」两份，再加一份源副本。100 分钟的 2K 片子约 26 GB，比攒齐全片再拼仍然省十几 GB。

开工前会按 `成品 × 2 + 源大小 + 96 MB` 查缓存分区，不够就直说「存储空间不足」，给出预计值和可用值，不会跑到一半把盘塞满。日志里留一行 `disk` 记着源大小、峰值估算和可用空间。

## 图标

`tools/make_icon.py` 出全套：`#0B0F14` 底加纯白像素字 AF。字形是手写位图，A 5×7 格、F 4×7 格，字距 1 格，笔画只占 1 格。所有密度都按整数倍格子硬画，不缩放，边缘永远是直角。

| 密度 | 传统图标 | 格子 | 字形宽 | 自适应前景 | 格子 | 字形宽 |
| --- | --- | --- | --- | --- | --- | --- |
| mdpi | 48 px | 2 | 20 px | 108 px | 5 | 50 px |
| hdpi | 72 px | 3 | 30 px | 162 px | 7 | 70 px |
| xhdpi | 96 px | 4 | 40 px | 216 px | 9 | 90 px |
| xxhdpi | 144 px | 6 | 60 px | 324 px | 14 | 140 px |
| xxxhdpi | 192 px | 8 | 80 px | 432 px | 18 | 180 px |

居中按墨迹重心算，不按外框。AF 不对称：A 下部宽，F 右侧只有顶横一道墨，按外框对中整组字会偏左上。脚本数所有实心像素求重心（4.40, 3.10 格，外框中心在 5.00, 3.50 格），压到画布正中，位移取整到像素，所以像素还是对齐的，只是整体往右下挪了 0.6 / 0.4 格。

```bash
python tools/make_icon.py            # 重出全套图标
python tools/make_ui_mock.py         # 界面版式预览（需要 Pillow）
```

## 已知限制

- 处理期间挂前台服务，切后台不会轻易被回收；强行清掉应用仍会中断。
- 源文件会复制一份到私有缓存，大文件多占一份空间。
- VFR 源按平均帧率处理，时长有极小偏差。
- 4× 之后输出帧率部分机型相册不认，比如 30fps × 4 = 120fps，播放器会按 60 播。
- 手写光流替代 `minterpolate` 试过一版，720p 每插值帧 83 ms，SSIM 0.9735，打不过 mci 的 0.9883，暂时搁置。
- 720p 以上 Pro 的锐度还不如 Lite（差约 3 dB），高分辨率建议用 Lite。
- x86_64 模拟器上引擎跑不起来：APK 里只有一个 arm64 的 ffmpeg。要在模拟器里验证，得自己换一份同版本的 x86_64 二进制。

## 许可与第三方

Java 代码 Apache-2.0，见 [LICENSE](LICENSE)。内置的 ffmpeg 是 GPLv3（`--enable-gpl --enable-version3` 加 libx264/libx265），以独立进程方式调用，源码可直接下载：[ffmpeg-9.0.2-source.tar.xz](https://github.com/AlLHHH/ALH-Frame/releases/download/v1.0.0/ffmpeg-9.0.2-source.tar.xz)（12 MB，与官方 tarball 字节一致）。重建步骤、完整 configure 行、各外部库许可证见 [docs/BUILD-SOURCE.md](docs/BUILD-SOURCE.md) 和 [docs/THIRD-PARTY.md](docs/THIRD-PARTY.md)。

## 关键词

补帧 · 插值 · 光流 · 运动补偿 · 视频补帧 · 帧率提升 · 高帧率 · 60fps · 120fps · 安卓 · 手机 · 本地处理 · 离线 · 无需联网 · 视频处理 · ffmpeg · minterpolate · frame interpolation · optical flow · motion compensation · android · offline · h264

GitHub 的 Topics 只接受小写字母、数字和连字符，中文放不进去，所以上面的中文词放在这里给搜索用，Topics 里放的是对应的英文词。