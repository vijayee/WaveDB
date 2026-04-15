//
// GraphQL Layer - Public API
// Created: 2026-04-12
//
// This is the umbrella header for the WaveDB GraphQL layer.
// Include this header to access the full GraphQL API.
//

#ifndef WAVEDB_GRAPHQL_H
#define WAVEDB_GRAPHQL_H

#include "graphql_types.h"
#include "graphql_schema.h"
#include "graphql_result.h"
#include "graphql_plan.h"
#include "../../Workers/promise.h"
#include "graphql_resolve.h"

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================
// Layer lifecycle
// ============================================================

/**
 * Create a GraphQL layer with its own database.
 *
 * Creates a database_t with the given config, writes __meta metadata,
 * and prepares the layer for schema registration and queries.
 *
 * If opening an existing database, validates __meta to confirm it was
 * created by the GraphQL layer and loads the stored schema.
 *
 * @param path    Database storage path (NULL for in-memory)
 * @param config  Configuration (NULL for defaults)
 * @return New layer or NULL on failure
 */
graphql_layer_t* graphql_layer_create(const char* path,
                                       const graphql_layer_config_t* config);

/**
 * Destroy a GraphQL layer.
 *
 * Closes the database, frees all types and resources.
 * Dereferences the layer; actual free happens when refcount reaches 0.
 *
 * @param layer  Layer to destroy
 */
void graphql_layer_destroy(graphql_layer_t* layer);

/**
 * Parse a GraphQL SDL string and register types.
 *
 * Parses type definitions, enum definitions, and schema declarations.
 * Stores schema metadata in the database under __schema paths.
 *
 * @param layer      Layer to register schema with
 * @param sdl        SDL string to parse
 * @param error_out  If non-NULL and parsing fails, receives the error message.
 *                   Caller must free(). If NULL, the message is freed internally.
 * @return 0 on success, -1 on parse error, -2 on validation error
 */
int graphql_schema_parse(graphql_layer_t* layer, const char* sdl, char** error_out);

/**
 * Register a custom resolver for a type+field.
 *
 * When a field has a registered custom resolver, it is called instead
 * of the default path-based resolution.
 *
 * @param layer      Layer
 * @param type_name  Type name (e.g., "User")
 * @param field_name Field name (e.g., "fullName")
 * @param resolver   Resolver function
 * @param ctx        Context passed to resolver
 * @return 0 on success, -1 on error
 */
int graphql_register_resolver(graphql_layer_t* layer,
                                const char* type_name,
                                const char* field_name,
                                graphql_result_node_t* (*resolver)(
                                    graphql_layer_t* layer,
                                    graphql_field_t* field,
                                    path_t* parent_path,
                                    identifier_t* parent_value,
                                    const char* args_json,
                                    void* ctx),
                                void* ctx);

// ============================================================
// Query (async-first)
// ============================================================

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
 * Execute a GraphQL query synchronously.
 *
 * @param layer  Layer
 * @param query  GraphQL query string
 * @return Result (may contain partial data and errors)
 */
graphql_result_t* graphql_query_sync(graphql_layer_t* layer,
                                      const char* query);

// ============================================================
// Mutation (async-first)
// ============================================================

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

/**
 * Execute a GraphQL mutation synchronously.
 *
 * @param layer    Layer
 * @param mutation GraphQL mutation string
 * @return Result (may contain partial data and errors)
 */
graphql_result_t* graphql_mutate_sync(graphql_layer_t* layer,
                                       const char* mutation);

#ifdef __cplusplus
}
#endif

#endif // WAVEDB_GRAPHQL_H