/*
 * sweet-sysapps: installs iOS's removable system apps (Calculator, Weather,
 * Notes, Maps, ...) from /private/var/staged_system_apps.
 *
 * On an iPhone this happens once after a restore: datamigrator runs Apple's
 * SystemAppMigrator plug-in, which hands each staged app to
 * installcoordinationd/installd. Our root skips data migration
 * (tools/sweet-root-tweaks.py writes LastSystemVersion: the whole migration
 * took an hour and repeated on every boot), so the apps stay staged and never
 * reach the home screen. This runs that one plug-in, from autorun.sh:
 *
 *   sweet-sysapps list               the staged apps (bundle IDs)
 *   sweet-sysapps migrate [FLAGS]    run SystemAppMigrator (user data
 *                                    disposition FLAGS, default 0x1), then list
 *
 * installd checks every app's signature unless the kernel says code signing
 * enforcement is off (boot-arg sweetcsoff=1, xnu 0134). The plug-in's XPC
 * peers check entitlements: sysapps/entitlements.plist has the install ones
 * of datamigrator's plug-in host (key names from its signature).
 *
 * Phone run sysapps1: performMigration -> 1 after 92 s, the 32 staged apps
 * moved out of staged_system_apps and appear on the home screen. The
 * "installed" column of list says "staged only" for everything: a command
 * line process gets no answers from LaunchServices here (lsd never sees its
 * requests), so only the staged directory itself tells. The plug-in's
 * os_log lines are not visible either (OS_ACTIVITY_DT_MODE does not mirror
 * them to stderr on our system).
 */
#import <Foundation/Foundation.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define STAGED          "/private/var/staged_system_apps"
#define DATA_MIGRATION  "/System/Library/PrivateFrameworks/DataMigration.framework/DataMigration"
#define MIGRATOR        @"/System/Library/DataClassMigrators/SystemAppMigrator.migrator"

/* DataMigration.framework's DataClassMigrator, MobileCoreServices' LSApplicationWorkspace */
@interface NSObject (SweetPrivate)
+ (id)dataClassMigratorForBundleAtPath:(NSString *)path;
- (void)setUserDataDisposition:(unsigned)flags;
- (unsigned)userDataDisposition;
- (BOOL)didUpgrade;
- (BOOL)performMigration;
+ (id)defaultWorkspace;
- (BOOL)applicationIsInstalled:(NSString *)bundleID;
@end

/* Bundle IDs of the staged apps, by .app name. */
static NSDictionary<NSString *, NSString *> *
staged_apps(void)
{
	NSMutableDictionary *apps = [NSMutableDictionary dictionary];
	NSArray *names = [[NSFileManager defaultManager] contentsOfDirectoryAtPath:@STAGED error:NULL];
	for (NSString *name in names) {
		NSString *info = [NSString stringWithFormat:@STAGED "/%@/Info.plist", name];
		NSString *bundleID = [NSDictionary dictionaryWithContentsOfFile:info][@"CFBundleIdentifier"];
		if ([bundleID isKindOfClass:[NSString class]]) {
			apps[name] = bundleID;
		}
	}
	return apps;
}

static void
list(void)
{
	NSDictionary *apps = staged_apps();
	id workspace = [NSClassFromString(@"LSApplicationWorkspace") defaultWorkspace];
	unsigned installed = 0;
	for (NSString *name in [[apps allKeys] sortedArrayUsingSelector:@selector(compare:)]) {
		BOOL is = [workspace applicationIsInstalled:apps[name]];
		installed += is;
		printf("SWEET SYSAPPS: %-24s %-36s %s\n", name.UTF8String, [apps[name] UTF8String],
		    is ? "installed" : "staged only");
	}
	printf("SWEET SYSAPPS: %u of %lu staged apps installed\n", installed, (unsigned long)apps.count);
}

static int
migrate(unsigned flags)
{
	if (dlopen(DATA_MIGRATION, RTLD_NOW) == NULL) {
		printf("SWEET SYSAPPS: %s\n", dlerror());
		return 1;
	}
	id migrator = [NSClassFromString(@"DataClassMigrator") dataClassMigratorForBundleAtPath:MIGRATOR];
	if (migrator == nil) {
		printf("SWEET SYSAPPS: no migrator from %s\n", MIGRATOR.UTF8String);
		return 1;
	}
	printf("SWEET SYSAPPS: %s: disposition %#x -> %#x, didUpgrade %d\n", object_getClassName(migrator),
	    [migrator userDataDisposition], flags, [migrator didUpgrade]);
	[migrator setUserDataDisposition:flags];
	fflush(stdout);
	time_t start = time(NULL);
	BOOL ok = [migrator performMigration];
	printf("SWEET SYSAPPS: performMigration -> %d after %ld s\n", ok, (long)(time(NULL) - start));
	return ok ? 0 : 1;
}

int
main(int argc, char *argv[])
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	@autoreleasepool {
		if (argc >= 2 && strcmp(argv[1], "list") == 0) {
			list();
			return 0;
		}
		if (argc >= 2 && strcmp(argv[1], "migrate") == 0) {
			unsigned flags = argc >= 3 ? (unsigned)strtoul(argv[2], NULL, 0) : 0x1;
			list();
			int status = migrate(flags);
			list();
			return status;
		}
	}
	fprintf(stderr, "usage: %s list | migrate [DISPOSITION]\n", argv[0]);
	return 2;
}
