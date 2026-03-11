//
// Created by victor on 4/7/25.
//

#ifndef WAVEDB_PROMISE_H
#define WAVEDB_PROMISE_H
#include <stdint.h>
#include "error.h"
#include "../RefCounter/refcounter.h"

typedef struct {
  refcounter_t refcounter;
  void (*resolve)(void*, void*);
  void (*reject)(void*, async_error_t*);
  void* ctx;
  uint8_t hasFired;
} promise_t;

promise_t* promise_create(void (*resolve)(void*, void*), void (*reject)(void*, async_error_t*), void* ctx);
void promise_destroy(promise_t* promise);
void promise_resolve(promise_t* promise, void* payload);
void promise_reject(promise_t* promise, async_error_t* error);
#endif //WAVEDB_PROMISE_H
