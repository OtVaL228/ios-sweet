/*
 * Minimal SPMI PMIC-arbiter v5 client (EE0). First job: tell ABL to stay
 * in fastboot after the next reset, the way Linux does on SM8150:
 *   qpnp_pon_set_restart_reason(PON_RESTART_REASON_BOOTLOADER)
 *   -> PM8150 (SID0) PON_SOFT_RB_SPARE[7:1] = 2
 * PS_HOLD is already in warm-reset mode after ABL (ctl=0x01, ctl2=0x80),
 * so PSCI SYSTEM_RESET afterwards lands in fastboot. Verified on guacamole
 * with guac-probe v5. The charger screen (charger.c) also reads the fuel
 * gauge and charger, sets the SMB5 current limits and the PS_HOLD power-off
 * type: spmi_write() takes only the registers listed in it.
 *
 * Layout (Linux drivers/spmi/spmi-pmic-arb.c, v5):
 *   core  + 0x900 + 4*apid : APID -> PPID map ([19:8] = sid<<8 | periph)
 *   cnfg  + 0x700 + 4*apid : ownership, [2:0] = owning EE (we are EE0)
 *   obsrvr + 0x80 * apid   : read channel   (CMD 0x00, STATUS 0x08, RDATA0 0x18)
 *   chnls + 0x10000 * apid : write channel  (CMD 0x00, STATUS 0x08, WDATA0 0x10)
 * (guac-probe v3 used 0x80*apid for writes: XPU violation -> CrashDump.)
 */
#include <stdint.h>
#include "board.h"
#include "spmi.h"

#if REBOOT_VIA_PMIC_PON
#define SPMI_CORE	0x0c440000UL
#define SPMI_CHNLS	0x0c600000UL
#define SPMI_OBSRVR	0x0e600000UL
#define SPMI_CNFG	0x0c40a000UL
#define PON_SOFT_RB_SPARE	0x88f	/* PM8150 (SID0) PON 0x800 + 0x8f */
#define PON_REASON_BOOTLOADER	0x02	/* enum pon_restart_reason; stored << 1 */

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })

static inline uint32_t rd32(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void wr32(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }

static void udelay1(void)
{
	__asm__ volatile("isb" ::: "memory");
	uint64_t end = rsr("cntpct_el0") + rsr("cntfrq_el0") / 1000000 + 1;
	while (({ __asm__ volatile("isb" ::: "memory"); rsr("cntpct_el0"); }) < end)
		;
}

static int wait_done(uintptr_t ch)
{
	for (int i = 0; i < 10000; i++) {
		uint32_t st = rd32(ch + 0x08);
		if (st & 1)
			return (st & 0x0e) ? -1 : 0;
		udelay1();
	}
	return -2;
}

/* The APID of (sid, peripheral); with owned, only one EE0 owns (a write channel). */
static int find_apid(unsigned sid, unsigned periph, int owned)
{
	uint32_t n = rd32(SPMI_CORE + 0x4) & 0x7ff;
	uint32_t ver = rd32(SPMI_CORE + 0x0);
	int found = -1;
	if (ver < 0x50000000u || ver >= 0x70000000u || n > 512)
		return -1;
	for (uint32_t i = 0; i < n; i++) {
		uint32_t map = rd32(SPMI_CORE + 0x900 + 4 * i);
		if (!map || ((map >> 8) & 0xfff) != ((sid << 8) | periph))
			continue;
		int ee0 = (rd32(SPMI_CNFG + 0x700 + 4 * i) & 7) == 0;
		if (ee0 || (!owned && found < 0))
			found = (int)i;
	}
	return found;
}

int spmi_read(unsigned sid, unsigned addr, uint8_t *out)
{
	int apid = find_apid(sid, (addr >> 8) & 0xff, 0);
	if (apid < 0)
		return -1;
	uintptr_t obs = SPMI_OBSRVR + 0x80UL * (unsigned)apid;
	wr32(obs + 0x00, (1u << 27) | ((addr & 0xff) << 4));	/* EXT_READL, 1 byte */
	__asm__ volatile("dsb sy" ::: "memory");
	if (wait_done(obs))
		return -3;
	*out = (uint8_t)rd32(obs + 0x18);
	return 0;
}

int spmi_write(unsigned sid, unsigned addr, uint8_t value)
{
	static const uint16_t pm8150[] = { 0x85a, 0x85b, PON_SOFT_RB_SPARE };	/* PS_HOLD_RST_CTL, CTL2 */
	static const uint16_t pm8150b[] = { 0x1061, 0x1342, 0x1365, 0x1366, 0x1370 };	/* xnu 0167's */
	const uint16_t *list = sid == 0 ? pm8150 : sid == 2 ? pm8150b : 0;
	unsigned count = sid == 0 ? 3 : sid == 2 ? 5 : 0, ok = 0;
	for (unsigned i = 0; i < count; i++)
		ok |= list[i] == addr;
	if (!ok)
		return -1;
	int apid = find_apid(sid, (addr >> 8) & 0xff, 1);
	if (apid < 0)
		return -1;
	uintptr_t chn = SPMI_CHNLS + 0x10000UL * (unsigned)apid;
	wr32(chn + 0x10, value);
	__asm__ volatile("dsb sy" ::: "memory");
	wr32(chn + 0x00, (0u << 27) | ((addr & 0xff) << 4));	/* EXT_WRITEL, 1 byte */
	__asm__ volatile("dsb sy" ::: "memory");
	return wait_done(chn) ? -4 : 0;
}

int pon_set_reboot_bootloader(void)
{
	uint8_t spare;
	if (spmi_read(0, PON_SOFT_RB_SPARE, &spare))
		return -3;
	return spmi_write(0, PON_SOFT_RB_SPARE, (spare & 0x01) | (PON_REASON_BOOTLOADER << 1)) ? -4 : 0;
}
#else
int pon_set_reboot_bootloader(void) { return 0; }
int spmi_read(unsigned sid, unsigned addr, uint8_t *out) { (void)sid; (void)addr; (void)out; return -1; }
int spmi_write(unsigned sid, unsigned addr, uint8_t value) { (void)sid; (void)addr; (void)value; return -1; }
#endif
