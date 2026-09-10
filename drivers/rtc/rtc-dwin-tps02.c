// SPDX-License-Identifier: GPL-2.0
/* DWIN TPS02 RTC */

#include <linux/completion.h>
#include <linux/jiffies.h>
#include <linux/mfd/dwin-tps02.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/rtc.h>

#define TPS02_RTC_TIMEOUT	(2 * HZ)
#define TPS02_RTC_MAX_AGE_MS	2000
#define TPS02_RTC_SET_TOLERANCE	2

struct tps02_rtc {
	struct dwin_tps02 *tps02;
	struct notifier_block notifier;
	struct completion sample;
};

static int tps02_rtc_notifier(struct notifier_block *notifier,
				      unsigned long event, void *data)
{
	struct tps02_rtc *rtc = container_of(notifier, struct tps02_rtc,
					      notifier);

	if (event == DWIN_TPS02_EVENT_RTC)
		complete(&rtc->sample);

	return NOTIFY_OK;
}

static bool tps02_rtc_is_fresh(const struct dwin_tps02_rtc *sample)
{
	return dwin_tps02_rtc_fresh(sample, ktime_get(), TPS02_RTC_MAX_AGE_MS);
}

static int tps02_rtc_read_time(struct device *dev, struct rtc_time *tm)
{
	struct tps02_rtc *rtc = dev_get_drvdata(dev);
	struct dwin_tps02_rtc sample;
	unsigned long timeout = jiffies + TPS02_RTC_TIMEOUT;
	long remaining;
	int ret;

	for (;;) {
		ret = dwin_tps02_get_rtc(rtc->tps02, &sample);
		if (!ret && !sample.valid)
			return -EINVAL;
		if (!ret && tps02_rtc_is_fresh(&sample)) {
			*tm = sample.tm;
			return 0;
		}

		if (time_after_eq(jiffies, timeout))
			return ret ? ret : -ETIME;

		reinit_completion(&rtc->sample);
		ret = dwin_tps02_get_rtc(rtc->tps02, &sample);
		if (!ret && !sample.valid)
			return -EINVAL;
		if (!ret && tps02_rtc_is_fresh(&sample)) {
			*tm = sample.tm;
			return 0;
		}

		remaining = timeout - jiffies;
		if (remaining <= 0)
			return -ETIMEDOUT;
		remaining = wait_for_completion_timeout(&rtc->sample, remaining);
		if (!remaining && time_after_eq(jiffies, timeout))
			return -ETIMEDOUT;
	}
}

static int tps02_rtc_set_time(struct device *dev, struct rtc_time *tm)
{
	struct tps02_rtc *rtc = dev_get_drvdata(dev);
	struct dwin_tps02_rtc sample;
	u8 data[6];
	time64_t requested;
	ktime_t sent;
	unsigned long timeout;
	u64 generation = 0;
	long remaining;
	int ret;

	if (tm->tm_year < 100 || tm->tm_year > 199 || rtc_valid_tm(tm))
		return -EINVAL;

	requested = rtc_tm_to_time64(tm);
	ret = dwin_tps02_get_rtc(rtc->tps02, &sample);
	if (!ret)
		generation = sample.generation;

	data[0] = tm->tm_year - 100;
	data[1] = tm->tm_mon;
	data[2] = tm->tm_mday;
	data[3] = tm->tm_hour;
	data[4] = tm->tm_min;
	data[5] = tm->tm_sec;

	reinit_completion(&rtc->sample);
	sent = ktime_get();
	ret = dwin_tps02_send(rtc->tps02, 0x02, data, sizeof(data));
	if (ret)
		return ret;

	timeout = jiffies + TPS02_RTC_TIMEOUT;
	for (;;) {
		ret = dwin_tps02_get_rtc(rtc->tps02, &sample);
		if (!ret && dwin_tps02_rtc_matches(&sample, requested, sent,
						 generation, TPS02_RTC_SET_TOLERANCE))
			return 0;

		if (time_after_eq(jiffies, timeout))
			return -ETIMEDOUT;

		reinit_completion(&rtc->sample);
		ret = dwin_tps02_get_rtc(rtc->tps02, &sample);
		if (!ret && dwin_tps02_rtc_matches(&sample, requested, sent,
						 generation, TPS02_RTC_SET_TOLERANCE))
			return 0;

		remaining = timeout - jiffies;
		if (remaining <= 0)
			return -ETIMEDOUT;
		wait_for_completion_timeout(&rtc->sample, remaining);
	}
}

static const struct rtc_class_ops tps02_rtc_ops = {
	.read_time = tps02_rtc_read_time,
	.set_time = tps02_rtc_set_time,
};

static void tps02_rtc_unregister_notifier(void *data)
{
	struct tps02_rtc *rtc = data;

	dwin_tps02_unregister_notifier(rtc->tps02, &rtc->notifier);
}

static int tps02_rtc_probe(struct platform_device *pdev)
{
	struct tps02_rtc *rtc;
	struct rtc_device *rtc_dev;
	int ret;

	rtc = devm_kzalloc(&pdev->dev, sizeof(*rtc), GFP_KERNEL);
	if (!rtc)
		return -ENOMEM;

	rtc->tps02 = dev_get_drvdata(pdev->dev.parent);
	if (!rtc->tps02)
		return -EPROBE_DEFER;

	init_completion(&rtc->sample);
	rtc->notifier.notifier_call = tps02_rtc_notifier;
	ret = dwin_tps02_register_notifier(rtc->tps02, &rtc->notifier);
	if (ret)
		return ret;

	ret = devm_add_action_or_reset(&pdev->dev,
			tps02_rtc_unregister_notifier, rtc);
	if (ret)
		return ret;

	rtc_dev = devm_rtc_allocate_device(&pdev->dev);
	if (IS_ERR(rtc_dev))
		return PTR_ERR(rtc_dev);

	rtc_dev->ops = &tps02_rtc_ops;
	rtc_dev->range_min = RTC_TIMESTAMP_BEGIN_2000;
	rtc_dev->range_max = RTC_TIMESTAMP_END_2099;
	platform_set_drvdata(pdev, rtc);

	return devm_rtc_register_device(rtc_dev);
}

static const struct of_device_id tps02_rtc_match[] = {
	{ .compatible = "dwin,tps02-rtc" },
	{}
};
MODULE_DEVICE_TABLE(of, tps02_rtc_match);

static struct platform_driver tps02_rtc_driver = {
	.probe = tps02_rtc_probe,
	.driver = {
		.name = "dwin-tps02-rtc",
		.of_match_table = tps02_rtc_match,
	},
};
module_platform_driver(tps02_rtc_driver);

MODULE_DESCRIPTION("DWIN TPS02 RTC driver");
MODULE_LICENSE("GPL");
