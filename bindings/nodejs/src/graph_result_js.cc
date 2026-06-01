// C++ headers that include <atomic> must come before C headers
#include <atomic>

#include "graph_result_js.h"

Napi::Array GraphResultToJS(Napi::Env env, graph_result_t* result) {
  if (!result) return Napi::Array::New(env, 0);

  size_t count = graph_result_count(result);
  const char* const* verts = graph_result_vertices(result);

  Napi::Array arr = Napi::Array::New(env, count);
  for (size_t i = 0; i < count; i++) {
    arr.Set(i, Napi::String::New(env, verts[i]));
  }
  return arr;
}
