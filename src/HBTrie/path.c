//
// Created by victor on 3/11/26.
//

#include "path.h"
#include "../Util/allocator.h"
#include "../Util/memory_pool.h"
#include <cbor.h>
#include <string.h>

path_t* path_create(void) {
  path_t* path = (path_t*)memory_pool_alloc(sizeof(path_t));
  if (path == NULL) {
    path = get_clear_memory(sizeof(path_t));
  } else {
    memset(path, 0, sizeof(path_t));
  }
  vec_init(&path->identifiers);
  refcounter_init((refcounter_t*)path);
  return path;
}

path_t* path_create_from_identifier(identifier_t* id) {
  path_t* path = path_create();
  if (path == NULL) return NULL;

  if (id != NULL) {
    path_append(path, id);
  }

  return path;
}

void path_destroy(path_t* path) {
  if (path == NULL) return;

  refcounter_dereference((refcounter_t*)path);
  if (refcounter_count((refcounter_t*)path) == 0) {
    // Destroy all identifiers
    identifier_t* id;
    int i;
    vec_foreach(&path->identifiers, id, i) {
      identifier_destroy(id);
    }
    vec_deinit(&path->identifiers);

    refcounter_destroy_lock((refcounter_t*)path);
    memory_pool_free(path, sizeof(path_t));
  }
}

int path_append(path_t* path, identifier_t* id) {
  if (path == NULL || id == NULL) return -1;

  // Take a reference to the identifier
  identifier_t* ref = (identifier_t*)refcounter_reference((refcounter_t*)id);
  if (ref == NULL) return -1;

  vec_push(&path->identifiers, ref);
  return 0;
}

identifier_t* path_get(path_t* path, size_t index) {
  if (path == NULL || (int)index >= path->identifiers.length) {
    return NULL;
  }
  return path->identifiers.data[index];
}

size_t path_length(path_t* path) {
  if (path == NULL) return 0;
  return (size_t)path->identifiers.length;
}

int path_is_empty(path_t* path) {
  return path == NULL || path->identifiers.length == 0;
}

path_t* path_copy(path_t* path) {
  if (path == NULL) return NULL;

  path_t* copy = path_create();
  if (copy == NULL) return NULL;

  // Reserve exact capacity
  vec_reserve(&copy->identifiers, path->identifiers.length);

  for (int i = 0; i < path->identifiers.length; i++) {
    identifier_t* id = path->identifiers.data[i];
    identifier_t* id_copy = (identifier_t*)refcounter_reference((refcounter_t*)id);
    vec_push(&copy->identifiers, id_copy);
  }

  return copy;
}

int path_compare(path_t* a, path_t* b) {
  if (a == NULL && b == NULL) return 0;
  if (a == NULL) return -1;
  if (b == NULL) return 1;

  // Compare identifier by identifier
  int min_len = a->identifiers.length < b->identifiers.length
      ? a->identifiers.length
      : b->identifiers.length;

  for (int i = 0; i < min_len; i++) {
    identifier_t* id_a = a->identifiers.data[i];
    identifier_t* id_b = b->identifiers.data[i];
    int cmp = identifier_compare(id_a, id_b);
    if (cmp != 0) return cmp;
  }

  // If all compared identifiers are equal, shorter path is "less"
  if (a->identifiers.length < b->identifiers.length) return -1;
  if (a->identifiers.length > b->identifiers.length) return 1;
  return 0;
}

cbor_item_t* path_to_cbor(path_t* path) {
  if (path == NULL) return NULL;

  // Create outer array with one entry per identifier
  cbor_item_t* array = cbor_new_definite_array((size_t)path->identifiers.length);
  if (array == NULL) return NULL;

  for (int i = 0; i < path->identifiers.length; i++) {
    identifier_t* id = path->identifiers.data[i];
    cbor_item_t* id_cbor = identifier_to_cbor(id);
    if (id_cbor == NULL) {
      cbor_decref(&array);
      return NULL;
    }
    if (!cbor_array_push(array, id_cbor)) {
      cbor_decref(&id_cbor);
      cbor_decref(&array);
      return NULL;
    }
    cbor_decref(&id_cbor);
  }

  return array;
}

path_t* cbor_to_path(cbor_item_t* item, size_t chunk_size) {
  if (item == NULL) return NULL;

  if (!cbor_isa_array(item)) {
    return NULL;
  }

  path_t* path = path_create();
  if (path == NULL) return NULL;

  size_t num_ids = cbor_array_size(item);

  // Reserve exact capacity
  vec_reserve(&path->identifiers, (int)num_ids);

  for (size_t i = 0; i < num_ids; i++) {
    cbor_item_t* id_item = cbor_array_get(item, i);
    identifier_t* id = cbor_to_identifier(id_item, chunk_size);
    cbor_decref(&id_item);

    if (id == NULL) {
      path_destroy(path);
      return NULL;
    }

    vec_push(&path->identifiers, id);
  }

  return path;
}
