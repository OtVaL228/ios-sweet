/*
 * tsample: a sampling profiler for one process (iOS has no `sample`).
 *   tsample PID [SAMPLES] [INTERVAL_US]
 * Picks the busiest thread, suspends it SAMPLES times (default 500, every 4000 us), walks its frame-pointer
 * chain and prints, per sample, "S <depth> <pc> <pc> ..." plus the loaded images ("I <base> <path>") for
 * symbolication on the host. Signed with task_for_pid-allow (tsample.plist).
 */
#include <mach/mach.h>
#include <mach/vm_map.h>
#include <mach-o/dyld_images.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static uint64_t
rd64(task_t t, uint64_t a)
{
	uint64_t v = 0;
	vm_size_t n = 0;
	if (vm_read_overwrite(t, a, 8, (vm_address_t)&v, &n) != KERN_SUCCESS || n != 8)
		return 0;
	return v;
}

int
main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: tsample PID [SAMPLES] [INTERVAL_US] [THREAD]\n");
		return 2;
	}
	int pid = atoi(argv[1]), samples = argc > 2 ? atoi(argv[2]) : 500, us = argc > 3 ? atoi(argv[3]) : 4000;
	task_t task;
	kern_return_t kr = task_for_pid(mach_task_self(), pid, &task);
	if (kr != KERN_SUCCESS) {
		fprintf(stderr, "task_for_pid: %s\n", mach_error_string(kr));
		return 1;
	}
	thread_act_array_t th;
	mach_msg_type_number_t nth;
	if (task_threads(task, &th, &nth) != KERN_SUCCESS)
		return 1;
	/*
	 * 2026-11-10: the busiest thread by CPU time used over one second (user + system time, measured twice), not
	 * by thread_info's instantaneous cpu_usage: a scroll sample picked an idle thread (0.5%) and sampled
	 * mach_msg for 6 s. The five busiest are listed; an optional 4th argument picks a thread index instead.
	 */
	enum { MAXT = 512 };
	static uint64_t t0[MAXT];
	unsigned n = nth < MAXT ? nth : MAXT;
	for (unsigned i = 0; i < n; i++) {
		thread_basic_info_data_t bi;
		mach_msg_type_number_t c = THREAD_BASIC_INFO_COUNT;
		t0[i] = thread_info(th[i], THREAD_BASIC_INFO, (thread_info_t)&bi, &c) == KERN_SUCCESS ?
		    (uint64_t)(bi.user_time.seconds + bi.system_time.seconds) * 1000000u + bi.user_time.microseconds +
		    bi.system_time.microseconds : 0;
	}
	usleep(1000000);
	static uint64_t used[MAXT];
	int best = -1;
	for (unsigned i = 0; i < n; i++) {
		thread_basic_info_data_t bi;
		mach_msg_type_number_t c = THREAD_BASIC_INFO_COUNT;
		uint64_t now = thread_info(th[i], THREAD_BASIC_INFO, (thread_info_t)&bi, &c) == KERN_SUCCESS ?
		    (uint64_t)(bi.user_time.seconds + bi.system_time.seconds) * 1000000u + bi.user_time.microseconds +
		    bi.system_time.microseconds : 0;
		used[i] = now > t0[i] ? now - t0[i] : 0;
		if (best < 0 || used[i] > used[best])
			best = (int)i;
	}
	for (int k = 0; k < 5; k++) {
		int m = -1;
		for (unsigned i = 0; i < n; i++)
			if (used[i] != (uint64_t)-1 && (m < 0 || used[i] > used[m]))
				m = (int)i;
		if (m < 0 || used[m] == 0)
			break;
		printf("T thread #%d: %.1f%% of a core over 1 s\n", m, used[m] / 10000.0);
		used[m] = (uint64_t)-1;
	}
	if (argc > 4)
		best = atoi(argv[4]) < (int)nth ? atoi(argv[4]) : best;
	printf("T %d threads, sampling #%d\n", nth, best);
	thread_act_t t = th[best];
	for (int s = 0; s < samples; s++) {
		if (thread_suspend(t) != KERN_SUCCESS)
			break;
		arm_thread_state64_t st;
		mach_msg_type_number_t c = ARM_THREAD_STATE64_COUNT;
		uint64_t pcs[64];
		int d = 0;
		if (thread_get_state(t, ARM_THREAD_STATE64, (thread_state_t)&st, &c) == KERN_SUCCESS) {
			const uint64_t mask = 0x0000000fffffffffull;
			pcs[d++] = __darwin_arm_thread_state64_get_pc(st) & mask;
			pcs[d++] = __darwin_arm_thread_state64_get_lr(st) & mask;
			uint64_t fp = __darwin_arm_thread_state64_get_fp(st) & mask;
			while (fp && d < 64) {
				uint64_t next = rd64(task, fp) & mask, lr = rd64(task, fp + 8) & mask;
				if (!lr)
					break;
				pcs[d++] = lr;
				if (next <= fp)
					break;
				fp = next;
			}
		}
		thread_resume(t);
		printf("S %d", d);
		for (int i = 0; i < d; i++)
			printf(" %llx", pcs[i]);
		printf("\n");
		usleep(us);
	}
	struct task_dyld_info di;
	mach_msg_type_number_t c = TASK_DYLD_INFO_COUNT;
	if (task_info(task, TASK_DYLD_INFO, (task_info_t)&di, &c) == KERN_SUCCESS) {
		struct dyld_all_image_infos all;
		vm_size_t n = 0;
		if (vm_read_overwrite(task, di.all_image_info_addr, sizeof(all), (vm_address_t)&all, &n) == KERN_SUCCESS) {
			printf("C %llx\n", (unsigned long long)all.sharedCacheSlide);
			for (uint32_t i = 0; i < all.infoArrayCount; i++) {
				struct dyld_image_info ii;
				if (vm_read_overwrite(task, (vm_address_t)all.infoArray + i * sizeof(ii), sizeof(ii),
				    (vm_address_t)&ii, &n) != KERN_SUCCESS)
					continue;
				char path[512] = {0};
				vm_read_overwrite(task, (vm_address_t)ii.imageFilePath, sizeof(path) - 1, (vm_address_t)path, &n);
				printf("I %llx %s\n", (unsigned long long)(uintptr_t)ii.imageLoadAddress, path);
			}
		}
	}
	return 0;
}
