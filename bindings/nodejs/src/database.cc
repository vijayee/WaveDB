#include <napi.h>
#include <string>
#include <unistd.h>  // for usleep
#include "../../../src/Database/database.h"
#include "path.h"
#include "identifier.h"
#include "async_worker.h"
#include "put_worker.h"
#include "get_worker.h"
#include "del_worker.h"
#include "batch_worker.h"
#include "iterator.h"

class WaveDB : public Napi::ObjectWrap<WaveDB> {
public:
  static Napi::Object Init(Napi::Env env, Napi::Object exports);

  WaveDB(const Napi::CallbackInfo& info);
  ~WaveDB();

private:
  database_t* db_;
  char delimiter_;

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
                                        const std::vector<std::pair<std::string, std::string>>& entries,
                                        const std::string& base_path,
                                        char delimiter);

  // Object operations
  Napi::Value PutObject(const Napi::CallbackInfo& info);
  Napi::Value GetObject(const Napi::CallbackInfo& info);

  // Streaming
  Napi::Value CreateReadStream(const Napi::CallbackInfo& info);

  // Lifecycle
  Napi::Value Close(const Napi::CallbackInfo& info);

  static Napi::FunctionReference constructor_;
  static void Cleanup(void* arg);
};

Napi::FunctionReference WaveDB::constructor_;

// Cleanup callback to release static references before Node.js shuts down
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
    InstanceMethod("createReadStream", &WaveDB::CreateReadStream),
    InstanceMethod("close", &WaveDB::Close),
  });

  constructor_ = Napi::Persistent(func);
  exports.Set("WaveDB", func);

  // Register cleanup hook to reset constructor_ before environment destruction
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

  // Parse options
  if (info.Length() > 1 && info[1].IsObject()) {
    Napi::Object options = info[1].As<Napi::Object>();
    if (options.Has("delimiter")) {
      Napi::String delim = options.Get("delimiter").As<Napi::String>();
      std::string delimStr = delim.Utf8Value();
      if (delimStr.length() != 1) {
        Napi::TypeError::New(env, "Delimiter must be a single character").ThrowAsJavaScriptException();
        return;
      }
      delimiter_ = delimStr[0];
    }
  }

  // Create database with defaults
  int error_code = 0;
  db_ = database_create(path.c_str(), 0, NULL, 0, 0, 1, 0, NULL, NULL, &error_code);
  if (!db_) {
    Napi::Error::New(env, "Failed to create database").ThrowAsJavaScriptException();
    return;
  }
}

WaveDB::~WaveDB() {
  if (db_) {
    database_destroy(db_);
    db_ = nullptr;
  }
}

Napi::Value WaveDB::Close(const Napi::CallbackInfo& info) {
  if (db_) {
    // Flush WAL and save index before closing
    database_snapshot(db_);

    // Wait for all references to be released
    // This ensures all async operations complete before we destroy
    uint32_t count = refcounter_count((refcounter_t*)db_);
    uint32_t max_wait_ms = 5000;  // Max 5 seconds
    uint32_t waited_ms = 0;
    while (count > 1 && waited_ms < max_wait_ms) {  // > 1 because we still hold a reference
      usleep(1000);  // Wait 1ms
      count = refcounter_count((refcounter_t*)db_);
      waited_ms += 1;
    }

    database_t* db = db_;
    db_ = nullptr;  // Clear pointer first to prevent double-destroy
    database_destroy(db);  // This just dereferences, actual destruction happens when all refs are gone
  }
  return info.Env().Undefined();
}

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

  // Convert key to path_t
  path_t* path = PathFromJS(env, info[0], delimiter_);
  if (!path) {
    return env.Null();  // Error already thrown
  }

  // Convert value to identifier_t
  identifier_t* value = ValueFromJS(env, info[1]);
  if (!value) {
    path_destroy(path);
    return env.Null();  // Error already thrown
  }

  // Get optional callback
  Napi::Function callback;
  if (info.Length() > 2 && info[2].IsFunction()) {
    callback = info[2].As<Napi::Function>();
  } else {
    callback = Napi::Function::New(env, [](const Napi::CallbackInfo& info) {
      return info.Env().Undefined();
    });
  }

  // Create and queue worker
  PutWorker* worker = new PutWorker(env, Value(), callback, db_, path, value);
  worker->Queue();

  return worker->Promise();
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

  // Convert key to path_t
  path_t* path = PathFromJS(env, info[0], delimiter_);
  if (!path) {
    return env.Null();  // Error already thrown
  }

  // Get optional callback
  Napi::Function callback;
  if (info.Length() > 1 && info[1].IsFunction()) {
    callback = info[1].As<Napi::Function>();
  } else {
    callback = Napi::Function::New(env, [](const Napi::CallbackInfo& info) {
      return info.Env().Undefined();
    });
  }

  // Create and queue worker
  GetWorker* worker = new GetWorker(env, Value(), callback, db_, path);
  worker->Queue();

  return worker->Promise();
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

  // Convert key to path_t
  path_t* path = PathFromJS(env, info[0], delimiter_);
  if (!path) {
    return env.Null();  // Error already thrown
  }

  // Get optional callback
  Napi::Function callback;
  if (info.Length() > 1 && info[1].IsFunction()) {
    callback = info[1].As<Napi::Function>();
  } else {
    callback = Napi::Function::New(env, [](const Napi::CallbackInfo& info) {
      return info.Env().Undefined();
    });
  }

  // Create and queue worker
  DelWorker* worker = new DelWorker(env, Value(), callback, db_, path);
  worker->Queue();

  return worker->Promise();
}

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

  // Convert key to path_t
  path_t* path = PathFromJS(env, info[0], delimiter_);
  if (!path) {
    return env.Undefined();  // Error already thrown
  }

  // Convert value to identifier_t
  identifier_t* value = ValueFromJS(env, info[1]);
  if (!value) {
    path_destroy(path);
    return env.Undefined();  // Error already thrown
  }

  // Call sync function (consumes path and value)
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

  // Convert key to path_t
  path_t* path = PathFromJS(env, info[0], delimiter_);
  if (!path) {
    return env.Null();  // Error already thrown
  }

  // Call sync function (consumes path)
  identifier_t* result = NULL;
  int rc = database_get_sync(db_, path, &result);

  if (rc == 0 && result != NULL) {
    // Success - convert result to JS value
    Napi::Value value = ValueToJS(env, result);
    identifier_destroy(result);
    return value;
  } else if (rc == -2) {
    // Not found
    return env.Null();
  } else {
    // Error
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

  // Convert key to path_t
  path_t* path = PathFromJS(env, info[0], delimiter_);
  if (!path) {
    return env.Undefined();  // Error already thrown
  }

  // Call sync function (consumes path)
  int rc = database_delete_sync(db_, path);

  if (rc != 0) {
    Napi::Error::New(env, "IO_ERROR: Failed to delete value").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  return env.Undefined();
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

  Napi::Function callback;
  if (info.Length() > 1 && info[1].IsFunction()) {
    callback = info[1].As<Napi::Function>();
  } else {
    callback = Napi::Function::New(env, [](const Napi::CallbackInfo& info) {
      return info.Env().Undefined();
    });
  }

  Napi::Array ops = info[0].As<Napi::Array>();
  std::vector<BatchOp> batchOps;

  for (uint32_t i = 0; i < ops.Length(); i++) {
    Napi::Object op = ops.Get(i).As<Napi::Object>();

    if (!op.Has("type")) {
      Napi::TypeError::New(env, "Operation must have 'type'").ThrowAsJavaScriptException();
      for (auto& bop : batchOps) {
        if (bop.path) path_destroy(bop.path);
        if (bop.value) identifier_destroy(bop.value);
      }
      return env.Null();
    }

    std::string type = op.Get("type").As<Napi::String>().Utf8Value();
    if (!op.Has("key")) {
      Napi::TypeError::New(env, "Operation must have 'key'").ThrowAsJavaScriptException();
      for (auto& bop : batchOps) {
        if (bop.path) path_destroy(bop.path);
        if (bop.value) identifier_destroy(bop.value);
      }
      return env.Null();
    }

    path_t* path = PathFromJS(env, op.Get("key"), delimiter_);
    if (!path) {
      for (auto& bop : batchOps) {
        if (bop.path) path_destroy(bop.path);
        if (bop.value) identifier_destroy(bop.value);
      }
      return env.Null();
    }

    BatchOp batchOp;
    batchOp.path = path;

    if (type == "put") {
      if (!op.Has("value")) {
        Napi::TypeError::New(env, "Put operation must have 'value'").ThrowAsJavaScriptException();
        path_destroy(path);
        for (auto& bop : batchOps) {
          if (bop.path) path_destroy(bop.path);
          if (bop.value) identifier_destroy(bop.value);
        }
        return env.Null();
      }

      identifier_t* value = ValueFromJS(env, op.Get("value"));
      if (!value) {
        path_destroy(path);
        for (auto& bop : batchOps) {
          if (bop.path) path_destroy(bop.path);
          if (bop.value) identifier_destroy(bop.value);
        }
        return env.Null();
      }

      batchOp.type = BatchOpType::PUT;
      batchOp.value = value;
    } else if (type == "del") {
      batchOp.type = BatchOpType::DEL;
      batchOp.value = nullptr;
    } else {
      Napi::TypeError::New(env, "Operation type must be 'put' or 'del'").ThrowAsJavaScriptException();
      path_destroy(path);
      for (auto& bop : batchOps) {
        if (bop.path) path_destroy(bop.path);
        if (bop.value) identifier_destroy(bop.value);
      }
      return env.Null();
    }

    batchOps.push_back(batchOp);
  }

  BatchWorker* worker = new BatchWorker(env, Value(), db_, std::move(batchOps), callback);
  worker->Queue();

  return worker->Promise();
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
    if (!path) {
      return env.Undefined();
    }

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
      // database_put_sync takes ownership of path and value
    } else if (type == "del") {
      rc = database_delete_sync(db_, path);
      // database_delete_sync takes ownership of path
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
      // Nested object - recurse
      FlattenObject(env, value.As<Napi::Object>(), path_parts, ops, delimiter);
    } else if (value.IsArray()) {
      // Array - use numeric indices
      Napi::Array arr = value.As<Napi::Array>();
      for (uint32_t j = 0; j < arr.Length(); j++) {
        path_parts.push_back(std::to_string(j));

        BatchOp op;
        op.type = BatchOpType::PUT;
        op.path = PathFromParts(path_parts);
        op.value = ValueFromJS(env, arr.Get(j));

        if (!op.path || !op.value) {
          // Error already thrown
          if (op.path) path_destroy(op.path);
          if (op.value) identifier_destroy(op.value);
          path_parts.pop_back();
          throw std::runtime_error("Failed to create path or value");
        }

        ops.push_back(op);
        path_parts.pop_back();
      }
    } else {
      // Leaf value
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

  Napi::Function callback;
  if (info.Length() > 1 && info[1].IsFunction()) {
    callback = info[1].As<Napi::Function>();
  } else {
    callback = Napi::Function::New(env, [](const Napi::CallbackInfo& info) {
      return info.Env().Undefined();
    });
  }

  Napi::Object obj = info[0].As<Napi::Object>();
  std::vector<BatchOp> ops;
  std::vector<std::string> path_parts;

  try {
    FlattenObject(env, obj, path_parts, ops, delimiter_);
  } catch (const std::exception& e) {
    // Clean up operations
    for (auto& op : ops) {
      if (op.path) path_destroy(op.path);
      if (op.value) identifier_destroy(op.value);
    }
    Napi::Error::New(env, e.what()).ThrowAsJavaScriptException();
    return env.Null();
  }

  BatchWorker* worker = new BatchWorker(env, Value(), db_, std::move(ops), callback);
  worker->Queue();

  return worker->Promise();
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
  if (!basePath) {
    return env.Null();
  }

  // TODO: Implement database_scan to get all entries under path
  // For now, return empty object
  path_destroy(basePath);

  Napi::Object result = Napi::Object::New(env);

  // Call callback and resolve promise
  if (!callback.IsEmpty()) {
    callback.Call({ env.Null(), result });
  }

  Napi::Promise::Deferred deferred = Napi::Promise::Deferred::New(env);
  deferred.Resolve(result);
  return deferred.Promise();
}

Napi::Value WaveDB::CreateReadStream(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  Napi::Object options = Napi::Object::New(env);
  if (info.Length() > 0 && info[0].IsObject()) {
    options = info[0].As<Napi::Object>();
  }

  // Create iterator instance
  // TODO: Pass options and database handle
  Napi::Object iterObj = Iterator::constructor_.New({ options });

  return iterObj;
}