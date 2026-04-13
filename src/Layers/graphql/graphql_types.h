//
// GraphQL Layer Core Types
// Created: 2026-04-12
//

#ifndef WAVEDB_GRAPHQL_TYPES_H
#define WAVEDB_GRAPHQL_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "../../RefCounter/refcounter.h"
#include "../../Util/vec.h"
#include "../../Database/database.h"

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================
// Forward declarations
// ============================================================

typedef struct graphql_layer_t graphql_layer_t;
typedef struct graphql_type_t graphql_type_t;
typedef struct graphql_field_t graphql_field_t;
typedef struct graphql_type_ref_t graphql_type_ref_t;
typedef struct graphql_directive_t graphql_directive_t;
typedef struct graphql_result_node_t graphql_result_node_t;
typedef struct graphql_result_t graphql_result_t;
typedef struct graphql_error_t graphql_error_t;
typedef struct graphql_location_t graphql_location_t;
typedef struct graphql_plan_t graphql_plan_t;
typedef struct graphql_arg_t graphql_arg_t;
typedef struct graphql_literal_t graphql_literal_t;
typedef struct graphql_object_field_t graphql_object_field_t;

// ============================================================
// Type kinds
// ============================================================

typedef enum {
    GRAPHQL_TYPE_SCALAR,     // String, Int, Float, Boolean, ID
    GRAPHQL_TYPE_OBJECT,    // User, Post, etc.
    GRAPHQL_TYPE_LIST,      // [User], [String]
    GRAPHQL_TYPE_ENUM,      // enum Role { ADMIN, USER }
    GRAPHQL_TYPE_NON_NULL,  // Type! (wrapping type)
    GRAPHQL_TYPE_INPUT,     // Input object type (v2)
    GRAPHQL_TYPE_INTERFACE, // Interface type (v2)
    GRAPHQL_TYPE_UNION,     // Union type (v2)
} graphql_type_kind_t;

// ============================================================
// Literal value kinds (for arguments and input values)
// ============================================================

typedef enum {
    GRAPHQL_LITERAL_NULL,
    GRAPHQL_LITERAL_STRING,
    GRAPHQL_LITERAL_INT,
    GRAPHQL_LITERAL_FLOAT,
    GRAPHQL_LITERAL_BOOL,
    GRAPHQL_LITERAL_ENUM,
    GRAPHQL_LITERAL_OBJECT,   // { key: value, ... }
    GRAPHQL_LITERAL_LIST,     // [value, ... ]
} graphql_literal_kind_t;

// Key-value pair in an object literal
struct graphql_object_field_t {
    char* name;
    struct graphql_literal_t* value;
};

// Literal value (string, int, float, bool, null, enum, object, list)
struct graphql_literal_t {
    graphql_literal_kind_t kind;
    union {
        char* string_val;
        int64_t int_val;
        double float_val;
        bool bool_val;
        vec_t(struct graphql_object_field_t*) object_fields;  // For OBJECT
        vec_t(struct graphql_literal_t*) list_items;            // For LIST
    };
};

// ============================================================
// Type reference (handles named types, lists, and non-null)
// ============================================================

typedef struct graphql_type_ref_t {
    graphql_type_kind_t kind;                // Wrapping kind. For named types this is the underlying type's kind (SCALAR, OBJECT, ENUM). For wrappers this is LIST or NON_NULL.
    char* name;                              // For named types: "User", "String". NULL for LIST/NON_NULL wrappers.
    struct graphql_type_ref_t* of_type;      // Inner type for LIST/NON_NULL wrappers. NULL for leaf named types.
} graphql_type_ref_t;

// ============================================================
// Directive (@skip, @include, @plural)
// ============================================================

typedef struct graphql_directive_t {
    char* name;                              // "skip", "include", "plural"
    vec_t(char*) arg_names;                  // Argument names
    vec_t(char*) arg_values;                 // Argument values (as strings)
} graphql_directive_t;

// ============================================================
// Field definition
// ============================================================

typedef struct graphql_field_t {
    char* name;                              // "name", "friends", etc.
    graphql_type_ref_t* type;               // Type reference for this field
    bool is_required;                        // Non-null (!) modifier
    graphql_literal_t* default_value;        // Default value (NULL if none)
    vec_t(graphql_directive_t*) directives;  // Directives on this field
    // Custom resolver (NULL = auto-resolve)
    struct graphql_result_node_t* (*resolver)(
        struct graphql_layer_t* layer,
        struct graphql_field_t* field,
        path_t* parent_path,
        identifier_t* parent_value,
        const char* args_json,
        void* ctx);
    void* resolver_ctx;                      // Context for custom resolver
} graphql_field_t;

// ============================================================
// Type definition
// ============================================================

typedef struct graphql_type_t {
    char* name;                              // "User", "Post", "String"
    graphql_type_kind_t kind;               // OBJECT, SCALAR, ENUM, etc.
    vec_t(graphql_field_t*) fields;         // Object type fields
    vec_t(char*) enum_values;               // Enum type values
    char* plural_name;                      // Pluralized name for path prefix (NULL = name + "s")
    vec_t(graphql_directive_t*) directives; // Type-level directives
} graphql_type_t;

// ============================================================
// Type registry
// ============================================================

typedef struct graphql_type_registry_t {
    vec_t(graphql_type_t*) types;            // All registered types
} graphql_type_registry_t;

// ============================================================
// Result types
// ============================================================

typedef enum {
    RESULT_NULL,
    RESULT_STRING,
    RESULT_INT,
    RESULT_FLOAT,
    RESULT_BOOL,
    RESULT_LIST,
    RESULT_OBJECT,
    RESULT_ID,
    RESULT_REF,
} graphql_result_kind_t;

typedef struct graphql_result_node_t {
    graphql_result_kind_t kind;
    char* name;                              // Field name (in objects)
    union {
        char* string_val;
        int64_t int_val;
        double float_val;
        bool bool_val;
        char* id_val;
        vec_t(struct graphql_result_node_t*) children;  // For LIST and OBJECT
    };
} graphql_result_node_t;

typedef struct graphql_location_t {
    size_t line;
    size_t column;
} graphql_location_t;

typedef struct graphql_error_t {
    char* message;
    vec_t(graphql_location_t) locations;
    char* path;                              // JSON path to error (e.g., "user.friends[1].name")
} graphql_error_t;

typedef struct graphql_result_t {
    graphql_result_node_t* data;             // Root result node (may be partial on error)
    vec_t(graphql_error_t) errors;         // Collected errors
    bool success;
} graphql_result_t;

// ============================================================
// Plan types
// ============================================================

typedef enum {
    PLAN_SCAN,              // Scan a path range
    PLAN_GET,               // Get a single path value
    PLAN_BATCH_GET,         // Get multiple specific paths
    PLAN_RESOLVE_FIELD,     // Resolve a single field from parent object
    PLAN_RESOLVE_REF,       // Follow a reference to another type
    PLAN_FILTER,            // Apply @skip/@include
    PLAN_CUSTOM,            // Call custom resolver
} graphql_plan_kind_t;

typedef struct graphql_arg_t {
    char* name;
    char* value;                            // String representation of argument value
} graphql_arg_t;

typedef struct graphql_plan_t {
    graphql_plan_kind_t kind;
    char* type_name;                        // "User", "Post" — which type this operates on
    char* field_name;                       // "name", "age" — original field name from the query
    char* alias;                             // Field alias (NULL if none, e.g. "admin" in "admin: user")
    path_t* base_path;                      // e.g., path("Users/1")
    path_t* scan_start;                     // For SCAN: start of range
    path_t* scan_end;                       // For SCAN: end of range
    path_t* get_path;                       // For GET: exact path to fetch
    char* ref_type;                         // For RESOLVE_REF: referenced type name
    identifier_t* ref_id;                   // For RESOLVE_REF: referenced ID
    vec_t(graphql_arg_t) args;             // Arguments
    bool skip;                               // @skip condition result
    bool include;                            // @include condition result
    struct graphql_result_node_t* (*resolver)(
        struct graphql_layer_t* layer,
        struct graphql_field_t* field,
        path_t* parent_path,
        identifier_t* parent_value,
        const char* args_json,
        void* ctx);
    void* resolver_ctx;
    vec_t(struct graphql_plan_t*) children; // Sub-plans (nested field resolution)
    struct graphql_result_node_t* result;   // Result destination
} graphql_plan_t;

// ============================================================
// Layer configuration
// ============================================================

typedef struct graphql_layer_config_t {
    const char* path;                        // Database storage path (NULL = in-memory)
    uint8_t chunk_size;                      // HBTrie chunk size (0 = default)
    uint32_t btree_node_size;               // B+tree node size (0 = default)
    size_t lru_memory_mb;                   // LRU cache size in MB (0 = default)
    uint16_t lru_shards;                    // LRU shards (0 = auto-scale)
    uint8_t worker_threads;                  // Worker threads (0 = default)
    uint8_t enable_persist;                  // 0 = in-memory, 1 = persistent
    wal_config_t wal_config;                // WAL configuration
    char delimiter;                          // Path delimiter (default '/')
} graphql_layer_config_t;

// ============================================================
// Layer struct
// ============================================================

struct graphql_layer_t {
    refcounter_t refcounter;                // MUST be first member
    database_t* db;                          // Owned by the layer
    graphql_type_registry_t* registry;       // In-memory type definitions
    work_pool_t* pool;                       // Reuses db's worker pool
    char delimiter;                           // Path delimiter (default '/')
    char* version;                           // Layer version string
    char* db_path;                           // Database storage path (owned if auto-generated)
    bool owns_db_path;                       // True if db_path was auto-generated and should be freed
    char* query_type;                        // Root query type name (e.g., "Query")
    char* mutation_type;                     // Root mutation type name (e.g., "Mutation")
};

// ============================================================
// Default config
// ============================================================

#define GRAPHQL_LAYER_VERSION "1.0.0"
#define GRAPHQL_LAYER_DEFAULT_DELIMITER '/'

/**
 * Create default configuration for the GraphQL layer.
 *
 * Returns a config with all defaults set. Caller must destroy.
 *
 * @return New config or NULL on failure
 */
graphql_layer_config_t* graphql_layer_config_default(void);

/**
 * Destroy a configuration.
 *
 * @param config  Config to destroy
 */
void graphql_layer_config_destroy(graphql_layer_config_t* config);

// ============================================================
// Type reference functions
// ============================================================

/**
 * Create a named type reference (e.g., "User", "String").
 */
graphql_type_ref_t* graphql_type_ref_create_named(graphql_type_kind_t kind, const char* name);

/**
 * Create a list type reference (e.g., [User]).
 */
graphql_type_ref_t* graphql_type_ref_create_list(graphql_type_ref_t* inner);

/**
 * Create a non-null type reference (e.g., User!).
 */
graphql_type_ref_t* graphql_type_ref_create_non_null(graphql_type_ref_t* inner);

/**
 * Destroy a type reference.
 */
void graphql_type_ref_destroy(graphql_type_ref_t* ref);

// ============================================================
// Field functions
// ============================================================

/**
 * Create a field definition.
 */
graphql_field_t* graphql_field_create(const char* name,
                                       graphql_type_ref_t* type,
                                       bool is_required);

/**
 * Destroy a field definition.
 */
void graphql_field_destroy(graphql_field_t* field);

// ============================================================
// Literal functions
// ============================================================

/**
 * Destroy a literal value.
 */
void graphql_literal_destroy(graphql_literal_t* literal);

/**
 * Deep-copy a literal value.
 */
graphql_literal_t* graphql_literal_copy(const graphql_literal_t* literal);

// ============================================================
// Type functions
// ============================================================

/**
 * Create a type definition.
 */
graphql_type_t* graphql_type_create(const char* name, graphql_type_kind_t kind);

/**
 * Destroy a type definition.
 */
void graphql_type_destroy(graphql_type_t* type);

// ============================================================
// Result node functions
// ============================================================

/**
 * Create a result node.
 */
graphql_result_node_t* graphql_result_node_create(graphql_result_kind_t kind, const char* name);

/**
 * Destroy a result node (recursively destroys children).
 */
void graphql_result_node_destroy(graphql_result_node_t* node);

/**
 * Destroy a result (destroys data and errors).
 */
void graphql_result_destroy(graphql_result_t* result);

// ============================================================
// Error functions
// ============================================================

/**
 * Create an error.
 */
graphql_error_t graphql_error_create(const char* message, const char* path);

/**
 * Destroy error contents (does not free the error struct itself).
 */
void graphql_error_contents_destroy(graphql_error_t* error);

// ============================================================
// Plan functions
// ============================================================

/**
 * Create a plan node.
 */
graphql_plan_t* graphql_plan_create(graphql_plan_kind_t kind);

/**
 * Destroy a plan (recursively destroys children).
 */
void graphql_plan_destroy(graphql_plan_t* plan);

// ============================================================
// Directive functions
// ============================================================

/**
 * Create a directive.
 */
graphql_directive_t* graphql_directive_create(const char* name);

/**
 * Destroy a directive.
 */
void graphql_directive_destroy(graphql_directive_t* directive);

// ============================================================
// Type registry functions
// ============================================================

/**
 * Create a type registry.
 */
graphql_type_registry_t* graphql_type_registry_create(void);

/**
 * Destroy a type registry (destroys all registered types).
 */
void graphql_type_registry_destroy(graphql_type_registry_t* registry);

/**
 * Register a type in the registry.
 */
int graphql_type_registry_register(graphql_type_registry_t* registry, graphql_type_t* type);

/**
 * Look up a type by name. Returns NULL if not found.
 */
graphql_type_t* graphql_type_registry_get(graphql_type_registry_t* registry, const char* name);

#ifdef __cplusplus
}
#endif

#endif // WAVEDB_GRAPHQL_TYPES_H