// C++ headers that include <atomic> must come before C headers
// that use ATOMIC_TYPE() macros expanding to std::atomic<T> in C++.
#include <atomic>

#include <napi.h>
#include <cctype>
#include <string>
#include "Database/database.h"
#include "Database/database_subtree.h"
#include "Database/batch.h"
#include "path.h"
#include "identifier.h"
#include "async_bridge.h"
#include "subtree.h"

Napi::FunctionReference Subtree::constructor_;

Napi::Object Subtree::Init(Napi::Env env, Napi::Object exports) {
  Napi::HandleScope scope(env);

  Napi::Function func = DefineClass(env, "Subtree", {
    InstanceMethod("putSync", &Subtree::PutSync),
    InstanceMethod("getSync", &Subtree::GetSync),
    InstanceMethod("delSync", &Subtree::DelSync),
    InstanceMethod("batchSync", &Subtree::BatchSync),
    InstanceMethod("scanSyncRaw", &Subtree::ScanSyncRaw),
    InstanceMethod("count", &Subtree::Count),
    InstanceMethod("snapshot", &Subtree::Snapshot),
    InstanceMethod("put", &Subtree::Put),
    InstanceMethod("get", &Subtree::Get),
    InstanceMethod("del", &Subtree::Del),
    InstanceMethod("close", &Subtree::Close),
    InstanceMethod("_getPtr", &Subtree::GetPtr),
  });

  constructor_ = Napi::Persistent(func);
  exports.Set("Subtree", func);

  return exports;
}

Subtree::Subtree(const Napi::CallbackInfo& info)
  : Napi::ObjectWrap<Subtree>(info),
    st_(nullptr),
    delimiter_('/'),
    syncOnly_(false) {

  Napi::Env env = info.Env();

  if (info.Length() < 1 || !info[0].IsExternal()) {
    Napi::TypeError::New(env, "Internal: External subtree pointer required").ThrowAsJavaScriptException();
    return;
  }

  st_ = info[0].As<Napi::External<database_subtree_t>>().Data();

  if (info.Length() > 1 && info[1].IsString()) {
    std::string delimStr = info[1].As<Napi::String>().Utf8Value();
    if (!delimStr.empty()) {
      delimiter_ = delimStr[0];
    }
  }

  if (info.Length() > 2 && info[2].IsBoolean()) {
    syncOnly_ = info[2].As<Napi::Boolean>().Value();
  }

  if (!syncOnly_) {
    bridge_.Init(env);
  }
}

Subtree::~Subtree() {
  if (st_) {
    if (!syncOnly_) {
      bridge_.Shutdown();
    }
    database_subtree_close(st_);
    st_ = nullptr;
  }
}

// --- Cross-addon pointer accessor ---

Napi::Value Subtree::GetPtr(const Napi::CallbackInfo& info) {
  return Napi::Number::New(info.Env(), reinterpret_cast<uintptr_t>(st_));
}

// --- Helper: create AsyncOpContext with JS Promise ---

AsyncOpContext* Subtree::CreateOpContext(Napi::Env env, AsyncOpType type,
                                          const Napi::CallbackInfo& info, int callbackArgIndex) {
  AsyncOpContext* ctx = new AsyncOpContext();
  ctx->type = type;
  ctx->result = nullptr;
  ctx->error = nullptr;
  ctx->promise_c = nullptr;
  ctx->batch = nullptr;
  ctx->callback_ref = nullptr;
  ctx->env = env;

  napi_create_promise(env, &ctx->deferred, &ctx->promise);

  if (callbackArgIndex >= 0 &&
      info.Length() > static_cast<size_t>(callbackArgIndex) &&
      info[callbackArgIndex].IsFunction()) {
    Napi::Function callback = info[callbackArgIndex].As<Napi::Function>();
    napi_create_reference(env, callback, 1, &ctx->callback_ref);
  }

  return ctx;
}

// --- Sync operations ---

Napi::Value Subtree::PutSync(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (!st_) {
    Napi::Error::New(env, "SUBTREE_CLOSED: Subtree is closed").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  if (info.Length() < 2) {
    Napi::TypeError::New(env, "Key and value required").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  std::string key_str = KeyFromJSDynamic(env, info[0], delimiter_);
  if (env.IsExceptionPending()) return env.Undefined();

  std::string val_str;
  const uint8_t* val_buf;
  size_t val_len;

  if (!ValueFromJSZeroCopy(env, info[1], val_str, &val_buf, &val_len)) {
    return env.Undefined();
  }

  int rc = database_subtree_put_sync_raw(st_, key_str.c_str(), key_str.size(), delimiter_,
                                           val_buf, val_len);
  if (rc != 0) {
    Napi::Error::New(env, "IO_ERROR: Failed to put value in subtree").ThrowAsJavaScriptException();
  }

  return env.Undefined();
}

Napi::Value Subtree::GetSync(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (!st_) {
    Napi::Error::New(env, "SUBTREE_CLOSED: Subtree is closed").ThrowAsJavaScriptException();
    return env.Null();
  }

  if (info.Length() < 1) {
    Napi::TypeError::New(env, "Key required").ThrowAsJavaScriptException();
    return env.Null();
  }

  std::string key_str = KeyFromJSDynamic(env, info[0], delimiter_);
  if (env.IsExceptionPending()) return env.Null();

  uint8_t* value = nullptr;
  size_t value_len = 0;
  int rc = database_subtree_get_sync_raw(st_, key_str.c_str(), key_str.size(), delimiter_,
                                           &value, &value_len);

  if (rc == 0 && value != nullptr) {
    // Strip trailing null bytes (chunk padding from identifier serialization)
    while (value_len > 0 && value[value_len - 1] == '\0') {
      value_len--;
    }

    // Check if printable ASCII to decide string vs Buffer return
    bool printable = true;
    for (size_t i = 0; i < value_len; i++) {
      if (!isprint(value[i]) && value[i] != '\t' && value[i] != '\n' && value[i] != '\r') {
        printable = false;
        break;
      }
    }
    Napi::Value result;
    if (printable) {
      result = Napi::String::New(env, std::string(reinterpret_cast<const char*>(value), value_len));
    } else {
      result = Napi::Buffer<uint8_t>::Copy(env, value, value_len);
    }
    database_raw_value_free(value);
    return result;
  } else if (rc == -2) {
    return env.Null();
  } else {
    if (value != nullptr) database_raw_value_free(value);
    Napi::Error::New(env, "IO_ERROR: Failed to get value from subtree").ThrowAsJavaScriptException();
    return env.Null();
  }
}

Napi::Value Subtree::DelSync(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (!st_) {
    Napi::Error::New(env, "SUBTREE_CLOSED: Subtree is closed").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  if (info.Length() < 1) {
    Napi::TypeError::New(env, "Key required").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  std::string key_str = KeyFromJSDynamic(env, info[0], delimiter_);
  if (env.IsExceptionPending()) return env.Undefined();

  int rc = database_subtree_delete_sync_raw(st_, key_str.c_str(), key_str.size(), delimiter_);
  if (rc != 0) {
    Napi::Error::New(env, "IO_ERROR: Failed to delete value from subtree").ThrowAsJavaScriptException();
  }

  return env.Undefined();
}

Napi::Value Subtree::BatchSync(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (!st_) {
    Napi::Error::New(env, "SUBTREE_CLOSED: Subtree is closed").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  if (info.Length() < 1 || !info[0].IsArray()) {
    Napi::TypeError::New(env, "Array of operations required").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  Napi::Array js_ops = info[0].As<Napi::Array>();
  uint32_t count = js_ops.Length();
  if (count == 0) return env.Undefined();

  // Build raw_op_t array with stable string storage
  std::vector<raw_op_t> ops(count);
  std::vector<std::string> key_strings(count);
  std::vector<std::string> val_strings;
  val_strings.reserve(count);

  for (uint32_t i = 0; i < count; i++) {
    Napi::Object op = js_ops.Get(i).As<Napi::Object>();

    if (!op.Has("type") || !op.Has("key")) {
      Napi::TypeError::New(env, "Operation must have 'type' and 'key'").ThrowAsJavaScriptException();
      return env.Undefined();
    }

    std::string type = op.Get("type").As<Napi::String>().Utf8Value();

    if (type == "put") {
      if (!op.Has("value")) {
        Napi::TypeError::New(env, "Put operation must have 'value'").ThrowAsJavaScriptException();
        return env.Undefined();
      }
      ops[i].type = 0;

      // Key
      key_strings[i] = KeyFromJSDynamic(env, op.Get("key"), delimiter_);
      if (env.IsExceptionPending()) return env.Undefined();
      ops[i].key = key_strings[i].c_str();
      ops[i].key_len = key_strings[i].size();

      // Value
      Napi::Value valArg = op.Get("value");
      std::string val_str;
      if (!ValueFromJSDynamic(env, valArg, val_str)) {
        return env.Undefined();
      }
      val_strings.push_back(val_str);
      ops[i].value = reinterpret_cast<const uint8_t*>(val_strings.back().c_str());
      ops[i].value_len = val_strings.back().size();
    } else if (type == "del") {
      ops[i].type = 1;
      ops[i].value = nullptr;
      ops[i].value_len = 0;

      // Key
      key_strings[i] = KeyFromJSDynamic(env, op.Get("key"), delimiter_);
      if (env.IsExceptionPending()) return env.Undefined();
      ops[i].key = key_strings[i].c_str();
      ops[i].key_len = key_strings[i].size();
    } else {
      Napi::TypeError::New(env, "Operation type must be 'put' or 'del'").ThrowAsJavaScriptException();
      return env.Undefined();
    }
  }

  int rc = database_subtree_batch_sync_raw(st_, delimiter_, ops.data(), ops.size());
  if (rc != 0) {
    Napi::Error::New(env, "IO_ERROR: Subtree batch operation failed").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  return env.Undefined();
}

Napi::Value Subtree::ScanSyncRaw(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (!st_) {
    Napi::Error::New(env, "SUBTREE_CLOSED: Subtree is closed").ThrowAsJavaScriptException();
    return env.Null();
  }

  std::string prefix_str;
  if (info.Length() > 0) {
    prefix_str = KeyFromJSDynamic(env, info[0], delimiter_);
    if (env.IsExceptionPending()) return env.Null();
  }

  raw_result_t* results = nullptr;
  size_t count = 0;
  int rc = database_subtree_scan_sync_raw(st_, prefix_str.c_str(), prefix_str.size(),
                                            delimiter_, &results, &count);

  if (rc != 0) {
    Napi::Error::New(env, "IO_ERROR: Subtree scan failed").ThrowAsJavaScriptException();
    return env.Null();
  }

  Napi::Array arr = Napi::Array::New(env, count);
  for (size_t i = 0; i < count; i++) {
    Napi::Object entry = Napi::Object::New(env);

    // Key
    if (results[i].key != nullptr && results[i].key_len > 0) {
      size_t key_len = results[i].key_len;
      // Strip trailing null bytes from key (chunk padding)
      while (key_len > 0 && results[i].key[key_len - 1] == '\0') {
        key_len--;
      }
      bool key_printable = true;
      for (size_t j = 0; j < key_len; j++) {
        if (!isprint(results[i].key[j]) && results[i].key[j] != '\t' &&
            results[i].key[j] != '\n' && results[i].key[j] != '\r') {
          key_printable = false;
          break;
        }
      }
      if (key_printable) {
        entry.Set("key", Napi::String::New(env, std::string(results[i].key, key_len)));
      } else {
        entry.Set("key", Napi::Buffer<uint8_t>::Copy(env, reinterpret_cast<const uint8_t*>(results[i].key), key_len));
      }
    } else {
      entry.Set("key", Napi::String::New(env, ""));
    }

    // Value
    if (results[i].value != nullptr && results[i].value_len > 0) {
      size_t val_len = results[i].value_len;
      while (val_len > 0 && results[i].value[val_len - 1] == '\0') {
        val_len--;
      }
      bool val_printable = true;
      for (size_t j = 0; j < val_len; j++) {
        if (!isprint(results[i].value[j]) && results[i].value[j] != '\t' &&
            results[i].value[j] != '\n' && results[i].value[j] != '\r') {
          val_printable = false;
          break;
        }
      }
      if (val_printable) {
        entry.Set("value", Napi::String::New(env, std::string(reinterpret_cast<const char*>(results[i].value), val_len)));
      } else {
        entry.Set("value", Napi::Buffer<uint8_t>::Copy(env, results[i].value, val_len));
      }
    } else {
      entry.Set("value", Napi::String::New(env, ""));
    }

    arr.Set(i, entry);
  }

  database_raw_results_free(results, count);
  return arr;
}

Napi::Value Subtree::Count(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (!st_) {
    Napi::Error::New(env, "SUBTREE_CLOSED: Subtree is closed").ThrowAsJavaScriptException();
    return env.Null();
  }

  size_t count = database_subtree_count(st_);
  return Napi::Number::New(env, static_cast<double>(count));
}

Napi::Value Subtree::Snapshot(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (!st_) {
    Napi::Error::New(env, "SUBTREE_CLOSED: Subtree is closed").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  int rc = database_subtree_snapshot(st_);
  if (rc != 0) {
    Napi::Error::New(env, "IO_ERROR: Subtree snapshot failed").ThrowAsJavaScriptException();
  }

  return env.Undefined();
}

// --- Async operations ---

Napi::Value Subtree::Put(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (!st_) {
    Napi::Error::New(env, "SUBTREE_CLOSED: Subtree is closed").ThrowAsJavaScriptException();
    return env.Null();
  }

  if (info.Length() < 2) {
    Napi::TypeError::New(env, "Key and value required").ThrowAsJavaScriptException();
    return env.Null();
  }

  std::string key_str = KeyFromJSDynamic(env, info[0], delimiter_);
  if (env.IsExceptionPending()) return env.Null();

  std::string val_str;
  if (!ValueFromJSDynamic(env, info[1], val_str)) {
    return env.Null();
  }

  AsyncOpContext* ctx = CreateOpContext(env, AsyncOpType::Put, info, 2);

  promise_t* promise_c = bridge_.CreatePromise(ctx);
  if (!promise_c) {
    napi_value error_val = Napi::Error::New(env, "Failed to create async promise").Value();
    napi_value promise_val = ctx->promise;
    napi_reject_deferred(env, ctx->deferred, error_val);
    if (ctx->callback_ref) napi_delete_reference(env, ctx->callback_ref);
    delete ctx;
    return Napi::Value(env, promise_val);
  }

  ctx->promise_c = promise_c;

  int rc = database_subtree_put_raw(st_, key_str.c_str(), key_str.size(), delimiter_,
                                     reinterpret_cast<const uint8_t*>(val_str.data()), val_str.size(),
                                     promise_c);
  if (rc != 0) {
    napi_value error_val = Napi::Error::New(env, "IO_ERROR: Failed to dispatch subtree put").Value();
    napi_value promise_val = ctx->promise;
    napi_reject_deferred(env, ctx->deferred, error_val);
    if (ctx->callback_ref) napi_delete_reference(env, ctx->callback_ref);
    delete ctx;
    return Napi::Value(env, promise_val);
  }

  return Napi::Value(env, ctx->promise);
}

Napi::Value Subtree::Get(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (!st_) {
    Napi::Error::New(env, "SUBTREE_CLOSED: Subtree is closed").ThrowAsJavaScriptException();
    return env.Null();
  }

  if (info.Length() < 1) {
    Napi::TypeError::New(env, "Key required").ThrowAsJavaScriptException();
    return env.Null();
  }

  std::string key_str = KeyFromJSDynamic(env, info[0], delimiter_);
  if (env.IsExceptionPending()) return env.Null();

  AsyncOpContext* ctx = CreateOpContext(env, AsyncOpType::Get, info, 1);

  promise_t* promise_c = bridge_.CreatePromise(ctx);
  if (!promise_c) {
    napi_value error_val = Napi::Error::New(env, "Failed to create async promise").Value();
    napi_value promise_val = ctx->promise;
    napi_reject_deferred(env, ctx->deferred, error_val);
    if (ctx->callback_ref) napi_delete_reference(env, ctx->callback_ref);
    delete ctx;
    return Napi::Value(env, promise_val);
  }

  ctx->promise_c = promise_c;

  int rc = database_subtree_get_raw(st_, key_str.c_str(), key_str.size(), delimiter_, promise_c);
  if (rc != 0) {
    napi_value error_val = Napi::Error::New(env, "IO_ERROR: Failed to dispatch subtree get").Value();
    napi_value promise_val = ctx->promise;
    napi_reject_deferred(env, ctx->deferred, error_val);
    if (ctx->callback_ref) napi_delete_reference(env, ctx->callback_ref);
    delete ctx;
    return Napi::Value(env, promise_val);
  }

  return Napi::Value(env, ctx->promise);
}

Napi::Value Subtree::Del(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (!st_) {
    Napi::Error::New(env, "SUBTREE_CLOSED: Subtree is closed").ThrowAsJavaScriptException();
    return env.Null();
  }

  if (info.Length() < 1) {
    Napi::TypeError::New(env, "Key required").ThrowAsJavaScriptException();
    return env.Null();
  }

  std::string key_str = KeyFromJSDynamic(env, info[0], delimiter_);
  if (env.IsExceptionPending()) return env.Null();

  AsyncOpContext* ctx = CreateOpContext(env, AsyncOpType::Delete, info, 1);

  promise_t* promise_c = bridge_.CreatePromise(ctx);
  if (!promise_c) {
    napi_value error_val = Napi::Error::New(env, "Failed to create async promise").Value();
    napi_value promise_val = ctx->promise;
    napi_reject_deferred(env, ctx->deferred, error_val);
    if (ctx->callback_ref) napi_delete_reference(env, ctx->callback_ref);
    delete ctx;
    return Napi::Value(env, promise_val);
  }

  ctx->promise_c = promise_c;

  int rc = database_subtree_delete_raw(st_, key_str.c_str(), key_str.size(), delimiter_, promise_c);
  if (rc != 0) {
    napi_value error_val = Napi::Error::New(env, "IO_ERROR: Failed to dispatch subtree delete").Value();
    napi_value promise_val = ctx->promise;
    napi_reject_deferred(env, ctx->deferred, error_val);
    if (ctx->callback_ref) napi_delete_reference(env, ctx->callback_ref);
    delete ctx;
    return Napi::Value(env, promise_val);
  }

  return Napi::Value(env, ctx->promise);
}

// --- Lifecycle ---

Napi::Value Subtree::Close(const Napi::CallbackInfo& info) {
  if (st_) {
    if (!syncOnly_) {
      bridge_.Shutdown();
    }
    database_subtree_close(st_);
    st_ = nullptr;
  }
  return info.Env().Undefined();
}