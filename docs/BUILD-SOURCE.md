# 从源码重建内置 ffmpeg

APK 里的 `app/assets/ff/ffmpeg` 是 arm64-v8a 静态二进制，由 FFmpeg 源码编译。这个目录说明怎么把它做出来，以及源码从哪里拿。

## 源码包

| 内容 | 链接 | 大小 | sha256 |
| --- | --- | --- | --- |
| FFmpeg 9.0.2 源码 | [ffmpeg-9.0.2-source.tar.xz](https://github.com/AlLHHH/ALH-Frame/releases/download/v1.0.0/ffmpeg-9.0.2-source.tar.xz) | 12,040,788 B | `8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e` |
| 官方同版本 | <https://ffmpeg.org/releases/ffmpeg-9.0.2.tar.xz> | 同上 | 同上 |

发布页上的源码包与官方 tarball 字节一致，只是放在 GitHub 上多一条可达路径。构建时未修改 FFmpeg 源码。

## 外部库

二进制静态链接了下面这些库。FFmpeg 的 configure 只记录开关，不记录版本号，所以版本以你在构建时可获取的上游 tag 为准；这里给的是仓库地址。

| 库 | 许可证 | 上游 | 说明 |
| --- | --- | --- | --- |
| libx264 | GPL-2.0-or-later | <https://code.videolan.org/videolan/x264> | 补帧后的编码器，`-c:v libx264` 走它 |
| libx265 | GPL-2.0 | <https://bitbucket.org/multicoreware/x265_git> | 二进制里有，应用未使用 |
| libdav1d | BSD-2-Clause | <https://code.videolan.org/videolan/dav1d> | AV1 解码 |
| libvpx | BSD-3-Clause | <https://chromium.googlesource.com/webm/libvpx> | VP8/VP9 |
| libmp3lame | LGPL-2.0 | <https://lame.sourceforge.io/> | MP3 |
| libopus | BSD-3-Clause | <https://github.com/xiph/opus> | Opus |
| libvorbis | BSD-3-Clause | <https://github.com/xiph/vorbis> | Vorbis |
| libass | ISC | <https://github.com/libass/libass> | 字幕 |
| FreeType | FTL / GPL-2.0 | <https://gitlab.freedesktop.org/freetype/freetype> | 字幕字形 |
| FriBidi | LGPL-2.1 | <https://github.com/fribidi/fribidi> | 双向文字 |
| HarfBuzz | MIT | <https://github.com/harfbuzz/harfbuzz> | 字形整形 |
| zimg | WTFPL-2 | <https://github.com/sekrit-twc/zimg> | 色彩空间转换 |
| libdavs2 / libxavs2 / libuavs3d / libuavs3e | 各自上游 | 各上游仓库 | AVS2/AVS3 |

## 重建步骤

以下命令在 Linux x86_64 上交叉编译到 Android arm64。NDK 用 r26 及以上，API level 24 起。

```bash
# 1. 解包
tar xf ffmpeg-9.0.2.tar.xz && cd ffmpeg-9.0.2

# 2. 先用 NDK 把外部库逐个编成 arm64 静态库（prefix 统一放到 $PREFIX）
#    x264 示例：
git clone https://code.videolan.org/videolan/x264 && cd x264
./configure --host=aarch64-linux-android --cross-prefix=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm- \
  --enable-static --enable-pic --disable-cli --prefix=$PREFIX
make -j$(nproc) && make install && cd ..

# 3. 编 ffmpeg：下面是发布二进制里记录的完整开关串（ffmpeg -version 的 configuration 行）
#    交叉编译相关的参数按你的 NDK 路径补，不影响功能开关
./configure   --enable-gpl --enable-version3 --enable-pic --disable-stripping   --enable-libdavs2 --enable-libxavs2 --enable-libuavs3d --enable-libuavs3e   --enable-libx264 --enable-libx265 --enable-libdav1d --enable-libvpx   --enable-libmp3lame --enable-libvorbis --enable-libopus --enable-libass   --enable-libfreetype --enable-libfribidi --enable-libharfbuzz --enable-libzimg   --target-os=android --arch=aarch64 --cpu=armv8-a   --enable-cross-compile --enable-static --disable-shared --disable-doc   --cross-prefix=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-   --cc=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android24-clang   --cxx=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android24-clang++   --pkg-config-flags=--static --extra-cflags="-I$PREFIX/include -O3"   --extra-ldflags="-L$PREFIX/lib -static" --prefix=$PREFIX/ffmpeg

# 这串开关就是发布二进制自报的那一份，逐字对应：
#   --enable-gpl --enable-version3 --enable-pic --disable-stripping
#   --enable-libdavs2 --enable-libxavs2 --enable-libuavs3d --enable-libuavs3e
#   --enable-libx264 --enable-libx265 --enable-libdav1d --enable-libvpx
#   --enable-libmp3lame --enable-libvorbis --enable-libopus --enable-libass
#   --enable-libfreetype --enable-libfribidi --enable-libharfbuzz --enable-libzimg

make -j$(nproc) && make install
```

构建主机上如果没有 `libdavs2`/`libxavs2`/`libuavs3d`/`libuavs3e`（AVS 系列比较冷门），把对应的 `--enable-*` 去掉也能编出来，只是少了 AVS2/AVS3 解码；本应用用不到这两族编码。

编完的 `$PREFIX/ffmpeg/bin/ffmpeg` 就是 `app/assets/ff/ffmpeg` 的来源。放进 assets 后不用改权限，应用首次启动会把 `Ff.VER` 里那一串版本标识写进 `files/bin/.ver`，版本变了就重新落地并加执行位。

FFmpeg 的 `--enable-version3` 决定了整个二进制按 GPLv3 分发，这一点见 [THIRD-PARTY.md](THIRD-PARTY.md)。

## 拿不到源码怎么办

发布页的源码包已经覆盖 FFmpeg 本体。外部库按上表的仓库地址自取。如果你需要本构建实际用到的那份完整源码树（含各库的确切 commit），在 issue 里说一声，作者会打包提供，这条承诺自发布日起三年有效，符合 GPLv3 第 6 条 b 项。