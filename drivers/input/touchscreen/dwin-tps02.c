// SPDX-License-Identifier: GPL-2.0
/* DWIN TPS02 touchscreen */

#include <linux/input.h>
#include <linux/input/touchscreen.h>
#include <linux/mfd/dwin-tps02.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

struct tps02_touch {
	struct dwin_tps02 *tps02;
	struct notifier_block notifier;
	struct input_dev *input;
	struct touchscreen_properties properties;
};

static int tps02_touch_notifier(struct notifier_block *notifier,
				unsigned long event, void *data)
{
	struct tps02_touch *touch = container_of(notifier, struct tps02_touch,
						     notifier);
	const struct dwin_tps02_touch *report = data;

	if (event != DWIN_TPS02_EVENT_TOUCH)
		return NOTIFY_DONE;
	if (report->x > touch->properties.max_x ||
	    report->y > touch->properties.max_y)
		return NOTIFY_DONE;

	touchscreen_report_pos(touch->input, &touch->properties,
			       report->x, report->y, false);
	input_report_key(touch->input, BTN_TOUCH, report->event != 2);
	input_sync(touch->input);

	return NOTIFY_OK;
}

static int tps02_touch_probe(struct platform_device *pdev)
{
	struct dwin_tps02 *tps02 = dev_get_drvdata(pdev->dev.parent);
	struct tps02_touch *touch;
	struct input_dev *input;
	u32 max_x = 1280, max_y = 800;
	int ret;

	if (!tps02)
		return -EPROBE_DEFER;

	touch = devm_kzalloc(&pdev->dev, sizeof(*touch), GFP_KERNEL);
	if (!touch)
		return -ENOMEM;

	of_property_read_u32(pdev->dev.of_node, "touchscreen-size-x", &max_x);
	of_property_read_u32(pdev->dev.of_node, "touchscreen-size-y", &max_y);
	if (!max_x || !max_y || max_x > 65536 || max_y > 65536)
		return -EINVAL;

	input = input_allocate_device();
	if (!input)
		return -ENOMEM;

	touch->tps02 = tps02;
	touch->input = input;
	input->name = "DWIN TPS02 Touchscreen";
	input->dev.parent = &pdev->dev;
	input->id.bustype = BUS_RS232;
	__set_bit(INPUT_PROP_DIRECT, input->propbit);
	input_set_abs_params(input, ABS_X, 0, max_x - 1, 0, 0);
	input_set_abs_params(input, ABS_Y, 0, max_y - 1, 0, 0);
	touchscreen_parse_properties(input, false, &touch->properties);
	input_set_capability(input, EV_KEY, BTN_TOUCH);

	ret = input_register_device(input);
	if (ret)
		goto err_free_input;

	touch->notifier.notifier_call = tps02_touch_notifier;
	ret = dwin_tps02_register_notifier(tps02, &touch->notifier);
	if (ret)
		goto err_unregister_input;

	platform_set_drvdata(pdev, touch);
	return 0;

err_unregister_input:
	input_unregister_device(input);
	return ret;

err_free_input:
	input_free_device(input);
	return ret;
}

static void tps02_touch_remove(struct platform_device *pdev)
{
	struct tps02_touch *touch = platform_get_drvdata(pdev);

	dwin_tps02_unregister_notifier(touch->tps02, &touch->notifier);
	input_unregister_device(touch->input);
}

static const struct of_device_id tps02_touch_match[] = {
	{ .compatible = "dwin,tps02-touch" },
	{}
};
MODULE_DEVICE_TABLE(of, tps02_touch_match);

static struct platform_driver tps02_touch_driver = {
	.probe = tps02_touch_probe,
	.remove = tps02_touch_remove,
	.driver = {
		.name = "dwin-tps02-touch",
		.of_match_table = tps02_touch_match,
	},
};
module_platform_driver(tps02_touch_driver);

MODULE_DESCRIPTION("DWIN TPS02 touchscreen driver");
MODULE_LICENSE("GPL");
