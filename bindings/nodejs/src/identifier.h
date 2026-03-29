#ifndef WAVEDB_BINDINGS_IDENTIFIER_H
#define WAVEDB_BINDINGS_IDENTIFIER_H

#include <napi.h>
#include "../../../src/HBTrie/identifier.h"

// Convert JavaScript value (string or Buffer) to identifier_t*
identifier_t* ValueFromJS(Napi::Env env, Napi::Value value);

// Convert identifier_t* to JavaScript value (string or Buffer)
Napi::Value ValueToJS(Napi::Env env, identifier_t* id);

#endif // WAVEDB_BINDINGS_IDENTIFIER_H