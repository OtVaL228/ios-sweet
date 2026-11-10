#!/usr/bin/env python3
"""
insert-dylib.py BINARY DYLIB_PATH [--weak] [--check]

Adds an LC_LOAD_DYLIB (or LC_LOAD_WEAK_DYLIB) of DYLIB_PATH to an arm64 Mach-O executable, in place (thin or fat;
only the arm64 slices are changed). Used to make an OpenGL ES app load /usr/lib/libsweet-gl.dylib (userland/gpu
eagl.c, 0190). The new load command goes into the free space after the existing ones; the binary's code signature
is invalid afterwards: re-sign ad hoc (codesign -f -s - BINARY, or ldid -S BINARY). --check only reports.
ios-sweet, MIT.
"""
import struct, sys

MH_MAGIC_64 = 0xfeedfacf
FAT_MAGIC = 0xcafebabe
FAT_MAGIC_64 = 0xcafebabf
CPU_TYPE_ARM64 = 0x0100000c
LC_LOAD_DYLIB = 0xc
LC_LOAD_WEAK_DYLIB = 0x80000018
LC_SEGMENT_64 = 0x19
DYLIB_CMDS = (0xc, 0x80000018, 0x8000001f, 0x80000023)  # load, weak, reexport, lazy (upward 0x80000023)


def patch_slice(data, base, path, weak, check):
    magic, cpu, sub, ftype, ncmds, sizeofcmds, flags, reserved = struct.unpack_from('<IiiIIIII', data, base)
    if magic != MH_MAGIC_64:
        raise SystemExit('not a 64-bit Mach-O slice at %#x' % base)
    off = base + 32
    first_data = None
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<II', data, off)
        if cmd in DYLIB_CMDS:
            name_off = struct.unpack_from('<I', data, off + 8)[0]
            name = bytes(data[off + name_off:off + size]).split(b'\0')[0].decode()
            if name == path:
                print('slice %#x: already loads %s' % (base, path))
                return False
        if cmd == LC_SEGMENT_64:
            nsects = struct.unpack_from('<I', data, off + 64)[0]
            for i in range(nsects):
                s = off + 72 + i * 80
                sect_size, sect_off = struct.unpack_from('<QI', data, s + 40)
                stype = struct.unpack_from('<I', data, s + 64)[0] & 0xff
                if sect_off and sect_size and stype not in (0x1, 0xc, 0x12):  # not zerofill
                    first_data = sect_off if first_data is None else min(first_data, sect_off)
        off += size
    end = base + 32 + sizeofcmds
    name = path.encode() + b'\0'
    cmdsize = (24 + len(name) + 7) & ~7
    room = (base + first_data) - end if first_data is not None else 0
    print('slice %#x: %d load commands, %d bytes of them, %d bytes free before the first section; need %d'
          % (base, ncmds, sizeofcmds, room, cmdsize))
    if check:
        return False
    if room < cmdsize:
        raise SystemExit('not enough room for the load command')
    if any(data[end:end + cmdsize]):
        raise SystemExit('the space after the load commands is not empty')
    cmd = struct.pack('<IIIIII', LC_LOAD_WEAK_DYLIB if weak else LC_LOAD_DYLIB, cmdsize, 24, 2, 0x10000, 0x10000)
    cmd += name + b'\0' * (cmdsize - 24 - len(name))
    data[end:end + cmdsize] = cmd
    struct.pack_into('<II', data, base + 16, ncmds + 1, sizeofcmds + cmdsize)
    print('slice %#x: added %s' % (base, path))
    return True


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    if len(args) != 2:
        raise SystemExit(__doc__)
    binary, path = args
    weak, check = '--weak' in sys.argv, '--check' in sys.argv
    data = bytearray(open(binary, 'rb').read())
    magic = struct.unpack_from('>I', data, 0)[0]
    changed = False
    if magic in (FAT_MAGIC, FAT_MAGIC_64):
        n = struct.unpack_from('>I', data, 4)[0]
        for i in range(n):
            if magic == FAT_MAGIC:
                cpu, sub, off, size, align = struct.unpack_from('>iiIII', data, 8 + i * 20)
            else:
                cpu, sub, off, size, align, _ = struct.unpack_from('>iiQQII', data, 8 + i * 32)
            if cpu == CPU_TYPE_ARM64:
                changed |= patch_slice(data, off, path, weak, check)
    else:
        changed = patch_slice(data, 0, path, weak, check)
    if changed:
        open(binary, 'wb').write(data)
        print('written; now re-sign: codesign -f -s - %s   (or: ldid -S %s)' % (binary, binary))


if __name__ == '__main__':
    main()
