// C++ headers that include <atomic> must come before C headers
// that use ATOMIC_TYPE() macros expanding to std::atomic<T> in C++.
#include <atomic>

#include <napi.h>
#include <cstdlib>
#include <string>
#include "../../../src/Layers/graph/graph.h"
#include "graph_result_js.h"

class GraphLayer : public Napi::ObjectWrap<GraphLayer> {
public:
  static Napi::Object Init(Napi::Env env, Napi::Object exports);

  GraphLayer(const Napi::CallbackInfo& info);
  ~GraphLayer();

private:
  static Napi::FunctionReference constructor_;
  graph_layer_t* layer_;

  Napi::Value Exec(const Napi::CallbackInfo& info);
  Napi::Value Count(const Napi::CallbackInfo& info);
  Napi::Value InsertSync(const Napi::CallbackInfo& info);
  Napi::Value DeleteSync(const Napi::CallbackInfo& info);
  Napi::Value Close(const Napi::CallbackInfo& info);
};

Napi::FunctionReference GraphLayer::constructor_;

Napi::Object GraphLayer::Init(Napi::Env env, Napi::Object exports) {
  Napi::HandleScope scope(env);

  Napi::Function func = DefineClass(env, "GraphLayer", {
    InstanceMethod("exec", &GraphLayer::Exec),
    InstanceMethod("count", &GraphLayer::Count),
    InstanceMethod("insertSync", &GraphLayer::InsertSync),
    InstanceMethod("deleteSync", &GraphLayer::DeleteSync),
    InstanceMethod("close", &GraphLayer::Close),
  });

  constructor_ = Napi::Persistent(func);
  exports.Set("GraphLayer", func);

  return exports;
}

GraphLayer::GraphLayer(const Napi::CallbackInfo& info)
  : Napi::ObjectWrap<GraphLayer>(info),
    layer_(nullptr) {

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
  config.sync_only = 1;  // Graph layer uses sync ops internally
  free(defaults);

  if (db_path) {
    layer_ = graph_layer_create(db_path, &config);
  } else {
    // In-memory: pass NULL path, use config for settings
    layer_ = graph_layer_create(NULL, &config);
  }

  if (!layer_) {
    Napi::Error::New(env, "Failed to create GraphLayer").ThrowAsJavaScriptException();
    return;
  }
}

GraphLayer::~GraphLayer() {
  if (layer_) {
    graph_layer_destroy(layer_);
    layer_ = nullptr;
  }
}

Napi::Value GraphLayer::Exec(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (!layer_) {
    Napi::Error::New(env, "GraphLayer is closed").ThrowAsJavaScriptException();
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
    Napi::Error::New(env, "GraphLayer is closed").ThrowAsJavaScriptException();
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
    Napi::Error::New(env, "GraphLayer is closed").ThrowAsJavaScriptException();
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
    Napi::Error::New(env, "GraphLayer is closed").ThrowAsJavaScriptException();
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

Napi::Value GraphLayer::Close(const Napi::CallbackInfo& info) {
  if (layer_) {
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
