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
build libsweet-insert-test.dylib dyldtest/insert-test.c -dynamiclib -install_name /private/var/tmp/libsweet-insert-test.dylib
# G3: needs userland/mesa/mk-mesa.sh first (libOSMesa.8.dylib + headers)
MESA=mesa/build
if [ -f $MESA/out/libOSMesa.8.dylib ]; then
	build osmesa-triangle gputest/osmesa-triangle.c -I$MESA/mesa-25.0.7/include $MESA/out/libOSMesa.8.dylib
	cp $MESA/out/libOSMesa.8.dylib out/
	# the same program loading Mesa through @executable_path (needs xnu 0121)
	cp out/osmesa-triangle out/osmesa-triangle-at
	install_name_tool -change /private/var/tmp/libOSMesa.8.dylib @executable_path/libOSMesa.8.dylib \
	    out/osmesa-triangle-at 2>/dev/null
	codesign -f -s - out/osmesa-triangle-at
	echo out/osmesa-triangle-at
else
	echo "skipped osmesa-triangle: run userland/mesa/mk-mesa.sh"
fi
