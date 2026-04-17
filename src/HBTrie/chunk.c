//
// Created by victor on 3/11/26.
//

#include "chunk.h"
#include "../Buffer/buffer.h"
#include "../Util/allocator.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

chunk_t* chunk_create(const void* data, size_t chunk_size) {
  if (chunk_size == 0) return NULL;

  chunk_t* chunk = (chunk_t*)calloc(1, sizeof(chunk_t) + chunk_size);
  if (chunk == NULL) return NULL;

  chunk->size = chunk_size;
  if (data != NULL) {
    memcpy(chunk->data, data, chunk_size);
  }

  refcounter_init((refcounter_t*)chunk);
  return chunk;
}

chunk_t* chunk_create_from_buffer(buffer_t* buf, size_t chunk_size) {
  if (chunk_size == 0) return NULL;

  chunk_t* chunk = (chunk_t*)calloc(1, sizeof(chunk_t) + chunk_size);
  if (chunk == NULL) return NULL;

  chunk->size = chunk_size;
  if (buf != NULL) {
    size_t copy_len = (buf->size < chunk_size) ? buf->size : chunk_size;
    memcpy(chunk->data, buf->data, copy_len);
  }

  refcounter_init((refcounter_t*)chunk);
  return chunk;
}

chunk_t* chunk_create_empty(size_t chunk_size) {
  if (chunk_size == 0) return NULL;

  chunk_t* chunk = (chunk_t*)calloc(1, sizeof(chunk_t) + chunk_size);
  if (chunk == NULL) return NULL;

  chunk->size = chunk_size;
  refcounter_init((refcounter_t*)chunk);
  return chunk;
}

void chunk_destroy(chunk_t* chunk) {
  if (chunk == NULL) return;

  refcounter_dereference((refcounter_t*)chunk);
  if (refcounter_count((refcounter_t*)chunk) == 0) {
    free(chunk);
  }
}

chunk_t* chunk_share(chunk_t* chunk) {
  if (chunk == NULL) return NULL;

  return (chunk_t*)refcounter_reference((refcounter_t*)chunk);
}

int chunk_compare(chunk_t* a, chunk_t* b) {
  if (a == NULL && b == NULL) return 0;
  if (a == NULL) return -1;
  if (b == NULL) return 1;

  // Compare chunk data byte by byte
  size_t size_a = a->size;
  size_t size_b = b->size;
  size_t min_size = size_a < size_b ? size_a : size_b;

  int cmp = memcmp(a->data, b->data, min_size);
  if (cmp != 0) return cmp;

  // If equal up to min_size, shorter chunk is "less"
  if (size_a < size_b) return -1;
  if (size_a > size_b) return 1;
  return 0;
}

void* chunk_data(chunk_t* chunk) {
  if (chunk == NULL) return NULL;
  return chunk->data;
}

const void* chunk_data_const(const chunk_t* chunk) {
  if (chunk == NULL) return NULL;
  return chunk->data;
}

int inline_key_compare(const uint8_t* key_data, uint8_t key_len, const chunk_t* chunk) {
  if (key_data == NULL && chunk == NULL) return 0;
  if (key_data == NULL) return -1;
  if (chunk == NULL) return 1;

  size_t chunk_len = chunk->size;
  size_t min_len = (size_t)key_len < chunk_len ? (size_t)key_len : chunk_len;

  int cmp = memcmp(key_data, chunk->data, min_len);
  if (cmp != 0) return cmp;

  if ((size_t)key_len < chunk_len) return -1;
  if ((size_t)key_len > chunk_len) return 1;
  return 0;
}

int inline_key_compare_direct(const uint8_t* a_data, uint8_t a_len,
                              const uint8_t* b_data, uint8_t b_len) {
  if (a_data == NULL && b_data == NULL) return 0;
  if (a_data == NULL) return -1;
  if (b_data == NULL) return 1;

  size_t min_len = (size_t)a_len < (size_t)b_len ? (size_t)a_len : (size_t)b_len;

  int cmp = memcmp(a_data, b_data, min_len);
  if (cmp != 0) return cmp;

  if ((size_t)a_len < (size_t)b_len) return -1;
  if ((size_t)a_len > (size_t)b_len) return 1;
  return 0;
}