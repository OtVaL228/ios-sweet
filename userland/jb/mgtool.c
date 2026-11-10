/*
 * mgtool: reads and edits MobileGestalt's answer cache (CacheExtra in com.apple.MobileGestalt.plist),
 * whose keys are base64(MD5("MGCopyAnswer" + name)) cut to 22 characters.
 *
 *   mgtool get NAME...          print each answer the cache has
 *   mgtool set NAME INTEGER     replace one integer answer (a copy of the file is kept as .sweet-orig)
 *   mgtool setbool NAME true|false   add or replace a boolean answer (same copy)
 *   mgtool setint NAME INTEGER  add or replace an integer answer (same copy); for answers MobileGestalt
 *                               computes and does not cache, e.g. ArtworkDeviceSubType (2556: Dynamic Island)
 *   mgtool setstr NAME STRING   add or replace a string answer (same copy); e.g. WifiVendor, which BlueTool
 *                               reads to choose the Bluetooth chip's boot script (it otherwise waits for an
 *                               AppleOLYHAL service this phone does not have: "Manufacturer is -1")
 *   mgtool unset NAME           remove an answer (MobileGestalt computes it again)
 *
 * NAME may be GROUP/ENTRY for an entry of an answer that is a dictionary: ArtworkTraits/ArtworkDeviceSubType
 * is the device subtype SpringBoard lays its screen out by (2556 = iPhone 14 Pro: Dynamic Island). A
 * dictionary answer is printed entry by entry.
 *
 * Safari (jb2): WebKit asks for 10-bit 'w30r' / 'b3a8' tiles on a P3 display, and only its sRGB
 * (BGRA) surfaces get drawn here; an sRGB answer for the display gamut keeps it on BGRA. MIT, as
 * userland/.
 */
#include <CommonCrypto/CommonDigest.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CACHE "/private/var/containers/Shared/SystemGroup/systemgroup.com.apple.mobilegestaltcache/Library/Caches/com.apple.MobileGestalt.plist"

static CFStringRef
obfuscated(const char *name)
{
	char in[256];
	unsigned char md[CC_MD5_DIGEST_LENGTH];
	snprintf(in, sizeof(in), "MGCopyAnswer%s", name);
	CC_MD5(in, (CC_LONG)strlen(in), md);
	static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	char out[25];
	int o = 0;
	for (int i = 0; i < 16; i += 3) {
		unsigned v = (unsigned)md[i] << 16 | (i + 1 < 16 ? (unsigned)md[i + 1] << 8 : 0) | (i + 2 < 16 ? md[i + 2] : 0);
		out[o++] = b64[(v >> 18) & 63];
		out[o++] = b64[(v >> 12) & 63];
		out[o++] = b64[(v >> 6) & 63];
		out[o++] = b64[v & 63];
	}
	out[22] = 0;
	return CFStringCreateWithCString(NULL, out, kCFStringEncodingASCII);
}

static void
print_value(const char *name, CFTypeRef v)
{
	if (v == NULL) {
		printf("mgtool: %s: not in the cache\n", name);
		return;
	}
	if (CFGetTypeID(v) == CFNumberGetTypeID()) {
		long long n = 0;
		CFNumberGetValue(v, kCFNumberLongLongType, &n);
		printf("mgtool: %s = %lld\n", name, n);
	} else if (CFGetTypeID(v) == CFBooleanGetTypeID()) {
		printf("mgtool: %s = %s\n", name, CFBooleanGetValue(v) ? "true" : "false");
	} else if (CFGetTypeID(v) == CFStringGetTypeID()) {
		char buf[256] = "";
		CFStringGetCString(v, buf, sizeof(buf), kCFStringEncodingUTF8);
		printf("mgtool: %s = \"%s\"\n", name, buf);
	} else if (CFGetTypeID(v) == CFDictionaryGetTypeID()) {
		CFIndex count = CFDictionaryGetCount(v);
		const void **keys = calloc((size_t)count, sizeof(void *)), **values = calloc((size_t)count, sizeof(void *));
		printf("mgtool: %s = a dictionary of %ld\n", name, (long)count);
		CFDictionaryGetKeysAndValues(v, keys, values);
		for (CFIndex i = 0; i < count; i++) {
			char key[128] = "?", full[300];
			if (CFGetTypeID(keys[i]) == CFStringGetTypeID())
				CFStringGetCString(keys[i], key, sizeof(key), kCFStringEncodingUTF8);
			snprintf(full, sizeof(full), "%s/%s", name, key);
			print_value(full, values[i]);
		}
		free(keys);
		free(values);
	} else {
		printf("mgtool: %s = (type %lu)\n", name, (unsigned long)CFGetTypeID(v));
	}
}

/*
 * Where NAME's answer lives: CacheExtra itself, or for GROUP/ENTRY the dictionary that is GROUP's answer
 * (made if `make` and it is not there). *key is the key in that dictionary.
 */
static CFMutableDictionaryRef
container_of(CFMutableDictionaryRef extra, const char *name, CFStringRef *key, bool make)
{
	const char *slash = strchr(name, '/');
	if (slash == NULL) {
		*key = obfuscated(name);
		return extra;
	}
	char group[128];
	snprintf(group, sizeof(group), "%.*s", (int)(slash - name), name);
	CFStringRef gkey = obfuscated(group);
	CFMutableDictionaryRef d = (CFMutableDictionaryRef)CFDictionaryGetValue(extra, gkey);
	if (d != NULL && CFGetTypeID(d) != CFDictionaryGetTypeID()) {
		printf("mgtool: %s is not a dictionary answer\n", group);
		return NULL;
	}
	if (d == NULL) {
		if (!make)
			return NULL;
		d = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
		CFDictionarySetValue(extra, gkey, d);
		printf("mgtool: %s was not in the cache: made\n", group);
	}
	*key = CFStringCreateWithCString(NULL, slash + 1, kCFStringEncodingUTF8);
	return d;
}

int
main(int argc, char **argv)
{
	int setbool = argc > 1 && strcmp(argv[1], "setbool") == 0;
	int setint = argc > 1 && strcmp(argv[1], "setint") == 0;
	int unset = argc > 1 && strcmp(argv[1], "unset") == 0;
	int setstr = argc > 1 && strcmp(argv[1], "setstr") == 0;
	if (argc < 3 || (strcmp(argv[1], "get") != 0 && strcmp(argv[1], "set") != 0 && !setbool && !setint && !setstr &&
	    !unset) ||
	    (strcmp(argv[1], "get") != 0 && !unset && argc != 4) || (unset && argc != 3)) {
		fprintf(stderr, "usage: mgtool get NAME... | mgtool set NAME INTEGER | mgtool setint NAME INTEGER | "
		    "mgtool setbool NAME true|false | mgtool setstr NAME STRING | mgtool unset NAME\n");
		return 2;
	}
	CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)CACHE, strlen(CACHE), false);
	CFReadStreamRef rs = CFReadStreamCreateWithFile(NULL, url);
	if (rs == NULL || !CFReadStreamOpen(rs)) {
		printf("mgtool: cannot open " CACHE "\n");
		return 1;
	}
	CFPropertyListFormat fmt = 0;
	CFErrorRef err = NULL;
	CFPropertyListRef pl = CFPropertyListCreateWithStream(NULL, rs, 0, kCFPropertyListMutableContainersAndLeaves, &fmt, &err);
	CFReadStreamClose(rs);
	if (pl == NULL || CFGetTypeID(pl) != CFDictionaryGetTypeID()) {
		printf("mgtool: the cache is not a dictionary\n");
		return 1;
	}
	CFMutableDictionaryRef extra = (CFMutableDictionaryRef)CFDictionaryGetValue(pl, CFSTR("CacheExtra"));
	if (extra == NULL || CFGetTypeID(extra) != CFDictionaryGetTypeID()) {
		printf("mgtool: no CacheExtra\n");
		return 1;
	}
	printf("mgtool: %ld answers in CacheExtra\n", (long)CFDictionaryGetCount(extra));
	if (strcmp(argv[1], "get") == 0) {
		for (int i = 2; i < argc; i++)
		{
			CFStringRef k = NULL;
			CFMutableDictionaryRef c = container_of(extra, argv[i], &k, false);
			print_value(argv[i], c ? CFDictionaryGetValue(c, k) : NULL);
		}
		return 0;
	}
	CFStringRef key = NULL;
	/* a group is not made: an answer of one entry would hide the rest MobileGestalt computes */
	CFMutableDictionaryRef where = container_of(extra, argv[2], &key, false);
	if (where == NULL) {
		print_value(argv[2], NULL);
		if (!unset)
			printf("mgtool: its group is not in the cache: left alone\n");
		return unset ? 0 : 1;
	}
	CFTypeRef old = CFDictionaryGetValue(where, key);
	print_value(argv[2], old);
	CFTypeRef nv = NULL;
	if (unset) {
		if (old == NULL) {
			printf("mgtool: %s is not in the cache: nothing to remove\n", argv[2]);
			return 0;
		}
	} else if (setbool) {
		/*
		 * WebKit's screenSupportsExtendedColor() is MGGetBoolAnswer(HasExtendedColorDisplay), which
		 * MobileGestalt derives from the model (iPhone10,3: true), not from the device tree; an
		 * answer in CacheExtra is what MobileGestalt returns first.
		 */
		nv = strcmp(argv[3], "true") == 0 ? kCFBooleanTrue : kCFBooleanFalse;
	} else if (setstr) {
		nv = CFStringCreateWithCString(NULL, argv[3], kCFStringEncodingUTF8);
	} else if (setint) {
		long long n = strtoll(argv[3], NULL, 0);
		nv = CFNumberCreate(NULL, kCFNumberLongLongType, &n);
	} else {
		if (old == NULL || CFGetTypeID(old) != CFNumberGetTypeID()) {
			printf("mgtool: %s is not an integer answer in the cache: left alone\n", argv[2]);
			return 1;
		}
		long long n = strtoll(argv[3], NULL, 0);
		nv = CFNumberCreate(NULL, kCFNumberLongLongType, &n);
	}
	if (unset)
		CFDictionaryRemoveValue(where, key);
	else
		CFDictionarySetValue(where, key, nv);
	if (access(CACHE ".sweet-orig", F_OK) != 0) {
		FILE *in = fopen(CACHE, "rb"), *out = fopen(CACHE ".sweet-orig", "wb");
		char buf[65536];
		size_t k;
		while (in != NULL && out != NULL && (k = fread(buf, 1, sizeof(buf), in)) > 0)
			fwrite(buf, 1, k, out);
		if (in != NULL)
			fclose(in);
		if (out != NULL)
			fclose(out);
	}
	CFWriteStreamRef ws = CFWriteStreamCreateWithFile(NULL, url);
	if (ws == NULL || !CFWriteStreamOpen(ws) ||
	    CFPropertyListWrite(pl, ws, fmt ? fmt : kCFPropertyListBinaryFormat_v1_0, 0, &err) <= 0) {
		printf("mgtool: writing the cache failed\n");
		return 1;
	}
	CFWriteStreamClose(ws);
	if (unset)
		printf("mgtool: %s removed\n", argv[2]);
	else
		print_value(argv[2], nv);
	printf("mgtool: written (original in " CACHE ".sweet-orig)\n");
	return 0;
}
