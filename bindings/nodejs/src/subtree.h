#pragma once
#include <napi.h>
#include "async_bridge.h"

class Subtree : public Napi::ObjectWrap<Subtree> {
public:
  static Napi::Object Init(Napi::Env env, Napi::Object exports);
  Subtree(const Napi::CallbackInfo& info);
  ~Subtree();

  // Accessor for getting the underlying C pointer (used by layer constructors)
  database_subtree_t* GetSubtree() const { return st_; }

  // Accessor for getting the raw pointer as a number (cross-addon boundary)
  Napi::Value GetPtr(const Napi::CallbackInfo& info);

  static Napi::FunctionReference constructor_;

private:
  database_subtree_t* st_;
  char delimiter_;
  bool syncOnly_;
  AsyncBridge bridge_;

  // Sync operations
  Napi::Value PutSync(const Napi::CallbackInfo& info);
  Napi::Value GetSync(const Napi::CallbackInfo& info);
  Napi::Value DelSync(const Napi::CallbackInfo& info);
  Napi::Value BatchSync(const Napi::CallbackInfo& info);
  Napi::Value ScanSyncRaw(const Napi::CallbackInfo& info);
  Napi::Value Count(const Napi::CallbackInfo& info);
  Napi::Value Snapshot(const Napi::CallbackInfo& info);

  // Async operations
  Napi::Value Put(const Napi::CallbackInfo& info);
  Napi::Value Get(const Napi::CallbackInfo& info);
  Napi::Value Del(const Napi::CallbackInfo& info);

  // Lifecycle
  Napi::Value Close(const Napi::CallbackInfo& info);

  // Helper
  AsyncOpContext* CreateOpContext(Napi::Env env, AsyncOpType type,
                                   const Napi::CallbackInfo& info, int callbackArgIndex);
};