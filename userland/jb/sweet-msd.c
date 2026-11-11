/*
 * sweet-msd: launchd job wrapper for mediaserverd, as sweet-bbd is for backboardd. launchd does not pass DYLD_*
 * variables from a job to a daemon, so this adds /usr/lib/libsweet-autrace.dylib (userland/jb/autrace.c: logs the
 * CoreAudio property calls that fail) to DYLD_INSERT_LIBRARIES and execs the real /usr/sbin/mediaserverd: same pid,
 * same job. Only while debugging VirtualAudio; the job is pointed back at /usr/sbin/mediaserverd afterwards.
 * MIT, as userland/.
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define REAL "/usr/sbin/mediaserverd"
#define LIB  "/usr/lib/libsweet-autrace.dylib"

int
main(int argc, char **argv)
{
	(void)argc;
	if (access(LIB, R_OK) == 0) {
		setenv("DYLD_INSERT_LIBRARIES", LIB, 1);
		if (access("/var/local/autrace-has", F_OK) == 0) {
			setenv("AUTRACE_HAS", "1", 1);   /* also log every HasProperty on objects >= 30 */
		}
	}
	argv[0] = (char *)REAL;
	execv(REAL, argv);
	perror("sweet-msd: exec " REAL);
	return 1;
}
