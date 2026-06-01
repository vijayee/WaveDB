//
// Created by victor on 06/01/26.
//
// Recursive descent parser for the Gremlin-inspired graph DSL.
// Compiles strings like g.V("alice").Out("follows").All() into
// graph_query_t step chains.
//

#include "graph_internal.h"
#include <string.h>
#include <stdio.h>
#include <ctype.h>

/* ── Parser state ── */

typedef struct {
    const char* input;
    size_t pos;
    size_t len;
    graph_layer_t* layer;
    graph_parse_error_t* error;
} parser_t;

/* ── Forward declarations ── */

static int parse_query(parser_t* p, graph_query_t* q);
static int parse_step(parser_t* p, graph_query_t* q);

/* ── Helpers ── */

static void set_error(parser_t* p, const char* msg) {
    if (p->error) {
        p->error->ok = 0;
        p->error->position = (int)p->pos;
        snprintf(p->error->message, sizeof(p->error->message), "%s", msg);
    }
}

static void set_error_expect(parser_t* p, const char* expected) {
    char buf[256];
    if (p->pos < p->len) {
        char c = p->input[p->pos];
        snprintf(buf, sizeof(buf), "Expected '%s' but found '%c' at position %zu",
                 expected, c, p->pos);
    } else {
        snprintf(buf, sizeof(buf), "Expected '%s' but reached end of input", expected);
    }
    set_error(p, buf);
}

static void skip_whitespace(parser_t* p) {
    while (p->pos < p->len && isspace((unsigned char)p->input[p->pos]))
        p->pos++;
}

static int peek(parser_t* p) {
    skip_whitespace(p);
    return p->pos < p->len ? (unsigned char)p->input[p->pos] : EOF;
}

static int expect_char(parser_t* p, char c) {
    skip_whitespace(p);
    if (p->pos < p->len && p->input[p->pos] == c) {
        p->pos++;
        return 1;
    }
    char buf[8] = {0};
    buf[0] = c;
    set_error_expect(p, buf);
    return 0;
}

static char* parse_string(parser_t* p) {
    skip_whitespace(p);
    if (p->pos >= p->len || p->input[p->pos] != '"') {
        set_error_expect(p, "\"");
        return NULL;
    }
    p->pos++; // skip opening quote

    size_t start = p->pos;
    while (p->pos < p->len && p->input[p->pos] != '"') {
        p->pos++;
    }
    if (p->pos >= p->len) {
        set_error(p, "Unterminated string");
        return NULL;
    }

    size_t slen = p->pos - start;
    char* s = (char*)get_memory(slen + 1);
    memcpy(s, p->input + start, slen);
    s[slen] = '\0';
    p->pos++; // skip closing quote
    return s;
}

static long parse_number(parser_t* p) {
    skip_whitespace(p);
    if (p->pos >= p->len || !isdigit((unsigned char)p->input[p->pos])) {
        set_error_expect(p, "number");
        return -1;
    }
    long n = 0;
    while (p->pos < p->len && isdigit((unsigned char)p->input[p->pos])) {
        n = n * 10 + (p->input[p->pos] - '0');
        p->pos++;
    }
    return n;
}

static char* parse_identifier(parser_t* p) {
    skip_whitespace(p);
    size_t start = p->pos;
    while (p->pos < p->len && (isalpha((unsigned char)p->input[p->pos]) || p->input[p->pos] == '_')) {
        p->pos++;
    }
    if (p->pos == start) return NULL;
    size_t slen = p->pos - start;
    char* s = (char*)get_memory(slen + 1);
    memcpy(s, p->input + start, slen);
    s[slen] = '\0';
    return s;
}

/* ── Deep copy a step chain (for morphism inlining) ── */

static query_step_t* copy_steps(query_step_t* src) {
    if (!src) return NULL;
    query_step_t* head = NULL;
    query_step_t* tail = NULL;
    while (src) {
        query_step_t* s = (query_step_t*)get_clear_memory(sizeof(query_step_t));
        s->type = src->type;
        s->limit = src->limit;
        if (src->vertex_id) s->vertex_id = strdup(src->vertex_id);
        if (src->predicate) s->predicate = strdup(src->predicate);
        if (src->has_predicate) s->has_predicate = strdup(src->has_predicate);
        if (src->has_value) s->has_value = strdup(src->has_value);
        if (src->num_children > 0) {
            s->num_children = src->num_children;
            s->children = (query_step_t**)get_clear_memory(s->num_children * sizeof(query_step_t*));
            for (size_t i = 0; i < s->num_children; i++) {
                s->children[i] = copy_steps(src->children[i]);
            }
        }
        if (tail) tail->next = s;
        else head = s;
        tail = s;
        src = src->next;
    }
    return head;
}

/* ── Step dispatcher ── */

static int parse_step(parser_t* p, graph_query_t* q) {
    char* ident = parse_identifier(p);
    if (!ident) {
        set_error_expect(p, "method name");
        return 0;
    }

    int rc = 1;
    if (strcmp(ident, "V") == 0) {
        if (!expect_char(p, '(')) goto done;
        char* id = parse_string(p);
        if (!id) goto done;
        if (!expect_char(p, ')')) { free(id); goto done; }
        rc = graph_query_vertex(q, id);
        free(id);
    } else if (strcmp(ident, "Out") == 0) {
        if (!expect_char(p, '(')) goto done;
        char* pred = parse_string(p);
        if (!pred) goto done;
        if (!expect_char(p, ')')) { free(pred); goto done; }
        rc = graph_query_out(q, pred);
        free(pred);
    } else if (strcmp(ident, "In") == 0) {
        if (!expect_char(p, '(')) goto done;
        char* pred = parse_string(p);
        if (!pred) goto done;
        if (!expect_char(p, ')')) { free(pred); goto done; }
        rc = graph_query_in(q, pred);
        free(pred);
    } else if (strcmp(ident, "Has") == 0) {
        if (!expect_char(p, '(')) goto done;
        char* pred = parse_string(p);
        if (!pred) goto done;
        if (!expect_char(p, ',')) { free(pred); goto done; }
        char* val = parse_string(p);
        if (!val) { free(pred); goto done; }
        if (!expect_char(p, ')')) { free(pred); free(val); goto done; }
        // Create HAS step manually
        query_step_t* s = (query_step_t*)get_clear_memory(sizeof(query_step_t));
        s->type = GRAPH_STEP_HAS;
        s->has_predicate = pred;
        s->has_value = val;
        rc = 0;
        // Append to query manually
        if (q->tail) q->tail->next = s;
        else q->head = s;
        q->tail = s;
    } else if (strcmp(ident, "And") == 0) {
        if (!expect_char(p, '(')) goto done;
        // Parse sub-query inside And()
        graph_query_t* child = graph_query_create(q->layer);
        if (!parse_query(p, child)) { graph_query_destroy(child); goto done; }
        if (!expect_char(p, ')')) { graph_query_destroy(child); goto done; }
        // Create INTERSECT step with the sub-query as its only child
        query_step_t* s = (query_step_t*)get_clear_memory(sizeof(query_step_t));
        s->type = GRAPH_STEP_INTERSECT;
        s->num_children = 1;
        s->children = (query_step_t**)get_clear_memory(sizeof(query_step_t*));
        s->children[0] = child->head;
        child->head = NULL; // Transfer ownership
        if (q->tail) q->tail->next = s;
        else q->head = s;
        q->tail = s;
        graph_query_destroy(child);
        rc = 0;
    } else if (strcmp(ident, "Or") == 0) {
        if (!expect_char(p, '(')) goto done;
        graph_query_t* child = graph_query_create(q->layer);
        if (!parse_query(p, child)) { graph_query_destroy(child); goto done; }
        if (!expect_char(p, ')')) { graph_query_destroy(child); goto done; }
        // Create UNION step with the sub-query as its only child
        query_step_t* s = (query_step_t*)get_clear_memory(sizeof(query_step_t));
        s->type = GRAPH_STEP_UNION;
        s->num_children = 1;
        s->children = (query_step_t**)get_clear_memory(sizeof(query_step_t*));
        s->children[0] = child->head;
        child->head = NULL;
        if (q->tail) q->tail->next = s;
        else q->head = s;
        q->tail = s;
        graph_query_destroy(child);
        rc = 0;
    } else if (strcmp(ident, "Limit") == 0 || strcmp(ident, "GetLimit") == 0) {
        if (!expect_char(p, '(')) goto done;
        long n = parse_number(p);
        if (n < 0) goto done;
        if (!expect_char(p, ')')) goto done;
        rc = graph_query_limit(q, (size_t)n);
    } else if (strcmp(ident, "All") == 0) {
        if (!expect_char(p, '(')) goto done;
        if (!expect_char(p, ')')) goto done;
        rc = 0; // All() is terminal — no-op, query runs normally
    } else if (strcmp(ident, "Count") == 0) {
        if (!expect_char(p, '(')) goto done;
        if (!expect_char(p, ')')) goto done;
        rc = 0; // Count() is handled by graph_parse_count wrapper
    } else if (strcmp(ident, "Follow") == 0) {
        if (!expect_char(p, '(')) goto done;
        char* name = parse_string(p);
        if (!name) goto done;
        if (!expect_char(p, ')')) { free(name); goto done; }

        // Look up morphism
        morphism_entry_t* found = NULL;
        for (size_t i = 0; i < p->layer->morphism_count; i++) {
            if (strcmp(p->layer->morphisms[i].name, name) == 0) {
                found = &p->layer->morphisms[i];
                break;
            }
        }
        if (!found) {
            char buf[256];
            snprintf(buf, sizeof(buf), "Unknown morphism '%s'", name);
            set_error(p, buf);
            free(name);
            goto done;
        }
        free(name);

        // Deep-copy the morphism's steps into the current query
        query_step_t* copied = copy_steps(found->steps);
        if (copied) {
            if (q->tail) q->tail->next = copied;
            else q->head = copied;
            query_step_t* t = copied;
            while (t->next) t = t->next;
            q->tail = t;
        }
        rc = 0;
    } else {
        char buf[256];
        snprintf(buf, sizeof(buf), "Unknown method '%s'", ident);
        set_error(p, buf);
        goto done;
    }

done:
    free(ident);
    return rc == 0;
}

/* ── Query parser: g.step().step()... ── */

static int parse_query(parser_t* p, graph_query_t* q) {
    // Expect "g"
    char* ident = parse_identifier(p);
    if (!ident) { set_error_expect(p, "g"); return 0; }
    if (strcmp(ident, "g") != 0) {
        char buf[128];
        snprintf(buf, sizeof(buf), "Expected 'g' but found '%s'", ident);
        set_error(p, buf);
        free(ident);
        return 0;
    }
    free(ident);

    // Expect "."
    if (!expect_char(p, '.')) return 0;

    // Parse steps
    while (p->pos < p->len) {
        int c = peek(p);
        if (c == EOF || c == ')') break; // End of query (reached by And/Or sub-query)

        if (!parse_step(p, q)) return 0;

        // Check for next '.' or end
        skip_whitespace(p);
        if (p->pos < p->len && p->input[p->pos] == '.') {
            p->pos++;
        } else if (p->pos < p->len && p->input[p->pos] != ')') {
            // Not a dot and not a closing paren — error
            set_error_expect(p, "'.'");
            return 0;
        }
    }

    return 1;
}

/* ── Public API (declared in graph_internal.h) ── */

graph_query_t* graph_parse_query(const char* input, size_t len, graph_layer_t* layer, graph_parse_error_t* error) {
    parser_t p;
    memset(&p, 0, sizeof(p));
    p.input = input;
    p.pos = 0;
    p.len = len;
    p.layer = layer;
    p.error = error;

    if (error) {
        error->ok = 1;
        error->message[0] = '\0';
        error->position = 0;
    }

    graph_query_t* q = graph_query_create(layer);
    if (!q) {
        set_error(&p, "Failed to create query");
        return NULL;
    }

    if (len == 0) {
        set_error(&p, "Empty query");
        graph_query_destroy(q);
        return NULL;
    }

    if (!parse_query(&p, q)) {
        graph_query_destroy(q);
        return NULL;
    }

    return q;
}

int graph_morphism_parse_and_store(graph_layer_t* layer, const char* name,
                                    const char* input, size_t len,
                                    graph_parse_error_t* error) {
    // Parse: g.Morphism("name").Out("pred").In("val")...
    // We consume g.Morphism("name") then parse the remaining steps.

    parser_t p;
    memset(&p, 0, sizeof(p));
    p.input = input;
    p.pos = 0;
    p.len = len;
    p.layer = layer;
    p.error = error;

    if (error) {
        error->ok = 1;
        error->message[0] = '\0';
        error->position = 0;
    }

    // Expect "g"
    char* ident = parse_identifier(&p);
    if (!ident || strcmp(ident, "g") != 0) {
        free(ident);
        set_error(&p, "Morphism definition must start with 'g.Morphism(\"name\")...'");
        return -1;
    }
    free(ident);

    if (!expect_char(&p, '.')) return -1;

    // Expect "Morphism"
    char* mname = parse_identifier(&p);
    if (!mname || strcmp(mname, "Morphism") != 0) {
        free(mname);
        set_error(&p, "Expected 'Morphism'");
        return -1;
    }
    free(mname);

    if (!expect_char(&p, '(')) return -1;

    // Parse the name
    char* parsed_name = parse_string(&p);
    if (!parsed_name) return -1;
    // Verify name matches
    if (strcmp(parsed_name, name) != 0) {
        char buf[256];
        snprintf(buf, sizeof(buf), "Morphism name mismatch: '%s' vs '%s'", parsed_name, name);
        set_error(&p, buf);
        free(parsed_name);
        return -1;
    }
    free(parsed_name);

    if (!expect_char(&p, ')')) return -1;

    // Parse remaining steps into a temporary query
    graph_query_t* q = graph_query_create(layer);
    if (!q) {
        set_error(&p, "Failed to create query for morphism");
        return -1;
    }

    while (p.pos < p.len) {
        if (!expect_char(&p, '.')) { graph_query_destroy(q); return -1; }
        if (!parse_step(&p, q)) { graph_query_destroy(q); return -1; }
    }

    // Store on layer
    if (layer->morphism_count >= layer->morphism_capacity) {
        size_t new_cap = layer->morphism_capacity ? layer->morphism_capacity * 2 : 4;
        morphism_entry_t* new_m = (morphism_entry_t*)get_clear_memory(new_cap * sizeof(morphism_entry_t));
        if (layer->morphisms) {
            memcpy(new_m, layer->morphisms, layer->morphism_count * sizeof(morphism_entry_t));
            free(layer->morphisms);
        }
        layer->morphisms = new_m;
        layer->morphism_capacity = new_cap;
    }

    size_t idx = layer->morphism_count;
    layer->morphisms[idx].name = strdup(name);
    layer->morphisms[idx].steps = q->head;
    q->head = NULL; // Transfer ownership
    layer->morphism_count++;

    graph_query_destroy(q);
    return 0;
}
