/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MT6358_BATTERY_H
#define _MT6358_BATTERY_H

#include <linux/types.h>

#if IS_ENABLED(CONFIG_KUNIT)
int mt6358_bat_current_ua(u16 raw);
s64 mt6358_bat_car_uah(u16 car_lo, u16 car_hi);
int mt6358_bat_soc_permille(int soc0_permille, s64 car0_uah, s64 car_uah, int full_uah);
#endif

#endif
