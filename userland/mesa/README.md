# Mesa freedreno for the iOS 16 userland (G3)

`mk-mesa.sh` builds Mesa 25.0.7 (gallium freedreno + softpipe, OSMesa) for
iOS arm64 on the Mac with Xcode's clang. freedreno talks to the kernel's
`RedwoodGPU` user client (xnu patch 0117) through `redwood-drm/`, which stands
in for libdrm: each msm DRM request becomes a call of the user client with the
request number as the selector.

Output: `build/out/libOSMesa.8.dylib` (ad-hoc signed, install name
`@executable_path/libOSMesa.8.dylib`, so it can sit next to the test program in
/private/var/tmp after a SWEETBOX drop).

## Origin and licence

`patches/freedreno-darwin.patch`, `patches/osmesa-es-profile.patch` and
`redwood-drm/` are taken unchanged from
[MakrSas/ios-topaz](https://github.com/MakrSas/ios-topaz) `userland/mesa`
(commit e4772911), which based them on HackiOS user space
([oosexgang/hackios-xnu-userland](https://github.com/oosexgang/hackios-xnu-userland),
via vcvkk/darwin-redwood). `mk-mesa.sh` is adapted from topaz's
`mk-mesa-mac.sh`. Their licence:

```
MIT License

Copyright (c) 2026 OOSEX GANG

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

Mesa itself is MIT-licensed (see its own `docs/license.rst`); it is downloaded
by the script and not stored here.
