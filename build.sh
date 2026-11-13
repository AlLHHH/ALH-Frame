#!/usr/bin/env bash
# ALH Frame build (no Gradle): aapt2 -> javac -> d8 -> zipalign -> apksigner
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
KS="$APP/alhframe.jks"
KS_PASS="alh123"
KS_ALIAS="alhframe"
OUT="$BUILD/ALHFrame-1.1.apk"

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
  --version-code 2 \
  --version-name 1.1 \
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
python -c "
import sys, zipfile
apk, dex = sys.argv[1], sys.argv[2]
z = zipfile.ZipFile(apk, 'a', zipfile.ZIP_DEFLATED)
z.write(dex, 'classes.dex')
z.close()
print('classes.dex injected')
" "$WORK/unsigned.apk" "$WORK/dex/classes.dex"
"$BT/zipalign" -f -p 4 "$WORK/unsigned.apk" "$WORK/aligned.apk"

echo "[6/6] sign"
if [ ! -f "$KS" ]; then
  "$JAVA/keytool" -genkeypair -keystore "$KS" -alias "$KS_ALIAS" \
    -keyalg RSA -keysize 2048 -validity 10000 \
    -storepass "$KS_PASS" -keypass "$KS_PASS" \
    -dname "CN=ALHFrame, O=ALH, C=CN" >/dev/null 2>&1
  echo "  generated $KS"
fi
"$BT/apksigner.bat" sign --ks "$KS" --ks-key-alias "$KS_ALIAS" \
  --ks-pass "pass:$KS_PASS" --key-pass "pass:$KS_PASS" \
  --v1-signing-enabled true --v2-signing-enabled true \
  --out "$OUT" "$WORK/aligned.apk"
"$BT/apksigner.bat" verify --print-certs "$OUT" | head -5

rm -rf "$WORK" "$OUT.idsig"
ls -l "$OUT"
echo "OK -> $OUT"