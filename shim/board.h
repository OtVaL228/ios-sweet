#pragma once
/*
 * Per-board constants for the shim. Build with BOARD=guacamole (see
 * build.sh) for the OnePlus 7 Pro; the default stays sweet.
 */
#if defined(BOARD_GUACAMOLE)
/* OnePlus 7 Pro (guacamole), SM8150. Facts from the live FDT + guac-probe:
 *  - splash: two VIG pipes (720 px halves) scanning one 1440x3120 a8r8g8b8
 *    buffer at cont_splash_region, stride 5760; the panel keeps refreshing
 *    with no MDP writes from us (CTL_START kicks corrupt it: never kick).
 *  - this ABL ignores IMEM restart_reason: reboot-to-fastboot is the PMIC
 *    PON_SOFT_RB_SPARE reason + PSCI SYSTEM_RESET. Writing PS_HOLD from EL1
 *    is XPU-protected (QUALCOMM CrashDump Mode). */
#define BOARD_NAME		"guacamole"
#define FB_W			1440
#define FB_H			3120
#define IMEM_RESTART_REASON	0x146bf65cUL	/* msm-imem@146bf000 + 0x65c (unused by ABL) */
#define REBOOT_VIA_PMIC_PON	1
#define PSHOLD_WRITABLE		0
#else
#define BOARD_NAME		"sweet"
#define FB_W			1080
#define FB_H			2400
#define IMEM_RESTART_REASON	0x146aa65cUL	/* qcom,msm-imem restart_reason@65c */
#define REBOOT_VIA_PMIC_PON	0
#define PSHOLD_WRITABLE		1
#endif
