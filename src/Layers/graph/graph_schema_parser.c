//
// Created by victor on 06/01/26.
//
// Recursive descent parser for the graph schema definition language.
// Parses: type Clip @index(spo, pos) { tagged_with: [Tag]; ... }
// Stores parsed types on the layer and persists to __gschema/* paths.
//

#include "graph_internal.h"
#include "../../Util/allocator.h"
#include <string.h>
#include <stdio.h>
#include <ctype.h>

/* ── Parser state ── */

typedef struct {
    const char* input;
    size_t pos;
    size_t len;
    char error[256];
    int error_pos;
    int has_error;
} parser_t;

/* ── Helpers ── */

static void set_error(parser_t* p, const char* msg) {
    if (!p->has_error) {
        p->has_error = 1;
        p->error_pos = (int)p->pos;
        snprintf(p->error, sizeof(p->error), "%s", msg);
    }
}

static void skip_whitespace(parser_t* p) {
    while (p->pos < p->len && isspace((unsigned char)p->input[p->pos]))
        p->pos++;
}

static int expect_char(parser_t* p, char c) {
    skip_whitespace(p);
    if (p->pos < p->len && p->input[p->pos] == c) {
        p->pos++;
        return 1;
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "Expected '%c'", c);
    set_error(p, buf);
    return 0;
}

static int expect_string(parser_t* p, const char* s) {
    skip_whitespace(p);
    size_t slen = strlen(s);
    if (p->pos + slen <= p->len && strncmp(p->input + p->pos, s, slen) == 0) {
        p->pos += slen;
        return 1;
    }
    char buf[128];
    snprintf(buf, sizeof(buf), "Expected '%s'", s);
    set_error(p, buf);
    return 0;
}

static char* parse_name(parser_t* p) {
    skip_whitespace(p);
    if (p->pos >= p->len || !isalpha((unsigned char)p->input[p->pos]))
        return NULL;
    size_t start = p->pos;
    while (p->pos < p->len && (isalnum((unsigned char)p->input[p->pos]) || p->input[p->pos] == '_'))
        p->pos++;
    if (p->pos == start) return NULL;
    size_t slen = p->pos - start;
    char* s = (char*)get_memory(slen + 1);
    memcpy(s, p->input + start, slen);
    s[slen] = '\0';
    return s;
}

/* ── Index flag parser ── */

static graph_index_flags_t parse_index_list(parser_t* p) {
    graph_index_flags_t flags = GRAPH_INDEX_NONE;
    int first = 1;
    for (;;) {
        skip_whitespace(p);
        if (!first) {
            if (p->pos < p->len && p->input[p->pos] == ',') {
                p->pos++;
            } else {
                break;
            }
        }
        first = 0;

        char* name = parse_name(p);
        if (!name) { set_error(p, "Expected index name"); return GRAPH_INDEX_NONE; }

        if (strcmp(name, "spo") == 0)       flags |= GRAPH_INDEX_SPO;
        else if (strcmp(name, "pos") == 0)  flags |= GRAPH_INDEX_POS;
        else if (strcmp(name, "osp") == 0)  flags |= GRAPH_INDEX_OSP;
        else if (strcmp(name, "pso") == 0)  flags |= GRAPH_INDEX_PSO;
        else {
            char buf[128];
            snprintf(buf, sizeof(buf), "Unknown index '%s'", name);
            set_error(p, buf);
            free(name);
            return GRAPH_INDEX_NONE;
        }
        free(name);
    }
    return flags;
}

/* ── Type reference parser: "String", "[Tag]", "User" ── */

static int parse_type_ref(parser_t* p, char** type_out, uint8_t* is_array_out) {
    skip_whitespace(p);
    *is_array_out = 0;

    if (p->pos < p->len && p->input[p->pos] == '[') {
        *is_array_out = 1;
        p->pos++;
    }

    char* name = parse_name(p);
    if (!name) { set_error(p, "Expected type name"); return -1; }
    *type_out = name;

    if (*is_array_out && !expect_char(p, ']')) return -1;
    return 0;
}

/* ── Field parser: name: Type @index(spo) ── */

static int parse_field(parser_t* p, graph_schema_field_t* field) {
    memset(field, 0, sizeof(*field));

    char* fname = parse_name(p);
    if (!fname) { set_error(p, "Expected field name"); return -1; }
    field->name = fname;

    if (!expect_char(p, ':')) return -1;
    if (parse_type_ref(p, &field->type, &field->is_array) != 0) return -1;

    // Optional @index(...)
    skip_whitespace(p);
    if (p->pos < p->len && p->input[p->pos] == '@') {
        p->pos++;
        char* directive = parse_name(p);
        if (directive && strcmp(directive, "index") == 0) {
            free(directive);
            if (!expect_char(p, '(')) return -1;
            field->indices = parse_index_list(p);
            if (p->has_error) return -1;
            if (!expect_char(p, ')')) return -1;
        } else {
            free(directive);
        }
    }

    return 0;
}

/* ── Top-level: parse a single type definition ── */

#define MAX_FIELDS_PER_TYPE 64

static int parse_type_def(parser_t* p, graph_layer_t* layer) {
    if (!expect_string(p, "type")) return -1;

    char* tname = parse_name(p);
    if (!tname) { set_error(p, "Expected type name after 'type'"); return -1; }

    graph_index_flags_t default_indices = GRAPH_INDEX_SPO | GRAPH_INDEX_POS;

    // Optional @index(...) on the type
    skip_whitespace(p);
    if (p->pos < p->len && p->input[p->pos] == '@') {
        p->pos++;
        char* directive = parse_name(p);
        if (directive && strcmp(directive, "index") == 0) {
            free(directive);
            if (!expect_char(p, '(')) { free(tname); return -1; }
            default_indices = parse_index_list(p);
            if (p->has_error) { free(tname); return -1; }
            if (!expect_char(p, ')')) { free(tname); return -1; }
        } else {
            free(directive);
        }
    }

    if (!expect_char(p, '{')) { free(tname); return -1; }

    // Use a fixed-size array to avoid allocation complexity
    graph_schema_field_t fields[MAX_FIELDS_PER_TYPE];
    size_t field_count = 0;
    memset(fields, 0, sizeof(fields));

    while (p->pos < p->len) {
        skip_whitespace(p);
        if (p->pos < p->len && p->input[p->pos] == '}') break;

        if (field_count >= MAX_FIELDS_PER_TYPE) {
            set_error(p, "Too many fields (max 64 per type)");
            free(tname);
            for (size_t i = 0; i < field_count; i++) {
                free(fields[i].name);
                free(fields[i].type);
            }
            return -1;
        }

        graph_schema_field_t* f = &fields[field_count];

        if (parse_field(p, f) != 0) {

            free(tname);
            for (size_t j = 0; j < field_count; j++) {
                free(fields[j].name);
                free(fields[j].type);
            }
            return -1;
        }

        // If field didn't specify its own @index, inherit the type default
        if (f->indices == GRAPH_INDEX_NONE) {
            f->indices = default_indices;
        }

        // Optional semicolon
        skip_whitespace(p);
        if (p->pos < p->len && p->input[p->pos] == ';') p->pos++;

        field_count++;
    }

    if (!expect_char(p, '}')) {
        free(tname);
        for (size_t i = 0; i < field_count; i++) {
            free(fields[i].name);
            free(fields[i].type);
        }
        return -1;
    }

    // Build the type entry on the layer's schema
    graph_schema_t* schema = layer->schema;
    if (!schema) {
        schema = (graph_schema_t*)get_clear_memory(sizeof(graph_schema_t));
        layer->schema = schema;
    }

    // Grow types array if needed
    if (schema->type_count >= schema->type_capacity) {
        size_t new_cap = schema->type_capacity ? schema->type_capacity * 2 : 4;
        graph_schema_type_t* new_types = (graph_schema_type_t*)get_memory(new_cap * sizeof(graph_schema_type_t));
        if (schema->types) {
            memcpy(new_types, schema->types, schema->type_count * sizeof(graph_schema_type_t));
            free(schema->types);
        }
        schema->types = new_types;
        schema->type_capacity = new_cap;
    }

    // Populate the new type
    graph_schema_type_t* st = &schema->types[schema->type_count];
    memset(st, 0, sizeof(*st));
    st->name = tname;
    st->indices = default_indices;
    st->field_count = field_count;
    st->fields = (graph_schema_field_t*)get_clear_memory(field_count * sizeof(graph_schema_field_t));
    for (size_t i = 0; i < field_count; i++) {
        st->fields[i] = fields[i];  // struct copy takes ownership of name/type pointers
        memset(&fields[i], 0, sizeof(fields[i]));  // prevent double-free in cleanup
    }
    schema->type_count++;

    // Persist to database
    graph_schema_store_type(layer, st);

    return 0;
}

/* ── Public API (declared in graph_internal.h) ── */

int graph_schema_parse_dsl(graph_layer_t* layer, const char* input, size_t len, char** error_out) {
    parser_t p;
    memset(&p, 0, sizeof(p));
    p.input = input;
    p.pos = 0;
    p.len = len;
    p.has_error = 0;

    while (p.pos < p.len) {
        skip_whitespace(&p);
        if (p.pos >= p.len) break;
        if (parse_type_def(&p, layer) != 0) {
            if (error_out) {
                *error_out = (char*)get_memory(256);
                snprintf(*error_out, 256, "Position %d: %s", p.error_pos, p.error);
            }
            return -1;
        }
    }

    return 0;
}

/* ── Schema persistence ── */

int graph_schema_store_type(graph_layer_t* layer, graph_schema_type_t* type) {
    if (!layer->db || !type) return -1;

    database_t* db = layer->db;
    char path[1024];
    char idx_str[64];

    // Store type indices: __gschema/<type>/indices
    int len = snprintf(path, sizeof(path), "__gschema/%s/indices", type->name);
    if (len < 0) return -1;

    // Build comma-separated index string
    idx_str[0] = '\0';
    if (type->indices & GRAPH_INDEX_SPO) strncat(idx_str, "spo,", sizeof(idx_str) - strlen(idx_str) - 1);
    if (type->indices & GRAPH_INDEX_POS) strncat(idx_str, "pos,", sizeof(idx_str) - strlen(idx_str) - 1);
    if (type->indices & GRAPH_INDEX_OSP) strncat(idx_str, "osp,", sizeof(idx_str) - strlen(idx_str) - 1);
    if (type->indices & GRAPH_INDEX_PSO) strncat(idx_str, "pso,", sizeof(idx_str) - strlen(idx_str) - 1);
    size_t isl = strlen(idx_str);
    if (isl > 0) idx_str[isl - 1] = '\0';  // Remove trailing comma

    database_put_sync_raw(db, path, strlen(path), '/', (const uint8_t*)idx_str, strlen(idx_str));

    for (size_t i = 0; i < type->field_count; i++) {
        graph_schema_field_t* f = &type->fields[i];

        // __gschema/<type>/fields/<field>/type → "Tag"
        snprintf(path, sizeof(path), "__gschema/%s/fields/%s/type", type->name, f->name);
        database_put_sync_raw(db, path, strlen(path), '/', (const uint8_t*)f->type, strlen(f->type));

        // __gschema/<type>/fields/<field>/array → "1" or "0"
        snprintf(path, sizeof(path), "__gschema/%s/fields/%s/array", type->name, f->name);
        char av = f->is_array ? '1' : '0';
        database_put_sync_raw(db, path, strlen(path), '/', (const uint8_t*)&av, 1);

        // __gschema/<type>/fields/<field>/indices → "spo,pos"
        snprintf(path, sizeof(path), "__gschema/%s/fields/%s/indices", type->name, f->name);
        char f_idx[64] = "";
        if (f->indices & GRAPH_INDEX_SPO) strncat(f_idx, "spo,", sizeof(f_idx) - strlen(f_idx) - 1);
        if (f->indices & GRAPH_INDEX_POS) strncat(f_idx, "pos,", sizeof(f_idx) - strlen(f_idx) - 1);
        if (f->indices & GRAPH_INDEX_OSP) strncat(f_idx, "osp,", sizeof(f_idx) - strlen(f_idx) - 1);
        if (f->indices & GRAPH_INDEX_PSO) strncat(f_idx, "pso,", sizeof(f_idx) - strlen(f_idx) - 1);
        isl = strlen(f_idx);
        if (isl > 0) f_idx[isl - 1] = '\0';

        database_put_sync_raw(db, path, strlen(path), '/', (const uint8_t*)f_idx, strlen(f_idx));
    }

    return 0;
}

int graph_schema_load(graph_layer_t* layer) {
    // Future: load schema from __gschema/* paths on reopen
    (void)layer;
    return 0;
}
