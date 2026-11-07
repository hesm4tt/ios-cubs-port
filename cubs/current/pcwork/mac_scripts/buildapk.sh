#!/bin/bash
# build com.cubs.cam: javac -> d8 -> aapt2 link -> add classes.dex -> jarsigner (v1)
set -euo pipefail
export JAVA_HOME=/opt/homebrew/opt/openjdk@21; export PATH=$JAVA_HOME/bin:$PATH
cd ~/apkbuild && rm -rf cls out.apk app.apk classes.dex && mkdir cls
javac --release 8 -cp android-all.jar -d cls CamActivity.java CamService.java SndService.java WifiStatusService.java
java -cp r8.jar com.android.tools.r8.D8 --release --min-api 26 --lib android-all.jar --output . $(find cls -name '*.class')
./aapt2 link -o out.apk -I framework-res.apk --manifest AndroidManifest.xml --min-sdk-version 26 --target-sdk-version 29
cp out.apk app.apk && zip -q -j app.apk classes.dex
[ -f debug.keystore ] || keytool -genkeypair -keystore debug.keystore -storepass android -keypass android -alias k -keyalg RSA -keysize 2048 -validity 9000 -dname "CN=cubs" >/dev/null 2>&1
jarsigner -keystore debug.keystore -storepass android -keypass android -sigalg SHA256withRSA -digestalg SHA-256 app.apk k >/dev/null
ls -la app.apk | cut -c20-70
