#pragma once
#include <napi.h>

class WaveDB : public Napi::ObjectWrap<WaveDB> {
public:
  static Napi::Object Init(Napi::Env env, Napi::Object exports);

private:
  // Implementation will be in database.cc (Task 6)
};
