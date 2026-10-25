#!/bin/bash
# Mesa 25.0.7 (gallium freedreno + softpipe, OSMesa) for the iOS 16 userland,
# cross-built on the Mac with Xcode's clang. Adapted from MakrSas/ios-topaz
# userland/mesa/mk-mesa-mac.sh (HackiOS / darwin-redwood, MIT; see README.md).
#
#   userland/mesa/mk-mesa.sh      -> userland/mesa/build/out/libOSMesa.8.dylib
#
# Needs python3 and Xcode; meson, ninja, mako, pyyaml and packaging go into a
# venv under build/. Uses /opt/homebrew/opt/bison when installed, else the
# system bison (Mesa keeps old-bison compat).
set -e
VERSION=25.0.7
SHA256=592272df3cf01e85e7db300c449df5061092574d099da275d19e97ef0510f8a6
ROOT=$(cd "$(dirname "$0")" && pwd)
B=$ROOT/build
SDK=$(xcrun --sdk iphoneos --show-sdk-path)
CC=$(xcrun --sdk iphoneos -f clang)
mkdir -p "$B"
cd "$B"
PATCHES=$(cat "$ROOT"/patches/*.patch | shasum -a 256 | cut -d' ' -f1)
if [ ! -d mesa-$VERSION ] || [ "$(cat mesa-$VERSION/.sweet-patches 2>/dev/null)" != "$PATCHES" ]; then
	[ -f mesa-$VERSION.tar.xz ] || curl -sL --fail -o mesa-$VERSION.tar.xz https://archive.mesa3d.org/mesa-$VERSION.tar.xz
	echo "$SHA256  mesa-$VERSION.tar.xz" | shasum -a 256 -c --quiet
	rm -rf mesa-$VERSION build-*
	tar xJf mesa-$VERSION.tar.xz
	for p in "$ROOT"/patches/*.patch; do patch -s --no-backup-if-mismatch -d mesa-$VERSION -p1 < "$p"; done
	echo "$PATCHES" > mesa-$VERSION/.sweet-patches
fi
rm -rf mesa-$VERSION/src/redwood-drm
cp -r "$ROOT/redwood-drm" mesa-$VERSION/src/redwood-drm
if [ ! -x venv/bin/meson ]; then
	python3 -m venv venv
	venv/bin/pip -q install meson ninja mako pyyaml packaging
fi
export PATH=$B/venv/bin:/opt/homebrew/opt/bison/bin:$PATH
cat > cross.ini <<INI
[binaries]
c = ['$CC', '-target', 'arm64-apple-ios16.0', '-isysroot', '$SDK']
cpp = ['$CC++', '-target', 'arm64-apple-ios16.0', '-isysroot', '$SDK', '-fuse-cxa-atexit']
objc = ['$CC', '-target', 'arm64-apple-ios16.0', '-isysroot', '$SDK']
ar = '$(xcrun -f ar)'
ranlib = '$(xcrun -f ranlib)'
strip = '$(xcrun -f strip)'
pkg-config = 'false'

[host_machine]
system = 'darwin'
subsystem = 'ios'
cpu_family = 'aarch64'
cpu = 'arm64'
endian = 'little'

[properties]
needs_exe_wrapper = true
INI
DRIVERS=${DRIVERS:-softpipe,freedreno}
BUILD=build-${DRIVERS//,/-}
[ -f $BUILD/build.ninja ] || meson setup $BUILD mesa-$VERSION --cross-file cross.ini --wrap-mode=nofallback \
	--prefix=/usr/local --libdir=lib --buildtype=release -Db_ndebug=true \
	-Dplatforms= -Dgallium-drivers="$DRIVERS" -Dfreedreno-kmds=msm -Dvulkan-drivers= -Dglx=disabled -Degl=disabled -Dgbm=disabled \
	-Dosmesa=true -Dllvm=disabled -Dshared-glapi=enabled -Dgles1=disabled -Dgles2=enabled -Dopengl=true \
	-Dzstd=disabled -Dxmlconfig=disabled -Dexpat=disabled -Dshader-cache=disabled -Dvalgrind=disabled \
	-Dlibunwind=disabled -Dglvnd=disabled -Dvideo-codecs= -Dtools= -Dgallium-vdpau=disabled -Dgallium-va=disabled \
	-Dgallium-xa=disabled -Dlmsensors=disabled -Dbuild-tests=false > meson-setup.log
ninja -C $BUILD > ninja.log
mkdir -p out
$(xcrun -f strip) -x -o out/libOSMesa.8.dylib $BUILD/src/gallium/targets/osmesa/libOSMesa.8.dylib
# dyld refuses @executable_path here: our built-in AMFI does not allow @ path
# expansion (amfi_check_dyld_policy_self). SWEETBOX puts files in /private/var/tmp.
install_name_tool -id /private/var/tmp/libOSMesa.8.dylib out/libOSMesa.8.dylib
codesign -f -s - out/libOSMesa.8.dylib
cp mesa-$VERSION/include/GL/osmesa.h out/
ls -l out
