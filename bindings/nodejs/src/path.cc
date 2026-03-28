#include <napi.h>
#include <string>
#include <vector>
#include "../../../src/HBTrie/path.h"
#include "../../../src/HBTrie/identifier.h"
#include "../../../src/Buffer/buffer.h"

// Split string by delimiter
std::vector<std::string> SplitString(const std::string& str, char delimiter) {
  std::vector<std::string> tokens;
  size_t start = 0;
  size_t end = str.find(delimiter);

  while (end != std::string::npos) {
    tokens.push_back(str.substr(start, end - start));
    start = end + 1;
    end = str.find(delimiter, start);
  }
  tokens.push_back(str.substr(start));

  return tokens;
}

// Convert JavaScript key (string or array) to path_t*
path_t* PathFromJS(Napi::Env env, Napi::Value key, char delimiter) {
  path_t* path = path_create();
  if (!path) {
    Napi::Error::New(env, "Failed to create path").ThrowAsJavaScriptException();
    return nullptr;
  }

  if (key.IsString()) {
    std::string str = key.As<Napi::String>().Utf8Value();
    std::vector<std::string> parts = SplitString(str, delimiter);

    for (const auto& part : parts) {
      if (part.empty()) continue;  // Skip empty segments

      buffer_t* buf = buffer_create_from_pointer_copy(
        const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(part.c_str())),
        part.size()
      );
      if (!buf) {
        path_destroy(path);
        Napi::Error::New(env, "Failed to create buffer").ThrowAsJavaScriptException();
        return nullptr;
      }

      identifier_t* id = identifier_create(buf, 0);
      buffer_destroy(buf);
      if (!id) {
        path_destroy(path);
        Napi::Error::New(env, "Failed to create identifier").ThrowAsJavaScriptException();
        return nullptr;
      }

      path_append(path, id);
      identifier_destroy(id);
    }
  } else if (key.IsArray()) {
    Napi::Array arr = key.As<Napi::Array>();
    for (uint32_t i = 0; i < arr.Length(); i++) {
      Napi::Value part = arr.Get(i);
      if (!part.IsString()) {
        path_destroy(path);
        Napi::TypeError::New(env, "Path array elements must be strings").ThrowAsJavaScriptException();
        return nullptr;
      }

      std::string partStr = part.As<Napi::String>().Utf8Value();
      buffer_t* buf = buffer_create_from_pointer_copy(
        const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(partStr.c_str())),
        partStr.size()
      );
      if (!buf) {
        path_destroy(path);
        Napi::Error::New(env, "Failed to create buffer").ThrowAsJavaScriptException();
        return nullptr;
      }

      identifier_t* id = identifier_create(buf, 0);
      buffer_destroy(buf);
      if (!id) {
        path_destroy(path);
        Napi::Error::New(env, "Failed to create identifier").ThrowAsJavaScriptException();
        return nullptr;
      }

      path_append(path, id);
      identifier_destroy(id);
    }
  } else {
    path_destroy(path);
    Napi::TypeError::New(env, "Key must be string or array").ThrowAsJavaScriptException();
    return nullptr;
  }

  return path;
}

// Convert path_t* to JavaScript string
std::string PathToJS(path_t* path, char delimiter) {
  std::string result;

  for (size_t i = 0; i < path->identifiers.length; i++) {
    if (i > 0) {
      result += delimiter;
    }

    identifier_t* id = path->identifiers.data[i];

    // Extract identifier bytes to string
    for (size_t j = 0; j < id->chunks.length; j++) {
      chunk_t* chunk = id->chunks.data[j];
      const uint8_t* data = static_cast<const uint8_t*>(chunk_data_const(chunk));
      size_t size = chunk->data->size;

      // Check if all bytes are printable ASCII
      bool printable = true;
      for (size_t k = 0; k < size; k++) {
        if (!isprint(data[k]) && data[k] != '\t' && data[k] != '\n' && data[k] != '\r') {
          printable = false;
          break;
        }
      }

      if (printable) {
        result += std::string(reinterpret_cast<const char*>(data), size);
      } else {
        // Non-printable: represent as hex
        char hex[3];
        for (size_t k = 0; k < size; k++) {
          snprintf(hex, 3, "%02x", data[k]);
          result += hex;
        }
      }
    }
  }

  return result;
}

// Convert path_t* to JavaScript array
Napi::Array PathToArrayJS(Napi::Env env, path_t* path, char delimiter) {
  Napi::Array arr = Napi::Array::New(env);

  for (size_t i = 0; i < path->identifiers.length; i++) {
    identifier_t* id = path->identifiers.data[i];

    // Extract identifier as string
    std::string part;
    for (size_t j = 0; j < id->chunks.length; j++) {
      chunk_t* chunk = id->chunks.data[j];
      const uint8_t* data = static_cast<const uint8_t*>(chunk_data_const(chunk));
      size_t size = chunk->data->size;

      // Check if all bytes are printable ASCII
      bool printable = true;
      for (size_t k = 0; k < size; k++) {
        if (!isprint(data[k]) && data[k] != '\t' && data[k] != '\n' && data[k] != '\r') {
          printable = false;
          break;
        }
      }

      if (printable) {
        part += std::string(reinterpret_cast<const char*>(data), size);
      } else {
        // Non-printable: represent as hex
        char hex[3];
        for (size_t k = 0; k < size; k++) {
          snprintf(hex, 3, "%02x", data[k]);
          part += hex;
        }
      }
    }

    arr.Set(i, Napi::String::New(env, part));
  }

  return arr;
}