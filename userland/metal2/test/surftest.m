/* surftest: a Metal texture over an IOSurface (as QuartzCore makes them): its texels must be the surface's bytes, row by row.
 * Copies texture -> buffer and buffer -> texture by blit and compares with the surface's memory. */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOSurface/IOSurfaceRef.h>
#include <stdio.h>

static int check(id<MTLDevice> dev, id<MTLCommandQueue> q, MTLPixelFormat fmt, int w, int h)
{
	IOSurfaceRef s = IOSurfaceCreate((__bridge CFDictionaryRef)@{
	    (id)kIOSurfaceWidth: @(w), (id)kIOSurfaceHeight: @(h), (id)kIOSurfaceBytesPerElement: @4,
	    (id)kIOSurfacePixelFormat: @((uint32_t)'BGRA')});
	size_t row = IOSurfaceGetBytesPerRow(s);
	IOSurfaceLock(s, 0, NULL);
	uint8_t *base = IOSurfaceGetBaseAddress(s);
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++)
			((uint32_t *)(base + y * row))[x] = (uint32_t)x | (uint32_t)y << 16;
	IOSurfaceUnlock(s, 0, NULL);
	MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fmt width:w height:h mipmapped:NO];
	d.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
	id<MTLTexture> t = [dev newTextureWithDescriptor:d iosurface:s plane:0];
	if (!t) {
		printf("FAIL format %lu: no texture\n", (unsigned long)fmt);
		return 1;
	}
	id<MTLBuffer> b = [dev newBufferWithLength:w * h * 4 options:MTLResourceStorageModeShared];
	id<MTLCommandBuffer> cb = [q commandBuffer];
	id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
	[e copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, 1)
	    toBuffer:b destinationOffset:0 destinationBytesPerRow:w * 4 destinationBytesPerImage:w * h * 4];
	[e endEncoding];
	[cb commit];
	[cb waitUntilCompleted];
	const uint32_t *o = b.contents;
	int bad = 0, first = -1;
	for (int i = 0; i < w * h; i++)
		if (o[i] != ((uint32_t)(i % w) | (uint32_t)(i / w) << 16) && bad++ == 0)
			first = i;
	printf("%s format %lu %dx%d row %zu: texture -> buffer %d wrong", bad ? "FAIL" : "PASS", (unsigned long)fmt, w, h, row, bad);
	if (bad)
		printf(" (first at x %d y %d: %08x)", first % w, first / w, o[first]);
	printf("\n");
	/* render-target clear of the texture: every surface byte must change */
	cb = [q commandBuffer];
	MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
	rp.colorAttachments[0].texture = t;
	rp.colorAttachments[0].loadAction = MTLLoadActionClear;
	rp.colorAttachments[0].storeAction = MTLStoreActionStore;
	rp.colorAttachments[0].clearColor = MTLClearColorMake(1, 0, 0, 1);
	[[cb renderCommandEncoderWithDescriptor:rp] endEncoding];
	[cb commit];
	[cb waitUntilCompleted];
	IOSurfaceLock(s, kIOSurfaceLockReadOnly, NULL);
	bad = 0;
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++)
			bad += ((uint32_t *)(base + y * row))[x] != 0xffff0000u;
	IOSurfaceUnlock(s, kIOSurfaceLockReadOnly, NULL);
	printf("%s format %lu: clear to red, %d of %d surface pixels wrong (pixel 0 %08x)\n", bad ? "FAIL" : "PASS",
	    (unsigned long)fmt, bad, w * h, ((uint32_t *)base)[0]);
	CFRelease(s);
	return bad != 0;
}

int main(void)
{
	@autoreleasepool {
		id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
		if (!dev)
			return printf("FAIL no device\n"), 1;
		id<MTLCommandQueue> q = [dev newCommandQueue];
		check(dev, q, MTLPixelFormatBGRA8Unorm, 300, 200);
		check(dev, q, MTLPixelFormatBGRA8Unorm, 1257, 64);
		check(dev, q, MTLPixelFormatBGR10_XR, 1257, 64);
	}
	return 0;
}
