/*
 * prefs: read and write the mobile user's preferences through cfprefsd (iOS has no `defaults`).
 *   prefs DOMAIN KEY                  print
 *   prefs DOMAIN KEY bool|int|str V   set
 *   prefs DOMAIN KEY del              remove
 * DOMAIN -g = global (.GlobalPreferences). As ios-flame's prefs tool is used (WebKit2GPUProcessForDOMRendering).
 */
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
	if (argc != 3 && argc != 4 && argc != 5) {
		fprintf(stderr, "usage: prefs DOMAIN|-g KEY [bool|int|str VALUE | del]\n");
		return 2;
	}
	CFStringRef app = strcmp(argv[1], "-g") ? CFStringCreateWithCString(NULL, argv[1], kCFStringEncodingUTF8) :
	    kCFPreferencesAnyApplication;
	CFStringRef key = CFStringCreateWithCString(NULL, argv[2], kCFStringEncodingUTF8);
	CFStringRef user = CFSTR("mobile");
	if (argc >= 4) {
		CFPropertyListRef v = NULL;
		if (argc == 4 && strcmp(argv[3], "del") == 0) {
			v = NULL;
		} else if (argc == 5 && strcmp(argv[3], "bool") == 0) {
			v = (!strcmp(argv[4], "YES") || !strcmp(argv[4], "1") || !strcmp(argv[4], "true")) ? kCFBooleanTrue :
			    kCFBooleanFalse;
		} else if (argc == 5 && strcmp(argv[3], "int") == 0) {
			long long n = atoll(argv[4]);
			v = CFNumberCreate(NULL, kCFNumberLongLongType, &n);
		} else if (argc == 5 && strcmp(argv[3], "str") == 0) {
			v = CFStringCreateWithCString(NULL, argv[4], kCFStringEncodingUTF8);
		} else {
			fprintf(stderr, "prefs: bad type\n");
			return 2;
		}
		CFPreferencesSetValue(key, v, app, user, kCFPreferencesAnyHost);
		if (!CFPreferencesSynchronize(app, user, kCFPreferencesAnyHost)) {
			fprintf(stderr, "prefs: synchronize failed\n");
			return 1;
		}
	}
	CFPropertyListRef cur = CFPreferencesCopyValue(key, app, user, kCFPreferencesAnyHost);
	if (cur == NULL) {
		printf("%s: (none)\n", argv[2]);
	} else {
		CFStringRef d = CFCopyDescription(cur);
		char buf[512];
		CFStringGetCString(d, buf, sizeof(buf), kCFStringEncodingUTF8);
		printf("%s: %s\n", argv[2], buf);
	}
	return 0;
}
