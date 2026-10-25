/*
 * osmesa-triangle: G3, the first picture drawn by the Adreno 640 under iOS.
 *
 * Makes an OSMesa context (Mesa built by userland/mesa/mk-mesa.sh: freedreno
 * on the RedwoodGPU user client, softpipe when OSMESA_SOFTWARE=1 or no GPU),
 * clears a 128x128 RGBA buffer to blue, draws one red triangle, glFinish(),
 * then checks pixels and prints the picture as text (the log is the only
 * screen we have for it). Exit status 0 when the renderer is freedreno and the
 * pixels are right.
 *
 * Runs from autorun.sh next to libOSMesa.8.dylib in /private/var/tmp.
 */
#include <GL/gl.h>
#include <GL/osmesa.h>
#include <mach/mach_time.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define W 128
#define H 128

static uint32_t pixels[W * H];

static uint64_t
us_since(uint64_t t0)
{
	mach_timebase_info_data_t tb;
	mach_timebase_info(&tb);
	return (mach_absolute_time() - t0) * tb.numer / tb.denom / 1000;
}

/* Bytes R, G, B, A in memory (OSMESA_RGBA, GL_UNSIGNED_BYTE). */
static char
classify(uint32_t p)
{
	uint8_t r = p & 0xff, g = (p >> 8) & 0xff, b = (p >> 16) & 0xff;
	if (r > 200 && g < 60 && b < 60) {
		return '#';                  /* the triangle */
	}
	if (b > 200 && r < 60 && g < 60) {
		return '.';                  /* the clear colour */
	}
	return '?';
}

int
main(void)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	/*
	 * Without the RedwoodGPU service OSMesa quietly falls back to softpipe:
	 * run redwood-gpu-test first (it waits for the service), and check
	 * GL_RENDERER below.
	 */
	const int attribs[] = {
		OSMESA_FORMAT, OSMESA_RGBA,
		OSMESA_DEPTH_BITS, 0,
		OSMESA_PROFILE, OSMESA_COMPAT_PROFILE,
		OSMESA_CONTEXT_MAJOR_VERSION, 2,
		OSMESA_CONTEXT_MINOR_VERSION, 1,
		0,
	};
	uint64_t t0 = mach_absolute_time();
	OSMesaContext ctx = OSMesaCreateContextAttribs(attribs, NULL);
	if (!ctx) {
		printf("OSMesaCreateContextAttribs failed\n");
		return 2;
	}
	if (!OSMesaMakeCurrent(ctx, pixels, GL_UNSIGNED_BYTE, W, H)) {
		printf("OSMesaMakeCurrent failed\n");
		return 2;
	}
	const char *renderer = (const char *)glGetString(GL_RENDERER);
	printf("context in %llu us: GL_VENDOR %s, GL_RENDERER %s, GL_VERSION %s\n", us_since(t0),
	    glGetString(GL_VENDOR), renderer, glGetString(GL_VERSION));

	t0 = mach_absolute_time();
	glViewport(0, 0, W, H);
	glClearColor(0.0f, 0.0f, 1.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	glBegin(GL_TRIANGLES);
	glColor3f(1.0f, 0.0f, 0.0f);
	glVertex2f(-0.75f, -0.75f);
	glVertex2f(0.75f, -0.75f);
	glVertex2f(0.0f, 0.75f);
	glEnd();
	glFinish();
	printf("drawn in %llu us, glGetError %#x\n", us_since(t0), glGetError());

	/* OSMesa rows go bottom-up by default: row 0 is the bottom of the picture. */
	char want_center = classify(pixels[(H / 2) * W + W / 2]);
	char want_corner = classify(pixels[(H - 1) * W + 0]);
	char want_bottom_mid = classify(pixels[(H / 8 + 2) * W + W / 2]);
	unsigned tri = 0, clear = 0, other = 0;
	for (unsigned i = 0; i < W * H; i++) {
		char c = classify(pixels[i]);
		tri += c == '#';
		clear += c == '.';
		other += c == '?';
	}
	printf("pixels: %u triangle, %u clear, %u other; center %c (%#010x), top-left %c, near bottom %c\n",
	    tri, clear, other, want_center, pixels[(H / 2) * W + W / 2], want_corner, want_bottom_mid);

	for (int y = H - 1; y >= 0; y -= 8) {
		char line[W / 4 + 1];
		for (int x = 0; x < W; x += 4) {
			line[x / 4] = classify(pixels[y * W + x]);
		}
		line[W / 4] = '\0';
		printf("|%s|\n", line);
	}

	/* The triangle covers (1.5 * 1.5 / 2) / 4 of the viewport: ~28%. */
	bool gpu = renderer && strstr(renderer, "FD") != NULL;
	bool ok = want_center == '#' && want_corner == '.' && want_bottom_mid == '#' &&
	    tri > W * H / 5 && tri < W * H * 2 / 5 && other < W * H / 50;
	printf("result: %s, picture %s\n", gpu ? "freedreno" : "NOT the GPU", ok ? "OK" : "WRONG");
	OSMesaDestroyContext(ctx);
	return gpu && ok ? 0 : 1;
}
