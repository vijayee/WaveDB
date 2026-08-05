// C++ headers that include <atomic> must come before C headers
// that use ATOMIC_TYPE() macros expanding to std::atomic<T> in C++.
#include <atomic>

#include "identifier.h"
#include "HBTrie/chunk.h"
#include "Buffer/buffer.h"
#include <cctype>
#include <cmath>
#include <cstdint>
#include <limits>
#include <node_api.h>

// Extract JS value into a std::string (copies Buffer data, no truncation for strings).
bool ValueFromJSDynamic(Napi::Env env, Napi::Value value, std::string& out) {
    if (value.IsString()) {
        out = value.As<Napi::String>().Utf8Value();
        return true;
    } else if (value.IsBuffer()) {
        Napi::Buffer<uint8_t> buffer = value.As<Napi::Buffer<uint8_t>>();
        out = std::string(reinterpret_cast<const char*>(buffer.Data()), buffer.Length());
        return true;
    } else if (value.IsNumber()) {
        // Store numbers as strings so object/array flattening can round-trip
        // through the scalar key-value store. GetObject's reader treats a
        // no-'.'/'e'/'E' string as a number, so integers >= 2 round-trip as
        // numbers when stored as plain "30". 0 and 1 collide with the boolean
        // encoding ("0"/"1" → false/true), so they keep the 6-decimal format
        // and read back as strings (pre-existing limitation). Floats also
        // keep the 6-decimal format — the reader currently treats any
        // '.'-containing string as a string, not a number.
        double d = value.As<Napi::Number>().DoubleValue();
        if (std::isfinite(d) && d == std::floor(d) && std::fabs(d) >= 2.0 &&
            d >= static_cast<double>(std::numeric_limits<int64_t>::min()) &&
            d <= static_cast<double>(std::numeric_limits<int64_t>::max())) {
            out = std::to_string(static_cast<int64_t>(d));
        } else {
            out = std::to_string(d);
        }
        return true;
    } else if (value.IsBoolean()) {
        out = value.As<Napi::Boolean>().Value() ? "1" : "0";
        return true;
    } else {
        Napi::TypeError::New(env, "Value must be string or Buffer").ThrowAsJavaScriptException();
        return false;
    }
}

// Extract JS value with zero-copy for Buffers.
bool ValueFromJSZeroCopy(Napi::Env env, Napi::Value value, std::string& out_str,
                         const uint8_t** val_buf, size_t* val_len) {
    if (value.IsString()) {
        out_str = value.As<Napi::String>().Utf8Value();
        *val_buf = reinterpret_cast<const uint8_t*>(out_str.c_str());
        *val_len = out_str.size();
        return true;
    } else if (value.IsBuffer()) {
        Napi::Buffer<uint8_t> buffer = value.As<Napi::Buffer<uint8_t>>();
        *val_buf = buffer.Data();
        *val_len = buffer.Length();
        return true;
    } else {
        Napi::TypeError::New(env, "Value must be string or Buffer").ThrowAsJavaScriptException();
        return false;
    }
}

// Convert identifier_t* to JavaScript value (string or Buffer)
Napi::Value ValueToJS(Napi::Env env, identifier_t* id) {
  if (!id) {
    return env.Null();
  }

  buffer_t* buf = identifier_to_buffer(id);
  if (!buf) {
    return env.Null();
  }

  const uint8_t* data = buf->data;
  size_t size = buf->size;

  // identifier_to_buffer may include trailing null padding from chunk alignment;
  // strip it to get the original data length.
  while (size > 0 && (data[size - 1] == '\0' || data[size - 1] == ' ')) {
    size--;
  }

  bool printable = true;
  for (size_t i = 0; i < size; i++) {
    if (!isprint(data[i]) && data[i] != '\t' && data[i] != '\n' && data[i] != '\r') {
      printable = false;
      break;
    }
  }

  Napi::Value result;
  if (printable) {
    result = Napi::String::New(env, std::string(reinterpret_cast<const char*>(data), size));
  } else {
    result = Napi::Buffer<uint8_t>::Copy(env, data, size);
  }

  buffer_destroy(buf);
  return result;
}