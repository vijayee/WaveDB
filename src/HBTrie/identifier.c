//
// Created by victor on 3/11/26.
//

#include "identifier.h"
#include "../Util/allocator.h"
#include <cbor.h>
#include <string.h>

identifier_t* identifier_create(buffer_t* buf, size_t chunk_size) {
  if (chunk_size == 0) {
    chunk_size = DEFAULT_CHUNK_SIZE;
  }

  size_t length = (buf != NULL) ? buf->size : 0;
  const uint8_t* data = (buf != NULL) ? buf->data : NULL;

  identifier_t* id = get_clear_memory(sizeof(identifier_t));
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
      memcpy(chunk->data->data, data + (i * chunk_size), chunk_len);
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

    refcounter_destroy_lock((refcounter_t*)id);
    free(id);
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
    memcpy(buf->data + offset, chunk->data->data, copy_len);
    offset += copy_len;
  }

  return buf;
}

cbor_item_t* identifier_to_cbor(identifier_t* id) {
  if (id == NULL) return NULL;

  // Create array with one entry per chunk
  cbor_item_t* array = cbor_new_definite_array((size_t)id->chunks.length);
  if (array == NULL) return NULL;

  for (int i = 0; i < id->chunks.length; i++) {
    chunk_t* chunk = id->chunks.data[i];
    cbor_item_t* bstr = cbor_build_bytestring(chunk->data->data, chunk->data->size);
    if (bstr == NULL) {
      cbor_decref(&array);
      return NULL;
    }
    if (!cbor_array_push(array, bstr)) {
      cbor_decref(&bstr);
      cbor_decref(&array);
      return NULL;
    }
    cbor_decref(&bstr);
  }

  return array;
}

identifier_t* cbor_to_identifier(cbor_item_t* item, size_t chunk_size) {
  if (item == NULL) return NULL;

  if (!cbor_isa_array(item)) {
    return NULL;
  }

  size_t num_chunks = cbor_array_size(item);
  if (num_chunks == 0) {
    return identifier_create_empty(chunk_size);
  }

  // Create identifier and calculate total length
  identifier_t* id = get_clear_memory(sizeof(identifier_t));
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
      free(id);
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

  // Create chunks from CBOR data
  for (size_t i = 0; i < num_chunks; i++) {
    cbor_item_t* chunk_item = cbor_array_get(item, i);
    size_t chunk_len = cbor_bytestring_length(chunk_item);
    cbor_data chunk_data = cbor_bytestring_handle(chunk_item);

    chunk_t* chunk = chunk_create(chunk_data, chunk_len);
    if (chunk == NULL) {
      cbor_decref(&chunk_item);
      // Clean up
      for (int j = 0; j < id->chunks.length; j++) {
        chunk_destroy(id->chunks.data[j]);
      }
      vec_deinit(&id->chunks);
      free(id);
      return NULL;
    }
    vec_push(&id->chunks, chunk);
    cbor_decref(&chunk_item);
  }

  refcounter_init((refcounter_t*)id);
  return id;
}