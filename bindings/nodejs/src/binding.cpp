#include <napi.h>
#include "database.h"

Napi::Object Init(Napi::Env env, Napi::Object exports) {
  return WaveDB::Init(env, exports);
}

NODE_API_MODULE(wavedb, Init)
