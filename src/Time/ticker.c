//
// Created by victor on 6/16/25.
//
#include "ticker.h"
#ifdef _WIN32
#include <synchapi.h>
void ticker_start(ticker_t ticker, uint64_t delay) {
  Sleep(delay);
  ticker.cb(ticker.ctx);
}
#else
#include <unistd.h>
void ticker_start(ticker_t ticker, uint64_t delay) {
  usleep(delay);
  ticker.cb(ticker.ctx);
}
#endif
