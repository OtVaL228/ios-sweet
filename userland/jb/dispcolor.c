/*
 * dispcolor: brightness and Night Shift by hand, through the display controller's colour-correction stage (DSPP PCC,
 * a 3x3 matrix on every pixel after the layer mixers), via /dev/pmem (kernel 0094+, boot-arg sweetpmem=1, root).
 *
 * The bootloader's continuous splash (dispprobe, 2026-11-09): MDP at 0x0ae01000 (version 0x50000001), one path CTL0
 * -> INTF1 (DSI0), two layer mixers LM0 + LM1 of 720x3120 each (the 1440-wide panel in halves), so DSPP0 and DSPP1.
 * PCC was off. Only CTL0's flush register and DSPP0/DSPP1's PCC are written, and only when the controller still
 * looks like that (anything else: refused, nothing written). DSPP2 and up are never touched (reading DSPP2 may hang).
 *
 * Linux dpu1 (sm8150): PCC at DSPP + 0x1700: +0x00 enable (bit 0), coefficients S3.15 in 18 bits (1.0 = 0x8000):
 * red output from R/G/B at +0x10/+0x1c/+0x28, green at +0x14/+0x20/+0x2c, blue at +0x18/+0x24/+0x30.
 * Applied at the next frame by CTL0 FLUSH (+0x18) bits 13 (DSPP0) and 14 (DSPP1). Undo: `dispcolor off` (or reboot).
 *
 *   dispcolor show                  what PCC holds now
 *   dispcolor rgb R G B             scale each colour (0.0 - 1.0), e.g. 1 0.8 0.55
 *   dispcolor brightness PCT        all three to PCT percent (AMOLED: less light), e.g. 50
 *   dispcolor night PCT [BRIGHT]    Night Shift of strength PCT (0 - 100) at BRIGHT percent (default 100)
 *   dispcolor off                   PCC off: the picture as before
 *   dispcolor dither on|temporal|off  dither the output to 8 bits (PINGPONG0/1 dither block): hides the banding a
 *                                   dimmed PCC leaves at low brightness ("temporal" also varies it frame to frame)
 *
 * Dither (Linux dpu_hw_pingpong.c, dpu_encoder.c; sdm845/sm8150 catalog: dither at PINGPONG + 0x30e0):
 * +0x0 enable, +0x4 bit depth (2 bits a component, 2 = 8 bits; bit 8 temporal), +0x8.. the 4x4 matrix, 4 bits each.
 * MIT, as userland/.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MDP             0x0ae01000ull
#define CTL0            (MDP + 0x1000)
#define CTL_FLUSH       0x018
#define CTL_INTF_ACTIVE 0x0f4
#define LM(n)           (MDP + 0x44000 + 0x1000 * (n))
#define DSPP(n)         (MDP + 0x54000 + 0x2000 * (n))
#define PCC             0x1700
#define FLUSH_DSPP0     (1u << 13)
#define FLUSH_DSPP1     (1u << 14)
#define PP(n)           (MDP + 0x70000 + 0x800 * (n))
#define PP_DITHER       0x30e0

static int fd = -1;

static uint32_t rd(uint64_t pa)
{
	uint32_t v = 0xdeadbeef;
	if (pread(fd, &v, 4, (off_t)pa) != 4)
		return 0xdeadbeef;
	return v;
}

static int wr(uint64_t pa, uint32_t v)
{
	if (pwrite(fd, &v, 4, (off_t)pa) != 4) {
		fprintf(stderr, "dispcolor: write %#llx: %s\n", (unsigned long long)pa, strerror(errno));
		return -1;
	}
	return 0;
}

/* the controller as dispprobe saw it, or refuse */
static int looks_right(void)
{
	uint32_t ver = rd(MDP), intf = rd(CTL0 + CTL_INTF_ACTIVE), lm0 = rd(LM(0) + 4), lm1 = rd(LM(1) + 4);
	if ((ver & 0xffff0000) != 0x50000000 || intf != 0x2 || lm0 != 0x0c3002d0 || lm1 != 0x0c3002d0) {
		fprintf(stderr, "dispcolor: the display controller is not as expected (version %08x, CTL0 INTF_ACTIVE %08x, "
		    "LM0 %08x, LM1 %08x; expected 5000xxxx, 2, 0c3002d0, 0c3002d0): nothing written\n", ver, intf, lm0, lm1);
		return 0;
	}
	return 1;
}

static uint32_t coeff(double v)
{
	if (v < 0)
		v = 0;
	if (v > 2)
		v = 2;
	return (uint32_t)(v * 32768.0 + 0.5) & 0x3ffff;
}

static void show(void)
{
	for (int n = 0; n < 2; n++) {
		uint64_t p = DSPP(n) + PCC;
		printf("DSPP%d PCC %s: R from rgb %05x %05x %05x, G %05x %05x %05x, B %05x %05x %05x (0x8000 = 1.0)\n", n,
		    (rd(p) & 1) ? "on" : "off", rd(p + 0x10), rd(p + 0x1c), rd(p + 0x28), rd(p + 0x14), rd(p + 0x20),
		    rd(p + 0x2c), rd(p + 0x18), rd(p + 0x24), rd(p + 0x30));
	}
}

static int set(int on, double r, double g, double b)
{
	if (!looks_right())
		return 1;
	for (int n = 0; n < 2; n++) {
		uint64_t p = DSPP(n) + PCC;
		if (on) {
			/* a diagonal matrix: each colour from itself only */
			if (wr(p + 0x10, coeff(r)) || wr(p + 0x1c, 0) || wr(p + 0x28, 0) ||
			    wr(p + 0x14, 0) || wr(p + 0x20, coeff(g)) || wr(p + 0x2c, 0) ||
			    wr(p + 0x18, 0) || wr(p + 0x24, 0) || wr(p + 0x30, coeff(b)) || wr(p, 1))
				return 1;
		} else if (wr(p, 0)) {
			return 1;
		}
	}
	if (wr(CTL0 + CTL_FLUSH, FLUSH_DSPP0 | FLUSH_DSPP1))
		return 1;
	usleep(50000);
	show();
	printf("CTL0 FLUSH now %08x (0: taken at the frame)\n", rd(CTL0 + CTL_FLUSH));
	return 0;
}

static int dither(int mode)       /* 0 off, 1 on, 2 temporal */
{
	static const unsigned matrix[16] = { 15, 7, 13, 5, 3, 11, 1, 9, 12, 4, 14, 6, 0, 8, 2, 10 };
	if (!looks_right())
		return 1;
	for (int n = 0; n < 2; n++) {
		uint64_t d = PP(n) + PP_DITHER;
		if (!mode) {
			if (wr(d, 0))
				return 1;
			continue;
		}
		uint32_t depth = 2 | 2 << 2 | 2 << 4 | 2 << 6 | (mode == 2 ? 1u << 8 : 0);
		if (wr(d + 4, depth))
			return 1;
		for (int i = 0; i < 16; i += 4)
			if (wr(d + 8 + i, matrix[i] | matrix[i + 1] << 4 | matrix[i + 2] << 8 | matrix[i + 3] << 12))
				return 1;
		if (wr(d, 1))
			return 1;
	}
	printf("PINGPONG0/1 dither: enable %u/%u, bit depth %03x/%03x\n", rd(PP(0) + PP_DITHER), rd(PP(1) + PP_DITHER),
	    rd(PP(0) + PP_DITHER + 4), rd(PP(1) + PP_DITHER + 4));
	return 0;
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: dispcolor show | rgb R G B | brightness PCT | night PCT [BRIGHT] | off | dither on|temporal|off\n");
		return 2;
	}
	fd = open("/dev/pmem", O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "/dev/pmem: %s (kernel 0094+, boot-arg sweetpmem=1, run as root)\n", strerror(errno));
		return 1;
	}
	if (!strcmp(argv[1], "show")) {
		show();
		return 0;
	}
	if (!strcmp(argv[1], "off"))
		return set(0, 1, 1, 1);
	if (!strcmp(argv[1], "dither") && argc == 3)
		return dither(!strcmp(argv[2], "on") ? 1 : !strcmp(argv[2], "temporal") ? 2 : 0);
	if (!strcmp(argv[1], "rgb") && argc == 5)
		return set(1, atof(argv[2]), atof(argv[3]), atof(argv[4]));
	if (!strcmp(argv[1], "brightness") && argc == 3) {
		double k = atof(argv[2]) / 100.0;
		return set(1, k, k, k);
	}
	if (!strcmp(argv[1], "night") && argc >= 3) {
		/* at full strength about 2700 K from 6500 K: green to 0.70, blue to 0.40 */
		double s = atof(argv[2]) / 100.0, k = argc > 3 ? atof(argv[3]) / 100.0 : 1.0;
		if (s < 0)
			s = 0;
		if (s > 1)
			s = 1;
		return set(1, k, k * (1.0 - 0.30 * s), k * (1.0 - 0.60 * s));
	}
	fprintf(stderr, "usage: dispcolor show | rgb R G B | brightness PCT | night PCT [BRIGHT] | off | dither on|temporal|off\n");
	return 2;
}
