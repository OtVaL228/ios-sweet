/*
 * libsweet-fpfs: lets fileproviderd keep its FPFS library on our HFS root. -[FPDVolume isEligibleForFPFS:]
 * (FileProviderDaemon.framework) takes a struct statfs and accepts only a local, writable volume whose
 * f_fstypename is "apfs" ("Non-eligible device /dev/ufsroot0 [...]: non-apfs"). With no eligible volume
 * fileproviderd loads no library and every domain (On My iPhone, iCloud Drive, Photos) stays
 * "url:(null) db:(null) state:disabled", so the Files app lists nothing. This answers yes for a local,
 * writable HFS volume too. Loaded by fpwrap through DYLD_INSERT_LIBRARIES. MIT, as userland/.
 */
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <os/log.h>
#include <string.h>
#include <sys/mount.h>

static BOOL (*eligible_orig)(id, SEL, struct statfs *);

static BOOL
eligible(id self, SEL cmd, struct statfs *st)
{
	if (eligible_orig(self, cmd, st))
		return YES;
	BOOL ok = st != NULL && !(st->f_flags & MNT_RDONLY) && (st->f_flags & MNT_LOCAL) &&
	    strcmp(st->f_fstypename, "hfs") == 0;
	os_log(OS_LOG_DEFAULT, "SWEET FPFS: %{public}s (%{public}s) %{public}s", st ? st->f_mntfromname : "?",
	    st ? st->f_fstypename : "?", ok ? "taken as eligible" : "left not eligible");
	return ok;
}

__attribute__((constructor)) static void
init(void)
{
	Class c = objc_getClass("FPDVolume");
	Method m = c ? class_getInstanceMethod(c, sel_registerName("isEligibleForFPFS:")) : NULL;
	if (m == NULL) {
		os_log_error(OS_LOG_DEFAULT, "SWEET FPFS: no -[FPDVolume isEligibleForFPFS:], nothing changed");
		return;
	}
	eligible_orig = (BOOL (*)(id, SEL, struct statfs *))method_setImplementation(m, (IMP)eligible);
	os_log(OS_LOG_DEFAULT, "SWEET FPFS: -[FPDVolume isEligibleForFPFS:] accepts HFS");
}
