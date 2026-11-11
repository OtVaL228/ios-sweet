/*
 * libsweet-autrace.dylib: logs CoreAudio HAL property calls that FAIL inside the process it is inserted into
 * (mediaserverd, via DYLD_INSERT_LIBRARIES on its launchd job: tools/sweet-root-tweaks.py --job-env), to find which
 * property VirtualAudio's Device_Codec_Aspen asks our device for and gets 'who?' (research/audio/README.md 23:20).
 * VirtualAudio.plugin is not in the shared cache, so its calls into CoreAudio go through dyld binds, which
 * interposing replaces. One line per failure, via os_log (oslog shows it):
 *   SWEET AUTRACE: Get obj 33 'ssc#'/'inpt'/0 -> 'who?' (VirtualAudio+0x23ff10)
 * Successful calls are not logged, except HasProperty on objects >= 30 when AUTRACE_HAS is set in the environment.
 * MIT, as userland/.
 */
#include <dlfcn.h>
#include <os/log.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

typedef struct { uint32_t sel, scope, elem; } addr_t;
typedef int32_t OSStatus;

extern OSStatus AudioObjectGetPropertyData(uint32_t, const addr_t *, uint32_t, const void *, uint32_t *, void *);
extern OSStatus AudioObjectGetPropertyDataSize(uint32_t, const addr_t *, uint32_t, const void *, uint32_t *);
extern OSStatus AudioObjectSetPropertyData(uint32_t, const addr_t *, uint32_t, const void *, uint32_t, const void *);
extern uint8_t AudioObjectHasProperty(uint32_t, const addr_t *);

static os_log_t lg;

static void
fcc(uint32_t v, char *b)
{
	for (int i = 0; i < 4; i++) {
		char c = (char)(v >> (24 - 8 * i));
		b[i] = c >= 32 && c < 127 ? c : '.';
	}
	b[4] = 0;
}

static void
report(const char *what, uint32_t obj, const addr_t *a, int32_t st, void *ret)
{
	char s[5] = "?", sc[5] = "?", e[5] = "", where[96] = "?";
	if (a != NULL) {
		fcc(a->sel, s);
		fcc(a->scope, sc);
	}
	fcc((uint32_t)st, e);
	Dl_info di;
	if (dladdr(ret, &di) && di.dli_fname != NULL) {
		const char *b = strrchr(di.dli_fname, '/');
		snprintf(where, sizeof(where), "%s+%#lx", b ? b + 1 : di.dli_fname,
		    (unsigned long)((uintptr_t)ret - (uintptr_t)di.dli_fbase));
	}
	if (lg == NULL) {
		lg = os_log_create("com.sweet.autrace", "hal");
	}
	os_log_error(lg, "SWEET AUTRACE: %{public}s obj %u '%{public}s'/'%{public}s'/%u -> %d '%{public}s' (%{public}s)", what,
	    obj, s, sc, a ? a->elem : 0, st, e, where);
}

static OSStatus
my_get(uint32_t o, const addr_t *a, uint32_t qs, const void *q, uint32_t *sz, void *d)
{
	OSStatus st = AudioObjectGetPropertyData(o, a, qs, q, sz, d);
	if (st != 0) {
		report("Get", o, a, st, __builtin_return_address(0));
	}
	return st;
}

static OSStatus
my_size(uint32_t o, const addr_t *a, uint32_t qs, const void *q, uint32_t *sz)
{
	OSStatus st = AudioObjectGetPropertyDataSize(o, a, qs, q, sz);
	if (st != 0) {
		report("Size", o, a, st, __builtin_return_address(0));
	}
	return st;
}

static OSStatus
my_set(uint32_t o, const addr_t *a, uint32_t qs, const void *q, uint32_t sz, const void *d)
{
	OSStatus st = AudioObjectSetPropertyData(o, a, qs, q, sz, d);
	if (st != 0) {
		report("Set", o, a, st, __builtin_return_address(0));
	}
	return st;
}

static uint8_t
my_has(uint32_t o, const addr_t *a)
{
	uint8_t r = AudioObjectHasProperty(o, a);
	static int verbose = -1;
	if (verbose < 0) {
		verbose = getenv("AUTRACE_HAS") != NULL;
	}
	if (verbose && o >= 30) {
		report(r ? "Has=1" : "Has=0", o, a, 0, __builtin_return_address(0));
	}
	return r;
}

extern const char *getprogname(void);

__attribute__((constructor)) static void
loaded(void)
{
	lg = os_log_create("com.sweet.autrace", "hal");
	os_log_error(lg, "SWEET AUTRACE: loaded into %{public}s", getprogname());
}

#define INTERPOSE(n, o) \
	__attribute__((used)) static const struct { const void *r, *orig; } interpose_##n \
	__attribute__((section("__DATA,__interpose"))) = { (const void *)(n), (const void *)(o) }
INTERPOSE(my_get, AudioObjectGetPropertyData);
INTERPOSE(my_size, AudioObjectGetPropertyDataSize);
INTERPOSE(my_set, AudioObjectSetPropertyData);
INTERPOSE(my_has, AudioObjectHasProperty);
