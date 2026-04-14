//
// GraphQL Plan - Query plan compilation
// Created: 2026-04-12
//

#ifndef WAVEDB_GRAPHQL_PLAN_H
#define WAVEDB_GRAPHQL_PLAN_H

#include "graphql_types.h"

// Forward declaration to avoid circular include with graphql_parser.h
typedef struct graphql_ast_node_t graphql_ast_node_t;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Compile a GraphQL query string into an execution plan.
 *
 * Walks the query AST, resolves types against the schema registry,
 * and produces a graphql_plan_t tree with computed paths.
 *
 * @param layer               Layer with registered schema
 * @param query               GraphQL query string
 * @param error_out           If non-NULL and parsing fails, receives the error message.
 *                            Caller must free(). If NULL, the message is freed internally.
 * @param compilation_errors  If non-NULL, field validation errors are pushed here.
 *                            Caller must destroy each error via graphql_error_contents_destroy.
 * @return Compiled plan or NULL on error
 */
graphql_plan_t* graphql_compile_query(graphql_layer_t* layer,
                                       const char* query,
                                       char** error_out,
                                       graphql_compilation_errors_t* compilation_errors);

/**
 * Compile a GraphQL mutation string into an execution plan.
 *
 * @param layer               Layer with registered schema
 * @param mutation            GraphQL mutation string
 * @param error_out           If non-NULL and parsing fails, receives the error message.
 *                            Caller must free(). If NULL, the message is freed internally.
 * @param compilation_errors  If non-NULL, field validation errors are pushed here.
 *                            Caller must destroy each error via graphql_error_contents_destroy.
 * @return Compiled plan or NULL on error
 */
graphql_plan_t* graphql_compile_mutation(graphql_layer_t* layer,
                                          const char* mutation,
                                          char** error_out,
                                          graphql_compilation_errors_t* compilation_errors);

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

/**
 * Compile the selection set of a mutation field into sub-plans.
 *
 * Used to resolve the requested fields on a created/updated entity.
 *
 * @param layer       Layer with registered schema
 * @param type_name   The type name (e.g., "User")
 * @param field       AST field node from the mutation (contains children as selections)
 * @return Plan with compiled child selections, or NULL if no selections
 */
graphql_plan_t* graphql_compile_mutation_selection(graphql_layer_t* layer,
                                                    const char* type_name,
                                                    graphql_ast_node_t* field);

#ifdef __cplusplus
}
#endif

#endif // WAVEDB_GRAPHQL_PLAN_H