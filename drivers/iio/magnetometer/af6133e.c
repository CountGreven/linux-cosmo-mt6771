// SPDX-License-Identifier: GPL-2.0-only
#include <linux/module.h>
#include <kunit/visibility.h>

#include "af6133e.h"

VISIBLE_IF_KUNIT void af6133e_cal_default(struct af6133e_cal *cal)
{
}
EXPORT_SYMBOL_IF_KUNIT(af6133e_cal_default);

VISIBLE_IF_KUNIT bool af6133e_cal_from_bist(struct af6133e_cal *cal, const s16 bist_x[3],
					    const s16 bist_y[3], const s16 bist_z[3])
{
	return false;
}
EXPORT_SYMBOL_IF_KUNIT(af6133e_cal_from_bist);

VISIBLE_IF_KUNIT void af6133e_compensate(const struct af6133e_cal *cal, const s16 raw[3],
					 s32 out[3])
{
	out[0] = out[1] = out[2] = 0;
}
EXPORT_SYMBOL_IF_KUNIT(af6133e_compensate);

MODULE_LICENSE("GPL");
