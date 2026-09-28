/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _MT6358_ACCDET_H_
#define _MT6358_ACCDET_H_

#include <linux/types.h>
#include <sound/jack.h>
#include <sound/soc.h>

int mt6358_accdet_enable_jack_detect(struct snd_soc_component *component,
				     struct snd_soc_jack *jack);

enum mt6358_accdet_state {
	MT6358_ACCDET_PLUG_OUT,
	MT6358_ACCDET_MIC_BIAS,		/* 4-pole, no button */
	MT6358_ACCDET_HOOK_SWITCH,	/* 3-pole, or a 4-pole with a button held */
};

enum mt6358_accdet_key_event {
	MT6358_ACCDET_KEY_NONE,
	MT6358_ACCDET_KEY_PRESS,
	MT6358_ACCDET_KEY_RELEASE,
};

struct mt6358_accdet_step {
	enum mt6358_accdet_state state;
	int jack;			/* SND_JACK_HEADPHONE, SND_JACK_HEADSET or 0 */
	enum mt6358_accdet_key_event key;
};

/* Button voltage boundaries in mV: below mid is play, below up is volume up, below down is volume down */
struct mt6358_accdet_key_thr {
	u32 mid;
	u32 up;
	u32 down;
};

#if IS_ENABLED(CONFIG_KUNIT)
int mt6358_accdet_key(const struct mt6358_accdet_key_thr *thr, int mv);
void mt6358_accdet_next(enum mt6358_accdet_state state, int jack, unsigned int ab,
			struct mt6358_accdet_step *next);
#endif

#endif
