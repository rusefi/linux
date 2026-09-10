// SPDX-License-Identifier: GPL-2.0
/* DWIN TPS02 backlight */

#include <linux/backlight.h>
#include <linux/mfd/dwin-tps02.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>

struct tps02_backlight {
	struct dwin_tps02 *tps02;
	struct mutex lock;
	u8 requested;
	int hw_error;
};

static int tps02_backlight_update_status(struct backlight_device *backlight)
{
	struct tps02_backlight *tps02 = bl_get_data(backlight);
	u8 brightness;
	int ret;

	brightness = backlight_get_brightness(backlight);

	mutex_lock(&tps02->lock);
	ret = dwin_tps02_send(tps02->tps02, 0x04, &brightness,
			       sizeof(brightness));
	tps02->hw_error = ret;
	if (!ret)
		tps02->requested = brightness;
	mutex_unlock(&tps02->lock);

	return ret;
}

static int tps02_backlight_get_brightness(struct backlight_device *backlight)
{
	struct tps02_backlight *tps02 = bl_get_data(backlight);
	int brightness;

	mutex_lock(&tps02->lock);
	/* The protocol has no brightness readback: this is last requested state. */
	brightness = tps02->hw_error ? tps02->hw_error : tps02->requested;
	mutex_unlock(&tps02->lock);

	return brightness;
}

static const struct backlight_ops tps02_backlight_ops = {
	.options = BL_CORE_SUSPENDRESUME,
	.update_status = tps02_backlight_update_status,
	.get_brightness = tps02_backlight_get_brightness,
};

static int tps02_backlight_probe(struct platform_device *pdev)
{
	struct backlight_properties props = {
		.type = BACKLIGHT_RAW,
		.max_brightness = 100,
		.brightness = 100,
	};
	struct tps02_backlight *tps02;
	struct backlight_device *backlight;

	tps02 = devm_kzalloc(&pdev->dev, sizeof(*tps02), GFP_KERNEL);
	if (!tps02)
		return -ENOMEM;

	tps02->tps02 = dev_get_drvdata(pdev->dev.parent);
	if (!tps02->tps02)
		return -EPROBE_DEFER;

	mutex_init(&tps02->lock);
	tps02->requested = props.brightness;
	backlight = devm_backlight_device_register(&pdev->dev, "dwin-tps02",
					     &pdev->dev, tps02,
					     &tps02_backlight_ops, &props);
	if (IS_ERR(backlight))
		return PTR_ERR(backlight);

	platform_set_drvdata(pdev, tps02);
	return backlight_update_status(backlight);
}

static const struct of_device_id tps02_backlight_match[] = {
	{ .compatible = "dwin,tps02-backlight" },
	{}
};
MODULE_DEVICE_TABLE(of, tps02_backlight_match);

static struct platform_driver tps02_backlight_driver = {
	.probe = tps02_backlight_probe,
	.driver = {
		.name = "dwin-tps02-backlight",
		.of_match_table = tps02_backlight_match,
	},
};
module_platform_driver(tps02_backlight_driver);

MODULE_DESCRIPTION("DWIN TPS02 backlight driver");
MODULE_LICENSE("GPL");
