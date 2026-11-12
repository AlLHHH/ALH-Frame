# 构建与运行

## 目录

```
app/
  AndroidManifest.xml        清单（minSdk 24 / targetSdk 28 / versionCode 1 / versionName 1.0）
  src/com/alh/frame/         Java 源码，无第三方依赖
    MainActivity.java        界面、选片、进度、条款
    Engine.java              分段并行补帧引擎
    Ff.java                  内置 ffmpeg 落地与版本管理
    Bar.java                 自绘进度条
    Sheet.java               自绘对话框（错误详情 / 运行日志 / 免责声明）
    Logs.java                滚动日志（上限 5 MB）
    Terms.java               免责声明文本与同意状态
  res/                       图标与配色
  assets/ff/ffmpeg           静态编译 ffmpeg（不入库，见 app/assets/ff/README.md）
docs/算法说明.md              管线的每一步、参数依据与实测数据
```

## 准备依赖

需要三样东西，都不入库：

1. **Android SDK**：Build-Tools 35.0.0 与 Platform android-33，放到 `tools/sdkroot/`。
2. **JDK 21**：放到 `tools/jdk-21.0.13+9/`。
3. **ffmpeg**：静态编译版，含 `minterpolate` 滤镜，放到 `app/assets/ff/ffmpeg`。

## 构建

```bash
bash build.sh
```

产物在 `build/ALHFrame-1.0.apk`；没有提供 keystore 时输出 `ALHFrame-1.0-unsigned.apk`。

要签名的：

```bash
export ALH_KS=/path/to/release.jks
export ALH_KS_PASS=********
export ALH_KS_ALIAS=alhframe
bash build.sh
```

## 安装

```bash
adb install -r build/ALHFrame-1.0.apk
```

换签名包的必须先卸载旧版本，否则系统会拒绝覆盖安装。

## 已知限制

- 处理期间不做前台服务，长时间任务在后台可能被系统回收。
- 源文件会整份复制到应用私有缓存，大文件等于多占一份磁盘。
- VFR 源按平均帧率处理，时长有极小偏差。
- 4× 之后的输出帧率部分机型相册/播放器不认（例如 30fps 源 4× = 120fps）。