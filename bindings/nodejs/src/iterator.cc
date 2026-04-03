#include "iterator.h"
#include "path.h"
#include "identifier.h"

Napi::FunctionReference Iterator::constructor_;

Napi::Object Iterator::Init(Napi::Env env, Napi::Object exports) {
  Napi::HandleScope scope(env);

  Napi::Function func = DefineClass(env, "Iterator", {
    InstanceMethod("read", &Iterator::Read),
    InstanceMethod("end", &Iterator::End),
  });

  constructor_ = Napi::Persistent(func);
  exports.Set("Iterator", func);

  // Register cleanup hook to release constructor_ before Node.js shuts down
  napi_add_env_cleanup_hook(env, [](void* arg) {
    Iterator::constructor_.Reset();
  }, nullptr);

  return exports;
}

Iterator::Iterator(const Napi::CallbackInfo& info)
  : Napi::ObjectWrap<Iterator>(info),
    db_(nullptr),
    reverse_(false),
    keys_(true),
    values_(true),
    keyAsArray_(false),
    delimiter_('/'),
    ended_(false),
    scan_handle_(nullptr) {

  Napi::Env env = info.Env();

  // Expected arguments: db, options
  if (info.Length() < 2) {
    Napi::TypeError::New(env, "Expected db and options arguments").ThrowAsJavaScriptException();
    return;
  }

  // Get database pointer from external
  Napi::External<database_t> dbExternal = info[0].As<Napi::External<database_t>>();
  db_ = dbExternal.Data();

  // Parse options
  Napi::Object options = info[1].As<Napi::Object>();

  if (options.Has("start")) {
    Napi::Value start = options.Get("start");
    if (start.IsString()) {
      start_ = start.As<Napi::String>().Utf8Value();
    }
  }

  if (options.Has("end")) {
    Napi::Value end = options.Get("end");
    if (end.IsString()) {
      end_ = end.As<Napi::String>().Utf8Value();
    }
  }

  if (options.Has("reverse")) {
    reverse_ = options.Get("reverse").As<Napi::Boolean>().Value();
  }

  if (options.Has("keys")) {
    keys_ = options.Get("keys").As<Napi::Boolean>().Value();
  }

  if (options.Has("values")) {
    values_ = options.Get("values").As<Napi::Boolean>().Value();
  }

  if (options.Has("keyAsArray")) {
    keyAsArray_ = options.Get("keyAsArray").As<Napi::Boolean>().Value();
  }

  if (options.Has("delimiter")) {
    Napi::Value delim = options.Get("delimiter");
    if (delim.IsString()) {
      std::string delimStr = delim.As<Napi::String>().Utf8Value();
      if (!delimStr.empty()) {
        delimiter_ = delimStr[0];
      }
    }
  }

  // Create start and end paths
  path_t* start_path = nullptr;
  path_t* end_path = nullptr;

  if (!start_.empty()) {
    Napi::Value startVal = Napi::String::New(env, start_);
    start_path = PathFromJS(env, startVal, delimiter_);
    if (!start_path) {
      return;  // Exception already thrown
    }
  }

  if (!end_.empty()) {
    Napi::Value endVal = Napi::String::New(env, end_);
    end_path = PathFromJS(env, endVal, delimiter_);
    if (!end_path) {
      if (start_path) path_destroy(start_path);
      return;  // Exception already thrown
    }
  }

  // Start the scan
  scan_handle_ = database_scan_start(db_, start_path, end_path);

  // Note: database_scan_start takes ownership of paths, so we don't destroy them here
}

Iterator::~Iterator() {
  if (scan_handle_) {
    database_scan_end(scan_handle_);
    scan_handle_ = nullptr;
  }
}

Napi::Value Iterator::Read(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (ended_ || !scan_handle_) {
    return env.Null();
  }

  path_t* out_path = nullptr;
  identifier_t* out_value = nullptr;

  int result = database_scan_next(scan_handle_, &out_path, &out_value);

  if (result != 0) {
    // End of iteration or error
    ended_ = true;
    return env.Null();
  }

  // Build result object
  Napi::Object resultObj = Napi::Object::New(env);

  if (keys_ && out_path) {
    if (keyAsArray_) {
      resultObj.Set("key", PathToArrayJS(env, out_path, delimiter_));
    } else {
      resultObj.Set("key", Napi::String::New(env, PathToJS(out_path, delimiter_)));
    }
    path_destroy(out_path);
  } else {
    resultObj.Set("key", env.Null());
    if (out_path) path_destroy(out_path);
  }

  if (values_ && out_value) {
    resultObj.Set("value", ValueToJS(env, out_value));
    identifier_destroy(out_value);
  } else {
    resultObj.Set("value", env.Null());
    if (out_value) identifier_destroy(out_value);
  }

  return resultObj;
}

Napi::Value Iterator::End(const Napi::CallbackInfo& info) {
  ended_ = true;

  if (scan_handle_) {
    database_scan_end(scan_handle_);
    scan_handle_ = nullptr;
  }

  return info.Env().Undefined();
}