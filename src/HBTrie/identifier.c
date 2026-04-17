//
// Created by victor on 3/11/26.
//

#include "identifier.h"
#include "../Util/allocator.h"
#include "../Util/memory_pool.h"
#include <cbor.h>
#include <string.h>

// Forward declaration for old format deserialization
identifier_t* cbor_to_identifier_old(cbor_item_t* item, size_t chunk_size);

identifier_t* identifier_create(buffer_t* buf, size_t chunk_size) {
  if (chunk_size == 0) {
    chunk_size = DEFAULT_CHUNK_SIZE;
  }

  size_t length = (buf != NULL) ? buf->size : 0;
  const uint8_t* data = (buf != NULL) ? buf->data : NULL;

  // Use memory pool for identifier_t (typically ~40-60 bytes)
  identifier_t* id = (identifier_t*)memory_pool_alloc(sizeof(identifier_t));
  if (id == NULL) {
    id = get_clear_memory(sizeof(identifier_t));
  }
  id->length = length;
  id->chunk_size = chunk_size;

  // Initialize chunk vector
  vec_init(&id->chunks);

  // Calculate number of chunks needed
  size_t nchunk = identifier_calc_nchunk(length, chunk_size);

  // Reserve space for chunks
  vec_reserve(&id->chunks, (int)nchunk);

  // Create chunks from data
  for (size_t i = 0; i < nchunk; i++) {
    size_t chunk_len = chunk_size;
    if (i == nchunk - 1) {
      // Last chunk may be smaller
      chunk_len = identifier_calc_last_chunk_size(length, chunk_size);
    }

    chunk_t* chunk = chunk_create_empty(chunk_size);
    if (data != NULL) {
      memcpy(chunk->data, data + (i * chunk_size), chunk_len);
      // Remaining bytes in last chunk are already zeroed by chunk_create_empty
    }
    vec_push(&id->chunks, chunk);
  }

  refcounter_init((refcounter_t*)id);
  return id;
}

identifier_t* identifier_create_empty(size_t chunk_size) {
  return identifier_create(NULL, chunk_size);
}

void identifier_destroy(identifier_t* id) {
  if (id == NULL) return;

  refcounter_dereference((refcounter_t*)id);
  if (refcounter_count((refcounter_t*)id) == 0) {
    // Free all chunks
    chunk_t* chunk;
    int i;
    vec_foreach(&id->chunks, chunk, i) {
      chunk_destroy(chunk);
    }
    vec_deinit(&id->chunks);

    memory_pool_free(id, sizeof(identifier_t));
  }
}

int identifier_compare(identifier_t* a, identifier_t* b) {
  if (a == NULL && b == NULL) return 0;
  if (a == NULL) return -1;
  if (b == NULL) return 1;

  // Compare chunk by chunk
  size_t min_chunks = a->chunks.length < b->chunks.length ? (size_t)a->chunks.length : (size_t)b->chunks.length;

  for (size_t i = 0; i < min_chunks; i++) {
    chunk_t* chunk_a = a->chunks.data[i];
    chunk_t* chunk_b = b->chunks.data[i];
    int cmp = chunk_compare(chunk_a, chunk_b);
    if (cmp != 0) return cmp;
  }

  // If all compared chunks are equal, shorter identifier is "less"
  if (a->chunks.length < b->chunks.length) return -1;
  if (a->chunks.length > b->chunks.length) return 1;
  return 0;
}

chunk_t* identifier_get_chunk(identifier_t* id, size_t index) {
  if (id == NULL || index >= (size_t)id->chunks.length) {
    return NULL;
  }
  return id->chunks.data[index];
}

size_t identifier_chunk_count(identifier_t* id) {
  if (id == NULL) return 0;
  return (size_t)id->chunks.length;
}

buffer_t* identifier_to_buffer(identifier_t* id) {
  if (id == NULL) return NULL;

  buffer_t* buf = buffer_create(id->length);
  if (buf == NULL) return NULL;

  size_t offset = 0;
  for (size_t i = 0; i < (size_t)id->chunks.length; i++) {
    chunk_t* chunk = id->chunks.data[i];
    size_t copy_len = id->chunk_size;
    if (i == (size_t)id->chunks.length - 1) {
      // Last chunk: only copy up to remaining length
      copy_len = id->length - offset;
    }
    memcpy(buf->data + offset, chunk->data, copy_len);
    offset += copy_len;
  }

  return buf;
}

uint8_t* identifier_get_data(identifier_t* id, size_t* out_len) {
  if (id == NULL || out_len == NULL) return NULL;

  *out_len = id->length;
  if (id->length == 0) return NULL;

  uint8_t* data = malloc(id->length);
  if (data == NULL) return NULL;

  size_t offset = 0;
  for (size_t i = 0; i < (size_t)id->chunks.length; i++) {
    chunk_t* chunk = id->chunks.data[i];
    size_t copy_len = id->chunk_size;
    if (i == (size_t)id->chunks.length - 1) {
      copy_len = id->length - offset;
    }
    memcpy(data + offset, chunk->data, copy_len);
    offset += copy_len;
  }

  return data;
}

cbor_item_t* identifier_to_cbor(identifier_t* id) {
  if (id == NULL) return NULL;

  // New format: single bytestring with original data
  // (was: array of chunk bytestrings, now deprecated)
  buffer_t* buf = identifier_to_buffer(id);
  if (buf == NULL) return NULL;

  cbor_item_t* bstr = cbor_build_bytestring(buf->data, id->length);
  buffer_destroy(buf);
  return bstr;
}

identifier_t* cbor_to_identifier(cbor_item_t* item, size_t chunk_size) {
  if (item == NULL) return NULL;

  if (!cbor_isa_bytestring(item)) {
    // Old format: array of chunks (deprecated)
    return cbor_to_identifier_old(item, chunk_size);
  }

  // New format: single bytestring with original data
  size_t data_len = cbor_bytestring_length(item);
  cbor_data data = cbor_bytestring_handle(item);

  // Create buffer and identifier
  buffer_t* buf = buffer_create_from_pointer_copy((uint8_t*)data, data_len);
  if (buf == NULL) return NULL;

  identifier_t* id = identifier_create(buf, chunk_size);
  buffer_destroy(buf);

  return id;
}

identifier_t* cbor_to_identifier_old(cbor_item_t* item, size_t chunk_size) {
  if (item == NULL) return NULL;

  if (!cbor_isa_array(item)) {
    return NULL;
  }

  size_t num_chunks = cbor_array_size(item);
  if (num_chunks == 0) {
    return identifier_create_empty(chunk_size);
  }

  // Create identifier and calculate total length
  // Use memory pool
  identifier_t* id = (identifier_t*)memory_pool_alloc(sizeof(identifier_t));
  if (id == NULL) {
    id = get_clear_memory(sizeof(identifier_t));
  }
  if (id == NULL) return NULL;

  id->chunk_size = (chunk_size == 0) ? DEFAULT_CHUNK_SIZE : chunk_size;
  vec_init(&id->chunks);
  vec_reserve(&id->chunks, (int)num_chunks);

  // Calculate total length from all chunks
  size_t total_length = 0;
  for (size_t i = 0; i < num_chunks; i++) {
    cbor_item_t* chunk_item = cbor_array_get(item, i);
    if (!cbor_isa_bytestring(chunk_item)) {
      cbor_decref(&chunk_item);
      // Clean up
      for (int j = 0; j < id->chunks.length; j++) {
        chunk_destroy(id->chunks.data[j]);
      }
      vec_deinit(&id->chunks);
      memory_pool_free(id, sizeof(identifier_t));
      return NULL;
    }
    size_t chunk_len = cbor_bytestring_length(chunk_item);
    if (i == num_chunks - 1) {
      // Last chunk: actual length
      total_length += chunk_len;
    } else {
      // Non-last chunk: should be full chunk_size
      total_length += id->chunk_size;
    }
    cbor_decref(&chunk_item);
  }

  id->length = total_length;

  // Create chunks from CBOR data - all chunks are full chunk_size except last
  for (size_t i = 0; i < num_chunks; i++) {
    cbor_item_t* chunk_item = cbor_array_get(item, i);
    size_t chunk_len = cbor_bytestring_length(chunk_item);
    cbor_data chunk_data = cbor_bytestring_handle(chunk_item);

    // All chunks should be created with full chunk_size
    chunk_t* chunk = chunk_create_empty(id->chunk_size);
    if (chunk == NULL) {
      cbor_decref(&chunk_item);
      // Clean up
      for (int j = 0; j < id->chunks.length; j++) {
        chunk_destroy(id->chunks.data[j]);
      }
      vec_deinit(&id->chunks);
      memory_pool_free(id, sizeof(identifier_t));
      return NULL;
    }

    // Copy data (chunk_len bytes for last chunk, chunk_size for others)
    memcpy(chunk->data, chunk_data, chunk_len);
    // Rest is already zeroed by chunk_create_empty

    vec_push(&id->chunks, chunk);
    cbor_decref(&chunk_item);
  }

  refcounter_init((refcounter_t*)id);
  return id;
}