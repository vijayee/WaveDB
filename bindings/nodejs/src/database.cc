#include <napi.h>
#include <string>
#include "../../../src/Database/database.h"
#include "path.h"
#include "identifier.h"
#include "async_worker.h"
#include "put_worker.h"

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