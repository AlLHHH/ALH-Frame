# 第三方组件与许可证

本仓库的 Java 代码以 [Apache-2.0](../LICENSE) 发布。

**但发布的 APK 里打包了一个 FFmpeg 命令行程序**（`app/assets/ff/ffmpeg`，约 39 MB），它不属于本仓库的 Apache-2.0 范围，而是 **GPLv3**。分发 APK = 分发这个二进制，因此必须随附它对应的源码获取方式与许可证声明。下面的内容就是为此准备的。

---

## 1. FFmpeg（内含在 APK 中）

| 项 | 值 |
| --- | --- |
| 组件 | FFmpeg（`ffmpeg` 可执行文件，arm64-v8a，静态链接 libx264 / libx265 / libdav1d / libvpx / libmp3lame / libopus / libvorbis / libass / freetype / fribidi / harfbuzz / zimg 等） |
| 版本 | 9.0.2（运行时自报 `ffmpeg version 9.0.2`） |
| 许可证 | **GNU GPL v3**（构建时启用了 `--enable-gpl --enable-version3`，并链接了 GPL 的 libx264 / libx265，因此整体为 GPLv3，而非 LGPL） |
| 许可条文本 | <https://www.gnu.org/licenses/gpl-3.0.txt> |
| 上游源码 | <https://ffmpeg.org/download.html>（对应 9.0 分支：<https://git.ffmpeg.org/ffmpeg.git>） |
| 本版本源码包（可直接下载） | [ffmpeg-9.0.2-source.tar.xz](https://github.com/AlLHHH/ALH-Frame/releases/download/v1.0.0/ffmpeg-9.0.2-source.tar.xz) · 12,040,788 B · sha256 `8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e`（与 ffmpeg.org 官方 tarball 字节一致） |
| 本二进制的构建配置 | 见下 |

本二进制内置的 `configuration` 行（由 `ffmpeg -version` 输出，是还原构建的唯一依据）：

```
--enable-gpl --enable-version3 --enable-pic --disable-stripping
--enable-libdavs2 --enable-libxavs2 --enable-libuavs3d --enable-libuavs3e
--enable-libx264 --enable-libx265 --enable-libdav1d --enable-libvpx
--enable-libmp3lame --enable-libvorbis --enable-libopus --enable-libass
--enable-libfreetype --enable-libfribidi --enable-libharfbuzz --enable-libzimg
```

### 合规状态与义务

1. **源码提供**：FFmpeg 本体的 9.0.2 源码包已作为发布资产上传，链接见上表，重建步骤见 [BUILD-SOURCE.md](BUILD-SOURCE.md)。外部库按各自上游仓库自取。分发者需按 GPLv3 第 6 条向收到 APK 的人提供「对应源码」。可选做法：
   - 在发布页附上 FFmpeg 官方源码地址 + 上面的 configure 行（若二进制与官方源码一致、未做修改，这是最常见也最省事的做法）；
   - 或提供一份包含 FFmpeg 源码 + 各外部库源码的压缩包/网盘链接；
   - 或提供书面承诺（written offer），有效期三年。
2. **许可证文本**：随 APK 的关于页/条款页已声明组件名与许可证，并指向本文件。
3. **修改声明**：若你自行改过 FFmpeg 源码，必须在同一条款下公开修改后的源码。
4. **调用方式**：本应用通过 `ProcessBuilder` 以**独立进程**调用 `ffmpeg`，不链接其库、不共享地址空间，属于聚合（aggregate）而非衍生作品；因此应用自身的 Java 代码可以继续用 Apache-2.0（这也是本仓库的选择）。若你改成 JNI/静态链接，则整个应用都需要按 GPL 发布。
5. **不得附加限制**：GPLv3 禁止在分发时附加额外限制（例如禁止再分发）。应用商店的 DRM/使用限制条款与此存在张力，自建分发（GitHub Release、官网 APK）没有这个问题。

## 2. FFmpeg 链接的外部库

| 库 | 许可证 | 上游 |
| --- | --- | --- |
| libx264 | GPL v2 or later | <https://www.videolan.org/developers/x264.html> |
| libx265 | GPL v2 | <https://www.videolan.org/developers/x265.html> |
| libdav1d | BSD-2-Clause | <https://code.videolan.org/videolan/dav1d> |
| libvpx | BSD-3-Clause | <https://chromium.googlesource.com/webm/libvpx> |
| libmp3lame | LGPL-2.0 | <https://lame.sourceforge.io/> |
| libopus | BSD-3-Clause | <https://opus-codec.org/> |
| libvorbis | BSD-3-Clause | <https://xiph.org/vorbis/> |
| libass | ISC | <https://github.com/libass/libass> |
| FreeType | FTL / GPLv2 双许可 | <https://freetype.org/> |
| FriBidi | LGPL-2.1 | <https://github.com/fribidi/fribidi> |
| HarfBuzz | MIT | <https://github.com/harfbuzz/harfbuzz> |
| zimg | WTFPL v2 | <https://github.com/sekrit-twc/zimg> |
| libdavs2 / libxavs2 / libuavs3d / libuavs3e | 各自上游许可（国产 AVS2/AVS3 编解码） | 见各上游仓库 |

上述库以静态方式链接进 `ffmpeg`，其源码同样属于「对应源码」范围；提供 FFmpeg 完整源码树时一并提供即可。

## 3. Android 平台

`android.jar`、AndroidX（本项目未使用）、Android 系统 API 由 Android SDK 提供，按其自身条款使用。本项目**不**包含任何 Google Play Services 或闭源 SDK。

## 4. 字体与图标

应用图标、界面全部由代码绘制，使用系统字体，不含第三方字体文件。FFmpeg 内部链接的 FreeType/HarfBuzz 只用于字幕渲染，本项目未使用字幕功能。

---

## 分发前检查清单

- [ ] Release 页写明：内置 FFmpeg 9.0.2，GPLv3，附 configure 行与源码地址
- [ ] 附上 `COPYING.GPLv3`（或链接）
- [ ] 若改动过 FFmpeg 或任一外部库，公开改动后的源码
- [ ] 不要在任何渠道声称「本应用完全 Apache-2.0」——APK 内含 GPLv3 二进制
- [ ] 若上架应用商店，确认商店条款不与 GPLv3 冲突；冲突时优先自建分发