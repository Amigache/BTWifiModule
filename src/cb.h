/**
 * @file cb.h
 * @brief Byte circular buffer (FIFO) with a clear API and semantics.
 *
 * Index-based implementation (no pointer arithmetic) to ease verification. Not
 * thread-safe: each instance must have a single producer and a single consumer,
 * or be protected externally.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Byte circular buffer. */
typedef struct {
  uint8_t *buffer;    //< storage
  size_t capacity;    //< maximum number of bytes
  size_t count;       //< bytes currently stored
  size_t head;        //< write index
  size_t tail;        //< read index
} cbuf_t;

/**
 * @brief Allocate memory and initialize the buffer.
 *
 * @param cb        buffer to initialize.
 * @param capacity  capacity in bytes (>= 1).
 * @return `true` on success, `false` on allocation failure.
 */
bool cbuf_init(cbuf_t *cb, size_t capacity);

/** Free the internal storage of the buffer. */
void cbuf_free(cbuf_t *cb);

/**
 * @brief Append a byte at the end of the buffer.
 *
 * @return `true` if inserted, `false` if the buffer is full.
 */
bool cbuf_push(cbuf_t *cb, uint8_t byte);

/**
 * @brief Remove the oldest byte from the buffer.
 *
 * @param[out] byte  removed byte (only valid if `true` is returned).
 * @return `true` if removed, `false` if the buffer is empty.
 */
bool cbuf_pop(cbuf_t *cb, uint8_t *byte);

/** @return number of stored bytes. */
size_t cbuf_count(const cbuf_t *cb);

/** @return `true` if no bytes are stored. */
bool cbuf_is_empty(const cbuf_t *cb);
