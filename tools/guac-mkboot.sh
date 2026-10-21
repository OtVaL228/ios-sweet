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
set -euo pipefail
cd "$(dirname "$0")/.."
[ $# -ge 4 ] || { sed -n '2,11p' "$0"; exit 1; }
RUN=$1 STOCK=$2 SHIM=$3 OUT=$4 EXTRA=${5:-}
K=xnu/out-$RUN/kernel.development.sweet
if [ ! -f "$K" ]; then
	gh run download "$RUN" -n xnu-kernel -D "xnu/out-$RUN"
fi
[ -f "$K" ] || { echo "no $K in the artifact"; ls "xnu/out-$RUN"; exit 1; }
mkdir -p shim/out
python3 tools/mkadt.py shim/out/guac-hi.adt --highmem --guacamole
ARGS="debug=0xc serial=3 cpus=1 keepsyms=1 -enable_kprintf_spam -noprogress sweettouch=0 sweetkeys=0 sweetusb=0 wdt=-1 $EXTRA"
python3 tools/mkpayload.py "$K" shim/out/guac-hi.adt shim/out/payload-guac.bin --cmdline "$ARGS"
python3 tools/mkguacboot.py "$STOCK" "$SHIM" "$OUT" --ramdisk shim/out/payload-guac.bin
echo "boot-args: $ARGS"
echo "next: fastboot boot $OUT"
