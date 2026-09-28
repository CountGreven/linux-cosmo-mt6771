// SPDX-License-Identifier: GPL-2.0-only
#include <linux/module.h>
#include <kunit/visibility.h>

#include "mt6358-battery.h"

VISIBLE_IF_KUNIT int mt6358_bat_current_ua(u16 raw) { return -1; }
EXPORT_SYMBOL_IF_KUNIT(mt6358_bat_current_ua);
VISIBLE_IF_KUNIT s64 mt6358_bat_car_uah(u16 car_lo, u16 car_hi) { return -1; }
EXPORT_SYMBOL_IF_KUNIT(mt6358_bat_car_uah);
VISIBLE_IF_KUNIT int mt6358_bat_soc_permille(int soc0_permille, s64 car0_uah, s64 car_uah,
					     int full_uah) { return -1; }
EXPORT_SYMBOL_IF_KUNIT(mt6358_bat_soc_permille);

MODULE_LICENSE("GPL");
