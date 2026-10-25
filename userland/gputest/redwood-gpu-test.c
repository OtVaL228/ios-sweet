/*
 * redwood-gpu-test: the smallest user of RedwoodGPU (xnu patch 0117).
 *
 * Opens the RedwoodGPUUserClient, reads two parameters, makes one 8 KiB
 * buffer (MSM_GEM_NEW), maps it, writes an IB into its first page that
 * stores a value into its second page (CP_MEM_WRITE), submits it
 * (MSM_GEM_SUBMIT), waits for the fence (MSM_WAIT_FENCE) and prints what
 * the GPU wrote. Exit status 0 only when the value is there.
 *
 * Requests are the user client's selectors with Linux's msm_drm.h numbers
 * and structures (include/uapi/drm/msm_drm.h); the layouts below must stay
 * those of iokit/Kernel/arm/RedwoodGPU.cpp. Packet encoding: Linux
 * drivers/gpu/drm/msm/adreno/adreno_pm4.xml.h (pm4_pkt7_hdr), CP_MEM_WRITE
 * is opcode 0x3d, as in the backend self-test in bsd/dev/sweet_gpu.c.
 *
 * Boot with sweetgpu=11; run from autorun.sh (sweet-autorun), its output
 * shows in the log as "SWEET RUN: [redwood-gpu-test] ...".
 */
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

enum {
	kDrmGemClose  = 0x09,
	kMsmGetParam  = 0x40,
	kMsmGemNew    = 0x42,
	kMsmGemInfo   = 0x43,
	kMsmGemSubmit = 0x46,
	kMsmWaitFence = 0x47,
};

struct msm_timespec { int64_t seconds, nanoseconds; };
struct msm_param { uint32_t pipe, param; uint64_t value; uint32_t len, pad; };
struct msm_gem_new { uint64_t size; uint32_t flags, handle; };
struct msm_gem_info { uint32_t handle, info; uint64_t value; uint32_t len, pad; };
struct msm_gem_submit_cmd { uint32_t type, submit_idx, submit_offset, size, pad, nr_relocs; uint64_t relocs; };
struct msm_gem_submit_bo { uint32_t flags, handle; uint64_t presumed; };
struct msm_gem_submit {
	uint32_t flags, fence, nr_bos, nr_cmds;
	uint64_t bos, cmds;
	int32_t fence_fd;
	uint32_t queueid;
	uint64_t in_syncobjs, out_syncobjs;
	uint32_t nr_in_syncobjs, nr_out_syncobjs, syncobj_stride, pad;
};
struct msm_wait_fence { uint32_t fence, flags; struct msm_timespec timeout; uint32_t queueid; };
struct drm_gem_close { uint32_t handle, pad; };

#define MSM_PIPE_3D0          0x10
#define MSM_PARAM_GPU_ID      0x01
#define MSM_PARAM_CHIP_ID     0x03
#define MSM_INFO_GET_IOVA     1
#define MSM_BO_WC             0x00020000
#define MSM_SUBMIT_BO_READ    0x0001
#define MSM_SUBMIT_BO_WRITE   0x0002
#define MSM_SUBMIT_CMD_BUF    1
#define CP_MEM_WRITE          0x3d

#define BUF_SIZE              0x2000u
#define OUT_OFFSET            0x1000u
#define MAGIC                 0x5eed0640u
#define WAIT_SECS             300u

static io_connect_t conn;

static kern_return_t
call(uint32_t selector, void *req, size_t size)
{
	size_t out = size;
	return IOConnectCallStructMethod(conn, selector, req, size, req, &out);
}

static uint32_t
parity(uint32_t v)
{
	return (0x9669u >> (0xfu & (v ^ (v >> 4) ^ (v >> 8) ^ (v >> 12) ^ (v >> 16) ^
	    (v >> 20) ^ (v >> 24) ^ (v >> 28)))) & 1u;
}

static uint32_t
pkt7(uint32_t opcode, uint32_t count)
{
	return 0x70000000u | count | (parity(count) << 15) | ((opcode & 0x7fu) << 16) |
	    (parity(opcode) << 23);
}

/* msm's timeouts are absolute, on the clock that runs during sleep. */
static struct msm_timespec
in_ms(uint64_t ms)
{
	mach_timebase_info_data_t tb;
	mach_timebase_info(&tb);
	uint64_t ns = mach_continuous_time() * tb.numer / tb.denom + ms * 1000000ull;
	struct msm_timespec t = { (int64_t)(ns / 1000000000ull), (int64_t)(ns % 1000000000ull) };
	return t;
}

int
main(void)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	/* The GPU stages start 60 s after the kernel and take a while: wait for the service. */
	io_service_t svc = 0;
	unsigned waited = 0;
	while (!(svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("RedwoodGPU")))) {
		if (waited == 0) {
			printf("waiting for the RedwoodGPU service (up to %u s)\n", WAIT_SECS);
		}
		if (waited++ >= WAIT_SECS) {
			printf("no RedwoodGPU service (boot with sweetgpu=11?)\n");
			return 2;
		}
		sleep(1);
	}
	if (waited) {
		printf("RedwoodGPU service after %u s\n", waited);
	}
	kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0, &conn);
	IOObjectRelease(svc);
	if (kr != KERN_SUCCESS) {
		printf("IOServiceOpen: %#x\n", kr);
		return 2;
	}
	printf("user client open\n");

	struct msm_param p = { .pipe = MSM_PIPE_3D0, .param = MSM_PARAM_GPU_ID };
	kr = call(kMsmGetParam, &p, sizeof(p));
	uint64_t gpu_id = p.value;
	p.param = MSM_PARAM_CHIP_ID;
	kr |= call(kMsmGetParam, &p, sizeof(p));
	printf("GET_PARAM: %#x, GPU_ID %llu, CHIP_ID %#llx\n", kr, gpu_id, p.value);

	struct msm_gem_new gn = { .size = BUF_SIZE, .flags = MSM_BO_WC };
	kr = call(kMsmGemNew, &gn, sizeof(gn));
	if (kr != KERN_SUCCESS || gn.handle == 0) {
		printf("GEM_NEW: %#x\n", kr);
		return 1;
	}
	struct msm_gem_info gi = { .handle = gn.handle, .info = MSM_INFO_GET_IOVA };
	kr = call(kMsmGemInfo, &gi, sizeof(gi));
	uint64_t iova = gi.value;
	printf("GEM_NEW: handle %u, GEM_INFO iova %#llx (%#x)\n", gn.handle, iova, kr);

	mach_vm_address_t addr = 0;
	mach_vm_size_t size = 0;
	kr = IOConnectMapMemory64(conn, gn.handle, mach_task_self(), &addr, &size, kIOMapAnywhere);
	if (kr != KERN_SUCCESS) {
		printf("IOConnectMapMemory64: %#x\n", kr);
		return 1;
	}
	printf("mapped %#llx bytes at %#llx\n", size, addr);

	volatile uint32_t *ib = (volatile uint32_t *)addr;
	volatile uint32_t *out = (volatile uint32_t *)(addr + OUT_OFFSET);
	uint32_t n = 0;
	ib[n++] = pkt7(CP_MEM_WRITE, 3);
	ib[n++] = (uint32_t)(iova + OUT_OFFSET);
	ib[n++] = (uint32_t)((iova + OUT_OFFSET) >> 32);
	ib[n++] = MAGIC;
	*out = 0xdeadbeefu;
	__asm__ volatile ("dsb sy" ::: "memory");

	struct msm_gem_submit_bo bo = { .flags = MSM_SUBMIT_BO_READ | MSM_SUBMIT_BO_WRITE, .handle = gn.handle };
	struct msm_gem_submit_cmd cmd = { .type = MSM_SUBMIT_CMD_BUF, .submit_idx = 0, .submit_offset = 0, .size = n * 4 };
	struct msm_gem_submit sub = {
		.nr_bos = 1, .nr_cmds = 1,
		.bos = (uint64_t)(uintptr_t)&bo, .cmds = (uint64_t)(uintptr_t)&cmd,
		.fence_fd = -1,
	};
	uint64_t t0 = mach_absolute_time();
	kr = call(kMsmGemSubmit, &sub, sizeof(sub));
	printf("GEM_SUBMIT: %#x, fence %u\n", kr, sub.fence);
	if (kr != KERN_SUCCESS || sub.fence == 0) {
		return 1;
	}
	struct msm_wait_fence wf = { .fence = sub.fence, .timeout = in_ms(2000) };
	kr = call(kMsmWaitFence, &wf, sizeof(wf));
	uint64_t t1 = mach_absolute_time();
	mach_timebase_info_data_t tb;
	mach_timebase_info(&tb);
	printf("WAIT_FENCE: %#x after %llu us\n", kr, (t1 - t0) * tb.numer / tb.denom / 1000);

	uint32_t got = *out;
	printf("result: %#x (want %#x): %s\n", got, MAGIC, got == MAGIC ? "OK" : "FAIL");

	IOConnectUnmapMemory64(conn, gn.handle, mach_task_self(), addr);
	struct drm_gem_close gc = { .handle = gn.handle };
	printf("GEM_CLOSE: %#x\n", call(kDrmGemClose, &gc, sizeof(gc)));
	IOServiceClose(conn);
	return got == MAGIC ? 0 : 1;
}
