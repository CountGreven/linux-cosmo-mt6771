/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Decision logic of the Fn + power-line combiner, kept free of the input
 * core so that it can be unit tested.
 */

#ifndef _FN_POWER_COMBINER_H
#define _FN_POWER_COMBINER_H

#include <linux/types.h>

struct fnp_state {
	bool fn_held;
	bool power_active;
};

/*
 * Handle a value of the power-line key (0 release, 1 press, 2 repeat).
 * Returns true when the event is a KEY_POWER event, false when it is a plain
 * line-key event. A press made while Fn is held, and everything up to its
 * release, is power whatever Fn does in between.
 */
static inline bool fnp_line_is_power(struct fnp_state *s, int value)
{
	if (value) {
		if (s->fn_held || s->power_active)
			s->power_active = true;
		return s->power_active;
	}

	if (!s->power_active)
		return false;

	s->power_active = false;
	return true;
}

#endif /* _FN_POWER_COMBINER_H */
