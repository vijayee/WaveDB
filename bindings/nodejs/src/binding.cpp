#include <napi.h>
#include "database.h"
#include "iterator.h"

Napi::Object Init(Napi::Env env, Napi::Object exports) {
  WaveDB::Init(env, exports);
  Iterator::Init(env, exports);
  return exports;
}

NODE_API_MODULE(wavedb, Init)