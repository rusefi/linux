/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_MFD_DWIN_TPS02_H
#define _LINUX_MFD_DWIN_TPS02_H

#include <linux/ktime.h>
#include <linux/notifier.h>
#include <linux/rtc.h>

#define DWIN_TPS02_EVENT_TOUCH 1
#define DWIN_TPS02_EVENT_RTC 2

struct dwin_tps02_touch {
	u8 event;
	u16 x;
	u16 y;
};

struct dwin_tps02_rtc {
	struct rtc_time tm;
	u64 generation;
	ktime_t received;
	bool valid;
};

struct dwin_tps02;

static inline bool dwin_tps02_rtc_fresh(const struct dwin_tps02_rtc *sample,
				      ktime_t now, unsigned int max_age_ms)
{
	s64 age = ktime_ms_delta(now, sample->received);

	return sample->valid && age >= 0 && age <= max_age_ms;
}

static inline bool dwin_tps02_rtc_matches(const struct dwin_tps02_rtc *sample,
					time64_t requested, ktime_t sent,
					u64 generation, unsigned int tolerance)
{
	time64_t expected, observed;
	struct rtc_time tm = sample->tm;
	s64 elapsed;

	if (!sample->valid || sample->generation <= generation)
		return false;
	elapsed = ktime_ms_delta(sample->received, sent);
	if (elapsed < 0)
		return false;
	expected = requested + div_s64(elapsed, MSEC_PER_SEC);
	observed = rtc_tm_to_time64(&tm);
	return observed >= expected - tolerance && observed <= expected + tolerance;
}

/* RX notifiers run in atomic context. Unregister before freeing their state. */
int dwin_tps02_register_notifier(struct dwin_tps02 *tps,
			       struct notifier_block *notifier);
int dwin_tps02_unregister_notifier(struct dwin_tps02 *tps,
				 struct notifier_block *notifier);
/* Send serializes one frame only; callers must not hold atomic-context locks. */
int dwin_tps02_send(struct dwin_tps02 *tps, u8 type, const u8 *data, size_t len);
int dwin_tps02_get_rtc(struct dwin_tps02 *tps, struct dwin_tps02_rtc *sample);

#endif
