/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LEDS_AW9524_H
#define _LEDS_AW9524_H

#include <linux/types.h>

#if IS_ENABLED(CONFIG_KUNIT)
int aw9524_dim_reg(unsigned int pin);
bool aw9524_any_lit(const u8 *dim);
#endif

#endif
