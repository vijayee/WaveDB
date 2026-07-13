// C++ headers that include <atomic> must come before C headers
// that use ATOMIC_TYPE() macros expanding to std::atomic<T> in C++.
#include <atomic>

#include <napi.h>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "Layers/vector/vector_layer.h"
#include "Database/database.h"
#include "Database/database_subtree.h"

// VectorLayer N-API wrapper.
//
// Mirrors the GraphLayer pattern (ObjectWrap holding an opaque C pointer).
// Two construction modes:
//   - Separate mode: new VectorLayer(pathStr, indexName, formatObj, runtimeObj)
//       -> vector_layer_open_separate
//   - Embedded mode: new VectorLayer(indexName, dbPtrBigInt, formatObj,
//                                    runtimeObj, subtreePtrBigInt?)
//       -> vector_layer_create
//
// The wrapper detects the mode by inspecting the second argument: a string
// means separate mode (path), a BigInt means embedded mode (database_t*).
//
// Config is split into Format (immutable after create) and Runtime (mutable
// via reconfigure). The C API has no slsh_bidirectional field — SLSH always
// scans bidirectionally — and this binding does not expose one.
//
// Async API (vector_layer_insert/search/delete returning void + promise_t*)
// is deferred: the C promise bridge would need a ThreadSafeFunction +
// per-op context, mirroring async_bridge.cc. The sync variants are wrapped
// here; lib/vector_layer.js exposes Promise-returning wrappers that just
// call the sync variants. A true async bridge is a follow-up.

namespace {

// Parse a JS format object into vector_layer_format_t. Returns true on
// success; on failure throws a JS exception and returns false.
bool ParseFormat(Napi::Env env, Napi::Object obj, vector_layer_format_t& out) {
  std::memset(&out, 0, sizeof(out));
  out.delimiter = '/';
  out.ivf_n_clusters = 0;     // 0 -> C layer applies default
  out.slsh_lsh_tables = 0;
  out.slsh_hash_bits = 0;
  out.slsh_bucket_width = 0.0f;

  if (obj.Has("index_type")) {
    out.index_type = static_cast<vl_index_type_t>(
        obj.Get("index_type").As<Napi::Number>().Int32Value());
  }
  if (obj.Has("dim")) {
    out.dim = obj.Get("dim").As<Napi::Number>().Int32Value();
  } else {
    Napi::TypeError::New(env, "format.dim is required").ThrowAsJavaScriptException();
    return false;
  }
  if (obj.Has("delimiter")) {
    Napi::Value v = obj.Get("delimiter");
    if (v.IsString()) {
      std::string s = v.As<Napi::String>().Utf8Value();
      if (!s.empty()) out.delimiter = s[0];
    } else if (v.IsNumber()) {
      out.delimiter = static_cast<char>(v.As<Napi::Number>().Int32Value());
    }
  }
  if (obj.Has("distance")) {
    out.distance = static_cast<vl_distance_t>(
        obj.Get("distance").As<Napi::Number>().Int32Value());
  }
  if (obj.Has("ivf_n_clusters")) {
    out.ivf_n_clusters = obj.Get("ivf_n_clusters").As<Napi::Number>().Int32Value();
  }
  if (obj.Has("slsh_lsh_tables")) {
    out.slsh_lsh_tables = obj.Get("slsh_lsh_tables").As<Napi::Number>().Int32Value();
  }
  if (obj.Has("slsh_hash_bits")) {
    out.slsh_hash_bits = obj.Get("slsh_hash_bits").As<Napi::Number>().Int32Value();
  }
  if (obj.Has("slsh_bucket_width")) {
    out.slsh_bucket_width = static_cast<float>(
        obj.Get("slsh_bucket_width").As<Napi::Number>().FloatValue());
  }
  return true;
}

// Parse a JS runtime object into vector_layer_runtime_t. Returns true on
// success; on failure throws a JS exception and returns false. Zeroed
// fields let the C layer apply its own defaults.
bool ParseRuntime(Napi::Env env, Napi::Object obj, vector_layer_runtime_t& out) {
  std::memset(&out, 0, sizeof(out));
  if (obj.Has("top_k")) {
    out.top_k = obj.Get("top_k").As<Napi::Number>().Int32Value();
  }
  if (obj.Has("sync_only")) {
    out.sync_only = obj.Get("sync_only").As<Napi::Number>().Int32Value();
  }
  if (obj.Has("ivf_nprobe")) {
    out.ivf_nprobe = obj.Get("ivf_nprobe").As<Napi::Number>().Int32Value();
  }
  if (obj.Has("ivf_flat_until")) {
    out.ivf_flat_until = obj.Get("ivf_flat_until").As<Napi::Number>().Int32Value();
  }
  if (obj.Has("slsh_scan_radius")) {
    out.slsh_scan_radius = obj.Get("slsh_scan_radius").As<Napi::Number>().Int32Value();
  }
  return true;
}

// Read a Float32Array (or any TypedArray of floats) into a contiguous
// float* buffer. Returns true on success; on failure throws a JS exception
// and returns false. The returned pointer is owned by the JS ArrayBuffer
// (no copy) when the input is already a Float32Array backed by an
// ArrayBuffer — we just borrow the data pointer for the duration of the
// sync call. `out_len` receives the number of floats.
bool BorrowFloat32Array(Napi::Env env, Napi::Value v, const float*& out_ptr,
                        size_t& out_len) {
  if (!v.IsTypedArray()) {
    Napi::TypeError::New(env, "Float32Array expected for vector argument")
        .ThrowAsJavaScriptException();
    return false;
  }
  Napi::TypedArray ta = v.As<Napi::TypedArray>();
  if (ta.TypedArrayType() != napi_float32_array) {
    Napi::TypeError::New(env, "Float32Array expected (got other TypedArray)")
        .ThrowAsJavaScriptException();
    return false;
  }
  Napi::Float32Array arr = v.As<Napi::Float32Array>();
  out_ptr = arr.Data();
  out_len = arr.ElementLength();
  return true;
}

// Read a Buffer (or null/undefined) into a (pointer, length) pair. If the
// argument is null/undefined, returns (nullptr, 0).
bool BorrowBuffer(Napi::Env env, Napi::Value v, const uint8_t*& out_ptr,
                  size_t& out_len) {
  if (v.IsNull() || v.IsUndefined()) {
    out_ptr = nullptr;
    out_len = 0;
    return true;
  }
  if (v.IsBuffer()) {
    Napi::Buffer<uint8_t> buf = v.As<Napi::Buffer<uint8_t>>();
    out_ptr = buf.Data();
    out_len = buf.Length();
    return true;
  }
  Napi::TypeError::New(env, "Buffer or null expected for metadata argument")
      .ThrowAsJavaScriptException();
  return false;
}

// Marshal a vl_result_t array + count into a JS array of
// {id, distance, metadata} objects. metadata is copied into a Node Buffer
// (so the caller can free the C results immediately after).
Napi::Array ResultsToJS(Napi::Env env, vl_result_t* results, int n) {
  Napi::Array arr = Napi::Array::New(env, static_cast<uint32_t>(n));
  for (int i = 0; i < n; i++) {
    Napi::Object obj = Napi::Object::New(env);
    obj.Set("id", Napi::String::New(env, results[i].id ? results[i].id : ""));
    obj.Set("distance", Napi::Number::New(env, results[i].distance));
    if (results[i].metadata && results[i].metadata_len > 0) {
      Napi::Buffer<uint8_t> buf = Napi::Buffer<uint8_t>::Copy(
          env, results[i].metadata, results[i].metadata_len);
      obj.Set("metadata", buf);
    } else {
      obj.Set("metadata", env.Null());
    }
    arr.Set(static_cast<uint32_t>(i), obj);
  }
  return arr;
}

}  // namespace

class VectorLayer : public Napi::ObjectWrap<VectorLayer> {
public:
  static Napi::Object Init(Napi::Env env, Napi::Object exports);

  VectorLayer(const Napi::CallbackInfo& info);
  ~VectorLayer();

private:
  static Napi::FunctionReference constructor_;
  vector_layer_t* layer_;

  // Sync operations
  Napi::Value InsertSync(const Napi::CallbackInfo& info);
  Napi::Value InsertBatchSync(const Napi::CallbackInfo& info);
  Napi::Value SearchSync(const Napi::CallbackInfo& info);
  Napi::Value DeleteSync(const Napi::CallbackInfo& info);
  Napi::Value Train(const Napi::CallbackInfo& info);
  Napi::Value Rebuild(const Napi::CallbackInfo& info);
  Napi::Value Count(const Napi::CallbackInfo& info);
  Napi::Value Reconfigure(const Napi::CallbackInfo& info);
  Napi::Value Close(const Napi::CallbackInfo& info);
};

Napi::FunctionReference VectorLayer::constructor_;

Napi::Object VectorLayer::Init(Napi::Env env, Napi::Object exports) {
  Napi::HandleScope scope(env);

  Napi::Function func = DefineClass(env, "VectorLayer", {
    InstanceMethod("insertSync", &VectorLayer::InsertSync),
    InstanceMethod("insertBatchSync", &VectorLayer::InsertBatchSync),
    InstanceMethod("searchSync", &VectorLayer::SearchSync),
    InstanceMethod("deleteSync", &VectorLayer::DeleteSync),
    InstanceMethod("train", &VectorLayer::Train),
    InstanceMethod("rebuild", &VectorLayer::Rebuild),
    InstanceMethod("count", &VectorLayer::Count),
    InstanceMethod("reconfigure", &VectorLayer::Reconfigure),
    InstanceMethod("close", &VectorLayer::Close),
  });

  constructor_ = Napi::Persistent(func);
  exports.Set("VectorLayer", func);

  // Export the enum values as top-level constants so JS callers don't have
  // to remember the integer values.
  exports.Set("VL_INDEX_FLAT", Napi::Number::New(env, VL_INDEX_FLAT));
  exports.Set("VL_INDEX_IVF",  Napi::Number::New(env, VL_INDEX_IVF));
  exports.Set("VL_INDEX_SLSH", Napi::Number::New(env, VL_INDEX_SLSH));
  exports.Set("VL_DIST_L2",     Napi::Number::New(env, VL_DIST_L2));
  exports.Set("VL_DIST_COSINE", Napi::Number::New(env, VL_DIST_COSINE));
  exports.Set("VL_DIST_DOT",    Napi::Number::New(env, VL_DIST_DOT));

  return exports;
}

VectorLayer::VectorLayer(const Napi::CallbackInfo& info)
  : Napi::ObjectWrap<VectorLayer>(info),
    layer_(nullptr) {

  Napi::Env env = info.Env();

  // Two modes:
  //   Separate:  (path: string, indexName: string, formatObj, runtimeObj)
  //   Embedded:  (indexName: string, dbPtr: bigint, formatObj, runtimeObj,
  //               subtreePtr?: bigint)
  // We dispatch on info[1]: string -> separate, bigint -> embedded.
  if (info.Length() < 4) {
    Napi::TypeError::New(env,
        "VectorLayer(path, indexName, format, runtime) or "
        "VectorLayer(indexName, dbPtr, format, runtime, subtreePtr?) required")
        .ThrowAsJavaScriptException();
    return;
  }

  if (!info[0].IsString()) {
    Napi::TypeError::New(env, "indexName (or path) string required as arg 0")
        .ThrowAsJavaScriptException();
    return;
  }
  std::string firstStr = info[0].As<Napi::String>().Utf8Value();

  if (!info[2].IsObject() || !info[3].IsObject()) {
    Napi::TypeError::New(env, "format and runtime objects required")
        .ThrowAsJavaScriptException();
    return;
  }

  vector_layer_config_t config;
  if (!ParseFormat(env, info[2].As<Napi::Object>(), config.format)) return;
  if (!ParseRuntime(env, info[3].As<Napi::Object>(), config.runtime)) return;

  int error_code = 0;

  if (info[1].IsBigInt()) {
    // Embedded mode: indexName, dbPtr, format, runtime, subtreePtr?
    bool lossless = false;
    int64_t dbPtrVal = info[1].As<Napi::BigInt>().Int64Value(&lossless);
    if (!lossless || dbPtrVal == 0) {
      Napi::TypeError::New(env, "invalid database pointer (BigInt required)")
          .ThrowAsJavaScriptException();
      return;
    }
    database_t* db = reinterpret_cast<database_t*>(static_cast<uintptr_t>(dbPtrVal));

    database_subtree_t* subtree = nullptr;
    if (info.Length() >= 5 && info[4].IsBigInt()) {
      int64_t subVal = info[4].As<Napi::BigInt>().Int64Value(&lossless);
      if (lossless && subVal != 0) {
        subtree = reinterpret_cast<database_subtree_t*>(
            static_cast<uintptr_t>(subVal));
      }
    }

    layer_ = vector_layer_create(firstStr.c_str(), db, subtree, &config, &error_code);
  } else if (info[1].IsString()) {
    // Separate mode: path, indexName, format, runtime
    std::string indexName = info[1].As<Napi::String>().Utf8Value();
    layer_ = vector_layer_open_separate(firstStr.c_str(), indexName.c_str(),
                                        &config, &error_code);
  } else {
    Napi::TypeError::New(env,
        "arg 1 must be indexName string (separate mode) or dbPtr bigint (embedded mode)")
        .ThrowAsJavaScriptException();
    return;
  }

  if (!layer_) {
    std::string msg = "Failed to create VectorLayer (error_code=" +
                      std::to_string(error_code) + ")";
    Napi::Error::New(env, msg).ThrowAsJavaScriptException();
    return;
  }
}

VectorLayer::~VectorLayer() {
  if (layer_) {
    vector_layer_t* l = layer_;
    layer_ = nullptr;
    vector_layer_destroy(l);
  }
}

Napi::Value VectorLayer::InsertSync(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!layer_) {
    Napi::Error::New(env, "VECTOR_LAYER_CLOSED").ThrowAsJavaScriptException();
    return env.Null();
  }
  if (info.Length() < 2 || !info[0].IsString()) {
    Napi::TypeError::New(env, "insertSync(id, vec, metadata?) required")
        .ThrowAsJavaScriptException();
    return env.Null();
  }
  std::string id = info[0].As<Napi::String>().Utf8Value();
  const float* vec = nullptr;
  size_t vec_len = 0;
  if (!BorrowFloat32Array(env, info[1], vec, vec_len)) return env.Null();
  const uint8_t* meta = nullptr;
  size_t meta_len = 0;
  if (info.Length() >= 3 && !BorrowBuffer(env, info[2], meta, meta_len)) {
    return env.Null();
  }
  int rc = vector_layer_insert_sync(layer_, id.c_str(), vec, meta, meta_len);
  if (rc != 0) {
    Napi::Error::New(env, std::string("insertSync failed: rc=" + std::to_string(rc)))
        .ThrowAsJavaScriptException();
  }
  return env.Undefined();
}

Napi::Value VectorLayer::InsertBatchSync(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!layer_) {
    Napi::Error::New(env, "VECTOR_LAYER_CLOSED").ThrowAsJavaScriptException();
    return env.Null();
  }
  if (info.Length() < 1 || !info[0].IsArray()) {
    Napi::TypeError::New(env, "insertBatchSync(items[]) required")
        .ThrowAsJavaScriptException();
    return env.Null();
  }
  Napi::Array items = info[0].As<Napi::Array>();
  int n = static_cast<int>(items.Length());
  if (n == 0) return env.Undefined();

  // Marshal JS items into C arrays. We hold std::string + Napi::Object
  // references in vectors to keep the borrowed pointers alive for the
  // duration of the sync call.
  std::vector<std::string> ids_str(n);
  std::vector<const char*> ids(n);
  std::vector<const float*> vecs(n);
  std::vector<const uint8_t*> metas(n);
  std::vector<size_t> meta_lens(n);
  std::vector<Napi::Value> held(n);  // keep TypedArrays / Buffers alive

  for (int i = 0; i < n; i++) {
    Napi::Value v = items.Get(static_cast<uint32_t>(i));
    if (!v.IsObject()) {
      Napi::TypeError::New(env, "each batch item must be an object")
          .ThrowAsJavaScriptException();
      return env.Null();
    }
    Napi::Object o = v.As<Napi::Object>();
    if (!o.Has("id") || !o.Has("vec")) {
      Napi::TypeError::New(env, "each batch item needs {id, vec}")
          .ThrowAsJavaScriptException();
      return env.Null();
    }
    ids_str[i] = o.Get("id").As<Napi::String>().Utf8Value();
    ids[i] = ids_str[i].c_str();

    Napi::Value vecVal = o.Get("vec");
    const float* fp = nullptr;
    size_t fl = 0;
    if (!BorrowFloat32Array(env, vecVal, fp, fl)) return env.Null();
    vecs[i] = fp;
    held.push_back(vecVal);

    const uint8_t* mp = nullptr;
    size_t ml = 0;
    if (o.Has("metadata")) {
      if (!BorrowBuffer(env, o.Get("metadata"), mp, ml)) return env.Null();
    }
    metas[i] = mp;
    meta_lens[i] = ml;
  }

  int rc = vector_layer_insert_batch_sync(layer_, ids.data(), vecs.data(),
                                          metas.data(), meta_lens.data(), n);
  if (rc != 0) {
    Napi::Error::New(env, std::string("insertBatchSync failed: rc=" + std::to_string(rc)))
        .ThrowAsJavaScriptException();
  }
  return env.Undefined();
}

Napi::Value VectorLayer::SearchSync(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!layer_) {
    Napi::Error::New(env, "VECTOR_LAYER_CLOSED").ThrowAsJavaScriptException();
    return env.Null();
  }
  if (info.Length() < 2) {
    Napi::TypeError::New(env, "searchSync(query: Float32Array, k: number) required")
        .ThrowAsJavaScriptException();
    return env.Null();
  }
  const float* q = nullptr;
  size_t q_len = 0;
  if (!BorrowFloat32Array(env, info[0], q, q_len)) return env.Null();
  int k = info[1].As<Napi::Number>().Int32Value();

  vl_result_t* results = nullptr;
  int n_results = 0;
  int rc = vector_layer_search_sync(layer_, q, k, &results, &n_results);
  if (rc != 0) {
    Napi::Error::New(env, std::string("searchSync failed: rc=" + std::to_string(rc)))
        .ThrowAsJavaScriptException();
    return env.Null();
  }
  Napi::Array arr = ResultsToJS(env, results, n_results);
  vector_layer_free_results(results, n_results);
  return arr;
}

Napi::Value VectorLayer::DeleteSync(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!layer_) {
    Napi::Error::New(env, "VECTOR_LAYER_CLOSED").ThrowAsJavaScriptException();
    return env.Null();
  }
  if (info.Length() < 1 || !info[0].IsString()) {
    Napi::TypeError::New(env, "deleteSync(id) required")
        .ThrowAsJavaScriptException();
    return env.Null();
  }
  std::string id = info[0].As<Napi::String>().Utf8Value();
  int rc = vector_layer_delete_sync(layer_, id.c_str());
  if (rc != 0) {
    Napi::Error::New(env, std::string("deleteSync failed: rc=" + std::to_string(rc)))
        .ThrowAsJavaScriptException();
  }
  return env.Undefined();
}

Napi::Value VectorLayer::Train(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!layer_) {
    Napi::Error::New(env, "VECTOR_LAYER_CLOSED").ThrowAsJavaScriptException();
    return env.Null();
  }
  int rc = vector_layer_train(layer_);
  if (rc != 0) {
    Napi::Error::New(env, std::string("train failed: rc=" + std::to_string(rc)))
        .ThrowAsJavaScriptException();
  }
  return env.Undefined();
}

Napi::Value VectorLayer::Rebuild(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!layer_) {
    Napi::Error::New(env, "VECTOR_LAYER_CLOSED").ThrowAsJavaScriptException();
    return env.Null();
  }
  int rc = vector_layer_rebuild(layer_);
  if (rc != 0) {
    Napi::Error::New(env, std::string("rebuild failed: rc=" + std::to_string(rc)))
        .ThrowAsJavaScriptException();
  }
  return env.Undefined();
}

Napi::Value VectorLayer::Count(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!layer_) {
    Napi::Error::New(env, "VECTOR_LAYER_CLOSED").ThrowAsJavaScriptException();
    return env.Null();
  }
  size_t c = vector_layer_count(layer_);
  return Napi::Number::New(env, static_cast<double>(c));
}

Napi::Value VectorLayer::Reconfigure(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!layer_) {
    Napi::Error::New(env, "VECTOR_LAYER_CLOSED").ThrowAsJavaScriptException();
    return env.Null();
  }
  if (info.Length() < 1 || !info[0].IsObject()) {
    Napi::TypeError::New(env, "reconfigure(runtimeObj) required")
        .ThrowAsJavaScriptException();
    return env.Null();
  }
  vector_layer_runtime_t rt;
  if (!ParseRuntime(env, info[0].As<Napi::Object>(), rt)) return env.Null();
  int rc = vector_layer_reconfigure(layer_, &rt);
  if (rc != 0) {
    Napi::Error::New(env, std::string("reconfigure failed: rc=" + std::to_string(rc)))
        .ThrowAsJavaScriptException();
  }
  return env.Undefined();
}

Napi::Value VectorLayer::Close(const Napi::CallbackInfo& info) {
  if (layer_) {
    vector_layer_t* l = layer_;
    layer_ = nullptr;
    vector_layer_destroy(l);
  }
  return info.Env().Undefined();
}

Napi::Object InitVectorLayer(Napi::Env env, Napi::Object exports) {
  return VectorLayer::Init(env, exports);
}