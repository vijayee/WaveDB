#include "iterator.h"
#include "../../../src/Database/database.h"
#include "../../../src/HBTrie/path.h"

Napi::FunctionReference Iterator::constructor_;

Napi::Object Iterator::Init(Napi::Env env, Napi::Object exports) {
  Napi::HandleScope scope(env);

  Napi::Function func = DefineClass(env, "Iterator", {
    InstanceMethod("read", &Iterator::Read),
    InstanceMethod("end", &Iterator::End),
  });

  constructor_ = Napi::Persistent(func);
  exports.Set("Iterator", func);

  // Register cleanup hook to release constructor_ before Node.js shuts down
  napi_add_env_cleanup_hook(env, [](void* arg) {
    Iterator::constructor_.Reset();
  }, nullptr);

  return exports;
}

Iterator::Iterator(const Napi::CallbackInfo& info)
  : Napi::ObjectWrap<Iterator>(info),
    db_(nullptr),
    reverse_(false),
    keys_(true),
    values_(true),
    keyAsArray_(false),
    delimiter_('/'),
    ended_(false),
    scan_handle_(nullptr) {

  Napi::Env env = info.Env();

  // TODO: Implement iterator initialization
  // This requires database_scan API to be available
}

Iterator::~Iterator() {
  if (scan_handle_) {
    // TODO: Clean up scan handle
    // database_scan_end(scan_handle_);
  }
}

Napi::Value Iterator::Read(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (ended_) {
    return env.Null();
  }

  // TODO: Implement read next entry
  // This requires database_scan API

  return env.Null();
}

Napi::Value Iterator::End(const Napi::CallbackInfo& info) {
  ended_ = true;

  if (scan_handle_) {
    // TODO: Clean up scan handle
    // database_scan_end(scan_handle_);
    scan_handle_ = nullptr;
  }

  return info.Env().Undefined();
}