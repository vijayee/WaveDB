//
// Created by victor on 4/7/25.
//

#ifndef WAVEDB_ERROR_H
#define WAVEDB_ERROR_H
#include "../RefCounter/refcounter.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  refcounter_t refcounter;
  char* message;
  char* file;
  char* function;
  int line;
} async_error_t;

async_error_t* error_create(char* message, char* file, char* function, int line);
void error_destroy(async_error_t* error);
#define ERROR(MESSAGE) error_create(MESSAGE, (char*)__FILE__, (char*)__func__, __LINE__)

#ifdef __cplusplus
}
#endif

#endif //WAVEDB_ERROR_H
