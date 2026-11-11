#!/usr/bin/env python3
"""
tsample-report.py: turn `tsample PID` output into a profile (run on the Mac; needs Xcode's atos).

  tools/tsample-report.py SAMPLES.txt [--dsym userland/metal2/out/SweetMetal2.dSYM] [--image SweetMetal2] [--top 40]

tsample prints "T ..." (thread), "I <base> <path>" (loaded images), "C <slide>" (shared cache slide) and one
"S <depth> <pc> <pc> ..." line per sample (innermost frame first). Frames inside --image (default SweetMetal2,
whose dSYM holds the Rust bridge and the Objective-C frontend) are symbolicated with atos; frames elsewhere are
shown as image+offset. Prints, per function: self samples (innermost frame) and total samples (anywhere on the
stack), as percentages of all samples.
"""
import collections
import os
import subprocess
import sys


def arg(name, default):
    if name in sys.argv:
        return sys.argv[sys.argv.index(name) + 1]
    return default


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    path = sys.argv[1]
    dsym = arg('--dsym', 'userland/metal2/out/SweetMetal2.dSYM')
    want = arg('--image', 'SweetMetal2')
    top = int(arg('--top', '40'))
    images, samples = [], []
    for line in open(path, errors='replace'):
        p = line.split()
        if not p:
            continue
        if p[0] == 'I' and len(p) >= 3:
            images.append((int(p[1], 16), ' '.join(p[2:])))
        elif p[0] == 'S' and len(p) >= 2:
            samples.append([int(x, 16) for x in p[2:]])
        elif p[0] == 'T':
            print(line.rstrip())
    if not samples:
        sys.exit('no S lines')
    images.sort()

    def image_of(pc):
        best = None
        for base, name in images:
            if base <= pc:
                best = (base, name)
            else:
                break
        return best

    # the wanted image's base (the highest-based image whose path ends in it)
    base = None
    for b, name in images:
        if os.path.basename(name) == want:
            base = b
    pcs = sorted({pc for s in samples for pc in s})
    names = {}
    if base is not None and os.path.exists(dsym):
        inside = [pc for pc in pcs if (image_of(pc) or (0, ''))[0] == base]
        for i in range(0, len(inside), 500):
            chunk = inside[i:i + 500]
            out = subprocess.run(['atos', '-o', dsym, '-l', hex(base)] + [hex(pc) for pc in chunk],
                                 capture_output=True, text=True).stdout.splitlines()
            for pc, sym in zip(chunk, out):
                # "func (in SweetMetal2) (file.rs:123)" -> "func"
                names[pc] = sym.split(' (in ')[0][:140]
    for pc in pcs:
        if pc not in names:
            im = image_of(pc)
            names[pc] = f'{os.path.basename(im[1])}+{pc - im[0]:#x}' if im else hex(pc)

    n = len(samples)
    self_c, total_c, img_c = collections.Counter(), collections.Counter(), collections.Counter()
    for s in samples:
        if not s:
            continue
        self_c[names[s[0]]] += 1
        im = image_of(s[0])
        img_c[os.path.basename(im[1]) if im else '?'] += 1
        for name in {names[pc] for pc in s}:
            total_c[name] += 1
    print(f'{n} samples')
    print('\n== innermost frame by image ==')
    for name, c in img_c.most_common(12):
        print(f'{100 * c / n:5.1f}%  {name}')
    print('\n== self (innermost frame) ==')
    for name, c in self_c.most_common(top):
        print(f'{100 * c / n:5.1f}%  {name}')
    print('\n== total (anywhere on the stack) ==')
    for name, c in total_c.most_common(top):
        print(f'{100 * c / n:5.1f}%  {name}')


if __name__ == '__main__':
    main()
