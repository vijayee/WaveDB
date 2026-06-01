// C++ headers that include <atomic> must come before C headers
// that use ATOMIC_TYPE() macros expanding to std::atomic<T> in C++.
#include <atomic>

#include <napi.h>
#include "../../../src/Layers/graph/graph.h"

#ifndef WAVEDB_BINDINGS_GRAPH_RESULT_JS_H
#define WAVEDB_BINDINGS_GRAPH_RESULT_JS_H

// Convert a C graph_result_t to a JS array of strings.
// Returns an empty array if result is NULL or has count 0.
// The caller must still destroy the C result separately.
Napi::Array GraphResultToJS(Napi::Env env, graph_result_t* result);

#endif
