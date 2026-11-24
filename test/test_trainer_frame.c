/**
 * @file test_trainer_frame.c
 * @brief Unit tests for the FrSky trainer frame parser
 *        (trainer_frame.h / trainer_frame.c).
 */

#include "unity.h"
#include <string.h>

#include "../src/trainer_frame.c"

/**
 * Build a wire trainer frame (0x7E-delimited, byte-stuffed, XOR checksum).
 * Channel values are absolute us (0..4095, center 1500).
 */
static size_t build_frame(uint8_t *out, size_t cap, uint8_t type,
                          const uint16_t channels[TRAINER_FRAME_CHANNELS])
{
  uint8_t body[TRAINER_FRAME_DECODED_SIZE - 1];  // type + 12 channel bytes
  body[0] = type;
  for (int c = 0, i = 1; c < TRAINER_FRAME_CHANNELS; c += 2, i += 3) {
    uint16_t ch0 = channels[c];
    uint16_t ch1 = channels[c + 1];
    body[i] = (uint8_t)(ch0 & 0xff);
    body[i + 1] = (uint8_t)(((ch0 & 0x0f00) >> 4) | ((ch1 & 0x00f0) >> 4));
    body[i + 2] = (uint8_t)(((ch1 & 0x000f) << 4) | ((ch1 & 0x0f00) >> 8));
  }

  uint8_t crc = 0;
  for (size_t i = 0; i < sizeof(body); i++) {
    crc ^= body[i];
  }

  size_t len = 0;
  out[len++] = TRAINER_FRAME_START_STOP;
  for (size_t i = 0; i < sizeof(body); i++) {
    if (body[i] == TRAINER_FRAME_START_STOP || body[i] == TRAINER_FRAME_BYTE_STUFF) {
      out[len++] = TRAINER_FRAME_BYTE_STUFF;
      out[len++] = (uint8_t)(body[i] ^ TRAINER_FRAME_STUFF_MASK);
    } else {
      out[len++] = body[i];
    }
  }
  if (crc == TRAINER_FRAME_START_STOP || crc == TRAINER_FRAME_BYTE_STUFF) {
    out[len++] = TRAINER_FRAME_BYTE_STUFF;
    out[len++] = (uint8_t)(crc ^ TRAINER_FRAME_STUFF_MASK);
  } else {
    out[len++] = crc;
  }
  out[len++] = TRAINER_FRAME_START_STOP;
  TEST_ASSERT_TRUE(len <= cap);
  return len;
}

/** Feed a whole buffer and return the last non-pending result. */
static trainer_frame_result_t feed_all(trainer_frame_parser_t *p, const uint8_t *data,
                                       size_t len)
{
  trainer_frame_result_t r = TRAINER_FRAME_PENDING;
  for (size_t i = 0; i < len; i++) {
    trainer_frame_result_t this = trainer_frame_feed(p, data[i]);
    if (this != TRAINER_FRAME_PENDING) {
      r = this;
    }
  }
  return r;
}

static const uint16_t center[TRAINER_FRAME_CHANNELS] = {1500, 1500, 1500, 1500,
                                                        1500, 1500, 1500, 1500};

void test_valid_center_frame(void)
{
  uint8_t frame[TRAINER_FRAME_WIRE_CAPACITY];
  size_t len = build_frame(frame, sizeof(frame), TRAINER_FRAME_TYPE, center);

  trainer_frame_parser_t p;
  trainer_frame_reset(&p);
  TEST_ASSERT_EQUAL(TRAINER_FRAME_VALID, feed_all(&p, frame, len));
  TEST_ASSERT_EQUAL(len, p.wire_len);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(frame, p.wire, len);
}

void test_valid_frame_with_stuffing(void)
{
  // ch0 = 1150 (0x47E) has low byte 0x7E, forcing byte-stuffing on the wire.
  const uint16_t channels[TRAINER_FRAME_CHANNELS] = {1150, 1500, 1500, 1500,
                                                     1500, 1500, 1500, 1500};
  uint8_t frame[TRAINER_FRAME_WIRE_CAPACITY];
  size_t len = build_frame(frame, sizeof(frame), TRAINER_FRAME_TYPE, channels);

  trainer_frame_parser_t p;
  trainer_frame_reset(&p);
  TEST_ASSERT_EQUAL(TRAINER_FRAME_VALID, feed_all(&p, frame, len));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(frame, p.wire, len);
}

void test_real_captured_frame(void)
{
  // Frame captured from a real headtracker on the bench.
  const uint8_t frame[] = {0x7e, 0x80, 0x9e, 0x7d, 0x5d, 0xc5, 0xce, 0x4d, 0xc5,
                           0xdc, 0x5d, 0xc5, 0xdc, 0x5d, 0xc5, 0xe0, 0x7e};

  trainer_frame_parser_t p;
  trainer_frame_reset(&p);
  TEST_ASSERT_EQUAL(TRAINER_FRAME_VALID, feed_all(&p, frame, sizeof(frame)));
}

void test_bad_crc(void)
{
  uint8_t frame[TRAINER_FRAME_WIRE_CAPACITY];
  size_t len = build_frame(frame, sizeof(frame), TRAINER_FRAME_TYPE, center);
  frame[len - 2] ^= 0x01;  // corrupt the CRC byte (before the trailing 0x7E)

  trainer_frame_parser_t p;
  trainer_frame_reset(&p);
  TEST_ASSERT_EQUAL(TRAINER_FRAME_INVALID, feed_all(&p, frame, len));
}

void test_wrong_type(void)
{
  uint8_t frame[TRAINER_FRAME_WIRE_CAPACITY];
  size_t len = build_frame(frame, sizeof(frame), 0x81, center);

  trainer_frame_parser_t p;
  trainer_frame_reset(&p);
  TEST_ASSERT_EQUAL(TRAINER_FRAME_INVALID, feed_all(&p, frame, len));
}

void test_truncated_frame(void)
{
  uint8_t frame[TRAINER_FRAME_WIRE_CAPACITY];
  size_t len = build_frame(frame, sizeof(frame), TRAINER_FRAME_TYPE, center);

  trainer_frame_parser_t p;
  trainer_frame_reset(&p);
  // Everything except the trailing delimiter must stay Pending.
  for (size_t i = 0; i < len - 1; i++) {
    TEST_ASSERT_EQUAL(TRAINER_FRAME_PENDING, trainer_frame_feed(&p, frame[i]));
  }
}

void test_garbage_before_frame(void)
{
  uint8_t frame[TRAINER_FRAME_WIRE_CAPACITY];
  size_t len = build_frame(frame, sizeof(frame), TRAINER_FRAME_TYPE, center);

  trainer_frame_parser_t p;
  trainer_frame_reset(&p);
  // Non-delimiter bytes while idle are ignored.
  const uint8_t garbage[] = {0x01, 0x02, 0x03, 'A', 'T', '+'};
  for (size_t i = 0; i < sizeof(garbage); i++) {
    TEST_ASSERT_EQUAL(TRAINER_FRAME_PENDING, trainer_frame_feed(&p, garbage[i]));
  }
  TEST_ASSERT_EQUAL(TRAINER_FRAME_VALID, feed_all(&p, frame, len));
}

void test_reset_mid_frame(void)
{
  uint8_t frame[TRAINER_FRAME_WIRE_CAPACITY];
  size_t len = build_frame(frame, sizeof(frame), TRAINER_FRAME_TYPE, center);

  trainer_frame_parser_t p;
  trainer_frame_reset(&p);
  for (size_t i = 0; i < len / 2; i++) {
    trainer_frame_feed(&p, frame[i]);
  }
  trainer_frame_reset(&p);
  TEST_ASSERT_EQUAL(TRAINER_FRAME_VALID, feed_all(&p, frame, len));
}

void run_trainer_frame_tests(void)
{
  RUN_TEST(test_valid_center_frame);
  RUN_TEST(test_valid_frame_with_stuffing);
  RUN_TEST(test_real_captured_frame);
  RUN_TEST(test_bad_crc);
  RUN_TEST(test_wrong_type);
  RUN_TEST(test_truncated_frame);
  RUN_TEST(test_garbage_before_frame);
  RUN_TEST(test_reset_mid_frame);
}
