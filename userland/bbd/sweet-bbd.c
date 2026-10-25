/*
 * sweet-bbd: launchd's com.apple.backboardd job runs this instead of
 * /usr/libexec/backboardd (tools/sweet-root-tweaks.py). launchd does not pass
 * DYLD_* variables from a job to a system daemon, so this adds our library to
 * DYLD_INSERT_LIBRARIES and execs the real window server: same pid, same
 * Mach services and bootstrap port from launchd. dyld honours the variable
 * since xnu 0121 (our AMFI answers its policy query).
 *
 * The library is left out when it is not there or the boot-args say
 * sweetbbd=0, so a broken library can be switched off without touching the
 * root. Written for ios-sweet after the idea of topaz's bbd_wrap.c.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

#define REAL      "/usr/libexec/backboardd"
#define LIBRARY   "/usr/lib/libsweet-bbd.dylib"

static int
switched_off(void)
{
	char args[1024];
	size_t len = sizeof(args) - 1;
	if (sysctlbyname("kern.bootargs", args, &len, NULL, 0) != 0) {
		return 0;
	}
	args[len] = '\0';
	for (char *p = strstr(args, "sweetbbd=0"); p; p = strstr(p + 1, "sweetbbd=0")) {
		if ((p == args || p[-1] == ' ') && (p[10] == '\0' || p[10] == ' ')) {
			return 1;
		}
	}
	return 0;
}

int
main(int argc, char **argv)
{
	(void)argc;
	if (access(LIBRARY, R_OK) == 0 && !switched_off()) {
		const char *old = getenv("DYLD_INSERT_LIBRARIES");
		char value[1024];
		snprintf(value, sizeof(value), "%s%s%s", LIBRARY, old ? ":" : "", old ? old : "");
		setenv("DYLD_INSERT_LIBRARIES", value, 1);
	}
	argv[0] = (char *)REAL;
	execv(REAL, argv);
	perror("sweet-bbd: exec " REAL);
	return 1;
}
