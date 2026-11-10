/*
 * nxprobe: what our kernel answers to bluetoothd's A2DP Skywalk setup (research/bluetooth/a2dp-skywalk.md).
 * bluetoothd (iOS 16.7) at AVDTP OPEN: os_nexus_controller_register_provider("BTAudioSkywalk"-like name,
 * NEXUS_TYPE_USER_PIPE, slot 2048, 1 TX / 1 RX ring, 8 / 8 slots) succeeds, then
 * os_nexus_controller_alloc_provider_instance fails ("Failed to alloc a nexus provider") and bluetoothd is gone.
 * The log has no errno; this makes the same calls and prints each result and errno. Creates nothing that outlives
 * it (instance freed, provider deregistered, controller destroyed). Run as root and as mobile:
 *   nxprobe            user pipe, as bluetoothd
 *   nxprobe k          kernel pipe type instead (for comparison)
 * Prototypes and enums from xnu-8796 bsd/skywalk/os_nexus.h (private, not in the SDK): libsystem_kernel exports them.
 * MIT, as userland/.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <uuid/uuid.h>
#include <unistd.h>

typedef struct nexus_attr *nexus_attr_t;
typedef struct nexus_controller *nexus_controller_t;
typedef char nexus_name_t[64];
typedef enum { NEXUS_TYPE_USER_PIPE, NEXUS_TYPE_KERNEL_PIPE, NEXUS_TYPE_NET_IF, NEXUS_TYPE_FLOW_SWITCH } nexus_type_t;
typedef enum {
	NEXUS_ATTR_TX_RINGS, NEXUS_ATTR_RX_RINGS, NEXUS_ATTR_TX_SLOTS, NEXUS_ATTR_RX_SLOTS, NEXUS_ATTR_SLOT_BUF_SIZE,
	NEXUS_ATTR_SLOT_META_SIZE, NEXUS_ATTR_ANONYMOUS, NEXUS_ATTR_MHINTS, NEXUS_ATTR_PIPES,
} nexus_attr_type_t;

extern nexus_attr_t os_nexus_attr_create(void);
extern int os_nexus_attr_set(const nexus_attr_t, const nexus_attr_type_t, const uint64_t);
extern void os_nexus_attr_destroy(nexus_attr_t);
extern nexus_controller_t os_nexus_controller_create(void);
extern void os_nexus_controller_destroy(nexus_controller_t);
extern int os_nexus_controller_register_provider(const nexus_controller_t, const nexus_name_t, const nexus_type_t,
    const nexus_attr_t, uuid_t *);
extern int os_nexus_controller_deregister_provider(const nexus_controller_t, const uuid_t);
extern int os_nexus_controller_alloc_provider_instance(const nexus_controller_t, const uuid_t, uuid_t *);
extern int os_nexus_controller_free_provider_instance(const nexus_controller_t, const uuid_t);

#define SAY(...) do { printf(__VA_ARGS__); printf("\n"); fflush(stdout); } while (0)

int main(int argc, char **argv)
{
	nexus_type_t type = argc > 1 && argv[1][0] == 'k' ? NEXUS_TYPE_KERNEL_PIPE : NEXUS_TYPE_USER_PIPE;
	SAY("uid %d, type %s", getuid(), type == NEXUS_TYPE_USER_PIPE ? "USER_PIPE" : "KERNEL_PIPE");
	nexus_controller_t ctl = os_nexus_controller_create();
	SAY("os_nexus_controller_create -> %p (errno %d %s)", (void *)ctl, errno, ctl ? "" : strerror(errno));
	if (!ctl)
		return 1;
	nexus_attr_t a = os_nexus_attr_create();
	static const struct { nexus_attr_type_t t; uint64_t v; const char *n; } set[] = {
		{ NEXUS_ATTR_SLOT_BUF_SIZE, 2048, "SLOT_BUF_SIZE" }, { NEXUS_ATTR_RX_RINGS, 1, "RX_RINGS" },
		{ NEXUS_ATTR_TX_RINGS, 1, "TX_RINGS" }, { NEXUS_ATTR_TX_SLOTS, 8, "TX_SLOTS" },
		{ NEXUS_ATTR_RX_SLOTS, 8, "RX_SLOTS" },
	};
	for (unsigned i = 0; a && i < sizeof(set) / sizeof(set[0]); i++) {
		int r = os_nexus_attr_set(a, set[i].t, set[i].v);
		if (r)
			SAY("os_nexus_attr_set %s %llu -> %d (%s)", set[i].n, (unsigned long long)set[i].v, r, strerror(errno));
	}
	nexus_name_t name;
	snprintf(name, sizeof(name), "com.sweet.nxprobe.%d", getpid());
	uuid_t prov, inst;
	char s[37];
	errno = 0;
	int r = os_nexus_controller_register_provider(ctl, name, type, a, &prov);
	uuid_unparse(prov, s);
	SAY("register_provider %s -> %d (errno %d %s) uuid %s", name, r, errno, r ? strerror(errno) : "", r ? "-" : s);
	if (r == 0) {
		errno = 0;
		r = os_nexus_controller_alloc_provider_instance(ctl, prov, &inst);
		uuid_unparse(inst, s);
		SAY("alloc_provider_instance -> %d (errno %d %s) uuid %s", r, errno, r ? strerror(errno) : "", r ? "-" : s);
		if (r == 0)
			SAY("free_provider_instance -> %d", os_nexus_controller_free_provider_instance(ctl, inst));
		SAY("deregister_provider -> %d", os_nexus_controller_deregister_provider(ctl, prov));
	}
	if (a)
		os_nexus_attr_destroy(a);
	os_nexus_controller_destroy(ctl);
	return 0;
}
