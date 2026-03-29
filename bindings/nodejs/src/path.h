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

#endif // WAVEDB_BINDINGS_PATH_H