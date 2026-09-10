// SPDX-License-Identifier: GPL-2.0
/* DWIN TPS02 beeper */

#include <linux/input.h>
#include <linux/jiffies.h>
#include <linux/mfd/dwin-tps02.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>

#include <kunit/static_stub.h>

#if IS_ENABLED(CONFIG_INPUT_DWIN_TPS02_BEEPER_KUNIT_TEST)
#include <kunit/test.h>
#endif

#define TPS02_BEEPER_MAX_DURATION	HZ

struct tps02_beeper {
	struct dwin_tps02 *tps02;
	struct input_dev *input;
	struct work_struct transmit_work;
	struct delayed_work off_work;
	spinlock_t lock;
	unsigned long off_deadline;
	u64 generation;
	bool enabled;
	bool shutting_down;
};

static int tps02_beeper_send(struct tps02_beeper *beeper, u8 command)
{
	KUNIT_STATIC_STUB_REDIRECT(tps02_beeper_send, beeper, command);

	return dwin_tps02_send(beeper->tps02, 0x03, &command,
				sizeof(command));
}

static void tps02_beeper_transmit(struct work_struct *work)
{
	struct tps02_beeper *beeper = container_of(work, struct tps02_beeper,
						      transmit_work);
	unsigned long flags;
	u64 generation;
	u8 command;
	int ret;

	for (;;) {
		spin_lock_irqsave(&beeper->lock, flags);
		generation = beeper->generation;
		command = beeper->enabled &&
			time_before(jiffies, beeper->off_deadline) ? 0xff : 0x00;
		spin_unlock_irqrestore(&beeper->lock, flags);

		ret = tps02_beeper_send(beeper, command);
		if (ret)
			dev_warn_ratelimited(&beeper->input->dev,
					    "beeper transmit failed: %d\n", ret);

		/* A concurrent OFF/retrigger must not be lost behind this write. */
		spin_lock_irqsave(&beeper->lock, flags);
		if (generation == beeper->generation) {
			spin_unlock_irqrestore(&beeper->lock, flags);
			return;
		}
		spin_unlock_irqrestore(&beeper->lock, flags);
	}
}

static void tps02_beeper_off(struct work_struct *work)
{
	struct tps02_beeper *beeper = container_of(to_delayed_work(work),
						      struct tps02_beeper, off_work);
	unsigned long flags, remaining;

	spin_lock_irqsave(&beeper->lock, flags);
	if (time_before(jiffies, beeper->off_deadline)) {
		remaining = beeper->off_deadline - jiffies;
		spin_unlock_irqrestore(&beeper->lock, flags);
		mod_delayed_work(system_wq, &beeper->off_work, remaining);
		return;
	}

	beeper->enabled = false;
	beeper->generation++;
	spin_unlock_irqrestore(&beeper->lock, flags);
	schedule_work(&beeper->transmit_work);
}

static int tps02_beeper_event(struct input_dev *input, unsigned int type,
			      unsigned int code, int value)
{
	struct tps02_beeper *beeper = input_get_drvdata(input);
	unsigned long flags;

	if (type != EV_SND || code != SND_BELL)
		return -EINVAL;

	spin_lock_irqsave(&beeper->lock, flags);
	if (beeper->shutting_down) {
		spin_unlock_irqrestore(&beeper->lock, flags);
		return -ENODEV;
	}

	if (!value) {
		beeper->enabled = false;
		beeper->generation++;
		cancel_delayed_work(&beeper->off_work);
		schedule_work(&beeper->transmit_work);
		spin_unlock_irqrestore(&beeper->lock, flags);
		return 0;
	}

	beeper->enabled = true;
	beeper->generation++;
	beeper->off_deadline = jiffies + TPS02_BEEPER_MAX_DURATION;
	mod_delayed_work(system_wq, &beeper->off_work,
			 TPS02_BEEPER_MAX_DURATION);
	schedule_work(&beeper->transmit_work);
	spin_unlock_irqrestore(&beeper->lock, flags);

	return 0;
}

static void tps02_beeper_stop(struct tps02_beeper *beeper)
{
	unsigned long flags;
	u8 command = 0;

	cancel_delayed_work_sync(&beeper->off_work);
	spin_lock_irqsave(&beeper->lock, flags);
	beeper->enabled = false;
	beeper->generation++;
	spin_unlock_irqrestore(&beeper->lock, flags);
	cancel_work_sync(&beeper->transmit_work);
	tps02_beeper_send(beeper, command);
}

static void tps02_beeper_cleanup(void *data)
{
	struct tps02_beeper *beeper = data;
	unsigned long flags;

	spin_lock_irqsave(&beeper->lock, flags);
	beeper->shutting_down = true;
	spin_unlock_irqrestore(&beeper->lock, flags);
	tps02_beeper_stop(beeper);
}

static int tps02_beeper_suspend(struct device *dev)
{
	tps02_beeper_stop(dev_get_drvdata(dev));
	return 0;
}

static const struct dev_pm_ops tps02_beeper_pm_ops = {
	.suspend = tps02_beeper_suspend,
};

static int tps02_beeper_probe(struct platform_device *pdev)
{
	struct tps02_beeper *beeper;
	struct input_dev *input;
	int ret;

	beeper = devm_kzalloc(&pdev->dev, sizeof(*beeper), GFP_KERNEL);
	if (!beeper)
		return -ENOMEM;

	beeper->tps02 = dev_get_drvdata(pdev->dev.parent);
	if (!beeper->tps02)
		return -EPROBE_DEFER;

	input = devm_input_allocate_device(&pdev->dev);
	if (!input)
		return -ENOMEM;

	spin_lock_init(&beeper->lock);
	INIT_WORK(&beeper->transmit_work, tps02_beeper_transmit);
	INIT_DELAYED_WORK(&beeper->off_work, tps02_beeper_off);
	beeper->input = input;
	input->name = "DWIN TPS02 Beeper";
	input->id.bustype = BUS_RS232;
	input->event = tps02_beeper_event;
	/* A short-lived evdev write requests a pulse; closing it is not OFF. */
	input_set_drvdata(input, beeper);
	input_set_capability(input, EV_SND, SND_BELL);

	ret = input_register_device(input);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, beeper);
	return devm_add_action_or_reset(&pdev->dev, tps02_beeper_cleanup, beeper);
}

static void tps02_beeper_remove(struct platform_device *pdev)
{
	tps02_beeper_cleanup(platform_get_drvdata(pdev));
}

static const struct of_device_id tps02_beeper_match[] = {
	{ .compatible = "dwin,tps02-beeper" },
	{}
};
MODULE_DEVICE_TABLE(of, tps02_beeper_match);

static struct platform_driver tps02_beeper_driver = {
	.probe = tps02_beeper_probe,
	.remove = tps02_beeper_remove,
	.driver = {
		.name = "dwin-tps02-beeper",
		.of_match_table = tps02_beeper_match,
		.pm = pm_ptr(&tps02_beeper_pm_ops),
	},
};
module_platform_driver(tps02_beeper_driver);

MODULE_DESCRIPTION("DWIN TPS02 beeper driver");
MODULE_LICENSE("GPL");

#if IS_ENABLED(CONFIG_INPUT_DWIN_TPS02_BEEPER_KUNIT_TEST)
struct tps02_beeper_test_context {
	struct tps02_beeper beeper;
	u8 commands[3];
	unsigned int command_count;
	bool request_off_during_on;
};

static int tps02_beeper_test_send(struct tps02_beeper *beeper, u8 command)
{
	struct kunit *test = kunit_get_current_test();
	struct tps02_beeper_test_context *context = test->priv;

	KUNIT_ASSERT_LT(test, context->command_count,
			ARRAY_SIZE(context->commands));
	context->commands[context->command_count++] = command;

	if (command == 0xff && context->request_off_during_on) {
		unsigned long flags;

		context->request_off_during_on = false;
		spin_lock_irqsave(&beeper->lock, flags);
		beeper->enabled = false;
		beeper->generation++;
		spin_unlock_irqrestore(&beeper->lock, flags);
	}

	return 0;
}

static int tps02_beeper_test_init(struct kunit *test)
{
	struct tps02_beeper_test_context *context;

	context = kunit_kzalloc(test, sizeof(*context), GFP_KERNEL);
	if (!context)
		return -ENOMEM;

	spin_lock_init(&context->beeper.lock);
	INIT_WORK(&context->beeper.transmit_work, tps02_beeper_transmit);
	INIT_DELAYED_WORK(&context->beeper.off_work, tps02_beeper_off);
	test->priv = context;
	kunit_activate_static_stub(test, tps02_beeper_send, tps02_beeper_test_send);

	return 0;
}

static void tps02_beeper_test_exit(struct kunit *test)
{
	struct tps02_beeper_test_context *context = test->priv;

	cancel_delayed_work_sync(&context->beeper.off_work);
	cancel_work_sync(&context->beeper.transmit_work);
}

static void tps02_beeper_off_during_send_test(struct kunit *test)
{
	struct tps02_beeper_test_context *context = test->priv;

	context->beeper.enabled = true;
	context->beeper.off_deadline = jiffies + TPS02_BEEPER_MAX_DURATION;
	context->beeper.generation = 1;
	context->request_off_during_on = true;

	tps02_beeper_transmit(&context->beeper.transmit_work);

	KUNIT_EXPECT_EQ(test, context->command_count, 2U);
	KUNIT_EXPECT_EQ(test, context->commands[0], (u8)0xff);
	KUNIT_EXPECT_EQ(test, context->commands[1], (u8)0x00);
	KUNIT_EXPECT_FALSE(test, context->beeper.enabled);
}

static void tps02_beeper_late_expiry_retrigger_test(struct kunit *test)
{
	struct tps02_beeper_test_context *context = test->priv;

	context->beeper.enabled = true;
	context->beeper.generation = 1;
	context->beeper.off_deadline = jiffies + TPS02_BEEPER_MAX_DURATION;

	/* Simulate the callback queued for an old pulse after a retrigger. */
	tps02_beeper_off(&context->beeper.off_work.work);
	tps02_beeper_transmit(&context->beeper.transmit_work);

	KUNIT_EXPECT_TRUE(test, context->beeper.enabled);
	KUNIT_EXPECT_EQ(test, context->beeper.generation, 1ULL);
	KUNIT_EXPECT_EQ(test, context->command_count, 1U);
	KUNIT_EXPECT_EQ(test, context->commands[0], (u8)0xff);
}

static void tps02_beeper_cleanup_test(struct kunit *test)
{
	struct tps02_beeper_test_context *context = test->priv;

	context->beeper.enabled = true;
	context->beeper.off_deadline = jiffies + TPS02_BEEPER_MAX_DURATION;
	mod_delayed_work(system_wq, &context->beeper.off_work,
			 TPS02_BEEPER_MAX_DURATION);

	tps02_beeper_cleanup(&context->beeper);

	KUNIT_EXPECT_TRUE(test, context->beeper.shutting_down);
	KUNIT_EXPECT_FALSE(test, context->beeper.enabled);
	KUNIT_EXPECT_FALSE(test, delayed_work_pending(&context->beeper.off_work));
	KUNIT_EXPECT_FALSE(test, work_pending(&context->beeper.transmit_work));
	KUNIT_EXPECT_EQ(test, context->command_count, 1U);
	KUNIT_EXPECT_EQ(test, context->commands[0], (u8)0x00);
}

static struct kunit_case tps02_beeper_test_cases[] = {
	KUNIT_CASE(tps02_beeper_off_during_send_test),
	KUNIT_CASE(tps02_beeper_late_expiry_retrigger_test),
	KUNIT_CASE(tps02_beeper_cleanup_test),
	{}
};

static struct kunit_suite tps02_beeper_test_suite = {
	.name = "dwin-tps02-beeper",
	.init = tps02_beeper_test_init,
	.exit = tps02_beeper_test_exit,
	.test_cases = tps02_beeper_test_cases,
};
kunit_test_suite(tps02_beeper_test_suite);
#endif
