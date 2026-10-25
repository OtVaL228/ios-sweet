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
| Display 1125x2436 presented, scaled x1.28 to 1440x3120 | works (0083) |
| Buttons (power, volume) | works (0084) |
| Touch (Samsung S6SY761) | works (0085-0087) |
| GPU (Adreno 640) | kernel bring-up done, IB + fence path works, redwood_gpu_* backend up; userland next |
| File drop (SWEETBOX) | works: raw image in `rawdump` -> files in /private/var/tmp |
| Wi-Fi, audio, RTC, CPU frequency | not started |

## Workflow

- Kernel: GitHub Actions on this fork, `gh workflow run xnu.yml --ref guacamole`.
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
- Grown L3 tables are separate pages: keep their kernel VA. Client VA from
  0x1100000 shares 2 MB slot 8 with the stage-7 block: reuse its L3.

## SWEETBOX file drop

`tools/sweet-mailbox.py pack out.img NAME SRC [NAME SRC ...]`, then in TWRP
`dd if=out.img of=/dev/block/by-name/rawdump bs=65536`; boot with
`sweetmailbox=1 sweetboxpart=rawdump`. The kernel extracts flat into
/private/var/tmp/ (no mkdir KPI) ~30 s after boot and stamps SWEETDNE.
sweet_ufs only talks to UFS LUN 0 (sda): rawdump/op2/userdata live there,
logdump does not. Reads/stamps take sweet_ufs_lock.

## Next

1. A launchd job in the root image that runs /private/var/tmp/autorun.sh
   (so binaries from SWEETBOX start by themselves).
2. G2b: port RedwoodGPU.cpp (topaz, from HackiOS, APSL) on the redwood_gpu_*
   backend; SweetSurface fields drawn/planeCount/state only needed for G4.
   A small userland test: open the user client, GEM_NEW, SUBMIT, WAIT_FENCE.
3. G3: Mesa freedreno for Darwin (topaz build script), offscreen triangle.
4. G4: EAGL shim + backboardd hook (topaz, iOS 16), surfaces, presentation.
   Later: Metal via ry-diffusion/Chlorophyll (MIT; A630 = closest sibling).
   Bergamota has no licence file: use its notes, ask before copying code.

References used for structure (no code copied): Linux msm a6xx driver,
mxtpasontop/ios-sweet `gpu` branch (A618 on SM7150), Bergamota
`docs/GPU_A630_BRINGUP.md`, MakrSas/ios-topaz (IOAcceleratorES, GPU userland).
