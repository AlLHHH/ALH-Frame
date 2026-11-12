# ALH Frame

本机视频补帧工具。选一个视频，按 2× / 3× / 4× 生成高帧率版本，全程离线，不联网、不上传。

- 包名 `com.alh.frame`，版本 1.0，minSdk 24，targetSdk 28
- 单 Activity、无第三方依赖，界面用代码构建，不依赖任何 UI 框架
- 引擎是一个自带 ffmpeg 9.0.2 的进程池：分段并行补帧 → 无损拼接 → 回封原音轨

## 功能

| 项 | 说明 |
| --- | --- |
| 输入 | MP4 / MOV / MKV 等 ffmpeg 能解的容器，视频轨任意方向（按 rotation 交换宽高） |
| 倍率 | 2× / 3× / 4×，输出帧率 = 源帧率 × 倍率 |
| 插值 | `minterpolate` 运动补偿（`mi_mode=mci`、`mc_mode=aobmc`、`me_mode=bilat`、`me=epzs`），带场景切换检测 |
| 音轨 | 优先 `-c:a copy`；失败重试 AAC 160k；再失败输出无音轨，绝不截断画面 |
| 输出 | `Movies/ALH Frame/ALHFrame_<时间戳>_<倍率>x.mp4`（H.264 High、CRF 19、yuv420p） |
| 日志 | 右上角「日志」，滚动保留最近 5 MB，可复制、可清空 |

## 时长与速度为什么是准的

补帧最容易出的错是「时长变短、画面变快」，根子是切段。这里的做法：

1. **按时间轴切段，不按帧数切**。第 i 段取 `[dur*i/N, dur*(i+1)/N)` 秒，该段的输出帧数由起止时间戳反算：`round(s1*outFps) - round(s0*outFps)`。
2. **不丢预热帧**。`trim=end_frame=<该段目标帧数>` 只截尾，`setpts=PTS-STARTPTS` 重置时间基；每段多取 `2/srcFps` 秒，保证 `tpad` 的克隆帧都在 trim 之外。
3. **源时长按样本数算**，不用容器元数据。`fps = (样本数-1) / 时间跨度`，`dur = 跨度 + 1/fps`；样本数超过 30 万或扫描超过 1.2 s 则整体回退容器元数据，避免半截统计得出更错的数。
4. 拼接用 `-f concat -c copy`，最后 `countFrames` 再核一遍实际帧数，写进完成提示。

端到端验证（10 s / 300 帧源，6 段 2×）：输出 600 帧、10.000 s、60/1 fps、音轨 10.000 s；逐帧条码校验显示第 2k 个输出帧正好等于源第 k 帧，零漂移。

## 构建

不需要 Gradle，链路是 `aapt2 → javac → d8 → zipalign → apksigner`：

```bash
./build.sh          # 产物：build/ALHFrame-1.0.apk
```

依赖都放在 `tools/` 下：JDK 21、Android SDK build-tools 35.0.0、platforms/android-33。构建中间文件写在 `build/.tmp`，结束后自动清掉，`build/` 里只留签名好的 APK。

两点约定：

- `ROOT` 必须解析成 Windows 路径（脚本里用 `pwd -W`），否则 javac 会把 `/d/deep/...` 当成相对路径。
- d8 用 build-tools **35.0.0**。33.0.2 的 R8 3.3.20 碰到含匿名内部类的 class 会抛 `NullPointerException: Cannot invoke "String.length()" because "<parameter1>" is null`。

## 工程结构

```
app/
  AndroidManifest.xml
  alhframe.jks                    签名（storepass/keypass: alh123，alias: alhframe）
  src/com/alh/frame/
    MainActivity.java             程序化 UI、选片、进度、日志入口
    Engine.java                   探测 / 切段 / 并行 / 拼接 / 回封 / 落盘
    Ff.java                       assets 里的 ffmpeg 落地到 files/bin 并加执行位
    Logs.java                     滚动日志（5 MB 上限，一次只留尾 3 MB）
    Bar.java                      自绘进度条
    Sheet.java                    底部面板（错误详情 / 日志）
  res/                            自适应图标与颜色
  assets/ff/ffmpeg                39 MB 静态 ffmpeg
docs/算法说明.md                  管线、参数、耗时拆解
tools/                            图标与预览图的生成脚本
build/ALHFrame-1.0.apk
```

## 安装

```bash
adb install -r build/ALHFrame-1.0.apk
```

签名证书 SHA-256 为 `0c38ea…`，与本机此前装过的任何版本都不同，装之前先卸载旧包，否则 `INSTALL_FAILED_UPDATE_INCOMPATIBLE`。

## 资源与图标

`tools/make_icon.py` 生成图标：`#0B0F14` 底 + 纯白像素字 AF。

字形是手写位图：A 5×7 格、F 4×7 格，字距 1 格，**笔画只占 1 格**——这是像素字最小的骨架，字腔留得开，缩小后不会糊成一块。所有密度都按整数倍格子硬绘制，不缩放，边缘永远是直角。

| 密度 | 传统图标 | 格子 | 字形宽 | 自适应前景 | 格子 | 字形宽 |
| --- | --- | --- | --- | --- | --- | --- |
| mdpi | 48 px | 2 | 20 px | 108 px | 5 | 50 px |
| hdpi | 72 px | 3 | 30 px | 162 px | 7 | 70 px |
| xhdpi | 96 px | 4 | 40 px | 216 px | 9 | 90 px |
| xxhdpi | 144 px | 6 | 60 px | 324 px | 14 | 140 px |
| xxxhdpi | 192 px | 8 | 80 px | 432 px | 18 | 180 px |

前景不透明底交给 `ic_launcher_bg`，字形宽度约为画布的 42%，四周留白 29%，完整落在自适应图标 66dp 的安全圆内；`ic_launcher_round` 再叠一层圆遮罩。

**居中按墨迹重心算，不按外框**。AF 是不对称的：A 下部宽、F 右侧只有顶横一道墨，外框对中时整组字会看着偏左上。脚本会数所有实心像素算重心（4.40, 3.10 格，外框中心是 5.00, 3.50 格），把它压到画布正中，位移取整到像素——所以像素仍然对齐，只是整体往右下挪了 0.6 / 0.4 格。

## 临时文件与磁盘占用

处理时只在应用私有缓存里放中间产物，**不写相册以外的任何公共目录**：

| 文件 | 位置 | 大小 | 何时删除 |
| --- | --- | --- | --- |
| 源文件副本 | `cache/in/` | 等于源文件 | 换片时（只留一份）、处理结束、下次启动 |
| 分段 `c000.ts…` | `cache/alhframe/` | 每段 ≈ 成品 / 段数 | 并进累加文件的瞬间就删 |
| 累加流 `acc.ts` | `cache/alhframe/` | ≈ 成品 | 封装完成、落盘后整目录删 |
| 成品 | `cache/alhframe/` → 相册 | ≈ 成品 | 拷进相册后删缓存副本 |

段数跟并发解耦（每段约 30 秒），段一编完就按序号追加进 `acc.ts` 并立刻删掉自己。所以磁盘上同时存在的只有「已并合的部分 + 正在跑的几段」，峰值 ≈ **成品 × 1.1 + 源副本**，不再是「整片 × 2」。100 分钟 2K 片子因此从 38 GB 峰值降到约 22 GB。

开工前按 `成品估算 + 成品/4 + 源大小 + 96 MB` 检查缓存分区，不够就直接报「存储空间不足」并给出预计值与可用值，不会跑到一半把盘塞满。日志里留一行 `disk` 记录源大小、峰值估算和可用空间。

```bash
python tools/make_icon.py            # 重新出全套图标
python tools/make_ui_mock.py         # 界面版式预览（可选，需要 Pillow）
```
## 兼容性与容错

- 宽高不是 16 的整数倍时（如 618×840），minterpolate 的块匹配会在右/下边缘越界，部分机型直接 SIGSEGV（退出码 139）。现在会在滤波链前面 `pad` 到 16 的倍数、算完 `crop` 回原尺寸。
- 单段失败会自动降级重跑：第 1 次原参数 → 第 2 次 `-threads 1` → 第 3 次 `-threads 1 -x264-params asm=c -cpuflags 0`（纯 C，慢但能出片）。
- 并发按可用内存 35% 折算，单路预算不低于 384 MB，避免多进程叠加被系统杀掉。
- 条款入口在页面最底部，随内容滚动，不常驻。
