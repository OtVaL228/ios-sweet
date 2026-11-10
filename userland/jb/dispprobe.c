/*
 * dispprobe: READ-ONLY look at the SM8150 display controller (MDSS/DPU) the bootloader left running, through
 * /dev/pmem (kernel 0094+, boot-arg sweetpmem=1, root). Nothing is written.
 *
 * Purpose: brightness and Night Shift with no display driver, via the DSPP colour-correction stage (PCC, a 3x3
 * matrix every pixel of a layer mixer's output passes through, as Linux's night light uses). Before writing it we
 * need to know: where the DPU registers are (the MDP block at 0x0ae01000 or 0x0ae00000), which CTL path and layer
 * mixers the bootloader's continuous splash uses, and whether PCC is already on.
 *
 * Offsets are Linux drivers/gpu/drm/msm/disp/dpu1 (sm8150 catalog: CTL 0x1000 + 0x200 n, LM 0x44000 + 0x1000 n,
 * DSPP 0x54000 + 0x2000 n with PCC at +0x1700, PINGPONG 0x70000 + 0x800 n, INTF 0x6a000 + 0x800 n).
 *
 *   dispprobe          print the table (send all of it)
 * MIT, as userland/.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int fd = -1;

static uint32_t rd(uint64_t pa)
{
	uint32_t v = 0xdeadbeef;
	if (pread(fd, &v, 4, (off_t)pa) != 4)
		return 0xdeadbeef;
	return v;
}

static void row(const char *name, uint64_t pa, const unsigned *offs, unsigned n)
{
	printf("%-8s %#010llx:", name, (unsigned long long)pa);
	for (unsigned i = 0; i < n; i++)
		printf(" +%03x=%08x", offs[i], rd(pa + offs[i]));
	printf("\n");
	fflush(stdout);
}

int main(void)
{
	fd = open("/dev/pmem", O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "/dev/pmem: %s (kernel 0094+, boot-arg sweetpmem=1, run as root)\n", strerror(errno));
		return 1;
	}
	printf("MDSS hw version @0x0ae00000 = %08x (sm8150 DPU 5.0: 50000000)\n", rd(0x0ae00000));
	const uint64_t bases[2] = { 0x0ae01000, 0x0ae00000 };
	for (int b = 0; b < 2; b++) {
		uint64_t B = bases[b];
		char name[16];
		printf("\n== MDP base candidate %#llx ==\n", (unsigned long long)B);
		static const unsigned top[] = { 0x000, 0x004, 0x014, 0x018, 0x024, 0x02c, 0x030 };
		row("TOP", B, top, 7);
		/* CTL: LAYER(LM0..LM2), TOP, FLUSH, START, INTF_ACTIVE (DPU 5 "active" CTL), FETCH_PIPE_ACTIVE */
		static const unsigned ctl[] = { 0x000, 0x004, 0x008, 0x014, 0x018, 0x01c, 0x0f4, 0x0f8, 0x0e4 };
		for (int i = 0; i < 6; i++) {
			snprintf(name, sizeof(name), "CTL%d", i);
			row(name, B + 0x1000 + 0x200 * i, ctl, 9);
		}
		/* LM: OP_MODE, OUT_SIZE (h << 16 | w), BORDER_COLOR */
		static const unsigned lm[] = { 0x000, 0x004, 0x008, 0x00c };
		for (int i = 0; i < 6; i++) {
			snprintf(name, sizeof(name), "LM%d", i);
			row(name, B + 0x44000 + 0x1000 * i, lm, 4);
		}
		/* DSPP: its first words, then PCC: enable word, then the nine coefficients (0x10..0x30) */
		static const unsigned dspp[] = { 0x000, 0x004, 0x008, 0x00c };
		static const unsigned pcc[] = { 0x1700, 0x1704, 0x1708, 0x170c, 0x1710, 0x1714, 0x1718, 0x171c, 0x1720,
			0x1724, 0x1728, 0x172c, 0x1730 };
		for (int i = 0; i < 4; i++) {
			snprintf(name, sizeof(name), "DSPP%d", i);
			row(name, B + 0x54000 + 0x2000 * i, dspp, 4);
			snprintf(name, sizeof(name), "PCC%d", i);
			row(name, B + 0x54000 + 0x2000 * i, pcc, 13);
		}
		/* PINGPONG and INTF: enough to see which ones run (INTF_TIMING_ENGINE_EN at +0x6c) */
		static const unsigned pp[] = { 0x000, 0x004, 0x030 };
		for (int i = 0; i < 4; i++) {
			snprintf(name, sizeof(name), "PP%d", i);
			row(name, B + 0x70000 + 0x800 * i, pp, 3);
		}
		static const unsigned intf[] = { 0x000, 0x004, 0x010, 0x06c, 0x2b0 };
		for (int i = 0; i < 4; i++) {
			snprintf(name, sizeof(name), "INTF%d", i);
			row(name, B + 0x6a000 + 0x800 * i, intf, 5);
		}
	}
	return 0;
}
