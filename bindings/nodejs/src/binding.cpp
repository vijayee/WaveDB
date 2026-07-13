// C++ headers that include <atomic> must come before C headers
// that use ATOMIC_TYPE() macros expanding to std::atomic<T> in C++.
#include <atomic>

#include <napi.h>
#include "database.h"
#include "iterator.h"
#include "subtree.h"

// Init functions for GraphLayer and GraphQLLayer — defined in graph_layer.cc
// and graphql_layer.cc, which are now linked into the single wavedb.node addon
// (instead of separate graph.node / graphql.node addons). Combining all
// native addons into ONE .node file is required because the C library is
// statically linked into each addon — separate .node files each get their
// own copy of the memory pool's static arrays (g_small_pool etc.), and
// objects allocated by one .node cannot be freed by another .node's
// memory_pool_free (the pointer is in the other .node's BSS, not recognized
// as a pool pointer, so free() is called on a BSS address → crash).
Napi::Object InitGraph(Napi::Env env, Napi::Object exports);
Napi::Object InitVectorLayer(Napi::Env env, Napi::Object exports);
Napi::Object InitModule(Napi::Env env, Napi::Object exports);

Napi::Object Init(Napi::Env env, Napi::Object exports) {
  WaveDB::Init(env, exports);
  Iterator::Init(env, exports);
  Subtree::Init(env, exports);
  InitGraph(env, exports);
  InitVectorLayer(env, exports);
  InitModule(env, exports);
  return exports;
}

NODE_API_MODULE(wavedb, Init)