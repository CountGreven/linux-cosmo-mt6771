// SPDX-License-Identifier: GPL-2.0
#include <kunit/test.h>
#include <sound/jack.h>

#include "mt6358-accdet.h"

static const struct mt6358_accdet_key_thr thr = { .mid = 80, .up = 220, .down = 400 };

static void mt6358_accdet_test_key(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, mt6358_accdet_key(&thr, 0), SND_JACK_BTN_0);
	KUNIT_EXPECT_EQ(test, mt6358_accdet_key(&thr, 79), SND_JACK_BTN_0);
	KUNIT_EXPECT_EQ(test, mt6358_accdet_key(&thr, 80), SND_JACK_BTN_2);
	KUNIT_EXPECT_EQ(test, mt6358_accdet_key(&thr, 219), SND_JACK_BTN_2);
	KUNIT_EXPECT_EQ(test, mt6358_accdet_key(&thr, 220), SND_JACK_BTN_1);
	KUNIT_EXPECT_EQ(test, mt6358_accdet_key(&thr, 399), SND_JACK_BTN_1);
	KUNIT_EXPECT_EQ(test, mt6358_accdet_key(&thr, 400), 0);
}

static void expect_step(struct kunit *test, enum mt6358_accdet_state state, int jack,
			unsigned int ab, enum mt6358_accdet_state nstate, int njack,
			enum mt6358_accdet_key_event key)
{
	struct mt6358_accdet_step s;

	mt6358_accdet_next(state, jack, ab, &s);
	KUNIT_EXPECT_EQ_MSG(test, s.state, nstate, "state %d ab %u", state, ab);
	KUNIT_EXPECT_EQ_MSG(test, s.jack, njack, "state %d ab %u", state, ab);
	KUNIT_EXPECT_EQ_MSG(test, s.key, key, "state %d ab %u", state, ab);
}

static void mt6358_accdet_test_fsm(struct kunit *test)
{
	/* plug-in: AB 00 is a 3-pole plug, AB 01 a 4-pole one */
	expect_step(test, MT6358_ACCDET_PLUG_OUT, 0, 0, MT6358_ACCDET_HOOK_SWITCH,
		    SND_JACK_HEADPHONE, MT6358_ACCDET_KEY_NONE);
	expect_step(test, MT6358_ACCDET_PLUG_OUT, 0, 1, MT6358_ACCDET_MIC_BIAS,
		    SND_JACK_HEADSET, MT6358_ACCDET_KEY_NONE);
	expect_step(test, MT6358_ACCDET_PLUG_OUT, 0, 3, MT6358_ACCDET_PLUG_OUT, 0,
		    MT6358_ACCDET_KEY_NONE);
	/* a button pulls the mic line to AB 00, releasing it returns to 01 */
	expect_step(test, MT6358_ACCDET_MIC_BIAS, SND_JACK_HEADSET, 0, MT6358_ACCDET_HOOK_SWITCH,
		    SND_JACK_HEADSET, MT6358_ACCDET_KEY_PRESS);
	expect_step(test, MT6358_ACCDET_HOOK_SWITCH, SND_JACK_HEADSET, 1, MT6358_ACCDET_MIC_BIAS,
		    SND_JACK_HEADSET, MT6358_ACCDET_KEY_RELEASE);
	expect_step(test, MT6358_ACCDET_MIC_BIAS, SND_JACK_HEADSET, 1, MT6358_ACCDET_MIC_BIAS,
		    SND_JACK_HEADSET, MT6358_ACCDET_KEY_NONE);
	expect_step(test, MT6358_ACCDET_HOOK_SWITCH, SND_JACK_HEADPHONE, 0,
		    MT6358_ACCDET_HOOK_SWITCH, SND_JACK_HEADPHONE, MT6358_ACCDET_KEY_NONE);
	/* AB 11 in a plugged state only moves the state machine; EINT reports the removal */
	expect_step(test, MT6358_ACCDET_MIC_BIAS, SND_JACK_HEADSET, 3, MT6358_ACCDET_PLUG_OUT,
		    SND_JACK_HEADSET, MT6358_ACCDET_KEY_NONE);
	expect_step(test, MT6358_ACCDET_HOOK_SWITCH, SND_JACK_HEADPHONE, 3,
		    MT6358_ACCDET_PLUG_OUT, SND_JACK_HEADPHONE, MT6358_ACCDET_KEY_NONE);
}

static struct kunit_case mt6358_accdet_test_cases[] = {
	KUNIT_CASE(mt6358_accdet_test_key),
	KUNIT_CASE(mt6358_accdet_test_fsm),
	{}
};

static struct kunit_suite mt6358_accdet_test_suite = {
	.name = "mt6358-accdet",
	.test_cases = mt6358_accdet_test_cases,
};
kunit_test_suite(mt6358_accdet_test_suite);

MODULE_DESCRIPTION("KUnit tests for the MT6358 accdet state machine");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");
