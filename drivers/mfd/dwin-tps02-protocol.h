/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _DWIN_TPS02_PROTOCOL_H
#define _DWIN_TPS02_PROTOCOL_H

#include <linux/errno.h>
#include <linux/mfd/dwin-tps02.h>
#include <linux/string.h>
#include <linux/unaligned.h>

/* LEN includes TYPE, DATA and checksum, but not the sync bytes or LEN itself. */
#define TPS02_MAX_LEN	16
#define TPS02_MAX_FRAME	(TPS02_MAX_LEN + 3)

struct tps02_parser {
	u8 buffer[TPS02_MAX_FRAME];
	u8 count;
	u32 checksum_errors;
	u32 length_errors;
};

typedef void (*tps02_frame_handler)(void *context, const u8 *frame);

static inline u8 tps02_checksum(const u8 *data, size_t len)
{
	u8 sum = 0;

	while (len--)
		sum += *data++;
	return sum;
}

static inline int tps02_encode(u8 *frame, u8 type, const u8 *data, size_t len)
{
	if (len > TPS02_MAX_LEN - 2)
		return -EMSGSIZE;
	frame[0] = 0x5a;
	frame[1] = 0xa5;
	frame[2] = len + 2;
	frame[3] = type;
	if (len)
		memcpy(frame + 4, data, len);
	frame[len + 4] = tps02_checksum(frame + 2, len + 2);
	return len + 5;
}

static inline void tps02_discard(struct tps02_parser *parser, size_t len)
{
	parser->count -= len;
	memmove(parser->buffer, parser->buffer + len, parser->count);
}

static inline void tps02_receive(struct tps02_parser *parser, const u8 *data,
				size_t len, tps02_frame_handler handler,
				void *context)
{
	size_t size;
	u8 *frame = parser->buffer;

	while (len--) {
		parser->buffer[parser->count++] = *data++;
		while (parser->count >= 2) {
			if (frame[0] != 0x5a || frame[1] != 0xa5) {
				tps02_discard(parser, 1);
				continue;
			}
			if (parser->count < 3)
				break;
			if (frame[2] < 2 || frame[2] > TPS02_MAX_LEN) {
				parser->length_errors++;
				tps02_discard(parser, 1);
				continue;
			}
			size = frame[2] + 3;
			if (parser->count < size)
				break;
			if (tps02_checksum(frame + 2, frame[2]) != frame[size - 1]) {
				parser->checksum_errors++;
				/* Retain embedded sync after corrupt/truncated frames. */
				tps02_discard(parser, 1);
				continue;
			}
			handler(context, frame);
			tps02_discard(parser, size);
		}
	}
}

static inline bool tps02_decode_touch(const u8 *frame,
				     struct dwin_tps02_touch *touch)
{
	if (frame[2] != 7 || frame[3] != 0x11 ||
	    frame[4] < 1 || frame[4] > 3)
		return false;
	touch->event = frame[4];
	touch->x = get_unaligned_be16(frame + 5);
	touch->y = get_unaligned_be16(frame + 7);
	return true;
}

/* Called only after a complete, checksum-valid TYPE 0x12 / LEN 11 report. */
static inline bool tps02_decode_calendar(const u8 *frame, struct rtc_time *tm)
{
	*tm = (struct rtc_time) {
		.tm_year = frame[4] + 100,
		.tm_mon = frame[5],
		.tm_mday = frame[6],
		.tm_hour = frame[8],
		.tm_min = frame[9],
		.tm_sec = frame[10],
	};
	if (tm->tm_year > 199 || rtc_valid_tm(tm))
		return false;
	/* Wire bytes 7, 11 and 12 are unknown, not weekday or subseconds. */
	rtc_time64_to_tm(rtc_tm_to_time64(tm), tm);
	return true;
}

#endif
