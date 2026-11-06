/* cgtest: CoreGraphics drawing into an IOSurface (CGIOSurfaceContext, the accelerated path WebKit's tiles take: QuartzCore
 * renders it with Metal) against the same drawing on the CPU. Writes /var/local/mtl/cg-gpu.png and cg-cpu.png and counts
 * the pixels that differ. List "cgtest" in /var/local/mtl2-clients. */
#import <Foundation/Foundation.h>
#import <CoreGraphics/CoreGraphics.h>
#import <CoreText/CoreText.h>
#import <ImageIO/ImageIO.h>
#import <IOSurface/IOSurfaceRef.h>
#include <stdio.h>
#include <stdlib.h>

CGContextRef CGIOSurfaceContextCreate(IOSurfaceRef surface, size_t width, size_t height, size_t bitsPerComponent,
    size_t bitsPerPixel, CGColorSpaceRef space, CGBitmapInfo info);

enum { W = 600, H = 400 };

static void draw(CGContextRef c, CGImageRef image)
{
	CGContextSetRGBFillColor(c, 0.05, 0.05, 0.1, 1);
	CGContextFillRect(c, CGRectMake(0, 0, W, H));
	CGContextSetRGBFillColor(c, 0, 0.9, 0.9, 1);
	CGContextFillRect(c, CGRectMake(20, 330, 200, 40));
	CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
	CGFloat comps[] = { 1, 0, 0.5, 1, 0, 0.5, 1, 1 };
	CGGradientRef g = CGGradientCreateWithColorComponents(cs, comps, NULL, 2);
	CGContextSaveGState(c);
	CGContextClipToRect(c, CGRectMake(250, 330, 330, 40));
	CGContextDrawLinearGradient(c, g, CGPointMake(250, 0), CGPointMake(580, 0), 0);
	CGContextRestoreGState(c);
	CGGradientRelease(g);
	CGFloat white[] = { 1, 1, 1, 1 };
	CGColorRef color = CGColorCreate(cs, white);
	CTFontRef font = CTFontCreateWithName(CFSTR("Helvetica"), 36, NULL);
	CFStringRef keys[] = { kCTFontAttributeName, kCTForegroundColorAttributeName };
	CFTypeRef values[] = { font, color };
	CFDictionaryRef attrs = CFDictionaryCreate(NULL, (const void **)keys, (const void **)values, 2,
	    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	CFAttributedStringRef text = CFAttributedStringCreate(NULL, CFSTR("Hello, methaneos.dev 0123"), attrs);
	CTLineRef line = CTLineCreateWithAttributedString(text);
	CGContextSetTextPosition(c, 20, 260);
	CTLineDraw(line, c);
	CFRelease(line);
	CFRelease(text);
	CFRelease(attrs);
	CFRelease(font);
	CGColorRelease(color);
	CGColorSpaceRelease(cs);
	CGContextSetRGBStrokeColor(c, 0.2, 1, 0.2, 1);
	CGContextSetLineWidth(c, 3);
	CGContextStrokeEllipseInRect(c, CGRectMake(20, 120, 160, 100));
	CGContextSetRGBFillColor(c, 1, 0.6, 0, 0.5);
	CGContextFillEllipseInRect(c, CGRectMake(100, 140, 160, 100));
	CGContextDrawImage(c, CGRectMake(300, 20, 280, 220), image);
}

static CGImageRef pattern(void)
{
	uint32_t *px = malloc(64 * 64 * 4);
	for (int y = 0; y < 64; y++)
		for (int x = 0; x < 64; x++)
			px[y * 64 + x] = ((x / 8 + y / 8) & 1) ? 0xff2040ffu : 0xffffd020u;
	CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
	CGContextRef c = CGBitmapContextCreate(px, 64, 64, 8, 256, cs, kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
	CGImageRef image = CGBitmapContextCreateImage(c);
	CGContextRelease(c);
	CGColorSpaceRelease(cs);
	free(px);
	return image;
}

static void save(const char *path, void *px, size_t row)
{
	CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
	CGContextRef c = CGBitmapContextCreate(px, W, H, 8, row, cs, kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
	CGImageRef image = CGBitmapContextCreateImage(c);
	CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, strlen(path), false);
	CGImageDestinationRef d = CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, NULL);
	CGImageDestinationAddImage(d, image, NULL);
	CGImageDestinationFinalize(d);
	CFRelease(d);
	CFRelease(url);
	CGImageRelease(image);
	CGContextRelease(c);
	CGColorSpaceRelease(cs);
}

int main(void)
{
	@autoreleasepool {
		CGImageRef image = pattern();
		CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
		CGBitmapInfo info = kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little;
		IOSurfaceRef s = IOSurfaceCreate((__bridge CFDictionaryRef)@{
		    (id)kIOSurfaceWidth: @(W), (id)kIOSurfaceHeight: @(H), (id)kIOSurfaceBytesPerElement: @4,
		    (id)kIOSurfacePixelFormat: @((uint32_t)'BGRA')});
		CGContextRef gc = CGIOSurfaceContextCreate(s, W, H, 8, 32, cs, info);
		if (!gc)
			return printf("FAIL no CGIOSurfaceContext\n"), 1;
		draw(gc, image);
		CGContextFlush(gc);
		CGContextRelease(gc);
		IOSurfaceLock(s, kIOSurfaceLockReadOnly, NULL);
		size_t grow = IOSurfaceGetBytesPerRow(s);
		uint8_t *gpx = IOSurfaceGetBaseAddress(s);
		save("/var/local/mtl/cg-gpu.png", gpx, grow);

		size_t crow = W * 4;
		uint8_t *cpx = calloc(H, crow);
		CGContextRef cc = CGBitmapContextCreate(cpx, W, H, 8, crow, cs, info);
		draw(cc, image);
		CGContextFlush(cc);
		CGContextRelease(cc);
		save("/var/local/mtl/cg-cpu.png", cpx, crow);
		int bad = 0;
		for (int y = 0; y < H; y++)
			for (int x = 0; x < W * 4; x++)
				bad += abs(gpx[y * grow + x] - cpx[y * crow + x]) > 24;
		IOSurfaceUnlock(s, kIOSurfaceLockReadOnly, NULL);
		printf("%s: %d of %d bytes differ (surface row %zu)\n", bad > W * H / 50 ? "FAIL" : "PASS", bad, W * H * 4, grow);
	}
	return 0;
}
