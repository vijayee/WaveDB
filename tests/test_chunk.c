//
// Test for chunk_t
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "HBTrie/chunk.h"

int test_chunk_create() {
    const char* data = "test";
    chunk_t* chunk = chunk_create(data, 4);
    if (chunk == NULL) {
        printf("FAIL: chunk_create returned NULL\n");
        return -1;
    }
    if (chunk->size != 4) {
        printf("FAIL: chunk size is %zu, expected 4\n", chunk->size);
        chunk_destroy(chunk);
        return -1;
    }
    if (memcmp(chunk->data, "test", 4) != 0) {
        printf("FAIL: chunk data doesn't match\n");
        chunk_destroy(chunk);
        return -1;
    }
    chunk_destroy(chunk);
    printf("PASS: test_chunk_create\n");
    return 0;
}

int test_chunk_compare() {
    chunk_t* a = chunk_create("abcd", 4);
    chunk_t* b = chunk_create("abcd", 4);
    chunk_t* c = chunk_create("abce", 4);
    chunk_t* d = chunk_create("abc", 4);

    if (a == NULL || b == NULL || c == NULL || d == NULL) {
        printf("FAIL: chunk_create returned NULL\n");
        return -1;
    }

    int cmp;

    cmp = chunk_compare(a, b);
    if (cmp != 0) {
        printf("FAIL: chunk_compare equal returned %d, expected 0\n", cmp);
        return -1;
    }

    cmp = chunk_compare(a, c);
    if (cmp >= 0) {
        printf("FAIL: chunk_compare 'abcd' < 'abce' returned %d, expected <0\n", cmp);
        return -1;
    }

    cmp = chunk_compare(c, a);
    if (cmp <= 0) {
        printf("FAIL: chunk_compare 'abce' > 'abcd' returned %d, expected >0\n", cmp);
        return -1;
    }

    chunk_destroy(a);
    chunk_destroy(b);
    chunk_destroy(c);
    chunk_destroy(d);

    printf("PASS: test_chunk_compare\n");
    return 0;
}

int main() {
    int result = 0;
    result |= test_chunk_create();
    result |= test_chunk_compare();

    if (result == 0) {
        printf("All chunk tests passed!\n");
    } else {
        printf("Some chunk tests failed!\n");
    }

    return result;
}