#ifndef WAVEDB_BINDINGS_IDENTIFIER_H
#define WAVEDB_BINDINGS_IDENTIFIER_H

#include <napi.h>
#include "../../../src/HBTrie/identifier.h"

// Convert JavaScript value (string or Buffer) to identifier_t*
identifier_t* ValueFromJS(Napi::Env env, Napi::Value value);

// Convert identifier_t* to JavaScript value (string or Buffer)
Napi::Value ValueToJS(Napi::Env env, identifier_t* id);

// Extract JS value (string or Buffer) into caller-provided buffers.
// For strings: writes into str_buf, sets val_buf/val_len.
// For Buffers: sets val_buf to the Buffer's data pointer (zero-copy, valid during call).
// Returns false on type error.
bool ValueFromJSRaw(Napi::Env env, Napi::Value value, char* str_buf, size_t str_buf_size,
                    const uint8_t** val_buf, size_t* val_len);

#endif // WAVEDB_BINDINGS_IDENTIFIER_H