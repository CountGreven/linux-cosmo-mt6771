// SPDX-License-Identifier: GPL-2.0-only
/*
 * Fn + power-line key combiner.
 *
 * On some keyboards the power-key line of the PMIC doubles as a key of the
 * keyboard (the Planet Cosmo Communicator wires it to Esc). This driver
 * watches the keyboard for the Fn key and the PMIC key device for that line
 * key: a press made while Fn is held becomes KEY_POWER on an input device of
 * its own, and the line event is dropped so that user space sees exactly one
 * of the two. The same handler drops every keyboard event while a lid switch
 * reports closed, because the screen presses the keys then.
 */

#include <linux/input.h>
#include <linux/kfifo.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_wakeup.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>

#include "fn-power-combiner.h"

#define FNP_WAKE_HOLD_MS	500

enum fnp_role {
	FNP_KEYBOARD,
	FNP_LINE,
	FNP_LID,
	FNP_NUM_ROLES,
};

static const char * const fnp_role_props[FNP_NUM_ROLES] = {
	[FNP_KEYBOARD]	= "keyboard",
	[FNP_LINE]	= "power-key",
	[FNP_LID]	= "lid-switch",
};

struct fnp {
	struct device *dev;
	struct input_dev *out;
	struct input_handler handler;
	struct device_node *nodes[FNP_NUM_ROLES];
	struct work_struct work;
	u32 fn_code;
	u32 line_code;

	spinlock_t lock;	/* protects everything below */
	struct fnp_state state;
	bool lid_closed;
	bool flushing;
	struct input_dev *kbd;
	DECLARE_KFIFO(queue, u8, 16);
};

struct fnp_handle {
	struct input_handle handle;
	enum fnp_role role;
};

static void fnp_work(struct work_struct *work)
{
	struct fnp *fnp = container_of(work, struct fnp, work);
	DECLARE_BITMAP(held, KEY_CNT);
	struct input_dev *kbd;
	unsigned long flags;
	bool closed;
	unsigned int code;
	u8 value;

	for (;;) {
		spin_lock_irqsave(&fnp->lock, flags);
		value = 0;
		if (!kfifo_get(&fnp->queue, &value)) {
			spin_unlock_irqrestore(&fnp->lock, flags);
			break;
		}
		spin_unlock_irqrestore(&fnp->lock, flags);

		input_report_key(fnp->out, KEY_POWER, value);
		input_sync(fnp->out);
	}

	spin_lock_irqsave(&fnp->lock, flags);
	closed = fnp->lid_closed;
	kbd = fnp->kbd;
	if (kbd && closed)
		fnp->flushing = true;
	spin_unlock_irqrestore(&fnp->lock, flags);

	if (!kbd)
		return;

	if (!closed) {
		spin_lock_irqsave(&fnp->lock, flags);
		fnp->state.fn_held = test_bit(fnp->fn_code, kbd->key);
		spin_unlock_irqrestore(&fnp->lock, flags);
		return;
	}

	/* Keys held when the lid closed never see a release from the matrix */
	bitmap_copy(held, kbd->key, KEY_CNT);
	for_each_set_bit(code, held, KEY_CNT)
		input_event(kbd, EV_KEY, code, 0);
	input_sync(kbd);

	spin_lock_irqsave(&fnp->lock, flags);
	fnp->flushing = false;
	spin_unlock_irqrestore(&fnp->lock, flags);
}

static bool fnp_filter(struct input_handle *handle, unsigned int type,
		       unsigned int code, int value)
{
	struct fnp *fnp = handle->private;
	struct fnp_handle *fh = container_of(handle, struct fnp_handle, handle);
	bool drop = false;

	spin_lock(&fnp->lock);

	switch (fh->role) {
	case FNP_KEYBOARD:
		if (fnp->lid_closed) {
			drop = !(fnp->flushing &&
				 (type == EV_SYN || (type == EV_KEY && !value)));
		} else if (type == EV_KEY && code == fnp->fn_code) {
			fnp->state.fn_held = !!value;
		}
		break;
	case FNP_LINE:
		if (type == EV_KEY && code == fnp->line_code) {
			drop = fnp_line_is_power(&fnp->state, value);
			if (drop && value != 2) {
				kfifo_put(&fnp->queue, !!value);
				if (value)
					pm_wakeup_event(fnp->dev, FNP_WAKE_HOLD_MS);
				schedule_work(&fnp->work);
			}
		}
		break;
	case FNP_LID:
		if (type == EV_SW && code == SW_LID && fnp->lid_closed != !!value) {
			fnp->lid_closed = !!value;
			fnp->state.fn_held = false;
			schedule_work(&fnp->work);
		}
		break;
	default:
		break;
	}

	spin_unlock(&fnp->lock);

	return drop;
}

static int fnp_role_of(struct fnp *fnp, struct input_dev *dev)
{
	struct device_node *np = dev->dev.parent ?
				 dev_of_node(dev->dev.parent) : NULL;
	int role;

	if (!np)
		return -ENODEV;

	for (role = 0; role < FNP_NUM_ROLES; role++) {
		if (fnp->nodes[role] != np)
			continue;
		if (role == FNP_LID && !test_bit(SW_LID, dev->swbit))
			continue;
		if (role != FNP_LID && !test_bit(EV_KEY, dev->evbit))
			continue;
		return role;
	}

	return -ENODEV;
}

static bool fnp_match(struct input_handler *handler, struct input_dev *dev)
{
	struct fnp *fnp = handler->private;

	return dev != fnp->out && fnp_role_of(fnp, dev) >= 0;
}

static int fnp_connect(struct input_handler *handler, struct input_dev *dev,
		       const struct input_device_id *id)
{
	struct fnp *fnp = handler->private;
	struct fnp_handle *fh;
	unsigned long flags;
	int role, error;

	role = fnp_role_of(fnp, dev);
	if (role < 0)
		return role;

	fh = kzalloc_obj(*fh);
	if (!fh)
		return -ENOMEM;

	fh->role = role;
	fh->handle.dev = dev;
	fh->handle.handler = handler;
	fh->handle.name = "fn-power-combiner";
	fh->handle.private = fnp;

	error = input_register_handle(&fh->handle);
	if (error)
		goto err_free;

	error = input_open_device(&fh->handle);
	if (error)
		goto err_unregister;

	spin_lock_irqsave(&fnp->lock, flags);
	if (role == FNP_KEYBOARD) {
		fnp->kbd = dev;
		fnp->state.fn_held = test_bit(fnp->fn_code, dev->key);
	} else if (role == FNP_LID) {
		fnp->lid_closed = test_bit(SW_LID, dev->sw);
	}
	spin_unlock_irqrestore(&fnp->lock, flags);

	if (role != FNP_LINE)
		schedule_work(&fnp->work);

	return 0;

err_unregister:
	input_unregister_handle(&fh->handle);
err_free:
	kfree(fh);
	return error;
}

static void fnp_disconnect(struct input_handle *handle)
{
	struct fnp *fnp = handle->private;
	struct fnp_handle *fh = container_of(handle, struct fnp_handle, handle);
	unsigned long flags;

	if (fh->role == FNP_KEYBOARD) {
		spin_lock_irqsave(&fnp->lock, flags);
		fnp->kbd = NULL;
		spin_unlock_irqrestore(&fnp->lock, flags);
		cancel_work_sync(&fnp->work);
	}

	input_close_device(handle);
	input_unregister_handle(handle);
	kfree(fh);
}

static const struct input_device_id fnp_ids[] = {
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT,
		.evbit = { BIT_MASK(EV_KEY) },
	},
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT,
		.evbit = { BIT_MASK(EV_SW) },
	},
	{ }
};

static void fnp_teardown(void *data)
{
	struct fnp *fnp = data;
	int role;

	input_unregister_handler(&fnp->handler);
	cancel_work_sync(&fnp->work);

	for (role = 0; role < FNP_NUM_ROLES; role++)
		of_node_put(fnp->nodes[role]);
}

static int fnp_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct fnp *fnp;
	int role, error;

	fnp = devm_kzalloc(dev, sizeof(*fnp), GFP_KERNEL);
	if (!fnp)
		return -ENOMEM;

	fnp->dev = dev;
	spin_lock_init(&fnp->lock);
	INIT_WORK(&fnp->work, fnp_work);
	INIT_KFIFO(fnp->queue);

	fnp->fn_code = KEY_LEFTMETA;
	fnp->line_code = KEY_ESC;
	device_property_read_u32(dev, "linux,fn-code", &fnp->fn_code);
	device_property_read_u32(dev, "linux,line-code", &fnp->line_code);
	if (fnp->fn_code >= KEY_CNT || fnp->line_code >= KEY_CNT)
		return -EINVAL;

	for (role = 0; role < FNP_NUM_ROLES; role++)
		fnp->nodes[role] = of_parse_phandle(dev->of_node,
						    fnp_role_props[role], 0);
	if (!fnp->nodes[FNP_KEYBOARD] || !fnp->nodes[FNP_LINE]) {
		error = dev_err_probe(dev, -EINVAL,
				      "keyboard and power-key are required\n");
		goto err_put;
	}

	fnp->out = devm_input_allocate_device(dev);
	if (!fnp->out) {
		error = -ENOMEM;
		goto err_put;
	}

	fnp->out->name = "fn-power-combiner";
	fnp->out->id.bustype = BUS_HOST;
	input_set_capability(fnp->out, EV_KEY, KEY_POWER);

	error = input_register_device(fnp->out);
	if (error)
		goto err_put;

	error = devm_device_init_wakeup(dev);
	if (error)
		goto err_put;

	fnp->handler.name = "fn-power-combiner";
	fnp->handler.filter = fnp_filter;
	fnp->handler.match = fnp_match;
	fnp->handler.connect = fnp_connect;
	fnp->handler.disconnect = fnp_disconnect;
	fnp->handler.id_table = fnp_ids;
	fnp->handler.private = fnp;

	error = input_register_handler(&fnp->handler);
	if (error)
		goto err_put;

	return devm_add_action_or_reset(dev, fnp_teardown, fnp);

err_put:
	for (role = 0; role < FNP_NUM_ROLES; role++)
		of_node_put(fnp->nodes[role]);
	return error;
}

static const struct of_device_id fnp_of_match[] = {
	{ .compatible = "linux,fn-power-combiner" },
	{ }
};
MODULE_DEVICE_TABLE(of, fnp_of_match);

static struct platform_driver fnp_driver = {
	.probe = fnp_probe,
	.driver = {
		.name = "fn-power-combiner",
		.of_match_table = fnp_of_match,
	},
};
module_platform_driver(fnp_driver);

MODULE_DESCRIPTION("Fn + power-line key combiner and lid keyboard mute");
MODULE_LICENSE("GPL");
