// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  Copyright (c) 2026 Andrey Gusakov
 */

/*
 * DWIN serial touchscreen driver.
 */

#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/input.h>
#include <linux/serio.h>

#define DRIVER_DESC	"DWIN HMI touchscreen driver"

MODULE_AUTHOR("Andrey Gusakov <dron0gus@gmail.com>");
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_LICENSE("GPL");

/*
 * Definitions & global arrays.
 */

#define	DWIN_MAX_LENGTH	32

#define DWIN_LEAD_BYTE0	0x5A
#define DWIN_LEAD_BYTE1	0xA5

/*
 * Per-touchscreen data.
 */

struct dwin {
	struct input_dev *dev;
	struct serio *serio;
	int idx;
	int len;
	unsigned char data[DWIN_MAX_LENGTH];
	char phys[32];
};

static void dwin_process_packet(struct serio *serio, struct dwin *dwin)
{
	/* TODO: add crc check at last byte */
	if (dwin->len == 7) {
		unsigned char *d = &dwin->data[3];
		uint16_t x = (d[2] << 8) | d[3];
		uint16_t y = (d[4] << 8) | d[5];

		input_report_abs(dwin->dev, ABS_X, x);
		input_report_abs(dwin->dev, ABS_Y, y);
		input_report_key(dwin->dev, BTN_TOUCH, d[1] == 0x03);
		input_sync(dwin->dev);
	} else if (dwin->len == 11) {
		/* TODO: support packets with len = 11 */
	} else {
		dev_err(&serio->dev, "unsuppoerted packet len %d\n", dwin->len);
	}
}

static irqreturn_t dwin_interrupt(struct serio *serio,
		unsigned char data, unsigned int flags)
{
	struct dwin *dwin = serio_get_drvdata(serio);

	if (dwin->idx == 0) {
		if (data != DWIN_LEAD_BYTE0) {
			goto sync_err;
		}
	} else if (dwin->idx == 1) {
		if (data != DWIN_LEAD_BYTE1) {
			goto sync_err;
		}
	} else if (dwin->idx == 2) {
		if (data + 3 > DWIN_MAX_LENGTH) {
			dev_dbg(&serio->dev, "Too long packed %d\n", data);
			goto sync_err;
		}
		/* expected len without header */
		dwin->len = data;
	}

	dwin->data[dwin->idx++] = data;

	if (dwin->idx < dwin->len + 3) {
		return IRQ_HANDLED;
	}

	dwin_process_packet(serio, dwin);

sync_err:
	dwin->idx = 0;
	dwin->len = 0;
	return IRQ_HANDLED;
}

/*
 * dwin_disconnect() is the opposite of dwin_connect()
 */

static void dwin_disconnect(struct serio *serio)
{
	struct dwin *dwin = serio_get_drvdata(serio);

	input_get_device(dwin->dev);
	input_unregister_device(dwin->dev);
	serio_close(serio);
	serio_set_drvdata(serio, NULL);
	input_put_device(dwin->dev);
	kfree(dwin);
}

/*
 * dwin_connect() is the routine that is called when someone adds a
 * new serio device that supports Gunze protocol and registers it as
 * an input device.
 */

static int dwin_connect(struct serio *serio, struct serio_driver *drv)
{
	struct dwin *dwin;
	struct input_dev *input_dev;
	int err;

	dwin = kzalloc(sizeof(*dwin), GFP_KERNEL);
	input_dev = input_allocate_device();
	if (!dwin || !input_dev) {
		err = -ENOMEM;
		goto fail1;
	}

	dwin->serio = serio;
	dwin->dev = input_dev;
	snprintf(dwin->phys, sizeof(serio->phys), "%s/input0", serio->phys);

	input_dev->name = "DWIN DGUSII TouchScreen";
	input_dev->phys = dwin->phys;
	input_dev->id.bustype = BUS_RS232;
	input_dev->id.vendor = SERIO_DWIN;
	input_dev->id.product = 0x0001;
	input_dev->id.version = 0x0100;
	input_dev->dev.parent = &serio->dev;
	input_dev->evbit[0] = BIT_MASK(EV_KEY) | BIT_MASK(EV_ABS);
	input_dev->keybit[BIT_WORD(BTN_TOUCH)] = BIT_MASK(BTN_TOUCH);
	input_set_abs_params(input_dev, ABS_X, 0, 1023, 0, 0);
	input_set_abs_params(input_dev, ABS_Y, 0, 599, 0, 0);

	serio_set_drvdata(serio, dwin);

	err = serio_open(serio, drv);
	if (err)
		goto fail2;

	err = input_register_device(dwin->dev);
	if (err)
		goto fail3;

	return 0;

 fail3:	serio_close(serio);
 fail2:	serio_set_drvdata(serio, NULL);
 fail1:	input_free_device(input_dev);
	kfree(dwin);
	return err;
}

/*
 * The serio driver structure.
 */

static const struct serio_device_id dwin_serio_ids[] = {
	{
		.type	= SERIO_RS232,
		.proto	= SERIO_DWIN,
		.id	= SERIO_ANY,
		.extra	= SERIO_ANY,
	},
	{ 0 }
};

MODULE_DEVICE_TABLE(serio, dwin_serio_ids);

static struct serio_driver dwin_drv = {
	.driver		= {
		.name	= "dwin",
	},
	.description	= DRIVER_DESC,
	.id_table	= dwin_serio_ids,
	.interrupt	= dwin_interrupt,
	.connect	= dwin_connect,
	.disconnect	= dwin_disconnect,
};

module_serio_driver(dwin_drv);
