#ifndef WAVEDB_BINDINGS_PATH_H
#define WAVEDB_BINDINGS_PATH_H

#include <napi.h>
#include <string>
#include <vector>
#include "../../../src/HBTrie/path.h"

// Split string by delimiter
std::vector<std::string> SplitString(const std::string& str, char delimiter);

// Convert JavaScript key (string or array) to path_t*
path_t* PathFromJS(Napi::Env env, Napi::Value key, char delimiter);

// Convert path_t* to JavaScript string
std::string PathToJS(path_t* path, char delimiter);

// Convert path_t* to JavaScript array
Napi::Array PathToArrayJS(Napi::Env env, path_t* path, char delimiter);

// Create path from vector of string parts
path_t* PathFromParts(const std::vector<std::string>& parts);

// Extract JS string key into caller-provided buffer.
// Returns false on type error (throws JS exception).
bool KeyFromJS(Napi::Env env, Napi::Value key, char* buf, size_t buf_size, size_t* out_len);

#endif // WAVEDB_BINDINGS_PATH_H