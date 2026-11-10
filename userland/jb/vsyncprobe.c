/*
 * vsyncprobe: READ-ONLY look at the panel's real frame timing (first step of a display driver: vsync from the
 * hardware instead of the kernel's 60 Hz timer; 90 Hz needs it). Through /dev/pmem (kernel 0094+, sweetpmem=1, root).
 *
 * The OnePlus 7 Pro's panel runs in DSI command mode (panelbright probe: CTRL 000001f5); the bootloader leaves the
 * display controller refreshing it by itself. In command mode the frame timing is the PINGPONG block's: tear check
 * (the panel's TE signal), auto-refresh, and a line counter that runs once per frame. PINGPONG0 and 1 are the two
 * halves (LM0/LM1, dispprobe). Linux dpu_hw_pingpong.c offsets:
 *   +0x000 TEAR_CHECK_EN   +0x004 SYNC_CONFIG_VSYNC (bit 19 counter on, bit 20 vsync in from TE; low 16: clocks
 *   a line)   +0x008 SYNC_CONFIG_HEIGHT   +0x00c SYNC_WRCOUNT   +0x010 VSYNC_INIT_VAL   +0x014 INT_COUNT_VAL
 *   +0x018 SYNC_THRESH   +0x01c START_POS   +0x020 RD_PTR_IRQ   +0x024 WR_PTR_IRQ   +0x028 OUT_LINE_COUNT
 *   +0x02c LINE_COUNT   +0x030 AUTOREFRESH_CONFIG (bit 31 on, low 16: frames between refreshes)
 * Then LINE_COUNT and INT_COUNT_VAL are read as fast as /dev/pmem allows for 300 ms: each time the line count goes
 * back down a frame has started; the intervals between are the real refresh period. Nothing is written.
 *   vsyncprobe        (send all of the output)
 * MIT, as userland/.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MDP     0x0ae01000ull
#define PP(n)   (MDP + 0x70000 + 0x800 * (n))
#define INTF1   (MDP + 0x6a800)         /* DSI0's interface (CTL0 INTF_ACTIVE = 2) */
#define DSI0    0x0ae94000ull

static int fd = -1;

static uint32_t rd(uint64_t pa)
{
	uint32_t v = 0xdeadbeef;
	return pread(fd, &v, 4, (off_t)pa) == 4 ? v : 0xdeadbeef;
}

static uint64_t now_ns(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

int main(void)
{
	fd = open("/dev/pmem", O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "/dev/pmem: %s (kernel 0094+, boot-arg sweetpmem=1, run as root)\n", strerror(errno));
		return 1;
	}
	if ((rd(MDP) & 0xffff0000) != 0x50000000) {
		fprintf(stderr, "vsyncprobe: MDP version %08x, not the probed 5000xxxx: stopped\n", rd(MDP));
		return 1;
	}
	static const char *names[] = { "TEAR_CHECK_EN", "SYNC_CONFIG_VSYNC", "SYNC_CONFIG_HEIGHT", "SYNC_WRCOUNT",
		"VSYNC_INIT_VAL", "INT_COUNT_VAL", "SYNC_THRESH", "START_POS", "RD_PTR_IRQ", "WR_PTR_IRQ", "OUT_LINE_COUNT",
		"LINE_COUNT", "AUTOREFRESH_CONFIG" };
	for (int n = 0; n < 2; n++) {
		printf("PINGPONG%d %#llx:\n", n, (unsigned long long)PP(n));
		for (int i = 0; i < 13; i++)
			printf("  +%03x %-19s %08x\n", 4 * i, names[i], rd(PP(n) + 4 * i));
	}
	printf("MDP INTR_EN %08x STATUS %08x; INTR2_EN %08x STATUS %08x\n", rd(MDP + 0x10), rd(MDP + 0x14),
	    rd(MDP + 0x20), rd(MDP + 0x24));
	fflush(stdout);

	/*
	 * Run 1 (2026-11-09): PINGPONG0/1 all zero (no tear check, no autorefresh, LINE_COUNT 0) yet the screen updates,
	 * and MDP INTR STATUS had bit 8 (PINGPONG0 done). So also: INTF1 (newer DPUs keep tear check there: +0x280..),
	 * and DSI0 STATUS0 (bit 2 = MDP stream busy: one rise per frame sent to the panel, whatever started it).
	 */
	static const unsigned intf[] = { 0x000, 0x004, 0x008, 0x010, 0x060, 0x064, 0x06c, 0x0ac, 0x0b0, 0x090, 0x2a4,
		0x280, 0x284, 0x288, 0x28c, 0x290, 0x294, 0x298, 0x29c, 0x2a0, 0x2b0, 0x2b8, 0x2bc };
	printf("INTF1 %#llx:", (unsigned long long)INTF1);
	for (unsigned i = 0; i < sizeof(intf) / sizeof(intf[0]); i++)
		printf("%s+%03x=%08x", i && i % 6 == 0 ? "\n   " : " ", intf[i], rd(INTF1 + intf[i]));
	printf("\nDSI0 STATUS0 %08x, MDP stream: STREAM0_CTRL %08x TOTAL %08x, TRIG_CTRL %08x\n", rd(DSI0 + 0x8),
	    rd(DSI0 + 0x58), rd(DSI0 + 0x5c), rd(DSI0 + 0x84));
	fflush(stdout);
	{
		uint64_t s0 = now_ns(), tt, rises[128], busy_ns = 0, busy_max = 0, rise_at = 0;
		unsigned nrise = 0, n = 0, nbusy = 0;
		uint32_t prev = rd(DSI0 + 0x8) & 4, f0 = rd(INTF1 + 0x0ac), l0 = rd(INTF1 + 0x2a4);
		while ((tt = now_ns()) - s0 < 300000000ull) {
			uint32_t busy = rd(DSI0 + 0x8) & 4;
			n++;
			if (busy && !prev) {
				if (nrise < 128)
					rises[nrise++] = tt;
				rise_at = tt;
			}
			if (!busy && prev && rise_at) {         /* how long a frame takes to send (90 Hz needs < 11 ms) */
				uint64_t d = tt - rise_at;
				busy_ns += d;
				busy_max = d > busy_max ? d : busy_max;
				nbusy++;
			}
			prev = busy;
		}
		printf("DSI0 MDP-busy sampled %u times in 300 ms: %u frames sent; INTF1 +0ac %08x -> %08x, +2a4 %08x -> %08x\n",
		    n, nrise, f0, rd(INTF1 + 0x0ac), l0, rd(INTF1 + 0x2a4));
		if (nrise >= 2) {
			double sum = 0;
			printf("frame-to-frame (ms):");
			for (unsigned i = 1; i < nrise; i++) {
				double ms = (rises[i] - rises[i - 1]) / 1e6;
				sum += ms;
				if (i <= 24)
					printf(" %.2f", ms);
			}
			printf("\naverage %.3f ms = %.2f Hz\n", sum / (nrise - 1), 1000.0 * (nrise - 1) / sum);
			if (nbusy)
				printf("sending a frame to the panel takes %.2f ms on average, %.2f ms at most\n",
				    busy_ns / 1e6 / nbusy, busy_max / 1e6);
		}
	}

	/*
	 * Run 2 (2026-11-09): DSI0 sends a frame every 16.706 ms (59.86 Hz); INTF1's tear check block (+0x280..) is set
	 * up (+0x284 1, +0x288 00180066, +0x29c 00050004, +0x2a4 0c31 = height + 1, ...). Which INTF1 word counts lines?
	 * Every word of +0x200..+0x2fc read 2000 times over ~50 ms: those that change, with their lowest and highest.
	 */
	{
		enum { WORDS = 64, ROUNDS = 2000 };
		uint32_t lo[WORDS], hi[WORDS], first[WORDS];
		unsigned changes[WORDS] = { 0 };
		for (int w = 0; w < WORDS; w++)
			lo[w] = hi[w] = first[w] = rd(INTF1 + 0x200 + 4 * w);
		uint64_t s1 = now_ns();
		for (int r = 0; r < ROUNDS; r++) {
			for (int w = 0; w < WORDS; w++) {
				uint32_t v = rd(INTF1 + 0x200 + 4 * w);
				if (v != first[w])
					changes[w]++;
				lo[w] = v < lo[w] ? v : lo[w];
				hi[w] = v > hi[w] ? v : hi[w];
			}
		}
		printf("INTF1 +0x200..+0x2fc over %.1f ms:", (now_ns() - s1) / 1e6);
		int any = 0;
		for (int w = 0; w < WORDS; w++)
			if (changes[w]) {
				printf("\n  +%03x changes %u/%u times, %08x .. %08x", 0x200 + 4 * w, changes[w], ROUNDS, lo[w], hi[w]);
				any = 1;
			}
		printf("%s\n", any ? "" : " nothing changes");
		printf("INTF1 +0x2b4 %08x (autorefresh?)\n", rd(INTF1 + 0x2b4));
	}

	/* the frame starts: LINE_COUNT going back down */
	uint64_t start = now_ns(), t, frames[64];
	unsigned nframes = 0, samples = 0, maxline = 0;
	uint32_t last = rd(PP(0) + 0x2c) & 0xffff;
	while ((t = now_ns()) - start < 300000000ull) {
		uint32_t line = rd(PP(0) + 0x2c) & 0xffff;
		samples++;
		if (line > maxline)
			maxline = line;
		if (line + 16 < last && nframes < 64)
			frames[nframes++] = t;
		last = line;
	}
	printf("sampled LINE_COUNT %u times in 300 ms (every %.1f us); highest line %u; %u frame starts\n", samples,
	    300000.0 / (samples ? samples : 1), maxline, nframes);
	if (nframes >= 2) {
		double sum = 0, lo = 1e9, hi = 0;
		printf("intervals (ms):");
		for (unsigned i = 1; i < nframes; i++) {
			double ms = (frames[i] - frames[i - 1]) / 1e6;
			sum += ms;
			lo = ms < lo ? ms : lo;
			hi = ms > hi ? ms : hi;
			if (i <= 20)
				printf(" %.2f", ms);
		}
		double avg = sum / (nframes - 1);
		printf("\naverage %.3f ms = %.2f Hz (shortest %.2f, longest %.2f)\n", avg, 1000.0 / avg, lo, hi);
	} else {
		printf("the line count did not cycle: no frames went out in 300 ms (idle?), or LINE_COUNT is not this "
		    "register here. Run again while something moves on screen.\n");
	}
	return 0;
}
