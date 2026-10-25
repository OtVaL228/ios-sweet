#!/bin/sh
# Build the ios-sweet userland helpers for the iOS 16 root (arm64, A11: no
# arm64e), ad-hoc signed: the kernel makes signed binaries on the root
# volume platform binaries (patch 0035), so no other signature is needed.
#
#   userland/build.sh            -> userland/out/{sweet-autorun,redwood-gpu-test,osmesa-triangle}
set -e
cd "$(dirname "$0")"
SDK=$(xcrun --sdk iphoneos --show-sdk-path)
CC="xcrun --sdk iphoneos clang -arch arm64 -mios-version-min=16.0 -isysroot $SDK -O2 -Wall -Wextra"
mkdir -p out
build() { # OUT SRC... [-- extra flags]
	out=$1; shift
	$CC -o "out/$out" "$@"
	codesign -f -s - "out/$out"
	echo "out/$out"
}
build sweet-autorun autorun/sweet-autorun.c
build redwood-gpu-test gputest/redwood-gpu-test.c -framework IOKit -framework CoreFoundation
# G3: needs userland/mesa/mk-mesa.sh first (libOSMesa.8.dylib + headers)
MESA=mesa/build
if [ -f $MESA/out/libOSMesa.8.dylib ]; then
	build osmesa-triangle gputest/osmesa-triangle.c -I$MESA/mesa-25.0.7/include $MESA/out/libOSMesa.8.dylib
	cp $MESA/out/libOSMesa.8.dylib out/
else
	echo "skipped osmesa-triangle: run userland/mesa/mk-mesa.sh"
fi
