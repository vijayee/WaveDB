#pragma once
#include <napi.h>
#include "async_bridge.h"

class WaveDB : public Napi::ObjectWrap<WaveDB> {
public:
  static Napi::Object Init(Napi::Env env, Napi::Object exports);

private:
  database_t* db_;
  char delimiter_;
  AsyncBridge bridge_;
};