#!/usr/bin/env bash
# ALH Frame 构建脚本（不依赖 Gradle）：aapt2 -> javac -> d8 -> zipalign -> apksigner
#
# 依赖（自行准备，不入库）：
#   tools/sdkroot/build-tools/35.0.0     Android SDK Build-Tools
#   tools/sdkroot/platforms/android-33    Android SDK Platform
#   tools/jdk-21.0.13+9                   JDK 21
#   app/assets/ff/ffmpeg                  静态编译的 ffmpeg 可执行文件（见 app/assets/ff/README.md）
#
# 签名：默认产出未签名包。要签名的，导出下面三个环境变量后重跑：
#   ALH_KS       keystore 路径
#   ALH_KS_PASS  keystore 口令
#   ALH_KS_ALIAS key 别名
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd -W)"
APP="$ROOT/app"
BUILD="$ROOT/build"
WORK="$BUILD/.tmp"
TOOLS="$ROOT/tools"
SDK="$TOOLS/sdkroot"
BT="$SDK/build-tools/35.0.0"
PLATFORM="$SDK/platforms/android-33/android.jar"
JAVA="$TOOLS/jdk-21.0.13+9/bin"
export JAVA_HOME="$(dirname "$JAVA")"
export PATH="$JAVA:$PATH"

VERSION_CODE=1
VERSION_NAME="1.0"
UNSIGNED="$BUILD/ALHFrame-unsigned.apk"
OUT="$BUILD/ALHFrame-$VERSION_NAME.apk"

rm -rf "$BUILD"
mkdir -p "$WORK/classes" "$WORK/dex" "$WORK/gen"

echo "[1/6] aapt2 compile"
"$BT/aapt2" compile --dir "$APP/res" -o "$WORK/res.zip"

echo "[2/6] aapt2 link"
"$BT/aapt2" link -o "$WORK/base.apk" \
  -I "$PLATFORM" \
  --manifest "$APP/AndroidManifest.xml" \
  -R "$WORK/res.zip" \
  -A "$APP/assets" \
  --java "$WORK/gen" \
  --min-sdk-version 24 \
  --target-sdk-version 28 \
  --version-code "$VERSION_CODE" \
  --version-name "$VERSION_NAME" \
  --auto-add-overlay

echo "[3/6] javac"
find "$APP/src" -name '*.java' > "$WORK/sources.txt"
"$JAVA/javac" -encoding UTF-8 -source 8 -target 8 -nowarn \
  -bootclasspath "$PLATFORM" \
  -d "$WORK/classes" @"$WORK/sources.txt"

echo "[4/6] d8"
find "$WORK/classes" -name '*.class' > "$WORK/classes.txt"
"$JAVA/java" -cp "$BT/lib/d8.jar" com.android.tools.r8.D8 \
  --release --min-api 24 --lib "$PLATFORM" \
  --output "$WORK/dex" @"$WORK/classes.txt"

echo "[5/6] package + zipalign"
cp "$WORK/base.apk" "$WORK/unsigned.apk"
python - "$WORK/unsigned.apk" "$WORK/dex/classes.dex" <<'PY'
import sys, zipfile
apk, dex = sys.argv[1], sys.argv[2]
z = zipfile.ZipFile(apk, 'a', zipfile.ZIP_DEFLATED)
z.write(dex, 'classes.dex')
z.close()
print('classes.dex injected')
PY
"$BT/zipalign" -p -f 4 "$WORK/unsigned.apk" "$UNSIGNED"

echo "[6/6] sign"
if [[ -n "${ALH_KS:-}" && -n "${ALH_KS_PASS:-}" && -n "${ALH_KS_ALIAS:-}" ]]; then
  "$BT/apksigner.bat" sign --ks "$ALH_KS" --ks-key-alias "$ALH_KS_ALIAS" \
    --ks-pass "pass:$ALH_KS_PASS" --key-pass "pass:$ALH_KS_PASS" \
    --out "$OUT" "$UNSIGNED"
  echo "OK -> $OUT"
else
  mv "$UNSIGNED" "$BUILD/ALHFrame-$VERSION_NAME-unsigned.apk"
  echo "OK -> $BUILD/ALHFrame-$VERSION_NAME-unsigned.apk（未签名，设置 ALH_KS / ALH_KS_PASS / ALH_KS_ALIAS 可签名）"
fi