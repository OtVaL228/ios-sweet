#!/bin/bash
# Build a OnePlus 7 Pro (guacamole) boot image: CI kernel + ADT + shim.
# Runs on macOS/Linux with python3 and gh; nothing is written to the phone.
#
#   tools/guac-mkboot.sh RUN_ID STOCK_boot.img SHIM.Image OUT.img ["extra boot-args"]
#
# RUN_ID: the `xnu` Actions run of this fork (gh run list -w xnu).
# Default boot-args are the safe first-boot set: no touch/keys/USB drivers
# (their addresses are sweet's), no rd= and no ufsrw, so the kernel never
# touches userdata and stops at "waiting for root device".
# CPUS=N (default 1) sets cpus=; e.g. CPUS=8 tools/guac-mkboot.sh ...
# KEYS=0/1 (default 1, PM8150 keys from 0084), TOUCH=0/1 and USB=0/1/2
# (default 0: still sweet hardware) set sweetkeys/sweettouch/sweetusb.
set -euo pipefail
[ $# -ge 4 ] || { sed -n '2,11p' "$0"; exit 1; }
# Resolve paths against the caller's directory BEFORE changing into the repo.
abs() { case "$1" in /*) echo "$1" ;; *) echo "$PWD/$1" ;; esac; }
RUN=$1 STOCK=$(abs "$2") SHIM=$(abs "$3") OUT=$(abs "$4") EXTRA=${5:-}
[ -f "$STOCK" ] || { echo "stock boot image not found: $STOCK"; exit 1; }
[ -f "$SHIM" ] || { echo "shim Image not found: $SHIM"; exit 1; }
cd "$(dirname "$0")/.."
K=xnu/out-$RUN/kernel.development.sweet
if [ ! -f "$K" ]; then
	gh run download "$RUN" -n xnu-kernel -D "xnu/out-$RUN"
fi
[ -f "$K" ] || { echo "no $K in the artifact"; ls "xnu/out-$RUN"; exit 1; }
mkdir -p shim/out
python3 tools/mkadt.py shim/out/guac-hi.adt --highmem --guacamole
ARGS="debug=0xc serial=3 cpus=${CPUS:-1} keepsyms=1 -enable_kprintf_spam -noprogress sweettouch=${TOUCH:-0} sweetkeys=${KEYS:-1} sweetusb=${USB:-0} wdt=-1 $EXTRA"
python3 tools/mkpayload.py "$K" shim/out/guac-hi.adt shim/out/payload-guac.bin --cmdline "$ARGS"
python3 tools/mkguacboot.py "$STOCK" "$SHIM" "$OUT" --ramdisk shim/out/payload-guac.bin
echo "boot-args: $ARGS"
echo "next: fastboot boot $OUT"
