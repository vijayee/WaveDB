//
// GraphQL Schema - Type registration, SDL parsing, and storage
// Created: 2026-04-12
//

#ifndef WAVEDB_GRAPHQL_SCHEMA_H
#define WAVEDB_GRAPHQL_SCHEMA_H

#include "graphql_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Parse a GraphQL SDL string and register all types.
 *
 * Parses type definitions, enum definitions, and schema declarations.
 * Stores schema metadata in the database under __schema paths.
 * Each type's fields are stored as individual path/value pairs.
 *
 * @param layer      Layer to register schema with
 * @param sdl        SDL string to parse
 * @param error_out  If non-NULL and parsing fails, receives the error message.
 *                   Caller must free(). If NULL, the message is freed internally.
 * @return 0 on success, -1 on parse error, -2 on validation error
 */
int graphql_schema_parse(graphql_layer_t* layer, const char* sdl, char** error_out);

/**
 * Look up a type by name from the registry.
 *
 * @param registry  Type registry
 * @param name      Type name (e.g., "User")
 * @return Type definition or NULL if not found
 */
graphql_type_t* graphql_schema_get_type(graphql_layer_t* layer, const char* name);

/**
 * Get the plural path prefix for a type.
 *
 * Returns the type's custom plural name (from @plural directive)
 * or the default (type name + "s").
 *
 * @param type  Type definition
 * @return Plural name string (do not free, owned by type)
 */
const char* graphql_type_get_plural(graphql_type_t* type);

/**
 * Load schema from the database's __schema paths.
 *
 * Called during graphql_layer_create() when reopening an existing database.
 * Scans __schema/types/ paths and reconstructs graphql_type_t structs.
 *
 * @param layer  Layer to load schema into
 * @return 0 on success, -1 on error
 */
int graphql_schema_load(graphql_layer_t* layer);

/**
 * Store a type definition to the database.
 *
 * Writes each field's metadata as individual path/value pairs under
 * {PluralType}/__schema/fields/{fieldname}/...
 *
 * @param layer  Layer
 * @param type   Type definition to store
 * @return 0 on success, -1 on error
 */
int graphql_schema_store_type(graphql_layer_t* layer, graphql_type_t* type);

#ifdef __cplusplus
}
#endif

#endif // WAVEDB_GRAPHQL_SCHEMA_H