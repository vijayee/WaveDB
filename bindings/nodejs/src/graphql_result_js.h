#ifndef WAVEDB_BINDINGS_GRAPHQL_RESULT_JS_H
#define WAVEDB_BINDINGS_GRAPHQL_RESULT_JS_H

#include <napi.h>
#include "../../../src/Layers/graphql/graphql.h"

// Convert a graphql_result_t to a JS object: { success, data, errors }
Napi::Value GraphQLResultToJS(Napi::Env env, graphql_result_t* result);

// Convert a graphql_result_node_t to a JS value
Napi::Value GraphQLResultNodeToJS(Napi::Env env, graphql_result_node_t* node);

#endif // WAVEDB_BINDINGS_GRAPHQL_RESULT_JS_H