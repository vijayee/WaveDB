//
// GraphQL Resolve - Query and mutation execution
// Created: 2026-04-12
//

#ifndef WAVEDB_GRAPHQL_RESOLVE_H
#define WAVEDB_GRAPHQL_RESOLVE_H

#include "graphql_types.h"

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
 * @param layer     Layer with registered schema
 * @param query     GraphQL query string
 * @param user_data Context passed to async callbacks
 * @return Result (caller must destroy)
 */
graphql_result_t* graphql_query(graphql_layer_t* layer,
                                 const char* query,
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
 * @param layer     Layer with registered schema
 * @param mutation  GraphQL mutation string
 * @param user_data Context passed to async callbacks
 * @return Result (caller must destroy)
 */
graphql_result_t* graphql_mutate(graphql_layer_t* layer,
                                   const char* mutation,
                                   void* user_data);

#ifdef __cplusplus
}
#endif

#endif // WAVEDB_GRAPHQL_RESOLVE_H