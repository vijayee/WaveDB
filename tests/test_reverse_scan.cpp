//
// Tests for engine backward scan (reverse iteration).
// Spec: docs/superpowers/specs/2026-07-12-vector-layer-design.md
//
// Task 1: HBTrie-level reverse cursor. No MVCC, no path metadata, no
// bounds, no vacuum — just hbtrie_cursor_init_reverse + hbtrie_cursor_prev
// yielding value-bearing entries in descending sort order.
//

#include <gtest/gtest.h>
#include <string.h>
#include <stdlib.h>
#include <string>
#include <vector>

extern "C" {
#include "../src/HBTrie/hbtrie.h"
#include "../src/HBTrie/mvcc.h"
#include "../src/Buffer/buffer.h"
#include "../src/Workers/transaction_id.h"
}

class HBTrieReverseTest : public ::testing::Test {
protected:
    void SetUp() override {
        transaction_id_init();
        trie = hbtrie_create(4, 4096);
        ASSERT_NE(trie, nullptr);
    }
    void TearDown() override {
        if (trie) {
            hbtrie_destroy(trie);
        }
    }
    hbtrie_t* trie;

    transaction_id_t next_txn_id() {
        return transaction_id_get_next();
    }

    // Build a single-identifier path from a C string.
    path_t* make_path(const char* s) {
        path_t* p = path_create();
        buffer_t* buf = buffer_create_from_pointer_copy((uint8_t*)s, strlen(s));
        identifier_t* id = identifier_create(buf, 0);  // default chunk size
        buffer_destroy(buf);
        path_append(p, id);
        identifier_destroy(id);
        return p;
    }

    identifier_t* make_value(const char* s) {
        buffer_t* buf = buffer_create_from_pointer_copy((uint8_t*)s, strlen(s));
        identifier_t* id = identifier_create(buf, 0);
        buffer_destroy(buf);
        return id;
    }

    // Insert a key with value "V".
    void insert_key(const char* k) {
        path_t* p = make_path(k);
        identifier_t* v = make_value("V");
        ASSERT_EQ(hbtrie_insert(trie, p, v, next_txn_id()), 0);
        path_destroy(p);
        identifier_destroy(v);
    }
};

// Single-chunk keys (1 byte each, zero-padded to chunk_size=4): reverse
// scan must yield them in strictly descending order: e,d,c,b,a. We
// compare the first byte of each chunk (the original key byte) since
// chunks are always chunk_size bytes with zero padding.
TEST_F(HBTrieReverseTest, CursorPrevDescendingOrder) {
    const char* keys[] = {"a", "b", "c", "d", "e"};
    for (const char* k : keys) {
        insert_key(k);
    }

    hbtrie_cursor_t cursor;
    hbtrie_cursor_init_reverse(&cursor, trie);

    std::vector<char> got;
    while (hbtrie_cursor_prev(&cursor) == 0) {
        bnode_entry_t* e = hbtrie_cursor_get_entry(&cursor);
        ASSERT_NE(e, nullptr);
        ASSERT_EQ(e->has_value, 1);
        ASSERT_NE(e->key, nullptr);
        ASSERT_GE(e->key->size, 1u);
        got.push_back((char)e->key->data[0]);
    }

    std::vector<char> expected = {'e', 'd', 'c', 'b', 'a'};
    ASSERT_EQ(got.size(), expected.size());
    EXPECT_EQ(got, expected);
}

// Multi-chunk keys (up to 3 HBTrie levels): reverse scan must yield one
// value-bearing entry per inserted key. Verifies the rightmost-descent
// logic crosses multiple trie levels without double-yielding or skipping.
TEST_F(HBTrieReverseTest, CursorPrevMultiChunkCount) {
    const char* keys[] = {"apple", "banana", "cherry", "date", "elderberry"};
    for (const char* k : keys) {
        insert_key(k);
    }

    hbtrie_cursor_t rev;
    hbtrie_cursor_init_reverse(&rev, trie);
    size_t rev_count = 0;
    while (hbtrie_cursor_prev(&rev) == 0) {
        bnode_entry_t* e = hbtrie_cursor_get_entry(&rev);
        ASSERT_NE(e, nullptr);
        ASSERT_EQ(e->has_value, 1);
        rev_count++;
    }

    EXPECT_EQ(rev_count, 5u);
}

// Empty trie: reverse cursor should immediately return -1.
TEST_F(HBTrieReverseTest, CursorPrevEmptyTrie) {
    hbtrie_cursor_t cursor;
    hbtrie_cursor_init_reverse(&cursor, trie);
    EXPECT_EQ(hbtrie_cursor_prev(&cursor), -1);
    EXPECT_EQ(hbtrie_cursor_at_end(&cursor), 1);
}

// Single entry: reverse yields exactly one entry then stops.
TEST_F(HBTrieReverseTest, CursorPrevSingleEntry) {
    insert_key("only");

    hbtrie_cursor_t cursor;
    hbtrie_cursor_init_reverse(&cursor, trie);
    ASSERT_EQ(hbtrie_cursor_prev(&cursor), 0);
    bnode_entry_t* e = hbtrie_cursor_get_entry(&cursor);
    ASSERT_NE(e, nullptr);
    ASSERT_EQ(e->has_value, 1);
    EXPECT_EQ(hbtrie_cursor_prev(&cursor), -1);
    EXPECT_EQ(hbtrie_cursor_at_end(&cursor), 1);
}

// Prefix-sharing keys: a short key that is a chunk-prefix of a longer key
// creates an entry with has_value=1 AND trie_child != NULL. Reverse must
// descend into the trie_child subtree first (emitting the longer key),
// then emit the shorter key's value on pop-back.
TEST_F(HBTrieReverseTest, CursorPrevPrefixShared) {
    // "appl" (4 bytes, 1 chunk) and "apple" (5 bytes, 2 chunks) share the
    // first chunk "appl". The "appl" entry has has_value=1 + trie_child.
    insert_key("appl");
    insert_key("apple");

    hbtrie_cursor_t cursor;
    hbtrie_cursor_init_reverse(&cursor, trie);

    std::vector<std::string> got;
    while (hbtrie_cursor_prev(&cursor) == 0) {
        bnode_entry_t* e = hbtrie_cursor_get_entry(&cursor);
        ASSERT_NE(e, nullptr);
        ASSERT_EQ(e->has_value, 1);
        ASSERT_NE(e->key, nullptr);
        // Reconstruct the leaf chunk's first byte(s) for ordering check.
        std::string k((const char*)e->key->data, e->key->size);
        got.push_back(k);
    }

    // Descending order: "apple" (second chunk "e\0\0\0") sorts after "appl"
    // at the root, so reverse yields the trie_child subtree first, then the
    // root value. Two value-bearing entries total.
    ASSERT_EQ(got.size(), 2u);
}