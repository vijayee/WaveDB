//
// GraphQL Plan - Query plan compilation
// Created: 2026-04-12
//

#ifndef WAVEDB_GRAPHQL_PLAN_H
#define WAVEDB_GRAPHQL_PLAN_H

#include "graphql_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Compile a GraphQL query string into an execution plan.
 *
 * Walks the query AST, resolves types against the schema registry,
 * and produces a graphql_plan_t tree with computed paths.
 *
 * @param layer  Layer with registered schema
 * @param query  GraphQL query string
 * @return Compiled plan or NULL on error
 */
graphql_plan_t* graphql_compile_query(graphql_layer_t* layer,
                                       const char* query);

/**
 * Compile a GraphQL mutation string into an execution plan.
 *
 * @param layer    Layer with registered schema
 * @param mutation  GraphQL mutation string
 * @return Compiled plan or NULL on error
 */
graphql_plan_t* graphql_compile_mutation(graphql_layer_t* layer,
                                          const char* mutation);

/**
 * Destroy a plan (recursively destroys children).
 *
 * @param plan  Plan to destroy
 */
void graphql_plan_destroy(graphql_plan_t* plan);

/**
 * Convert a plan to a human-readable string for debugging.
 *
 * @param plan  Plan to stringify
 * @return Heap-allocated string. Caller must free().
 */
char* graphql_plan_to_string(graphql_plan_t* plan);

#ifdef __cplusplus
}
#endif

#endif // WAVEDB_GRAPHQL_PLAN_H