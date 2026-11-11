# ios-sweet: guacamole

**Apple's iOS 16 running on a OnePlus 7 Pro.**

This branch ports Apple's open-source XNU kernel (xnu-8796, the iOS 16 generation) to the OnePlus 7 Pro
(`guacamole`, Qualcomm SM8150 / Snapdragon 855, 8 GB, UFS 3.0, 1440×3120 AMOLED). On top of it runs the
**unmodified iOS 16.7.16 (20H392) userland of the iPhone X**: launchd, the system daemons, backboardd,
SpringBoard, the stock apps and sideloaded apps, drawn on the phone's **Adreno 640 GPU**.

> Experimental bring-up, not a usable phone OS. Many things are missing or stubbed (see below), and Apple's
> security model is mostly switched off. It installs into the `userdata` partition and so **erases Android's
> user data**.

## Status

Status as of **2026-11-10** (details and history: [`docs/guacamole.md`](docs/guacamole.md), `NOTES.md`).

### Works

| Area | State |
| --- | --- |
| Boot, CPU | Stock OnePlus ABL → `fastboot boot` of the shim; all 8 cores (4× A55, 3× A76, 1× A76 prime) |
| Display | 1125×2436 presented, scaled to the 1440×3120 panel; 90 Hz by default (0199, `sweetfb90=0` for 60 Hz); brightness slider and Night Shift through `sweet-displayd` (0197) |
| Touch, buttons | Samsung S6SY761 over I²C (QUP SE17); power and volume over SPMI |
| GPU | Adreno 640 brought up from the kernel (GMU, HFI, zap shader, ring, fences); `RedwoodGPU` user client speaking Linux msm's DRM requests |
| Graphics | OpenGL ES: Mesa 25.0.7 freedreno behind iOS's `EAGLContext`; CoreAnimation in backboardd renders on the GPU. Metal: SweetMetal2 (metal2vulkan + Turnip) for backboardd, WebKit and selected apps (`/var/local/mtl2-clients`) |
| Apps and games | Stock removable apps, Settings, Photos, Wallet, Safari; sideloaded games such as Subway Surfers and Angry Birds run on the GPU (support varies per game); share sheet and Game Center sign-in sheet appear |
| Wi-Fi | WPA2-PSK join, DHCP, internet (`userland/wland` over `/dev/pmem`, en0 + Apple80211 for wifid and Settings); starts at boot (`com.sweet.wland`) |
| Bluetooth | Qualcomm WCN3998 behind iOS's Broadcom-expecting stack via `userland/bt/btbridge` at 3 Mbaud, kernel controls + Skywalk channel (0220–0223); starts at boot (`com.sweet.btbridge`). Scanning, pairing, encryption, SDP, AVRCP, HFP, AAP and A2DP setup with Beats / AirPods; the headset appears in Settings and plays audio (A2DP) |
| SSH | Procursus `sshd` at boot (`com.sweet.sshd`) |
| Wallpaper | Wallpaper picker and poster providers; photo wallpapers draw (YCbCr photos converted on the CPU, 0217; the colour fix 0224 is not yet confirmed on the phone) |
| Clock | Wall time from the PM8150 RTC plus an offset kept on the root (`sweettime=` sets it) |
| Battery | Percentage, voltage and current from the PM8150B fuel gauge, charger state from its SMB5 (`sweetbattery=2`) |
| File drop, autorun | SWEETBOX: a raw image in the `rawdump` partition is unpacked into `/private/var/tmp` (and `ROOT/` entries onto the root); `/private/var/tmp/autorun.sh` runs at boot |

### Partly working / in progress

| Area | State |
| --- | --- |
| Audio | Bluetooth headphones play (A2DP). mediaserverd loads `HakiosAudio.driver` (`userland/audio`, from [ios-apollo](https://github.com/aTechGeekDev/ios-apollo) / hakios-apollo, MIT) with the iPhone X's device names (Speaker, Hawking, Codec, Actuator), so VirtualAudio builds the speaker, built-in mic and actuator routes; boot with `sweetaudio=0`. The phone's own speaker and mic are silent (no kernel audio path to the DSP / amplifiers yet) |
| Dynamic Island | Optional (`ArtworkDeviceSubType` 2556 via `mgtool`); disappeared over full-screen games until the framebuffer edge-band fix (0225, not yet confirmed on the phone) |
| Framebuffer composition | Two-layer frames (app + CoreAnimation) on the GPU; partly transparent overlays fall back to a CPU blend, which costs frame rate |
| Safari | Pages render (GPU page drawing on); video plays (software decode); YouTube plays videos without DRM (FairPlay ones show a licensing error) |
| Photos | Opens; has crashed in some sessions |

### Does not work (yet)

- **Audio**: see above (Bluetooth headphones work; no speaker, earpiece or microphone).
- **Cellular, SIM, calls, SMS**: no modem stack (`CommCenter` removed from launchd).
- **Camera, sensors** (accelerometer, gyro, proximity, ambient light), **GPS, NFC**, haptics.
- **SEP / Face ID, Touch ID, passcode-backed data protection**: AppleKeyStore is a stub; security is mostly off.
- **App Store, Apple ID / iCloud**, FairPlay-encrypted apps, AirPlay (pairing fails).
- **CPU frequency scaling**, power management, sleep.
- Bluetooth housekeeping: the controller uses the NVM's default address (no per-device BD_ADDR yet), btbridge polls the UART (CPU), Wi-Fi/Bluetooth coexistence is not configured.

## How it boots

```
ABL (stock OnePlus bootloader, unlocked)
  -> fastboot boot shim image                 (temporary; nothing is flashed to boot)
     -> shim: own MMU setup, reads the live FDT, loads the payload from the ramdisk
        -> XNU Mach-O + generated Apple Device Tree (ADT) + boot-args
           -> XNU -> UFS driver -> HFS+ root in the userdata partition
              -> dyld + shared cache -> launchd -> daemons -> backboardd + SpringBoard
```

The kernel draws its console into the framebuffer the bootloader leaves behind and keeps a persistent log ring
at the end of `userdata`; read it from TWRP with `tools/sweet-log-pull.sh` (see [`docs/guacamole.md`](docs/guacamole.md),
Workflow). Never format or wipe Data in recovery: `userdata` holds the iOS root.

The iOS root image is built from the iPhone X (iPhone10,3) 16.7.16 (20H392) IPSW with `tools/mkiosroot.sh`
(Linux, `hfsplus` module), then `tools/sweet-root-tweaks.py --net` (Setup skipped, activation seeded, baseband
daemon removed, Wi-Fi / Bluetooth / SSH launchd jobs), `img2simg` and `fastboot flash userdata`.

## Building

- **Kernel:** `gh workflow run xnu.yml --ref guacamole` on GitHub, or on a Mac with
  `tools/xnu-local-build.sh --full` once and `tools/xnu-local-build.sh` afterwards (uses the Mac's
  Xcode; `xnu/local/` adapts xnu to a newer iig).
- **Boot image:** `TOUCH=1 CPUS=8 tools/guac-mkboot.sh RUN_ID_OR_KERNEL_PATH stock_boot_b.img
  shim-guacamole.Image out.img "<boot-args>"`; it prints the final `boot-args:` line.
- **Userland:** `userland/build.sh` (autorun helper, backboardd wrapper, GPU tests, the EAGL library);
  Mesa first with `userland/mesa/mk-mesa.sh` (needs Homebrew's bison 3).
- **Getting files onto the phone:** `tools/sweet-mailbox.py pack box.img NAME FILE ...`, write it to
  `rawdump` from recovery, boot with `sweetmailbox=1 sweetboxpart=rawdump` (`sweetboxroot=1` for
  `ROOT/` entries).

| Boot-arg | Effect |
| --- | --- |
| `sweetgpu=11` | Bring up the Adreno 640 and publish `RedwoodGPU` |
| `sweetgl=0` | Keep CoreAnimation on its software renderer |
| `sweetfb_hz=60` | Panel refresh rate |
| `sweetmailbox=1 sweetboxpart=rawdump sweetboxroot=1` | Unpack a SWEETBOX drop (and its `ROOT/` entries) |
| `sweettime=SECONDS` | Unix time now, for the RTC offset (added by `guac-mkboot.sh`) |
| `sweetbattery=2` | Real battery state from the PM8150B fuel gauge and charger (default: static 100 %) |
| `sweetpmem=1` | `/dev/pmem` for `wland` (root gets all physical memory: development only) |
| `sweetcsoff=1` | Tell installd and FrontBoard that code signing enforcement is off (only needed to install the staged system apps; makes driverkitd crash-loop) |

## Kernel patches

The series in `xnu/patches/` is applied in order; the early patches (0001–0069) are the common kernel bring-up, 0082 onward are this phone.

| # | What |
| --- | --- |
| 0001–0009 | Toolchain compatibility, SWEET machine config, non-PAC arm64, stubs for Apple-internal pieces |
| 0010–0018 | Early framebuffer console, physmap fix, PRNG without corecrypto, device-tree platform expert, timer PPI, boot CPU driver |
| 0019–0020 | Apple open-source libpthread kernel provider; BearSSL AES |
| 0021–0027 | RAM-disk root, mockfs, SHA-256, minimal AMFI, Apple open-source HFS+, MD5 |
| 0028–0034 | UFS host driver and root device, read/write root, DMA above 4 GiB |
| 0035–0045 | Entitlements, bindfs, AppleKeyStore stub, IOWatchdog, console crash reports |
| 0046 | IOSurface (IOCoreSurfaceRoot) and IOMobileFramebuffer (AppleCLCD) services |
| 0047–0055 | Diagnostics: persistent log, /dev/null tee, IOKit trace, heap strings and user stack sampler on crash; permissive Sandbox policy; HFS rename-swap; AppleKeyStore selectors |
| 0056–0057 | Throttled panel copy; no external identity resolver |
| 0058 | HID event services. Apple's IOHIDFamily-1915.140.3 user client and event queue, matching the iOS 16.7 userland |
| 0059 | iPhone X identity (`hw.product` / `hw.target` from the device tree) |
| 0060 | Power and volume keys over SPMI; touch over GENI I²C |
| 0061 | Present the display as 1125×2436 |
| 0062 | RPMh regulator vote for the touch rail; battery power source |
| 0063 | GENI FIFO packing fix and the Goodix GT9896 protocol |
| 0064 | Grant PRIV_WORK_INTERVAL (UIKit's CoreAnimation work interval) |
| 0065 | SMP: secondary A55 cores via PSCI CPU_ON |
| 0066–0067 | Fault-region crash info; console mute |
| 0068 | IOSurface lookup semantics for already-mapped surfaces |
| 0069 | Batched persistent-log flushes |
| 0082–0087 | guacamole: framebuffer geometry, 1125×2436 scaled to 1440×3120, PM8150 keys, S6SY761 touch |
| 0088–0114 | Adreno 640 bring-up in stages (clocks, GMU, HFI, GX, hw init, CP, zap, IB + fence), the `redwood_gpu_*` backend; SWEETBOX file drop |
| 0115–0119 | SWEETBOX `ROOT/` installs before launchd, autorun marker, file truncation |
| 0117 | `RedwoodGPU` user client (from topaz / HackiOS) |
| 0120 | 64-bit physical address in `pmap_sync_page_attributes_phys` |
| 0121 | AMFI answers dyld's policy query (@ paths, `DYLD_INSERT_LIBRARIES`) |
| 0122 | GPU fence cache maintenance |
| 0123–0132 | IOSurface work for the GPU renderer: wrapped client memory, 10-bit formats as BGRA, `IOSurfaceAddress`, surfaces as GPU buffers, display cache invalidation, instant screen-off |
| 0133 | PM8150 RTC wall clock |
| 0134 | AMFI boot-arg state query (`sweetcsoff`), every unsupported AMFI op logged once |
| 0135 | Sandbox extension tokens (tccd grants: Photos) |
| 0136 | PM8150B fuel gauge battery (`sweetbattery=2`) |
| 0137 | `AppleMobileFileIntegrity` stand-in answering trust cache queries (ios-flame 0103), so LaunchServices registers app extensions |
| 0138 | `/dev/pmem` (physical memory, SCM calls, RPMh votes) for userland drivers, `sweetpmem=1` (ios-flame 0094/0102) |

## Repository layout

| Path | What |
| --- | --- |
| `shim/` | Bare-metal boot shim (C + asm) packed as an Android boot.img v2 |
| `xnu/patches/` | The XNU patch series (applied in order by CI), plus staging scripts for libpthread, HFS and BearSSL |
| `.github/workflows/xnu.yml` | Kernel CI build |
| `tools/mkadt.py` | Generates the Apple Device Tree from the board's MMIO layout and the iPhone X identity |
| `tools/mkpayload.py` | Packs kernel + ADT + boot-args into the shim payload |
| `tools/mkiosroot.sh` | Builds the HFS+ iOS root from the IPSW images |
| `tools/sweet-root-tweaks.py` | Bring-up changes to the root (Setup skip, activation, daemons) |
| `tools/sweet-log-pull.sh`, `sweet-log.py` | Read the persistent kernel log from recovery |
| `tools/sweet-crash.py` | Symbolicate console crash reports |
| `tools/xnu-cc.py` | Local single-file compile check from a CI log |
| `tools/xnu-local-build.sh`, `xnu/local/` | Build the kernel on a Mac |
| `tools/guac-mkboot.sh`, `tools/mkguacboot.py`, `tools/sweet-mailbox.py` | Boot image from the phone's stock `boot.img`; SWEETBOX file drop |
| `userland/` | autorun, backboardd wrapper, GPU tests, Mesa / Turnip builds, EAGL on Mesa, SweetMetal2, Wi-Fi (`wland`), Bluetooth (`bt/`), display and jailbreak-side helpers |
| `tools/qemu-sweet.py` | QEMU test boots with framebuffer text decoding |
| `tools/adt.py`, `remotezip.py`, `unpack_bootv2.py`, `sparsediff.py` | Helpers |
| `docs/guacamole.md` | Board facts, GPU / Wi-Fi / Metal bring-up, workflow |
| `NOTES.md` | Detailed engineering log: every boot, crash and decision, plus hardware facts |

Not in git: partition backups, live device trees, recovery images, Apple firmware and root images, and downloaded or generated build trees. They contain device serials and calibration data, or Apple's copyrighted files.

## Board facts

CPU, memory, GPU, display, touch, buttons, PMICs, Wi-Fi and Bluetooth hardware of the OnePlus 7 Pro are in
[`docs/guacamole.md`](docs/guacamole.md) (Board facts) and `research/` notes; everything measured on the phone
is in [`NOTES.md`](NOTES.md).

## Credits

Built on Apple's open-source releases (xnu, IOHIDFamily, libpthread, hfs), [BearSSL](https://bearssl.org),
[blacktop/darwin-xnu-build](https://github.com/blacktop/darwin-xnu-build), [ipsw](https://github.com/blacktop/ipsw),
and Qualcomm's and OnePlus's published Linux kernel sources for the hardware details. iOS itself is Apple's and is
not distributed here.

The GPU work also builds on [HackiOS](https://github.com/oosexgang/hackios-xnu-kernel) (kernel APSL 2.0, user
space MIT) and [MakrSas/ios-topaz](https://github.com/MakrSas/ios-topaz): `RedwoodGPU.cpp`, several SweetSurface
changes, the Mesa patches, `redwood-drm` and the EAGL library come from there (origins and licences in the files
and in `userland/*/README.md`); on [Mesa](https://mesa3d.org) (freedreno, Turnip; MIT) and the Linux msm driver
for register sequences; on ios-flame's SweetMetal2 and Wi-Fi work (`userland/metal2`, `userland/wland`); and on
[dortania/KdkSupportPkg](https://github.com/dortania/KdkSupportPkg) for local kernel builds. The original
ios-sweet project (OtVaL228/ios-sweet) brought the same kernel and userland up first on another phone.
