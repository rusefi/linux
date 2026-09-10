// SPDX-License-Identifier: GPL-2.0-only
/* Shared UART transport for the DWIN 36-series TPS02 controller. */
#include <linux/mfd/core.h>
#include <linux/mfd/dwin-tps02.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/pm.h>
#include <linux/serdev.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

#include "dwin-tps02-protocol.h"

struct dwin_tps02 {
	struct serdev_device *serdev;
	struct mutex tx_lock;
	spinlock_t cache_lock;
	struct tps02_parser parser;
	struct dwin_tps02_rtc rtc;
	struct atomic_notifier_head notifier;
};

static void tps02_frame(void *context, const u8 *frame)
{
	struct dwin_tps02 *tps = context;
	struct dwin_tps02_touch touch;
	struct dwin_tps02_rtc sample = {};
	unsigned long flags;

	if (tps02_decode_touch(frame, &touch)) {
		atomic_notifier_call_chain(&tps->notifier, DWIN_TPS02_EVENT_TOUCH,
					  &touch);
	} else if (frame[2] == 11 && frame[3] == 0x12) {
		sample.valid = tps02_decode_calendar(frame, &sample.tm);
		sample.received = ktime_get();
		spin_lock_irqsave(&tps->cache_lock, flags);
		sample.generation = tps->rtc.generation + 1;
		tps->rtc = sample;
		spin_unlock_irqrestore(&tps->cache_lock, flags);
		atomic_notifier_call_chain(&tps->notifier, DWIN_TPS02_EVENT_RTC,
					  &sample);
	}
}

static size_t tps02_receive_buf(struct serdev_device *serdev, const u8 *data,
			       size_t len)
{
	struct dwin_tps02 *tps = serdev_device_get_drvdata(serdev);
	u32 errors = tps->parser.checksum_errors + tps->parser.length_errors;

	tps02_receive(&tps->parser, data, len, tps02_frame, tps);
	if (errors != tps->parser.checksum_errors + tps->parser.length_errors)
		dev_warn_ratelimited(&serdev->dev,
			"invalid serial frames: checksum=%u length=%u\n",
			tps->parser.checksum_errors, tps->parser.length_errors);
	return len;
}

static const struct serdev_device_ops tps02_serdev_ops = {
	.receive_buf = tps02_receive_buf,
	.write_wakeup = serdev_device_write_wakeup,
};

int dwin_tps02_send(struct dwin_tps02 *tps, u8 type, const u8 *data, size_t len)
{
	u8 frame[TPS02_MAX_FRAME];
	int size, ret;

	size = tps02_encode(frame, type, data, len);
	if (size < 0)
		return size;

	/* Verification waits belong to the child, outside this shared TX lock. */
	mutex_lock(&tps->tx_lock);
	ret = serdev_device_write(tps->serdev, frame, size, msecs_to_jiffies(200));
	if (ret == size)
		serdev_device_wait_until_sent(tps->serdev, msecs_to_jiffies(200));
	else
		serdev_device_write_flush(tps->serdev);
	mutex_unlock(&tps->tx_lock);

	return ret == size ? 0 : ret < 0 ? ret : -EIO;
}
EXPORT_SYMBOL_GPL(dwin_tps02_send);

int dwin_tps02_register_notifier(struct dwin_tps02 *tps,
			       struct notifier_block *notifier)
{
	return atomic_notifier_chain_register(&tps->notifier, notifier);
}
EXPORT_SYMBOL_GPL(dwin_tps02_register_notifier);

int dwin_tps02_unregister_notifier(struct dwin_tps02 *tps,
				 struct notifier_block *notifier)
{
	return atomic_notifier_chain_unregister(&tps->notifier, notifier);
}
EXPORT_SYMBOL_GPL(dwin_tps02_unregister_notifier);

int dwin_tps02_get_rtc(struct dwin_tps02 *tps, struct dwin_tps02_rtc *sample)
{
	unsigned long flags;

	spin_lock_irqsave(&tps->cache_lock, flags);
	*sample = tps->rtc;
	spin_unlock_irqrestore(&tps->cache_lock, flags);
	return sample->generation ? 0 : -ENODATA;
}
EXPORT_SYMBOL_GPL(dwin_tps02_get_rtc);

static const struct mfd_cell tps02_cells[] = {
	{ .name = "dwin-tps02-touch", .of_compatible = "dwin,tps02-touch" },
	{ .name = "dwin-tps02-rtc", .of_compatible = "dwin,tps02-rtc" },
	{ .name = "dwin-tps02-beeper", .of_compatible = "dwin,tps02-beeper" },
	{ .name = "dwin-tps02-backlight", .of_compatible = "dwin,tps02-backlight" },
};

static int tps02_probe(struct serdev_device *serdev)
{
	struct dwin_tps02 *tps;
	int ret;

	tps = devm_kzalloc(&serdev->dev, sizeof(*tps), GFP_KERNEL);
	if (!tps)
		return -ENOMEM;
	tps->serdev = serdev;
	mutex_init(&tps->tx_lock);
	spin_lock_init(&tps->cache_lock);
	ATOMIC_INIT_NOTIFIER_HEAD(&tps->notifier);
	serdev_device_set_drvdata(serdev, tps);
	serdev_device_set_client_ops(serdev, &tps02_serdev_ops);

	ret = devm_serdev_device_open(&serdev->dev, serdev);
	if (ret)
		return dev_err_probe(&serdev->dev, ret, "cannot open serial port\n");
	if (serdev_device_set_baudrate(serdev, 115200) != 115200)
		return dev_err_probe(&serdev->dev, -EINVAL, "cannot set 115200 baud\n");
	serdev_device_set_flow_control(serdev, false);
	ret = serdev_device_set_parity(serdev, SERDEV_PARITY_NONE);
	if (ret)
		return ret;

	/* devres removes all children before closing the shared serial port. */
	return devm_mfd_add_devices(&serdev->dev, PLATFORM_DEVID_AUTO,
				    tps02_cells, ARRAY_SIZE(tps02_cells),
				    NULL, 0, NULL);
}

static int tps02_resume(struct device *dev)
{
	struct dwin_tps02 *tps = dev_get_drvdata(dev);
	unsigned long flags;

	/* ktime_get() excludes suspend; a pre-suspend sample cannot remain fresh. */
	spin_lock_irqsave(&tps->cache_lock, flags);
	tps->rtc.valid = false;
	spin_unlock_irqrestore(&tps->cache_lock, flags);
	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(tps02_pm_ops, NULL, tps02_resume);

static const struct of_device_id tps02_match[] = {
	{ .compatible = "dwin,tps02" },
	{}
};
MODULE_DEVICE_TABLE(of, tps02_match);

static struct serdev_device_driver tps02_driver = {
	.probe = tps02_probe,
	.driver = {
		.name = "dwin-tps02",
		.of_match_table = tps02_match,
		.pm = pm_sleep_ptr(&tps02_pm_ops),
	},
};
module_serdev_device_driver(tps02_driver);

MODULE_DESCRIPTION("DWIN TPS02 serial multifunction controller");
MODULE_LICENSE("GPL");
