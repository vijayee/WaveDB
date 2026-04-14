//
// GraphQL Schema - Type registration, SDL parsing, and storage
// Created: 2026-04-12
//

#include "graphql_schema.h"
#include "graphql_parser.h"
#include "graphql_lexer.h"
#include "../../Util/allocator.h"
#include "../../Util/log.h"
#include "../../HBTrie/path.h"
#include "../../HBTrie/identifier.h"
#include "../../HBTrie/chunk.h"
#include "../../Database/database.h"
#include "../../Database/database_iterator.h"
#include "../../Database/batch.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

// ============================================================
// Forward declarations for internal helpers
// ============================================================

static graphql_type_t* load_type_from_database(graphql_layer_t* layer, const char* type_name);
static int convert_ast_to_types(graphql_layer_t* layer, graphql_ast_node_t* doc);
static graphql_type_t* convert_type_definition(graphql_ast_node_t* node);
static graphql_type_t* convert_enum_definition(graphql_ast_node_t* node);
static int convert_schema_definition(graphql_layer_t* layer, graphql_ast_node_t* node);
// Helper: resolve the base type name from a type reference
// For named types, returns the name directly
// For LIST/NON_NULL wrappers, unwraps to find the inner named type
static const char* resolve_base_type_name(graphql_type_ref_t* type_ref) {
    if (type_ref == NULL) return NULL;
    while (type_ref->of_type != NULL) {
        type_ref = type_ref->of_type;
    }
    return type_ref->name;
}

// Helper: check if a type reference is a list type (at any nesting level)
static bool is_list_type(graphql_type_ref_t* type_ref) {
    if (type_ref == NULL) return false;
    if (type_ref->kind == GRAPHQL_TYPE_LIST) return true;
    return is_list_type(type_ref->of_type);
}

// Helper: parse a type-prefixed default value string back to a graphql_literal_t
// Format: s:hello, i:42, f:3.14, b:true, n:null, e:ENUMVAL, o:{...}, l:[...]
static graphql_literal_t* parse_default_value(const char* str) {
    if (str == NULL || str[0] == '\0' || str[1] != ':') return NULL;

    graphql_literal_t* lit = get_clear_memory(sizeof(graphql_literal_t));
    if (lit == NULL) return NULL;

    const char* val = str + 2;
    switch (str[0]) {
        case 's':
            lit->kind = GRAPHQL_LITERAL_STRING;
            lit->string_val = strdup(val);
            break;
        case 'i':
            lit->kind = GRAPHQL_LITERAL_INT;
            lit->int_val = strtoll(val, NULL, 10);
            break;
        case 'f':
            lit->kind = GRAPHQL_LITERAL_FLOAT;
            lit->float_val = strtod(val, NULL);
            break;
        case 'b':
            lit->kind = GRAPHQL_LITERAL_BOOL;
            lit->bool_val = (strcmp(val, "true") == 0);
            break;
        case 'n':
            lit->kind = GRAPHQL_LITERAL_NULL;
            break;
        case 'e':
            lit->kind = GRAPHQL_LITERAL_ENUM;
            lit->string_val = strdup(val);
            break;
        case 'o':
            // Object default: stored as o:{key: value, ...}
            // Parse back into object fields — for now store as string representation
            // Full round-trip requires recursive parsing which is complex;
            // store the raw string for reconstruction on load
            lit->kind = GRAPHQL_LITERAL_STRING;
            lit->string_val = strdup(val);
            break;
        case 'l':
            // List default: stored as l:[item, ...]
            // Store the raw string for reconstruction on load
            lit->kind = GRAPHQL_LITERAL_STRING;
            lit->string_val = strdup(val);
            break;
        default:
            free(lit);
            return NULL;
    }
    return lit;
}

// Simple pluralization: add 's' to the name

// Helper: write a path/value pair to the database
static int db_put_string(database_t* db, const char* path_str, const char* value) {
    path_t* path = path_create();
    if (path == NULL) return -1;

    // Parse path string by delimiter '/'
    const char* start = path_str;
    while (*start) {
        const char* end = strchr(start, '/');
        size_t len = end ? (size_t)(end - start) : strlen(start);
        if (len > 0) {
            buffer_t* buf = buffer_create(len);
            if (buf == NULL) {
                path_destroy(path);
                return -1;
            }
            memcpy(buf->data, start, len);
            buf->size = len;
            identifier_t* id = identifier_create(buf, 0);
            buffer_destroy(buf);
            if (id == NULL) {
                path_destroy(path);
                return -1;
            }
            path_append(path, id);
            identifier_destroy(id);
        }
        if (end) {
            start = end + 1;
        } else {
            break;
        }
    }

    buffer_t* val_buf = buffer_create(strlen(value));
    if (val_buf == NULL) {
        path_destroy(path);
        return -1;
    }
    memcpy(val_buf->data, value, strlen(value));
    val_buf->size = strlen(value);
    identifier_t* val_id = identifier_create(val_buf, 0);
    buffer_destroy(val_buf);
    if (val_id == NULL) {
        path_destroy(path);
        return -1;
    }

    // database_put_sync takes ownership of both path and val_id
    return database_put_sync(db, path, val_id);
}

// Helper: read a path/value pair from the database
static char* db_get_string(database_t* db, const char* path_str) {
    path_t* path = path_create();
    if (path == NULL) return NULL;

    const char* start = path_str;
    while (*start) {
        const char* end = strchr(start, '/');
        size_t len = end ? (size_t)(end - start) : strlen(start);
        if (len > 0) {
            buffer_t* buf = buffer_create(len);
            if (buf == NULL) {
                path_destroy(path);
                return NULL;
            }
            memcpy(buf->data, start, len);
            buf->size = len;
            identifier_t* id = identifier_create(buf, 0);
            buffer_destroy(buf);
            if (id == NULL) {
                path_destroy(path);
                return NULL;
            }
            path_append(path, id);
            identifier_destroy(id);
        }
        if (end) {
            start = end + 1;
        } else {
            break;
        }
    }

    identifier_t* value = NULL;
    int rc = database_get_sync(db, path, &value);
    // path is consumed by database_get_sync, do not destroy it
    if (rc != 0) return NULL;

    buffer_t* buf = identifier_to_buffer(value);
    identifier_destroy(value);
    if (buf == NULL) return NULL;

    char* result = malloc(buf->size + 1);
    if (result == NULL) {
        buffer_destroy(buf);
        return NULL;
    }
    memcpy(result, buf->data, buf->size);
    result[buf->size] = '\0';
    buffer_destroy(buf);
    return result;
}

// Simple pluralization: add 's' to the name
static char* make_plural(const char* name) {
    if (name == NULL) return NULL;
    size_t len = strlen(name);
    char* plural = malloc(len + 2);
    if (plural == NULL) return NULL;
    memcpy(plural, name, len);
    plural[len] = 's';
    plural[len + 1] = '\0';
    return plural;
}

// ============================================================
// Layer lifecycle
// ============================================================

graphql_layer_t* graphql_layer_create(const char* path,
                                       const graphql_layer_config_t* config) {
    // Create config with defaults if not provided
    graphql_layer_config_t* cfg = config ? NULL : graphql_layer_config_default();
    if (config == NULL && cfg == NULL) return NULL;

    const graphql_layer_config_t* active_config = config ? config : cfg;

    // Database requires a path — if NULL, create a temp directory
    char* db_path = NULL;
    bool owns_path = false;
    if (path != NULL) {
        db_path = (char*)path;
    } else {
        // Generate a temp directory for in-memory mode
        db_path = malloc(256);
        if (db_path == NULL) {
            graphql_layer_config_destroy(cfg);
            return NULL;
        }
        snprintf(db_path, 256, "/tmp/wavedb_graphql_XXXXXX");
        if (mkdtemp(db_path) == NULL) {
            free(db_path);
            graphql_layer_config_destroy(cfg);
            return NULL;
        }
        owns_path = true;
    }

    // Create database configuration
    database_config_t* db_config = database_config_default();
    if (db_config == NULL) {
        if (owns_path) free(db_path);
        graphql_layer_config_destroy(cfg);
        return NULL;
    }

    db_config->chunk_size = active_config->chunk_size;
    db_config->btree_node_size = active_config->btree_node_size;
    db_config->lru_memory_mb = active_config->lru_memory_mb;
    db_config->lru_shards = active_config->lru_shards;
    db_config->worker_threads = active_config->worker_threads;
    db_config->enable_persist = active_config->enable_persist;
    // WAL config is copied separately if needed

    // Create the database
    int error_code = 0;
    database_t* db = database_create_with_config(db_path, db_config, &error_code);
    database_config_destroy(db_config);

    if (db == NULL) {
        if (owns_path) {
            // Clean up temp directory
            rmdir(db_path);
            free(db_path);
        }
        graphql_layer_config_destroy(cfg);
        return NULL;
    }

    // Create the layer
    graphql_layer_t* layer = get_clear_memory(sizeof(graphql_layer_t));
    if (layer == NULL) {
        database_destroy(db);
        if (owns_path) { rmdir(db_path); free(db_path); }
        else { free(db_path); }
        graphql_layer_config_destroy(cfg);
        return NULL;
    }

    refcounter_init((refcounter_t*)layer);
    layer->db = db;
    layer->registry = graphql_type_registry_create();
    layer->pool = db->pool;
    layer->delimiter = active_config->delimiter ? active_config->delimiter : '/';
    layer->version = strdup(GRAPHQL_LAYER_VERSION);
    layer->db_path = owns_path ? db_path : (path ? strdup(path) : NULL);
    layer->owns_db_path = owns_path;

    if (layer->registry == NULL || layer->version == NULL) {
        if (layer->registry) graphql_type_registry_destroy(layer->registry);
        free(layer->version);
        free(layer->db_path);
        refcounter_destroy_lock((refcounter_t*)layer);
        database_destroy(db);
        free(layer);
        graphql_layer_config_destroy(cfg);
        return NULL;
    }

    // Check if this is a new database or an existing one
    char* existing_layer = db_get_string(db, "__meta/layer");
    if (existing_layer != NULL) {
        // Existing database - validate and load schema
        char* existing_version = db_get_string(db, "__meta/version");
        if (existing_version != NULL) {
            // Version compatibility check
            if (strcmp(existing_version, GRAPHQL_LAYER_VERSION) != 0) {
                log_warn("GraphQL layer version mismatch: stored=%s, current=%s",
                         existing_version, GRAPHQL_LAYER_VERSION);
            }
            free(existing_version);
        }
        free(existing_layer);
        graphql_schema_load(layer);
    } else {
        // New database - write metadata
        db_put_string(db, "__meta/layer", "graphql");
        db_put_string(db, "__meta/version", GRAPHQL_LAYER_VERSION);
        // Write creation timestamp
        time_t now = time(NULL);
        char ts_buf[64];
        snprintf(ts_buf, sizeof(ts_buf), "%ld", (long)now);
        db_put_string(db, "__meta/created", ts_buf);
    }

    graphql_layer_config_destroy(cfg);
    return layer;
}

void graphql_layer_destroy(graphql_layer_t* layer) {
    if (layer == NULL) return;
    refcounter_dereference((refcounter_t*)layer);
    if (refcounter_count((refcounter_t*)layer) == 0) {
        if (layer->registry) graphql_type_registry_destroy(layer->registry);
        if (layer->db) database_destroy(layer->db);
        if (layer->db_path) {
            if (layer->owns_db_path) {
                // Clean up auto-generated temp directory
                rmdir(layer->db_path);
            }
            free(layer->db_path);
        }
        free(layer->version);
        free(layer->query_type);
        free(layer->mutation_type);
        refcounter_destroy_lock((refcounter_t*)layer);
        free(layer);
    }
}

// ============================================================
// Schema parsing (SDL -> types -> database)
// ============================================================

int graphql_schema_parse(graphql_layer_t* layer, const char* sdl, char** error_out) {
    if (layer == NULL || sdl == NULL) {
        if (error_out != NULL) *error_out = strdup("Invalid arguments");
        return -1;
    }

    // Parse SDL into AST
    graphql_ast_node_t* ast = graphql_parse(sdl, strlen(sdl), error_out);
    if (ast == NULL) return -2;

    // Convert AST to type definitions
    int result = convert_ast_to_types(layer, ast);
    graphql_ast_destroy(ast);

    if (result != 0) return result;

    // Store all types to the database
    for (int i = 0; i < layer->registry->types.length; i++) {
        graphql_type_t* type = layer->registry->types.data[i];
        if (graphql_schema_store_type(layer, type) != 0) {
            return -1;
        }
    }

    return 0;
}

graphql_type_t* graphql_schema_get_type(graphql_layer_t* layer, const char* name) {
    if (layer == NULL || layer->registry == NULL || name == NULL) return NULL;
    return graphql_type_registry_get(layer->registry, name);
}

// ============================================================
// Schema storage (types -> database paths)
// ============================================================

int graphql_schema_store_type(graphql_layer_t* layer, graphql_type_t* type) {
    if (layer == NULL || type == NULL || layer->db == NULL) return -1;

    const char* plural = graphql_type_get_plural(type);
    char path_buf[512];

    // Store type kind
    snprintf(path_buf, sizeof(path_buf), "%s/__schema/type", plural);
    const char* kind_str = "object";
    if (type->kind == GRAPHQL_TYPE_SCALAR) kind_str = "scalar";
    else if (type->kind == GRAPHQL_TYPE_ENUM) kind_str = "enum";
    if (db_put_string(layer->db, path_buf, kind_str) != 0) return -1;

    // Register in global type registry
    snprintf(path_buf, sizeof(path_buf), "__schema/types/%s", type->name);
    if (db_put_string(layer->db, path_buf, type->name) != 0) return -1;

    // Store fields
    for (int i = 0; i < type->fields.length; i++) {
        graphql_field_t* field = type->fields.data[i];

        // Store field type - resolve the base type name (unwrap LIST/NON_NULL)
        snprintf(path_buf, sizeof(path_buf), "%s/__schema/fields/%s/type", plural, field->name);
        const char* field_type = resolve_base_type_name(field->type);
        if (db_put_string(layer->db, path_buf, field_type ? field_type : "Unknown") != 0) return -1;

        // Store field required
        snprintf(path_buf, sizeof(path_buf), "%s/__schema/fields/%s/required", plural, field->name);
        if (db_put_string(layer->db, path_buf, field->is_required ? "true" : "false") != 0) return -1;

        // Store field list flag
        if (is_list_type(field->type)) {
            snprintf(path_buf, sizeof(path_buf), "%s/__schema/fields/%s/is_list", plural, field->name);
            if (db_put_string(layer->db, path_buf, "true") != 0) return -1;
        }

        // Store field directives
        for (int j = 0; j < field->directives.length; j++) {
            graphql_directive_t* dir = field->directives.data[j];
            snprintf(path_buf, sizeof(path_buf), "%s/__schema/fields/%s/directive/%s", plural, field->name, dir->name);
            // Store directive as a simple presence marker for now
            if (db_put_string(layer->db, path_buf, "true") != 0) return -1;
        }

        // Store default value if present (type-prefixed: s:hello, i:42, b:true, n:null, f:3.14, e:ENUMVAL)
        if (field->default_value != NULL) {
            char val_buf[256];
            switch (field->default_value->kind) {
                case GRAPHQL_LITERAL_STRING:
                    snprintf(val_buf, sizeof(val_buf), "s:%s", field->default_value->string_val ? field->default_value->string_val : "");
                    break;
                case GRAPHQL_LITERAL_INT:
                    snprintf(val_buf, sizeof(val_buf), "i:%lld", (long long)field->default_value->int_val);
                    break;
                case GRAPHQL_LITERAL_FLOAT:
                    snprintf(val_buf, sizeof(val_buf), "f:%g", field->default_value->float_val);
                    break;
                case GRAPHQL_LITERAL_BOOL:
                    snprintf(val_buf, sizeof(val_buf), "b:%s", field->default_value->bool_val ? "true" : "false");
                    break;
                case GRAPHQL_LITERAL_NULL:
                    snprintf(val_buf, sizeof(val_buf), "n:null");
                    break;
                case GRAPHQL_LITERAL_ENUM:
                    snprintf(val_buf, sizeof(val_buf), "e:%s", field->default_value->string_val ? field->default_value->string_val : "");
                    break;
                case GRAPHQL_LITERAL_OBJECT: {
                    char* obj_str = graphql_literal_to_string(field->default_value);
                    snprintf(val_buf, sizeof(val_buf), "o:%s", obj_str);
                    free(obj_str);
                    break;
                }
                case GRAPHQL_LITERAL_LIST: {
                    char* list_str = graphql_literal_to_string(field->default_value);
                    snprintf(val_buf, sizeof(val_buf), "l:%s", list_str);
                    free(list_str);
                    break;
                }
                default:
                    val_buf[0] = '\0';
                    break;
            }
            if (val_buf[0] != '\0') {
                snprintf(path_buf, sizeof(path_buf), "%s/__schema/fields/%s/default_value", plural, field->name);
                if (db_put_string(layer->db, path_buf, val_buf) != 0) return -1;
            }
        }
    }

    // Store enum values
    for (int i = 0; i < type->enum_values.length; i++) {
        snprintf(path_buf, sizeof(path_buf), "%s/__schema/enum/%s", plural, type->enum_values.data[i]);
        if (db_put_string(layer->db, path_buf, type->enum_values.data[i]) != 0) return -1;
    }

    // Store custom plural name if set
    if (type->plural_name != NULL) {
        snprintf(path_buf, sizeof(path_buf), "%s/__schema/plural", plural);
        if (db_put_string(layer->db, path_buf, type->plural_name) != 0) return -1;
    }

    // Store next_id for object types
    if (type->kind == GRAPHQL_TYPE_OBJECT) {
        snprintf(path_buf, sizeof(path_buf), "%s/__meta/next_id", plural);
        if (db_put_string(layer->db, path_buf, "1") != 0) return -1;
    }

    return 0;
}

// ============================================================
// Schema loading (database paths -> types)
// ============================================================

int graphql_schema_load(graphql_layer_t* layer) {
    if (layer == NULL || layer->db == NULL || layer->registry == NULL) return -1;

    // Scan __schema/types/* to find all type names
    path_t* start_path = path_create();
    buffer_t* buf = buffer_create(8);
    buffer_t* buf2 = buffer_create(8);

    // Create path: __schema/types
    memcpy(buf->data, "__schema", 8);
    buf->size = 8;
    identifier_t* id1 = identifier_create(buf, 0);
    path_append(start_path, id1);
    identifier_destroy(id1);

    memcpy(buf2->data, "types", 5);
    buf2->size = 5;
    identifier_t* id2 = identifier_create(buf2, 0);
    path_append(start_path, id2);
    identifier_destroy(id2);

    buffer_destroy(buf);
    buffer_destroy(buf2);

    // Iterate over all types
    database_iterator_t* iter = database_scan_start(layer->db, start_path, NULL);
    path_destroy(start_path);

    if (iter == NULL) return -1;

    path_t* out_path = NULL;
    identifier_t* out_value = NULL;

    while (database_scan_next(iter, &out_path, &out_value) == 0) {
        if (out_path == NULL) break;

        // Extract type name from path: __schema/types/{TypeName}
        // The path has 3 identifiers: __schema, types, TypeName
        if (path_length(out_path) >= 3) {
            identifier_t* type_name_id = path_get(out_path, 2);
            if (type_name_id != NULL) {
                buffer_t* type_name_buf = identifier_to_buffer(type_name_id);
                if (type_name_buf != NULL) {
                    char* type_name = malloc(type_name_buf->size + 1);
                    if (type_name != NULL) {
                        memcpy(type_name, type_name_buf->data, type_name_buf->size);
                        type_name[type_name_buf->size] = '\0';
                        // Load the type from its __schema paths
                        graphql_type_t* type = load_type_from_database(layer, type_name);
                        if (type != NULL) {
                            graphql_type_registry_register(layer->registry, type);
                        }
                        free(type_name);
                    }
                    buffer_destroy(type_name_buf);
                }
            }
        }

        if (out_path) path_destroy(out_path);
        if (out_value) identifier_destroy(out_value);
        out_path = NULL;
        out_value = NULL;
    }

    database_scan_end(iter);

    // Load query and mutation type names — layer takes ownership
    layer->query_type = db_get_string(layer->db, "__schema/query_type");
    layer->mutation_type = db_get_string(layer->db, "__schema/mutation_type");

    return 0;
}

// Load a single type from its __schema paths
static graphql_type_t* load_type_from_database(graphql_layer_t* layer, const char* type_name) {
    if (layer == NULL || layer->db == NULL || type_name == NULL) return NULL;

    // Get plural name - first try to get custom plural, then default
    char path_buf[512];
    snprintf(path_buf, sizeof(path_buf), "%s/__schema/plural", type_name);
    char* plural_name = db_get_string(layer->db, path_buf);
    const char* plural = plural_name ? plural_name : make_plural(type_name);

    // Check if there's a type at plural/__schema/type
    // (schema data is stored using the plural form as the path prefix)
    snprintf(path_buf, sizeof(path_buf), "%s/__schema/type", plural);
    char* kind_str = db_get_string(layer->db, path_buf);

    // If not found, try using type_name directly (backward compat)
    if (kind_str == NULL) {
        snprintf(path_buf, sizeof(path_buf), "%s/__schema/type", type_name);
        kind_str = db_get_string(layer->db, path_buf);
    }

    // If still not found, unknown type
    if (kind_str == NULL) {
        free(plural_name);
        return NULL;
    }

    graphql_type_kind_t kind = GRAPHQL_TYPE_OBJECT;
    if (strcmp(kind_str, "scalar") == 0) kind = GRAPHQL_TYPE_SCALAR;
    else if (strcmp(kind_str, "enum") == 0) kind = GRAPHQL_TYPE_ENUM;

    graphql_type_t* type = graphql_type_create(type_name, kind);
    if (type == NULL) {
        free(kind_str);
        free(plural_name);
        return NULL;
    }

    type->plural_name = plural_name ? plural_name : make_plural(type_name);

    // Load fields by scanning __schema/fields/* paths
    // Build scan path: {PluralType}/__schema/fields/
    snprintf(path_buf, sizeof(path_buf), "%s/__schema/fields", type->plural_name);
    path_t* fields_start = path_create();
    if (fields_start != NULL) {
        const char* s = path_buf;
        while (*s) {
            const char* e = strchr(s, '/');
            size_t len = e ? (size_t)(e - s) : strlen(s);
            if (len > 0) {
                buffer_t* buf = buffer_create(len);
                if (buf != NULL) {
                    memcpy(buf->data, s, len);
                    buf->size = len;
                    identifier_t* id = identifier_create(buf, 0);
                    buffer_destroy(buf);
                    if (id != NULL) {
                        path_append(fields_start, id);
                        identifier_destroy(id);
                    }
                }
            }
            if (e) { s = e + 1; } else { break; }
        }

        database_iterator_t* iter = database_scan_start(layer->db, fields_start, NULL);
        // fields_start consumed by scan_start

        if (iter != NULL) {
            path_t* out_path = NULL;
            identifier_t* out_value = NULL;
            // Track field names we've already processed to avoid duplicates
            vec_t(char*) seen_fields;
            vec_init(&seen_fields);

            while (database_scan_next(iter, &out_path, &out_value) == 0) {
                if (out_path == NULL) break;

                // Path format: {PluralType}/__schema/fields/{fieldName}/{attr}
                // We want to extract fieldName (position 3)
                size_t path_len = path_length(out_path);
                if (path_len >= 4) {
                    identifier_t* field_name_id = path_get(out_path, 3);
                    if (field_name_id != NULL) {
                        buffer_t* fname_buf = identifier_to_buffer(field_name_id);
                        if (fname_buf != NULL) {
                            char* fname = malloc(fname_buf->size + 1);
                            if (fname != NULL) {
                                memcpy(fname, fname_buf->data, fname_buf->size);
                                fname[fname_buf->size] = '\0';

                                // Check if we've already processed this field
                                bool already_seen = false;
                                for (size_t k = 0; k < seen_fields.length; k++) {
                                    if (strcmp(seen_fields.data[k], fname) == 0) {
                                        already_seen = true;
                                        break;
                                    }
                                }

                                if (!already_seen) {
                                    vec_push(&seen_fields, strdup(fname));

                                    // Load field type
                                    char field_path[512];
                                    snprintf(field_path, sizeof(field_path), "%s/__schema/fields/%s/type", plural, fname);
                                    char* field_type_str = db_get_string(layer->db, field_path);

                                    // Load field required
                                    snprintf(field_path, sizeof(field_path), "%s/__schema/fields/%s/required", plural, fname);
                                    char* required_str = db_get_string(layer->db, field_path);
                                    bool is_required = (required_str != NULL && strcmp(required_str, "true") == 0);

                                    // Load field is_list
                                    snprintf(field_path, sizeof(field_path), "%s/__schema/fields/%s/is_list", plural, fname);
                                    char* is_list_str = db_get_string(layer->db, field_path);
                                    bool is_list = (is_list_str != NULL && strcmp(is_list_str, "true") == 0);

                                    // Build type reference
                                    // NOTE: Type resolution depends on all types being loaded
                                    // into the registry before their fields. Custom scalars
                                    // that are loaded after referencing types will be
                                    // misclassified as OBJECT. The load order follows
                                    // database scan order, which is deterministic but
                                    // depends on insertion order.
                                    graphql_type_ref_t* type_ref = NULL;
                                    if (field_type_str != NULL) {
                                        // Determine the kind of the referenced type
                                        graphql_type_kind_t ref_kind = GRAPHQL_TYPE_SCALAR;
                                        // Check if it's a known object type
                                        for (size_t k = 0; k < layer->registry->types.length; k++) {
                                            graphql_type_t* t = layer->registry->types.data[k];
                                            if (strcmp(t->name, field_type_str) == 0) {
                                                ref_kind = t->kind;
                                                break;
                                            }
                                        }
                                        // Default: if not found and not a known scalar, assume object
                                        if (strcmp(field_type_str, "String") == 0 ||
                                            strcmp(field_type_str, "Int") == 0 ||
                                            strcmp(field_type_str, "Float") == 0 ||
                                            strcmp(field_type_str, "Boolean") == 0 ||
                                            strcmp(field_type_str, "ID") == 0) {
                                            ref_kind = GRAPHQL_TYPE_SCALAR;
                                        } else if (ref_kind == GRAPHQL_TYPE_SCALAR) {
                                            ref_kind = GRAPHQL_TYPE_OBJECT;
                                        }

                                        type_ref = graphql_type_ref_create_named(ref_kind, field_type_str);
                                        // NOTE: Wrapping order is always [Type]! (NonNull wrapping
                                        // List). This correctly represents most GraphQL schemas.
                                        // The variant [Type!]! (NonNull wrapping both) is stored
                                        // with is_required=true and is_list=true, producing the
                                        // same [Type]! result. The distinction between [Type]!
                                        // and [Type!]! is lost in the current storage format,
                                        // which only records boolean is_list and is_required flags.
                                        if (is_list) {
                                            graphql_type_ref_t* inner = type_ref;
                                            type_ref = graphql_type_ref_create_list(inner);
                                            // Inner is consumed by create_list
                                        }
                                        if (is_required) {
                                            graphql_type_ref_t* inner = type_ref;
                                            type_ref = graphql_type_ref_create_non_null(inner);
                                            // Inner is consumed by create_non_null
                                        }
                                    }

                                    // Create field
                                    graphql_field_t* field = graphql_field_create(fname, type_ref, is_required);
                                    if (field != NULL) {
                                        // Load default value if present
                                        snprintf(field_path, sizeof(field_path), "%s/__schema/fields/%s/default_value", plural, fname);
                                        char* default_val_str = db_get_string(layer->db, field_path);
                                        if (default_val_str != NULL) {
                                            field->default_value = parse_default_value(default_val_str);
                                            free(default_val_str);
                                        }
                                        vec_push(&type->fields, field);
                                    }

                                    free(field_type_str);
                                    free(required_str);
                                    free(is_list_str);
                                }
                                free(fname);
                            }
                            buffer_destroy(fname_buf);
                        }
                    }
                }

                if (out_path) path_destroy(out_path);
                if (out_value) identifier_destroy(out_value);
                out_path = NULL;
                out_value = NULL;
            }

            // Free seen_fields
            for (size_t k = 0; k < seen_fields.length; k++) {
                free(seen_fields.data[k]);
            }
            vec_deinit(&seen_fields);

            database_scan_end(iter);
        }
    }

    // Load enum values by scanning __schema/enum/* paths
    if (kind == GRAPHQL_TYPE_ENUM) {
        snprintf(path_buf, sizeof(path_buf), "%s/__schema/enum", plural);
        path_t* enum_start = path_create();
        if (enum_start != NULL) {
            const char* s = path_buf;
            while (*s) {
                const char* e = strchr(s, '/');
                size_t len = e ? (size_t)(e - s) : strlen(s);
                if (len > 0) {
                    buffer_t* buf = buffer_create(len);
                    if (buf != NULL) {
                        memcpy(buf->data, s, len);
                        buf->size = len;
                        identifier_t* id = identifier_create(buf, 0);
                        buffer_destroy(buf);
                        if (id != NULL) {
                            path_append(enum_start, id);
                            identifier_destroy(id);
                        }
                    }
                }
                if (e) { s = e + 1; } else { break; }
            }

            database_iterator_t* iter = database_scan_start(layer->db, enum_start, NULL);
            // enum_start consumed by scan_start

            if (iter != NULL) {
                path_t* out_path = NULL;
                identifier_t* out_value = NULL;
                while (database_scan_next(iter, &out_path, &out_value) == 0) {
                    if (out_path == NULL) break;

                    // Path format: {PluralType}/__schema/enum/{value_name}
                    size_t path_len = path_length(out_path);
                    if (path_len >= 4) {
                        identifier_t* val_id = path_get(out_path, 3);
                        if (val_id != NULL) {
                            buffer_t* val_buf = identifier_to_buffer(val_id);
                            if (val_buf != NULL) {
                                char* val_str = malloc(val_buf->size + 1);
                                if (val_str != NULL) {
                                    memcpy(val_str, val_buf->data, val_buf->size);
                                    val_str[val_buf->size] = '\0';
                                    vec_push(&type->enum_values, val_str);
                                }
                                buffer_destroy(val_buf);
                            }
                        }
                    }

                    if (out_path) path_destroy(out_path);
                    if (out_value) identifier_destroy(out_value);
                    out_path = NULL;
                    out_value = NULL;
                }
                database_scan_end(iter);
            }
        }
    }

    free(kind_str);
    return type;
}

// ============================================================
// AST to type conversion
// ============================================================

static int convert_ast_to_types(graphql_layer_t* layer, graphql_ast_node_t* doc) {
    if (layer == NULL || doc == NULL) return -1;

    for (int i = 0; i < doc->children.length; i++) {
        graphql_ast_node_t* def = doc->children.data[i];
        switch (def->kind) {
            case GRAPHQL_AST_TYPE_DEFINITION: {
                graphql_type_t* type = convert_type_definition(def);
                if (type == NULL) return -1;

                // Check for @plural directive
                for (int j = 0; j < def->directives.length; j++) {
                    graphql_directive_t* dir = def->directives.data[j];
                    if (strcmp(dir->name, "plural") == 0) {
                        // Find the "name" argument
                        for (int k = 0; k < dir->arg_names.length; k++) {
                            if (strcmp(dir->arg_names.data[k], "name") == 0) {
                                free(type->plural_name);  // Free the default make_plural() result
                                type->plural_name = strdup(dir->arg_values.data[k]);
                            }
                        }
                    }
                }

                // If type already exists (from a prior extension), merge fields instead of duplicating
                graphql_type_t* existing = graphql_type_registry_get(layer->registry, type->name);
                if (existing != NULL) {
                    fprintf(stderr, "graphql: merging duplicate type definition '%s'\n", type->name);
                    // Merge new fields into existing type
                    for (int j = 0; j < type->fields.length; j++) {
                        graphql_field_t* new_field = type->fields.data[j];
                        // Skip if field already exists
                        bool already_exists = false;
                        for (int k = 0; k < existing->fields.length; k++) {
                            if (strcmp(existing->fields.data[k]->name, new_field->name) == 0) {
                                already_exists = true;
                                break;
                            }
                        }
                        if (!already_exists) {
                            vec_push(&existing->fields, new_field);
                        } else {
                            graphql_field_destroy(new_field);
                        }
                    }
                    // Transfer plural_name if new definition has one
                    if (type->plural_name != NULL) {
                        if (existing->plural_name != NULL) {
                            free(existing->plural_name);
                        }
                        existing->plural_name = type->plural_name;
                        type->plural_name = NULL;  // Prevent double-free
                    }
                    // Clear fields vector to prevent double-free since we transferred ownership
                    type->fields.length = 0;
                    // Handle enum values similarly
                    for (int j = 0; j < type->enum_values.length; j++) {
                        vec_push(&existing->enum_values, type->enum_values.data[j]);
                    }
                    type->enum_values.length = 0;
                    // Destroy the new type (fields were transferred or destroyed)
                    graphql_type_destroy(type);
                } else {
                    graphql_type_registry_register(layer->registry, type);
                }
                break;
            }
            case GRAPHQL_AST_ENUM_DEFINITION: {
                graphql_type_t* type = convert_enum_definition(def);
                if (type == NULL) return -1;
                graphql_type_registry_register(layer->registry, type);
                break;
            }
            case GRAPHQL_AST_SCHEMA_DEFINITION: {
                if (convert_schema_definition(layer, def) != 0) return -1;
                break;
            }
            case GRAPHQL_AST_SCALAR_DEFINITION: {
                graphql_type_t* type = graphql_type_create(def->name, GRAPHQL_TYPE_SCALAR);
                if (type == NULL) return -1;
                type->plural_name = make_plural(def->name);
                graphql_type_registry_register(layer->registry, type);
                break;
            }
            case GRAPHQL_AST_TYPE_EXTENSION: {
                // Look up existing type and merge new fields into it
                graphql_type_t* existing = graphql_type_registry_get(layer->registry, def->name);
                if (existing == NULL) {
                    // Extension before definition — create the type first
                    existing = graphql_type_create(def->name, GRAPHQL_TYPE_OBJECT);
                    if (existing == NULL) return -1;
                    existing->plural_name = make_plural(def->name);
                    graphql_type_registry_register(layer->registry, existing);
                }
                // Merge fields from the extension into the existing type
                for (int i = 0; i < def->children.length; i++) {
                    graphql_ast_node_t* field_node = def->children.data[i];
                    if (field_node->kind != GRAPHQL_AST_FIELD_DEFINITION) continue;

                    // Check if field already exists (skip duplicates)
                    bool already_exists = false;
                    for (int j = 0; j < existing->fields.length; j++) {
                        if (strcmp(existing->fields.data[j]->name, field_node->name) == 0) {
                            already_exists = true;
                            break;
                        }
                    }
                    if (already_exists) continue;

                    // Skip fields with missing type information
                    if (field_node->type_ref == NULL) {
                        fprintf(stderr, "graphql: skipping field '%s' with missing type in extension\n",
                                field_node->name ? field_node->name : "(unknown)");
                        continue;
                    }

                    graphql_field_t* field = graphql_field_create(
                        field_node->name,
                        field_node->type_ref,
                        field_node->is_required
                    );
                    if (field == NULL) continue;

                    // Copy directives from AST to field
                    for (int d = 0; d < field_node->directives.length; d++) {
                        graphql_directive_t* dir = field_node->directives.data[d];
                        graphql_directive_t* field_dir = graphql_directive_create(dir->name);
                        if (field_dir == NULL) {
                            graphql_field_destroy(field);
                            continue;
                        }
                        for (int a = 0; a < dir->arg_names.length; a++) {
                            vec_push(&field_dir->arg_names, strdup(dir->arg_names.data[a]));
                            vec_push(&field_dir->arg_values, strdup(dir->arg_values.data[a]));
                        }
                        vec_push(&field->directives, field_dir);
                    }

                    // Transfer type_ref ownership (don't destroy it in AST)
                    field_node->type_ref = NULL;

                    // Copy default value if present
                    if (field_node->literal != NULL) {
                        field->default_value = graphql_literal_copy(field_node->literal);
                    }

                    vec_push(&existing->fields, field);
                }
                break;
            }
            default:
                fprintf(stderr, "graphql: unexpected AST kind %d in convert_ast_to_types\n", def->kind);
                break;
        }
    }
    return 0;
}

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
                               void* ctx) {
    if (layer == NULL || type_name == NULL || field_name == NULL || resolver == NULL) {
        return -1;
    }

    // Look up the type
    graphql_type_t* type = graphql_schema_get_type(layer, type_name);
    if (type == NULL) {
        return -1;
    }

    // Find the field and set its resolver
    for (int i = 0; i < type->fields.length; i++) {
        graphql_field_t* field = type->fields.data[i];
        if (strcmp(field->name, field_name) == 0) {
            field->resolver = resolver;
            field->resolver_ctx = ctx;
            return 0;
        }
    }

    return -1;  // Field not found
}

static graphql_type_t* convert_type_definition(graphql_ast_node_t* node) {
    if (node == NULL || node->name == NULL) return NULL;

    graphql_type_t* type = graphql_type_create(node->name, GRAPHQL_TYPE_OBJECT);
    if (type == NULL) return NULL;

    // Set default plural name (TypeName + "s") if not overridden by @plural
    type->plural_name = make_plural(node->name);

    // Convert field definitions
    for (int i = 0; i < node->children.length; i++) {
        graphql_ast_node_t* field_node = node->children.data[i];
        if (field_node->kind != GRAPHQL_AST_FIELD_DEFINITION) continue;

        graphql_field_t* field = graphql_field_create(
            field_node->name,
            field_node->type_ref ? field_node->type_ref : NULL,
            field_node->is_required
        );
        if (field == NULL) {
            graphql_type_destroy(type);
            return NULL;
        }

        // Copy directives from AST to field
        for (int j = 0; j < field_node->directives.length; j++) {
            graphql_directive_t* dir = field_node->directives.data[j];
            // Create a copy of the directive
            graphql_directive_t* field_dir = graphql_directive_create(dir->name);
            if (field_dir == NULL) {
                graphql_field_destroy(field);
                graphql_type_destroy(type);
                return NULL;
            }
            for (int k = 0; k < dir->arg_names.length; k++) {
                char* name_copy = strdup(dir->arg_names.data[k]);
                char* value_copy = strdup(dir->arg_values.data[k]);
                vec_push(&field_dir->arg_names, name_copy);
                vec_push(&field_dir->arg_values, value_copy);
            }
            vec_push(&field->directives, field_dir);
        }

        // Transfer type_ref ownership (don't destroy it)
        field_node->type_ref = NULL;

        // Copy default value if present
        if (field_node->literal != NULL) {
            field->default_value = graphql_literal_copy(field_node->literal);
        }

        vec_push(&type->fields, field);
    }

    return type;
}

static graphql_type_t* convert_enum_definition(graphql_ast_node_t* node) {
    if (node == NULL || node->name == NULL) return NULL;

    graphql_type_t* type = graphql_type_create(node->name, GRAPHQL_TYPE_ENUM);
    if (type == NULL) return NULL;

    // Convert enum values
    for (int i = 0; i < node->children.length; i++) {
        graphql_ast_node_t* value_node = node->children.data[i];
        if (value_node->name == NULL) continue;
        char* value = strdup(value_node->name);
        if (value == NULL) {
            graphql_type_destroy(type);
            return NULL;
        }
        vec_push(&type->enum_values, value);
    }

    return type;
}

static int convert_schema_definition(graphql_layer_t* layer, graphql_ast_node_t* node) {
    if (layer == NULL || node == NULL) return -1;

    // Store query and mutation type names in __schema
    for (int i = 0; i < node->children.length; i++) {
        graphql_ast_node_t* field = node->children.data[i];
        if (field->name == NULL) continue;

        if (strcmp(field->name, "query") == 0 && field->type_ref && field->type_ref->name) {
            db_put_string(layer->db, "__schema/query_type", field->type_ref->name);
        } else if (strcmp(field->name, "mutation") == 0 && field->type_ref && field->type_ref->name) {
            db_put_string(layer->db, "__schema/mutation_type", field->type_ref->name);
        }
    }

    return 0;
}