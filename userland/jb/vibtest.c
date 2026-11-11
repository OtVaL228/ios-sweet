/*
 * vibtest: plays vibrations through iOS's own system-sound path (AudioToolbox -> mediaserverd SSServer ->
 * FigVibrator -> the kernel's "vibrator"), so the haptics path can be tested without the UI.
 *   vibtest              kSystemSoundID_Vibrate (4095)
 *   vibtest ID           system sound ID ID (1519 peek, 1520 pop, 1521 nope, 1102/1107 ..)
 *   vibtest pat MS...    a custom legacy pattern: on, off, on ... ms at full intensity
 *                        (AudioServicesPlaySystemSoundWithVibration, the private API UIKit-era apps used)
 *   vibtest stop ID      AudioServicesStopSystemSound(ID)
 *   vibtest io [MS]      straight to the kernel, as FigVibrator does: IOServiceNameMatching("vibrator") then
 *                        IORegistryEntrySetCFProperties { intensity, hertz_millisecs [on MS, off 100], repeat no }
 *                        (says whether the service exists at all; skips mediaserverd)
 * Each request should show up as a "SWEET VIBRATOR: request" line in dmesg. MIT, as userland/.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <AudioToolbox/AudioToolbox.h>
#include <IOKit/IOKitLib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern void AudioServicesPlaySystemSoundWithVibration(SystemSoundID id, const void *unused, CFDictionaryRef pattern);
/* the SDK marks it unavailable on iOS; IOKit exports it (FigVibrator calls it) */
extern kern_return_t vib_set_props(io_registry_entry_t entry, CFTypeRef properties) __asm__("_IORegistryEntrySetCFProperties");
extern void AudioServicesStopSystemSound(SystemSoundID id, Boolean now) __attribute__((weak_import));

static void
done(SystemSoundID id, void *ctx)
{
	(void)id;
	*(int *)ctx = 1;
}

int
main(int argc, char **argv)
{
	SystemSoundID sid = 4095;
	if (argc >= 3 && strcmp(argv[1], "stop") == 0) {
		if (AudioServicesStopSystemSound == NULL) {
			printf("vibtest: AudioServicesStopSystemSound not exported\n");
			return 1;
		}
		AudioServicesStopSystemSound((SystemSoundID)strtoul(argv[2], NULL, 0), true);
		printf("vibtest: stop %s sent\n", argv[2]);
		return 0;
	}
	if (argc >= 2 && strcmp(argv[1], "io") == 0) {
		io_service_t s = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceNameMatching("vibrator"));
		if (s == IO_OBJECT_NULL) {
			printf("vibtest: no IOService named \"vibrator\"\n");
			return 1;
		}
		int ms = argc >= 3 ? atoi(argv[2]) : 30, off = 100;
		SInt32 level = 65536;
		CFNumberRef in = CFNumberCreate(NULL, kCFNumberSInt32Type, &level);
		const void *ik[] = { CFSTR("intensity") }, *iv[] = { in };
		CFDictionaryRef idict = CFDictionaryCreate(NULL, ik, iv, 1, &kCFTypeDictionaryKeyCallBacks,
		    &kCFTypeDictionaryValueCallBacks);
		CFNumberRef on_ms = CFNumberCreate(NULL, kCFNumberIntType, &ms), off_ms = CFNumberCreate(NULL, kCFNumberIntType, &off);
		const void *pv[] = { idict, on_ms, kCFBooleanFalse, off_ms };
		CFArrayRef pat = CFArrayCreate(NULL, pv, 4, &kCFTypeArrayCallBacks);
		const void *k[] = { CFSTR("intensity"), CFSTR("hertz_millisecs"), CFSTR("repeat") }, *v[] = { in, pat, kCFBooleanFalse };
		CFDictionaryRef d = CFDictionaryCreate(NULL, k, v, 3, &kCFTypeDictionaryKeyCallBacks,
		    &kCFTypeDictionaryValueCallBacks);
		kern_return_t kr = vib_set_props(s, d);
		printf("vibtest: \"vibrator\" found, %d ms request: 0x%x\n", ms, kr);
		return kr != KERN_SUCCESS;
	}
	int finished = 0;
	if (argc >= 3 && strcmp(argv[1], "pat") == 0) {
		CFMutableArrayRef a = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
		for (int i = 2; i < argc; i++) {
			int ms = atoi(argv[i]);
			CFArrayAppendValue(a, (i % 2 == 0) ? kCFBooleanTrue : kCFBooleanFalse);
			CFNumberRef n = CFNumberCreate(NULL, kCFNumberIntType, &ms);
			CFArrayAppendValue(a, n);
			CFRelease(n);
		}
		float one = 1.0f;
		CFNumberRef in = CFNumberCreate(NULL, kCFNumberFloatType, &one);
		const void *k[] = { CFSTR("VibePattern"), CFSTR("Intensity") }, *v[] = { a, in };
		CFDictionaryRef d = CFDictionaryCreate(NULL, k, v, 2, &kCFTypeDictionaryKeyCallBacks,
		    &kCFTypeDictionaryValueCallBacks);
		AudioServicesAddSystemSoundCompletion(4095, NULL, NULL, done, &finished);
		AudioServicesPlaySystemSoundWithVibration(4095, NULL, d);
		printf("vibtest: pattern of %d segment(s) sent\n", argc - 2);
	} else {
		if (argc >= 2)
			sid = (SystemSoundID)strtoul(argv[1], NULL, 0);
		AudioServicesAddSystemSoundCompletion(sid, NULL, NULL, done, &finished);
		AudioServicesPlaySystemSound(sid);
		printf("vibtest: system sound %u sent\n", sid);
	}
	for (int i = 0; i < 50 && !finished; i++)
		CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.1, false);
	printf("vibtest: %s\n", finished ? "completed" : "no completion within 5 s");
	return 0;
}
