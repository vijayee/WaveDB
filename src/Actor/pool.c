//
// Created by victor on 5/6/25.
//

#include "pool.h"
#include "../Util/allocator.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

static pool_global_t _pool_global[POOL_COUNT];
static _Thread_local pool_local_t _pool_local[POOL_COUNT];

static void _pool_global_init(void) {
  for (size_t index = 0; index < POOL_COUNT; index++) {
    size_t used_size = pool_used_size(index);
    size_t threshold = POOL_ALIGN / used_size;
    if (threshold < POOL_MIN_THRESHOLD) {
      threshold = POOL_MIN_THRESHOLD;
    }
    if (threshold > POOL_MAX_THRESHOLD) {
      threshold = POOL_MAX_THRESHOLD;
    }
    _pool_global[index].head = NULL;
    _pool_global[index].count = threshold;
    _pool_global[index].lock = platform_mutex_create();
  }
}

static pthread_once_t _pool_init_once = PTHREAD_ONCE_INIT;

static void _pool_global_init_once(void) {
  pthread_once(&_pool_init_once, _pool_global_init);
}

static void _pool_push_global(size_t index, pool_item_t* list_head) {
  platform_mutex_lock(_pool_global[index].lock);
  list_head->next = _pool_global[index].head;
  _pool_global[index].head = list_head;
  platform_mutex_unlock(_pool_global[index].lock);
}

static pool_item_t* _pool_pull_global(size_t index) {
  platform_mutex_lock(_pool_global[index].lock);
  pool_item_t* head = _pool_global[index].head;
  if (head != NULL) {
    _pool_global[index].head = NULL;
  }
  platform_mutex_unlock(_pool_global[index].lock);
  return head;
}

static void* _pool_alloc_block(size_t index) {
  size_t used_size = pool_used_size(index);
  if (used_size < POOL_ALIGN) {
    pool_local_t* local = &_pool_local[index];
    if (local->start == NULL || local->start >= local->end) {
      size_t block_size = POOL_ALIGN;
      local->start = get_clear_memory(block_size);
      local->end = local->start + block_size;
    }
    void* pointer = local->start;
    local->start += used_size;
    return pointer;
  }
  void* pointer = NULL;
  if (posix_memalign(&pointer, POOL_ALIGN, used_size) != 0) {
    abort();
  }
  memset(pointer, 0, used_size);
  return pointer;
}

size_t pool_index(size_t size) {
  if (size == 0) {
    size = 1;
  }
  size_t bits = POOL_MIN_BITS;
  while ((1UL << bits) < size) {
    bits++;
  }
  size_t index = bits - POOL_MIN_BITS;
  if (index >= POOL_COUNT) {
    index = POOL_COUNT - 1;
  }
  return index;
}

size_t pool_used_size(size_t index) {
  return (1UL << (POOL_MIN_BITS + index));
}

void* pool_alloc(size_t index) {
  _pool_global_init_once();
  pool_local_t* local = &_pool_local[index];
  if (local->head != NULL) {
    pool_item_t* item = local->head;
    local->head = item->next;
    local->length--;
    return item;
  }
  pool_item_t* global_head = _pool_pull_global(index);
  if (global_head != NULL) {
    local->head = global_head->next;
    local->length = 0;
    return global_head;
  }
  return _pool_alloc_block(index);
}

void pool_free(size_t index, void* pointer) {
  if (pointer == NULL) {
    return;
  }
  _pool_global_init_once();
  pool_item_t* item = (pool_item_t*)pointer;
  pool_local_t* local = &_pool_local[index];
  item->next = local->head;
  local->head = item;
  local->length++;
  if (local->length >= _pool_global[index].count) {
    pool_item_t* list_head = local->head;
    local->head = NULL;
    local->length = 0;
    _pool_push_global(index, list_head);
  }
}

void pool_thread_cleanup(void) {
  for (size_t index = 0; index < POOL_COUNT; index++) {
    pool_local_t* local = &_pool_local[index];
    if (local->head != NULL) {
      _pool_push_global(index, local->head);
      local->head = NULL;
      local->length = 0;
    }
    local->start = NULL;
    local->end = NULL;
  }
}
