/**
 * Minimal test for MVCC version chain recovery
 *
 * This test isolates the issue where version chains are not created
 * during WAL recovery.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "src/HBTrie/hbtrie.h"
#include "src/HBTrie/path.h"
#include "src/HBTrie/identifier.h"
#include "src/HBTrie/chunk.h"
#include "src/Workers/transaction_id.h"
#include "src/Util/allocator.h"

void print_identifier(const char* label, identifier_t* id) {
    printf("%s: length=%zu, chunks=%d, chunk_size=%d\n",
           label, id->length, id->chunks.length, id->chunk_size);
    for (int i = 0; i < id->chunks.length; i++) {
        chunk_t* chunk = id->chunks.data[i];
        printf("  Chunk %d: ", i);
        for (int j = 0; j < chunk->size; j++) {
            printf("%02x", chunk->data[j]);
        }
        printf("\n");
    }
}

int main() {
    printf("=== MVCC Recovery Test ===\n\n");

    // Initialize transaction ID generator
    transaction_id_init();

    // Create HBTrie with default chunk size
    uint8_t chunk_size = 4;
    uint32_t btree_node_size = 4096;
    hbtrie_t* trie = hbtrie_create(chunk_size, btree_node_size);
    assert(trie != NULL);
    printf("Created trie with chunk_size=%d, btree_node_size=%d\n\n", chunk_size, btree_node_size);

    // Create two transaction IDs (simulating two writes)
    transaction_id_t txn1 = transaction_id_get_next();
    transaction_id_t txn2 = transaction_id_get_next();
    printf("Txn1: %lu.%09lu.%lu\n", txn1.time, txn1.nanos, txn1.count);
    printf("Txn2: %lu.%09lu.%lu\n\n", txn2.time, txn2.nanos, txn2.count);

    // Create path for "key0"
    path_t* path = path_create();
    assert(path != NULL);

    // Create identifier from string "key0"
    char* key_str = "key0";
    size_t key_len = strlen(key_str);

    // Manually create identifier with chunk_size=4
    identifier_t* id1 = identifier_create(key_len, chunk_size);
    assert(id1 != NULL);
    memcpy(id1->chunks.data[0]->data, key_str, key_len);
    id1->chunks.data[0]->size = chunk_size;

    path_append(path, id1);
    printf("Created path with identifier:\n");
    print_identifier("key0", id1);
    printf("\n");

    // Create two values
    char* val1_str = "value0";
    char* val2_str = "value0b";

    identifier_t* value1 = identifier_create(strlen(val1_str), chunk_size);
    assert(value1 != NULL);
    memcpy(value1->chunks.data[0]->data, val1_str, strlen(val1_str));

    identifier_t* value2 = identifier_create(strlen(val2_str), chunk_size);
    assert(value2 != NULL);
    memcpy(value2->chunks.data[0]->data, val2_str, strlen(val2_str));

    printf("Created value1:\n");
    print_identifier("value0", value1);
    printf("\nCreated value2:\n");
    print_identifier("value0b", value2);
    printf("\n");

    // Test 1: First insert (should create new entry)
    printf("=== Insert 1: key0 = value0 (txn1) ===\n");
    int result1 = hbtrie_insert_mvcc(trie, path, value1, txn1);
    printf("Result: %d\n\n", result1);
    assert(result1 == 0);

    // Test 2: Second insert (should upgrade to version chain)
    printf("=== Insert 2: key0 = value0b (txn2) ===\n");
    int result2 = hbtrie_insert_mvcc(trie, path, value2, txn2);
    printf("Result: %d\n\n", result2);
    assert(result2 == 0);

    // Test 3: Read with txn2 (should get value2)
    printf("=== Find: key0 (read with txn2) ===\n");
    transaction_id_t read_txn = txn2;
    identifier_t* found_value = hbtrie_find_mvcc(trie, path, read_txn);

    if (found_value == NULL) {
        printf("ERROR: No value found!\n");
        return 1;
    }

    printf("Found value (length=%zu):\n", found_value->length);
    print_identifier("found", found_value);
    printf("\n");

    // Check if it's the correct value
    char found_str[256] = {0};
    size_t copy_len = found_value->length < 255 ? found_value->length : 255;
    memcpy(found_str, found_value->chunks.data[0]->data, copy_len);
    found_str[copy_len] = '\0';

    printf("Found string: '%s'\n", found_str);
    printf("Expected: '%s'\n\n", val2_str);

    if (strcmp(found_str, val2_str) != 0) {
        printf("FAILED: Got '%s', expected '%s'\n", found_str, val2_str);
        printf("This indicates the MVCC version chain was NOT created properly!\n");

        // Try reading with txn1 to see if we get value1
        printf("\n=== Find: key0 (read with txn1) ===\n");
        identifier_t* found_value1 = hbtrie_find_mvcc(trie, path, txn1);
        if (found_value1 != NULL) {
            char found_str1[256] = {0};
            copy_len = found_value1->length < 255 ? found_value1->length : 255;
            memcpy(found_str1, found_value1->chunks.data[0]->data, copy_len);
            printf("With txn1, got: '%s'\n", found_str1);
        }

        return 1;
    }

    printf("SUCCESS: Version chain working correctly!\n");

    // Cleanup
    path_destroy(path);
    identifier_destroy(id1);
    identifier_destroy(value1);
    identifier_destroy(value2);
    hbtrie_destroy(trie);

    return 0;
}