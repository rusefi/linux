// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/test.h>

#include "dwin-tps02-protocol.h"

/* DMT12800T101_36WTC, captured 2026-09-10; guide 36-series rev. 03. */
static const u8 rtc_report[] = {
	0x5a, 0xa5, 0x0b, 0x12, 0x1a, 0x08, 0x0a,
	0x01, 0x13, 0x21, 0x19, 0x02, 0xd9, 0x72,
};
static const u8 touch_press[] = {
	0x5a, 0xa5, 0x07, 0x11, 0x01, 0x04, 0xee, 0x00, 0x12, 0x1d,
};
static const u8 touch_release[] = {
	0x5a, 0xa5, 0x07, 0x11, 0x02, 0x00, 0x06, 0x00, 0x05, 0x25,
};

struct frame_capture {
	unsigned int count;
	u8 last[TPS02_MAX_FRAME];
};

static void capture_frame(void *context, const u8 *frame)
{
	struct frame_capture *capture = context;

	capture->count++;
	memcpy(capture->last, frame, frame[2] + 3);
}

static void tps02_encoder_test(struct kunit *test)
{
	const u8 rtc_data[] = { 0x1a, 0x08, 0x0a, 0x13, 0x21, 0x19 };
	const u8 rtc_set[] = {
		0x5a, 0xa5, 0x08, 0x02, 0x1a, 0x08, 0x0a, 0x13, 0x21, 0x19, 0x83,
	};
	const u8 beep_on[] = { 0x5a, 0xa5, 0x03, 0x03, 0xff, 0x05 };
	const u8 beep_off[] = { 0x5a, 0xa5, 0x03, 0x03, 0x00, 0x06 };
	u8 frame[TPS02_MAX_FRAME], value = 0xff;

	KUNIT_ASSERT_EQ(test, tps02_encode(frame, 0x02, rtc_data, sizeof(rtc_data)),
			(int)sizeof(rtc_set));
	KUNIT_EXPECT_MEMEQ(test, frame, rtc_set, sizeof(rtc_set));
	KUNIT_ASSERT_EQ(test, tps02_encode(frame, 0x03, &value, 1), 6);
	KUNIT_EXPECT_MEMEQ(test, frame, beep_on, sizeof(beep_on));
	value = 0;
	KUNIT_ASSERT_EQ(test, tps02_encode(frame, 0x03, &value, 1), 6);
	KUNIT_EXPECT_MEMEQ(test, frame, beep_off, sizeof(beep_off));
	KUNIT_EXPECT_EQ(test, tps02_encode(frame, 0, NULL, TPS02_MAX_LEN), -EMSGSIZE);
}

static void tps02_split_stream_test(struct kunit *test)
{
	u8 stream[sizeof(rtc_report) + sizeof(touch_press)];
	size_t split;

	memcpy(stream, rtc_report, sizeof(rtc_report));
	memcpy(stream + sizeof(rtc_report), touch_press, sizeof(touch_press));
	for (split = 0; split <= sizeof(stream); split++) {
		struct tps02_parser parser = {};
		struct frame_capture capture = {};

		tps02_receive(&parser, stream, split, capture_frame, &capture);
		tps02_receive(&parser, stream + split, sizeof(stream) - split,
			      capture_frame, &capture);
		KUNIT_EXPECT_EQ(test, capture.count, 2U);
		KUNIT_EXPECT_EQ(test, parser.count, 0);
		KUNIT_EXPECT_EQ(test, parser.checksum_errors, 0U);
		KUNIT_EXPECT_MEMEQ(test, capture.last, touch_press, sizeof(touch_press));
	}
}

static void tps02_checksum_recovery_test(struct kunit *test)
{
	struct tps02_parser parser = {};
	struct frame_capture capture = {};
	u8 damaged[sizeof(rtc_report)];

	memcpy(damaged, rtc_report, sizeof(damaged));
	damaged[13] ^= 1;
	tps02_receive(&parser, damaged, sizeof(damaged), capture_frame, &capture);
	KUNIT_EXPECT_EQ(test, capture.count, 0U);
	KUNIT_EXPECT_EQ(test, parser.checksum_errors, 1U);
	tps02_receive(&parser, touch_release, sizeof(touch_release), capture_frame, &capture);
	KUNIT_EXPECT_EQ(test, capture.count, 1U);
	KUNIT_EXPECT_MEMEQ(test, capture.last, touch_release, sizeof(touch_release));
}

static void tps02_length_recovery_test(struct kunit *test)
{
	const u8 garbage[] = {
		0xaa, 0x5a, 0x5a, 0xa5, 0xff, 0x5a, 0xa5, 0x00,
		0x5a, 0xa5, 0x01, 0x5a,
	};
	struct tps02_parser parser = {};
	struct frame_capture capture = {};

	tps02_receive(&parser, garbage, sizeof(garbage), capture_frame, &capture);
	tps02_receive(&parser, rtc_report, sizeof(rtc_report), capture_frame, &capture);
	KUNIT_EXPECT_EQ(test, capture.count, 1U);
	KUNIT_EXPECT_EQ(test, parser.length_errors, 3U);
	KUNIT_EXPECT_MEMEQ(test, capture.last, rtc_report, sizeof(rtc_report));
}

static void tps02_truncated_recovery_test(struct kunit *test)
{
	struct tps02_parser parser = {};
	struct frame_capture capture = {};
	size_t i;

	/* A truncated packet consumes part of the following valid packet. */
	tps02_receive(&parser, rtc_report, 6, capture_frame, &capture);
	for (i = 0; i < sizeof(touch_press); i++)
		tps02_receive(&parser, touch_press + i, 1, capture_frame, &capture);
	KUNIT_EXPECT_EQ(test, capture.count, 1U);
	KUNIT_EXPECT_EQ(test, parser.checksum_errors, 1U);
	KUNIT_EXPECT_MEMEQ(test, capture.last, touch_press, sizeof(touch_press));
}

static void tps02_touch_test(struct kunit *test)
{
	struct dwin_tps02_touch touch;
	u8 frame[sizeof(touch_press)];

	KUNIT_ASSERT_TRUE(test, tps02_decode_touch(touch_press, &touch));
	KUNIT_EXPECT_EQ(test, touch.event, 1);
	KUNIT_EXPECT_EQ(test, touch.x, 1262);
	KUNIT_EXPECT_EQ(test, touch.y, 18);
	KUNIT_ASSERT_TRUE(test, tps02_decode_touch(touch_release, &touch));
	KUNIT_EXPECT_EQ(test, touch.event, 2);
	memcpy(frame, touch_press, sizeof(frame));
	frame[4] = 3;
	KUNIT_EXPECT_TRUE(test, tps02_decode_touch(frame, &touch));
	frame[4] = 0;
	KUNIT_EXPECT_FALSE(test, tps02_decode_touch(frame, &touch));
	frame[4] = 1;
	frame[3] = 0x12;
	KUNIT_EXPECT_FALSE(test, tps02_decode_touch(frame, &touch));
	KUNIT_EXPECT_FALSE(test, tps02_decode_touch(rtc_report, &touch));
}

static void tps02_calendar_test(struct kunit *test)
{
	struct rtc_time tm;
	u8 frame[sizeof(rtc_report)];

	KUNIT_ASSERT_TRUE(test, tps02_decode_calendar(rtc_report, &tm));
	KUNIT_EXPECT_EQ(test, tm.tm_year, 126);
	KUNIT_EXPECT_EQ(test, tm.tm_mon, 8);
	KUNIT_EXPECT_EQ(test, tm.tm_mday, 10);
	KUNIT_EXPECT_EQ(test, tm.tm_hour, 19);
	KUNIT_EXPECT_EQ(test, tm.tm_min, 33);
	KUNIT_EXPECT_EQ(test, tm.tm_sec, 25);
	/* Captured byte 7 is 1, but the actual weekday is Thursday (4). */
	KUNIT_EXPECT_EQ(test, tm.tm_wday, 4);
	memcpy(frame, rtc_report, sizeof(frame));
	frame[7] = 0xff;
	frame[11] = 0xff;
	frame[12] = 0xff;
	KUNIT_EXPECT_TRUE(test, tps02_decode_calendar(frame, &tm));
	KUNIT_EXPECT_EQ(test, tm.tm_wday, 4);
	frame[5] = 12;
	KUNIT_EXPECT_FALSE(test, tps02_decode_calendar(frame, &tm));
	frame[5] = 1;
	frame[6] = 29;
	KUNIT_EXPECT_FALSE(test, tps02_decode_calendar(frame, &tm));
	frame[4] = 24;
	KUNIT_EXPECT_TRUE(test, tps02_decode_calendar(frame, &tm));
	frame[4] = 100;
	KUNIT_EXPECT_FALSE(test, tps02_decode_calendar(frame, &tm));
}

static void tps02_rtc_freshness_test(struct kunit *test)
{
	struct dwin_tps02_rtc sample = {
		.received = ms_to_ktime(1000),
		.valid = true,
	};

	KUNIT_EXPECT_TRUE(test, dwin_tps02_rtc_fresh(&sample, ms_to_ktime(1000), 2000));
	KUNIT_EXPECT_TRUE(test, dwin_tps02_rtc_fresh(&sample, ms_to_ktime(3000), 2000));
	KUNIT_EXPECT_FALSE(test, dwin_tps02_rtc_fresh(&sample, ms_to_ktime(3001), 2000));
	KUNIT_EXPECT_FALSE(test, dwin_tps02_rtc_fresh(&sample, ms_to_ktime(999), 2000));
	sample.valid = false;
	KUNIT_EXPECT_FALSE(test, dwin_tps02_rtc_fresh(&sample, ms_to_ktime(1000), 2000));
}

static void tps02_rtc_verification_test(struct kunit *test)
{
	/* Two seconds after 2026-12-31 23:59:59 crosses year/day/minute boundaries. */
	struct rtc_time requested = {
		.tm_year = 126, .tm_mon = 11, .tm_mday = 31,
		.tm_hour = 23, .tm_min = 59, .tm_sec = 59,
	};
	struct dwin_tps02_rtc sample = {
		.tm = { .tm_year = 127, .tm_mon = 0, .tm_mday = 1, .tm_sec = 1 },
		.generation = 2,
		.received = ms_to_ktime(3000),
		.valid = true,
	};
	time64_t epoch = rtc_tm_to_time64(&requested);
	ktime_t sent = ms_to_ktime(1000);

	KUNIT_EXPECT_TRUE(test, dwin_tps02_rtc_matches(&sample, epoch, sent, 1, 2));
	KUNIT_EXPECT_FALSE(test, dwin_tps02_rtc_matches(&sample, epoch, sent, 2, 2));
	sample.received = ms_to_ktime(999);
	KUNIT_EXPECT_FALSE(test, dwin_tps02_rtc_matches(&sample, epoch, sent, 1, 2));
	sample.received = ms_to_ktime(3000);
	sample.tm.tm_year = 117; /* Old-clock broadcast after an unsuccessful write. */
	KUNIT_EXPECT_FALSE(test, dwin_tps02_rtc_matches(&sample, epoch, sent, 1, 2));
	sample.tm.tm_year = 127;
	sample.tm.tm_sec = 4;
	KUNIT_EXPECT_FALSE(test, dwin_tps02_rtc_matches(&sample, epoch, sent, 1, 2));
	sample.tm.tm_sec = 3;
	KUNIT_EXPECT_TRUE(test, dwin_tps02_rtc_matches(&sample, epoch, sent, 1, 2));
	sample.valid = false;
	KUNIT_EXPECT_FALSE(test, dwin_tps02_rtc_matches(&sample, epoch, sent, 1, 2));
}

static struct kunit_case tps02_cases[] = {
	KUNIT_CASE(tps02_encoder_test),
	KUNIT_CASE(tps02_split_stream_test),
	KUNIT_CASE(tps02_checksum_recovery_test),
	KUNIT_CASE(tps02_length_recovery_test),
	KUNIT_CASE(tps02_truncated_recovery_test),
	KUNIT_CASE(tps02_touch_test),
	KUNIT_CASE(tps02_calendar_test),
	KUNIT_CASE(tps02_rtc_freshness_test),
	KUNIT_CASE(tps02_rtc_verification_test),
	{}
};

static struct kunit_suite tps02_suite = {
	.name = "dwin-tps02-protocol",
	.test_cases = tps02_cases,
};
kunit_test_suite(tps02_suite);

MODULE_LICENSE("GPL");
