/*
 * haptic: drive the AW8697 vibration motor through the kernel's sysctl kern.sweet_haptic (xnu 0248, boot-arg
 * sweethaptic=2).
 *   haptic N        play RAM waveform N (1..127) once: OnePlus's short effects (clicks, ticks) live in the low numbers
 *   haptic buzz MS  vibrate for MS milliseconds
 *   haptic stop     stop
 *   haptic reg R [V] read (or write V to, then read) AW8697 register R (hex); result in dmesg (0251)
 *   haptic dump     print chip registers, I2C controller, clock vote and pin state without changing anything (0251)
 *   haptic init     reset and set the chip up again, one click, register dump (0251; then dmesg | grep "SWEET HAPTIC")
 *   haptic          print the state (0x97 = configured, 0 = not ready)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/sysctl.h>

int
main(int argc, char **argv)
{
	int value = 0;
	size_t len = sizeof(value);
	if (argc == 1) {
		if (sysctlbyname("kern.sweet_haptic", &value, &len, NULL, 0)) {
			perror("kern.sweet_haptic");
			return 1;
		}
		printf("kern.sweet_haptic = %#x (%s)\n", value, value ? "ready" : "not configured: boot with sweethaptic=2");
		return 0;
	}
	if (!strcmp(argv[1], "stop")) {
		value = 0;
	} else if (!strcmp(argv[1], "init")) {
		value = 200;
	} else if (!strcmp(argv[1], "dump")) {
		value = 201;
	} else if (!strcmp(argv[1], "reg") && argc >= 3) {
		value = 0x100000 | (int)(strtoul(argv[2], NULL, 16) & 0xff) << 8;
		if (argc >= 4)
			value |= 0x10000 | (int)(strtoul(argv[3], NULL, 16) & 0xff);
	} else if (!strcmp(argv[1], "buzz") && argc == 3) {
		value = -atoi(argv[2]);
	} else {
		value = atoi(argv[1]);
	}
	if (sysctlbyname("kern.sweet_haptic", NULL, NULL, &value, sizeof(value))) {
		fprintf(stderr, "kern.sweet_haptic = %d: %s\n", value, strerror(errno));
		return 1;
	}
	return 0;
}
