/*
 * libsweet-bbd.dylib: our code inside backboardd (the window server and
 * CoreAnimation's render server), put there by /usr/libexec/sweet-bbd.
 *
 * G4 step 3: CoreAnimation picks its renderer once, when the window server
 * starts: OpenGL ES if an EAGLContext can be made, else its software
 * rasterizer. /usr/lib/libsweet-gl.dylib (userland/gpu: eagl.c from HackiOS
 * via topaz, on our Mesa freedreno) makes EAGLContext work, but only once the
 * kernel has published the GPU (RedwoodGPU, sweetgpu=11), which happens some
 * 70 s after backboardd starts. So this constructor, which runs before
 * backboardd's main(), waits for the service and then loads the library.
 *
 * Boot-args: sweetgl=0 never loads it (software rendering as before),
 * sweetglwait=N waits at most N seconds (default 120), sweetglfn=0 leaves
 * CoreAnimation's GLESContext alone (see hook_gles_context()).
 *
 * backboardd's stdout/stderr go to /dev/null, which the kernel copies to the
 * console log for it (sweetnull, xnu 0048): lines show as
 * "|backboardd| SWEET BBD: ...".
 */
#include <IOKit/IOKitLib.h>
#include <IOSurface/IOSurfaceRef.h>
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

#define GL_LIBRARY "/usr/lib/libsweet-gl.dylib"

static long boot_arg(const char *name, long dfault);
static intptr_t quartzcore_slide(void);

/*
 * Boot-arg sweetlockwho=1 (debug): who in backboardd locks and unlocks IOSurfaces that have no
 * pixel format, and how many of their bytes are set by the unlock. G4 runs gl9/gl10: the home
 * screen's icons are 180 x 180 such surfaces (CoreAnimation's deferred image slots) that
 * backboardd locks for writing and unlocks with nothing written. IOSurfaceLock/Unlock are
 * interposed (dyld allows it since xnu 0121); the chain is unslid by QuartzCore's slide.
 */
static int lock_who = -1;

static void
lock_report(const char *what, IOSurfaceRef surface, uint32_t options)
{
	static unsigned told;
	if (lock_who < 0) {
		lock_who = boot_arg("sweetlockwho", 0) != 0;
	}
	if (!lock_who || surface == NULL || IOSurfaceGetPixelFormat(surface) != 0 || told >= 60) {
		return;
	}
	told++;
	const uint8_t *base = IOSurfaceGetBaseAddress(surface);
	size_t size = IOSurfaceGetBytesPerRow(surface) * IOSurfaceGetHeight(surface), set = 0;
	for (size_t i = 0; base != NULL && i < size; i++) {
		set += base[i] != 0;
	}
	intptr_t slide = quartzcore_slide();
	uintptr_t *fp = __builtin_frame_address(0);
	char chain[256];
	size_t used = 0;
	chain[0] = 0;
	for (unsigned depth = 0; depth < 8 && fp != NULL && used < sizeof(chain); depth++) {
		used += (size_t)snprintf(chain + used, sizeof(chain) - used, " %#lx", (unsigned long)(fp[1] - (uintptr_t)slide));
		fp = (uintptr_t *)fp[0];
	}
	fprintf(stderr, "SWEET BBD: %s surface %u %zux%zu options %#x: %zu of %zu bytes set; from%s\n", what,
	    IOSurfaceGetID(surface), IOSurfaceGetWidth(surface), IOSurfaceGetHeight(surface), options, set, size, chain);
}

static kern_return_t
my_IOSurfaceLock(IOSurfaceRef surface, IOSurfaceLockOptions options, uint32_t *seed)
{
	kern_return_t ret = IOSurfaceLock(surface, options, seed);
	lock_report("lock", surface, options);
	return ret;
}

static kern_return_t
my_IOSurfaceUnlock(IOSurfaceRef surface, IOSurfaceLockOptions options, uint32_t *seed)
{
	lock_report("unlock", surface, options);
	return IOSurfaceUnlock(surface, options, seed);
}

__attribute__((used)) static const struct {
	const void *replacement;
	const void *replacee;
} interposers[] __attribute__((section("__DATA,__interpose"))) = {
	{ (const void *)my_IOSurfaceLock, (const void *)IOSurfaceLock },
	{ (const void *)my_IOSurfaceUnlock, (const void *)IOSurfaceUnlock },
};

/*
 * CoreAnimation's OpenGL ES renderer gives a "function" (a filter such as
 * vibrantColorMatrix) its numbers through GLContext::function_uniform(), and
 * asks GLContext::function_supported() first. For the functions it has no
 * shader for the numbers go nowhere; eagl.c draws some of them itself once
 * it is told them (redwood_gl_function_values(), redwood_gl_function_refused()).
 * Without that the lock screen's vibrantColorMatrix layer is drawn as a
 * picture: a red veil over the wallpaper (G4 run gl4).
 *
 * Both are virtual: their entries in the vtable of CA::OGL::GLESContext are
 * replaced. Addresses of iOS 16.7.16 (20H392, iPhone10,3) QuartzCore, read
 * off the IPSW's dyld cache (tools/check-eagl-slots.py setup): vtable
 * 0x1d5b2f6e8, entries at +0x10 +0x378 (function_supported, 0x1884b2164) and
 * +0x388 (function_uniform, 0x1884b21ac); the current function is the byte at
 * [[context + 0x10] + 0x10] (function_supported reads it so). The scheme is
 * HackiOS's (inject.c, MIT); every entry is checked before it is touched.
 */
#define QC_VT_GLES_CONTEXT      0x1d5b2f6e8UL
#define QC_FUNCTION_SUPPORTED   0x1884b2164UL
#define QC_FUNCTION_UNIFORM     0x1884b21acUL
#define QC_SLOT(offset)         (QC_VT_GLES_CONTEXT + 0x10 + (offset))

static int (*orig_function_supported)(void *context);
static void (*orig_function_uniform)(void *context, unsigned index, size_t components, size_t count,
    const float *values);
static void (*gl_function_values)(void *context, unsigned function, unsigned index, size_t components,
    size_t count, const float *values);
static void (*gl_function_refused)(unsigned function);

static unsigned
current_function(void *context)
{
	return *(*(uint8_t **)((char *)context + 0x10) + 0x10);
}

static int
my_function_supported(void *context)
{
	int supported = orig_function_supported(context);
	if (!supported) {
		gl_function_refused(current_function(context));
	}
	return supported;
}

static void
my_function_uniform(void *context, unsigned index, size_t components, size_t count, const float *values)
{
	gl_function_values(context, current_function(context), index, components, count, values);
	orig_function_uniform(context, index, components, count, values);
}

/* QuartzCore's slide (the dyld cache's). */
static intptr_t
quartzcore_slide(void)
{
	for (uint32_t i = 0; i < _dyld_image_count(); i++) {
		const char *name = _dyld_get_image_name(i);
		if (name && strstr(name, "/QuartzCore.framework/QuartzCore")) {
			return _dyld_get_image_vmaddr_slide(i);
		}
	}
	return -1;
}

/* Swaps one vtable entry if it holds what we expect; returns the old one or NULL. */
static void *
replace_entry(const char *what, intptr_t slide, uintptr_t slot, uintptr_t expected, void *ours)
{
	uintptr_t *entry = (uintptr_t *)(slot + slide);
	if (*entry != expected + slide) {
		fprintf(stderr, "SWEET BBD: %s: entry holds %#lx, want %#lx: left alone\n", what,
		    (unsigned long)(*entry - slide), (unsigned long)expected);
		return NULL;
	}
	vm_address_t page = (vm_address_t)entry & ~(vm_address_t)(vm_page_size - 1);
	if (vm_protect(mach_task_self(), page, vm_page_size, FALSE,
	    VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY) != KERN_SUCCESS) {
		fprintf(stderr, "SWEET BBD: %s: vtable page not writable\n", what);
		return NULL;
	}
	void *old = (void *)*entry;
	*entry = (uintptr_t)ours;
	vm_protect(mach_task_self(), page, vm_page_size, FALSE, VM_PROT_READ);
	return old;
}

static void
hook_gles_context(void *gl)
{
	gl_function_values = (void (*)(void *, unsigned, unsigned, size_t, size_t, const float *))dlsym(gl,
	    "redwood_gl_function_values");
	gl_function_refused = (void (*)(unsigned))dlsym(gl, "redwood_gl_function_refused");
	intptr_t slide = quartzcore_slide();
	if (gl_function_values == NULL || gl_function_refused == NULL || slide == -1) {
		fprintf(stderr, "SWEET BBD: GLESContext hooks: nothing to hook with (slide %ld)\n", (long)slide);
		return;
	}
	orig_function_supported = (int (*)(void *))replace_entry("function_supported", slide,
	    QC_SLOT(0x378), QC_FUNCTION_SUPPORTED, (void *)my_function_supported);
	orig_function_uniform = (void (*)(void *, unsigned, size_t, size_t, const float *))replace_entry(
	    "function_uniform", slide, QC_SLOT(0x388), QC_FUNCTION_UNIFORM, (void *)my_function_uniform);
	fprintf(stderr, "SWEET BBD: GLESContext hooks (slide %#lx): function_supported %s, function_uniform %s\n",
	    (unsigned long)slide, orig_function_supported ? "on" : "OFF", orig_function_uniform ? "on" : "OFF");
}

/* Value of "name=" in the boot-args, or dfault. */
static long
boot_arg(const char *name, long dfault)
{
	char args[1024], key[64];
	size_t len = sizeof(args) - 1;
	if (sysctlbyname("kern.bootargs", args, &len, NULL, 0) != 0) {
		return dfault;
	}
	args[len] = '\0';
	snprintf(key, sizeof(key), "%s=", name);
	for (char *p = strstr(args, key); p; p = strstr(p + 1, key)) {
		if (p == args || p[-1] == ' ') {
			return strtol(p + strlen(key), NULL, 10);
		}
	}
	return dfault;
}

__attribute__((constructor)) static void
sweet_bbd_loaded(void)
{
	fprintf(stderr, "SWEET BBD: library loaded into pid %d (uid %d)\n", getpid(), getuid());
	if (boot_arg("sweetgl", 1) == 0) {
		fprintf(stderr, "SWEET BBD: sweetgl=0: CoreAnimation keeps its software renderer\n");
		return;
	}
	if (access(GL_LIBRARY, R_OK) != 0) {
		fprintf(stderr, "SWEET BBD: no " GL_LIBRARY ": software renderer\n");
		return;
	}
	long patience = boot_arg("sweetglwait", 120);
	long waited = 0;
	io_service_t gpu;
	while (!(gpu = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("RedwoodGPU")))) {
		if (waited == 0) {
			fprintf(stderr, "SWEET BBD: waiting up to %ld s for RedwoodGPU\n", patience);
		}
		if (waited++ >= patience) {
			fprintf(stderr, "SWEET BBD: no RedwoodGPU after %ld s: software renderer\n", patience);
			return;
		}
		sleep(1);
	}
	IOObjectRelease(gpu);
	void *gl = dlopen(GL_LIBRARY, RTLD_NOW);
	fprintf(stderr, "SWEET BBD: RedwoodGPU after %ld s; " GL_LIBRARY " %s%s\n", waited,
	    gl ? "loaded" : "NOT loaded: ", gl ? "" : dlerror());
	if (gl != NULL && boot_arg("sweetglfn", 1) != 0) {
		hook_gles_context(gl);
	}
}
