#!/bin/sh
# Build Turnip (Mesa's Vulkan driver for Adreno) for iOS 16 arm64 with the darwin kernel backend, as ios-flame's
# tools/build-turnip-ios.sh does.
#   tools/build-turnip-ios.sh [host]     (host: a macOS build of the same source, for checks on the Mac)
# Source, fetched into $METAL_WORK/turnip (never stored here): Mesa 85fda3b3 + UniXNU's 8 patches/mesa at
# UniXNU 453f1a89 (BSD-2-Clause-Patent; the order of scripts/data/adreno-compiler-inputs.json) +
# userland/turnip/patches. The backend (tu_knl_darwin.cc) talks to the kernel's GPU user client through
# userland/turnip/NativeAdrenoABI.h. METAL_WORK defaults to ../metal-work next to the repository.
# Needs Homebrew bison, flex, glslang; a venv with meson, mako, pyyaml, packaging is made on the first run.
# Output: $METAL_WORK/turnip/build-<target>/src/freedreno/vulkan/libvulkan_freedreno.dylib
set -e
cd "$(dirname "$0")/.."
ROOT=$PWD
W=${METAL_WORK:-$(dirname "$ROOT")/metal-work}
T=$W/turnip
target=${1:-ios}
MESA_SHA=85fda3b36405bcaea8bffc64aa4f2651dc90770d
UNIXNU_SHA=453f1a893a923c81c99efc2ca08a4f4bd69312d3
mkdir -p "$T"
if [ ! -d "$T/venv" ]; then
	python3 -m venv "$T/venv"
	"$T/venv/bin/pip" -q install meson mako pyyaml packaging
fi
if [ ! -d "$T/mesa/src" ]; then
	git init -q "$T/mesa"
	git -C "$T/mesa" fetch -q --depth 1 https://chromium.googlesource.com/external/gitlab.freedesktop.org/mesa/mesa $MESA_SHA
	git -C "$T/mesa" checkout -q FETCH_HEAD
	mkdir -p "$T/patches"
	for p in freedreno-tools-nir ir3-darwin-build-id ir3-portable-asm-roundtrip offline-adreno-compiler-probe \
	    turnip-darwin-kmd turnip-graphics-cache-feedback turnip-empty-scissor turnip-builtin-shader-pack; do
		curl -sfL -o "$T/patches/$p.patch" "https://raw.githubusercontent.com/UniXNU/UniXNU/$UNIXNU_SHA/patches/mesa/$p.patch"
		git -C "$T/mesa" apply "$T/patches/$p.patch"
	done
	for p in "$ROOT"/userland/turnip/patches/*.patch; do git -C "$T/mesa" apply "$p"; done
fi
export PATH=$T/venv/bin:/opt/homebrew/opt/bison/bin:/opt/homebrew/opt/flex/bin:$PATH
opts="-Dgallium-drivers= -Dvulkan-drivers=freedreno -Dfreedreno-kmds=darwin -Dtu-darwin-abi-dir=$ROOT/userland/turnip
 -Dplatforms=macos -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dopengl=false -Dgles1=disabled -Dgles2=disabled
 -Dllvm=disabled -Dshared-glapi=disabled -Dxmlconfig=disabled -Dshader-cache=disabled -Dtools= -Dbuild-tests=false
 -Dvideo-codecs= -Dvulkan-layers= -Dlibunwind=disabled -Dlmsensors=disabled -Dvalgrind=disabled -Dexpat=disabled
 -Dzstd=disabled -Dbuildtype=release -Db_ndebug=true"
if [ "$target" = ios ]; then
	out=$T/build-ios
	sdk=$(xcrun --sdk iphoneos --show-sdk-path)
	cross=$T/ios16.cross
	flags="'-target', 'arm64-apple-ios16.0', '-isysroot', '$sdk', '-fstack-protector-strong'"
	cat > "$cross" <<EOF
[binaries]
c = '$(xcrun --sdk iphoneos --find clang)'
cpp = '$(xcrun --sdk iphoneos --find clang++)'
objc = '$(xcrun --sdk iphoneos --find clang)'
ar = '$(xcrun --sdk iphoneos --find ar)'
strip = '$(xcrun --sdk iphoneos --find strip)'
pkg-config = '/usr/bin/false'
[host_machine]
system = 'darwin'
cpu_family = 'aarch64'
cpu = 'arm64'
endian = 'little'
[properties]
needs_exe_wrapper = true
[built-in options]
c_args = [$flags]
cpp_args = [$flags]
objc_args = [$flags]
c_link_args = [$flags]
cpp_link_args = [$flags]
objc_link_args = [$flags]
EOF
	cross_arg="--cross-file $cross"
	export CC=$(xcrun --sdk iphoneos --find clang) CXX=$(xcrun --sdk iphoneos --find clang++)
else
	out=$T/build-host
	cross_arg=
fi
if [ -f "$out/build.ninja" ]; then
	meson configure "$out" $opts >/dev/null
else
	meson setup "$out" "$T/mesa" --wrap-mode=nodownload $cross_arg $opts > "$T/configure-$target.log" 2>&1 ||
		{ tail -30 "$T/configure-$target.log"; exit 1; }
fi
ninja -C "$out" src/freedreno/vulkan/libvulkan_freedreno.dylib > "$T/build-$target.log" 2>&1 ||
	{ grep -E "error:|FAILED" "$T/build-$target.log" | head -30; exit 1; }
lib=$out/src/freedreno/vulkan/libvulkan_freedreno.dylib
nm -gU "$lib" | grep -q " T _vkGetInstanceProcAddr" || { echo "no vkGetInstanceProcAddr export" >&2; exit 1; }
if [ "$target" = ios ]; then
	# iOS 16 lacks symbols newer SDKs declare: weak imports resolve to NULL there (the strchrnul lesson)
	nm -m "$lib" | grep "weak external" || true
fi
ls -la "$lib"
