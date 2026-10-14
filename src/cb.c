/**
 * @file cb.c
 * @brief Implementation of cb.h.
 */

#include "cb.h"

#include <stdlib.h>

bool cbuf_init(cbuf_t *cb, size_t capacity)
{
  if (cb == NULL || capacity == 0) {
    return false;
  }
  cb->buffer = (uint8_t *)malloc(capacity);
  if (cb->buffer == NULL) {
    return false;
  }
  cb->capacity = capacity;
  cb->count = 0;
  cb->head = 0;
  cb->tail = 0;
  return true;
}

void cbuf_free(cbuf_t *cb)
{
  if (cb == NULL) {
    return;
  }
  free(cb->buffer);
  cb->buffer = NULL;
  cb->capacity = 0;
  cb->count = 0;
}

bool cbuf_push(cbuf_t *cb, uint8_t byte)
{
  if (cb == NULL || cb->count == cb->capacity) {
    return false;
  }
  cb->buffer[cb->head] = byte;
  cb->head = (cb->head + 1) % cb->capacity;
  cb->count++;
  return true;
}

bool cbuf_pop(cbuf_t *cb, uint8_t *byte)
{
  if (cb == NULL || cb->count == 0) {
    return false;
  }
  if (byte != NULL) {
    *byte = cb->buffer[cb->tail];
  }
  cb->tail = (cb->tail + 1) % cb->capacity;
  cb->count--;
  return true;
}

size_t cbuf_count(const cbuf_t *cb)
{
  return cb == NULL ? 0 : cb->count;
}

bool cbuf_is_empty(const cbuf_t *cb)
{
  return cb == NULL || cb->count == 0;
}
