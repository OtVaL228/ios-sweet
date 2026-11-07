/*
 * slider: the Alert Slider's TLMM pins through /dev/pmem, read only.
 *   slider [SECONDS]    prints cfg and GPIO_IN_OUT of gpio27/134/125 whenever they change (default 30 s)
 *   slider -nopull [S]  first writes GPIO_CFG = 0 to the three pins (gpio function, 2 mA, input, no pull: the DT's
 *                       tri_state_key_active, which Android applies; the bootloader leaves them pulled down)
 *   slider -stats [S]   reads every 1 ms and prints, per second, how often each pin was low and how often it changed
 * Tiles from Linux pinctrl-sm8150.c: 27 EAST 0x03500000, 134 SOUTH 0x03d00000, 125 WEST 0x03100000,
 * one 0x1000 page per pin, GPIO_CFG at +0, GPIO_IN_OUT at +4 (bit 0 in).
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static const struct {
	const char *name;
	uint64_t pa;
} pins[] = {
	{ "gpio125 (top)", 0x03100000u + 125u * 0x1000u },
	{ "gpio134 (middle)", 0x03d00000u + 134u * 0x1000u },
	{ "gpio27 (bottom)", 0x03500000u + 27u * 0x1000u },
};

static uint32_t rd(int fd, uint64_t pa)
{
	uint32_t v;
	return pread(fd, &v, 4, (off_t)pa) == 4 ? v : 0xdeadbeef;
}

int main(int argc, char **argv)
{
	int nopull = argc > 1 && !strcmp(argv[1], "-nopull");
	if (nopull) {
		argc--;
		argv++;
	}
	int stats = argc > 1 && !strcmp(argv[1], "-stats");
	if (stats) {
		argc--;
		argv++;
	}
	int secs = argc > 1 ? atoi(argv[1]) : 30;
	int fd = open("/dev/pmem", nopull ? O_RDWR : O_RDONLY);
	if (fd < 0) {
		perror("/dev/pmem");
		return 1;
	}
	for (int i = 0; nopull && i < 3; i++) {
		uint32_t v = 0;
		if (pwrite(fd, &v, 4, (off_t)pins[i].pa) != 4)
			perror("pmem write");
	}
	if (nopull)
		usleep(10000);
	for (int t = 0; stats && t < secs; t++) {
		unsigned low[3] = { 0 }, flips[3] = { 0 }, prev[3] = { 2, 2, 2 }, n = 0;
		for (; n < 1000; n++) {
			for (int i = 0; i < 3; i++) {
				unsigned v = rd(fd, pins[i].pa + 4) & 1;
				low[i] += !v;
				flips[i] += prev[i] != 2 && v != prev[i];
				prev[i] = v;
			}
			usleep(1000);
		}
		printf("%2d s:", t);
		for (int i = 0; i < 3; i++)
			printf("  %s low %3u/%u flips %u", pins[i].name, low[i], n, flips[i]);
		printf("\n");
		fflush(stdout);
	}
	if (stats)
		return 0;
	uint32_t last[3][2];
	memset(last, 0xff, sizeof(last));
	for (int t = 0; t < secs * 5; t++) {
		uint32_t now[3][2];
		for (int i = 0; i < 3; i++) {
			now[i][0] = rd(fd, pins[i].pa);
			now[i][1] = rd(fd, pins[i].pa + 4);
		}
		if (memcmp(now, last, sizeof(now))) {
			printf("%5.1f s:", t / 5.0);
			for (int i = 0; i < 3; i++)
				printf("  %s cfg %#x io %#x", pins[i].name, now[i][0], now[i][1]);
			printf("\n");
			fflush(stdout);
			memcpy(last, now, sizeof(now));
		}
		usleep(200000);
	}
	return 0;
}
