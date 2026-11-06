#!/bin/sh
# Build vktest and v2test (Turnip smoke tests) for the phone into userland/turnip/out, plus a signed copy of Turnip.
#   userland/turnip/build.sh        (Turnip itself first: tools/build-turnip-ios.sh). From ios-flame phone/turnip (MIT).
# On the phone: vktest/v2test next to libvulkan_freedreno.dylib, or /var/local/lib/libvulkan_freedreno.dylib.
set -e
cd "$(dirname "$0")"
ROOT=$(cd ../.. && pwd)
W=${METAL_WORK:-$(dirname "$ROOT")/metal-work}
SDK=$(xcrun --sdk iphoneos --show-sdk-path)
CC="xcrun --sdk iphoneos clang -arch arm64 -mios-version-min=16.0 -isysroot $SDK"
mkdir -p out
: > out/shaders.h
for s in fill.comp:fill_comp tri.vert:tri_vert tri.frag:tri_frag; do
	glslangValidator -V --target-env vulkan1.2 --vn "${s#*:}" -o out/"${s#*:}".h shaders/"${s%%:*}" >/dev/null
	cat out/"${s#*:}".h >> out/shaders.h
done
$CC -O2 -Wall -I out -I "$W/turnip/mesa/include" -o out/vktest vktest.c
codesign -f -s - out/vktest 2>/dev/null
$CC -x c++ -std=c++17 -O2 -Wall -I . -framework IOKit -framework CoreFoundation -o out/v2test v2test.cpp -lc++
codesign -f -s - out/v2test 2>/dev/null
cp "$W/turnip/build-ios/src/freedreno/vulkan/libvulkan_freedreno.dylib" out/
codesign -f -s - out/libvulkan_freedreno.dylib 2>/dev/null
ls -la out/vktest out/v2test out/libvulkan_freedreno.dylib
