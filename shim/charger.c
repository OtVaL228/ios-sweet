/*
 * Charger screen (guacamole). When a charger powers the phone on, ABL boots the boot image with
 * androidboot.mode=charger in /chosen/bootargs (Android then shows its charging animation). Instead of
 * booting xnu straight away: set the SMB5 input and charge current limits as xnu 0167 does, show the
 * battery on the splash framebuffer for a while and black otherwise (an AMOLED's black pixels are off).
 * A press of the power key shows it again, holding it 2 s starts iOS (refused below 3% and 3.6 V), and
 * the charger unplugged for 3 s powers the phone off (PS_HOLD set to shutdown first: ABL leaves it at
 * warm reset). Registers: the OnePlus SM8150 kernel's smb5-reg.h, fg-reg.h and qpnp-power-on.c.
 * MMU off: every access is Device memory (aligned stores, -mstrict-align).
 */
#include <stdint.h>
#include "board.h"
#include "console.h"
#include "fdt.h"
#include "font.h"
#include "spmi.h"
#include "charger.h"

#if REBOOT_VIA_PMIC_PON

#define PSCI_SYSTEM_OFF		0x84000008UL
#define PSCI_SYSTEM_RESET	0x84000009UL
#define SHOW_MS			15000	/* the battery stays on screen this long after a press */
#define HOLD_MS			2000	/* holding the power key this long starts iOS */
#define UNPLUGGED_S		3	/* the charger gone this long powers off */

#define BLACK			0xff000000U
#define GREY			0xffd0d0d0U
#define CHARGE_GREEN		0xff30d158U
#define LOW_RED			0xffff453aU

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })
#define wsr(r, v) __asm__ volatile("msr " r ", %0" : : "r"((uint64_t)(v)))

struct battery {
	unsigned percent, mv;
	int ma;			/* into the battery: positive while charging */
	int usb, state;		/* USBIN present; BATTERY_CHARGER_STATUS_1 [2:0] */
};

static uint64_t now_ms(void)
{
	__asm__ volatile("isb" ::: "memory");
	return rsr("cntpct_el0") / (rsr("cntfrq_el0") / 1000);
}

/* Waits on the timer's event stream (charger_mode turns it on): the core sleeps between events. */
static void wait_ms(unsigned ms)
{
	uint64_t end = now_ms() + ms;
	while (now_ms() < end)
		__asm__ volatile("wfe" ::: "memory");
}

static uint64_t smc(uint64_t fn)
{
	register uint64_t x0 __asm__("x0") = fn;
	register uint64_t x1 __asm__("x1") = 0, x2 __asm__("x2") = 0, x3 __asm__("x3") = 0;
	__asm__ volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) : : "memory");
	return x0;
}

/* ---- PMIC ---- */

/* A 16-bit fuel gauge value and its shadow copy, read until they agree. */
static int fg_word(unsigned addr, unsigned copy, uint16_t *out)
{
	for (int i = 0; i < 4; i++) {
		uint8_t lo, hi, lo2, hi2;
		if (spmi_read(2, addr, &lo) || spmi_read(2, addr + 1, &hi) ||
		    spmi_read(2, copy, &lo2) || spmi_read(2, copy + 1, &hi2))
			return -1;
		if (lo == lo2 && hi == hi2) {
			*out = (uint16_t)(lo | hi << 8);
			return 0;
		}
	}
	return -1;
}

/* As xnu's SweetBattery (sweetbattery=2): FG gen4 monotonic SOC, VBATT, IBATT; SMB5 status. */
static int read_battery(struct battery *b)
{
	uint8_t soc = 0, copy = 1, usb, status;
	uint16_t v, i;
	for (int k = 0; k < 4 && soc != copy; k++)
		if (spmi_read(2, 0x4009, &soc) || spmi_read(2, 0x400a, &copy))
			return -1;
	if (soc != copy || fg_word(0x41a0, 0x41a6, &v) || spmi_read(2, 0x1310, &usb) ||
	    spmi_read(2, 0x1006, &status))
		return -1;
	b->percent = soc == 255 ? 100 : soc == 0 ? 0 : ((soc - 1u) * 98u + 126u) / 253u + 1u;
	b->mv = (unsigned)(((uint64_t)v * 122070u) / 1000000u);
	b->ma = 0;
	if (!fg_word(0x41a2, 0x41a8, &i))	/* signed, 488.281 uA, positive = discharging */
		b->ma = (int)(-((int64_t)(int16_t)i * 488281) / 1000000);
	b->usb = (usb & 0x10) != 0;		/* USBIN_PLUGIN_RT_STS */
	b->state = status & 7;
	return 0;
}

/* xnu 0167's SweetBattery::configureCharging: the USB ICL by the source, FCC 2 A. */
static void configure_charging(void)
{
	uint8_t apsd_status, apsd, rp, icl, options, override, load, fcc;
	if (spmi_read(2, 0x1307, &apsd_status) || !(apsd_status & 1) || spmi_read(2, 0x1308, &apsd) ||
	    spmi_read(2, 0x1506, &rp) || spmi_read(2, 0x1370, &icl) || spmi_read(2, 0x1366, &options) ||
	    spmi_read(2, 0x1342, &override) || spmi_read(2, 0x1365, &load) || spmi_read(2, 0x1061, &fcc))
		return;
	unsigned ma = (apsd & 0x01) ? 0 :		/* SDP: a computer's port keeps its 500 mA */
	    (rp & 0x02) ? 2000 :			/* Type-C Rp 3.0 A */
	    ((apsd & 0x0e) || (rp & 0x04)) ? 1500 :	/* DCP, CDP, OCP; Rp 1.5 A */
	    1000;
	if (ma && (icl != ma / 50 || !(options & 1) || (override & 1) || !(load & 0x10))) {
		spmi_write(2, 0x1370, (uint8_t)(ma / 50));
		spmi_write(2, 0x1366, options | 1);	/* USBIN_MODE_CHG */
		spmi_write(2, 0x1342, override & ~1);	/* no ICL override */
		spmi_write(2, 0x1365, load | 0x10);	/* ICL_OVERRIDE_AFTER_APSD */
	}
	if (fcc < 2000 / 50)
		spmi_write(2, 0x1061, 2000 / 50);
}

static int power_key(void)
{
	uint8_t rt;
	return !spmi_read(0, 0x810, &rt) && (rt & 1);	/* PON_RT_STS: KPDPWR_N */
}

/* qpnp_pon_reset_config(PON_POWER_OFF_SHUTDOWN) on PM8150, then PSCI: TZ drops PS_HOLD. */
static void __attribute__((noreturn)) power_off(void)
{
	uint8_t ctl, ctl2;
	if (!spmi_read(0, 0x85b, &ctl2) && !spmi_write(0, 0x85b, ctl2 & ~0x80)) {	/* PS_HOLD_RST_CTL2: off */
		wait_ms(1);					/* 10 sleep clocks, +50% */
		if (!spmi_read(0, 0x85a, &ctl))
			spmi_write(0, 0x85a, (ctl & ~0x0f) | 0x04);	/* PS_HOLD_RST_CTL: SHUTDOWN */
		spmi_write(0, 0x85b, ctl2 | 0x80);
	}
	smc(PSCI_SYSTEM_OFF);
	smc(PSCI_SYSTEM_RESET);
	for (;;)
		__asm__ volatile("wfi");
}

/* ---- drawing ---- */

static void fill(int x, int y, int w, int h, uint32_t c)
{
	for (int j = y; j < y + h; j++) {
		volatile uint32_t *line = con_fb + j * (FB_STRIDE / 4) + x;
		for (int i = 0; i < w; i++)
			line[i] = c;
	}
}

static void text(int y, const char *s, int scale, uint32_t fg)
{
	int n = 0;
	while (s[n])
		n++;
	int x0 = (FB_W - n * 8 * scale) / 2;
	for (int k = 0; k < n; k++) {
		char ch = s[k] < 0x20 || s[k] > 0x7e ? '?' : s[k];
		const unsigned char *g = font8x16[ch - 0x20];
		for (int gy = 0; gy < 16; gy++)
			for (int gx = 0; gx < 8; gx++)
				if (g[gy] & (0x80 >> gx))
					fill(x0 + (k * 8 + gx) * scale, y + gy * scale, scale, scale, fg);
	}
}

#define AREA_Y	700	/* everything drawn lies in rows AREA_Y .. AREA_Y + AREA_H */
#define AREA_H	2000
#define BODY_X	480
#define BODY_Y	900
#define BODY_W	480
#define BODY_H	1000
#define EDGE	20
#define GAP	24

static void draw(const struct battery *b, const char *note)
{
	char pct[5];
	int n = 0;
	unsigned p = b->percent > 100 ? 100 : b->percent;
	if (p >= 100)
		pct[n++] = '1';
	if (p >= 10)
		pct[n++] = (char)('0' + p / 10 % 10);
	pct[n++] = (char)('0' + p % 10);
	pct[n++] = '%';
	pct[n] = 0;

	fill(0, AREA_Y, FB_W, AREA_H, BLACK);
	fill(BODY_X + BODY_W / 2 - 100, BODY_Y - 60, 200, 60, GREY);		/* the terminal */
	fill(BODY_X, BODY_Y, BODY_W, EDGE, GREY);
	fill(BODY_X, BODY_Y + BODY_H - EDGE, BODY_W, EDGE, GREY);
	fill(BODY_X, BODY_Y, EDGE, BODY_H, GREY);
	fill(BODY_X + BODY_W - EDGE, BODY_Y, EDGE, BODY_H, GREY);
	int inner = BODY_H - 2 * (EDGE + GAP), level = inner * (int)p / 100;
	if (level)
		fill(BODY_X + EDGE + GAP, BODY_Y + BODY_H - EDGE - GAP - level, BODY_W - 2 * (EDGE + GAP), level,
		     p < 15 ? LOW_RED : CHARGE_GREEN);
	text(2000, pct, 14, WHITE);
	const char *state = b->state == 5 || p >= 100 ? "Charged" :
	    b->state >= 1 && b->state <= 4 ? "Charging" : "Not charging";
	text(2300, note ? note : state, 5, note ? LOW_RED : GREY);
	text(2500, "Hold power to start iOS", 3, GREY);
}

/* ---- the screen ---- */

int charger_requested(uintptr_t dtb)
{
	return fdt_chosen_has(dtb, "bootargs", "androidboot.mode=charger");
}

void charger_mode(void)
{
	/* The timer's event stream (CNTKCTL_EL1.EVNTEN, every 2^15 ticks, 1.7 ms): wfe sleeps between. */
	uint64_t cntkctl = rsr("cntkctl_el1");
	wsr("cntkctl_el1", (cntkctl & ~0xf4UL) | (1u << 2) | (14u << 4));
	__asm__ volatile("isb");

	struct battery b = { 0 }, shown = { 0 };
	const char *note = 0;
	con_clear(BLACK);				/* ABL's splash logo goes */
	read_battery(&b);
	configure_charging();
	draw(&b, 0);
	shown = b;
	uint64_t t = now_ms(), until = t + SHOW_MS, held = 0, polled = t, configured = t;
	int on = 1, unplugged = 0, was_down = power_key();

	for (;;) {
		wait_ms(100);
		t = now_ms();
		int down = power_key();
		if (down && !was_down) {		/* a press: show the battery */
			held = t;
			until = t + SHOW_MS;
			note = 0;
			if (!on) {
				read_battery(&b);
				draw(&b, 0);
				shown = b;
				on = 1;
			}
		}
		if (down && held && t - held >= HOLD_MS) {	/* held: start iOS, unless too empty */
			read_battery(&b);
			if (b.percent >= 3 && b.mv >= 3600) {
				draw(&b, "Starting iOS");
				wsr("cntkctl_el1", cntkctl);
				__asm__ volatile("isb");
				return;
			}
			note = "Battery too low";
			draw(&b, note);
			shown = b;
			held = 0;
			until = t + SHOW_MS;
			on = 1;
		}
		if (!down)
			held = 0;
		was_down = down;

		if (t - polled >= 1000) {
			polled = t;
			if (read_battery(&b) == 0) {
				unplugged = b.usb ? 0 : unplugged + 1;
				if (unplugged >= UNPLUGGED_S) {
					draw(&b, "Powering off");
					wait_ms(1000);
					fill(0, AREA_Y, FB_W, AREA_H, BLACK);
					power_off();
				}
				if (on && (b.percent != shown.percent || b.state != shown.state)) {
					draw(&b, note);
					shown = b;
				}
			}
		}
		if (t - configured >= 5000) {	/* APSD may finish, or the charger change, after we start */
			configured = t;
			configure_charging();
		}
		if (on && t >= until) {
			fill(0, AREA_Y, FB_W, AREA_H, BLACK);
			on = 0;
		}
	}
}
#else
int charger_requested(uintptr_t dtb) { (void)dtb; return 0; }
void charger_mode(void) { }
#endif
