/**
 * @file trainer_frame.h
 * @brief Pure FrSky Bluetooth trainer frame parser (no ESP-IDF dependencies,
 *        host-testable).
 *
 * A trainer frame carries 8 channels over BLE/UART. It is 0x7E-delimited and
 * byte-stuffed: 0x7D escapes the next byte XOR 0x20. The decoded payload is 14
 * bytes: type (0x80), 12 packed channel bytes (8 channels x 12 bits) and an
 * XOR checksum of the first 13 bytes.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Trainer frame type byte (the only type accepted). */
#define TRAINER_FRAME_TYPE 0x80

/** Frame delimiter, byte-stuff escape and stuff mask. */
#define TRAINER_FRAME_START_STOP 0x7E
#define TRAINER_FRAME_BYTE_STUFF 0x7D
#define TRAINER_FRAME_STUFF_MASK 0x20

/** Decoded payload size: type (1) + 8 channels x 12 bits (12) + XOR (1). */
#define TRAINER_FRAME_DECODED_SIZE 14

/** Maximum wire bytes of one frame (2 delimiters + fully stuffed payload). */
#define TRAINER_FRAME_WIRE_CAPACITY 34

/** Number of trainer channels per frame. */
#define TRAINER_FRAME_CHANNELS 8

/** Result of feeding one byte to the parser. */
typedef enum {
  TRAINER_FRAME_PENDING = 0,  //< need more bytes
  TRAINER_FRAME_VALID,        //< a complete, valid frame was just decoded
  TRAINER_FRAME_INVALID,      //< the frame was rejected; parser resynced
} trainer_frame_result_t;

/** Stateful parser for a stream of trainer frame bytes. */
typedef struct {
  /** Wire bytes of the current frame (raw, byte-stuffed, incl. delimiters). */
  uint8_t wire[TRAINER_FRAME_WIRE_CAPACITY];
  /** Number of valid bytes in `wire`. */
  size_t wire_len;
  /** Decoded (unstuffed) bytes seen so far in the current frame. */
  size_t decoded_len;
  /** Type byte of the current frame (first decoded byte). */
  uint8_t decoded_type;
  /** Running XOR of the decoded bytes (0 = valid checksum at frame end). */
  uint8_t xor_sum;
  /** Inside a byte-stuff escape (previous decoded byte was 0x7D). */
  bool escape;
  /** Inside a frame (after a start delimiter). */
  bool active;
} trainer_frame_parser_t;

/** Reset the parser (e.g. on reconnect or protocol change). */
void trainer_frame_reset(trainer_frame_parser_t *p);

/**
 * @brief Feed one byte to the parser.
 *
 * @return @ref TRAINER_FRAME_PENDING while the frame is incomplete,
 *         @ref TRAINER_FRAME_VALID once a complete valid frame has been
 *         decoded (its wire bytes are in `p->wire` / `p->wire_len`), or
 *         @ref TRAINER_FRAME_INVALID when the frame is malformed.
 */
trainer_frame_result_t trainer_frame_feed(trainer_frame_parser_t *p, uint8_t byte);
