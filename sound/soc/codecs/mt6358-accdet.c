// SPDX-License-Identifier: GPL-2.0
#include <linux/module.h>
#include <kunit/visibility.h>

#include "mt6358-accdet.h"

VISIBLE_IF_KUNIT int mt6358_accdet_key(const struct mt6358_accdet_key_thr *thr, int mv)
{
	return -1;
}
EXPORT_SYMBOL_IF_KUNIT(mt6358_accdet_key);

VISIBLE_IF_KUNIT void mt6358_accdet_next(enum mt6358_accdet_state state, int jack,
					 unsigned int ab, struct mt6358_accdet_step *next)
{
	next->state = state;
	next->jack = -1;
	next->key = MT6358_ACCDET_KEY_NONE;
}
EXPORT_SYMBOL_IF_KUNIT(mt6358_accdet_next);

MODULE_LICENSE("GPL");
