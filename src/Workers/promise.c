//
// Created by victor on 4/8/25.
//
#include "promise.h"
#include "../Util/log.h"
#include <stdio.h>
#include <stdlib.h>
#include "../Util/allocator.h"

promise_t* promise_create(void (*resolve)(void*, void*), void (*reject)(void*, async_error_t*), void* ctx) {
  promise_t* promise = get_clear_memory(sizeof(promise_t));
  promise->resolve = resolve;
  promise->reject = reject;
  promise->ctx = ctx;
  refcounter_init((refcounter_t*) promise);
  return promise;
}

void promise_destroy(promise_t* promise) {
  refcounter_dereference((refcounter_t*) promise);
  if (refcounter_count((refcounter_t*) promise) == 0) {
    refcounter_destroy_lock((refcounter_t*) promise);
    free(promise);
  }
}

void promise_resolve(promise_t* promise, void* payload) {
  if (promise->resolve == NULL) {
    log_error("Unresolvable Promise");
    abort();
  }
  if (promise->hasFired == 0) {
    promise->hasFired = 1;
    promise->resolve(promise->ctx, payload);
  }
}

void promise_reject(promise_t* promise, async_error_t* error) {
  if (promise->reject == NULL) {
    char err[100];
    sprintf(err,"Unhandled Error -%s:%d %s", error->file, error->line, error->message);
    log_error(err);
    abort();
  }
  if (promise->hasFired == 0) {
    promise->hasFired = 1;
    promise->reject(promise->ctx, error);
  }
}