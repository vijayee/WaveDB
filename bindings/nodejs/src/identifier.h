#ifndef WAVEDB_BINDINGS_IDENTIFIER_H
#define WAVEDB_BINDINGS_IDENTIFIER_H

#include <napi.h>
#include <string>
#include "../../../src/HBTrie/identifier.h"

// Extract JS value (string or Buffer) into a std::string (no truncation).
// For Buffers: copies data into the string (safe for async/batch where data must outlive the call).
// Returns false on type error (throws JS exception).
bool ValueFromJSDynamic(Napi::Env env, Napi::Value value, std::string& out);

// Extract JS value (string or Buffer) with zero-copy for Buffers.
// For Buffers: sets val_buf to the Buffer's data pointer (valid only while V8 Buffer is alive).
// For Strings: writes into out_str (no truncation).
// Returns false on type error (throws JS exception).
// Use this for sync operations or when the C function copies data before returning.
bool ValueFromJSZeroCopy(Napi::Env env, Napi::Value value, std::string& out_str,
                         const uint8_t** val_buf, size_t* val_len);

// Convert identifier_t* to JavaScript value (string or Buffer)
Napi::Value ValueToJS(Napi::Env env, identifier_t* id);

#endif // WAVEDB_BINDINGS_IDENTIFIER_H