#!/bin/sh
# Build the native Metal bridge (userland/metal2/bridge/NativeMetalBridge.rs: UniXNU's bridge over the
# metal2vulkan runtime, MIT; via ios-flame) as a static library for iOS 16 arm64.
#   tools/build-m2v-bridge.sh [host]
# Source: $METAL_WORK/metal2vulkan = github.com/steelbrain/metal2vulkan 720ef1ce (UniXNU's pin) +
# userland/metal2/bridge/patches (GPL-2.0-or-later OR LGPL-3.0-or-later; fetched, never stored here).
# METAL_WORK defaults to ../metal-work next to the repository (like ../xnu-local).
# Needs rustup (brew install rustup; rustup toolchain install 1.98.0; rustup target add aarch64-apple-ios)
# and Homebrew llvm (llvm-as). Cargo fetches the crates the runtime depends on.
# Output: $METAL_WORK/m2v-bridge/<target>/libnative_metal_bridge.a
set -e
cd "$(dirname "$0")/.."
ROOT=$PWD
W=${METAL_WORK:-$(dirname "$ROOT")/metal-work}
SRC=$W/metal2vulkan
OUT=$W/m2v-bridge
target=${1:-ios}
export PATH=/opt/homebrew/opt/rustup/bin:$PATH
if [ ! -d "$SRC/crates" ]; then
	git init -q "$SRC"
	git -C "$SRC" fetch -q --depth 1 https://github.com/steelbrain/metal2vulkan.git 720ef1ce9ae2a2068999a8675717418b9416283e
	git -C "$SRC" checkout -q FETCH_HEAD
	for p in "$ROOT"/userland/metal2/bridge/patches/*.patch; do git -C "$SRC" apply "$p"; done
fi
mkdir -p "$OUT"
LLVM_AS=/opt/homebrew/opt/llvm/bin/llvm-as
"$LLVM_AS" -o "$OUT/runtime_examples.air" "$SRC/crates/metal2vulkan-validation/fixtures/public/examples/runtime_examples.ll"
"$LLVM_AS" -o "$OUT/function_constant.air" "$SRC/crates/metal2vulkan-validation/fixtures/public/kernel_function_constant_gate_fold.ll"
cat > "$OUT/Cargo.toml" <<EOT
[package]
name = "native-metal-bridge"
version = "0.1.0"
edition = "2024"
rust-version = "1.98"
[workspace]
[lib]
crate-type = ["staticlib"]
path = "$ROOT/userland/metal2/bridge/NativeMetalBridge.rs"
[dependencies]
metal2vulkan = { path = "$SRC/crates/metal2vulkan", features = ["ash"] }
metal2vulkan-runtime = { path = "$SRC/crates/metal2vulkan-runtime" }
libc = "0.2"
sha2 = { version = "0.10", features = ["asm"] }
[profile.release]
panic = "unwind"
debug = 1
EOT
if [ "$target" = ios ]; then triple=aarch64-apple-ios; else triple=aarch64-apple-darwin; fi
NMB_FIXTURE_AIR=$OUT/runtime_examples.air NMB_FC_AIR=$OUT/function_constant.air IPHONEOS_DEPLOYMENT_TARGET=16.0 \
	MACOSX_DEPLOYMENT_TARGET=14.0 cargo +1.98.0 build --release --manifest-path "$OUT/Cargo.toml" --target $triple \
	--target-dir "$OUT/target" > "$OUT/build-$target.log" 2>&1 || { grep -E "^(error|warning: unused)" -A6 "$OUT/build-$target.log" | head -60; exit 1; }
mkdir -p "$OUT/$target"
cp "$OUT/target/$triple/release/libnative_metal_bridge.a" "$OUT/$target/"
ls -la "$OUT/$target/libnative_metal_bridge.a"
