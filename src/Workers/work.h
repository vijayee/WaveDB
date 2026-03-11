//
// Created by victor on 4/8/25.
//

#ifndef WAVEDB_WORK_H
#define WAVEDB_WORK_H
#include "priority.h"
#include "../RefCounter/refcounter.h"
typedef struct {
  refcounter_t refcounter;
  priority_t priority;
  void* ctx;
  void (* execute)(void*);
  void (* abort)(void*);
} work_t;

work_t* work_create(priority_t priority, void* ctx, void (* execute)(void*), void (* abort)(void*));
void work_execute(work_t* work);
void work_abort(work_t* work);
void work_destroy(work_t* work);
#endif //WAVEDB_WORK_H
