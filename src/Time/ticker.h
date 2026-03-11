//
// Created by victor on 6/16/25.
//

#ifndef WAVEDB_TICKER_H
#define WAVEDB_TICKER_H
#include <stdint.h>
typedef struct {
  void* ctx;
  void (* cb)(void*);
} ticker_t;
void ticker_start(ticker_t ticker, uint64_t delay);
#endif //WAVEDB_TICKER_H
