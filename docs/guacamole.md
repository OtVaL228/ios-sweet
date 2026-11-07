# guacamole: ios-sweet on the OnePlus 7 Pro (SM8150)

Port of ios-sweet (iOS 16.7.16 userland + XNU 8796) from the Redmi Note 10 Pro
(sweet, SM7150) to the OnePlus 7 Pro (guacamole, SM8150, 8 GB, 256 GB UFS).
Branch `guacamole`. Everything here was measured on the phone unless marked
as coming from a device tree (DT) or Linux.

## Status

| Area | State |
| --- | --- |
| Boot (`fastboot boot` of a shim image, stock ABL) | works |
| Reboot to fastboot from our code | works (PMIC PON reason + PSCI) |
| Framebuffer console, kernel log on screen | works |
| XNU kernel, 8 CPUs (4x A55 + 3x A76 + 1x A76 prime) | works |
| UFS root from `userdata`, persistent log ring | works |
| iOS userland: lock screen, home screen, apps | works (CPU rendering) |
| Display 1125x2436 presented, scaled x1.28 to 1440x3120 | works (0083); 60 Hz with sweetfb_hz=60 (gl16, 0132) |
| Buttons (power, volume) | works (0084) |
| Files app, On My iPhone | works (0160, 0161, userland/jb fpwrap + libsweet-fpfs; see Files app) |
| Alert Slider as the ring/silent switch | works (0158; MobileGestalt ringer-switch answer, see Alert Slider) |
| Touch (Samsung S6SY761) | works (0085-0087) |
| GPU (Adreno 640) | kernel bring-up, RedwoodGPU (G2b), Mesa freedreno (G3), CoreAnimation in backboardd on the GPU drawing straight into IOSurfaces at 60 Hz (G4: 0117-0132, userland/gpu, userland/bbd) |
| File drop (SWEETBOX) | works: raw image in `rawdump` -> files in /private/var/tmp |
| Autorun of /private/var/tmp/autorun.sh | works (0115/0116, userland/autorun) |
| Wall clock | PM8150 RTC + offset file on the root, sweettime= (0133) |
| Battery | PM8150B fuel gauge SOC, voltage, current; charger state (0136, sweetbattery=2) |
| Stock removable apps (Calculator, Weather, Notes, ...) | installed from staged_system_apps (userland/sysapps, sweetcsoff=1 once) |
| Photos, Settings > About, Wallet | work (0135 sandbox extensions; fairplayd job removed) |
| Wallpaper picker, app extensions | work (0137 AMFI trust stand-in + ADT mac-address-*; once `lsreg -r`): all 10 poster providers |
| Wi-Fi | works: WPA2-PSK join, DHCP, internet (wifi25: HTTPS 200 from apple.com); userland/wland from ios-flame over 0138 /dev/pmem; en0 + Apple80211 for wifid/Settings (0139, from ios-flame 0123) |
| Audio, CPU frequency | not started |

## Workflow

- Kernel: GitHub Actions on this fork, `gh workflow run xnu.yml --ref guacamole`.
- Kernel on this Mac (when GitHub's macOS arm64 runners have no capacity:
  "The job was not acquired by Runner of type hosted"): `tools/xnu-local-build.sh --full`
  once (~20 min: blacktop build.sh at the CI commit, KDK 13.5 unpacked under
  ../xnu-local without sudo), then `tools/xnu-local-build.sh` applies new
  xnu/patches and makes incrementally. It uses this Mac's Xcode (27): its iig
  needs IORPC.kernelContent, added only for local builds by
  xnu/local/0001-iorpc-kernelcontent.patch (as in xnu-12377). guac-mkboot.sh
  takes the kernel's path instead of a run id. First local kernel: same
  symbols as CI's plus 0128/0129, 3% smaller (other clang).
- Boot image (macOS or Linux, python3 + gh):
  `TOUCH=1 CPUS=8 tools/guac-mkboot.sh RUN_ID STOCK_boot.img shim-guacamole.Image out.img "boot-args"`.
  `TOUCH/KEYS/USB/CPUS` are environment variables of the script; kernel boot-args
  go in the quoted last argument. The script prints the final `boot-args:` line:
  check it before booting.
- Full iOS boot-args: `rd=ufsroot0 unentitled_ios_sim_launch=1 ignition_level=1
  wdt_test=no_user_checkin ufsrw=1 rootrw=1 launchd_unsecure_cache=1 sweetfb_hz=30
  sweetfb_noconsole=1` (+ `sweetgpu=N ufslogsecs=1` for GPU stages).
- Log: TWRP (`fastboot boot twrp.img`), then `mkdir -p logs; tools/sweet-log-pull.sh logs/x.txt`.
  Never format/wipe Data in TWRP: `userdata` holds the HFS+ iOS root.
- The shim is built with `BOARD=guacamole shim/build.sh`; `tools/mkguacboot.py`
  wraps it with the DTB and header fields of the phone's own stock `boot.img`.
- Root image: `tools/mkiosroot.sh` on Linux with a stock kernel (needs the
  `hfsplus` module), then `sweet-root-tweaks.py`, `img2simg`,
  `fastboot flash userdata ios16-root.simg`.
- GPU firmware (from the phone's `/vendor/firmware`, never committed):
  `research/gpu/fw/{a640_gmu.bin,a630_sqe.fw,a640_zap.mdt,.b00-.b02}`;
  `mkadt.py --guacamole` puts them in ADT `/sweet-gpu`.

## Board facts

- SoC-level addresses are identical to SM7150: GIC 0x17a00000/0x17a60000, UFS
  0x1d84000, SPMI core/obsrvr/cnfg 0xc440000/0xe600000/0xc40a000, RSC drv-2
  0x18220000 (TCS +0xd00), DWC3 0xa600000, GCC 0x100000, cmd-db locator 0xc3f000c.
- DDR: 0x80000000-0xbbb00000, hole, 0xc0000000-0x280000000. Use `--highmem`
  (0x100000000 + 0xfcc00000); the sweet low window crosses the hole.
- Splash framebuffer 0x9c000000, 1440x3120 a8r8g8b8, stride 5760, two VIG pipes
  of 720 px; DSC command-mode panel that keeps refreshing on its own.
  Never write CTL_START (corrupts the picture).
- Reboot to fastboot: PM8150 (SID0) PON_SOFT_RB_SPARE (0x88f) bits 7:1 = 2,
  then PSCI SYSTEM_RESET. ABL ignores IMEM restart_reason; writing PS_HOLD from
  EL1 is XPU-protected (QUALCOMM CrashDump Mode).
- SPMI arbiter v5: read channel obsrvr + 0x80*apid, write channel
  chnls (0xc600000) + 0x10000*apid. PON is APID 0xf9, owned by EE0.
- Keys: power = PON KPDPWR (SID0 0x810 bit0), volume up = PM8150 GPIO6, volume
  down = PM8150 GPIO7 or PON RESIN; GPIOs active low.
- Touch: S6SY761 at I2C 0x48 on QUP wrapper 2 SE0 (0xc80000, DT qupv3_se17),
  GCC vote 0x52014 (bits 0-4), RCG 0x1e148 with DFS (level 0 = XO), S_AHB branch
  0x1e008 is HW-gated (don't poll it), TLMM SOUTH tile 0x3d00000: reset GPIO54,
  IRQ GPIO122, 1.8 V switch GPIO119 (pull-up), SDA/SCL GPIO55/56 (qup17, mux 1),
  vdd = pm8150 L17A (RPMh "ldoa17") at 3.008 V. Coordinates 0..1439, 0..3119.
- Userdata: LUN0, 4 KiB blocks, metadata-encrypted under Android.
  The phone also has `esp`/`win` partitions (Windows on ARM): leave them alone.

## GPU (Adreno 640, chip id 0x06040001, speed bin 0)

Code: `bsd/dev/sweet_gpu.c`, boot-arg `sweetgpu=N` runs stages 1..N 60 s after
boot. Add `ufslogsecs=1` so the 2.5 s checkpoints flush the log before risky
accesses. Register numbers: Linux v6.6 `a6xx.xml.h`, `a6xx_gmu.xml.h`.

| Stage | What | Result |
| --- | --- | --- |
| 1 | read-only state after ABL | all GPU clocks/GDSCs off, speed bin 0 |
| 2 | CX GDSC, GMU 200 MHz, ahb/cxo/cx_gmu, TCM test, SMMU dump | ok |
| 3 | GMU memory, SMMU CB0 + exact SMRs 0x4/0x5/0x404/0x405 | ok |
| 4 | RSC/PDC microcode, TCM blocks, boot regs, release GMU | `0xbabeface` in ~0.6 ms |
| 5 | HFI v2: PERF_TABLE, BW_TABLE, CORE_FW_START, START | all acked |
| 6 | GX_BW_PERF_VOTE + OOB GPU_SET, read RBBM/CP | GX on, CP halted |
| 7 | GPU SMMU CB1, SQE/ring/shadow, hw_init registers, CP halted | ok, all readbacks match |
| 8 | CP_SQE_CNTL=1, CP_ME_INIT + WHERE_AM_I | ok, ring drained in 1 ms |
| 9 | zap via SCM PAS (INIT_IMAGE/MEM_SETUP/AUTH_AND_RESET), CP_SET_SECURE_MODE 0 | ok |
| 10 | IB (CP_INDIRECT_BUFFER) + fence (CP_EVENT_WRITE CACHE_FLUSH_TS\|IRQ) | ok |
| 11 | redwood_gpu_* backend (VA allocator, L3 tables on demand, submit/fence) + self-test | ok; RedwoodGPUStart is a weak stub |
| G3 | Mesa 25.0.7 freedreno via OSMesa (userland/mesa), osmesa-triangle | ok: FD640, GL 4.6 compat; triangle 128x128 in GMEM (1 bin), UBWC -> linear blit; 2 submissions (245 + 68 words); pixels identical to softpipe (4608/11776); context 48 ms, frame 19 ms |
| G2b | RedwoodGPU.cpp user client (0117, topaz 0901 / HackiOS, APSL) on that backend; test userland/gputest | ok (CI 40230428763, with 0118-0120): GEM_NEW iova 0x1110000, SUBMIT fence 2, WAIT_FENCE 109 us, 0x5eed0640 written; second run same iova, fence 3 |

Lessons:
- MMIO to the GPU SMMU's SMR/S2CR is trapped by the hypervisor: use single
  base-only `ldr`/`str` (valid ISS syndrome); a compiler-generated
  post-indexed load in a loop caused CrashDump.
- `gcc_gpu_snoc_dvm_gfx` only reports running after the CX GDSC is on.
- a640_gmu.bin is the block format: 3 ITCM blocks (0x0-0x4000), 2 DTCM blocks
  (0x40000-0x44000), 1 icache block (GMU VA 0x4000), 1 dcache block (VA
  0x44000). icache/dcache live in DDR behind the SMMU, so the SMMU context
  must exist before the GMU leaves reset. Firmware version 0x20000105 ->
  init handshake 0xbabeface. CM3_FW_INIT_RESULT is 0x501c.
- A640 is not "legacy" for Linux: HFI v2, queue 1 type id 1, OOB GPU_SET =
  HOST2GMU bit 30 / ack bit 31.
- cmd-db ARC levels: gfx.lvl 0 16 48 64 128 192 224 256 320 384 416. GPU OPP
  levels (mainline sm8150): 257/345/427/499.2/585 MHz = LOW_SVS/SVS/SVS_L1/
  SVS_L2/NOM; GMU 200 MHz = MIN_SVS (rounds up to 64 in cx.lvl).
- The GMU's HFI memory is cacheable for us: clean after writing, invalidate
  before reading, and only talk while nothing is outstanding.
- SCM: PAS_IS_SUPPORTED(13) answers 0 but INIT_IMAGE works (Linux never asks).
  The zap region must be 1 MiB aligned; kmem_alloc_contig's mask does not
  align, so over-allocate and align by hand. Metadata (the .mdt as shipped) in
  uncached low RAM at 0xf0000000.
- Commands in the ring run privileged, IBs unprivileged: pages an IB reads or
  writes need AP[1]. FSR bit 3 is a permission fault (bit 1 is translation).
- CP_EVENT_WRITE's TIMESTAMP flag stores the CP counter; the payload fence is
  CACHE_FLUSH_TS | IRQ. RBBM_INT_0_STATUS stays 0 while RBBM_INT_0_MASK is 0.
- XNU's pmap_sync_page_attributes_phys() computes `pp << PAGE_SHIFT` in 32
  bits: any page above 4 GiB (all our RAM) whose cache mode changes (a
  write-combined IOBufferMemoryDescriptor) panics in phystokv_range with the
  truncated PA. Apple never hits it (coherent I/O: flush_dcache is a no-op).
  Fixed in 0120 with ptoa().
- Grown L3 tables are separate pages: keep their kernel VA. Client VA from
  0x1100000 shares 2 MB slot 8 with the stage-7 block: reuse its L3.

## Wi-Fi (WCN3990, firmware WLAN.HL.3.2.0.c2)

userland/wland (from ios-flame, README there) does the whole bring-up: modem
PAS boot, GLINK/QRTR, rmtfs/pd-mapper/TFTP, WLFW, then the radio over the copy
engines (WMI-TLV, HTT). What this phone's firmware needed beyond Pixel 4's
(each `guacamole:` in the source):

- MSA region: the whole pil_wlan_fw_region (0x8bc00000 + 0x180000), as
  icnss gives it (qcom,wlan-msa-fixed-region), not 1 MB.
- WLFW CAP has no board_info TLV: board id 0xff (icnss). The board file is
  then picked as the stock cnss-daemon does (OnePlus code, by
  vendor.boot.prj_version / hw_version / rf_version and
  persist.vendor.oem.wifi.chain; this phone 1 / 21 / 4 (Europe), chain 0):
  `bdwlan.b0e`, sent with bdf_type 0. regdb.bin would go first (type 4);
  the modem partition has none.
- The MAC from WMI READY.
- WLFW INI enablefwlog=1 (stock gEnablefwlog=1): only then does the
  firmware send its log, as WMI_DIAG_EVENTID (0x1d011). Records: u32
  0x07000000, u32 timestamp, u8 payload length, u8 0xf8|level, u16 id
  (a source line number; ids repeat across files), payload. 6027 = "WMI
  event sent" (payload: event id).
- **The BSS peer before VDEV_START.** wifi2-wifi24: every VDEV_START came
  back status -1, chain mask 0, at once, no log record. In wlanmdsp.mbn
  (Hexagon; WMI table entry 0x5003 -> handler 0xb0039f88): for an AP/STA
  vdev whose word at +0xc is 0 it goes straight to 0xb0039f18, which sends
  VDEV_START_RESP with status 0xffffffff. qcacld creates the AP's peer
  (PEER_CREATE with the BSSID) before VDEV_START; ath10k after. With the
  peer first (wifi25): status 0, chain mask 3, auth, assoc, 4-way, DHCP.
- The firmware keeps printf strings for some messages (`wlan_vdev.c:...`):
  descriptors of {u32 format or hash, u32 module<<16 | line, u32 flags}.

Settings: 0139 (ios-flame 0123: topaz 0900 + HackiOS) makes en0 and the
Apple80211 interface for wifid over /dev/wlan; wland's bridge.c serves it
(iOS then does DHCP on en0 itself; without /dev/wlan, or boot-arg
sweetwifid=0, wland keeps its own utun + netsvc). 0139 also has AKS
wrap/unwrap (keychain items, wifid's passwords) and drops IOKit's busy
hold for an IOKit daemon that never reports (configd's InterfaceNamer and
wifid waited for it).

Auto-join after boot (0159): wifid's auto-join threshold is the device's
RSSI_THRESHOLD (`__WiFiDeviceManagerGetAutoJoinThreshold`), which IO80211
reads with Apple80211 get 106 (RSSI bounds: version, min, max, threshold as
int32). Our ioctl did not answer it, the threshold stayed 0 dBm and every
network was "Below minimum RSSI threshold"; a join from Settings skips that
check, so only manual joins worked. 0159 answers -100 / 0 / -80 (CoreWiFi's
own auto-join minRSSI is -80). wland also restarts wifid once if the radio
scans and nothing joined for 20 s (WLAND_NO_WIFID_KICK turns it off).

## Alert Slider

0158 (bsd/dev/sweet_keys.c, keys thread, every 20 ms): TLMM gpio125 (top),
gpio134 (middle), gpio27 (bottom), active low, one pin low per position.
The bootloader leaves them pulled down (cfg 0x1, all read 0); the kernel
writes GPIO_CFG = 0 once (no pull, Android's tri_state_key_active). Top and
middle (OnePlus silent / vibrate) are silent, bottom rings; sweetslider=0
off, 2 inverted. `userland/jb/slider` reads the pins over /dev/pmem.

What iOS needs, from the disassembly: backboardd's BKButtonHIDEventProcessor
sends only Telephony page 0xB usage **0x2E** to _processRingerSwitchEvent
(0x2D is "Unknown page"), ringerState = the key's down value (0 "silent",
1 "tone1"), so SweetHID sends down while at ring. SpringBoard's
_ringerChanged: then does nothing unless SBUIHasHIDRingerSwitch(), which
is MGGetBoolAnswer("ringer-switch"): false on this phone. Fixed in the
MobileGestalt cache: `mgtool setbool ringer-switch true` (once; a rebuilt
cache needs it again), then respring.

## Metal (metal2vulkan + Turnip, sweetmtl=2)

From ios-flame's SweetMetal2: `userland/metal2` (SweetMetal2.bundle: UniXNU's
frontend + the metal2vulkan bridge, MIT) over Turnip (`userland/turnip`, Mesa
85fda3b3 + UniXNU's darwin backend) over the kernel's NativeAdreno user client
(0144). Host builds: `tools/build-m2v-bridge.sh`, `tools/build-turnip-ios.sh`
(sources fetched into ../metal-work, never stored here), then
`userland/metal2/build.sh`; install the bundle into /System/Library/Extensions.

- Who gets a device: `/var/local/mtl2-clients` (names, `*`, `-NAME`). Now
  `backboardd com.apple.WebKit.GPU com.apple.WebKit.WebContent Telegram` and
  the tests. backboardd on Metal needs the global prefs CAEnableDeepFramebuffer,
  CAEnableFramebufferCompression, CAEnableIOSurfaceCompression = NO
  (`userland/jb/prefs.c`); then CoreAnimation draws every filter itself
  (methaneos.dev's blur(130px) and backdrop-filter were streaks on eagl.c).
- Safari: WebKit2GPUProcessForDOMRendering = NO (pages drawn in WebContent,
  as ios-flame); no `sweetsurf10=3` (10-bit tiles stay one-plane BGRA; the
  bridge maps the XR formats 550-555 to BGRA8 in textures and pipelines).
- Kernel: 0144 user client + GPU pool + client VA windows (and the L2-per-GB
  fix: VA above 1 GB aliased), 0145 a swap is shown only after the GPU's
  last submit (half-drawn frames) and surfaces are always cache-flushed,
  0146 small buffers may use the compositor's reserve (Turnip writes through
  NULL when a command stream BO is refused), 0147 GPU 585 MHz
  (`sweetgpufreq`), DDR/SH0 BCM votes (`sweetddr`), pool +2 GB above XNU's
  window (`sweetgpuhigh`; 0x2_0000_0000-0x2_8000_0000 is RAM per Android's
  iomem), vsync period in the notification; 0148 64 GB of GPU VA (backboardd
  ran out of 512 MB importing every app's layers: Telegram stayed black).
- Turnip patch 0101: no dedicated allocation for host imports (no shadow copy
  per IOSurface texture, half the pool) and a refused command stream fails
  vkBeginCommandBuffer instead of crashing.
- Tests (`userland/turnip/out`, `userland/metal2/test`): v2test (kernel ABI),
  vktest (Turnip), m2vtest (bridge), mtltest/surftest/cgtest (Metal.framework,
  IOSurface textures, CGIOSurfaceContext against the CPU).
- Decrypted IPAs: re-sign every framework/dylib ad hoc (their decrypted pages
  no longer match Apple's page hashes, the kernel drops CS_VALID, and then
  entitlements, app group containers and keychain all fail), and the main
  binary with the IPA's original entitlements (Telegram 10.2.6 crashes in
  CloudKit without icloud-services, stays black without its app group).
- Frame rate (swipes, `SWEET FB: 5s:` lines of 0151/0153/0155): 25 -> 37-44
  fps. 0149 presents on the GPU's 2D engine (CP_BLIT scale, 18 us of CPU
  instead of 21 ms), 0150 at the swap (the GPU orders the blit behind the
  frame), bridge answers cached (supportsFamily: was 29% of backboardd's render
  thread) and surfaces cleaned only when their seed moved, bridge built with
  LTO and cortex-a76; 0156 puts perf threads on idle A76s (the SMP scheduler
  took the lowest idle CPU, an A55) and with `sweetcpufreq=2` asks the A76 OSM
  domains for their top row (the kernel never set CPU clocks). iOS 16's window
  server never asks for vsync notifications (0 sent, selector 72 never called):
  it clocks itself by IOMFBDisplayRefresh (0154, as hakios). vmd (visual
  voicemail) spun a whole core without CoreTelephony: `launchctl disable`
  system/ and user/501/com.apple.voicemail.vmd.
- Profiling: `userland/jb/tsample` (task_for_pid, frame-pointer samples of the
  busiest thread; symbolicate the bridge with its dSYM) and `exports` (a shared
  cache library's exports and code, read in place).
- Open: GPU now 55-82% busy in swipes (585 MHz); next: render at @2x (hakios:
  750x1624, 107-120 fps) and UniXNU/sheng's metal2vulkan per-draw patches.
  Chat videos hang: mediaserverd waits for an audio device at start
  (vaemGetVirtualAudioDeviceIDs). web.telegram.org restarts itself when its
  shared worker dies (not a GPU error).

## Files app

On My iPhone is FileProvider's LocalStorage domain on FPFS, which iOS 16
builds for APFS; our root is HFS. Four walls, each seen in the unified log:

1. fileproviderd keeps its FPFS library only on an eligible volume:
   -[FPDVolume isEligibleForFPFS:] (FileProviderDaemon) takes a statfs and
   wants f_fstypename "apfs" ("Non-eligible device /dev/ufsroot0: non-apfs"),
   so every domain stayed "url:(null) state:disabled" and Files listed
   nothing. userland/jb/fpfs.m (/var/local/lib/libsweet-fpfs.dylib) also
   accepts a local writable HFS volume; userland/jb/fpwrap.c
   (/var/local/bin/fpwrap) puts it in DYLD_INSERT_LIBRARIES and execs
   fileproviderd. The com.apple.FileProvider job's Program in
   /System/Library/xpc/launchd.plist is /var/local/bin/fpwrap (backup
   launchd.plist.pre-fpwrap; /var/local/fpwrap.off turns the library off).
2. The LocalStorage extension aborted: PlugInKit asks
   sandbox_container_path_for_pid (Sandbox op 4) and throws on failure
   ("plugin has invalid sandbox or container: error 2"). launchd spawns it
   with Sandbox spawn attributes, profile "plugin", empty container; pkd
   puts the data container in the launch overlay as HOME. 0160:
   sweet_sandbox_exec (called from exec_mach_imgact) keeps the container
   per pid (spawn attributes, else HOME under /private/var/mobile/
   Containers/), op 4 answers it.
3. FPFS marks a domain's storage as a sync root with APFS fsctls
   ("can't obtain VFSItem ... [25: Inappropriate ioctl]"). 0161 emulates
   _IOW/_IOR('J', 72) (set/get, xattr com.apple.sweet.syncroot) and
   _IOR('J', 74) (sync root id above an item) for HFS. The storage made
   before 0161 needed the mark once by hand (`xattr -w
   com.apple.sweet.syncroot 1 ".../File Provider Storage"`).
4. Directories could not be found by file id (a new folder vanished):
   fpfs_fgethandle asks fpfs_pkg_dirstat, _IOWR('J', 32, 40 bytes), and
   accepts only ENOTSUP as "not supported". 0161 answers ENOTSUP for that
   one fsctl on HFS. Not for every 'J' fsctl: launchd queries the root with
   one and panics the boot on ENOTSUP ("failed to query root device
   status: 45"); it wants ENOTTY.

0161 also logs, once per command, file system ioctls that end in ENOTTY or
ENOTSUP from the FileProvider processes ("SWEET IOCTL: ...").

## Audio (recon, nothing built yet)

From the Android dumps (live.dtb, dmesg, i2c.txt, vendor_b, modem_b):

- Speakers: two NXP **TFA9874** smart amps (driver tfa98xx v6.5.3) on QUP
  SE4 I2C (`i2c@890000`, Android i2c-0), 0x34 right (reset GPIO 37) and
  0x35 left (reset GPIO 100). fsa4480 (USB-C audio switch) on the same bus
  at 0x42. Haptics: aw8697 on i2c-2 at 0x5a.
- Card `sm8150-tavil-snd-card` (WCD9340 on SLIMbus for headset/mics). The
  speakers are fed by **QUAT_MI2S_RX** (AFE port 0x1006), 2 channels,
  S24_LE (mixer_paths_tavil.xml "deep-buffer-playback quat_i2s",
  audio_platform_info: speaker backend QUAT_MI2S_RX, ACDB id 15);
  VI feedback off (QUAT_MI2S_RX_VI_FB_MUX = ZERO). Pins gpio137-142
  (qua_mi2s). OnePlus commented out the WSA speaker path.
- ADSP: APR over the GLINK channel `apr_audio_svc` on the lpass edge; audio
  buffers through apps SMMU SID 0x1b21 (mask 0xf, msm-audio-ion). Firmware
  modem_b `image/adsp.mdt` + b00-b14 (13 MB); `adspr.jsn` (root_pd) and
  `adspua.jsn` (msm/adsp/audio_pd provides avs/audio) for pd-mapper.
  Android also runs `adsprpcd audiopd` (fastrpc audio PD).
- Precedent: the sm8150-mainline Linux tree (map220v/sm8150-mainline) has
  APR q6core/q6afe/q6asm/q6adm on sm8150 without a protection-domain
  property, and Xiaomi Pad 5 (nabu, SM8150) plays through
  QUATERNARY_TDM_RX_0 to four CS35L41: same shape as ours.
- iOS side: UniXNU-sheng (BSD-2-Clause-Patent) has a working
  AudioServerPlugIn + kernel ring service (AudioReach DSP, SM8550); the DSP
  half does not apply to our APR ADSP. ios-port-marble (SM7475) and
  ios-sweet-grus (SM7150, APR, stopped at the audio PD) have no sound.
- Extracted files (not in git): ../dumps/vendor-audio/ (mixer paths,
  platform info, tfa98xx.cnt, acdbdata, adsp/).

Bring-up from userland (userland/adsp, over /dev/pmem like wland):

- `adspd` (firmware in /var/local/fw/adsp, lower-case names): PAS id 1,
  region 0x8be00000 + 0x1e00000, cx vote >= 385, AOP load_state adsp,
  SMP2P items 443 in / 429 out (IPC bit 10), GLINK on SMEM host 2 (IPC
  bit 8). Works: ready after 100 ms; the ADSP opens IPCRTR, apr_audio_svc,
  fastrpcglink-apps-dsp, apr_apps2, glink_ssr, LOOPBACK_CTL_LPASS; QRTR node
  5 asks our pd-mapper for "avs/audio" (adspr/adspua.jsn table); APR q6core:
  state 1 (ready), framework 2.9, services core 3, AFE 4 (0x260000), ASM 7,
  ADM 8, 9-0xd. No audio PD needed for these.
- `tfa`: GENI I2C on QUP wrapper 0 SE4 (GPIO51/52 mux 1; GCC vote 0x5200c
  bits 6/7/14, RCG 0x17608 DFS level 0 = XO), the touch driver's FIFO engine.
  Both amps answer: REV 0x0c74 (TFA9874, NXP "V1.14" settings), SYS_CTRL0
  0x0001 (powered down). `tfa init` = NXP tfa_dev_start for a part without
  CoolFlux (no CFE/SBSL/ACS fields): I2CR, key1/key2 unlock, the 0x0c74
  register set, the container's device items (56 with the "speaker"
  profile), MANSCONF=1, PWDN=0 -> MANSTATE 4, waiting for the I2S clock.
- AFE plan: QUAT_MI2S_RX (0x1006) on SD1 (rx-lines 2, GPIO140; SCK 137,
  WS 138), 48 kHz stereo 32-bit slots, Q6AFE clock QUAD_MI2S_IBIT (0x106)
  3.072 MHz via AFE_SVC_CMD_SET_PARAM (0x100f3) / AFE_MODULE_CLOCK_SET,
  AFE_PARAM_ID_I2S_CONFIG, DEVICE_START.
- A silent ADSP restart kills Wi-Fi: after a plain PAS shutdown and a new
  boot of the ADSP the modem went ERR-FATAL (wland.out) and the phone fell
  off the network (the kernel kept running). Linux tells the other
  processors over glink_ssr; we do not, so the ADSP boots once per phone
  boot. `adspd serve` keeps it (commands through the FIFO
  /var/local/adspd.cmd: afe on/off, status, pd PATH, open NAME RCID,
  aprch, apr SVC OPCODE WORDS, quit, stop); `quit` saves the GLINK/QRTR
  state (/var/local/adspd.state, .qrtr) so a new adspd adopts the running
  edge instead of restarting the ADSP. `stop` is graceful (force-stop,
  stop-ack, PAS shutdown).
- The audio services do not answer: AFE (clock set, even DEVICE_STOP), ASM
  and ADM never reply on apr_audio_svc (q6core does), though PDR (servreg
  notifier, QRTR 0x42 at 5:3) reports msm/adsp/audio_pd UP. apr_apps2
  (the ADSP's other APR channel, unused by Android's apr_tal_rpmsg) gives no
  intents. Android runs `adsprpcd audiopd`: FastRPC INIT_CREATE_STATIC of
  the audio PD with its remote heap; ios-sweet-grus stopped there too.
- FastRPC over fastrpcglink-apps-dsp crashes the SoC: any invoke (the
  audio PD create and even a plain attach) sends the phone to crashdump,
  with nothing in the logs. adspd keeps the code but it must not be used.
- WoA (Project-Aloha/windows_oem_oneplus_qc8150, WOA-Project
  windows_silicon_qcom_hana) also runs the audio PD over FastRPC: SIDs
  0x1b23/24/25 (FastRPCADSPSidInfo) translated through context banks
  0x1b/0x1c/0x1f, AridBase 17 03 00 10/11/12, and ARPC depends on SCM0
  (most likely a hyp_assign of the remote heap). Our SMMU setup bypasses
  (identity), and we assign nothing: the likely reason for the crash.
  Audio waits on that.

## Bluetooth (userland/bt + xnu 0162: controller up, iOS stack running)

- Chip: the WCN3990-family BT core ("cherokee", persist.vendor.qcom.
  bluetooth.soc), EDL version: product 0xa, patch 0x6699, ROM 0x1001,
  SoC 0x40010224 = Cherokee 2.1.1. The QTI HAL picks files by SoC id:
  linux-firmware `qca/crbtfw21.tlv` + `qca/crnv21.bin` (in
  /var/local/fw/bt, not in git). After them: HCI/LMP 9 (5.0),
  manufacturer 0x1d, subversion 0x2be.
- UART: QUP wrapper 2 SE3 = qupv3_se13 at 0xc8c000 (UART fw 0x20b,
  oversampling 32), CTS gpio43, RTS 44 (pull-down), TX 45 (pull-up), RX 46,
  mux 1, east tile. GCC vote 0x52014 bit 7, CBCR 0x1e4d4, RCG 0x1e4d8 from
  GPLL0 main: 384/31250 = 7.3728 MHz (115200 = div 2, 2400 = div 96).
  Rails are WLAN's (pm8150 l7, pm8150l l2/l11), already on through wland.
- `SE_IRQ_EN` must stay 0: all ones (as Linux) routes the SE interrupt to
  the GIC, where XNU has no handler, and froze the whole SoC.
- A stuck RX command survives between processes and swallows everything
  (rx fifo status 0x30000000, s_irq 0x2 overrun): cancel is not enough,
  abort it (qcom_geni_serial stop_rx). Before that fix the controller
  seemed to ignore the power pulses every other run.
- Power: 0xC0 at 2400 baud without flow control (off), 0xFC at 115200 (on),
  then RTS/CTS on. btd waits 2 s / 1 s (Linux 10 ms / 100 ms) to be safe.
- Firmware (btqca.c): the whole file in 243-byte EDL_PATCH_TLV_REQ (0x1e)
  segments of vendor command 0xfc00. The patch is download mode 3: no answer
  per segment, the last one ends with a Command Complete for opcode 0 and
  then the vendor event 0x04. The NVM is answered per segment. The NVM's
  HCI tag (17) is set to 115200 (data[2] = 0) and in-band sleep off
  (data[0] &= ~0x80), so no IBS wake bytes are needed.
- The controller sends its own log as ACL on handle 0x2edc (hci_qca
  QCA_DEBUG_HANDLE): the H4 reader skips ACL/SCO packets.
- `btd scan 15`: 301 LE advertising reports from 14 devices and 2 BR/EDR
  inquiry results. The BD address is the NVM placeholder 39:90:21:64:07:00;
  the phone's own address (Android keeps it in persist) is not set yet.
- iOS stack (bluetoothd), what it needs and what we give it:
  - Address: only from IOService `bluetooth` (or `marconi-bt`) ->
    IODeviceTree `local-mac-address`. `BT_DEVICE_ADDRESS` is parsed and then
    zeroed in the release build (and, when set, skips the device tree), so
    without the node: "No bluetooth on this device", exit 0, every 10 s.
    mkadt.py adds `/arm-io/bluetooth` with 02:53:57:45:45:02
    (mac-address-bluetooth0); btd writes the same address into the NVM.
  - Transport: `BT_HCI_TRANSPORT=TRANSPORT:DRIVER:ARG` (transports USB H4DS
    H4BC H4 H5 APPLEBT VIRT PCIE PCIESW, drivers USB POSIX APPLEBT VIRT PCIE
    PCIESW). The POSIX driver is not a tty: socket(PF_SYSTEM, SOCK_STREAM,
    SYSPROTO_CONTROL), CTLIOCGINFO by name (default Apple's Skywalk
    com.apple.uart.sk.bluetooth), getsockopt(TIOCGETA) / setsockopt(TIOCSETA)
    with a 72-byte termios, then the H4 stream. xnu 0162
    (bsd/dev/sweet_btctl.c, boot-arg `sweetbt=1`) registers
    `com.sweet.bt`, a relay: the client that sets option 0x5357 is the
    controller (btd serve), the others are hosts. bluetoothd gets
    `H4:POSIX:com.sweet.bt`.
  - Chip boot: bluetoothd asks the XPC service com.apple.BlueTool to
    RunBuiltinScript "boot" then "init" (kMsgArgs: script, btaddress).
    BlueTool knows Broadcom/Apple chips only (platform = hex ProductHash,
    override /etc/bluetool/<hash>.<seq>.script never found; then it waits
    for an AppleOLYHAL Wi-Fi vendor and times out). userland/bt/bluetoold
    answers every request with result 0; the com.apple.BlueTool job runs it.
  - HCI init (btd serve rewrites on the way):
    - Read Local Supported Commands: bluetoothd only sends listed commands;
      Write Default Erroneous Data Reporting (0x0c5b, octet 18 bit 3) is
      listed and its "unknown command" answered as done.
    - Vendor commands (OGF 0x3f) are Broadcom's (it takes the chip for
      BCM_4357 from the model) and never go to the Qualcomm firmware, whose
      own vendor range overlaps (0xfc00 = download). btd answers them:
      Unknown HCI Command plus 2 bytes, since replies parsed by
      _BCM_ByteStreamEventParser need >= 2 bytes or the command never
      completes (15 s, "timedout waiting for controller", restart). The
      "Olympic" multiplexer 0xfce2 (sub-command first: 0x15 ANTENNA_PREF_B,
      0x0d power statistics, ...) gets success, the sub-command echoed and
      34 zero bytes (0x0d asserts on a short reply, bcm_vsc.c:139).
    - LE Read Transmit Power (0x204a) is answered here as unknown: after it
      the controller refuses the legacy LE commands bluetoothd then uses
      (Set Scan Parameters, Read Advertising Channel Tx Power: 0x0c).
    - LE Set Scan / Advertising Parameters with a random own address go out
      as public: the random address is set with a Broadcom command.
  - Restarting btd under a running bluetoothd leaves the controller
    unconfigured (bluetoothd does not notice): restart bluetoothd after it.
- Audio (A2DP), with OnePlus Buds 4: pairing, ACL, AVRCP, AVDTP discover /
  get capabilities / set configuration (AAC) and open work. Right after the
  stream channel opens bluetoothd registers a Skywalk user pipe nexus (the
  audio path to mediaserverd's BTAudioHALPlugin): our MAC policy denied
  PRIV_SKYWALK_REGISTER_USER_PIPE (EPERM for non-root; root works),
  "Failed to alloc a nexus provider", SIGSEGV in os_channel_get_fd. xnu 0163
  grants the Skywalk privileges to processes whose main binary has the
  matching com.apple.private.skywalk.* entitlement, as iOS AMFI does.
- VirtualAudio.plugin is back in /Library/Audio/Plug-Ins/HAL (it had been
  moved to HAL.disabled for a mediaserverd crash with no audio device at
  all); mediaserverd stays up with it. BTAudioHALPlugin builds the
  "Unified Device" for the buds (ports -tacl A2DP, -tsco HFP), but
  VirtualAudio routes every category (RingtonePreview, MediaPlayback) to an
  aggregate over Null_Device: "Connection between ports Application
  Processor and <addr>-tacl not allowed due to property inclusion policy
  [dst: Require out of band speech input support]; [dst: Require wireless
  splitter not in use]", for the ringtone "Route change failed: nort". No
  AVDTP_Start follows. Next: where VirtualAudio gets that policy and the
  port properties. Video in Safari crashes mediaserverd separately
  (exception in FigGetAlignmentForIOSurfaceOutputToDisplayService).
- Result: bluetoothd stays up (STACK_READY), LE scanning delivers
  advertisements. Jobs (sweet-root-tweaks.py --net): com.sweet.btd,
  com.apple.BlueTool -> bluetoold, bluetoothd env. /var/local/btd.off
  keeps btd from starting; /var/local/btd.trace logs every command.

## SWEETBOX file drop

`tools/sweet-mailbox.py pack out.img NAME SRC [NAME SRC ...]`, then in TWRP
`dd if=out.img of=/dev/block/by-name/rawdump bs=65536`; boot with
`sweetmailbox=1 sweetboxpart=rawdump`. The kernel extracts flat into
/private/var/tmp/ (no mkdir KPI) ~30 s after boot and stamps SWEETDNE.
sweet_ufs only talks to UFS LUN 0 (sda): rawdump/op2/userdata live there,
logdump does not. Reads/stamps take sweet_ufs_lock.

### Autorun

launchd here takes every system job from the cache
`/System/Library/xpc/launchd.plist` (launchd_cache_loader, unsigned because
of `launchd_unsecure_cache=1`; launchd's own strings: "system services must
be in cache"). Its bootstrap paths are all on the root volume, none under
/private/var, and the system has no `/bin/sh`. So autorun needs one change to
the root: a job in that cache, plus the program it runs. Both go in through
SWEETBOX instead of reflashing userdata:

- 0115: entries named `ROOT/<path>` are written to `/<path>` right after the
  root mounts (bsd_init, before launchd), with boot-arg `sweetboxroot=1`.
  A broken launchd.plist can be replaced the same way on the next boot.
- 0116: the kernel writes `/private/var/tmp/SWEETBOX.done` after extracting;
  `sweet-autorun` is in the default `sweetnull` list (its stdout -> log).
- 0118: the root pass empties SWEETBOX.done before launchd (whenever
  `sweetmailbox=1`), and sweet-autorun waits for it to be non-empty. There
  is no RTC: the clock starts at 0 every boot, so an mtime check took the
  last boot's marker for this boot's (GPU test run: autorun.sh not found,
  the box was extracted only afterwards).
- 0119: 0118 still did not empty it: the kernel's vnode_open() ignores
  O_TRUNC (vn_open_auth leaves it to the open syscall), so every box file
  written over a longer one kept the old tail. box_write_file truncates
  with vnode_setattr(va_data_size = 0), as kern_core.c does.
- `tools/sweet-root-tweaks.py` adds job `com.sweet.autorun`
  (`/usr/libexec/sweet-autorun`, RunAtLoad). `userland/autorun/sweet-autorun.c`
  waits for SWEETBOX.done of this boot (non-empty), renames autorun.sh to autorun.sh.ran
  and runs it line by line (no shell: words, quotes, `&`, builtins cd, sleep,
  export, echo, wait, exit). Output lines appear in the log as `SWEET RUN:`.
- The phone's launchd.plist is the IPSW one + the sweet-root-tweaks.py edits
  (checked byte for byte in ios16-root.img, offset 0x1e025f000), so it can be
  regenerated from the IPSW; keep `launchd.tweaked.plist` (without the job)
  as the rescue box.

One-time install:
```
userland/build.sh
python3 tools/sweet-root-tweaks.py --launchd-plist IPSW_OS/System/Library/xpc/launchd.plist launchd.plist
python3 tools/sweet-mailbox.py pack box.img \
  ROOT/System/Library/xpc/launchd.plist launchd.plist \
  ROOT/usr/libexec/sweet-autorun userland/out/sweet-autorun \
  autorun.sh autorun.sh
```
Boot with `sweetmailbox=1 sweetboxpart=rawdump sweetboxroot=1`. Later boxes
only carry autorun.sh and its files (no `sweetboxroot`).

First phone run (CI 40200983409): both ROOT/ files installed before
`exec launchd`, launchd took the new cache, sweet-autorun waited for
SWEETBOX.done and relayed `vm_stat` (`SWEET RUN: [vm_stat] ...`). To update
only the helper: a box with `ROOT/usr/libexec/sweet-autorun` and
`sweetboxroot=1` again.

## Next

1. Done: autorun works on the phone (CI 40200983409, see Autorun).
2. Done. G2b: 0117 adds iokit/Kernel/arm/RedwoodGPU.cpp from topaz 0901 (the msm
   DRM interface as a user client; HackiOS origin, APSL 2.0, attribution kept
   in its header). Changes: buffers from 36-bit RAM, not contiguous (this
   kernel's RAM is all above 4 GiB; topaz asked for < 4 GiB); SweetSurface
   paths (surfaceBuffer, seed/drawn in submit, topaz_surface_drawn) off with
   REDWOOD_SURFACES 0, needed for G4 (our SweetSurface has no `drawn`).
   `userland/gputest/redwood-gpu-test.c`: GET_PARAM, GEM_NEW, map, IB with
   CP_MEM_WRITE of 0x5eed0640, SUBMIT, WAIT_FENCE, prints "result: ... OK".
   Run it from autorun.sh with sweetgpu=11. Works on the phone (see table);
   the service appears ~70 s after the test starts (GPU stages begin 60 s
   after the kernel), the test waits for it.
3. Done. G3: Mesa freedreno for Darwin, offscreen triangle. `userland/mesa/`:
   topaz's Mesa patches and redwood-drm (HackiOS, MIT, README.md), our
   `mk-mesa.sh` (Mesa 25.0.7, freedreno + softpipe, OSMesa; needs Homebrew
   bison 3 for the ir3 parser). Mesa knows A640 as GPUId(640) (chip id
   0x060400ff matches 0x06040001); the msm version 1.7 we report selects the
   softpin path (no relocs), which RedwoodGPU requires.
   `userland/gputest/osmesa-triangle.c`: red triangle on blue, 128x128,
   checks pixels, prints the picture as text; softpipe first
   (OSMESA_SOFTWARE=1), then freedreno. Done: works on the phone (table).
   libOSMesa's install name is /private/var/tmp/libOSMesa.8.dylib: dyld
   refused `@executable_path` ("security policy does not allow @ path
   expansion"); our built-in AMFI does not answer dyld's policy query
   (topaz 0902 does). Fixed by 0121, see below.
4. G4 prerequisite done: xnu 0121, our MAC policy is named "AMFI" and
   answers dyld's amfi_check_dyld_policy_self (__sandbox_ms("AMFI", 90,
   {in, *out}), checked in the IPSW's dyld) with all AMFI_DYLD_OUTPUT_*
   flags but a custom shared cache. Phone (CI 40236282664): Mesa loads via
   @executable_path, DYLD_INSERT_LIBRARIES runs our constructor in vm_stat
   ("SWEET INSERT"), DYLD_PRINT_LIBRARIES lists the images. Other AMFI ops
   return ENOSYS (topaz answers 0: ExtensionKit/PosterBoard need that).
   G4 step 1 works (phone: "|backboardd| SWEET BBD: library loaded into pid
   57 (uid 501)", lock screen and touch as before): com.apple.backboardd runs
   /usr/libexec/sweet-bbd (userland/bbd), which puts
   /usr/lib/libsweet-bbd.dylib into DYLD_INSERT_LIBRARIES and execs the
   real backboardd (same pid and Mach services). Boot-arg sweetbbd=0 leaves
   the library out. Installed with a ROOT/ box; rescue box = the previous
   launchd.plist.
   G4 step 2: eagl_slots.txt (HackiOS, read off iOS 16.5 OpenGLES) checked
   against our 16.7.16 OpenGLES from the IPSW's dyld cache
   (tools/check-eagl-slots.py, dsc_extractor.bundle from Xcode): all 457
   names at the same offsets. The functions jump through [ctx + slot + 0x20],
   ctx = [TPIDRRO_EL0 + 0xf0].
   G4 step 3 (written, not yet run): userland/gpu (eagl.c, gl_osmesa.c,
   mkeagl.py; README.md for origin and MIT notices) -> /usr/lib/libsweet-gl
   .dylib on /usr/lib/libOSMesa.8.dylib (@rpath). libsweet-bbd waits in its
   constructor (before backboardd's main) for RedwoodGPU, up to sweetglwait
   (120 s), then dlopens it; sweetgl=0 keeps the software renderer.
   First run (gl1): no GL, because stage 10 failed for the first time (IB
   ran, fence read as 0 for 500 ms) and RedwoodGPU was not published; boot
   stalled 180 s in backboardd. Cause: the zeroed fence/scratch were never
   flushed before the submit and the poll did not invalidate the fence page
   (cached stale 0); the backend start had the same order bug. 0122 fixes
   both.
   Second run (gl2, 0122): stage 10 ok, RedwoodGPU after 94 s, libsweet-gl
   loaded, CoreAnimation made an OpenGL ES 3.2 context on FD640 (7/7
   EAGLContext methods ours, first shaders compiled). Then: the screen
   surface is 'b3a8' (2 planes, 10-bit wide colour), which eagl.c cannot
   attach ("not done", glDrawElements 0x506), and backboardd crashed 108
   times (SIGSEGV at 0x30): render_contents_background -> wrap_buffer ->
   iosurface_get_edr_factor -> IOSurfaceClientGetBulkAttachments reads the
   state page at client+0x70 = 0. CoreAnimation had wrapped an image with
   IOSurfaceWrapClientImage (IOSurfaceRoot selector 6), our default case
   answered success with a zeroed lock result. 0123 (topaz 0131) refuses
   such calls.
   Third run (gl3, 0123): no more crashes, CoreAnimation renders frames
   through Mesa (136 GL calls, 4 draws in frame 1) but "none into an
   IOSurface that is known here": the screen surface is 'b3a8', which
   eagl.c cannot attach, so the screen stayed black. 0124: SweetSurface
   makes 'w30r', 'l10r', 'w30R' and 'b3a8' one-plane BGRA (from topaz 0900 /
   HackiOS); sweetsurf10=1 keeps them. The display path already copies
   plane 0 as 4-byte BGRA.
   Fourth run (gl4, 0124): first UI drawn by the Adreno 640 (lock screen,
   home screen, touch), no crashes. Three faults: (1) frames take 166-195
   ms, ~155-160 of them copying the 11 MB frame out of the GPU's texture
   into the screen surface ("a copy"), plus a 10.8 MB texture refilled from
   an IOSurface each frame -> zero-copy (REDWOOD_SURFACES) next; (2) app
   icons missing: CoreAnimation wraps images with IOSurfaceWrapClientImage,
   refused since 0123 -> 0125 implements selectors 6/7 (topaz 0132);
   (3) red lock screen: the vibrantColorMatrix filter's numbers never
   reach eagl.c -> libsweet-bbd hooks GLESContext function_supported /
   function_uniform (vtable 0x1d5b2f6e8 +0x10 +0x378/+0x388, checked in our
   cache; scheme from HackiOS inject.c); sweetglfn=0 turns that off.
   gl5-gl9: red veil gone (hooks on); icons still missing. Traced: each icon
   is drawn from a 180x180 surface with no pixel format that backboardd
   makes for a CoreAnimation deferred image slot
   (DeferredImageSlotIOSurface::realize_buffer); GLESContext::
   attach_surface_texture gives it to -texImageIOSurface: all zero. Only
   backboardd touches it (sweetsurflog, 0126): create, lock for writing,
   unlock (seed 1), then eagl.c reads zeros. 0127 counts the bytes set at
   each event to tell "nothing was written" from "eagl.c reads elsewhere".
   gl10: nothing written (0 bytes after the write lock); without GL no such
   surfaces exist. gl12 (0128, user stack at the lock trap): the lock and
   unlock come from CA::Render::Image::retain_iosurface(). For an image whose
   CA format has an IOSurface pixel format it calls IOSurfaceWrapClientImage
   (selector 6, 0125); for one without (the icons) it calls IOSurfaceCreate
   with "IOSurfaceAddress" = the image's data in backboardd, then only locks
   and unlocks. SweetSurface ignored that key and made fresh zeroed memory.
   0129 wraps the caller's memory at IOSurfaceAddress instead.
   gl13 (first local kernel): "not wired": the image data is mapped
   read-only in backboardd. 0130 wires such ranges for reading only and maps
   the surface read-only. gl14: app icons are back.
   Lags: 0131 turns on REDWOOD_SURFACES (IOSurface memory as GPU buffers,
   eagl.c's "drawn in place" instead of a CPU copy of every frame), adds
   SweetSurface::drawn, and the display invalidates its cache of a surface
   the GPU drew before showing it.
   gl15 (0131): frames "1 in place, 0 copied", visibly smoother; ~22 frames
   a second with sweetfb_hz=30 (the panel is refreshed every 2nd vsync, by
   a CPU nearest-neighbour scale 1125x2436 -> 1440x3120). Settings' small
   icons sliced: surfaceBuffer() ignored wrapDelta (0132 fixes). Screen-off
   went frame by frame: the fade read back write-combined panel memory
   (0132: one black fill). 0132 also logs the cost of showing a frame
   every 300 frames, to decide between sweetfb_hz=60 and moving the
   scaling to the display hardware.
   G4: EAGL shim + backboardd hook (topaz, iOS 16), surfaces, presentation.
   Later: Metal via ry-diffusion/Chlorophyll (MIT; A630 = closest sibling).
   Bergamota has no licence file: use its notes, ask before copying code.

References used for structure (no code copied): Linux msm a6xx driver,
mxtpasontop/ios-sweet `gpu` branch (A618 on SM7150), Bergamota
`docs/GPU_A630_BRINGUP.md`, MakrSas/ios-topaz (IOAcceleratorES, GPU userland).
