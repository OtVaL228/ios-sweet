/*
 * libsweet-vabt: lets VirtualAudio (mediaserverd's HAL plug-in) route playback to Bluetooth A2DP without an
 * audio codec. Its only source port here is "Application Processor" (pap); the connection pap -> A2DP port is
 * guarded by an inclusion policy whose first rule, "Require out of band speech input support", asks the A2DP
 * port for an OOB speech input device UUID (Port_BluetoothA2DP vtable slot +0x1c0, 0x404b9c): AirPods have
 * one, other headphones do not, and on an iPhone ordinary playback reaches them from the codec's port instead.
 * Without the codec every route ends on Null_Device ("Connection between ports Application Processor and
 * <addr>-tacl not allowed due to property inclusion policy").
 *
 * When VirtualAudio.plugin is loaded, and only for the build checked below (LC_UUID), the slot is pointed at
 * a function answering yes. Loaded by mswrap through DYLD_INSERT_LIBRARIES. MIT, as userland/.
 */
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach/mach.h>
#include <os/log.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* VirtualAudio of iOS 16.7.16 (20H392) */
static const uint8_t va_uuid[16] = { 0x31, 0x96, 0x33, 0x3f, 0x5a, 0x48, 0x37, 0x3b,
				     0xb4, 0x27, 0x01, 0x46, 0xdc, 0x11, 0x77, 0xfb };
#define VA_A2DP_VTABLE 0x62a2d8u	/* Port_BluetoothA2DP (stored by its constructor at 0x405734) */
#define VA_SLOT_OOB    0x1c0u
#define VA_OOB_ORIG    0x404b9cu	/* what the slot holds in this build */

static bool
oob_supported(void *port)
{
	(void)port;
	return true;
}

static void
image_added(const struct mach_header *mh, intptr_t slide)
{
	const struct mach_header_64 *h = (const struct mach_header_64 *)mh;
	if (h->magic != MH_MAGIC_64 || (h->filetype != MH_BUNDLE && h->filetype != MH_DYLIB))
		return;
	const struct load_command *lc = (const void *)(h + 1);
	const uint8_t *uuid = NULL;
	for (uint32_t i = 0; i < h->ncmds; i++) {
		if (lc->cmd == LC_UUID)
			uuid = ((const struct uuid_command *)lc)->uuid;
		lc = (const void *)((const char *)lc + lc->cmdsize);
	}
	if (uuid == NULL || memcmp(uuid, va_uuid, 16) != 0)
		return;
	uintptr_t *slot = (uintptr_t *)(VA_A2DP_VTABLE + VA_SLOT_OOB + (uintptr_t)slide);
	if (*slot != VA_OOB_ORIG + (uintptr_t)slide) {
		os_log(OS_LOG_DEFAULT, "SWEET VABT: VirtualAudio slot holds %#lx, not the expected method: left alone",
		    (unsigned long)(*slot - (uintptr_t)slide));
		return;
	}
	vm_address_t page = (vm_address_t)slot & ~(vm_address_t)(vm_page_size - 1);
	kern_return_t kr = vm_protect(mach_task_self(), page, vm_page_size, false,
	    VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY);
	if (kr != KERN_SUCCESS) {
		os_log(OS_LOG_DEFAULT, "SWEET VABT: vm_protect -> %d", kr);
		return;
	}
	*slot = (uintptr_t)oob_supported;
	vm_protect(mach_task_self(), page, vm_page_size, false, VM_PROT_READ);
	os_log(OS_LOG_DEFAULT, "SWEET VABT: A2DP ports now report out of band speech input support "
	    "(VirtualAudio slide %#lx)", (unsigned long)slide);
}

__attribute__((constructor)) static void
init(void)
{
	_dyld_register_func_for_add_image(image_added);
}
