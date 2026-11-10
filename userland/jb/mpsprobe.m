/*
 * mpsprobe: why Metal Performance Shaders refuses our Metal device (PhotosPosterProvider and the Pride poster crash in
 * MPSAutoCache::MPSAutoCache reading nil+0x18: -[MPSCommandBufferImageCache initWithCommandBuffer:] returned nil).
 * That init returns nil when the command buffer does not conform to MTLCommandBuffer (0206 adopted it), or when
 * MPSDevice::GetMPSDevice([cb device]) is NULL. GetMPSDevice (research/metal/MPSDevice-GetMPSDevice.txt) asks
 * supportsFamily: Apple8 (1008) down to Apple2 (1002), and in some families compares -name with Apple GPU names:
 *   Apple8 / Apple7 true: accepted either way      Apple6 true: only "Apple A13 GPU" (else NULL)
 *   Apple5 / Apple4 / Apple3 / Apple2: accepted     none of them: NULL
 * This asks the same questions of the device, then MPS itself, one step at a time (each line flushed: if one crashes,
 * the last line says where). Read-only apart from one small blur on the GPU.
 *   mpsprobe
 * MIT, as userland/.
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/message.h>
#import <objc/runtime.h>
#include <dlfcn.h>
#include <stdio.h>

#define SAY(...) do { printf(__VA_ARGS__); printf("\n"); fflush(stdout); } while (0)

int main(void)
{
	@autoreleasepool {
		id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
		if (dev == nil) {
			SAY("no Metal device");
			return 1;
		}
		SAY("device %s (%s), name \"%s\"", class_getName(object_getClass(dev)),
		    class_getName(class_getSuperclass(object_getClass(dev))), dev.name.UTF8String);
		for (long f = 1001; f <= 1009; f++)
			SAY("  supportsFamily: Apple%ld (%ld) = %d", f - 1000, f, [dev supportsFamily:(MTLGPUFamily)f]);
		for (long f = 3001; f <= 3003; f++)
			SAY("  supportsFamily: Common%ld (%ld) = %d", f - 3000, f, [dev supportsFamily:(MTLGPUFamily)f]);
		SAY("  supportsFamily: Mac2 (2002) = %d, Metal3 (5001) = %d", [dev supportsFamily:(MTLGPUFamily)2002],
		    [dev supportsFamily:(MTLGPUFamily)5001]);
		SEL q[] = { sel_registerName("gpuCoreCount"), sel_registerName("originalObject"),
			sel_registerName("maxComputeThreadgroupMemory"), sel_registerName("maxTextureLayers"),
			sel_registerName("maxThreadgroupMemoryLength") };
		for (unsigned i = 0; i < sizeof(q) / sizeof(q[0]); i++) {
			BOOL r = [(id)dev respondsToSelector:q[i]];
			unsigned long long v = 0;
			if (r && i != 1)
				v = ((unsigned long long (*)(id, SEL))objc_msgSend)(dev, q[i]);
			SAY("  %-28s responds %d%s%llu", sel_getName(q[i]), r, r && i != 1 ? ", value " : "", v);
		}

		void *mps = dlopen("/System/Library/Frameworks/MetalPerformanceShaders.framework/MetalPerformanceShaders", RTLD_NOW);
		SAY("MetalPerformanceShaders %s", mps ? "loaded" : dlerror());
		BOOL (*supports)(id) = (BOOL (*)(id))dlsym(RTLD_DEFAULT, "MPSSupportsMTLDevice");
		SAY("MPSSupportsMTLDevice = %s", supports ? (supports(dev) ? "YES" : "NO (GetMPSDevice refuses it)") : "not found");

		id<MTLCommandQueue> queue = [dev newCommandQueue];
		id<MTLCommandBuffer> cb = [queue commandBuffer];
		SAY("command buffer %s, conforms MTLCommandBuffer %d, device %s", class_getName(object_getClass(cb)),
		    [(id)cb conformsToProtocol:@protocol(MTLCommandBuffer)], cb.device == dev ? "same" : "DIFFERENT");
		Class cacheClass = NSClassFromString(@"MPSCommandBufferImageCache");
		if (cacheClass != nil) {
			id cache = ((id (*)(id, SEL, id))objc_msgSend)([cacheClass alloc], sel_registerName("initWithCommandBuffer:"), cb);
			SAY("[[MPSCommandBufferImageCache alloc] initWithCommandBuffer:] = %s", cache ? "an object (MPS accepts us)" : "nil");
		} else {
			SAY("no MPSCommandBufferImageCache class");
		}

		/* MPS for real: a 64x64 Gaussian blur, as the poster's filters do (MPSAutoCache sits under every image kernel) */
		Class blurClass = NSClassFromString(@"MPSImageGaussianBlur");
		if (blurClass == nil || !(supports && supports(dev))) {
			SAY("skipping the blur (MPS refuses the device or is missing)");
			return 0;
		}
		MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
		    width:64 height:64 mipmapped:NO];
		d.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
		id<MTLTexture> src = [dev newTextureWithDescriptor:d], dst = [dev newTextureWithDescriptor:d];
		SAY("blur: kernel...");
		id blur = ((id (*)(id, SEL, id, float))objc_msgSend)([blurClass alloc], sel_registerName("initWithDevice:sigma:"), dev, 2.0f);
		SAY("blur: kernel %s; encoding...", blur ? "made" : "nil");
		if (blur == nil)
			return 0;
		id<MTLCommandBuffer> cb2 = [queue commandBuffer];
		((void (*)(id, SEL, id, id, id))objc_msgSend)(blur, sel_registerName("encodeToCommandBuffer:sourceTexture:destinationTexture:"),
		    cb2, src, dst);
		SAY("blur: encoded; committing...");
		[cb2 commit];
		[cb2 waitUntilCompleted];
		SAY("blur: done, status %ld%s%s", (long)cb2.status, cb2.error ? ", error " : "",
		    cb2.error ? cb2.error.description.UTF8String : "");
	}
	return 0;
}
