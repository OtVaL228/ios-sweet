#!/bin/sh
# Build the ios-sweet userland helpers for the iOS 16 root (arm64, A11: no
# arm64e), ad-hoc signed: the kernel makes signed binaries on the root
# volume platform binaries (patch 0035), so no other signature is needed.
#
#   userland/build.sh            -> userland/out/ (autorun, GPU tests, backboardd wrapper)
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
build sweet-bbd bbd/sweet-bbd.c
build libsweet-bbd.dylib bbd/bbd-insert.c -dynamiclib -install_name /usr/lib/libsweet-bbd.dylib -framework IOSurface -Wl,-not_for_dyld_shared_cache \
	-framework IOKit -framework CoreFoundation
build libsweet-insert-test.dylib dyldtest/insert-test.c -dynamiclib -install_name /private/var/tmp/libsweet-insert-test.dylib
# G3: needs userland/mesa/mk-mesa.sh first (libOSMesa.8.dylib + headers)
MESA=mesa/build
if [ -f $MESA/out/libOSMesa.8.dylib ]; then
	# libOSMesa is @rpath/...: next to the test, or in /usr/lib for libsweet-gl
	build osmesa-triangle gputest/osmesa-triangle.c -I$MESA/mesa-25.0.7/include $MESA/out/libOSMesa.8.dylib \
		-Wl,-rpath,@executable_path
	cp $MESA/out/libOSMesa.8.dylib out/
	# G4: EAGLContext on Mesa for backboardd (eagl.c: HackiOS via topaz, MIT)
	python3 gpu/mkeagl.py gpu/eagl_slots.txt $MESA/out/gl.xml $MESA/out/exported-gl.txt gpu/eagl.c > out/eagl_gen.h
	build libsweet-gl.dylib gpu/eagl.c gpu/gl_osmesa.c -dynamiclib -install_name /usr/lib/libsweet-gl.dylib \
		-Iout -Igpu -I$MESA/mesa-25.0.7/include $MESA/out/libOSMesa.8.dylib -Wl,-rpath,/usr/lib \
		-Wl,-not_for_dyld_shared_cache -Wno-unused-but-set-variable \
		-framework OpenGLES -framework IOKit -framework IOSurface -framework CoreFoundation -lobjc
else
	echo "skipped osmesa-triangle: run userland/mesa/mk-mesa.sh"
fi
