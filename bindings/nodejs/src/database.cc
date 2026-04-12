#include <napi.h>
#include <string>
#include <unistd.h>
#include "../../../src/Database/database.h"
#include "../../../src/Database/batch.h"
#include "path.h"
#include "identifier.h"
#include "async_bridge.h"
#include "iterator.h"

// BatchOp struct for PutObject/FlattenObject (local use, not the C batch_t)
enum class BatchOpType { PUT, DEL };

struct BatchOp {
  BatchOpType type;
  path_t* path;
  identifier_t* value;  // nullptr for DEL
};

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

  // Sync operations
  Napi::Value PutSync(const Napi::CallbackInfo& info);
  Napi::Value GetSync(const Napi::CallbackInfo& info);
  Napi::Value DeleteSync(const Napi::CallbackInfo& info);
  Napi::Value BatchSync(const Napi::CallbackInfo& info);

  // Object operations helpers
  static void FlattenObject(Napi::Env env,
                           Napi::Object obj,
                           std::vector<std::string>& path_parts,
                           std::vector<BatchOp>& ops,
                           char delimiter);

  static Napi::Object ReconstructObject(Napi::Env env,
                                        const std::vector<std::pair<std::vector<std::string>, Napi::Value>>& entries);

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

Napi::FunctionReference WaveDB::constructor_;

void WaveDB::Cleanup(void* arg) {
  constructor_.Reset();
}

Napi::Object WaveDB::Init(Napi::Env env, Napi::Object exports) {
  Napi::HandleScope scope(env);

  Napi::Function func = DefineClass(env, "WaveDB", {
    InstanceMethod("put", &WaveDB::Put),
    InstanceMethod("get", &WaveDB::Get),
    InstanceMethod("del", &WaveDB::Delete),
    InstanceMethod("batch", &WaveDB::Batch),
    InstanceMethod("putSync", &WaveDB::PutSync),
    InstanceMethod("getSync", &WaveDB::GetSync),
    InstanceMethod("delSync", &WaveDB::DeleteSync),
    InstanceMethod("batchSync", &WaveDB::BatchSync),
    InstanceMethod("putObject", &WaveDB::PutObject),
    InstanceMethod("getObject", &WaveDB::GetObject),
    InstanceMethod("getObjectSync", &WaveDB::GetObjectSync),
    InstanceMethod("createReadStream", &WaveDB::CreateReadStream),
    InstanceMethod("close", &WaveDB::Close),
  });

  constructor_ = Napi::Persistent(func);
  exports.Set("WaveDB", func);

  napi_add_env_cleanup_hook(env, Cleanup, nullptr);

  return exports;
}

WaveDB::WaveDB(const Napi::CallbackInfo& info)
  : Napi::ObjectWrap<WaveDB>(info),
    db_(nullptr),
    delimiter_('/') {

  Napi::Env env = info.Env();

  if (info.Length() < 1 || !info[0].IsString()) {
    Napi::TypeError::New(env, "Database path required").ThrowAsJavaScriptException();
    return;
  }

  std::string path = info[0].As<Napi::String>().Utf8Value();

  database_config_t* config = database_config_default();
  if (!config) {
    Napi::Error::New(env, "Failed to create default configuration").ThrowAsJavaScriptException();
    return;
  }

  if (info.Length() > 1 && info[1].IsObject()) {
    Napi::Object options = info[1].As<Napi::Object>();

    if (options.Has("delimiter")) {
      Napi::String delim = options.Get("delimiter").As<Napi::String>();
      std::string delimStr = delim.Utf8Value();
      if (delimStr.length() != 1) {
        database_config_destroy(config);
        Napi::TypeError::New(env, "Delimiter must be a single character").ThrowAsJavaScriptException();
        return;
      }
      delimiter_ = delimStr[0];
    }

    if (options.Has("chunkSize")) {
      Napi::Number val = options.Get("chunkSize").As<Napi::Number>();
      config->chunk_size = static_cast<uint8_t>(val.Uint32Value());
    }

    if (options.Has("btreeNodeSize")) {
      Napi::Number val = options.Get("btreeNodeSize").As<Napi::Number>();
      config->btree_node_size = static_cast<uint32_t>(val.Uint32Value());
    }

    if (options.Has("enablePersist")) {
      Napi::Boolean val = options.Get("enablePersist").As<Napi::Boolean>();
      config->enable_persist = val.Value() ? 1 : 0;
    }

    if (options.Has("lruMemoryMb")) {
      Napi::Number val = options.Get("lruMemoryMb").As<Napi::Number>();
      config->lru_memory_mb = static_cast<size_t>(val.Uint32Value());
    }

    if (options.Has("lruShards")) {
      Napi::Number val = options.Get("lruShards").As<Napi::Number>();
      config->lru_shards = static_cast<uint16_t>(val.Uint32Value());
    }

    if (options.Has("storageCacheSize")) {
      Napi::Number val = options.Get("storageCacheSize").As<Napi::Number>();
      config->storage_cache_size = static_cast<size_t>(val.Uint32Value());
    }

    if (options.Has("wal")) {
      Napi::Object walOpts = options.Get("wal").As<Napi::Object>();

      if (walOpts.Has("syncMode")) {
        Napi::String mode = walOpts.Get("syncMode").As<Napi::String>();
        std::string modeStr = mode.Utf8Value();
        if (modeStr == "immediate") {
          config->wal_config.sync_mode = WAL_SYNC_IMMEDIATE;
        } else if (modeStr == "debounced") {
          config->wal_config.sync_mode = WAL_SYNC_DEBOUNCED;
        } else if (modeStr == "async") {
          config->wal_config.sync_mode = WAL_SYNC_ASYNC;
        }
      }

      if (walOpts.Has("debounceMs")) {
        Napi::Number val = walOpts.Get("debounceMs").As<Napi::Number>();
        config->wal_config.debounce_ms = static_cast<uint64_t>(val.Uint32Value());
      }

      if (walOpts.Has("maxFileSize")) {
        Napi::Number val = walOpts.Get("maxFileSize").As<Napi::Number>();
        config->wal_config.max_file_size = static_cast<size_t>(val.Uint32Value());
      }
    }

    if (options.Has("workerThreads")) {
      Napi::Number val = options.Get("workerThreads").As<Napi::Number>();
      config->worker_threads = static_cast<uint8_t>(val.Uint32Value());
    }
  }

  int error_code = 0;
  db_ = database_create_with_config(path.c_str(), config, &error_code);
  database_config_destroy(config);

  if (!db_) {
    Napi::Error::New(env, "Failed to create database").ThrowAsJavaScriptException();
    return;
  }

  // Initialize the async bridge for all async operations
  bridge_.Init(env);
}

WaveDB::~WaveDB() {
  if (db_) {
    database_destroy(db_);
    db_ = nullptr;
  }
}

// --- Helper: create AsyncOpContext with JS Promise and optional callback ---

AsyncOpContext* WaveDB::CreateOpContext(Napi::Env env, AsyncOpType type, const Napi::CallbackInfo& info, int callbackArgIndex) {
  AsyncOpContext* ctx = new AsyncOpContext();
  ctx->type = type;
  ctx->result = nullptr;
  ctx->error = nullptr;
  ctx->promise_c = nullptr;
  ctx->batch = nullptr;
  ctx->callback_ref = nullptr;
  ctx->env = env;

  // Create JS Promise using raw C API
  napi_create_promise(env, &ctx->deferred, &ctx->promise);

  if (callbackArgIndex >= 0 &&
      info.Length() > static_cast<size_t>(callbackArgIndex) &&
      info[callbackArgIndex].IsFunction()) {
    Napi::Function callback = info[callbackArgIndex].As<Napi::Function>();
    napi_create_reference(env, callback, 1, &ctx->callback_ref);
  }

  return ctx;
}

// --- Async operations using C async API + AsyncBridge ---

Napi::Value WaveDB::Put(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (!db_) {
    Napi::Error::New(env, "DATABASE_CLOSED: Database is closed").ThrowAsJavaScriptException();
    return env.Null();
  }

  if (info.Length() < 2) {
    Napi::TypeError::New(env, "Key and value required").ThrowAsJavaScriptException();
    return env.Null();
  }

  path_t* path = PathFromJS(env, info[0], delimiter_);
  if (!path) return env.Null();

  identifier_t* value = ValueFromJS(env, info[1]);
  if (!value) {
    path_destroy(path);
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
    path_destroy(path);
    identifier_destroy(value);
    return Napi::Value(env, promise_val);
  }

  ctx->promise_c = promise_c;

  // C async API takes ownership of path and value
  database_put(db_, path, value, promise_c);

  return Napi::Value(env, ctx->promise);
}

Napi::Value WaveDB::Get(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (!db_) {
    Napi::Error::New(env, "DATABASE_CLOSED: Database is closed").ThrowAsJavaScriptException();
    return env.Null();
  }

  if (info.Length() < 1) {
    Napi::TypeError::New(env, "Key required").ThrowAsJavaScriptException();
    return env.Null();
  }

  path_t* path = PathFromJS(env, info[0], delimiter_);
  if (!path) return env.Null();

  AsyncOpContext* ctx = CreateOpContext(env, AsyncOpType::Get, info, 1);

  promise_t* promise_c = bridge_.CreatePromise(ctx);
  if (!promise_c) {
    napi_value error_val = Napi::Error::New(env, "Failed to create async promise").Value();
    napi_value promise_val = ctx->promise;
    napi_reject_deferred(env, ctx->deferred, error_val);
    if (ctx->callback_ref) napi_delete_reference(env, ctx->callback_ref);
    delete ctx;
    path_destroy(path);
    return Napi::Value(env, promise_val);
  }

  ctx->promise_c = promise_c;

  database_get(db_, path, promise_c);

  return Napi::Value(env, ctx->promise);
}

Napi::Value WaveDB::Delete(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (!db_) {
    Napi::Error::New(env, "DATABASE_CLOSED: Database is closed").ThrowAsJavaScriptException();
    return env.Null();
  }

  if (info.Length() < 1) {
    Napi::TypeError::New(env, "Key required").ThrowAsJavaScriptException();
    return env.Null();
  }

  path_t* path = PathFromJS(env, info[0], delimiter_);
  if (!path) return env.Null();

  AsyncOpContext* ctx = CreateOpContext(env, AsyncOpType::Delete, info, 1);

  promise_t* promise_c = bridge_.CreatePromise(ctx);
  if (!promise_c) {
    napi_value error_val = Napi::Error::New(env, "Failed to create async promise").Value();
    napi_value promise_val = ctx->promise;
    napi_reject_deferred(env, ctx->deferred, error_val);
    if (ctx->callback_ref) napi_delete_reference(env, ctx->callback_ref);
    delete ctx;
    path_destroy(path);
    return Napi::Value(env, promise_val);
  }

  ctx->promise_c = promise_c;

  database_delete(db_, path, promise_c);

  return Napi::Value(env, ctx->promise);
}

Napi::Value WaveDB::Batch(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (!db_) {
    Napi::Error::New(env, "DATABASE_CLOSED: Database is closed").ThrowAsJavaScriptException();
    return env.Null();
  }

  if (info.Length() < 1 || !info[0].IsArray()) {
    Napi::TypeError::New(env, "Array of operations required").ThrowAsJavaScriptException();
    return env.Null();
  }

  Napi::Array ops = info[0].As<Napi::Array>();

  // Build a C batch_t from the JS array
  batch_t* batch = batch_create(ops.Length());
  if (!batch) {
    Napi::Error::New(env, "Failed to create batch").ThrowAsJavaScriptException();
    return env.Null();
  }

  for (uint32_t i = 0; i < ops.Length(); i++) {
    Napi::Object op = ops.Get(i).As<Napi::Object>();

    if (!op.Has("type") || !op.Has("key")) {
      batch_destroy(batch);
      Napi::TypeError::New(env, "Operation must have 'type' and 'key'").ThrowAsJavaScriptException();
      return env.Null();
    }

    std::string type = op.Get("type").As<Napi::String>().Utf8Value();
    path_t* path = PathFromJS(env, op.Get("key"), delimiter_);
    if (!path) {
      batch_destroy(batch);
      return env.Null();
    }

    int rc;
    if (type == "put") {
      if (!op.Has("value")) {
        path_destroy(path);
        batch_destroy(batch);
        Napi::TypeError::New(env, "Put operation must have 'value'").ThrowAsJavaScriptException();
        return env.Null();
      }

      identifier_t* value = ValueFromJS(env, op.Get("value"));
      if (!value) {
        path_destroy(path);
        batch_destroy(batch);
        return env.Null();
      }

      rc = batch_add_put(batch, path, value);
      if (rc != 0) {
        // batch_add_put failed — ownership remains with caller
        path_destroy(path);
        identifier_destroy(value);
        batch_destroy(batch);
        Napi::Error::New(env, "Failed to add put to batch").ThrowAsJavaScriptException();
        return env.Null();
      }
    } else if (type == "del") {
      rc = batch_add_delete(batch, path);
      if (rc != 0) {
        path_destroy(path);
        batch_destroy(batch);
        Napi::Error::New(env, "Failed to add delete to batch").ThrowAsJavaScriptException();
        return env.Null();
      }
    } else {
      path_destroy(path);
      batch_destroy(batch);
      Napi::TypeError::New(env, "Operation type must be 'put' or 'del'").ThrowAsJavaScriptException();
      return env.Null();
    }
  }

  // Dispatch async batch operation
  AsyncOpContext* ctx = CreateOpContext(env, AsyncOpType::Batch, info, 1);

  promise_t* promise_c = bridge_.CreatePromise(ctx);
  if (!promise_c) {
    napi_value error_val = Napi::Error::New(env, "Failed to create async promise").Value();
    napi_value promise_val = ctx->promise;
    napi_reject_deferred(env, ctx->deferred, error_val);
    if (ctx->callback_ref) napi_delete_reference(env, ctx->callback_ref);
    delete ctx;
    batch_destroy(batch);
    return Napi::Value(env, promise_val);
  }

  ctx->promise_c = promise_c;
  ctx->batch = batch;

  database_write_batch(db_, batch, promise_c);

  return Napi::Value(env, ctx->promise);
}

// --- Sync operations (unchanged) ---

Napi::Value WaveDB::PutSync(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (!db_) {
    Napi::Error::New(env, "DATABASE_CLOSED: Database is closed").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  if (info.Length() < 2) {
    Napi::TypeError::New(env, "Key and value required").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  path_t* path = PathFromJS(env, info[0], delimiter_);
  if (!path) return env.Undefined();

  identifier_t* value = ValueFromJS(env, info[1]);
  if (!value) {
    path_destroy(path);
    return env.Undefined();
  }

  int rc = database_put_sync(db_, path, value);

  if (rc != 0) {
    Napi::Error::New(env, "IO_ERROR: Failed to put value").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  return env.Undefined();
}

Napi::Value WaveDB::GetSync(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (!db_) {
    Napi::Error::New(env, "DATABASE_CLOSED: Database is closed").ThrowAsJavaScriptException();
    return env.Null();
  }

  if (info.Length() < 1) {
    Napi::TypeError::New(env, "Key required").ThrowAsJavaScriptException();
    return env.Null();
  }

  path_t* path = PathFromJS(env, info[0], delimiter_);
  if (!path) return env.Null();

  identifier_t* result = NULL;
  int rc = database_get_sync(db_, path, &result);

  if (rc == 0 && result != NULL) {
    Napi::Value value = ValueToJS(env, result);
    identifier_destroy(result);
    return value;
  } else if (rc == -2) {
    return env.Null();
  } else {
    Napi::Error::New(env, "IO_ERROR: Failed to get value").ThrowAsJavaScriptException();
    return env.Null();
  }
}

Napi::Value WaveDB::DeleteSync(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (!db_) {
    Napi::Error::New(env, "DATABASE_CLOSED: Database is closed").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  if (info.Length() < 1) {
    Napi::TypeError::New(env, "Key required").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  path_t* path = PathFromJS(env, info[0], delimiter_);
  if (!path) return env.Undefined();

  int rc = database_delete_sync(db_, path);

  if (rc != 0) {
    Napi::Error::New(env, "IO_ERROR: Failed to delete value").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  return env.Undefined();
}

Napi::Value WaveDB::BatchSync(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (!db_) {
    Napi::Error::New(env, "DATABASE_CLOSED: Database is closed").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  if (info.Length() < 1 || !info[0].IsArray()) {
    Napi::TypeError::New(env, "Array of operations required").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  Napi::Array ops = info[0].As<Napi::Array>();

  for (uint32_t i = 0; i < ops.Length(); i++) {
    Napi::Object op = ops.Get(i).As<Napi::Object>();

    if (!op.Has("type") || !op.Has("key")) {
      Napi::TypeError::New(env, "Operation must have 'type' and 'key'").ThrowAsJavaScriptException();
      return env.Undefined();
    }

    std::string type = op.Get("type").As<Napi::String>().Utf8Value();
    path_t* path = PathFromJS(env, op.Get("key"), delimiter_);
    if (!path) return env.Undefined();

    int rc;
    if (type == "put") {
      if (!op.Has("value")) {
        Napi::TypeError::New(env, "Put operation must have 'value'").ThrowAsJavaScriptException();
        path_destroy(path);
        return env.Undefined();
      }

      identifier_t* value = ValueFromJS(env, op.Get("value"));
      if (!value) {
        path_destroy(path);
        return env.Undefined();
      }

      rc = database_put_sync(db_, path, value);
    } else if (type == "del") {
      rc = database_delete_sync(db_, path);
    } else {
      Napi::TypeError::New(env, "Operation type must be 'put' or 'del'").ThrowAsJavaScriptException();
      path_destroy(path);
      return env.Undefined();
    }

    if (rc != 0) {
      Napi::Error::New(env, "IO_ERROR: Batch operation failed").ThrowAsJavaScriptException();
      return env.Undefined();
    }
  }

  return env.Undefined();
}

// --- Object operations ---

void WaveDB::FlattenObject(Napi::Env env,
                          Napi::Object obj,
                          std::vector<std::string>& path_parts,
                          std::vector<BatchOp>& ops,
                          char delimiter) {
  Napi::Array keys = obj.GetPropertyNames();

  for (uint32_t i = 0; i < keys.Length(); i++) {
    std::string key = keys.Get(i).As<Napi::String>().Utf8Value();
    Napi::Value value = obj.Get(key);

    path_parts.push_back(key);

    if (value.IsObject() && !value.IsArray() && !value.IsBuffer()) {
      FlattenObject(env, value.As<Napi::Object>(), path_parts, ops, delimiter);
    } else if (value.IsArray()) {
      Napi::Array arr = value.As<Napi::Array>();
      for (uint32_t j = 0; j < arr.Length(); j++) {
        path_parts.push_back(std::to_string(j));

        BatchOp op;
        op.type = BatchOpType::PUT;
        op.path = PathFromParts(path_parts);
        op.value = ValueFromJS(env, arr.Get(j));

        if (!op.path || !op.value) {
          if (op.path) path_destroy(op.path);
          if (op.value) identifier_destroy(op.value);
          path_parts.pop_back();
          throw std::runtime_error("Failed to create path or value");
        }

        ops.push_back(op);
        path_parts.pop_back();
      }
    } else {
      BatchOp op;
      op.type = BatchOpType::PUT;
      op.path = PathFromParts(path_parts);
      op.value = ValueFromJS(env, value);

      if (!op.path || !op.value) {
        if (op.path) path_destroy(op.path);
        if (op.value) identifier_destroy(op.value);
        path_parts.pop_back();
        throw std::runtime_error("Failed to create path or value");
      }

      ops.push_back(op);
    }

    path_parts.pop_back();
  }
}

Napi::Value WaveDB::PutObject(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (!db_) {
    Napi::Error::New(env, "DATABASE_CLOSED: Database is closed").ThrowAsJavaScriptException();
    return env.Null();
  }

  if (info.Length() < 1 || !info[0].IsObject()) {
    Napi::TypeError::New(env, "Object required").ThrowAsJavaScriptException();
    return env.Null();
  }

  Napi::Object obj = info[0].As<Napi::Object>();
  std::vector<BatchOp> ops;
  std::vector<std::string> path_parts;

  try {
    FlattenObject(env, obj, path_parts, ops, delimiter_);
  } catch (const std::exception& e) {
    for (auto& op : ops) {
      if (op.path) path_destroy(op.path);
      if (op.value) identifier_destroy(op.value);
    }
    Napi::Error::New(env, e.what()).ThrowAsJavaScriptException();
    return env.Null();
  }

  // Build a C batch_t from the flattened operations
  batch_t* batch = batch_create(ops.size());
  if (!batch) {
    for (auto& op : ops) {
      if (op.path) path_destroy(op.path);
      if (op.value) identifier_destroy(op.value);
    }
    Napi::Error::New(env, "Failed to create batch").ThrowAsJavaScriptException();
    return env.Null();
  }

  for (auto& op : ops) {
    int rc;
    if (op.type == BatchOpType::PUT) {
      rc = batch_add_put(batch, op.path, op.value);
      if (rc != 0) {
        path_destroy(op.path);
        identifier_destroy(op.value);
      }
    } else {
      rc = batch_add_delete(batch, op.path);
      if (rc != 0) {
        path_destroy(op.path);
      }
    }
    if (rc != 0) {
      batch_destroy(batch);
      Napi::Error::New(env, "Failed to add operation to batch").ThrowAsJavaScriptException();
      return env.Null();
    }
  }

  // Dispatch async batch
  AsyncOpContext* ctx = CreateOpContext(env, AsyncOpType::Batch, info, 1);

  promise_t* promise_c = bridge_.CreatePromise(ctx);
  if (!promise_c) {
    napi_value error_val = Napi::Error::New(env, "Failed to create async promise").Value();
    napi_value promise_val = ctx->promise;
    napi_reject_deferred(env, ctx->deferred, error_val);
    if (ctx->callback_ref) napi_delete_reference(env, ctx->callback_ref);
    delete ctx;
    batch_destroy(batch);
    return Napi::Value(env, promise_val);
  }

  ctx->promise_c = promise_c;
  ctx->batch = batch;

  database_write_batch(db_, batch, promise_c);

  return Napi::Value(env, ctx->promise);
}

Napi::Value WaveDB::GetObject(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (!db_) {
    Napi::Error::New(env, "DATABASE_CLOSED: Database is closed").ThrowAsJavaScriptException();
    return env.Null();
  }

  if (info.Length() < 1) {
    Napi::TypeError::New(env, "Path required").ThrowAsJavaScriptException();
    return env.Null();
  }

  Napi::Function callback;
  if (info.Length() > 1 && info[1].IsFunction()) {
    callback = info[1].As<Napi::Function>();
  } else {
    callback = Napi::Function::New(env, [](const Napi::CallbackInfo& info) {
      return info.Env().Undefined();
    });
  }

  path_t* basePath = PathFromJS(env, info[0], delimiter_);
  if (!basePath) return env.Null();

  path_t* startPath = nullptr;
  path_t* endPath = nullptr;

  startPath = path_create();
  if (!startPath) {
    path_destroy(basePath);
    Napi::Error::New(env, "Failed to create start path").ThrowAsJavaScriptException();
    return env.Null();
  }

  for (size_t i = 0; i < static_cast<size_t>(basePath->identifiers.length); i++) {
    identifier_t* id = basePath->identifiers.data[i];
    REFERENCE(id, identifier_t);
    path_append(startPath, id);
  }

  std::vector<std::string> basePathParts;
  for (size_t i = 0; i < static_cast<size_t>(basePath->identifiers.length); i++) {
    identifier_t* id = basePath->identifiers.data[i];
    std::string part;
    for (size_t j = 0; j < static_cast<size_t>(id->chunks.length); j++) {
      chunk_t* chunk = id->chunks.data[j];
      const uint8_t* data = static_cast<const uint8_t*>(chunk_data_const(chunk));
      size_t size = chunk->size;
      part += std::string(reinterpret_cast<const char*>(data), size);
    }
    while (!part.empty() && (part.back() == '\0' || part.back() == ' ')) {
      part.pop_back();
    }
    basePathParts.push_back(part);
  }

  database_iterator_t* iter = database_scan_start(db_, startPath, nullptr);
  path_destroy(startPath);
  path_destroy(basePath);

  if (!iter) {
    Napi::Object result = Napi::Object::New(env);
    callback.Call({ env.Null(), result });
    Napi::Promise::Deferred deferred = Napi::Promise::Deferred::New(env);
    deferred.Resolve(result);
    return deferred.Promise();
  }

  std::vector<std::pair<std::vector<std::string>, Napi::Value>> entries;

  while (true) {
    path_t* outPath = nullptr;
    identifier_t* outValue = nullptr;

    int rc = database_scan_next(iter, &outPath, &outValue);
    if (rc != 0) break;

    std::vector<std::string> pathParts;
    for (size_t i = 0; i < static_cast<size_t>(outPath->identifiers.length); i++) {
      identifier_t* id = outPath->identifiers.data[i];
      std::string part;
      for (size_t j = 0; j < static_cast<size_t>(id->chunks.length); j++) {
        chunk_t* chunk = id->chunks.data[j];
        const uint8_t* data = static_cast<const uint8_t*>(chunk_data_const(chunk));
        size_t size = chunk->size;
        part += std::string(reinterpret_cast<const char*>(data), size);
      }
      while (!part.empty() && (part.back() == '\0' || part.back() == ' ')) {
        part.pop_back();
      }
      pathParts.push_back(part);
    }

    Napi::Value jsValue = ValueToJS(env, outValue);

    if (pathParts.size() >= basePathParts.size()) {
      bool matches = true;
      for (size_t i = 0; i < basePathParts.size() && matches; i++) {
        if (pathParts[i] != basePathParts[i]) {
          matches = false;
        }
      }
      if (matches) {
        entries.push_back({pathParts, jsValue});
      }
    }

    path_destroy(outPath);
    identifier_destroy(outValue);
  }

  database_scan_end(iter);

  Napi::Object result = ReconstructObject(env, entries);

  callback.Call({ env.Null(), result });

  Napi::Promise::Deferred deferred = Napi::Promise::Deferred::New(env);
  deferred.Resolve(result);
  return deferred.Promise();
}

Napi::Value WaveDB::GetObjectSync(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (!db_) {
    Napi::Error::New(env, "DATABASE_CLOSED: Database is closed").ThrowAsJavaScriptException();
    return env.Null();
  }

  if (info.Length() < 1) {
    Napi::TypeError::New(env, "Path required").ThrowAsJavaScriptException();
    return env.Null();
  }

  path_t* basePath = PathFromJS(env, info[0], delimiter_);
  if (!basePath) return env.Null();

  std::vector<std::string> basePathParts;
  for (size_t i = 0; i < static_cast<size_t>(basePath->identifiers.length); i++) {
    identifier_t* id = basePath->identifiers.data[i];
    std::string part;
    for (size_t j = 0; j < static_cast<size_t>(id->chunks.length); j++) {
      chunk_t* chunk = id->chunks.data[j];
      const uint8_t* data = static_cast<const uint8_t*>(chunk_data_const(chunk));
      size_t size = chunk->size;
      part += std::string(reinterpret_cast<const char*>(data), size);
    }
    while (!part.empty() && (part.back() == '\0' || part.back() == ' ')) {
      part.pop_back();
    }
    basePathParts.push_back(part);
  }

  path_t* startPath = path_create();
  if (!startPath) {
    path_destroy(basePath);
    Napi::Error::New(env, "Failed to create start path").ThrowAsJavaScriptException();
    return env.Null();
  }

  for (size_t i = 0; i < static_cast<size_t>(basePath->identifiers.length); i++) {
    identifier_t* id = basePath->identifiers.data[i];
    REFERENCE(id, identifier_t);
    path_append(startPath, id);
  }

  database_iterator_t* iter = database_scan_start(db_, startPath, nullptr);

  path_destroy(startPath);
  path_destroy(basePath);

  if (!iter) {
    return Napi::Object::New(env);
  }

  std::vector<std::pair<std::vector<std::string>, Napi::Value>> entries;

  while (true) {
    path_t* outPath = nullptr;
    identifier_t* outValue = nullptr;

    int rc = database_scan_next(iter, &outPath, &outValue);
    if (rc != 0) break;

    std::vector<std::string> pathParts;
    for (size_t i = 0; i < static_cast<size_t>(outPath->identifiers.length); i++) {
      identifier_t* id = outPath->identifiers.data[i];
      std::string part;
      for (size_t j = 0; j < static_cast<size_t>(id->chunks.length); j++) {
        chunk_t* chunk = id->chunks.data[j];
        const uint8_t* data = static_cast<const uint8_t*>(chunk_data_const(chunk));
        size_t size = chunk->size;
        part += std::string(reinterpret_cast<const char*>(data), size);
      }
      while (!part.empty() && (part.back() == '\0' || part.back() == ' ')) {
        part.pop_back();
      }
      pathParts.push_back(part);
    }

    Napi::Value jsValue = ValueToJS(env, outValue);

    if (pathParts.size() >= basePathParts.size()) {
      bool matches = true;
      for (size_t i = 0; i < basePathParts.size() && matches; i++) {
        if (pathParts[i] != basePathParts[i]) {
          matches = false;
        }
      }
      if (matches) {
        entries.push_back({pathParts, jsValue});
      }
    }

    path_destroy(outPath);
    identifier_destroy(outValue);
  }

  database_scan_end(iter);

  return ReconstructObject(env, entries);
}

Napi::Object WaveDB::ReconstructObject(Napi::Env env,
                                       const std::vector<std::pair<std::vector<std::string>, Napi::Value>>& entries) {
  Napi::Object result = Napi::Object::New(env);

  for (const auto& entry : entries) {
    const std::vector<std::string>& pathParts = entry.first;
    Napi::Value value = entry.second;

    if (pathParts.empty()) continue;

    Napi::Object current = result;
    for (size_t i = 0; i < pathParts.size() - 1; i++) {
      const std::string& key = pathParts[i];

      if (!current.Has(key)) {
        current.Set(key, Napi::Object::New(env));
      }
      current = current.Get(key).As<Napi::Object>();
    }

    current.Set(pathParts.back(), value);
  }

  return ConvertArrays(env, result).As<Napi::Object>();
}

Napi::Value WaveDB::ConvertArrays(Napi::Env env, Napi::Value value) {
  if (!value.IsObject()) {
    return value;
  }

  Napi::Object obj = value.As<Napi::Object>();
  Napi::Array keys = obj.GetPropertyNames();

  std::vector<uint32_t> indices;
  bool allNumeric = true;

  for (uint32_t i = 0; i < keys.Length(); i++) {
    Napi::Value key = keys.Get(i);
    if (!key.IsString()) {
      allNumeric = false;
      break;
    }
    std::string keyStr = key.As<Napi::String>().Utf8Value();
    char* end;
    long idx = strtol(keyStr.c_str(), &end, 10);
    if (*end != '\0' || idx < 0) {
      allNumeric = false;
      break;
    }
    indices.push_back(static_cast<uint32_t>(idx));
  }

  if (allNumeric && !indices.empty()) {
    std::sort(indices.begin(), indices.end());

    bool contiguous = true;
    for (size_t i = 0; i < indices.size(); i++) {
      if (indices[i] != i) {
        contiguous = false;
        break;
      }
    }

    if (contiguous) {
      Napi::Array arr = Napi::Array::New(env);
      for (uint32_t i = 0; i < indices.size(); i++) {
        Napi::Value val = obj.Get(Napi::String::New(env, std::to_string(i)));
        arr.Set(i, ConvertArrays(env, val));
      }
      return arr;
    }
  }

  for (uint32_t i = 0; i < keys.Length(); i++) {
    Napi::Value key = keys.Get(i);
    std::string keyStr = key.As<Napi::String>().Utf8Value();
    Napi::Value val = obj.Get(key);
    obj.Set(key, ConvertArrays(env, val));
  }

  return value;
}

Napi::Value WaveDB::CreateReadStream(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  Napi::Object options = Napi::Object::New(env);
  if (info.Length() > 0 && info[0].IsObject()) {
    options = info[0].As<Napi::Object>();
  }

  Napi::External<database_t> dbExternal = Napi::External<database_t>::New(env, db_);
  Napi::Object iterObj = Iterator::constructor_.New({ dbExternal, options, Value() });

  return iterObj;
}

// --- Lifecycle ---

Napi::Value WaveDB::Close(const Napi::CallbackInfo& info) {
  if (db_) {
    // Shutdown the async bridge — waits for pending operations
    bridge_.Shutdown();

    // Flush all thread-local WALs
    if (db_->wal_manager != NULL) {
      wal_manager_flush(db_->wal_manager);
    }

    database_t* db = db_;
    db_ = nullptr;
    database_destroy(db);
  }
  return info.Env().Undefined();
}