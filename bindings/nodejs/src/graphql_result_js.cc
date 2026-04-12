#include "graphql_result_js.h"

Napi::Value GraphQLResultNodeToJS(Napi::Env env, graphql_result_node_t* node) {
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
        arr.Set(i, GraphQLResultNodeToJS(env, node->children.data[i]));
      }
      return arr;
    }

    case RESULT_OBJECT: {
      Napi::Object obj = Napi::Object::New(env);
      for (size_t i = 0; i < node->children.length; i++) {
        graphql_result_node_t* child = node->children.data[i];
        const char* key = child->name ? child->name : "";
        obj.Set(key, GraphQLResultNodeToJS(env, child));
      }
      return obj;
    }

    case RESULT_REF:
      return env.Null();

    default:
      return env.Null();
  }
}

Napi::Value GraphQLResultToJS(Napi::Env env, graphql_result_t* result) {
  if (result == nullptr) {
    Napi::Object empty = Napi::Object::New(env);
    empty.Set("data", env.Null());
    empty.Set("success", Napi::Boolean::New(env, false));
    return empty;
  }

  Napi::Object obj = Napi::Object::New(env);
  obj.Set("success", Napi::Boolean::New(env, result->success));
  obj.Set("data", GraphQLResultNodeToJS(env, result->data));

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