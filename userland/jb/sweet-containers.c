/*
 * sweet-containers: tell the kernel's Sandbox stand-in (0202) which data container belongs to which code signing
 * identity, from containermanagerd's own records: every /var/mobile/Containers/Data/<class>/<UUID>/ has
 * .com.apple.mobile_container_manager.metadata.plist with MCMMetadataIdentifier (the app's or extension's bundle
 * id = its signing identity). Each pair goes to sandbox_set_container_path_for_signing_id() (op 0xd), as
 * containermanagerd does on a real iPhone. Extensions ask for their container at start (PlugInKit:
 * sandbox_container_path_for_pid) and abort without one ("plugin has invalid sandbox or container": Photos' picker).
 *
 *   sweet-containers         send all, then every 20 s the new ones (launchd job com.sweet.containers)
 *   sweet-containers -1      send all once and exit (prints each)
 * MIT, as userland/.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern int sandbox_set_container_path_for_signing_id(const char *identity, const char *path);

static const char *const classes[] = {
	"/private/var/mobile/Containers/Data/Application",
	"/private/var/mobile/Containers/Data/PluginKitPlugin",
	"/private/var/mobile/Containers/Data/InternalDaemon",
	"/private/var/mobile/Containers/Data/System",
	"/private/var/mobile/Containers/Data/VPNPlugin",
	"/private/var/mobile/Containers/Data/XPCService",
	"/private/var/containers/Data/System",
};

/* the identity a container's metadata names, or 0 */
static int identity_of(const char *dir, char *out, size_t size)
{
	char file[PATH_MAX];
	snprintf(file, sizeof(file), "%s/.com.apple.mobile_container_manager.metadata.plist", dir);
	FILE *f = fopen(file, "rb");
	if (f == NULL)
		return 0;
	unsigned char buf[65536];
	size_t n = fread(buf, 1, sizeof(buf), f);
	fclose(f);
	CFDataRef data = CFDataCreate(NULL, buf, (CFIndex)n);
	CFPropertyListRef pl = data ? CFPropertyListCreateWithData(NULL, data, 0, NULL, NULL) : NULL;
	int ok = 0;
	if (pl != NULL && CFGetTypeID(pl) == CFDictionaryGetTypeID()) {
		CFStringRef id = CFDictionaryGetValue(pl, CFSTR("MCMMetadataIdentifier"));
		ok = id != NULL && CFGetTypeID(id) == CFStringGetTypeID() &&
		    CFStringGetCString(id, out, (CFIndex)size, kCFStringEncodingUTF8);
	}
	if (pl != NULL)
		CFRelease(pl);
	if (data != NULL)
		CFRelease(data);
	return ok;
}

/* send every container not sent yet; returns how many were sent */
static unsigned scan(int verbose)
{
	static char **seen;
	static size_t nseen, capseen;
	unsigned sent = 0;
	for (size_t c = 0; c < sizeof(classes) / sizeof(classes[0]); c++) {
		DIR *d = opendir(classes[c]);
		if (d == NULL)
			continue;
		struct dirent *e;
		while ((e = readdir(d)) != NULL) {
			if (e->d_name[0] == '.')
				continue;
			char dir[PATH_MAX], id[256];
			snprintf(dir, sizeof(dir), "%s/%s", classes[c], e->d_name);
			int known = 0;
			for (size_t i = 0; i < nseen && !known; i++)
				known = strcmp(seen[i], dir) == 0;
			if (known || !identity_of(dir, id, sizeof(id)))
				continue;
			int r = sandbox_set_container_path_for_signing_id(id, dir);
			if (verbose || r != 0)
				printf("%s %s -> %s\n", r == 0 ? "sent" : "REFUSED", id, dir);
			if (r == 0) {
				sent++;
				if (nseen == capseen) {
					capseen = capseen ? capseen * 2 : 256;
					seen = realloc(seen, capseen * sizeof(*seen));
				}
				seen[nseen++] = strdup(dir);
			}
		}
		closedir(d);
	}
	return sent;
}

int main(int argc, char **argv)
{
	int once = argc > 1 && strcmp(argv[1], "-1") == 0;
	unsigned n = scan(once);
	printf("sweet-containers: %u containers sent to the Sandbox\n", n);
	fflush(stdout);
	if (once)
		return 0;
	for (;;) {
		sleep(20);
		n = scan(0);
		if (n) {
			printf("sweet-containers: %u new containers sent\n", n);
			fflush(stdout);
		}
	}
}
