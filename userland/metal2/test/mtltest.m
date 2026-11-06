/* mtltest: Metal through Metal.framework (SweetMetal2.bundle): the device, a buffer fill by blit, a compute kernel
 * compiled from source. List "mtltest" in /var/local/mtl2-clients first. */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdio.h>

int main(void)
{
	@autoreleasepool {
		id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
		if (!dev) {
			printf("FAIL no device\n");
			return 1;
		}
		printf("device: %s\n", dev.name.UTF8String);
		id<MTLCommandQueue> q = [dev newCommandQueue];
		id<MTLBuffer> buf = [dev newBufferWithLength:4096 * 4 options:MTLResourceStorageModeShared];
		id<MTLCommandBuffer> cb = [q commandBuffer];
		id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
		[blit fillBuffer:buf range:NSMakeRange(0, 4096 * 4) value:0x5a];
		[blit endEncoding];
		[cb commit];
		[cb waitUntilCompleted];
		const uint32_t *w = buf.contents;
		int bad = 0;
		for (int i = 0; i < 4096; i++)
			bad += w[i] != 0x5a5a5a5a;
		printf("%s blit fill: %d of 4096 wrong (status %ld)\n", bad ? "FAIL" : "PASS", bad, (long)cb.status);
		NSError *err = nil;
		id<MTLLibrary> lib = [dev newLibraryWithSource:
		    @"kernel void k(device uint *o [[buffer(0)]], uint i [[thread_position_in_grid]]) { o[i] = i * 3 + 1; }"
		    options:nil error:&err];
		if (!lib) {
			printf("FAIL source library: %s\n", err.description.UTF8String);
			return 1;
		}
		id<MTLComputePipelineState> ps = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"k"] error:&err];
		if (!ps) {
			printf("FAIL pipeline: %s\n", err.description.UTF8String);
			return 1;
		}
		cb = [q commandBuffer];
		id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
		[ce setComputePipelineState:ps];
		[ce setBuffer:buf offset:0 atIndex:0];
		[ce dispatchThreads:MTLSizeMake(4096, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
		[ce endEncoding];
		[cb commit];
		[cb waitUntilCompleted];
		bad = 0;
		for (int i = 0; i < 4096; i++)
			bad += w[i] != (uint32_t)i * 3 + 1;
		printf("%s compute: %d of 4096 wrong (status %ld)\n", bad ? "FAIL" : "PASS", bad, (long)cb.status);
	}
	return 0;
}
