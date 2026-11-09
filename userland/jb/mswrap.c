/*
 * mswrap: mediaserverd with libsweet-vabt (see vabt.c). launchd drops DYLD_* from a job's EnvironmentVariables,
 * so the com.apple.mediaserverd job runs this instead: it puts the library in DYLD_INSERT_LIBRARIES and execs
 * mediaserverd in the same process (same pid and Mach services). Without the library, or with a file
 * /var/local/mswrap.off, it execs mediaserverd as it is.
 */
#include <stdlib.h>
#include <unistd.h>

#define MEDIASERVERD "/usr/sbin/mediaserverd"
#define LIBRARY      "/var/local/lib/libsweet-vabt.dylib"

int
main(int argc, char **argv)
{
	(void)argc;
	if (access(LIBRARY, R_OK) == 0 && access("/var/local/mswrap.off", F_OK) != 0) {
		setenv("DYLD_INSERT_LIBRARIES", LIBRARY, 1);
	}
	argv[0] = MEDIASERVERD;
	execv(MEDIASERVERD, argv);
	return 127;
}
