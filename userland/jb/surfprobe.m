/*
 * surfprobe: why +[IOSurface bs_IOSurfaceWithWidth:height:options:] (BaseBoard) returns nil in PosterBoard, which then
 * snapshots the wallpaper over no surface (black lock and home screen).
 *
 * The helper (research/posterboard/BaseBoard-IOSurface-helper.txt) takes integer width and height and a POINTER to a
 * 64-bit options word (PosterBoard: &1 = plain 4 bytes a pixel), and returns nil at once when
 * IOSurfaceGetPropertyMaximum(kIOSurfaceWidth) < width or IOSurfaceGetPropertyMaximum(kIOSurfaceHeight) < height;
 * otherwise [[IOSurface alloc] initWithProperties:] with width, height, a cache mode, aligned row bytes and size.
 * This prints those maxima (and alignments), the IOSurfaceRoot service's registry properties (where the framework
 * gets them), then calls the helper as PosterBoard does. Creates and frees a few small surfaces; nothing else.
 *   surfprobe [width height]      (default 375 811, PosterBoard's snapshot)
 * MIT, as userland/.
 */
#import <Foundation/Foundation.h>
#import <IOKit/IOKitLib.h>
#import <IOSurface/IOSurfaceRef.h>
#import <objc/message.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>

#define SAY(...) do { printf(__VA_ARGS__); printf("\n"); fflush(stdout); } while (0)

static void describe(const char *what, IOSurfaceRef s)
{
	if (s == NULL) {
		SAY("%-40s nil", what);
		return;
	}
	uint32_t f = IOSurfaceGetPixelFormat(s);
	SAY("%-40s id %#x %zux%zu '%c%c%c%c' row %zu alloc %zu planes %zu", what, IOSurfaceGetID(s), IOSurfaceGetWidth(s),
	    IOSurfaceGetHeight(s), (char)(f >> 24), (char)(f >> 16), (char)(f >> 8), (char)f, IOSurfaceGetBytesPerRow(s),
	    IOSurfaceGetAllocSize(s), IOSurfaceGetPlaneCount(s));
}

int main(int argc, char **argv)
{
	@autoreleasepool {
		unsigned long w = argc == 3 ? strtoul(argv[1], NULL, 0) : 375, h = argc == 3 ? strtoul(argv[2], NULL, 0) : 811;

		/* what the helper checks first */
		struct { const char *name; CFStringRef key; } keys[] = {
			{ "Width", kIOSurfaceWidth }, { "Height", kIOSurfaceHeight }, { "BytesPerRow", kIOSurfaceBytesPerRow },
			{ "BytesPerElement", kIOSurfaceBytesPerElement }, { "AllocSize", kIOSurfaceAllocSize },
		};
		for (unsigned i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
			SAY("IOSurfaceGetPropertyMaximum(%s) = %zu, alignment %zu", keys[i].name,
			    IOSurfaceGetPropertyMaximum(keys[i].key), IOSurfaceGetPropertyAlignment(keys[i].key));

		/* the kernel service the framework reads them from */
		io_service_t root = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("IOSurfaceRoot"));
		if (root == MACH_PORT_NULL) {
			SAY("IOSurfaceRoot: no such service");
		} else {
			CFMutableDictionaryRef props = NULL;
			kern_return_t kr = IORegistryEntryCreateCFProperties(root, &props, kCFAllocatorDefault, 0);
			io_name_t cls = "";
			IOObjectGetClass(root, cls);
			SAY("IOSurfaceRoot (class %s) properties (kr %#x):", cls, kr);
			if (props != NULL) {
				NSDictionary *registryProps = (__bridge NSDictionary *)props;
				NSArray *propertyKeys = [[registryProps allKeys] sortedArrayUsingSelector:@selector(compare:)];
				for (NSString *k in propertyKeys)
					SAY("  %s = %s", k.UTF8String, [[registryProps objectForKey:k] description].UTF8String);
				CFRelease(props);
			}
			IOObjectRelease(root);
		}

		/* the plain way, as a reference */
		NSDictionary *plainProps = @{ (id)kIOSurfaceWidth: @(w), (id)kIOSurfaceHeight: @(h), (id)kIOSurfaceBytesPerElement: @4,
			(id)kIOSurfacePixelFormat: @((uint32_t)'BGRA') };
		IOSurfaceRef plain = IOSurfaceCreate((__bridge CFDictionaryRef)plainProps);
		describe("IOSurfaceCreate BGRA", plain);
		if (plain)
			CFRelease(plain);

		/* the helper, as PosterBoard calls it */
		void *bb = dlopen("/System/Library/PrivateFrameworks/BaseBoard.framework/BaseBoard", RTLD_NOW);
		SAY("BaseBoard %s", bb ? "loaded" : dlerror());
		Class cls = NSClassFromString(@"IOSurface");
		SEL sel = sel_registerName("bs_IOSurfaceWithWidth:height:options:");
		if (![cls respondsToSelector:sel]) {
			SAY("+[IOSurface bs_IOSurfaceWithWidth:height:options:] MISSING");
			return 0;
		}
		unsigned long sizes[3][2] = { { w, h }, { 1125, 2436 }, { 64, 64 } };
		for (int z = 0; z < 3; z++) {
			uint64_t options = 1;
			char what[64];
			snprintf(what, sizeof(what), "bs_IOSurface %lux%lu options &1", sizes[z][0], sizes[z][1]);
			id s = ((id (*)(id, SEL, unsigned long, unsigned long, uint64_t *))objc_msgSend)(cls, sel, sizes[z][0],
			    sizes[z][1], &options);
			describe(what, (__bridge IOSurfaceRef)s);
		}
	}
	return 0;
}
