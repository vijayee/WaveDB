#include <napi.h>
#include <string>
#include <vector>
#include <cctype>
#include "../../../src/HBTrie/identifier.h"
#include "../../../src/HBTrie/chunk.h"
#include "../../../src/Buffer/buffer.h"

// Check if bytes are printable ASCII
static bool IsPrintableASCII(const uint8_t* data, size_t size) {
  for (size_t i = 0; i < size; i++) {
    if (!isprint(data[i]) && data[i] != '\t' && data[i] != '\n' && data[i] != '\r') {
      return false;
    }
  }
  return true;
}

// Convert JavaScript value (string or Buffer) to identifier_t*
identifier_t* ValueFromJS(Napi::Env env, Napi::Value value) {
  if (value.IsNull() || value.IsUndefined()) {
    return nullptr;
  }

  buffer_t* buf = nullptr;

  if (value.IsString()) {
    std::string str = value.As<Napi::String>().Utf8Value();
    buf = buffer_create_from_pointer_copy(
      const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(str.c_str())),
      str.size()
    );
  } else if (value.IsBuffer()) {
    Napi::Buffer<uint8_t> buffer = value.As<Napi::Buffer<uint8_t>>();
    buf = buffer_create_from_pointer_copy(buffer.Data(), buffer.Length());
  } else {
    Napi::TypeError::New(env, "Value must be string or Buffer").ThrowAsJavaScriptException();
    return nullptr;
  }

  if (!buf) {
    Napi::Error::New(env, "Failed to create buffer").ThrowAsJavaScriptException();
    return nullptr;
  }

  identifier_t* id = identifier_create(buf, 0);
  buffer_destroy(buf);

  if (!id) {
    Napi::Error::New(env, "Failed to create identifier").ThrowAsJavaScriptException();
    return nullptr;
  }

  return id;
}

// Convert identifier_t* to JavaScript value (string or Buffer)
Napi::Value ValueToJS(Napi::Env env, identifier_t* id) {
  if (!id) {
    return env.Null();
  }

  // Collect all bytes from identifier
  std::vector<uint8_t> bytes;
  for (size_t i = 0; i < static_cast<size_t>(id->chunks.length); i++) {
    chunk_t* chunk = static_cast<chunk_t*>(id->chunks.data[i]);
    const uint8_t* data = static_cast<const uint8_t*>(chunk_data_const(chunk));
    size_t size = chunk->data->size;

    bytes.insert(bytes.end(), data, data + size);
  }

  // Return as string if printable ASCII, otherwise Buffer
  if (IsPrintableASCII(bytes.data(), bytes.size())) {
    return Napi::String::New(env,
      std::string(bytes.begin(), bytes.end()));
  } else {
    return Napi::Buffer<uint8_t>::Copy(env, bytes.data(), bytes.size());
  }
}