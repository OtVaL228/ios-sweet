/*
 * panelbright: the OnePlus 7 Pro panel's own brightness (DCS 0x51, "set display brightness") sent over DSI0 while the
 * bootloader's video mode keeps running, with no display driver. Through /dev/pmem (kernel 0094+, boot-arg
 * sweetpmem=1, root). Dimming in the colour stage crushed colours (AMOLED low codes); the panel's brightness lowers
 * the drive current and keeps every colour.
 *
 *   panelbright probe      READ-ONLY: DSI0 controller registers and the command buffer page. Run this first.
 *   panelbright set N      send DCS 0x51 with level N (0 - 1023, OnePlus' bl-max-level), then put DSI back as it was
 *   panelbright raw HEX..  send a DCS long write of these bytes (first = the command), e.g. `raw 53 20`
 *
 * DSI 6G registers = the v1 offsets + 4 (Linux msm dsi_host: ctrl_base += io_offset 4; Qualcomm LK mipi_dsi.h):
 *   +0x000 HW_VERSION   +0x004 CTRL (bit0 enable, bit1 video mode, bit2 command mode)   +0x008 STATUS0 (bit1 DMA busy)
 *   +0x03c CMD_DMA_CTRL (bit28 embedded: packet headers from the buffer; bit26 low power; bit31/30 broadcast)
 *   +0x048 DMA_BASE (bus address of the packets)   +0x04c DMA_LEN   +0x084 TRIG_CTRL (bits 2:0 DMA trigger, 4 = SW)
 *   +0x090 TRIG_DMA (write 1: send)   +0x110 INTR_CTRL (bit0 CMD_DMA_DONE, write 1 to clear; bit1 its mask)
 * As Linux's video-mode transfer: CTRL |= command mode for the transfer, embedded DMA, software trigger, wait for
 * CMD_DMA_DONE, then CTRL back. The controller sends it in the next blanking period.
 *
 * Packet (Linux dsi_cmd_dma_add): word 0 = WC low, WC high, data type (0x39 DCS long write, VC 0), 0xc0 (last packet,
 * long); then the payload padded with 0xff to a whole word.
 *
 * The packets live in the bootloader's splash memory just past the visible frame (0x9c000000 + 3120 * 5760 rounded
 * up to a page): XNU runs in the upper DDR bank and never touches it, and the display already reads from that
 * region (if the display's SMMU maps only what the bootloader set up, this is where it can read).
 * MIT, as userland/.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define DSI0            0x0ae94000ull
#define HW_VERSION      0x000
#define CTRL            0x004
#define STATUS0         0x008
#define CMD_DMA_CTRL    0x03c
#define DMA_BASE        0x048
#define DMA_LEN         0x04c
#define TRIG_CTRL       0x084
#define TRIG_DMA        0x090
#define INTR_CTRL       0x110

#define SPLASH          0x9c000000ull
#define BUF             ((SPLASH + 3120ull * 5760 + 0xfff) & ~0xfffull)     /* 0x9d124000 */

static int fd = -1;

static uint32_t rd(uint64_t pa)
{
	uint32_t v = 0xdeadbeef;
	return pread(fd, &v, 4, (off_t)pa) == 4 ? v : 0xdeadbeef;
}

static int wr(uint64_t pa, uint32_t v)
{
	if (pwrite(fd, &v, 4, (off_t)pa) != 4) {
		fprintf(stderr, "panelbright: write %#llx: %s\n", (unsigned long long)pa, strerror(errno));
		return -1;
	}
	return 0;
}

static void dump(const char *name, uint64_t b)
{
	static const unsigned offs[] = { HW_VERSION, CTRL, STATUS0, 0x00c, CMD_DMA_CTRL, 0x040, DMA_BASE, DMA_LEN, 0x050,
		TRIG_CTRL, 0x088, 0x0ac, 0x10c, INTR_CTRL, 0x118, 0x1f0 };
	printf("%s %#llx:", name, (unsigned long long)b);
	for (unsigned i = 0; i < sizeof(offs) / sizeof(offs[0]); i++)
		printf("%s+%03x=%08x", i == 8 ? "\n    " : " ", offs[i], rd(b + offs[i]));
	printf("\n");
	fflush(stdout);
}

static int probe(void)
{
	dump("DSI0", DSI0);     /* not DSI1: an unused block may not answer (reading DSPP2 seemed to hang) */
	/* the bootloader's last command packet, where DMA_BASE still points (plain physical memory if no SMMU) */
	uint32_t last = rd(DSI0 + DMA_BASE), n = rd(DSI0 + DMA_LEN);
	if (last >= 0x80000000u && n && n <= 64) {
		printf("bootloader's last packet at %#x (%u bytes):", last, n);
		for (uint32_t i = 0; i < n; i += 4)
			printf(" %08x", rd(last + i));
		printf("\n");
	}
	printf("command buffer page %#llx:", (unsigned long long)BUF);
	for (int i = 0; i < 8; i++)
		printf(" %08x", rd(BUF + 4 * i));
	printf("\n");
	return 0;
}

static uint64_t now_us(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}

/* one DCS long write of len bytes (bytes[0] = the command) */
static int send_dcs(const uint8_t *bytes, unsigned len)
{
	uint32_t ver = rd(DSI0 + HW_VERSION), ctrl = rd(DSI0 + CTRL);
	/*
	 * The OnePlus 7 Pro's panel runs in COMMAND mode (probe 2026-11-09: CTRL 000001f5 = enable, command mode, four
	 * lanes, clock; the display controller refreshes it by itself): DMA commands go between its frames. Video mode
	 * (CTRL bit 1) is also accepted, with command mode on for the transfer as Linux does.
	 */
	if ((ver >> 24) != 0x20 || !(ctrl & 1) || !(ctrl & 6) || len == 0 || len > 24) {
		fprintf(stderr, "panelbright: DSI0 is not as expected (version %08x, CTRL %08x: want 20xxxxxx, enabled, video "
		    "or command mode): nothing sent\n", ver, ctrl);
		return 1;
	}
	/* the packet, into the buffer */
	uint8_t pkt[32];
	memset(pkt, 0xff, sizeof(pkt));
	pkt[0] = (uint8_t)len;
	pkt[1] = 0;
	pkt[2] = 0x39;          /* DCS long write, virtual channel 0 */
	pkt[3] = 0xc0;          /* last packet, long packet */
	memcpy(pkt + 4, bytes, len);
	unsigned total = (4 + len + 3) & ~3u;
	for (unsigned i = 0; i < total; i += 4) {
		uint32_t w;
		memcpy(&w, pkt + i, 4);
		if (wr(BUF + i, w))
			return 1;
	}

	uint32_t dma_ctrl = rd(DSI0 + CMD_DMA_CTRL), trig = rd(DSI0 + TRIG_CTRL), intr = rd(DSI0 + INTR_CTRL);
	int ret = 0;
	/* clear a stale "done", then: command mode on for the transfer, embedded high-speed DMA, software trigger */
	wr(DSI0 + INTR_CTRL, intr | 1);
	wr(DSI0 + CTRL, ctrl | 4);
	wr(DSI0 + CMD_DMA_CTRL, (dma_ctrl & ~((1u << 31) | (1u << 30) | (1u << 26))) | (1u << 28));
	if ((trig & 7) != 4)
		wr(DSI0 + TRIG_CTRL, (trig & ~7u) | 4);
	wr(DSI0 + DMA_BASE, (uint32_t)BUF);
	wr(DSI0 + DMA_LEN, total);
	wr(DSI0 + TRIG_DMA, 1);

	uint64_t began = now_us();
	uint32_t st = 0;
	while (!((st = rd(DSI0 + INTR_CTRL)) & 1) && now_us() - began < 100000)
		usleep(200);
	if (st & 1) {
		printf("panelbright: sent (%u bytes) in %llu us\n", total, (unsigned long long)(now_us() - began));
	} else {
		fprintf(stderr, "panelbright: no CMD_DMA_DONE after 100 ms (INTR_CTRL %08x, STATUS0 %08x)\n", st,
		    rd(DSI0 + STATUS0));
		ret = 1;
	}
	/* DSI back as the bootloader had it */
	wr(DSI0 + INTR_CTRL, rd(DSI0 + INTR_CTRL) | 1);
	if ((trig & 7) != 4)
		wr(DSI0 + TRIG_CTRL, trig);
	wr(DSI0 + CMD_DMA_CTRL, dma_ctrl);
	wr(DSI0 + CTRL, ctrl);
	return ret;
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: panelbright probe | set 0-1023 | raw HEX...\n");
		return 2;
	}
	fd = open("/dev/pmem", O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "/dev/pmem: %s (kernel 0094+, boot-arg sweetpmem=1, run as root)\n", strerror(errno));
		return 1;
	}
	if (!strcmp(argv[1], "probe"))
		return probe();
	if (!strcmp(argv[1], "set") && argc == 3) {
		long n = strtol(argv[2], NULL, 0);
		if (n < 0)
			n = 0;
		if (n > 1023)
			n = 1023;
		uint8_t cmd[3] = { 0x51, (uint8_t)(n >> 8), (uint8_t)n };
		return send_dcs(cmd, 3);
	}
	if (!strcmp(argv[1], "raw") && argc >= 3 && argc <= 26) {
		uint8_t b[24];
		for (int i = 2; i < argc; i++)
			b[i - 2] = (uint8_t)strtoul(argv[i], NULL, 16);
		return send_dcs(b, (unsigned)(argc - 2));
	}
	fprintf(stderr, "usage: panelbright probe | set 0-1023 | raw HEX...\n");
	return 2;
}
