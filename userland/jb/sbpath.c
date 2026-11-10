/*
 * sbpath: ask the kernel's Sandbox (stand-in, 0201) for processes' data containers, as PlugInKit does for an
 * extension (sandbox_container_path_for_pid(getpid(), buf, 1024): op 4). Read-only.
 *   sbpath                  every running process's answer (path, or the error)
 *   sbpath PID...           just these
 * A path for apps/extensions means 0201 learned them at launch; ENOENT (2) everywhere means it learned none.
 * MIT, as userland/.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <stdint.h>

extern int sandbox_container_path_for_pid(int pid, char *buffer, size_t length);
extern int proc_listallpids(void *buffer, int buffersize);     /* libproc (no header in the iOS SDK) */
extern int proc_name(int pid, void *buffer, uint32_t buffersize);

static void ask(int pid)
{
	char path[1024] = "", name[256] = "";
	proc_name(pid, name, sizeof(name));
	errno = 0;
	int r = sandbox_container_path_for_pid(pid, path, sizeof(path));
	if (r == 0)
		printf("%6d %-28s %s\n", pid, name, path);
	else
		printf("%6d %-28s error %d (%s)\n", pid, name, errno, strerror(errno));
}

int main(int argc, char **argv)
{
	if (argc > 1) {
		for (int i = 1; i < argc; i++)
			ask(atoi(argv[i]));
		return 0;
	}
	int pids[4096];
	int n = proc_listallpids(pids, (int)sizeof(pids));
	unsigned found = 0, none = 0;
	for (int i = 0; i < n; i++) {
		char path[1024];
		if (pids[i] <= 0)
			continue;
		if (sandbox_container_path_for_pid(pids[i], path, sizeof(path)) == 0) {
			ask(pids[i]);
			found++;
		} else {
			none++;
		}
	}
	printf("%u processes with a container, %u without\n", found, none);
	return 0;
}
