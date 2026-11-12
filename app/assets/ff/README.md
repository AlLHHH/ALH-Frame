# ffmpeg

构建与运行时都需要一个静态编译的 ffmpeg 可执行文件，放在本目录，命名为 `ffmpeg`。

要求：

- 静态链接（Android 上无 libc 动态库可用），arm64-v8a 与 armeabi-v7a 通用单文件
- 含 `minterpolate` 滤镜（构建时需 `--enable-gpl --enable-avfilter`，非 free 组件）
- 建议一并保留 `-version` 输出，应用启动时会自检

体积约 39 MB，所以不入库。发布页的 APK 已经内置，从源码构建才需要自己准备。
