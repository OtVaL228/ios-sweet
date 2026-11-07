/*
 * fpwrap: fileproviderd with libsweet-fpfs (see fpfs.m). launchd drops DYLD_* from a job's EnvironmentVariables,
 * so the com.apple.FileProvider job runs this instead: it puts the library in DYLD_INSERT_LIBRARIES and execs
 * fileproviderd in the same process (same pid and Mach services). Without the library, or with a file
 * /var/local/fpwrap.off, it execs fileproviderd as it is.
 */
#include <stdlib.h>
#include <unistd.h>

#define FILEPROVIDERD "/System/Library/Frameworks/FileProvider.framework/Support/fileproviderd"
#define LIBRARY       "/var/local/lib/libsweet-fpfs.dylib"

int
main(int argc, char **argv)
{
	(void)argc;
	if (access(LIBRARY, R_OK) == 0 && access("/var/local/fpwrap.off", F_OK) != 0) {
		setenv("DYLD_INSERT_LIBRARIES", LIBRARY, 1);
	}
	argv[0] = FILEPROVIDERD;
	execv(FILEPROVIDERD, argv);
	return 127;
}
