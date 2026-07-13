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

#if _WIN32
#include <io.h>
#include <direct.h>
#include <process.h>
#define getpid() _getpid()
#define mkdir(path, mode) _mkdir(path)
#else
#include <unistd.h>
#include <sys/stat.h>
#endif

extern "C" {
#include "../src/HBTrie/hbtrie.h"
#include "../src/HBTrie/mvcc.h"
#include "../src/HBTrie/path.h"
#include "../src/HBTrie/identifier.h"
#include "../src/Buffer/buffer.h"
#include "../src/Workers/transaction_id.h"
#include "../src/Database/database.h"
#include "../src/Database/database_config.h"
#include "../src/Database/database_iterator.h"
}

static size_t rev_test_counter = 0;

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
// value-bearing entry per inserted key IN STRICTLY DESCENDING ORDER.
// Verifies the rightmost-descent logic crosses multiple trie levels
// without double-yielding or skipping, and that the order is correct.
// We compare the FIRST chunk of each yielded key (read from the root
// frame) rather than the leaf chunk, because two keys ("cherry" and
// "elderberry") share the same last chunk "ry\0\0" — leaf chunks alone
// can't distinguish their order.
TEST_F(HBTrieReverseTest, CursorPrevMultiChunkCount) {
    const char* keys[] = {"apple", "banana", "cherry", "date", "elderberry"};
    for (const char* k : keys) {
        insert_key(k);
    }

    hbtrie_cursor_t rev;
    hbtrie_cursor_init_reverse(&rev, trie);
    size_t rev_count = 0;
    std::vector<std::string> first_chunks;
    while (hbtrie_cursor_prev(&rev) == 0) {
        bnode_entry_t* e = hbtrie_cursor_get_entry(&rev);
        ASSERT_NE(e, nullptr);
        ASSERT_EQ(e->has_value, 1);
        // Read the root frame's current entry to get the first chunk of
        // the yielded key. reverse_push_child / hbtrie_cursor_prev set
        // stack[0].entry_index to (this_index - 1) on regular descents,
        // so entry_index + 1 recovers the root entry we descended from.
        // (For stack_depth==1 yields like "date", the same +1 recovers
        // the yielded root entry itself.)
        bnode_t* root_btree = rev.stack[0].node->btree;
        size_t root_count = bnode_count(root_btree);
        size_t root_idx = rev.stack[0].entry_index + 1;
        ASSERT_LT(root_idx, root_count);
        bnode_entry_t* root_entry = bnode_get(root_btree, root_idx);
        ASSERT_NE(root_entry, nullptr);
        ASSERT_NE(root_entry->key, nullptr);
        first_chunks.emplace_back((const char*)root_entry->key->data,
                                  root_entry->key->size);
        rev_count++;
    }

    EXPECT_EQ(rev_count, 5u);
    // Expected descending first-chunk order: elde > date > cher > bana > appl.
    std::vector<std::string> expected = {
        std::string("elde", 4),
        std::string("date", 4),
        std::string("cher", 4),
        std::string("bana", 4),
        std::string("appl", 4),
    };
    ASSERT_EQ(first_chunks.size(), expected.size());
    EXPECT_EQ(first_chunks, expected);
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
    // The value_pending mechanism must emit the root value AFTER the subtree.
    // Asserting the ORDER proves the two-visit logic works — without this,
    // the test would pass even if the value were emitted before the subtree
    // (which would be wrong).
    ASSERT_EQ(got.size(), 2u);
    // got[0]: leaf chunk of "apple" — "e" zero-padded to chunk_size=4.
    EXPECT_EQ(got[0], std::string("e\0\0\0", 4));
    // got[1]: root value chunk for "appl" (emitted on value_pending pop-back).
    EXPECT_EQ(got[1], std::string("appl", 4));
}

// value_pending at an INTERMEDIATE trie level (not the root).
//
// Keys (chunk_size=4):
//   "aaaabbbb"   -> chunks ["aaaa", "bbbb"]            (2 chunks)
//   "aaaabbbbX"  -> chunks ["aaaa", "bbbb", "X\0\0\0"] (3 chunks)
//   "aaaabbbbY"  -> chunks ["aaaa", "bbbb", "Y\0\0\0"] (3 chunks)
//   "aaaacccc"   -> chunks ["aaaa", "cccc"]            (2 chunks)
//
// Trie shape:
//   root: entry "aaaa" (has_value=0, trie_child != NULL)  -- regular descent
//     depth 1: entries "bbbb" and "cccc"
//       "cccc": has_value=1, no child         -> plain leaf ("aaaacccc")
//       "bbbb": has_value=1 + trie_child       -> INTERMEDIATE value_pending
//         depth 2: entries "X\0\0\0", "Y\0\0\0" (both plain leaves)
//
// This forces the value_pending two-visit logic to fire at depth 1: the
// "bbbb" entry's subtree ("aaaabbbbY", "aaaabbbbX") must be exhausted
// BEFORE the "bbbb" value ("aaaabbbb") is emitted, then iteration continues
// to siblings. CursorPrevPrefixShared only exercises value_pending at the
// root; this test covers the non-root case.
//
// Expected descending full-key order:
//   "aaaacccc" > "aaaabbbbY" > "aaaabbbbX" > "aaaabbbb"
//
// We assert both the leaf chunk and the stack depth at each yield, which
// uniquely identifies which key was emitted and at which trie level.
TEST_F(HBTrieReverseTest, CursorPrevValueWithTrieChildIntermediateLevel) {
    insert_key("aaaabbbb");
    insert_key("aaaabbbbX");
    insert_key("aaaabbbbY");
    insert_key("aaaacccc");

    hbtrie_cursor_t cursor;
    hbtrie_cursor_init_reverse(&cursor, trie);

    // Collect (leaf_chunk_bytes, stack_depth) for each yielded entry.
    std::vector<std::pair<std::string, size_t>> got;
    while (hbtrie_cursor_prev(&cursor) == 0) {
        bnode_entry_t* e = hbtrie_cursor_get_entry(&cursor);
        ASSERT_NE(e, nullptr);
        ASSERT_EQ(e->has_value, 1);
        ASSERT_NE(e->key, nullptr);
        ASSERT_GE(e->key->size, 1u);
        std::string k((const char*)e->key->data, e->key->size);
        got.emplace_back(k, cursor.stack_depth);
    }

    ASSERT_EQ(got.size(), 4u);

    // 1) "aaaacccc" -> leaf chunk "cccc" at depth 1 (stack_depth=2).
    EXPECT_EQ(got[0].first, std::string("cccc", 4));
    EXPECT_EQ(got[0].second, 2u);

    // 2) "aaaabbbbY" -> leaf chunk "Y\0\0\0" at depth 2 (stack_depth=3).
    static const char kY[] = {'Y', 0, 0, 0};
    EXPECT_EQ(got[1].first, std::string(kY, 4));
    EXPECT_EQ(got[1].second, 3u);

    // 3) "aaaabbbbX" -> leaf chunk "X\0\0\0" at depth 2 (stack_depth=3).
    static const char kX[] = {'X', 0, 0, 0};
    EXPECT_EQ(got[2].first, std::string(kX, 4));
    EXPECT_EQ(got[2].second, 3u);

    // 4) "aaaabbbb" -> leaf chunk "bbbb" at depth 1 (stack_depth=2),
    //    emitted by the value_pending pop-back AFTER the subtree was
    //    exhausted. This is the key assertion: if value_pending were
    //    broken, "bbbb" would appear before "Y"/"X" (or not at all).
    EXPECT_EQ(got[3].first, std::string("bbbb", 4));
    EXPECT_EQ(got[3].second, 2u);
}

// Task 3: Database-level reverse scan with no bounds and single-identifier
// keys. database_scan_start_reverse + database_scan_prev must yield the 5
// inserted keys in strictly descending order. Keys are inserted via the raw
// API with a NUL delimiter (so each key is a single path identifier);
// multi-chunk keys (e.g. "elderberry" = 3 chunks at chunk_size=4) exercise
// the rightmost-descent across trie levels.
class DatabaseReverseTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_dir = "/tmp/wavedb_dbrev_" + std::to_string(getpid()) + "_" +
                   std::to_string(rev_test_counter++);
        mkdir(test_dir.c_str(), 0700);
    }
    void TearDown() override {
        std::string cmd = "rm -rf " + test_dir;
        system(cmd.c_str());
    }
    std::string test_dir;
};

TEST_F(DatabaseReverseTest, BasicDescendingOrder) {
    database_config_t* cfg = database_config_default();
    ASSERT_NE(cfg, nullptr);
    database_config_set_sync_only(cfg, 1);
    int err = 0;
    database_t* db = database_create_with_config(test_dir.c_str(), cfg, &err);
    ASSERT_NE(db, nullptr);
    ASSERT_EQ(err, 0);

    const char* keys[] = {"apple", "banana", "cherry", "date", "elderberry"};
    for (const char* k : keys) {
        size_t klen = strlen(k);
        int rc = database_put_sync_raw(db, k, klen, '\0',
                                       (const uint8_t*)"V", 1);
        ASSERT_EQ(rc, 0);
    }

    database_iterator_t* it = database_scan_start_reverse(db, NULL, NULL);
    ASSERT_NE(it, nullptr);

    std::vector<std::string> got;
    path_t* p = nullptr;
    identifier_t* v = nullptr;
    while (database_scan_prev(it, &p, &v) == 0) {
        ASSERT_NE(p, nullptr);
        ASSERT_GE(path_length(p), 1u);
        identifier_t* id = path_get(p, 0);
        ASSERT_NE(id, nullptr);
        size_t dlen = 0;
        uint8_t* data = identifier_get_data_copy(id, &dlen);
        ASSERT_NE(data, nullptr);
        got.push_back(std::string((const char*)data, dlen));
        free(data);
        path_destroy(p);
        identifier_destroy(v);
        p = nullptr; v = nullptr;
    }
    database_scan_end(it);

    std::vector<std::string> expected = {
        "elderberry", "date", "cherry", "banana", "apple"
    };
    ASSERT_EQ(got, expected);

    database_destroy(db);
    database_config_destroy(cfg);
}
