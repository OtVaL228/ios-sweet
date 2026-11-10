/*
 * btprobe: first look at the OnePlus 7 Pro's Bluetooth path, READ-ONLY. Through /dev/pmem (kernel 0138+, boot-arg
 * sweetpmem=1, root), with wland's pmem.c.
 *
 * Hardware (research/bluetooth/dts-excerpt.txt, live Android FDT):
 *   controller  WCN3990 Bluetooth ("qca,wcn3990"), H4 over a QUP UART; no enable GPIO, powered by its rails:
 *               L7A 1.8 V (core), L2C 1.3 V (PA), L11C 3.0-3.3 V (LDO), plus the RF clock. These are the same rails
 *               and clock wland already votes for Wi-Fi (ldoa7, ldoc2, ldoc11, rfclka2): with Wi-Fi up they are on.
 *   UART        qupv3_se13_4uart (alias hsuart0) at 0xc8c000 = QUP wrapper 2, SE3 ("qcom,msm-geni-serial-hs"),
 *               pins GPIO43 TX, 44 RTS, 45 RX, 46 CTS. Not linked from the BT node in this tree: to be confirmed.
 *   clocks      as kernel sweet_touch.c (wrapper 2 SE0): GCC 0x100000; wrapper-2 vote register 0x52014 (bit 0 CORE,
 *               1 S_AHB, 2 M_AHB, 3 CORE_2X, 4 + n = SEn); SEn source RCG 0x1e148 + 0x130 * n, branch just below it.
 *
 * An unclocked QUP serial engine can stall the bus when read (a display block did: dispprobe), so the engine's own
 * registers are read only when the GCC says its clocks are on. GCC and TLMM registers are always safe.
 *   btprobe            (send all of the output)
 *   btprobe clock      the one write: set SE3's bit (7) in the wrapper-2 clock vote, as sweet_touch.c does for SE0
 *                      (bit 4), wait for the SE3 branch to run, then read the engine as above. Undo: reboot.
 * 2026-11-09 first run: wrapper-2 CORE/AHB on (the touch driver's vote 0x1f), SE3 branch off; GPIO43-46 are in the
 * EAST tile (TLMM 0x03500000) as plain GPIO with pull-down (RX GPIO45 pull-up, reads high), 172-174 in NORTH.
 * MIT, as userland/.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "pmem.h"

#define GCC             0x00100000ull
#define GCC_WRAP2_VOTE  0x52014u
#define SE_INDEX        3u                              /* wrapper 2 SE3 = 0xc8c000 */
#define SE_RCG          (0x1e148u + 0x130u * SE_INDEX)  /* 0x1e4d8 */
#define SE_BRANCH       (SE_RCG - 4u)                   /* 0x1e4d4 */
#define WRAP2_M_AHB     0x1e004u
#define WRAP2_S_AHB     0x1e008u
#define SE_PA           0x00c8c000ull

/* GENI (Linux include/linux/soc/qcom/geni-se.h) and its UART (drivers/tty/serial/qcom_geni_serial.c) */
static const struct { uint32_t off; const char *name; } se_regs[] = {
	{ 0x020, "GENI_FORCE_DEFAULT" }, { 0x024, "GENI_OUTPUT_CTRL" }, { 0x040, "GENI_STATUS" },
	{ 0x048, "GENI_SER_M_CLK_CFG" }, { 0x04c, "GENI_SER_S_CLK_CFG" }, { 0x068, "GENI_FW_REVISION_RO" },
	{ 0x06c, "GENI_S_FW_REVISION_RO" }, { 0x07c, "SE_GENI_CLK_SEL" }, { 0x22c, "SE_UART_LOOPBACK_CFG" },
	{ 0x258, "SE_GENI_DMA_MODE_EN" }, { 0x25c, "SE_UART_TX_TRANS_CFG" }, { 0x268, "SE_UART_TX_WORD_LEN" },
	{ 0x26c, "SE_UART_TX_STOP_BIT_LEN" }, { 0x270, "SE_UART_TX_TRANS_LEN" }, { 0x280, "SE_UART_RX_TRANS_CFG" },
	{ 0x28c, "SE_UART_RX_WORD_LEN" }, { 0x294, "SE_UART_RX_STALE_CNT" }, { 0x2a4, "SE_UART_TX_PARITY_CFG" },
	{ 0x2a8, "SE_UART_RX_PARITY_CFG" }, { 0x2ac, "SE_UART_MANUAL_RFR" }, { 0x600, "SE_GENI_M_CMD0" },
	{ 0x610, "SE_GENI_M_IRQ_STATUS" }, { 0x614, "SE_GENI_M_IRQ_EN" }, { 0x630, "SE_GENI_S_CMD0" },
	{ 0x640, "SE_GENI_S_IRQ_STATUS" }, { 0x800, "SE_GENI_TX_FIFO_STATUS" }, { 0x804, "SE_GENI_RX_FIFO_STATUS" },
	{ 0x80c, "SE_GENI_TX_WATERMARK" }, { 0x810, "SE_GENI_RX_WATERMARK" }, { 0x908, "SE_GENI_IOS" },
	{ 0xe24, "SE_HW_PARAM_0" }, { 0xe28, "SE_HW_PARAM_1" },
};

/* a GCC RCG level's configuration: source and divider (Linux clk-rcg2.c CFG_SRC_SEL/CFG_SRC_DIV, half-integer) */
static void rcg_cfg(const char *what, uint32_t cfg)
{
	unsigned div = cfg & 0x1f, src = (cfg >> 8) & 7, mode = (cfg >> 12) & 3;
	printf("  %-10s cfg %08x: source %u%s, divider %u.%u, %s\n", what, cfg, src,
	    src == 0 ? " (XO 19.2 MHz)" : src == 1 ? " (GPLL0 600 MHz)" : src == 5 ? " (GPLL0 even 300 MHz)" : "",
	    div ? (div + 1) / 2 : 1, div && !(div & 1) ? 5 : 0, mode ? "M/N on" : "no M/N");
}

#define WRAP2_COMMON    0x00cc0000ull   /* QUP wrapper 2 common registers (QUPV3_HW_VER at +4) */

int main(int argc, char **argv)
{
	pm_init();
	if (argc > 1 && !strcmp(argv[1], "clock")) {
		uint32_t v = rd32(GCC + GCC_WRAP2_VOTE);
		if ((v & 0x0f) != 0x0f)
			printf("wrapper-2 CORE/AHB bits not all set (%#x): setting them too\n", v);
		wr32(GCC + GCC_WRAP2_VOTE, v | 0x0f | (1u << (4 + SE_INDEX)));
		int rc = wait_bit(GCC + SE_BRANCH, 1u << 31, 0, 100);
		printf("vote %#x -> %#x; SE3 branch %08x %s\n", v, rd32(GCC + GCC_WRAP2_VOTE), rd32(GCC + SE_BRANCH),
		    rc ? "STILL OFF after 100 ms" : "running");
	}

	/* 1. clocks (GCC: always readable) */
	uint32_t vote = rd32(GCC + GCC_WRAP2_VOTE);
	printf("GCC wrapper-2 vote %#x: CORE %d, S_AHB %d, M_AHB %d, CORE_2X %d, SE0..5 %d%d%d%d%d%d\n", vote,
	    vote & 1, !!(vote & 2), !!(vote & 4), !!(vote & 8), !!(vote & 0x10), !!(vote & 0x20), !!(vote & 0x40),
	    !!(vote & 0x80), !!(vote & 0x100), !!(vote & 0x200));
	uint32_t mahb = rd32(GCC + WRAP2_M_AHB), sahb = rd32(GCC + WRAP2_S_AHB), br = rd32(GCC + SE_BRANCH);
	printf("branches: M_AHB %08x (%s), S_AHB %08x (%s%s), SE3 %08x (%s)\n", mahb, mahb >> 31 ? "OFF" : "on", sahb,
	    sahb >> 31 ? "OFF" : "on", sahb & 2 ? ", hw gated" : "", br, br >> 31 ? "OFF" : "on");
	uint32_t cmd = rd32(GCC + SE_RCG), dfs = rd32(GCC + SE_RCG + 0x14);
	printf("SE3 clock source RCG %#x: cmd %08x, DFS %s\n", SE_RCG, cmd, dfs & 1 ? "on" : "off");
	rcg_cfg("current", rd32(GCC + SE_RCG + 4));
	if (dfs & 1) {
		for (unsigned l = 0; l < 8; l++) {
			char name[16];
			snprintf(name, sizeof(name), "level %u", l);
			rcg_cfg(name, rd32(GCC + SE_RCG + 0x1c + 4 * l));
		}
	}

	/* 2. the UART pins (TLMM: always readable). Which tile holds GPIO43-46 is not known here, so all four */
	static const struct { const char *name; uint64_t pa; } tiles[] = {
		{ "west", 0x03100000 }, { "east", 0x03500000 }, { "north", 0x03900000 }, { "south", 0x03d00000 },
	};
	printf("TLMM GPIO43-46 (TX RTS RX CTS) and 172-174 (bt_en, conn 1.8 V, conn PA) per tile: cfg [function, pull, "
	    "drive] / in-out\n");
	for (unsigned t = 0; t < 4; t++) {
		printf("  %-5s", tiles[t].name);
		static const unsigned pins[] = { 43, 44, 45, 46, 172, 173, 174 };
		for (unsigned i = 0; i < 7; i++) {
			uint32_t c = rd32(tiles[t].pa + 0x1000ull * pins[i]), io = rd32(tiles[t].pa + 0x1000ull * pins[i] + 4);
			printf(" %u:%03x/%x", pins[i], c & 0x7ff, io & 3);
		}
		printf("\n");
	}

	/* 3. the serial engine itself, only with its clocks on */
	if ((vote & 0x86) != 0x86 || (br >> 31) || (mahb >> 31)) {
		printf("SE3 at %#llx: NOT READ (its clocks are not all on; reading an unclocked engine can hang). "
		    "`btprobe clock` turns them on.\n",
		    (unsigned long long)SE_PA);
		return 0;
	}
	printf("QUP wrapper 2 HW version %08x (at %#llx + 4)\n", rd32(WRAP2_COMMON + 4), (unsigned long long)WRAP2_COMMON);
	printf("SE3 at %#llx:\n", (unsigned long long)SE_PA);
	for (unsigned i = 0; i < sizeof(se_regs) / sizeof(se_regs[0]); i++)
		printf("  +%03x %-24s %08x\n", se_regs[i].off, se_regs[i].name, rd32(SE_PA + se_regs[i].off));
	uint32_t fw = rd32(SE_PA + 0x68);
	unsigned proto = (fw >> 8) & 0xff;
	printf("serial engine firmware: protocol %u (%s), version %#x\n", proto,
	    proto == 1 ? "SPI" : proto == 2 ? "UART" : proto == 3 ? "I2C" : proto == 0 ? "none loaded" : "?", fw & 0xff);
	return 0;
}
