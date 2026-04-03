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
    InstanceMethod("getObjectSync", &WaveDB::GetObjectSync),
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

    // Flush all thread-local WALs to ensure persistence
    // WAL recovery provides data persistence across database restarts
    if (db_->wal_manager != NULL) {
      wal_manager_flush(db_->wal_manager);
    }

    // Note: Do NOT call database_snapshot() here!
    // Snapshot saves the index file with old transaction IDs that conflict with WAL replay.
    // Data persistence is provided by WAL recovery on next database open.

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

  // Create start path (same as base path for inclusive start)
  // We need to scan all entries that start with this path
  path_t* startPath = nullptr;
  path_t* endPath = nullptr;

  // For scanning under a path, we use the path as start
  // and create a path that's one past the last possible key
  startPath = path_create();
  if (!startPath) {
    path_destroy(basePath);
    Napi::Error::New(env, "Failed to create start path").ThrowAsJavaScriptException();
    return env.Null();
  }

  // Copy base path to start path
  for (size_t i = 0; i < static_cast<size_t>(basePath->identifiers.length); i++) {
    identifier_t* id = basePath->identifiers.data[i];
    REFERENCE(id, identifier_t);
    path_append(startPath, id);
  }

  // Store base path parts for filtering
  std::vector<std::string> basePathParts;
  for (size_t i = 0; i < static_cast<size_t>(basePath->identifiers.length); i++) {
    identifier_t* id = basePath->identifiers.data[i];
    std::string part;
    for (size_t j = 0; j < static_cast<size_t>(id->chunks.length); j++) {
      chunk_t* chunk = id->chunks.data[j];
      const uint8_t* data = static_cast<const uint8_t*>(chunk_data_const(chunk));
      size_t size = chunk->data->size;
      part += std::string(reinterpret_cast<const char*>(data), size);
    }
    // Strip trailing null characters and whitespace (padding from chunk reconstruction)
    while (!part.empty() && (part.back() == '\0' || part.back() == ' ')) {
      part.pop_back();
    }
    basePathParts.push_back(part);
  }

  // Start the scan
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

  // Collect all entries that match base path
  std::vector<std::pair<std::vector<std::string>, Napi::Value>> entries;

  while (true) {
    path_t* outPath = nullptr;
    identifier_t* outValue = nullptr;

    int rc = database_scan_next(iter, &outPath, &outValue);
    if (rc != 0) break;

    // Convert path to vector of strings
    std::vector<std::string> pathParts;
    for (size_t i = 0; i < static_cast<size_t>(outPath->identifiers.length); i++) {
      identifier_t* id = outPath->identifiers.data[i];
      std::string part;
      for (size_t j = 0; j < static_cast<size_t>(id->chunks.length); j++) {
        chunk_t* chunk = id->chunks.data[j];
        const uint8_t* data = static_cast<const uint8_t*>(chunk_data_const(chunk));
        size_t size = chunk->data->size;
        part += std::string(reinterpret_cast<const char*>(data), size);
      }
      // Strip trailing null characters and whitespace (padding from chunk reconstruction)
      while (!part.empty() && (part.back() == '\0' || part.back() == ' ')) {
        part.pop_back();
      }
      pathParts.push_back(part);
    }

    // Convert value to JS value
    Napi::Value jsValue = ValueToJS(env, outValue);

    // Filter: only include entries under base path
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

  // Reconstruct object from entries
  Napi::Object result = ReconstructObject(env, entries);

  // Call callback and resolve promise
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
  if (!basePath) {
    return env.Null();
  }

  // Store base path parts for filtering
  std::vector<std::string> basePathParts;
  for (size_t i = 0; i < static_cast<size_t>(basePath->identifiers.length); i++) {
    identifier_t* id = basePath->identifiers.data[i];
    std::string part;
    for (size_t j = 0; j < static_cast<size_t>(id->chunks.length); j++) {
      chunk_t* chunk = id->chunks.data[j];
      const uint8_t* data = static_cast<const uint8_t*>(chunk_data_const(chunk));
      size_t size = chunk->data->size;
      part += std::string(reinterpret_cast<const char*>(data), size);
    }
    // Strip trailing null characters and whitespace (padding from chunk reconstruction)
    while (!part.empty() && (part.back() == '\0' || part.back() == ' ')) {
      part.pop_back();
    }
    basePathParts.push_back(part);
  }

  // Create start path (same as base path for inclusive start)
  path_t* startPath = path_create();
  if (!startPath) {
    path_destroy(basePath);
    Napi::Error::New(env, "Failed to create start path").ThrowAsJavaScriptException();
    return env.Null();
  }

  // Copy base path to start path
  for (size_t i = 0; i < static_cast<size_t>(basePath->identifiers.length); i++) {
    identifier_t* id = basePath->identifiers.data[i];
    REFERENCE(id, identifier_t);
    path_append(startPath, id);
  }

  path_destroy(startPath);
  path_destroy(basePath);

  // Start the scan
  database_iterator_t* iter = database_scan_start(db_, startPath, nullptr);

  if (!iter) {
    return Napi::Object::New(env);
  }

  // Collect all entries that match base path
  std::vector<std::pair<std::vector<std::string>, Napi::Value>> entries;

  while (true) {
    path_t* outPath = nullptr;
    identifier_t* outValue = nullptr;

    int rc = database_scan_next(iter, &outPath, &outValue);
    if (rc != 0) break;

    // Convert path to vector of strings
    std::vector<std::string> pathParts;
    for (size_t i = 0; i < static_cast<size_t>(outPath->identifiers.length); i++) {
      identifier_t* id = outPath->identifiers.data[i];
      std::string part;
      for (size_t j = 0; j < static_cast<size_t>(id->chunks.length); j++) {
        chunk_t* chunk = id->chunks.data[j];
        const uint8_t* data = static_cast<const uint8_t*>(chunk_data_const(chunk));
        size_t size = chunk->data->size;
        part += std::string(reinterpret_cast<const char*>(data), size);
      }
      // Strip trailing null characters and whitespace (padding from chunk reconstruction)
      while (!part.empty() && (part.back() == '\0' || part.back() == ' ')) {
        part.pop_back();
      }
      pathParts.push_back(part);
    }

    // Convert value to JS value
    Napi::Value jsValue = ValueToJS(env, outValue);

    // Filter: only include entries under base path
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

  // Reconstruct object from entries
  return ReconstructObject(env, entries);
}

Napi::Object WaveDB::ReconstructObject(Napi::Env env,
                                       const std::vector<std::pair<std::vector<std::string>, Napi::Value>>& entries) {
  Napi::Object result = Napi::Object::New(env);

  for (const auto& entry : entries) {
    const std::vector<std::string>& pathParts = entry.first;
    Napi::Value value = entry.second;

    if (pathParts.empty()) continue;

    // Navigate/create nested structure
    Napi::Object current = result;
    for (size_t i = 0; i < pathParts.size() - 1; i++) {
      const std::string& key = pathParts[i];

      if (!current.Has(key)) {
        current.Set(key, Napi::Object::New(env));
      }
      current = current.Get(key).As<Napi::Object>();
    }

    // Set final value
    current.Set(pathParts.back(), value);
  }

  // Convert arrays with contiguous numeric keys
  return ConvertArrays(env, result).As<Napi::Object>();
}

Napi::Value WaveDB::ConvertArrays(Napi::Env env, Napi::Value value) {
  if (!value.IsObject()) {
    return value;
  }

  Napi::Object obj = value.As<Napi::Object>();
  Napi::Array keys = obj.GetPropertyNames();

  // Check if all keys are contiguous numeric
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
    // Sort indices
    std::sort(indices.begin(), indices.end());

    // Check if contiguous from 0
    bool contiguous = true;
    for (size_t i = 0; i < indices.size(); i++) {
      if (indices[i] != i) {
        contiguous = false;
        break;
      }
    }

    if (contiguous) {
      // Convert to array
      Napi::Array arr = Napi::Array::New(env);
      for (uint32_t i = 0; i < indices.size(); i++) {
        Napi::Value val = obj.Get(Napi::String::New(env, std::to_string(i)));
        arr.Set(i, ConvertArrays(env, val));
      }
      return arr;
    }
  }

  // Recursively convert nested objects
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

  // Pass database pointer as External and options
  Napi::External<database_t> dbExternal = Napi::External<database_t>::New(env, db_);
  Napi::Object iterObj = Iterator::constructor_.New({ dbExternal, options });

  return iterObj;
}