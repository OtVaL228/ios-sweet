/*
 * hfsgrow: grow the mounted HFS+ root online after its GPT partition was made larger (HFSIOC_RESIZE_VOLUME,
 * apple-oss hfs core/hfs_fsctl.h; hfs_extendfs refuses sizes past the device, DKIOCGETBLOCKCOUNT).
 *   hfsgrow            print the volume size
 *   hfsgrow BYTES      resize the volume at / to BYTES
 * MIT, as userland/.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
int fsctl(const char *, unsigned long, void *, unsigned int);

#define HFSIOC_RESIZE_VOLUME _IOW('h', 2, uint64_t)

static void show(const char *when)
{
	struct statfs s;
	if (statfs("/", &s) == 0)
		printf("%s: %s on %s, %llu blocks of %u = %.2f GiB, free %.2f GiB\n", when, s.f_fstypename, s.f_mntfromname,
		       (unsigned long long)s.f_blocks, s.f_bsize, (double)s.f_blocks * s.f_bsize / (1 << 30),
		       (double)s.f_bavail * s.f_bsize / (1 << 30));
}

int main(int argc, char **argv)
{
	show("now");
	if (argc < 2)
		return 0;
	uint64_t size = strtoull(argv[1], NULL, 0);
	printf("resizing / to %llu bytes (%.2f GiB)\n", (unsigned long long)size, (double)size / (1 << 30));
	if (fsctl("/", HFSIOC_RESIZE_VOLUME, &size, 0) != 0) {
		printf("fsctl: %s\n", strerror(errno));
		return 1;
	}
	show("after");
	return 0;
}
