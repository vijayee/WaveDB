#include <stdio.h>
#include <stddef.h>

// Minimal declarations to check sizes
typedef struct {
    int a;
} refcounter_t;

typedef struct {
    int length;
} buffer_t;

typedef struct {
    buffer_t* data;
} chunk_t;

typedef vec_t chunk_t;
typedef struct {
    refcounter_t refcounter;
    vec_t chunks;
    size_t length;
    size_t chunk_size;
} identifier_t;

typedef struct {
    refcounter_t refcounter;
    chunk_t* key;
    union {
        struct hbtrie_node_t* child;
        identifier_t* value;
        struct version_entry_t* versions;
    };
    uint8_t has_value;
    uint8_t has_versions;
    // Transaction ID and storage fields...
} bnode_entry_t;

int main() {
    printf("sizeof(identifier_t*) = %zu\n", sizeof(identifier_t*));
    printf("sizeof(version_entry_t*) = %zu\n", sizeof(void*));
    printf("sizeof(bnode_entry_t) = %zu\n", sizeof(bnode_entry_t));
    printf("Offset of union in bnode_entry_t: %zu\n", offsetof(bnode_entry_t, has_value));
    return 0;
}
