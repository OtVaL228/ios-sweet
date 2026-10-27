#!/bin/bash
# Build the SWEET kernel on this Mac, as .github/workflows/xnu.yml does on
# GitHub (for when its macOS arm64 runners have no capacity).
#
#   tools/xnu-local-build.sh --full   first time, or after a patch already
#                                     applied has changed: everything from scratch
#   tools/xnu-local-build.sh          apply the xnu/patches/*.patch not applied
#                                     yet, then make (incremental)
#
# Kernel: $XNU_LOCAL/dxb/build/xnu.obj/DEVELOPMENT_ARM64_SWEET/kernel.development.sweet
# (XNU_LOCAL defaults to ../xnu-local next to the repository). Give that path
# to tools/guac-mkboot.sh instead of a run id.
#
# Differences from CI:
# - Xcode: whatever is selected here (CI pins Xcode 15 on macos-14). The iig of
#   Xcode 16+ emits IORPC.kernelContent, which xnu-8796 lacks, so
#   xnu/local/*.patch adds it (as xnu-12377 has it); CI does not apply those.
# - KDK 13.5 (22G74) comes from dortania/KdkSupportPkg like build.sh fetches
#   it, but is unpacked under $XNU_LOCAL (KDKROOT) instead of installed with
#   sudo; xcodes is a stub in $XNU_LOCAL/bin, as in CI.
# Needs brew's jq, gum, cmake, ninja.
set -euo pipefail
REPO=$(cd "$(dirname "$0")/.." && pwd)
L=${XNU_LOCAL:-$(dirname "$REPO")/xnu-local}
DXB_REPO=$(sed -n 's/^  DXB_REPO: //p' "$REPO/.github/workflows/xnu.yml")
DXB_SHA=$(sed -n 's/^  DXB_SHA: //p' "$REPO/.github/workflows/xnu.yml")
KDK_NAME="Kernel Debug Kit 13.5 build 22G74"
KDK=$L/KDK_13.5_22G74.kdk
APPLIED=$L/applied-patches.txt
export PATH=$L/bin:$PATH MACOS_VERSION=13.5 KERNEL_CONFIG=DEVELOPMENT MACHINE_CONFIG=SWEET KDKROOT=$KDK

mkdir -p "$L/bin"
printf '#!/bin/sh\nexit 0\n' > "$L/bin/xcodes"
chmod +x "$L/bin/xcodes"

if [ ! -d "$KDK/System" ]; then
	url=$(curl -s https://raw.githubusercontent.com/dortania/KdkSupportPkg/gh-pages/manifest.json |
	    jq -r --arg n "$KDK_NAME" '.[] | select(.name==$n) | .url')
	rm -rf "$L/kdk-tmp" && mkdir -p "$L/kdk-tmp"
	curl -L --fail -o "$L/kdk-tmp/KDK.dmg" "$url"
	hdiutil attach -readonly -nobrowse -mountpoint "$L/kdk-tmp/mnt" "$L/kdk-tmp/KDK.dmg" >/dev/null
	pkgutil --expand-full "$L/kdk-tmp/mnt/KernelDebugKit.pkg" "$L/kdk-tmp/pkg" >/dev/null
	hdiutil detach "$L/kdk-tmp/mnt" -quiet
	mv "$L/kdk-tmp/pkg/KDK.pkg/Payload" "$KDK"
	rm -rf "$L/kdk-tmp"
fi

record() {
	(cd "$REPO" && shasum -a 256 xnu/patches/*.patch xnu/local/*.patch) > "$APPLIED"
}

if [ "${1:-}" = "--full" ] || [ ! -f "$APPLIED" ]; then
	rm -rf "$L/dxb"
	git init -q "$L/dxb"
	git -C "$L/dxb" fetch -q --depth 1 "$DXB_REPO" "$DXB_SHA"
	git -C "$L/dxb" checkout -q FETCH_HEAD
	cp "$REPO"/xnu/patches/*.patch "$REPO"/xnu/patches/*.sh "$L/dxb/patches/"
	# xnu/local after ours: the same file name order build.sh applies them in
	for p in "$REPO"/xnu/local/*.patch; do
		cp "$p" "$L/dxb/patches/9999-local-$(basename "$p")"
	done
	sed -i '' 's/ARCHS="x86_64 arm64e" VALID_ARCHS="x86_64 arm64e"/ARCHS="x86_64 arm64e arm64" VALID_ARCHS="x86_64 arm64e arm64"/' "$L/dxb/build.sh"
	(cd "$L/dxb" && bash build.sh)
	record
else
	X=$L/dxb/xnu
	while read -r sum file; do
		now=$(cd "$REPO" && shasum -a 256 "$file" 2>/dev/null | cut -d' ' -f1 || true)
		if [ "$now" != "$sum" ]; then
			echo "$file changed (or is gone) since it was applied: run $0 --full"
			exit 1
		fi
	done < "$APPLIED"
	for p in "$REPO"/xnu/patches/*.patch; do
		rel=xnu/patches/$(basename "$p")
		if ! grep -q " $rel\$" "$APPLIED"; then
			echo "applying $rel"
			git -C "$X" apply "$p"
		fi
	done
	sed '$d' "$L/dxb/build.sh" > "$L/dxb/build-functions.sh"
	(cd "$L/dxb" && source ./build-functions.sh && BUILDKC=0 BUILDLIB=0 &&
	    setup_xcode_toolchain && choose_xnu && venv && rm -f build/xnu.obj/kernel.development.sweet &&
	    build_xnu)
	record
fi
ls -l "$L/dxb/build/xnu.obj/DEVELOPMENT_ARM64_SWEET/kernel.development.sweet"
