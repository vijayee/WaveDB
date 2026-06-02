//
// Created by victor on 06/01/26.
//
// Recursive descent parser for the graph schema definition language.
// Parses: type Clip @index(spo, pos) { tagged_with: [Tag]; ... }
// Stores parsed types on the layer and persists to __gschema/* paths.
//

#include "graph_internal.h"
#include "../../Util/allocator.h"
#include "../../Util/vec.h"
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

    // Build the type locally and push it into the schema's types vector
    graph_schema_type_t st;
    memset(&st, 0, sizeof(st));
    st.name = tname;
    st.indices = default_indices;
    vec_init(&st.fields);
    vec_reserve(&st.fields, (int)field_count);
    for (size_t i = 0; i < field_count; i++) {
        vec_push(&st.fields, fields[i]);  // struct copy takes ownership of name/type pointers
        memset(&fields[i], 0, sizeof(fields[i]));  // prevent double-free in cleanup
    }
    vec_push(&schema->types, st);

    // Persist to database (st was copied into the vec, use the vec's copy)
    graph_schema_store_type(layer, &schema->types.data[schema->types.length - 1]);

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

    // Store type name in the master list: __gschema/types
    // Read existing list, append this type if not already present
    uint8_t* existing = NULL;
    size_t existing_len = 0;
    int grc = database_get_sync_raw(db, "__gschema/types", 15, '/', &existing, &existing_len);
    vec_char_t types_buf;
    vec_init(&types_buf);
    if (grc == 0 && existing != NULL) {
        vec_pusharr(&types_buf, (const char*)existing, existing_len);
        vec_push(&types_buf, '\0');
        database_raw_value_free(existing);
    }

    // Check if type name already in list
    int already_present = 0;
    if (types_buf.length > 1) {
        vec_char_t search;
        vec_init(&search);
        vec_push(&search, ',');
        vec_pusharr(&search, type->name, strlen(type->name));
        vec_push(&search, ',');
        vec_push(&search, '\0');

        vec_char_t list_search;
        vec_init(&list_search);
        vec_push(&list_search, ',');
        vec_pusharr(&list_search, types_buf.data, types_buf.length - 1);
        vec_push(&list_search, ',');
        vec_push(&list_search, '\0');

        if (strstr(list_search.data, search.data) != NULL) already_present = 1;
        if (!already_present) {
            vec_clear(&search);
            vec_pusharr(&search, type->name, strlen(type->name));
            vec_push(&search, ',');
            vec_push(&search, '\0');
            if (strncmp(types_buf.data, search.data, strlen(search.data) - 1) == 0) already_present = 1;
        }
        vec_deinit(&search);
        vec_deinit(&list_search);
    }
    if (!already_present) {
        // Remove trailing null (added above for strstr check) before appending
        if (types_buf.length > 0) types_buf.length--;
        if (types_buf.length > 0) vec_pusharr(&types_buf, ",", 1);
        vec_pusharr(&types_buf, type->name, strlen(type->name));
        vec_push(&types_buf, '\0');
        database_put_sync_raw(db, "__gschema/types", 15, '/', (const uint8_t*)types_buf.data, types_buf.length - 1);
    }
    vec_deinit(&types_buf);

    // Store type indices: __gschema/<type>/indices
    vec_char_t path;
    vec_init(&path);
    vec_pusharr(&path, "__gschema/", 10);
    vec_pusharr(&path, type->name, strlen(type->name));
    vec_pusharr(&path, "/indices", 9);
    vec_push(&path, '\0');

    // Build comma-separated index string
    char idx_str[64];
    idx_str[0] = '\0';
    if (type->indices & GRAPH_INDEX_SPO) strncat(idx_str, "spo,", sizeof(idx_str) - strlen(idx_str) - 1);
    if (type->indices & GRAPH_INDEX_POS) strncat(idx_str, "pos,", sizeof(idx_str) - strlen(idx_str) - 1);
    if (type->indices & GRAPH_INDEX_OSP) strncat(idx_str, "osp,", sizeof(idx_str) - strlen(idx_str) - 1);
    if (type->indices & GRAPH_INDEX_PSO) strncat(idx_str, "pso,", sizeof(idx_str) - strlen(idx_str) - 1);
    size_t isl = strlen(idx_str);
    if (isl > 0) idx_str[isl - 1] = '\0';  // Remove trailing comma

    database_put_sync_raw(db, path.data, path.length - 1, '/', (const uint8_t*)idx_str, strlen(idx_str));

    for (int i = 0; i < type->fields.length; i++) {
        graph_schema_field_t* f = &type->fields.data[i];

        // __gschema/<type>/fields/<field>/type
        vec_clear(&path);
        vec_pusharr(&path, "__gschema/", 10);
        vec_pusharr(&path, type->name, strlen(type->name));
        vec_pusharr(&path, "/fields/", 9);
        vec_pusharr(&path, f->name, strlen(f->name));
        vec_pusharr(&path, "/type", 5);
        vec_push(&path, '\0');
        database_put_sync_raw(db, path.data, path.length - 1, '/', (const uint8_t*)f->type, strlen(f->type));

        // __gschema/<type>/fields/<field>/array → "1" or "0"
        vec_clear(&path);
        vec_pusharr(&path, "__gschema/", 10);
        vec_pusharr(&path, type->name, strlen(type->name));
        vec_pusharr(&path, "/fields/", 9);
        vec_pusharr(&path, f->name, strlen(f->name));
        vec_pusharr(&path, "/array", 6);
        vec_push(&path, '\0');
        char av = f->is_array ? '1' : '0';
        database_put_sync_raw(db, path.data, path.length - 1, '/', (const uint8_t*)&av, 1);

        // __gschema/<type>/fields/<field>/indices → "spo,pos"
        vec_clear(&path);
        vec_pusharr(&path, "__gschema/", 10);
        vec_pusharr(&path, type->name, strlen(type->name));
        vec_pusharr(&path, "/fields/", 9);
        vec_pusharr(&path, f->name, strlen(f->name));
        vec_pusharr(&path, "/indices", 9);
        vec_push(&path, '\0');
        char f_idx[64] = "";
        if (f->indices & GRAPH_INDEX_SPO) strncat(f_idx, "spo,", sizeof(f_idx) - strlen(f_idx) - 1);
        if (f->indices & GRAPH_INDEX_POS) strncat(f_idx, "pos,", sizeof(f_idx) - strlen(f_idx) - 1);
        if (f->indices & GRAPH_INDEX_OSP) strncat(f_idx, "osp,", sizeof(f_idx) - strlen(f_idx) - 1);
        if (f->indices & GRAPH_INDEX_PSO) strncat(f_idx, "pso,", sizeof(f_idx) - strlen(f_idx) - 1);
        isl = strlen(f_idx);
        if (isl > 0) f_idx[isl - 1] = '\0';

        database_put_sync_raw(db, path.data, path.length - 1, '/', (const uint8_t*)f_idx, strlen(f_idx));
    }

    // Store field name list: __gschema/<type>/fields_list
    if (type->fields.length > 0) {
        vec_clear(&path);
        vec_pusharr(&path, "__gschema/", 10);
        vec_pusharr(&path, type->name, strlen(type->name));
        vec_pusharr(&path, "/fields_list", 13);
        vec_push(&path, '\0');

        vec_char_t fl_buf;
        vec_init(&fl_buf);
        for (int i = 0; i < type->fields.length; i++) {
            if (i > 0) vec_push(&fl_buf, ',');
            vec_pusharr(&fl_buf, type->fields.data[i].name, strlen(type->fields.data[i].name));
        }
        vec_push(&fl_buf, '\0');
        database_put_sync_raw(db, path.data, path.length - 1, '/', (const uint8_t*)fl_buf.data, fl_buf.length - 1);
        vec_deinit(&fl_buf);
    }

    vec_deinit(&path);
    return 0;
}


/* ── Helper: parse index string "spo,pos" → flags ── */

static graph_index_flags_t parse_indices_str(const char* str) {
    graph_index_flags_t flags = GRAPH_INDEX_NONE;
    if (!str) return flags;

    char buf[64];
    strncpy(buf, str, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char* tok = strtok(buf, ",");
    while (tok) {
        if (strcmp(tok, "spo") == 0)       flags |= GRAPH_INDEX_SPO;
        else if (strcmp(tok, "pos") == 0)  flags |= GRAPH_INDEX_POS;
        else if (strcmp(tok, "osp") == 0)  flags |= GRAPH_INDEX_OSP;
        else if (strcmp(tok, "pso") == 0)  flags |= GRAPH_INDEX_PSO;
        tok = strtok(NULL, ",");
    }
    return flags;
}

int graph_schema_load(graph_layer_t* layer) {
    if (!layer || !layer->db) return 0;

    database_t* db = layer->db;

    // Read the master type list: __gschema/types (comma-separated)
    uint8_t* types_val = NULL;
    size_t types_val_len = 0;
    int rc = database_get_sync_raw(db, "__gschema/types", 15, '/', &types_val, &types_val_len);
    if (rc != 0 || !types_val || types_val_len == 0) {
        if (types_val) database_raw_value_free(types_val);
        return 0;
    }

    vec_char_t types_buf;
    vec_init(&types_buf);
    size_t copy_len = types_val_len;
    vec_pusharr(&types_buf, (const char*)types_val, copy_len);
    vec_push(&types_buf, '\0');
    database_raw_value_free(types_val);

    // Parse comma-separated type names
    char type_names[64][128];
    size_t type_count = 0;

    char* tok = strtok(types_buf.data, ",");
    while (tok && type_count < 64) {
        if (strlen(tok) < 128) {
            strcpy(type_names[type_count], tok);
            type_count++;
        }
        tok = strtok(NULL, ",");
    }

    if (type_count == 0) {
        vec_deinit(&types_buf);
        return 0;
    }

    // Allocate schema (types vec is zeroed by get_clear_memory)
    graph_schema_t* schema = (graph_schema_t*)get_clear_memory(sizeof(graph_schema_t));

    vec_char_t path;
    vec_init(&path);

    for (size_t ti = 0; ti < type_count; ti++) {
        graph_schema_type_t st;
        memset(&st, 0, sizeof(st));
        st.name = strdup(type_names[ti]);

        // Read type indices: __gschema/<type>/indices
        vec_clear(&path);
        vec_pusharr(&path, "__gschema/", 10);
        vec_pusharr(&path, type_names[ti], strlen(type_names[ti]));
        vec_pusharr(&path, "/indices", 9);
        vec_push(&path, '\0');

        uint8_t* val = NULL;
        size_t val_len = 0;
        int grc = database_get_sync_raw(db, path.data, path.length - 1, '/', &val, &val_len);
        if (grc == 0 && val != NULL) {
            char idx_str[64];
            size_t cl = val_len < sizeof(idx_str) - 1 ? val_len : sizeof(idx_str) - 1;
            memcpy(idx_str, val, cl);
            idx_str[cl] = '\0';
            st.indices = parse_indices_str(idx_str);
            database_raw_value_free(val);
        } else {
            st.indices = GRAPH_INDEX_SPO | GRAPH_INDEX_POS;
        }

        // Read field list: __gschema/<type>/fields_list (comma-separated)
        char field_names[64][128];
        size_t field_count = 0;

        vec_clear(&path);
        vec_pusharr(&path, "__gschema/", 10);
        vec_pusharr(&path, type_names[ti], strlen(type_names[ti]));
        vec_pusharr(&path, "/fields_list", 13);
        vec_push(&path, '\0');

        uint8_t* fl_val = NULL;
        size_t fl_len = 0;
        int flrc = database_get_sync_raw(db, path.data, path.length - 1, '/', &fl_val, &fl_len);
        if (flrc == 0 && fl_val != NULL && fl_len > 0) {
            vec_char_t fl_buf;
            vec_init(&fl_buf);
            vec_pusharr(&fl_buf, (const char*)fl_val, fl_len);
            vec_push(&fl_buf, '\0');
            database_raw_value_free(fl_val);

            char* ftok = strtok(fl_buf.data, ",");
            while (ftok && field_count < 64) {
                if (strlen(ftok) < 128) {
                    strcpy(field_names[field_count], ftok);
                    field_count++;
                }
                ftok = strtok(NULL, ",");
            }
            vec_deinit(&fl_buf);
        }

        if (field_count > 0) {
            vec_init(&st.fields);
            vec_reserve(&st.fields, (int)field_count);

            for (size_t fi = 0; fi < field_count; fi++) {
                graph_schema_field_t f;
                memset(&f, 0, sizeof(f));
                f.name = strdup(field_names[fi]);

                // __gschema/<type>/fields/<field>/type
                vec_clear(&path);
                vec_pusharr(&path, "__gschema/", 10);
                vec_pusharr(&path, type_names[ti], strlen(type_names[ti]));
                vec_pusharr(&path, "/fields/", 9);
                vec_pusharr(&path, field_names[fi], strlen(field_names[fi]));
                vec_pusharr(&path, "/type", 5);
                vec_push(&path, '\0');
                val = NULL; val_len = 0;
                grc = database_get_sync_raw(db, path.data, path.length - 1, '/', &val, &val_len);
                if (grc == 0 && val != NULL) {
                    f.type = (char*)get_memory(val_len + 1);
                    memcpy(f.type, val, val_len);
                    f.type[val_len] = '\0';
                    database_raw_value_free(val);
                } else {
                    f.type = strdup("String");
                }

                // __gschema/<type>/fields/<field>/array
                vec_clear(&path);
                vec_pusharr(&path, "__gschema/", 10);
                vec_pusharr(&path, type_names[ti], strlen(type_names[ti]));
                vec_pusharr(&path, "/fields/", 9);
                vec_pusharr(&path, field_names[fi], strlen(field_names[fi]));
                vec_pusharr(&path, "/array", 6);
                vec_push(&path, '\0');
                val = NULL; val_len = 0;
                grc = database_get_sync_raw(db, path.data, path.length - 1, '/', &val, &val_len);
                if (grc == 0 && val != NULL && val_len > 0) {
                    f.is_array = (val[0] == '1');
                    database_raw_value_free(val);
                }

                // __gschema/<type>/fields/<field>/indices
                vec_clear(&path);
                vec_pusharr(&path, "__gschema/", 10);
                vec_pusharr(&path, type_names[ti], strlen(type_names[ti]));
                vec_pusharr(&path, "/fields/", 9);
                vec_pusharr(&path, field_names[fi], strlen(field_names[fi]));
                vec_pusharr(&path, "/indices", 9);
                vec_push(&path, '\0');
                val = NULL; val_len = 0;
                grc = database_get_sync_raw(db, path.data, path.length - 1, '/', &val, &val_len);
                if (grc == 0 && val != NULL) {
                    char idx_str[64];
                    size_t cl = val_len < sizeof(idx_str) - 1 ? val_len : sizeof(idx_str) - 1;
                    memcpy(idx_str, val, cl);
                    idx_str[cl] = '\0';
                    f.indices = parse_indices_str(idx_str);
                    database_raw_value_free(val);
                } else {
                    f.indices = st.indices;
                }
                vec_push(&st.fields, f);
            }
        }

        vec_push(&schema->types, st);
    }

    vec_deinit(&path);
    vec_deinit(&types_buf);
    layer->schema = schema;
    return 0;
}
