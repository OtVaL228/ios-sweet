# Tools from ios-flame

`lsreg.m`, `oslog.m` and their entitlement plists are copied unchanged from
[NWylynko/ios-flame](https://github.com/NWylynko/ios-flame) `phone/tools`
(commit 745c434), iOS 16 on the Pixel 4 (SM8150, like guacamole).

- `lsreg`: LaunchServices registration (`-r` rebuilds the app databases, `-l` lists
  apps, `-p` registers one .appex). Signed with com.apple.migrationpluginwrapper's
  entitlements (key names only).
- `oslog`: streams the unified log (`oslog [-t SECS] [-l] [PROCESS...]`), which iOS has
  no command for.

## Licence

```
MIT License

Copyright (c) 2026 dipa-studio

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
