#!/usr/bin/env python3
"""mkguacboot.py - wrap a shim Image into an Android boot image v2 for the
OnePlus 7 Pro (guacamole), reusing the DTB section and header fields
(addresses, os_version/patch level) of the phone's own stock boot image.

  python3 mkguacboot.py STOCK_boot.img guac-probe.Image OUT.img [--ramdisk FILE]

Only Python 3 standard library; runs on macOS/Linux. Writes nothing to the phone.
"""
import argparse, gzip, hashlib, io, struct, sys

HDR_FMT_HEAD = "<8s10I16s512s32s1024sIQIIQ"  # boot_img_hdr_v2, 1660 bytes


def parse_v2(b):
    if b[:8] != b"ANDROID!":
        sys.exit("stock image: no ANDROID! magic")
    f = struct.unpack_from(HDR_FMT_HEAD, b, 0)
    (magic, ksz, kaddr, rsz, raddr, ssz, saddr, tags, page, hver, osver,
     name, cmdline, _id, extra, rdtbo_sz, rdtbo_off, hdr_sz, dtb_sz, dtb_addr) = f
    if hver != 2:
        sys.exit("stock image has header_version %d, expected 2" % hver)
    pages = lambda n: (n + page - 1) // page
    off = page * (1 + pages(ksz) + pages(rsz) + pages(ssz) + pages(rdtbo_sz))
    dtb = b[off: off + dtb_sz]
    if len(dtb) != dtb_sz or dtb[:4] != b"\xd0\x0d\xfe\xed":
        sys.exit("stock image: DTB section not found or not an FDT")
    return dict(kaddr=kaddr, raddr=raddr, saddr=saddr, tags=tags, page=page, osver=osver,
                name=name, dtb=dtb, dtb_addr=dtb_addr)


def empty_cpio_gz():
    name = b"TRAILER!!!\0"
    hdr = b"070701" + b"".join(b"%08x" % v for v in (0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, len(name), 0))
    body = hdr + name
    body += b"\0" * (-len(body) % 4)
    body += b"\0" * (-len(body) % 512)
    out = io.BytesIO()
    with gzip.GzipFile(fileobj=out, mode="wb", mtime=0) as g:
        g.write(body)
    return out.getvalue()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("stock")
    ap.add_argument("image")
    ap.add_argument("out")
    ap.add_argument("--ramdisk", help="payload for the ramdisk field (default: empty cpio)")
    ap.add_argument("--cmdline", default="console=null")
    a = ap.parse_args()

    st = parse_v2(open(a.stock, "rb").read())
    kernel = open(a.image, "rb").read()
    if kernel[56:60] != b"ARM\x64":
        sys.exit("image has no arm64 Image header")
    ramdisk = open(a.ramdisk, "rb").read() if a.ramdisk else empty_cpio_gz()
    dtb, page = st["dtb"], st["page"]

    sha = hashlib.sha1()
    for blob in (kernel, ramdisk, b"", b"", dtb):  # kernel, ramdisk, second, recovery_dtbo, dtb
        sha.update(blob)
        sha.update(struct.pack("<I", len(blob)))
    hdr = struct.pack(HDR_FMT_HEAD, b"ANDROID!",
                      len(kernel), st["kaddr"], len(ramdisk), st["raddr"], 0, st["saddr"],
                      st["tags"], page, 2, st["osver"], st["name"],
                      a.cmdline.encode()[:511], sha.digest() + b"\0" * 12, b"",
                      0, 0, 1660, len(dtb), st["dtb_addr"])
    assert len(hdr) == 1660
    pad = lambda x: x + b"\0" * (-len(x) % page)
    img = pad(hdr) + pad(kernel) + pad(ramdisk) + pad(dtb)
    open(a.out, "wb").write(img)
    print("wrote %s: %d bytes (kernel %d, ramdisk %d, dtb %d, page %d, kernel_addr %#x, dtb_addr %#x)"
          % (a.out, len(img), len(kernel), len(ramdisk), len(dtb), page, st["kaddr"], st["dtb_addr"]))
    print("sha256", hashlib.sha256(img).hexdigest())


if __name__ == "__main__":
    main()
