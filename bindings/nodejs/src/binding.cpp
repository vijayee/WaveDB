// C++ headers that include <atomic> must come before C headers
// that use ATOMIC_TYPE() macros expanding to std::atomic<T> in C++.
#include <atomic>

#include <napi.h>
#include "database.h"
#include "iterator.h"

Napi::Object Init(Napi::Env env, Napi::Object exports) {
  WaveDB::Init(env, exports);
  Iterator::Init(env, exports);
  return exports;
}

NODE_API_MODULE(wavedb, Init)