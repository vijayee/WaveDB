//
// Created by victor on 3/11/26.
//

#include "chunk.h"
#include "../Util/allocator.h"
#include "../Util/memory_pool.h"
#include <string.h>
#include <stdio.h>
#include <stdio.h>

chunk_t* chunk_create(const void* data, size_t chunk_size) {
  // Try memory pool first (chunk_t is typically 16-24 bytes)
  chunk_t* chunk = (chunk_t*)memory_pool_alloc(sizeof(chunk_t));
  if (chunk == NULL) {
    chunk = get_clear_memory(sizeof(chunk_t));
  }
  chunk->data = buffer_create(chunk_size);
  if (data != NULL) {
    buffer_copy_from_pointer(chunk->data, (uint8_t*)data, chunk_size);
  }
  return chunk;
}

chunk_t* chunk_create_from_buffer(buffer_t* buf, size_t chunk_size) {
  chunk_t* chunk = (chunk_t*)memory_pool_alloc(sizeof(chunk_t));
  if (chunk == NULL) {
    chunk = get_clear_memory(sizeof(chunk_t));
  }
  chunk->data = buffer_create(chunk_size);
  if (buf != NULL) {
    buffer_copy_from_pointer(chunk->data, buf->data, chunk_size);
  }
  return chunk;
}

chunk_t* chunk_create_empty(size_t chunk_size) {
  chunk_t* chunk = (chunk_t*)memory_pool_alloc(sizeof(chunk_t));
  if (chunk == NULL) {
    chunk = get_clear_memory(sizeof(chunk_t));
  }
  chunk->data = buffer_create(chunk_size);
  return chunk;
}

void chunk_destroy(chunk_t* chunk) {
  if (chunk == NULL) return;
  if (chunk->data != NULL) {
    buffer_destroy(chunk->data);
  }
  memory_pool_free(chunk, sizeof(chunk_t));
}

chunk_t* chunk_share(chunk_t* chunk) {
  if (chunk == NULL) return NULL;

  chunk_t* new_chunk = (chunk_t*)memory_pool_alloc(sizeof(chunk_t));
  if (new_chunk == NULL) {
    new_chunk = get_clear_memory(sizeof(chunk_t));
  }
  if (new_chunk == NULL) return NULL;

  new_chunk->data = (buffer_t*)refcounter_reference((refcounter_t*)chunk->data);
  return new_chunk;
}

int chunk_compare(chunk_t* a, chunk_t* b) {
  if (a == NULL && b == NULL) return 0;
  if (a == NULL) return -1;
  if (b == NULL) return 1;

  // Compare chunk data byte by byte
  size_t size_a = a->data->size;
  size_t size_b = b->data->size;
  size_t min_size = size_a < size_b ? size_a : size_b;

  // Debug logging for WAL recovery
  if (getenv("WAVEDB_DEBUG_CHUNK")) {
    fprintf(stderr, "CHUNK_COMPARE: size_a=%zu, size_b=%zu\n", size_a, size_b);
    fprintf(stderr, "  Chunk A: ");
    for (size_t i = 0; i < size_a && i < 8; i++) {
      fprintf(stderr, "%02x ", a->data->data[i]);
    }
    fprintf(stderr, "\n  Chunk B: ");
    for (size_t i = 0; i < size_b && i < 8; i++) {
      fprintf(stderr, "%02x ", b->data->data[i]);
    }
    fprintf(stderr, "\n");
  }

  int cmp = memcmp(a->data->data, b->data->data, min_size);
  if (cmp != 0) return cmp;

  // If equal up to min_size, shorter chunk is "less"
  if (size_a < size_b) return -1;
  if (size_a > size_b) return 1;
  return 0;
}

void* chunk_data(chunk_t* chunk) {
  if (chunk == NULL) return NULL;
  return chunk->data->data;
}

const void* chunk_data_const(const chunk_t* chunk) {
  if (chunk == NULL) return NULL;
  return chunk->data->data;
}