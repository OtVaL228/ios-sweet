/*
 * sbwrap: SpringBoard with tweaks. launchd drops DYLD_* from a job's EnvironmentVariables, so the SpringBoard job runs
 * this instead: it puts ElleKit's tweak loader in DYLD_INSERT_LIBRARIES and execs SpringBoard in the same process
 * (same pid: launchd and RunningBoard see SpringBoard). Without the loader (ElleKit not installed) it execs SpringBoard
 * as it is; a file /var/local/sbwrap.off does the same.
 */
#include <stdlib.h>
#include <unistd.h>

#define SPRINGBOARD "/System/Library/CoreServices/SpringBoard.app/SpringBoard"
#define LOADER      "/var/jb/usr/lib/ellekit/libinjector.dylib"

int
main(int argc, char **argv)
{
	(void)argc;
	if (access(LOADER, R_OK) == 0 && access("/var/local/sbwrap.off", F_OK) != 0) {
		setenv("DYLD_INSERT_LIBRARIES", LOADER, 1);
	}
	argv[0] = SPRINGBOARD;
	execv(SPRINGBOARD, argv);
	return 127;
}
