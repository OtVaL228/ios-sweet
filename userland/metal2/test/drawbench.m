/* drawbench: the CPU cost of a draw and of a pass through Metal.framework (SweetMetal2.bundle), shaped like QuartzCore's
 * MetalContext: a 1125x2436 BGRA8 target (plus a memoryless second color, as QuartzCore's passes have), premultiplied
 * blending, each draw a quad whose positions and colors go by setVertexBytes and whose texture alternates, indexed from a
 * buffer. Shaders: the runtime's own runtime_examples (colored_vertex, sampled_fragment), the AIR tools/build-m2v-bridge.sh
 * assembles (metal-work/m2v-bridge/runtime_examples.air), copied next to the binary as drawbench.air.
 *   drawbench [DRAWS_PER_PASS [PASSES [FRAMES [second]]]]   (defaults 40 4 60; "second" adds the memoryless color)
 * Prints the encode time per draw, per pass end and per commit, and the GPU time per frame (commit to completion).
 * List "drawbench" in /var/local/mtl2-clients first. MIT, as userland/. */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <dlfcn.h>
#include <mach/mach_time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static double us(uint64_t t)
{
	static mach_timebase_info_data_t tb;
	if (!tb.denom)
		mach_timebase_info(&tb);
	return t * (double)tb.numer / tb.denom / 1000.0;
}

int main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	int draws = argc > 1 ? atoi(argv[1]) : 40, passes = argc > 2 ? atoi(argv[2]) : 4, frames = argc > 3 ? atoi(argv[3]) : 60;
	bool second = argc > 4 && strcmp(argv[4], "second") == 0;
	@autoreleasepool {
		id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
		if (!dev) {
			printf("FAIL no device\n");
			return 1;
		}
		NSError *err = nil;
		NSString *self_ = [[NSString stringWithUTF8String:argv[0]] stringByDeletingLastPathComponent];
		NSData *metallib = [NSData dataWithContentsOfFile:[self_ stringByAppendingPathComponent:@"drawbench.air"]];
		/* Metal refuses bare AIR; the frontend makes its own fixtures' libraries from it (NativeBridgeMetalFrontend.mm) */
		id (*with_air)(const void *, size_t) = dlsym(RTLD_DEFAULT, "native_bridge_metal_library_with_air");
		id<MTLLibrary> lib = metallib && with_air ? with_air(metallib.bytes, metallib.length) : nil;
		if (!lib) {
			printf("FAIL library: %s\n", err.description.UTF8String);
			return 1;
		}
		MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
		pd.vertexFunction = [lib newFunctionWithName:@"colored_vertex"];
		pd.fragmentFunction = [lib newFunctionWithName:@"sampled_fragment"];
		pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
		pd.colorAttachments[0].blendingEnabled = YES;
		pd.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorOne;
		pd.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
		pd.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
		pd.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
		if (second) {
			pd.colorAttachments[1].pixelFormat = MTLPixelFormatBGRA8Unorm;
			pd.colorAttachments[1].writeMask = MTLColorWriteMaskNone;
		}
		id<MTLRenderPipelineState> ps = [dev newRenderPipelineStateWithDescriptor:pd error:&err];
		if (!ps) {
			printf("FAIL pipeline: %s\n", err.description.UTF8String);
			return 1;
		}
		MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
											     width:1125 height:2436 mipmapped:NO];
		td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
		td.storageMode = MTLStorageModePrivate;
		id<MTLTexture> target = [dev newTextureWithDescriptor:td];
		id<MTLTexture> scratch = nil;
		if (second) {
			td.usage = MTLTextureUsageRenderTarget;
			td.storageMode = MTLStorageModeMemoryless;
			scratch = [dev newTextureWithDescriptor:td];
		}
		MTLTextureDescriptor *sd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
											     width:256 height:256 mipmapped:NO];
		sd.storageMode = MTLStorageModeShared;
		id<MTLTexture> tex[2] = { [dev newTextureWithDescriptor:sd], [dev newTextureWithDescriptor:sd] };
		uint32_t *px = malloc(256 * 256 * 4);
		for (int t = 0; t < 2; t++) {
			for (int i = 0; i < 256 * 256; i++)
				px[i] = t ? 0x80402010u : 0xff808080u;
			[tex[t] replaceRegion:MTLRegionMake2D(0, 0, 256, 256) mipmapLevel:0 withBytes:px bytesPerRow:256 * 4];
		}
		free(px);
		const uint16_t idx[6] = { 0, 1, 2, 2, 1, 3 };
		id<MTLBuffer> ib = [dev newBufferWithBytes:idx length:sizeof(idx) options:MTLResourceStorageModeShared];
		id<MTLCommandQueue> q = [dev newCommandQueue];

		double encode = 0, ends = 0, commits = 0, gpu = 0;
		int counted = 0;
		for (int f = 0; f < frames; f++) {
			@autoreleasepool {
				uint64_t t0 = mach_absolute_time(), tEnd = 0;
				id<MTLCommandBuffer> cb = [q commandBuffer];
				for (int p = 0; p < passes; p++) {
					MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
					rp.colorAttachments[0].texture = target;
					rp.colorAttachments[0].loadAction = p ? MTLLoadActionLoad : MTLLoadActionClear;
					rp.colorAttachments[0].storeAction = MTLStoreActionStore;
					if (scratch) {
						rp.colorAttachments[1].texture = scratch;
						rp.colorAttachments[1].loadAction = MTLLoadActionDontCare;
						rp.colorAttachments[1].storeAction = MTLStoreActionDontCare;
					}
					id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
					[e setRenderPipelineState:ps];
					for (int d = 0; d < draws; d++) {
						float x = -1 + 2.0f * (d % 8) / 8, y = -1 + 2.0f * ((d / 8) % 8) / 8, w = 0.25f;
						const float pos[8] = { x, y, x + w, y, x, y + w, x + w, y + w };
						const float color[16] = { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 };
						[e setVertexBytes:pos length:sizeof(pos) atIndex:0];
						[e setVertexBytes:color length:sizeof(color) atIndex:1];
						[e setFragmentTexture:tex[d & 1] atIndex:0];
						[e drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:6 indexType:MTLIndexTypeUInt16
							     indexBuffer:ib indexBufferOffset:0];
					}
					uint64_t a = mach_absolute_time();
					[e endEncoding];
					tEnd += mach_absolute_time() - a;
				}
				uint64_t t1 = mach_absolute_time();
				[cb commit];
				uint64_t t2 = mach_absolute_time();
				[cb waitUntilCompleted];
				uint64_t t3 = mach_absolute_time();
				if (f >= frames / 5) {		/* the first fifth warms pipelines and caches */
					encode += us(t1 - t0 - tEnd);
					ends += us(tEnd);
					commits += us(t2 - t1);
					gpu += us(t3 - t2);
					counted++;
				}
				if (cb.status != MTLCommandBufferStatusCompleted)
					printf("frame %d: status %ld %s\n", f, (long)cb.status, cb.error.description.UTF8String);
			}
		}
		if (!counted)
			return 1;
		printf("drawbench: %d draws x %d passes%s, %d frames: draw %.1f us, pass end %.1f us, commit %.0f us, "
		       "frame CPU %.2f ms, GPU %.2f ms\n", draws, passes, second ? " +memoryless" : "", counted,
		       encode / counted / (draws * passes), ends / counted / passes, commits / counted,
		       (encode + ends + commits) / counted / 1000, gpu / counted / 1000);
	}
	return 0;
}
