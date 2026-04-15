//
// GraphQL Resolve - Query and mutation execution
// Created: 2026-04-12
//

#ifndef WAVEDB_GRAPHQL_RESOLVE_H
#define WAVEDB_GRAPHQL_RESOLVE_H

#include "graphql_types.h"
#include "../../Workers/promise.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Execute a GraphQL query synchronously.
 *
 * Compiles the query into a plan, executes it against the database,
 * and returns the result.
 *
 * @param layer  Layer with registered schema
 * @param query  GraphQL query string
 * @return Result (caller must destroy with graphql_result_destroy)
 */
graphql_result_t* graphql_query_sync(graphql_layer_t* layer,
                                      const char* query);

/**
 * Execute a GraphQL query asynchronously.
 *
 * Dispatches query compilation and execution to the worker pool.
 * The result is delivered via the promise's resolve callback.
 * If no pool is available, executes synchronously and resolves inline.
 *
 * @param layer     Layer with registered schema
 * @param query     GraphQL query string (copied internally)
 * @param promise   Promise to resolve/reject with the result
 * @param user_data Context passed through (currently unused)
 */
void graphql_query(graphql_layer_t* layer,
                   const char* query,
                   promise_t* promise,
                   void* user_data);

/**
 * Execute a GraphQL mutation synchronously.
 *
 * @param layer     Layer with registered schema
 * @param mutation  GraphQL mutation string
 * @return Result (caller must destroy)
 */
graphql_result_t* graphql_mutate_sync(graphql_layer_t* layer,
                                       const char* mutation);

/**
 * Execute a GraphQL mutation asynchronously.
 *
 * Dispatches mutation execution to the worker pool.
 * The result is delivered via the promise's resolve callback.
 * If no pool is available, executes synchronously and resolves inline.
 *
 * @param layer     Layer with registered schema
 * @param mutation  GraphQL mutation string (copied internally)
 * @param promise   Promise to resolve/reject with the result
 * @param user_data Context passed through (currently unused)
 */
void graphql_mutate(graphql_layer_t* layer,
                    const char* mutation,
                    promise_t* promise,
                    void* user_data);

#ifdef __cplusplus
}
#endif

#endif // WAVEDB_GRAPHQL_RESOLVE_H