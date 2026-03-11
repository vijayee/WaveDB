//
// Created by victor on 4/29/25.
//
#include <xxh3.h>
#include "hash.h"
#include "allocator.h"


size_t hash_pointer(const void* ptr) {
  if(sizeof(size_t) == 8) {
    size_t g = (size_t) ptr;
    return XXH3_64bits(&g,8);
  } else {
    size_t g = (size_t) ptr;
    return XXH32(&g, 4, 0);
  }
}

size_t hash_uint32(const void* data) {
  if(sizeof(size_t) == 8) {
    return XXH3_64bits(data,4);
  } else {
    return XXH32(data, 4, 0);
  }
}

size_t hash_size_t(const void* data) {
  if(sizeof(size_t) == 8) {
    return XXH3_64bits(data,4);
  } else {
    return XXH32(data, 4, 0);
  }
}

size_t hash_uint64(const void* data) {
  if(sizeof(size_t) == 8) {
    return XXH3_64bits(data,8);
  } else {
    return XXH32(data, 8, 0);
  }
}

int compare_uint32(const void* data1, const void* data2) {
  const uint32_t* _data1 = data1;
  const uint32_t* _data2 = data2;
  if (*_data1 == *_data2){
    return 0;
  } else if (*_data1 > *_data2) {
    return 1;
  } else {
    return -1;
  }
}

int compare_uint64(const void* data1, const void* data2) {
  const uint64_t* _data1 = data1;
  const uint64_t* _data2 = data2;
  if (*_data1 == *_data2){
    return 0;
  } else if (*_data1 > *_data2) {
    return 1;
  } else {
    return -1;
  }
}

int compare_size_t(const void* data1, const void* data2) {
  const size_t* _data1 = data1;
  const size_t* _data2 = data2;
  if (*_data1 == *_data2){
    return 0;
  } else if (*_data1 > *_data2) {
    return 1;
  } else {
    return -1;
  }
}

int compare_buffer(const void* data1, const void* data2) {
  const buffer_t* _data1 = data1;
  const buffer_t* _data2 = data2;
  return buffer_compare((buffer_t*)_data1, (buffer_t*)_data2);
}

uint32_t* duplicate_uint32(const uint32_t* key) {
  uint32_t* copy = get_clear_memory(4);
  memcpy(copy, key,4);
  return copy;
}

uint64_t* duplicate_uint64(const uint64_t* key) {
  uint64_t* copy = get_clear_memory(8);
  memcpy(copy, key,8);
  return copy;
}

size_t* duplicate_size_t(const size_t* key) {
  uint64_t* copy = get_clear_memory(sizeof(size_t));
  memcpy(copy, key,sizeof(size_t));
  return copy;
}

size_t hash_buffer(const buffer_t* data) {
  if(sizeof(size_t) == 8) {
    return XXH3_64bits(data->data,data->size);
  } else {
    return XXH32(data->data, data->size, 0);
  }
}