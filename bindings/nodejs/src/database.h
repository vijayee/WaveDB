#pragma once
#include <napi.h>
#include "async_bridge.h"

class WaveDB : public Napi::ObjectWrap<WaveDB> {
public:
  static Napi::Object Init(Napi::Env env, Napi::Object exports);
  WaveDB(const Napi::CallbackInfo& info);
  ~WaveDB();

private:
  database_t* db_;
  char delimiter_;
  AsyncBridge bridge_;

  // Async operations
  Napi::Value Put(const Napi::CallbackInfo& info);
  Napi::Value Get(const Napi::CallbackInfo& info);
  Napi::Value Delete(const Napi::CallbackInfo& info);
  Napi::Value Batch(const Napi::CallbackInfo& info);

  // Callback-style async operations (use C async API with error-first callback)
  Napi::Value PutCb(const Napi::CallbackInfo& info);
  Napi::Value GetCb(const Napi::CallbackInfo& info);
  Napi::Value DeleteCb(const Napi::CallbackInfo& info);
  Napi::Value BatchCb(const Napi::CallbackInfo& info);

  // Sync operations
  Napi::Value PutSync(const Napi::CallbackInfo& info);
  Napi::Value GetSync(const Napi::CallbackInfo& info);
  Napi::Value DeleteSync(const Napi::CallbackInfo& info);
  Napi::Value BatchSync(const Napi::CallbackInfo& info);

  // Object operations helpers
  static Napi::Value ConvertArrays(Napi::Env env, Napi::Value value);

  // Object operations
  Napi::Value PutObject(const Napi::CallbackInfo& info);
  Napi::Value GetObject(const Napi::CallbackInfo& info);
  Napi::Value GetObjectSync(const Napi::CallbackInfo& info);

  // Streaming
  Napi::Value CreateReadStream(const Napi::CallbackInfo& info);

  // Lifecycle
  Napi::Value Close(const Napi::CallbackInfo& info);

  // Helper: create an AsyncOpContext with a JS Promise and optional callback
  AsyncOpContext* CreateOpContext(Napi::Env env, AsyncOpType type, const Napi::CallbackInfo& info, int callbackArgIndex);

  static Napi::FunctionReference constructor_;
  static void Cleanup(void* arg);
};