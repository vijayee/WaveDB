//
// Created by victor on 4/7/25.
//
#include "error.h"
#include "../Util/allocator.h"
#include <string.h>

async_error_t* error_create(char* message, char* file, char* function, int line) {
  if ((message == NULL) || (file == NULL) || (function == NULL)) {
    return NULL;
  }
  async_error_t* error = get_clear_memory(sizeof(async_error_t));
  error->message = get_memory(strlen(message) + 1);
  strcpy(error->message, message);
  error->file = get_memory(strlen(file) + 1);
  strcpy(error->file, file);
  error->function = get_memory(strlen(function) +1);
  strcpy(error->function, function);
  error->line = line;
  refcounter_init((refcounter_t*) error);
  return error;
}
void error_destroy(async_error_t* error) {
  refcounter_dereference((refcounter_t*) error);
  if (refcounter_count((refcounter_t*) error) == 0) {
    free(error->message);
    free(error->file);
    free(error->function);
    refcounter_reference((refcounter_t*) error);
    free(error);
  }
}