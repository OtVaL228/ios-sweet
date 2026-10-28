/*
 * sweet-fs: look at (and rename) files on the phone's root, from autorun.sh
 * (iOS has no shell). Output lines reach the log as
 * "SWEET RUN: [sweet-fs] SWEET FS: ...".
 *
 *   sweet-fs container ID [SUBPATH]
 *       Finds the data container of ID (an app's, under
 *       /private/var/mobile/Containers/Data/Application, or a daemon's, under
 *       /private/var/containers/Data/System: the directory whose
 *       .com.apple.mobile_container_manager.metadata.plist has that
 *       MCMMetadataIdentifier), prints its path and lists SUBPATH inside it
 *       (default "Library/Application Support").
 *   sweet-fs ls PATH [DEPTH]
 *       Lists PATH, DEPTH levels deep (default: all).
 *   sweet-fs mv FROM TO
 *       rename(2); refuses if TO exists.
 *   sweet-fs aside ID SUBPATH PREFIX
 *       In ID's container, renames each entry of SUBPATH whose name starts
 *       with PREFIX to NAME.bak (e.g. lsd's database).
 *   sweet-fs kill NAME
 *       SIGTERM to every process called NAME (launchd starts a daemon again
 *       on demand).
 *
 * mv and aside set data aside rather than delete it.
 * Listings are recursive with sizes, at most MAX_ENTRIES lines.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <dirent.h>
#include <errno.h>
#include <fts.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <sys/stat.h>

/* libproc (not in the iOS SDK's headers) */
int proc_listallpids(void *buffer, int buffersize);
int proc_name(int pid, void *buffer, uint32_t buffersize);

static const char *const containers[] = {
	"/private/var/mobile/Containers/Data/Application",
	"/private/var/containers/Data/System",
};
#define METADATA    ".com.apple.mobile_container_manager.metadata.plist"
#define MAX_ENTRIES 400

/* MCMMetadataIdentifier of the container at dir, or false. */
static bool
container_id(const char *dir, char *id, size_t size)
{
	char path[PATH_MAX];
	snprintf(path, sizeof(path), "%s/%s", dir, METADATA);
	CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, (CFIndex)strlen(path), false);
	CFReadStreamRef stream = CFReadStreamCreateWithFile(NULL, url);
	CFRelease(url);
	bool found = false;
	if (stream != NULL && CFReadStreamOpen(stream)) {
		CFPropertyListRef plist = CFPropertyListCreateWithStream(NULL, stream, 0, kCFPropertyListImmutable, NULL, NULL);
		if (plist != NULL && CFGetTypeID(plist) == CFDictionaryGetTypeID()) {
			CFTypeRef value = CFDictionaryGetValue(plist, CFSTR("MCMMetadataIdentifier"));
			found = value != NULL && CFGetTypeID(value) == CFStringGetTypeID() &&
			    CFStringGetCString(value, id, (CFIndex)size, kCFStringEncodingUTF8);
		}
		if (plist != NULL) {
			CFRelease(plist);
		}
		CFReadStreamClose(stream);
	}
	if (stream != NULL) {
		CFRelease(stream);
	}
	return found;
}

static void
list(const char *top, int depth)
{
	char *paths[] = { (char *)top, NULL };
	FTS *fts = fts_open(paths, FTS_PHYSICAL | FTS_NOCHDIR, NULL);
	if (fts == NULL) {
		printf("SWEET FS: %s: cannot open\n", top);
		return;
	}
	unsigned entries = 0;
	unsigned long long bytes = 0;
	FTSENT *e;
	while ((e = fts_read(fts)) != NULL) {
		if (e->fts_info == FTS_DP) {
			continue;
		}
		if (e->fts_info == FTS_NS || e->fts_info == FTS_ERR || e->fts_info == FTS_DNR) {
			printf("SWEET FS:   %s: %s\n", e->fts_path, strerror(e->fts_errno));
			continue;
		}
		bool dir = e->fts_info == FTS_D;
		if (dir && depth >= 0 && e->fts_level >= depth) {
			fts_set(fts, e, FTS_SKIP);
		}
		if (!dir) {
			bytes += (unsigned long long)e->fts_statp->st_size;
		}
		if (++entries <= MAX_ENTRIES) {
			printf("SWEET FS:   %*s%s%s %lld\n", e->fts_level * 2, "", e->fts_name, dir ? "/" : "",
			    dir ? 0LL : (long long)e->fts_statp->st_size);
		}
	}
	fts_close(fts);
	printf("SWEET FS: %u entries, %llu bytes in files%s\n", entries, bytes,
	    entries > MAX_ENTRIES ? " (list cut)" : "");
}

/* Path of the first data container of id; false if none. */
static bool
find_container(const char *bundle_id, char *path, size_t size)
{
	for (size_t i = 0; i < sizeof(containers) / sizeof(containers[0]); i++) {
		DIR *d = opendir(containers[i]);
		if (d == NULL) {
			continue;
		}
		struct dirent *de;
		while ((de = readdir(d)) != NULL) {
			char id[256];
			snprintf(path, size, "%s/%s", containers[i], de->d_name);
			if (de->d_name[0] != '.' && container_id(path, id, sizeof(id)) && strcmp(id, bundle_id) == 0) {
				closedir(d);
				return true;
			}
		}
		closedir(d);
	}
	printf("SWEET FS: no data container for %s\n", bundle_id);
	return false;
}

static int
container(const char *bundle_id, const char *sub)
{
	char dir[PATH_MAX], top[PATH_MAX];
	if (!find_container(bundle_id, dir, sizeof(dir))) {
		return 1;
	}
	printf("SWEET FS: %s container %s\n", bundle_id, dir);
	snprintf(top, sizeof(top), "%s/%s", dir, sub);
	list(top, -1);
	return 0;
}

static int
move(const char *from, const char *to)
{
	struct stat st;
	if (lstat(to, &st) == 0) {
		printf("SWEET FS: mv %s: %s exists, nothing done\n", from, to);
		return 1;
	}
	int status = rename(from, to);
	printf("SWEET FS: mv %s -> %s: %s\n", from, to, status == 0 ? "done" : strerror(errno));
	return status == 0 ? 0 : 1;
}

static int
aside(const char *bundle_id, const char *sub, const char *prefix)
{
	char dir[PATH_MAX], top[PATH_MAX];
	if (!find_container(bundle_id, dir, sizeof(dir))) {
		return 1;
	}
	snprintf(top, sizeof(top), "%s/%s", dir, sub);
	DIR *d = opendir(top);
	if (d == NULL) {
		printf("SWEET FS: %s: %s\n", top, strerror(errno));
		return 1;
	}
	int moved = 0, status = 0;
	struct dirent *de;
	while ((de = readdir(d)) != NULL) {
		size_t len = strlen(de->d_name);
		if (strncmp(de->d_name, prefix, strlen(prefix)) != 0 ||
		    (len > 4 && strcmp(de->d_name + len - 4, ".bak") == 0)) {
			continue;
		}
		char from[PATH_MAX], to[PATH_MAX];
		snprintf(from, sizeof(from), "%s/%s", top, de->d_name);
		snprintf(to, sizeof(to), "%s.bak", from);
		status |= move(from, to);
		moved++;
	}
	closedir(d);
	printf("SWEET FS: aside: %d entries starting with %s in %s\n", moved, prefix, top);
	return status;
}

static int
kill_named(const char *name)
{
	int pids[4096];
	int count = proc_listallpids(pids, sizeof(pids));
	int killed = 0;
	for (int i = 0; i < count; i++) {
		char this[64] = "";
		if (pids[i] > 1 && proc_name(pids[i], this, sizeof(this)) > 0 && strcmp(this, name) == 0) {
			int status = kill(pids[i], SIGTERM);
			printf("SWEET FS: kill %s[%d]: %s\n", name, pids[i], status == 0 ? "sent" : strerror(errno));
			killed++;
		}
	}
	if (!killed) {
		printf("SWEET FS: kill: no process called %s\n", name);
	}
	return killed ? 0 : 1;
}

int
main(int argc, char *argv[])
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	if (argc >= 3 && strcmp(argv[1], "container") == 0) {
		return container(argv[2], argc >= 4 ? argv[3] : "Library/Application Support");
	}
	if (argc >= 3 && strcmp(argv[1], "ls") == 0) {
		printf("SWEET FS: %s\n", argv[2]);
		list(argv[2], argc >= 4 ? atoi(argv[3]) : -1);
		return 0;
	}
	if (argc == 4 && strcmp(argv[1], "mv") == 0) {
		return move(argv[2], argv[3]);
	}
	if (argc == 5 && strcmp(argv[1], "aside") == 0) {
		return aside(argv[2], argv[3], argv[4]);
	}
	if (argc == 3 && strcmp(argv[1], "kill") == 0) {
		return kill_named(argv[2]);
	}
	fprintf(stderr, "usage: %s container ID [SUBPATH] | ls PATH [DEPTH] | mv FROM TO | aside ID SUBPATH PREFIX"
	    " | kill NAME\n", argv[0]);
	return 2;
}
