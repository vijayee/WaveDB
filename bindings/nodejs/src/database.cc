// C++ headers that include <atomic> must come before C headers
// that use ATOMIC_TYPE() macros expanding to std::atomic<T> in C++.
#include <atomic>

#include <napi.h>
#include <cctype>
#include <string>
#include <unistd.h>
#include "../../../src/Database/database.h"
#include "../../../src/Database/database_config.h"
#include "../../../src/Database/database_iterator.h"
#include "../../../src/Storage/encryption.h"
#include "path.h"
#include "identifier.h"
#include "async_bridge.h"
#include "iterator.h"
#include "database.h"

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
    InstanceMethod("putCb", &WaveDB::PutCb),
    InstanceMethod("getCb", &WaveDB::GetCb),
    InstanceMethod("delCb", &WaveDB::DeleteCb),
    InstanceMethod("batchCb", &WaveDB::BatchCb),
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

    if (options.Has("bnodeCacheMemoryMb")) {
      Napi::Number val = options.Get("bnodeCacheMemoryMb").As<Napi::Number>();
      config->bnode_cache_memory_mb = static_cast<size_t>(val.Uint32Value());
    }

    if (options.Has("bnodeCacheShards")) {
      Napi::Number val = options.Get("bnodeCacheShards").As<Napi::Number>();
      config->bnode_cache_shards = static_cast<uint16_t>(val.Uint32Value());
    }

    if (options.Has("wal") && options.Get("wal").IsObject()) {
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

  // Check for encryption configuration
  bool has_encryption = false;
  Napi::Object encryptionObj;
  if (info.Length() > 1 && info[1].IsObject()) {
    Napi::Object options = info[1].As<Napi::Object>();
    if (options.Has("encryption") && options.Get("encryption").IsObject()) {
      has_encryption = true;
      encryptionObj = options.Get("encryption").As<Napi::Object>();
    }
  }

  if (has_encryption) {
    // Encrypted database path
    encrypted_database_config_t* enc_config = encrypted_database_config_default();
    if (!enc_config) {
      database_config_destroy(config);
      Napi::Error::New(env, "Failed to create encrypted configuration").ThrowAsJavaScriptException();
      return;
    }

    // Copy base config settings into the embedded config
    enc_config->config.chunk_size = config->chunk_size;
    enc_config->config.btree_node_size = config->btree_node_size;
    enc_config->config.enable_persist = config->enable_persist;
    enc_config->config.lru_memory_mb = config->lru_memory_mb;
    enc_config->config.lru_shards = config->lru_shards;
    enc_config->config.bnode_cache_memory_mb = config->bnode_cache_memory_mb;
    enc_config->config.bnode_cache_shards = config->bnode_cache_shards;
    enc_config->config.worker_threads = config->worker_threads;
    enc_config->config.wal_config = config->wal_config;
    enc_config->config.timer_resolution_ms = config->timer_resolution_ms;

    // Read encryption.type
    if (!encryptionObj.Has("type") || !encryptionObj.Get("type").IsString()) {
      encrypted_database_config_destroy(enc_config);
      database_config_destroy(config);
      Napi::TypeError::New(env, "encryption.type is required ('symmetric' or 'asymmetric')").ThrowAsJavaScriptException();
      return;
    }

    std::string encType = encryptionObj.Get("type").As<Napi::String>().Utf8Value();
    if (encType == "symmetric") {
      encrypted_database_config_set_type(enc_config, ENCRYPTION_SYMMETRIC);

      if (!encryptionObj.Has("key") || !encryptionObj.Get("key").IsBuffer()) {
        encrypted_database_config_destroy(enc_config);
        database_config_destroy(config);
        Napi::TypeError::New(env, "encryption.key is required for symmetric encryption (32-byte Buffer)").ThrowAsJavaScriptException();
        return;
      }

      Napi::Buffer<uint8_t> keyBuf = encryptionObj.Get("key").As<Napi::Buffer<uint8_t>>();
      if (keyBuf.Length() != 32) {
        encrypted_database_config_destroy(enc_config);
        database_config_destroy(config);
        Napi::RangeError::New(env, "encryption.key must be exactly 32 bytes for AES-256").ThrowAsJavaScriptException();
        return;
      }
      encrypted_database_config_set_symmetric_key(enc_config, keyBuf.Data(), keyBuf.Length());

    } else if (encType == "asymmetric") {
      encrypted_database_config_set_type(enc_config, ENCRYPTION_ASYMMETRIC);

      if (!encryptionObj.Has("publicKey") || !encryptionObj.Get("publicKey").IsBuffer()) {
        encrypted_database_config_destroy(enc_config);
        database_config_destroy(config);
        Napi::TypeError::New(env, "encryption.publicKey is required for asymmetric encryption (DER Buffer)").ThrowAsJavaScriptException();
        return;
      }

      Napi::Buffer<uint8_t> pubKeyBuf = encryptionObj.Get("publicKey").As<Napi::Buffer<uint8_t>>();
      encrypted_database_config_set_asymmetric_public_key(enc_config, pubKeyBuf.Data(), pubKeyBuf.Length());

      // Private key is optional (write-only mode)
      if (encryptionObj.Has("privateKey") && encryptionObj.Get("privateKey").IsBuffer()) {
        Napi::Buffer<uint8_t> privKeyBuf = encryptionObj.Get("privateKey").As<Napi::Buffer<uint8_t>>();
        encrypted_database_config_set_asymmetric_private_key(enc_config, privKeyBuf.Data(), privKeyBuf.Length());
      }

    } else {
      encrypted_database_config_destroy(enc_config);
      database_config_destroy(config);
      Napi::TypeError::New(env, "encryption.type must be 'symmetric' or 'asymmetric'").ThrowAsJavaScriptException();
      return;
    }

    db_ = database_create_encrypted(path.c_str(), enc_config, &error_code);
    encrypted_database_config_destroy(enc_config);
    database_config_destroy(config);

    if (!db_) {
      if (error_code == DATABASE_ERR_ENCRYPTION_REQUIRED) {
        Napi::Error::New(env, "Encryption required: this database was created with encryption").ThrowAsJavaScriptException();
        return;
      } else if (error_code == DATABASE_ERR_ENCRYPTION_KEY_INVALID) {
        Napi::Error::New(env, "Invalid encryption key").ThrowAsJavaScriptException();
        return;
      } else if (error_code == DATABASE_ERR_ENCRYPTION_UNSUPPORTED) {
        Napi::Error::New(env, "Encryption unsupported: OpenSSL not available").ThrowAsJavaScriptException();
        return;
      } else {
        Napi::Error::New(env, "Failed to create encrypted database").ThrowAsJavaScriptException();
        return;
      }
    }
  } else {
    // Unencrypted database path (existing behavior)
    db_ = database_create_with_config(path.c_str(), config, &error_code);
    database_config_destroy(config);

    if (!db_) {
      Napi::Error::New(env, "Failed to create database").ThrowAsJavaScriptException();
      return;
    }
  }

  // Initialize the async bridge for all async operations
  bridge_.Init(env);
}

WaveDB::~WaveDB() {
  if (db_) {
    // Shutdown the async bridge before destroying the database
    // to ensure pending operations complete before teardown
    bridge_.Shutdown();
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

  database_put_raw(db_, key_str.c_str(), key_str.size(), delimiter_,
                   reinterpret_cast<const uint8_t*>(val_str.data()), val_str.size(), promise_c);

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

  database_get_raw(db_, key_str.c_str(), key_str.size(), delimiter_, promise_c);

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

  database_delete_raw(db_, key_str.c_str(), key_str.size(), delimiter_, promise_c);

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

  Napi::Array js_ops = info[0].As<Napi::Array>();
  uint32_t count = js_ops.Length();
  if (count == 0) {
    // Empty batch — resolve immediately
    Napi::Promise::Deferred deferred = Napi::Promise::Deferred::New(env);
    deferred.Resolve(env.Undefined());
    return deferred.Promise();
  }

  // Build raw_op_t array with stable string storage
  // These vectors must stay alive until database_batch_raw copies data internally
  auto* key_strings = new std::vector<std::string>(count);
  auto* val_strings = new std::vector<std::string>();
  val_strings->reserve(count);
  auto* raw_ops = new std::vector<raw_op_t>(count);

  for (uint32_t i = 0; i < count; i++) {
    Napi::Object op = js_ops.Get(i).As<Napi::Object>();

    if (!op.Has("type") || !op.Has("key")) {
      delete key_strings;
      delete val_strings;
      delete raw_ops;
      Napi::TypeError::New(env, "Operation must have 'type' and 'key'").ThrowAsJavaScriptException();
      return env.Null();
    }

    std::string type = op.Get("type").As<Napi::String>().Utf8Value();

    // Key
    std::string key_str = KeyFromJSDynamic(env, op.Get("key"), delimiter_);
    if (env.IsExceptionPending()) {
      delete key_strings;
      delete val_strings;
      delete raw_ops;
      return env.Null();
    }
    (*key_strings)[i] = key_str;
    (*raw_ops)[i].key = (*key_strings)[i].c_str();
    (*raw_ops)[i].key_len = (*key_strings)[i].size();

    if (type == "put") {
      if (!op.Has("value")) {
        delete key_strings;
        delete val_strings;
        delete raw_ops;
        Napi::TypeError::New(env, "Put operation must have 'value'").ThrowAsJavaScriptException();
        return env.Null();
      }
      (*raw_ops)[i].type = 0;

      std::string val_str;
      Napi::Value valArg = op.Get("value");
      if (!ValueFromJSDynamic(env, valArg, val_str)) {
        delete key_strings;
        delete val_strings;
        delete raw_ops;
        return env.Null();
      }

      val_strings->push_back(val_str);
      (*raw_ops)[i].value = reinterpret_cast<const uint8_t*>(val_strings->back().c_str());
      (*raw_ops)[i].value_len = val_strings->back().size();
    } else if (type == "del") {
      (*raw_ops)[i].type = 1;
      (*raw_ops)[i].value = nullptr;
      (*raw_ops)[i].value_len = 0;
    } else {
      delete key_strings;
      delete val_strings;
      delete raw_ops;
      Napi::TypeError::New(env, "Operation type must be 'put' or 'del'").ThrowAsJavaScriptException();
      return env.Null();
    }
  }

  AsyncOpContext* ctx = CreateOpContext(env, AsyncOpType::Batch, info, 1);

  promise_t* promise_c = bridge_.CreatePromise(ctx);
  if (!promise_c) {
    napi_value error_val = Napi::Error::New(env, "Failed to create async promise").Value();
    napi_value promise_val = ctx->promise;
    napi_reject_deferred(env, ctx->deferred, error_val);
    if (ctx->callback_ref) napi_delete_reference(env, ctx->callback_ref);
    delete ctx;
    delete key_strings;
    delete val_strings;
    delete raw_ops;
    return Napi::Value(env, promise_val);
  }

  ctx->promise_c = promise_c;

  int rc = database_batch_raw(db_, delimiter_, raw_ops->data(), raw_ops->size(), promise_c);

  // database_batch_raw copies all data internally before dispatching,
  // so safe to free the heap-allocated vectors now
  delete key_strings;
  delete val_strings;
  delete raw_ops;

  if (rc != 0) {
    napi_value error_val = Napi::Error::New(env, "IO_ERROR: Failed to dispatch batch").Value();
    napi_value promise_val = ctx->promise;
    napi_reject_deferred(env, ctx->deferred, error_val);
    if (ctx->callback_ref) napi_delete_reference(env, ctx->callback_ref);
    delete ctx;
    return Napi::Value(env, promise_val);
  }

  return Napi::Value(env, ctx->promise);
}

// --- Callback-style async operations (use C async API with error-first callback) ---

Napi::Value WaveDB::GetCb(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (!db_) {
    Napi::Error::New(env, "DATABASE_CLOSED: Database is closed").ThrowAsJavaScriptException();
    return env.Null();
  }

  if (info.Length() < 2 || !info[1].IsFunction()) {
    Napi::TypeError::New(env, "Key and callback required").ThrowAsJavaScriptException();
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

  database_get_raw(db_, key_str.c_str(), key_str.size(), delimiter_, promise_c);

  return Napi::Value(env, ctx->promise);
}

Napi::Value WaveDB::PutCb(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (!db_) {
    Napi::Error::New(env, "DATABASE_CLOSED: Database is closed").ThrowAsJavaScriptException();
    return env.Null();
  }

  if (info.Length() < 3 || !info[2].IsFunction()) {
    Napi::TypeError::New(env, "Key, value, and callback required").ThrowAsJavaScriptException();
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

  database_put_raw(db_, key_str.c_str(), key_str.size(), delimiter_,
                   reinterpret_cast<const uint8_t*>(val_str.data()), val_str.size(), promise_c);

  return Napi::Value(env, ctx->promise);
}

Napi::Value WaveDB::DeleteCb(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (!db_) {
    Napi::Error::New(env, "DATABASE_CLOSED: Database is closed").ThrowAsJavaScriptException();
    return env.Null();
  }

  if (info.Length() < 2 || !info[1].IsFunction()) {
    Napi::TypeError::New(env, "Key and callback required").ThrowAsJavaScriptException();
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

  database_delete_raw(db_, key_str.c_str(), key_str.size(), delimiter_, promise_c);

  return Napi::Value(env, ctx->promise);
}

Napi::Value WaveDB::BatchCb(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (!db_) {
    Napi::Error::New(env, "DATABASE_CLOSED: Database is closed").ThrowAsJavaScriptException();
    return env.Null();
  }

  if (info.Length() < 2 || !info[0].IsArray() || !info[1].IsFunction()) {
    Napi::TypeError::New(env, "Array of operations and callback required").ThrowAsJavaScriptException();
    return env.Null();
  }

  Napi::Array js_ops = info[0].As<Napi::Array>();
  uint32_t count = js_ops.Length();
  if (count == 0) {
    // Empty batch — call callback with no error
    Napi::Function callback = info[1].As<Napi::Function>();
    callback.Call({ env.Null() });
    Napi::Promise::Deferred deferred = Napi::Promise::Deferred::New(env);
    deferred.Resolve(env.Undefined());
    return deferred.Promise();
  }

  // Build raw_op_t array with stable string storage
  auto* key_strings = new std::vector<std::string>(count);
  auto* val_strings = new std::vector<std::string>();
  val_strings->reserve(count);
  auto* raw_ops = new std::vector<raw_op_t>(count);

  for (uint32_t i = 0; i < count; i++) {
    Napi::Object op = js_ops.Get(i).As<Napi::Object>();

    if (!op.Has("type") || !op.Has("key")) {
      delete key_strings;
      delete val_strings;
      delete raw_ops;
      Napi::TypeError::New(env, "Operation must have 'type' and 'key'").ThrowAsJavaScriptException();
      return env.Null();
    }

    std::string type = op.Get("type").As<Napi::String>().Utf8Value();

    // Key
    std::string key_str = KeyFromJSDynamic(env, op.Get("key"), delimiter_);
    if (env.IsExceptionPending()) {
      delete key_strings;
      delete val_strings;
      delete raw_ops;
      return env.Null();
    }
    (*key_strings)[i] = key_str;
    (*raw_ops)[i].key = (*key_strings)[i].c_str();
    (*raw_ops)[i].key_len = (*key_strings)[i].size();

    if (type == "put") {
      if (!op.Has("value")) {
        delete key_strings;
        delete val_strings;
        delete raw_ops;
        Napi::TypeError::New(env, "Put operation must have 'value'").ThrowAsJavaScriptException();
        return env.Null();
      }
      (*raw_ops)[i].type = 0;

      std::string val_str;
      Napi::Value valArg = op.Get("value");
      if (!ValueFromJSDynamic(env, valArg, val_str)) {
        delete key_strings;
        delete val_strings;
        delete raw_ops;
        return env.Null();
      }

      val_strings->push_back(val_str);
      (*raw_ops)[i].value = reinterpret_cast<const uint8_t*>(val_strings->back().c_str());
      (*raw_ops)[i].value_len = val_strings->back().size();
    } else if (type == "del") {
      (*raw_ops)[i].type = 1;
      (*raw_ops)[i].value = nullptr;
      (*raw_ops)[i].value_len = 0;
    } else {
      delete key_strings;
      delete val_strings;
      delete raw_ops;
      Napi::TypeError::New(env, "Operation type must be 'put' or 'del'").ThrowAsJavaScriptException();
      return env.Null();
    }
  }

  // callbackArgIndex = 1 for BatchCb
  AsyncOpContext* ctx = CreateOpContext(env, AsyncOpType::Batch, info, 1);

  promise_t* promise_c = bridge_.CreatePromise(ctx);
  if (!promise_c) {
    napi_value error_val = Napi::Error::New(env, "Failed to create async promise").Value();
    napi_value promise_val = ctx->promise;
    napi_reject_deferred(env, ctx->deferred, error_val);
    if (ctx->callback_ref) napi_delete_reference(env, ctx->callback_ref);
    delete ctx;
    delete key_strings;
    delete val_strings;
    delete raw_ops;
    return Napi::Value(env, promise_val);
  }

  ctx->promise_c = promise_c;

  int rc = database_batch_raw(db_, delimiter_, raw_ops->data(), raw_ops->size(), promise_c);

  // database_batch_raw copies all data internally before dispatching
  delete key_strings;
  delete val_strings;
  delete raw_ops;

  if (rc != 0) {
    napi_value error_val = Napi::Error::New(env, "IO_ERROR: Failed to dispatch batch").Value();
    napi_value promise_val = ctx->promise;
    napi_reject_deferred(env, ctx->deferred, error_val);
    if (ctx->callback_ref) napi_delete_reference(env, ctx->callback_ref);
    delete ctx;
    return Napi::Value(env, promise_val);
  }

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

  std::string key_str = KeyFromJSDynamic(env, info[0], delimiter_);
  if (env.IsExceptionPending()) return env.Undefined();

  std::string val_str;
  const uint8_t* val_buf;
  size_t val_len;

  if (!ValueFromJSZeroCopy(env, info[1], val_str, &val_buf, &val_len)) {
    return env.Undefined();
  }

  int rc = database_put_sync_raw(db_, key_str.c_str(), key_str.size(), delimiter_, val_buf, val_len);
  if (rc != 0) {
    Napi::Error::New(env, "IO_ERROR: Failed to put value").ThrowAsJavaScriptException();
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

  std::string key_str = KeyFromJSDynamic(env, info[0], delimiter_);
  if (env.IsExceptionPending()) return env.Null();

  uint8_t* value = nullptr;
  size_t value_len = 0;
  int rc = database_get_sync_raw(db_, key_str.c_str(), key_str.size(), delimiter_, &value, &value_len);

  if (rc == 0 && value != nullptr) {
    // Strip trailing null bytes (chunk padding from identifier serialization)
    while (value_len > 0 && (value[value_len - 1] == '\0' || value[value_len - 1] == ' ')) {
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

  std::string key_str = KeyFromJSDynamic(env, info[0], delimiter_);
  if (env.IsExceptionPending()) return env.Undefined();

  int rc = database_delete_sync_raw(db_, key_str.c_str(), key_str.size(), delimiter_);
  if (rc != 0) {
    Napi::Error::New(env, "IO_ERROR: Failed to delete value").ThrowAsJavaScriptException();
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

  int rc = database_batch_sync_raw(db_, delimiter_, ops.data(), ops.size());
  if (rc != 0) {
    Napi::Error::New(env, "IO_ERROR: Batch operation failed").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  return env.Undefined();
}

// --- Object operations ---

static bool FlattenObjectRaw(Napi::Env env,
                             Napi::Object obj,
                             std::vector<std::string>& path_parts,
                             std::vector<std::string>& key_strings,
                             std::vector<std::string>& val_strings,
                             char delimiter) {
  Napi::Array keys = obj.GetPropertyNames();

  for (uint32_t i = 0; i < keys.Length(); i++) {
    std::string key = keys.Get(i).As<Napi::String>().Utf8Value();
    Napi::Value value = obj.Get(key);

    path_parts.push_back(key);

    if (value.IsObject() && !value.IsArray() && !value.IsBuffer()) {
      if (!FlattenObjectRaw(env, value.As<Napi::Object>(), path_parts, key_strings, val_strings, delimiter)) {
        return false;
      }
    } else if (value.IsArray()) {
      Napi::Array arr = value.As<Napi::Array>();
      for (uint32_t j = 0; j < arr.Length(); j++) {
        path_parts.push_back(std::to_string(j));

        std::string key_str;
        for (size_t k = 0; k < path_parts.size(); k++) {
          if (k > 0) key_str += delimiter;
          key_str += path_parts[k];
        }
        key_strings.push_back(key_str);

        Napi::Value arrVal = arr.Get(j);
        std::string arr_val_str;
        if (!ValueFromJSDynamic(env, arrVal, arr_val_str)) {
          return false;
        }
        val_strings.push_back(arr_val_str);

        path_parts.pop_back();
      }
    } else {
      std::string key_str;
      for (size_t k = 0; k < path_parts.size(); k++) {
        if (k > 0) key_str += delimiter;
        key_str += path_parts[k];
      }
      key_strings.push_back(key_str);

      std::string leaf_val_str;
      if (!ValueFromJSDynamic(env, value, leaf_val_str)) {
        return false;
      }
      val_strings.push_back(leaf_val_str);
    }

    path_parts.pop_back();
  }

  return true;
}

Napi::Value WaveDB::PutObject(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (!db_) {
    Napi::Error::New(env, "DATABASE_CLOSED: Database is closed").ThrowAsJavaScriptException();
    return env.Null();
  }

  if (info.Length() < 2) {
    Napi::TypeError::New(env, "Key prefix and object required").ThrowAsJavaScriptException();
    return env.Null();
  }

  std::string key_prefix = KeyFromJSDynamic(env, info[0], delimiter_);
  if (env.IsExceptionPending()) return env.Null();

  if (!info[1].IsObject()) {
    Napi::TypeError::New(env, "Object required as second argument").ThrowAsJavaScriptException();
    return env.Null();
  }

  Napi::Object obj = info[1].As<Napi::Object>();
  std::vector<std::string> key_strings;
  std::vector<std::string> val_strings;
  std::vector<std::string> path_parts;

  // Seed path_parts with key prefix segments
  if (!key_prefix.empty()) {
    size_t start = 0;
    size_t pos;
    while ((pos = key_prefix.find(delimiter_, start)) != std::string::npos) {
      std::string segment = key_prefix.substr(start, pos - start);
      if (!segment.empty()) path_parts.push_back(segment);
      start = pos + 1;
    }
    std::string last = key_prefix.substr(start);
    if (!last.empty()) path_parts.push_back(last);
  }

  bool ok;
  try {
    ok = FlattenObjectRaw(env, obj, path_parts, key_strings, val_strings, delimiter_);
  } catch (const std::exception& e) {
    Napi::Error::New(env, e.what()).ThrowAsJavaScriptException();
    return env.Null();
  }

  if (!ok || env.IsExceptionPending()) return env.Null();

  if (key_strings.empty()) {
    if (info.Length() > 2 && info[2].IsFunction()) {
      Napi::Function callback = info[2].As<Napi::Function>();
      callback.Call({env.Null()});
    }
    Napi::Promise::Deferred deferred = Napi::Promise::Deferred::New(env);
    deferred.Resolve(env.Undefined());
    return deferred.Promise();
  }

  // Build raw_op_t array from finalized string vectors (no more push_backs,
  // so c_str() pointers are stable)
  std::vector<raw_op_t> ops(key_strings.size());
  for (size_t i = 0; i < key_strings.size(); i++) {
    ops[i].type = 0;
    ops[i].key = key_strings[i].c_str();
    ops[i].key_len = key_strings[i].size();
    ops[i].value = reinterpret_cast<const uint8_t*>(val_strings[i].c_str());
    ops[i].value_len = val_strings[i].size();
  }

  int rc = database_batch_sync_raw(db_, delimiter_, ops.data(), ops.size());
  if (rc != 0) {
    Napi::Error::New(env, "IO_ERROR: Batch put failed").ThrowAsJavaScriptException();
    return env.Null();
  }

  // Create Promise first, then invoke callback
  Napi::Promise::Deferred deferred = Napi::Promise::Deferred::New(env);
  deferred.Resolve(env.Undefined());

  // Call callback if provided (after Promise creation to avoid pending exception UB)
  if (!env.IsExceptionPending() && info.Length() > 2 && info[2].IsFunction()) {
    Napi::Function callback = info[2].As<Napi::Function>();
    callback.Call({env.Null()});
  }

  return deferred.Promise();
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

  std::string prefix_str = KeyFromJSDynamic(env, info[0], delimiter_);
  if (env.IsExceptionPending()) return env.Null();

  raw_result_t* results = nullptr;
  size_t count = 0;
  int rc = database_scan_sync_raw(db_, prefix_str.c_str(), prefix_str.size(), delimiter_, &results, &count);

  if (rc != 0) {
    Napi::Error::New(env, "IO_ERROR: Scan failed").ThrowAsJavaScriptException();
    return env.Null();
  }

  // Reconstruct JS object from flat results
  Napi::Object result_obj = Napi::Object::New(env);
  for (size_t i = 0; i < count; i++) {
    Napi::Value val;
    if (results[i].value == nullptr || results[i].value_len == 0) {
      val = Napi::String::New(env, "");
    } else {
      // Strip trailing null bytes (chunk padding from identifier serialization)
      size_t val_len = results[i].value_len;
      while (val_len > 0 && (results[i].value[val_len - 1] == '\0' || results[i].value[val_len - 1] == ' ')) {
        val_len--;
      }

      bool printable = true;
      for (size_t j = 0; j < val_len; j++) {
        if (!isprint(results[i].value[j]) && results[i].value[j] != '\t' &&
            results[i].value[j] != '\n' && results[i].value[j] != '\r') {
          printable = false;
          break;
        }
      }
      if (printable) {
        val = Napi::String::New(env, std::string(reinterpret_cast<const char*>(results[i].value), val_len));
      } else {
        val = Napi::Buffer<uint8_t>::Copy(env, results[i].value, val_len);
      }
    }

    std::string key(results[i].key, results[i].key_len);
    // Strip trailing null bytes from key (chunk padding)
    while (!key.empty() && (key.back() == '\0' || key.back() == ' ')) {
      key.pop_back();
    }
    Napi::Object current = result_obj;
    size_t start = 0;
    for (size_t j = 0; j <= key.size(); j++) {
      if (j == key.size() || key[j] == delimiter_) {
        std::string segment = key.substr(start, j - start);
        if (j == key.size()) {
          current.Set(segment, val);
        } else {
          if (!current.Has(segment) || !current.Get(segment).IsObject()) {
            current.Set(segment, Napi::Object::New(env));
          }
          current = current.Get(segment).As<Napi::Object>();
        }
        start = j + 1;
      }
    }
  }

  database_raw_results_free(results, count);

  Napi::Value converted = ConvertArrays(env, result_obj);

  // Create Promise first, then invoke callback
  Napi::Promise::Deferred deferred = Napi::Promise::Deferred::New(env);
  deferred.Resolve(converted);

  if (!env.IsExceptionPending() && info.Length() > 1 && info[1].IsFunction()) {
    Napi::Function callback = info[1].As<Napi::Function>();
    callback.Call({ env.Null(), converted });
  }

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

  std::string prefix_str = KeyFromJSDynamic(env, info[0], delimiter_);
  if (env.IsExceptionPending()) return env.Null();

  raw_result_t* results = nullptr;
  size_t count = 0;
  int rc = database_scan_sync_raw(db_, prefix_str.c_str(), prefix_str.size(), delimiter_, &results, &count);

  if (rc != 0) {
    Napi::Error::New(env, "IO_ERROR: Scan failed").ThrowAsJavaScriptException();
    return env.Null();
  }

  // Reconstruct JS object from flat results
  Napi::Object result_obj = Napi::Object::New(env);
  for (size_t i = 0; i < count; i++) {
    Napi::Value val;
    if (results[i].value == nullptr || results[i].value_len == 0) {
      val = Napi::String::New(env, "");
    } else {
      // Strip trailing null bytes (chunk padding from identifier serialization)
      size_t val_len = results[i].value_len;
      while (val_len > 0 && (results[i].value[val_len - 1] == '\0' || results[i].value[val_len - 1] == ' ')) {
        val_len--;
      }

      // Check if printable ASCII to decide string vs Buffer
      bool printable = true;
      for (size_t j = 0; j < val_len; j++) {
        if (!isprint(results[i].value[j]) && results[i].value[j] != '\t' &&
            results[i].value[j] != '\n' && results[i].value[j] != '\r') {
          printable = false;
          break;
        }
      }
      if (printable) {
        val = Napi::String::New(env, std::string(reinterpret_cast<const char*>(results[i].value), val_len));
      } else {
        val = Napi::Buffer<uint8_t>::Copy(env, results[i].value, val_len);
      }
    }

    // Walk/create nested object path and set the value at the leaf
    std::string key(results[i].key, results[i].key_len);
    // Strip trailing null bytes from key (chunk padding)
    while (!key.empty() && (key.back() == '\0' || key.back() == ' ')) {
      key.pop_back();
    }
    Napi::Object current = result_obj;
    size_t start = 0;
    for (size_t j = 0; j <= key.size(); j++) {
      if (j == key.size() || key[j] == delimiter_) {
        std::string segment = key.substr(start, j - start);
        if (j == key.size()) {
          current.Set(segment, val);
        } else {
          if (!current.Has(segment) || !current.Get(segment).IsObject()) {
            current.Set(segment, Napi::Object::New(env));
          }
          current = current.Get(segment).As<Napi::Object>();
        }
        start = j + 1;
      }
    }
  }

  database_raw_results_free(results, count);

  return ConvertArrays(env, result_obj);
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

  if (!db_) {
    Napi::Error::New(env, "DATABASE_CLOSED: Database is closed").ThrowAsJavaScriptException();
    return env.Null();
  }

  Napi::Object options = Napi::Object::New(env);
  if (info.Length() > 0 && info[0].IsObject()) {
    options = info[0].As<Napi::Object>();
  }

  // Napi::External without a finalizer creates a wrapper that V8 may not GC,
  // causing ASan/LeakSan to report a 40-byte leak. Pass a no-op finalizer so
  // V8 knows the wrapper can be collected. The db_ pointer is owned by the
  // WaveDB wrapper, not the External — do NOT free db_ in the finalizer.
  Napi::External<database_t> dbExternal = Napi::External<database_t>::New(
    env, db_, [](Napi::Env, database_t*) { /* no-op: WaveDB owns the pointer */ });
  Napi::Object iterObj = Iterator::constructor_.New({ dbExternal, options, info.This() });

  return iterObj;
}

// --- Lifecycle ---

Napi::Value WaveDB::Close(const Napi::CallbackInfo& info) {
  if (db_) {
    // Shutdown the async bridge — waits for pending operations
    bridge_.Shutdown();

    database_t* db = db_;
    db_ = nullptr;
    database_destroy(db);
  }
  return info.Env().Undefined();
}