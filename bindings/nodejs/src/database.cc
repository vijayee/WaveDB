#include <napi.h>
#include <string>
#include "../../../src/Database/database.h"
#include "path.h"
#include "identifier.h"
#include "async_worker.h"
#include "put_worker.h"
#include "get_worker.h"
#include "del_worker.h"
#include "batch_worker.h"

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

  // Object operations
  Napi::Value PutObject(const Napi::CallbackInfo& info);
  Napi::Value GetObject(const Napi::CallbackInfo& info);

  // Streaming
  Napi::Value CreateReadStream(const Napi::CallbackInfo& info);

  // Lifecycle
  Napi::Value Close(const Napi::CallbackInfo& info);

  static Napi::FunctionReference constructor_;
};

Napi::FunctionReference WaveDB::constructor_;

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
    database_destroy(db_);
    db_ = nullptr;
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
  PutWorker* worker = new PutWorker(env, callback, db_, path, value);
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
  GetWorker* worker = new GetWorker(env, callback, db_, path);
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
  DelWorker* worker = new DelWorker(env, callback, db_, path);
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

  BatchWorker* worker = new BatchWorker(env, db_, std::move(batchOps), callback);
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