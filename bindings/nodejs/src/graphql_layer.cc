// C++ headers that include <atomic> must come before C headers
// that use ATOMIC_TYPE() macros expanding to std::atomic<T> in C++.
#include <atomic>

#include <napi.h>
#include <string>
#include "../../../src/Layers/graphql/graphql.h"
#include "async_bridge.h"
#include "graphql_result_js.h"

class GraphQLLayer : public Napi::ObjectWrap<GraphQLLayer> {
public:
  static Napi::Object Init(Napi::Env env, Napi::Object exports);

  GraphQLLayer(const Napi::CallbackInfo& info);
  ~GraphQLLayer();

private:
  static Napi::FunctionReference constructor_;
  graphql_layer_t* layer_;
  AsyncBridge bridge_;

  // Schema
  Napi::Value ParseSchema(const Napi::CallbackInfo& info);

  // Sync queries
  Napi::Value QuerySync(const Napi::CallbackInfo& info);
  Napi::Value MutateSync(const Napi::CallbackInfo& info);

  // Async queries
  Napi::Value Query(const Napi::CallbackInfo& info);
  Napi::Value Mutate(const Napi::CallbackInfo& info);

  // Lifecycle
  Napi::Value Close(const Napi::CallbackInfo& info);

  // Helper: create an AsyncOpContext with a JS Promise and optional callback
  AsyncOpContext* CreateOpContext(Napi::Env env, AsyncOpType type,
                                   const Napi::CallbackInfo& info, int callbackArgIndex);
};

Napi::FunctionReference GraphQLLayer::constructor_;

Napi::Object GraphQLLayer::Init(Napi::Env env, Napi::Object exports) {
  Napi::HandleScope scope(env);

  Napi::Function func = DefineClass(env, "GraphQLLayer", {
    InstanceMethod("parseSchema", &GraphQLLayer::ParseSchema),
    InstanceMethod("querySync", &GraphQLLayer::QuerySync),
    InstanceMethod("mutateSync", &GraphQLLayer::MutateSync),
    InstanceMethod("query", &GraphQLLayer::Query),
    InstanceMethod("mutate", &GraphQLLayer::Mutate),
    InstanceMethod("close", &GraphQLLayer::Close),
  });

  constructor_ = Napi::Persistent(func);
  exports.Set("GraphQLLayer", func);

  return exports;
}

GraphQLLayer::GraphQLLayer(const Napi::CallbackInfo& info)
  : Napi::ObjectWrap<GraphQLLayer>(info),
    layer_(nullptr) {

  Napi::Env env = info.Env();

  graphql_layer_config_t* config = graphql_layer_config_default();
  if (!config) {
    Napi::Error::New(env, "Failed to create default configuration").ThrowAsJavaScriptException();
    return;
  }

  if (info.Length() > 0 && info[0].IsString()) {
    std::string pathStr = info[0].As<Napi::String>().Utf8Value();
    config->path = strdup(pathStr.c_str());
  }

  if (info.Length() > 1 && info[1].IsObject()) {
    Napi::Object options = info[1].As<Napi::Object>();

    if (options.Has("enablePersist")) {
      Napi::Boolean val = options.Get("enablePersist").As<Napi::Boolean>();
      config->enable_persist = val.Value() ? 1 : 0;
    }

    if (options.Has("chunkSize")) {
      Napi::Number val = options.Get("chunkSize").As<Napi::Number>();
      config->chunk_size = static_cast<uint8_t>(val.Uint32Value());
    }

    if (options.Has("workerThreads")) {
      Napi::Number val = options.Get("workerThreads").As<Napi::Number>();
      config->worker_threads = static_cast<uint8_t>(val.Uint32Value());
    }
  }

  layer_ = graphql_layer_create(config->path, config);
  if (!layer_) {
    graphql_layer_config_destroy(config);
    Napi::Error::New(env, "Failed to create GraphQL layer").ThrowAsJavaScriptException();
    return;
  }

  graphql_layer_config_destroy(config);
  bridge_.Init(env);
}

GraphQLLayer::~GraphQLLayer() {
  if (layer_) {
    graphql_layer_destroy(layer_);
    layer_ = nullptr;
  }
}

// --- Helper ---

AsyncOpContext* GraphQLLayer::CreateOpContext(Napi::Env env, AsyncOpType type,
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

// --- Schema ---

Napi::Value GraphQLLayer::ParseSchema(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (!layer_) {
    Napi::Error::New(env, "LAYER_CLOSED: GraphQL layer is closed").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  if (info.Length() < 1 || !info[0].IsString()) {
    Napi::TypeError::New(env, "SDL string required").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  std::string sdl = info[0].As<Napi::String>().Utf8Value();
  char* error_msg = nullptr;
  int rc = graphql_schema_parse(layer_, sdl.c_str(), &error_msg);

  if (rc != 0) {
    std::string msg = error_msg ? error_msg : "Failed to parse schema";
    free(error_msg);
    Napi::Error::New(env, msg).ThrowAsJavaScriptException();
    return env.Undefined();
  }

  return env.Undefined();
}

// --- Sync operations ---

Napi::Value GraphQLLayer::QuerySync(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (!layer_) {
    Napi::Error::New(env, "LAYER_CLOSED: GraphQL layer is closed").ThrowAsJavaScriptException();
    return env.Null();
  }

  if (info.Length() < 1 || !info[0].IsString()) {
    Napi::TypeError::New(env, "Query string required").ThrowAsJavaScriptException();
    return env.Null();
  }

  std::string query = info[0].As<Napi::String>().Utf8Value();
  graphql_result_t* result = graphql_query_sync(layer_, query.c_str());

  if (!result) {
    Napi::Error::New(env, "Query execution failed").ThrowAsJavaScriptException();
    return env.Null();
  }

  Napi::Value jsResult = GraphQLResultToJS(env, result);
  graphql_result_destroy(result);

  return jsResult;
}

Napi::Value GraphQLLayer::MutateSync(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (!layer_) {
    Napi::Error::New(env, "LAYER_CLOSED: GraphQL layer is closed").ThrowAsJavaScriptException();
    return env.Null();
  }

  if (info.Length() < 1 || !info[0].IsString()) {
    Napi::TypeError::New(env, "Mutation string required").ThrowAsJavaScriptException();
    return env.Null();
  }

  std::string mutation = info[0].As<Napi::String>().Utf8Value();
  graphql_result_t* result = graphql_mutate_sync(layer_, mutation.c_str());

  if (!result) {
    Napi::Error::New(env, "Mutation execution failed").ThrowAsJavaScriptException();
    return env.Null();
  }

  Napi::Value jsResult = GraphQLResultToJS(env, result);
  graphql_result_destroy(result);

  return jsResult;
}

// --- Async operations using C async API + AsyncBridge ---

Napi::Value GraphQLLayer::Query(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (!layer_) {
    Napi::Error::New(env, "LAYER_CLOSED: GraphQL layer is closed").ThrowAsJavaScriptException();
    return env.Null();
  }

  if (info.Length() < 1 || !info[0].IsString()) {
    Napi::TypeError::New(env, "Query string required").ThrowAsJavaScriptException();
    return env.Null();
  }

  std::string query = info[0].As<Napi::String>().Utf8Value();

  AsyncOpContext* ctx = CreateOpContext(env, AsyncOpType::Query, info, 1);

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

  graphql_query(layer_, query.c_str(), promise_c, nullptr);

  return Napi::Value(env, ctx->promise);
}

Napi::Value GraphQLLayer::Mutate(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (!layer_) {
    Napi::Error::New(env, "LAYER_CLOSED: GraphQL layer is closed").ThrowAsJavaScriptException();
    return env.Null();
  }

  if (info.Length() < 1 || !info[0].IsString()) {
    Napi::TypeError::New(env, "Mutation string required").ThrowAsJavaScriptException();
    return env.Null();
  }

  std::string mutation = info[0].As<Napi::String>().Utf8Value();

  AsyncOpContext* ctx = CreateOpContext(env, AsyncOpType::Mutate, info, 1);

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

  graphql_mutate(layer_, mutation.c_str(), promise_c, nullptr);

  return Napi::Value(env, ctx->promise);
}

// --- Lifecycle ---

Napi::Value GraphQLLayer::Close(const Napi::CallbackInfo& info) {
  if (layer_) {
    bridge_.Shutdown();

    graphql_layer_t* layer = layer_;
    layer_ = nullptr;
    graphql_layer_destroy(layer);
  }
  return info.Env().Undefined();
}

// Module init
Napi::Object InitModule(Napi::Env env, Napi::Object exports) {
  return GraphQLLayer::Init(env, exports);
}

NODE_API_MODULE(graphql, InitModule)