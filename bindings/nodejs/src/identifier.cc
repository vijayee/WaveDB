// C++ headers that include <atomic> must come before C headers
// that use ATOMIC_TYPE() macros expanding to std::atomic<T> in C++.
#include <atomic>

#include "identifier.h"
#include "../../../src/HBTrie/chunk.h"
#include "../../../src/Buffer/buffer.h"
#include <cctype>
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