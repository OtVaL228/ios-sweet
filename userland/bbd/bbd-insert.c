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
 * sweetglwait=N waits at most N seconds (default 180).
 *
 * backboardd's stdout/stderr go to /dev/null, which the kernel copies to the
 * console log for it (sweetnull, xnu 0048): lines show as
 * "|backboardd| SWEET BBD: ...".
 */
#include <IOKit/IOKitLib.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

#define GL_LIBRARY "/usr/lib/libsweet-gl.dylib"

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
	long patience = boot_arg("sweetglwait", 180);
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
}
