#!/bin/sh
# Builds HakiosAudio.driver (userland/audio/README.md) into userland/out/HakiosAudio.driver.
set -e
cd "$(dirname "$0")/.."
SDK=$(xcrun --sdk iphoneos --show-sdk-path)
mkdir -p out/HakiosAudio.driver
xcrun --sdk iphoneos clang -arch arm64 -miphoneos-version-min=16.0 -isysroot "$SDK" \
	-O1 -Wall -bundle -framework CoreFoundation -o out/HakiosAudio.driver/HakiosAudio audio/audio_plugin.c
codesign -s - -f out/HakiosAudio.driver/HakiosAudio
cp audio/audio_plugin.plist out/HakiosAudio.driver/Info.plist
plutil -lint out/HakiosAudio.driver/Info.plist
echo out/HakiosAudio.driver
