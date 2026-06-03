// C++ headers that include <atomic> must come before C headers
// that use ATOMIC_TYPE() macros expanding to std::atomic<T> in C++.
#include <atomic>

#include <napi.h>
#include <cstdlib>
#include <string>
#include "Layers/graph/graph.h"
#include "Database/database_subtree.h"
#include "async_bridge.h"
#include "graph_result_js.h"

class GraphLayer : public Napi::ObjectWrap<GraphLayer> {
public:
  static Napi::Object Init(Napi::Env env, Napi::Object exports);

  GraphLayer(const Napi::CallbackInfo& info);
  ~GraphLayer();

private:
  static Napi::FunctionReference constructor_;
  graph_layer_t* layer_;
  AsyncBridge bridge_;
  bool syncOnly_;

  // Async operations
  Napi::Value Insert(const Napi::CallbackInfo& info);
  Napi::Value Delete(const Napi::CallbackInfo& info);
  Napi::Value Query(const Napi::CallbackInfo& info);

  // Sync operations
  Napi::Value Exec(const Napi::CallbackInfo& info);
  Napi::Value Count(const Napi::CallbackInfo& info);
  Napi::Value InsertSync(const Napi::CallbackInfo& info);
  Napi::Value DeleteSync(const Napi::CallbackInfo& info);
  Napi::Value ParseSchema(const Napi::CallbackInfo& info);
  Napi::Value DefineMorphism(const Napi::CallbackInfo& info);
  Napi::Value Close(const Napi::CallbackInfo& info);

  // Helper: create an AsyncOpContext with a JS Promise and optional callback
  AsyncOpContext* CreateOpContext(Napi::Env env, AsyncOpType type,
                                   const Napi::CallbackInfo& info, int callbackArgIndex);
};

Napi::FunctionReference GraphLayer::constructor_;

Napi::Object GraphLayer::Init(Napi::Env env, Napi::Object exports) {
  Napi::HandleScope scope(env);

  Napi::Function func = DefineClass(env, "GraphLayer", {
    InstanceMethod("insert", &GraphLayer::Insert),
    InstanceMethod("del", &GraphLayer::Delete),
    InstanceMethod("query", &GraphLayer::Query),
    InstanceMethod("exec", &GraphLayer::Exec),
    InstanceMethod("count", &GraphLayer::Count),
    InstanceMethod("insertSync", &GraphLayer::InsertSync),
    InstanceMethod("deleteSync", &GraphLayer::DeleteSync),
    InstanceMethod("parseSchema", &GraphLayer::ParseSchema),
    InstanceMethod("defineMorphism", &GraphLayer::DefineMorphism),
    InstanceMethod("close", &GraphLayer::Close),
  });

  constructor_ = Napi::Persistent(func);
  exports.Set("GraphLayer", func);

  return exports;
}

GraphLayer::GraphLayer(const Napi::CallbackInfo& info)
  : Napi::ObjectWrap<GraphLayer>(info),
    layer_(nullptr),
    syncOnly_(false) {

  Napi::Env env = info.Env();

  const char* db_path = nullptr;
  std::string path_str;
  if (info.Length() > 0 && info[0].IsString()) {
    path_str = info[0].As<Napi::String>().Utf8Value();
    db_path = path_str.c_str();
  }

  // Build a config. The graph layer C API accepts database_config_t or NULL.
  database_config_t* defaults = database_config_default();
  if (!defaults) {
    Napi::Error::New(env, "Failed to create default config").ThrowAsJavaScriptException();
    return;
  }
  database_config_t config = *defaults;
  config.lru_memory_mb = 50;

  // In-memory databases (no path) must use sync_only mode since there's
  // no WAL path for persistence. Persistent databases default to async mode.
  if (!db_path) {
    config.sync_only = 1;
    config.worker_threads = 0;
    syncOnly_ = true;
  }

  // Override config from options object if provided
  if (info.Length() > 1 && info[1].IsObject()) {
    Napi::Object options = info[1].As<Napi::Object>();

    if (options.Has("lruMemoryMb")) {
      config.lru_memory_mb = static_cast<size_t>(options.Get("lruMemoryMb").As<Napi::Number>().Uint32Value());
    }

    if (options.Has("workerThreads")) {
      config.worker_threads = static_cast<uint8_t>(options.Get("workerThreads").As<Napi::Number>().Uint32Value());
    }

    if (options.Has("syncOnly")) {
      syncOnly_ = options.Get("syncOnly").As<Napi::Boolean>().Value();
      if (syncOnly_) {
        config.sync_only = 1;
        config.worker_threads = 0;
      } else {
        config.sync_only = 0;
      }
    }

    if (options.Has("enablePersist")) {
      config.enable_persist = options.Get("enablePersist").As<Napi::Boolean>().Value() ? 1 : 0;
    }
  }

  // Extract subtree pointer if provided (cross-addon via number)
  database_subtree_t* subtree_ptr = nullptr;
  if (info.Length() > 1 && info[1].IsObject()) {
    Napi::Object options = info[1].As<Napi::Object>();
    if (options.Has("_subtreePtr")) {
      double ptrVal = options.Get("_subtreePtr").As<Napi::Number>().DoubleValue();
      subtree_ptr = reinterpret_cast<database_subtree_t*>(static_cast<uintptr_t>(ptrVal));
    }
  }

  free(defaults);

  // Wrap the database_config_t in a graph_layer_config_t for the new API
  graph_layer_config_t layer_config;
  layer_config.path = db_path;
  layer_config.db_config = &config;

  if (db_path) {
    int error_code = 0;
    layer_ = graph_layer_create(db_path, &layer_config, subtree_ptr, &error_code);
    if (!layer_) {
      std::string msg;
      switch (error_code) {
        case -3: msg = "Failed to create GraphLayer: database already contains schema from a different layer type. Use a subtree to isolate this layer."; break;
        case -2: msg = "Failed to create GraphLayer: database open failed"; break;
        default: msg = "Failed to create GraphLayer"; break;
      }
      Napi::Error::New(env, msg).ThrowAsJavaScriptException();
      return;
    }
  } else {
    // In-memory: pass NULL path, use config for settings
    int error_code = 0;
    layer_ = graph_layer_create(NULL, &layer_config, subtree_ptr, &error_code);
    if (!layer_) {
      Napi::Error::New(env, "Failed to create GraphLayer").ThrowAsJavaScriptException();
      return;
    }
  }

  // Initialize async bridge (skip in sync_only mode)
  if (!syncOnly_) {
    bridge_.Init(env);
  }
}

GraphLayer::~GraphLayer() {
  if (layer_) {
    if (!syncOnly_) {
      bridge_.Shutdown();
    }
    graph_layer_t* layer = layer_;
    layer_ = nullptr;
    graph_layer_destroy(layer);
  }
}

// --- Helper ---

AsyncOpContext* GraphLayer::CreateOpContext(Napi::Env env, AsyncOpType type,
                                             const Napi::CallbackInfo& info, int callbackArgIndex) {
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

// --- Async operations ---

Napi::Value GraphLayer::Insert(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (!layer_) {
    Napi::Error::New(env, "GRAPH_CLOSED: GraphLayer is closed").ThrowAsJavaScriptException();
    return env.Null();
  }

  if (info.Length() < 3) {
    Napi::TypeError::New(env, "s, p, o strings required").ThrowAsJavaScriptException();
    return env.Null();
  }

  std::string s = info[0].As<Napi::String>().Utf8Value();
  std::string p = info[1].As<Napi::String>().Utf8Value();
  std::string o = info[2].As<Napi::String>().Utf8Value();

  // Fallback to sync when bridge is not available (sync_only mode)
  if (syncOnly_ || !bridge_.IsInitialized()) {
    int rc = graph_insert_sync(layer_, s.c_str(), p.c_str(), o.c_str());
    Napi::Promise::Deferred deferred = Napi::Promise::Deferred::New(env);
    if (rc != 0) {
      deferred.Reject(Napi::Error::New(env, "Insert failed").Value());
    } else {
      deferred.Resolve(env.Undefined());
    }
    return deferred.Promise();
  }

  AsyncOpContext* ctx = CreateOpContext(env, AsyncOpType::GraphInsert, info, 3);

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

  graph_insert(layer_, s.c_str(), p.c_str(), o.c_str(), promise_c);

  return Napi::Value(env, ctx->promise);
}

Napi::Value GraphLayer::Delete(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (!layer_) {
    Napi::Error::New(env, "GRAPH_CLOSED: GraphLayer is closed").ThrowAsJavaScriptException();
    return env.Null();
  }

  if (info.Length() < 3) {
    Napi::TypeError::New(env, "s, p, o strings required").ThrowAsJavaScriptException();
    return env.Null();
  }

  std::string s = info[0].As<Napi::String>().Utf8Value();
  std::string p = info[1].As<Napi::String>().Utf8Value();
  std::string o = info[2].As<Napi::String>().Utf8Value();

  // Fallback to sync when bridge is not available (sync_only mode)
  if (syncOnly_ || !bridge_.IsInitialized()) {
    int rc = graph_delete_sync(layer_, s.c_str(), p.c_str(), o.c_str());
    Napi::Promise::Deferred deferred = Napi::Promise::Deferred::New(env);
    if (rc != 0) {
      deferred.Reject(Napi::Error::New(env, "Delete failed").Value());
    } else {
      deferred.Resolve(env.Undefined());
    }
    return deferred.Promise();
  }

  AsyncOpContext* ctx = CreateOpContext(env, AsyncOpType::GraphDelete, info, 3);

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

  graph_delete(layer_, s.c_str(), p.c_str(), o.c_str(), promise_c);

  return Napi::Value(env, ctx->promise);
}

Napi::Value GraphLayer::Query(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (!layer_) {
    Napi::Error::New(env, "GRAPH_CLOSED: GraphLayer is closed").ThrowAsJavaScriptException();
    return env.Null();
  }

  if (info.Length() < 1 || !info[0].IsString()) {
    Napi::TypeError::New(env, "DSL string required").ThrowAsJavaScriptException();
    return env.Null();
  }

  std::string dsl = info[0].As<Napi::String>().Utf8Value();

  // Fallback to sync when bridge is not available (sync_only mode)
  if (syncOnly_ || !bridge_.IsInitialized()) {
    graph_parse_error_t err;
    graph_result_t* result = graph_parse_execute(dsl.c_str(), layer_, &err);
    Napi::Promise::Deferred deferred = Napi::Promise::Deferred::New(env);
    if (!result) {
      deferred.Reject(Napi::Error::New(env, err.message).Value());
    } else {
      Napi::Array arr = GraphResultToJS(env, result);
      graph_result_destroy(result);
      deferred.Resolve(arr);
    }
    return deferred.Promise();
  }

  // Parse DSL synchronously to check for errors early
  graph_parse_error_t err;
  graph_query_t* q = graph_parse(dsl.c_str(), layer_, &err);
  if (!q) {
    std::string msg = err.ok ? "Parse failed" : err.message;
    // Reject the promise instead of throwing — callers use await and expect rejection
    Napi::Promise::Deferred deferred = Napi::Promise::Deferred::New(env);
    deferred.Reject(Napi::Error::New(env, msg).Value());
    return deferred.Promise();
  }

  AsyncOpContext* ctx = CreateOpContext(env, AsyncOpType::GraphQuery, info, 1);

  promise_t* promise_c = bridge_.CreatePromise(ctx);
  if (!promise_c) {
    graph_query_destroy(q);
    napi_value error_val = Napi::Error::New(env, "Failed to create async promise").Value();
    napi_value promise_val = ctx->promise;
    napi_reject_deferred(env, ctx->deferred, error_val);
    if (ctx->callback_ref) napi_delete_reference(env, ctx->callback_ref);
    delete ctx;
    return Napi::Value(env, promise_val);
  }

  ctx->promise_c = promise_c;

  // graph_query_execute takes ownership of q and destroys it in the worker
  graph_query_execute(q, promise_c);

  return Napi::Value(env, ctx->promise);
}

// --- Sync operations ---

Napi::Value GraphLayer::Exec(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (!layer_) {
    Napi::Error::New(env, "GRAPH_CLOSED: GraphLayer is closed").ThrowAsJavaScriptException();
    return env.Null();
  }

  if (info.Length() < 1 || !info[0].IsString()) {
    Napi::TypeError::New(env, "DSL string required").ThrowAsJavaScriptException();
    return env.Null();
  }

  std::string dsl = info[0].As<Napi::String>().Utf8Value();

  graph_parse_error_t err;
  graph_result_t* result = graph_parse_execute(dsl.c_str(), layer_, &err);

  if (!result) {
    Napi::Error::New(env, err.message).ThrowAsJavaScriptException();
    return env.Null();
  }

  Napi::Array arr = GraphResultToJS(env, result);
  graph_result_destroy(result);
  return arr;
}

Napi::Value GraphLayer::Count(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (!layer_) {
    Napi::Error::New(env, "GRAPH_CLOSED: GraphLayer is closed").ThrowAsJavaScriptException();
    return env.Null();
  }

  if (info.Length() < 1 || !info[0].IsString()) {
    Napi::TypeError::New(env, "DSL string required").ThrowAsJavaScriptException();
    return env.Null();
  }

  std::string dsl = info[0].As<Napi::String>().Utf8Value();

  graph_parse_error_t err;
  size_t count = 0;
  int rc = graph_parse_count(dsl.c_str(), layer_, &count, &err);

  if (rc != 0) {
    Napi::Error::New(env, err.message).ThrowAsJavaScriptException();
    return env.Null();
  }

  return Napi::Number::New(env, count);
}

Napi::Value GraphLayer::InsertSync(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (!layer_) {
    Napi::Error::New(env, "GRAPH_CLOSED: GraphLayer is closed").ThrowAsJavaScriptException();
    return env.Null();
  }

  if (info.Length() < 3) {
    Napi::TypeError::New(env, "s, p, o strings required").ThrowAsJavaScriptException();
    return env.Null();
  }

  std::string s = info[0].As<Napi::String>().Utf8Value();
  std::string p = info[1].As<Napi::String>().Utf8Value();
  std::string o = info[2].As<Napi::String>().Utf8Value();

  int rc = graph_insert_sync(layer_, s.c_str(), p.c_str(), o.c_str());
  if (rc != 0) {
    Napi::Error::New(env, "Insert failed").ThrowAsJavaScriptException();
  }

  return env.Undefined();
}

Napi::Value GraphLayer::DeleteSync(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (!layer_) {
    Napi::Error::New(env, "GRAPH_CLOSED: GraphLayer is closed").ThrowAsJavaScriptException();
    return env.Null();
  }

  if (info.Length() < 3) {
    Napi::TypeError::New(env, "s, p, o strings required").ThrowAsJavaScriptException();
    return env.Null();
  }

  std::string s = info[0].As<Napi::String>().Utf8Value();
  std::string p = info[1].As<Napi::String>().Utf8Value();
  std::string o = info[2].As<Napi::String>().Utf8Value();

  int rc = graph_delete_sync(layer_, s.c_str(), p.c_str(), o.c_str());
  if (rc != 0) {
    Napi::Error::New(env, "Delete failed").ThrowAsJavaScriptException();
  }

  return env.Undefined();
}

Napi::Value GraphLayer::ParseSchema(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (!layer_) {
    Napi::Error::New(env, "GRAPH_CLOSED: GraphLayer is closed").ThrowAsJavaScriptException();
    return env.Null();
  }

  if (info.Length() < 1 || !info[0].IsString()) {
    Napi::TypeError::New(env, "Schema DSL string required").ThrowAsJavaScriptException();
    return env.Null();
  }

  std::string sdl = info[0].As<Napi::String>().Utf8Value();

  char* error_out = nullptr;
  int rc = graph_schema_parse(layer_, sdl.c_str(), &error_out);

  if (rc != 0) {
    std::string msg = error_out ? error_out : "Schema parse failed";
    if (error_out) free(error_out);
    Napi::Error::New(env, msg).ThrowAsJavaScriptException();
    return env.Null();
  }

  return env.Undefined();
}

Napi::Value GraphLayer::DefineMorphism(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (!layer_) {
    Napi::Error::New(env, "GRAPH_CLOSED: GraphLayer is closed").ThrowAsJavaScriptException();
    return env.Null();
  }

  if (info.Length() < 2 || !info[0].IsString() || !info[1].IsString()) {
    Napi::TypeError::New(env, "name and DSL string required").ThrowAsJavaScriptException();
    return env.Null();
  }

  std::string name = info[0].As<Napi::String>().Utf8Value();
  std::string dsl = info[1].As<Napi::String>().Utf8Value();

  graph_parse_error_t err;
  int rc = graph_morphism_define(layer_, name.c_str(), dsl.c_str(), &err);

  if (rc != 0) {
    Napi::Error::New(env, err.ok ? "Morphism define failed" : err.message).ThrowAsJavaScriptException();
    return env.Null();
  }

  return env.Undefined();
}

Napi::Value GraphLayer::Close(const Napi::CallbackInfo& info) {
  if (layer_) {
    if (!syncOnly_) {
      bridge_.Shutdown();
    }
    graph_layer_t* layer = layer_;
    layer_ = nullptr;
    graph_layer_destroy(layer);
  }
  return info.Env().Undefined();
}

Napi::Object InitGraph(Napi::Env env, Napi::Object exports) {
  return GraphLayer::Init(env, exports);
}

NODE_API_MODULE(wavedb_graph, InitGraph)