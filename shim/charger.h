#pragma once
#include <stdint.h>
/* ABL booted us for a charger (androidboot.mode=charger in /chosen/bootargs). */
int charger_requested(uintptr_t dtb);
/* The charger screen; returns when the power key is held to start iOS (or never: power off). */
void charger_mode(void);
