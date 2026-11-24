/**
 * @file trainer_frame.c
 * @brief Implementation of trainer_frame.h (trainer frame parser).
 */

#include "trainer_frame.h"

void trainer_frame_reset(trainer_frame_parser_t *p)
{
  p->wire_len = 0;
  p->decoded_len = 0;
  p->decoded_type = 0;
  p->xor_sum = 0;
  p->escape = false;
  p->active = false;
}

static void trainer_frame_start(trainer_frame_parser_t *p)
{
  p->active = true;
  p->escape = false;
  p->decoded_len = 0;
  p->decoded_type = 0;
  p->xor_sum = 0;
  p->wire_len = 1;
  p->wire[0] = TRAINER_FRAME_START_STOP;
}

trainer_frame_result_t trainer_frame_feed(trainer_frame_parser_t *p, uint8_t byte)
{
  if (byte == TRAINER_FRAME_START_STOP) {
    if (!p->active) {
      trainer_frame_start(p);
      return TRAINER_FRAME_PENDING;
    }
    // A delimiter ends the frame. An empty frame only restarts it.
    if (p->decoded_len == 0) {
      trainer_frame_start(p);
      return TRAINER_FRAME_PENDING;
    }
    bool valid = !p->escape && p->decoded_len == TRAINER_FRAME_DECODED_SIZE &&
                 p->xor_sum == 0 && p->decoded_type == TRAINER_FRAME_TYPE;
    if (valid && p->wire_len < TRAINER_FRAME_WIRE_CAPACITY) {
      p->wire[p->wire_len++] = TRAINER_FRAME_START_STOP;
      p->active = false;
      return TRAINER_FRAME_VALID;
    }
    trainer_frame_start(p);
    return TRAINER_FRAME_INVALID;
  }

  if (!p->active) {
    return TRAINER_FRAME_PENDING;
  }

  if (p->wire_len >= TRAINER_FRAME_WIRE_CAPACITY - 1) {
    trainer_frame_reset(p);
    return TRAINER_FRAME_INVALID;
  }
  p->wire[p->wire_len++] = byte;

  if (p->escape) {
    byte ^= TRAINER_FRAME_STUFF_MASK;
    p->escape = false;
  } else if (byte == TRAINER_FRAME_BYTE_STUFF) {
    p->escape = true;
    return TRAINER_FRAME_PENDING;
  }

  if (p->decoded_len == 0) {
    p->decoded_type = byte;
  }
  p->xor_sum ^= byte;
  if (++p->decoded_len > TRAINER_FRAME_DECODED_SIZE) {
    trainer_frame_reset(p);
    return TRAINER_FRAME_INVALID;
  }
  return TRAINER_FRAME_PENDING;
}
