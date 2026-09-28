/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _AF6133E_H
#define _AF6133E_H

#include <linux/types.h>

/* Per-axis gain in 1/100 and cross-axis coefficients in 1/100 from the self test */
struct af6133e_cal {
	s16 gain[3];
	s16 comp[4];
};

#if IS_ENABLED(CONFIG_KUNIT)
void af6133e_cal_default(struct af6133e_cal *cal);
bool af6133e_cal_from_bist(struct af6133e_cal *cal, const s16 bist_x[3],
			   const s16 bist_y[3], const s16 bist_z[3]);
void af6133e_compensate(const struct af6133e_cal *cal, const s16 raw[3],
			s32 out[3]);
#endif

#endif
