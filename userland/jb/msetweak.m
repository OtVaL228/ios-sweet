/*
 * SweetMSE: Media Source Extensions in Safari, as WebKit has them on iPad. iOS 16 leaves WebKit's
 * MediaSourceEnabled off on an iPhone, so window.MediaSource is undefined and YouTube serves Safari only
 * its progressive 360p mp4 (2026-11-15: no isTypeSupported call, a 'src' URL, FigFilePlayer at ~300-580
 * kbps). MobileSafari sets the preference itself (its MediaSourceEnabled default, off on an iPhone) on
 * the WKPreferences it builds: this makes -[WKPreferences _setMediaSourceEnabled:] (WebKit SPI) always set
 * YES, and sets it on every configuration a WKWebView is made with. MSE then exists but answered
 * isTypeSupported false for H.264, HEVC, VP9 and AV1 (Opus, which WebKit parses itself, true): WebKit
 * admits a type of its MediaContentTypesRequiringHardwareSupport only where VTIsHardwareDecodeSupported
 * says so, and this phone decodes in software (xnu 0165). So that list is kept empty and the "as
 * fallback" preference on. An ElleKit tweak:
 * /var/jb/usr/lib/TweakInject/SweetMSE.dylib with SweetMSE.plist (Bundles: com.apple.mobilesafari).
 * MIT, as userland/.
 */
#import <Foundation/Foundation.h>
#import <objc/message.h>
#import <objc/runtime.h>
#include <os/log.h>

static id (*init_orig)(id, SEL, CGRect, id);
static void (*set_orig)(id, SEL, BOOL);

static void (*types_orig)(id, SEL, NSString *);

static void
set_media_source(id self, SEL cmd, BOOL on)
{
	(void)on;
	set_orig(self, cmd, YES);
}

static void
set_hardware_types(id self, SEL cmd, NSString *types)
{
	(void)types;
	types_orig(self, cmd, @"");
}

/* method's implementation replaced by imp; the old one, or NULL */
static IMP
hook(const char *cls, const char *sel, IMP imp)
{
	Method m = class_getInstanceMethod(objc_getClass(cls), sel_registerName(sel));
	return m ? method_setImplementation(m, imp) : NULL;
}

static id
init_with_configuration(id self, SEL cmd, CGRect frame, id configuration)
{
	id preferences = configuration ? ((id (*)(id, SEL))objc_msgSend)(configuration, sel_registerName("preferences")) : nil;
	SEL set = sel_registerName("_setMediaSourceEnabled:");
	SEL types = sel_registerName("_setMediaContentTypesRequiringHardwareSupport:");
	SEL fallback = sel_registerName("_setAllowMediaContentTypesRequiringHardwareSupportAsFallback:");
	if (preferences && [preferences respondsToSelector:set])	/* through set_media_source once hooked */
		((void (*)(id, SEL, BOOL))objc_msgSend)(preferences, set, YES);
	if (preferences && [preferences respondsToSelector:types])
		((void (*)(id, SEL, NSString *))objc_msgSend)(preferences, types, @"");
	if (preferences && [preferences respondsToSelector:fallback])
		((void (*)(id, SEL, BOOL))objc_msgSend)(preferences, fallback, YES);
	return init_orig(self, cmd, frame, configuration);
}

__attribute__((constructor)) static void
init(void)
{
	Class c = objc_getClass("WKWebView");
	Method m = c ? class_getInstanceMethod(c, sel_registerName("initWithFrame:configuration:")) : NULL;
	if (m == NULL) {
		os_log_error(OS_LOG_DEFAULT, "SWEET MSE: no -[WKWebView initWithFrame:configuration:], nothing changed");
		return;
	}
	init_orig = (id (*)(id, SEL, CGRect, id))method_setImplementation(m, (IMP)init_with_configuration);
	set_orig = (void (*)(id, SEL, BOOL))hook("WKPreferences", "_setMediaSourceEnabled:", (IMP)set_media_source);
	types_orig = (void (*)(id, SEL, NSString *))hook("WKPreferences", "_setMediaContentTypesRequiringHardwareSupport:",
	    (IMP)set_hardware_types);
	if (set_orig == NULL || types_orig == NULL)
		os_log_error(OS_LOG_DEFAULT, "SWEET MSE: a WKPreferences setter is missing (%d %d)", set_orig != NULL,
		    types_orig != NULL);
	os_log(OS_LOG_DEFAULT, "SWEET MSE: Media Source enabled for new web views");
}
