#include <napi.h>
#include <string>
#include "../../../src/Layers/graphql/graphql.h"

class GraphQLLayer : public Napi::ObjectWrap<GraphQLLayer> {
public:
  static Napi::Object Init(Napi::Env env, Napi::Object exports);

  GraphQLLayer(const Napi::CallbackInfo& info);
  ~GraphQLLayer();

private:
  static Napi::FunctionReference constructor_;
  graphql_layer_t* layer_;

  // Schema
  Napi::Value ParseSchema(const Napi::CallbackInfo& info);

  // Queries
  Napi::Value QuerySync(const Napi::CallbackInfo& info);
  Napi::Value MutateSync(const Napi::CallbackInfo& info);

  // Lifecycle
  Napi::Value Close(const Napi::CallbackInfo& info);

  // Convert graphql_result_t to JS object
  static Napi::Value ResultToJS(Napi::Env env, graphql_result_t* result);
  static Napi::Value ResultNodeToJS(Napi::Env env, graphql_result_node_t* node);
};

Napi::FunctionReference GraphQLLayer::constructor_;

Napi::Object GraphQLLayer::Init(Napi::Env env, Napi::Object exports) {
  Napi::HandleScope scope(env);

  Napi::Function func = DefineClass(env, "GraphQLLayer", {
    InstanceMethod("parseSchema", &GraphQLLayer::ParseSchema),
    InstanceMethod("querySync", &GraphQLLayer::QuerySync),
    InstanceMethod("mutateSync", &GraphQLLayer::MutateSync),
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

  // Create default config
  graphql_layer_config_t* config = graphql_layer_config_default();
  if (!config) {
    Napi::Error::New(env, "Failed to create default configuration").ThrowAsJavaScriptException();
    return;
  }

  // Path argument (optional - NULL for in-memory)
  const char* path = nullptr;
  if (info.Length() > 0 && info[0].IsString()) {
    std::string pathStr = info[0].As<Napi::String>().Utf8Value();
    config->path = strdup(pathStr.c_str());
  }

  // Options argument
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
    // Config path is a string literal, don't free it
    graphql_layer_config_destroy(config);
    Napi::Error::New(env, "Failed to create GraphQL layer").ThrowAsJavaScriptException();
    return;
  }

  graphql_layer_config_destroy(config);
}

GraphQLLayer::~GraphQLLayer() {
  if (layer_) {
    graphql_layer_destroy(layer_);
    layer_ = nullptr;
  }
}

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
  int rc = graphql_schema_parse(layer_, sdl.c_str());

  if (rc != 0) {
    Napi::Error::New(env, "Failed to parse schema").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  return env.Undefined();
}

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

  Napi::Value jsResult = ResultToJS(env, result);
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

  Napi::Value jsResult = ResultToJS(env, result);
  graphql_result_destroy(result);

  return jsResult;
}

Napi::Value GraphQLLayer::Close(const Napi::CallbackInfo& info) {
  if (layer_) {
    graphql_layer_t* layer = layer_;
    layer_ = nullptr;
    graphql_layer_destroy(layer);
  }
  return info.Env().Undefined();
}

Napi::Value GraphQLLayer::ResultNodeToJS(Napi::Env env, graphql_result_node_t* node) {
  if (node == nullptr) {
    return env.Null();
  }

  switch (node->kind) {
    case RESULT_NULL:
      return env.Null();

    case RESULT_STRING:
      if (node->string_val) {
        return Napi::String::New(env, node->string_val);
      }
      return env.Null();

    case RESULT_INT:
      return Napi::Number::New(env, static_cast<double>(node->int_val));

    case RESULT_FLOAT:
      return Napi::Number::New(env, node->float_val);

    case RESULT_BOOL:
      return Napi::Boolean::New(env, node->bool_val);

    case RESULT_ID:
      if (node->id_val) {
        return Napi::String::New(env, node->id_val);
      }
      return env.Null();

    case RESULT_LIST: {
      Napi::Array arr = Napi::Array::New(env);
      for (size_t i = 0; i < node->children.length; i++) {
        arr.Set(i, ResultNodeToJS(env, node->children.data[i]));
      }
      return arr;
    }

    case RESULT_OBJECT: {
      Napi::Object obj = Napi::Object::New(env);
      for (size_t i = 0; i < node->children.length; i++) {
        graphql_result_node_t* child = node->children.data[i];
        const char* key = child->name ? child->name : "";
        obj.Set(key, ResultNodeToJS(env, child));
      }
      return obj;
    }

    case RESULT_REF:
      return env.Null();

    default:
      return env.Null();
  }
}

Napi::Value GraphQLLayer::ResultToJS(Napi::Env env, graphql_result_t* result) {
  if (result == nullptr) {
    Napi::Object empty = Napi::Object::New(env);
    empty.Set("data", env.Null());
    empty.Set("success", Napi::Boolean::New(env, false));
    return empty;
  }

  Napi::Object obj = Napi::Object::New(env);
  obj.Set("success", Napi::Boolean::New(env, result->success));
  obj.Set("data", ResultNodeToJS(env, result->data));

  // Convert errors
  if (result->errors.length > 0) {
    Napi::Array errors = Napi::Array::New(env);
    for (size_t i = 0; i < result->errors.length; i++) {
      Napi::Object errObj = Napi::Object::New(env);
      errObj.Set("message", Napi::String::New(env, result->errors.data[i].message ? result->errors.data[i].message : ""));
      errors.Set(i, errObj);
    }
    obj.Set("errors", errors);
  }

  return obj;
}

// Module init
Napi::Object InitModule(Napi::Env env, Napi::Object exports) {
  return GraphQLLayer::Init(env, exports);
}

NODE_API_MODULE(graphql, InitModule)