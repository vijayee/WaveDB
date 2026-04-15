//
// GraphQL Result - Result construction and JSON serialization
// Created: 2026-04-12
//

#ifndef WAVEDB_GRAPHQL_RESULT_H
#define WAVEDB_GRAPHQL_RESULT_H

#include "graphql_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Create a result node.
 *
 * @param kind  Result kind (STRING, INT, OBJECT, LIST, etc.)
 * @param name  Field name (may be NULL for list items)
 * @return New result node or NULL on failure
 */
graphql_result_node_t* graphql_result_node_create(graphql_result_kind_t kind,
                                                    const char* name);

/**
 * Destroy a result node (recursively destroys children).
 *
 * @param node  Result node to destroy
 */
void graphql_result_node_destroy(graphql_result_node_t* node);

/**
 * Destroy a result (destroys data and errors).
 *
 * @param result  Result to destroy
 */
void graphql_result_destroy(graphql_result_t* result);

/**
 * Convert a result to a JSON string.
 *
 * Produces standard GraphQL response format:
 * { "data": { ... }, "errors": [ ... ] }
 *
 * @param result  Result to serialize
 * @return Heap-allocated JSON string. Caller must free().
 */
const char* graphql_result_to_json(graphql_result_t* result);

/**
 * Add a child node to an OBJECT or LIST result node.
 *
 * @param parent  Parent node (must be OBJECT or LIST)
 * @param child   Child node to add
 * @return 0 on success, -1 on error
 */
int graphql_result_node_add_child(graphql_result_node_t* parent,
                                    graphql_result_node_t* child);

#ifdef __cplusplus
}
#endif

#endif // WAVEDB_GRAPHQL_RESULT_H