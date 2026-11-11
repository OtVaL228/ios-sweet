/*
 * auprobe: what CoreAudio's HAL (inside mediaserverd) says about its devices: ID, UID, name, transport, which
 * ('mute','outp',0..4) properties exist (VirtualAudio makes the speaker / receiver ports only if
 * HasProperty(mute, outp, 4 / 3): research/audio/ioa2-mute-property-0400.md), and the control objects each device
 * owns (class, base class, scope, element). The HAL client API is not in the iOS SDK headers: resolved with dlsym.
 *   auprobe            the devices clients see (on iOS only VirtualAudio's)
 *   auprobe ID...      those object IDs directly (e.g. 33 = SweetBuiltInAudio in mediaserverd's log)
 * MIT, as userland/.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct { uint32_t sel, scope, elem; } addr_t;
typedef int32_t (*get_t)(uint32_t, const addr_t *, uint32_t, const void *, uint32_t *, void *);
typedef int32_t (*size_t_fn)(uint32_t, const addr_t *, uint32_t, const void *, uint32_t *);
typedef uint8_t (*has_t)(uint32_t, const addr_t *);

static get_t get;
static size_t_fn gsize;
static has_t has;

#define FC(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))
#define GLOB FC('g', 'l', 'o', 'b')

static const char *
fcc(uint32_t v, char *b)
{
	for (int i = 0; i < 4; i++) {
		char c = (char)(v >> (24 - 8 * i));
		b[i] = c >= 32 && c < 127 ? c : '.';
	}
	b[4] = 0;
	return b;
}

static uint32_t
u32(uint32_t obj, uint32_t sel, uint32_t scope, uint32_t elem, int *ok)
{
	addr_t a = { sel, scope, elem };
	uint32_t v = 0, n = sizeof(v);
	*ok = has(obj, &a) && get(obj, &a, 0, NULL, &n, &v) == 0;
	return v;
}

static void
str(uint32_t obj, uint32_t sel, char *out, size_t len)
{
	addr_t a = { sel, GLOB, 0 };
	CFStringRef s = NULL;
	uint32_t n = sizeof(s);
	out[0] = 0;
	if (has(obj, &a) && get(obj, &a, 0, NULL, &n, &s) == 0 && s != NULL) {
		CFStringGetCString(s, out, (CFIndex)len, kCFStringEncodingUTF8);
		CFRelease(s);
	}
}

int
main(int argc, char **argv)
{
	void *h = dlopen("/System/Library/Frameworks/CoreAudio.framework/CoreAudio", RTLD_NOW);
	if (h != NULL) {
		get = (get_t)dlsym(h, "AudioObjectGetPropertyData");
		gsize = (size_t_fn)dlsym(h, "AudioObjectGetPropertyDataSize");
		has = (has_t)dlsym(h, "AudioObjectHasProperty");
	}
	if (get == NULL || gsize == NULL || has == NULL) {
		printf("auprobe: CoreAudio HAL client functions not found (%s)\n", dlerror());
		return 1;
	}
	addr_t devs = { FC('d', 'e', 'v', '#'), GLOB, 0 };
	uint32_t n = 0;
	uint32_t *ids;
	if (argc > 1) {
		n = (uint32_t)(argc - 1) * 4;
		ids = calloc((size_t)argc, 4);
		for (int a = 1; a < argc; a++) {
			ids[a - 1] = (uint32_t)strtoul(argv[a], NULL, 0);
		}
	} else {
		int32_t err = gsize(1, &devs, 0, NULL, &n);
		if (err != 0 || n == 0) {
			printf("auprobe: device list: error %d, %u bytes\n", err, n);
			return 1;
		}
		ids = calloc(n / 4, 4);
		get(1, &devs, 0, NULL, &n, ids);
	}
	for (uint32_t i = 0; i < n / 4; i++) {
		char uid[256], name[256], t[5];
		int ok;
		str(ids[i], FC('u', 'i', 'd', ' '), uid, sizeof(uid));
		str(ids[i], FC('l', 'n', 'a', 'm'), name, sizeof(name));
		uint32_t tran = u32(ids[i], FC('t', 'r', 'a', 'n'), GLOB, 0, &ok);
		printf("device %u: \"%s\" uid \"%s\" transport '%s'\n", ids[i], name, uid, fcc(tran, t));
		for (uint32_t e = 0; e <= 4; e++) {
			addr_t m = { FC('m', 'u', 't', 'e'), FC('o', 'u', 't', 'p'), e };
			uint32_t v = u32(ids[i], m.sel, m.scope, e, &ok);
			printf("  HasProperty(mute, outp, %u) = %d%s", e, has(ids[i], &m), ok ? "" : "\n");
			if (ok) {
				printf(", value %u\n", v);
			}
		}
		/* the other properties VirtualAudio's codec setup asks (research/audio/device-codec-construction-1635.md) */
		static const addr_t more[] = {
			{ FC('j', 'a', 'c', 'k'), FC('o', 'u', 't', 'p'), 0 },
			{ FC('v', 'o', 'l', 'm'), FC('o', 'u', 't', 'p'), 2 },
			{ FC('d', 'a', 'c', 'p'), FC('o', 'u', 't', 'p'), 0 },
			{ FC('m', 'u', 't', 'e'), FC('o', 'u', 't', 'p'), 5 },
			{ FC('s', 's', 'c', '#'), FC('i', 'n', 'p', 't'), 0 },	/* data sources (input): Device_Codec_Aspen */
			{ FC('s', 's', 'r', 'c'), FC('i', 'n', 'p', 't'), 0 },	/* current data source (input) */
			{ FC('s', 's', 'c', '#'), FC('o', 'u', 't', 'p'), 0 },
			{ FC('s', 's', 'r', 'c'), FC('o', 'u', 't', 'p'), 0 },
		};
		for (unsigned k = 0; k < sizeof(more) / sizeof(more[0]); k++) {
			char s1[5], s2[5];
			uint32_t v = u32(ids[i], more[k].sel, more[k].scope, more[k].elem, &ok);
			printf("  HasProperty(%s, %s, %u) = %d", fcc(more[k].sel, s1), fcc(more[k].scope, s2), more[k].elem,
			    has(ids[i], &more[k]));
			if (ok) {
				printf(", value %u", v);
			} else if (has(ids[i], &more[k])) {
				uint32_t sz = 0;
				int32_t e2 = gsize(ids[i], &more[k], 0, NULL, &sz);
				char s3[5];
				printf(", but GetPropertyDataSize -> %d '%s' (%u bytes)", e2, fcc((uint32_t)e2, s3), sz);
			}
			printf("\n");
		}
		addr_t own = { FC('o', 'w', 'n', 'd'), GLOB, 0 };
		uint32_t on = 0;
		if (has(ids[i], &own) && gsize(ids[i], &own, 0, NULL, &on) == 0 && on > 0) {
			uint32_t *objs = calloc(on / 4, 4);
			get(ids[i], &own, 0, NULL, &on, objs);
			for (uint32_t j = 0; j < on / 4; j++) {
				char c1[5], c2[5], c3[5];
				uint32_t cls = u32(objs[j], FC('c', 'l', 'a', 's'), GLOB, 0, &ok);
				uint32_t base = u32(objs[j], FC('b', 'c', 'l', 's'), GLOB, 0, &ok);
				uint32_t sc = u32(objs[j], FC('c', 's', 'c', 'p'), GLOB, 0, &ok);
				int sok = ok;
				uint32_t el = u32(objs[j], FC('c', 'e', 'l', 'm'), GLOB, 0, &ok);
				printf("  owns %u: class '%s' base '%s'", objs[j], fcc(cls, c1), fcc(base, c2));
				if (sok) {
					printf(" scope '%s' element %u", fcc(sc, c3), el);
				}
				printf("\n");
			}
			free(objs);
		}
	}
	return 0;
}
