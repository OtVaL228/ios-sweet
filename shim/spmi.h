#pragma once
#include <stdint.h>
/* PMIC arbiter v5 access, EE0, MMU off. Returns 0 on success. */
int pon_set_reboot_bootloader(void);
/* One byte of a PMIC register (sid, 16-bit address) through the observer channel. */
int spmi_read(unsigned sid, unsigned addr, uint8_t *out);
/*
 * One byte to a PMIC register through a write channel EE0 owns. Only the
 * registers the shim needs (spmi.c): PON reboot reason and PS_HOLD reset type
 * on PM8150, the SMB5 input/charge current limits on PM8150B.
 */
int spmi_write(unsigned sid, unsigned addr, uint8_t value);
