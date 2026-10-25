#!/usr/bin/env python3
"""
tools/sweet-mailbox.py — pack files into a SWEETBOX image for logdump.

Usage:
  # pack
  python3 tools/sweet-mailbox.py pack out.img bin/hello /build/hello lib/foo.dylib /build/foo.dylib

  # flash (TWRP shell, phone in recovery with adb)
  adb push out.img /tmp/mailbox.img
  adb shell 'dd if=/tmp/mailbox.img of=/dev/block/by-name/logdump bs=65536'

  # clear (wipe the mailbox so the kernel doesn't re-extract on next boot)
  python3 tools/sweet-mailbox.py clear out.img
  adb shell 'dd if=/dev/zero of=/dev/block/by-name/logdump bs=512 count=8'

The kernel (sweet_mailbox.c, sweetmailbox=1) reads logdump after the root
mounts, extracts each file to /var/sweet/<name> (creating directories),
then writes a 512-byte "done" block at offset 0 to stop re-extraction.
Permissions are 0755 for binaries (no extension) and 0644 for everything else.
"""
import struct, sys, os

MAGIC = b'SWEETBOX'      # 8 bytes: present = extract this boot
MAGIC_DONE = b'SWEETDNE'  # 8 bytes: already extracted, skip

def cmd_pack(args):
    if len(args) < 2 or len(args) % 2 != 0:
        sys.exit('usage: pack OUT DST1 SRC1 [DST2 SRC2 ...]')
    out_path = args[0]
    pairs = [(args[i], args[i+1]) for i in range(1, len(args), 2)]
    files = []
    for dst, src in pairs:
        data = open(src, 'rb').read()
        files.append((dst.encode('utf-8'), data))
        print(f'  + {dst}  ({len(data)} bytes)')
    with open(out_path, 'wb') as f:
        f.write(MAGIC)
        f.write(struct.pack('<I', len(files)))
        for name, data in files:
            f.write(struct.pack('<H', len(name)))
            f.write(name)
            f.write(struct.pack('<I', len(data)))
            f.write(data)
        pos = f.tell()
        f.write(b'\x00' * ((-pos) % 512))
    print(f'=> {out_path}  ({os.path.getsize(out_path)} bytes, {len(files)} files)')

def cmd_clear(args):
    if not args:
        sys.exit('usage: clear OUT')
    with open(args[0], 'r+b') as f:
        f.write(MAGIC_DONE)
    print(f'{args[0]}: magic set to SWEETDNE (kernel will skip on next boot)')

if __name__ == '__main__':
    if len(sys.argv) < 2:
        print(__doc__); sys.exit(0)
    sub = sys.argv[1]
    rest = sys.argv[2:]
    if sub == 'pack':
        cmd_pack(rest)
    elif sub == 'clear':
        cmd_clear(rest)
    else:
        sys.exit(f'unknown subcommand: {sub}')
