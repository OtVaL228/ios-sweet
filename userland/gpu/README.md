# EAGLContext on Mesa freedreno (G4)

`libsweet-gl.dylib` makes iOS's `EAGLContext` work on our Mesa (OSMesa with an
OpenGL ES profile, freedreno on the Adreno 640 through RedwoodGPU), so that
CoreAnimation's render server in backboardd draws with the GPU instead of its
software rasterizer. `userland/bbd/bbd-insert.c` loads it into backboardd once
the kernel has published RedwoodGPU.

- `eagl.c`: replaces seven methods of `EAGLContext` (ObjC runtime, no code
  patching) and hands CoreAnimation a function table at the offsets of iOS's
  own OpenGLES (`getMacroContextPrivate`).
- `eagl_slots.txt`: those offsets, read off iOS 16.5's OpenGLES by HackiOS.
  `tools/check-eagl-slots.py` compares it with a disassembly of our iOS
  16.7.16 OpenGLES (from the IPSW's dyld cache): all 457 names match.
- `mkeagl.py`: generates `eagl_gen.h` (the table) from the slots, Mesa's
  `gl.xml` and the list of functions libOSMesa exports.
- `gl_osmesa.c`, `redwood_gl.h`: the OSMesa side.

Changes of ours in `eagl.c`: the glwho=1 debug lines (`who_made()`, and a
line when a surface with no pixel format changes), marked "ios-sweet".

## Origin and licence

`eagl.c`, `gl_osmesa.c` and `redwood_gl.h` are taken from
[MakrSas/ios-topaz](https://github.com/MakrSas/ios-topaz) `userland/gpu`
(commit e4772911), which adapted them from HackiOS user space.
`mkeagl.py` and `eagl_slots.txt` are taken from HackiOS,
[oosexgang/hackios-xnu-kernel](https://github.com/oosexgang/hackios-xnu-kernel)
`userland/` (commit 88513176), whose LICENSE puts its user-space code under MIT.
Both notices:

```
MIT License

Copyright (c) 2026 OOSEX GANG
Copyright (c) 2025-2026 the HakiOS authors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```
