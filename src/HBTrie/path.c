//
// Created by victor on 3/11/26.
//

#include "path.h"
#include "../Util/allocator.h"
#include <string.h>

path_t* path_create(void) {
  path_t* path = get_clear_memory(sizeof(path_t));
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
    free(path);
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