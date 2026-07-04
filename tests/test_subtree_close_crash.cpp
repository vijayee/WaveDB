// Test: closing a persistent db after subtree insert + root batch delete
// must not crash. Reproduces the "free(): invalid pointer" crash observed via
// the graph layer's expandTriple(delete=True) + batchSync path.

#include <gtest/gtest.h>

extern "C" {
#include "Database/database.h"
#include "Database/database_config.h"
#include "Database/database_subtree.h"
#include "Database/database_iterator.h"
#include "HBTrie/hbtrie.h"
#include "HBTrie/path.h"
#include "HBTrie/identifier.h"
#include "Buffer/buffer.h"
#include "Util/allocator.h"
#include "Layers/graph/graph.h"
}

#if _WIN32
#include <io.h>
#include <direct.h>
#define getpid() _getpid()
#define mkdir(path, mode) _mkdir(path)
#else
#include <unistd.h>
#include <sys/stat.h>
#endif
#include <cstdlib>
#include <cstring>
#include <cstdio>

class SubtreeCloseCrashTest : public ::testing::Test {
protected:
    char tmpdir[256];

    void SetUp() override {
#if _WIN32
        strcpy(tmpdir, getenv("TEMP"));
        strcat(tmpdir, "/wavedb_st_close_XXXXXX");
        _mktemp(tmpdir);
        _mkdir(tmpdir);
#else
        strcpy(tmpdir, "/tmp/wavedb_st_close_XXXXXX");
        mkdtemp(tmpdir);
#endif
    }

    void TearDown() override {
        char cmd[512];
#if _WIN32
        snprintf(cmd, sizeof(cmd), "rmdir /s /q %s", tmpdir);
#else
        snprintf(cmd, sizeof(cmd), "rm -rf %s", tmpdir);
#endif
        system(cmd);
    }

    database_t* create_db() {
        database_config_t* config = database_config_default();
        config->chunk_size = 4;
        config->btree_node_size = 4096;
        config->worker_threads = 2;
        config->enable_persist = 1;
        config->timer_resolution_ms = 100;
        int error_code = 0;
        database_t* result = database_create_with_config(tmpdir, config, &error_code);
        database_config_destroy(config);
        return result;
    }
};

// Reproduce: insert via subtree, delete via root batch, close db.
// This is the exact sequence that trips "free(): invalid pointer" in db.close.
TEST_F(SubtreeCloseCrashTest, SubtreeInsertThenRootBatchDeleteThenClose) {
    database_t* db = create_db();
    ASSERT_NE(db, nullptr);

    database_subtree_t* st = database_subtree_open(db, "graph", '/');
    ASSERT_NE(st, nullptr);

    // Insert via subtree batch (mirrors graph_insert_sync's path).
    // Key: "/spo/alice/knows/bob" (with leading "/") — subtree prepends "graph/".
    raw_op_t put_ops[1];
    put_ops[0].type = 0;
    put_ops[0].key = const_cast<char*>("/spo/alice/knows/bob");
    put_ops[0].key_len = 20;
    put_ops[0].value = const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(""));
    put_ops[0].value_len = 0;
    ASSERT_EQ(0, database_subtree_batch_sync_raw(st, '/', put_ops, 1));

    // Close the subtree (user's reference).
    database_subtree_close(st);

    // Now delete via ROOT batch (NOT via subtree). The keys are in the root
    // namespace: "graph/spo/alice/knows/bob" etc. This is the expandTriple
    // + root batchSync path.
    raw_op_t del_ops[1];
    del_ops[0].type = 1;  // delete
    del_ops[0].key = const_cast<char*>("graph/spo/alice/knows/bob");
    del_ops[0].key_len = 25;
    del_ops[0].value = nullptr;
    del_ops[0].value_len = 0;
    ASSERT_EQ(0, database_batch_sync_raw(db, '/', del_ops, 1));

    // Close the db. This is where the crash happens.
    // If the bug is present, this calls abort() via "free(): invalid pointer".
    database_destroy(db);

    // If we reach here, the crash is fixed.
    SUCCEED();
}

// Reproduce: preceding in-memory GraphLayer create+close, THEN subtree insert
// + root batch delete + close. The Node.js binding crashes only when there's
// a preceding in-memory GraphLayer; this test mirrors that sequence exactly
// using graph_layer_create for the outer db (as the Node.js binding does).
TEST_F(SubtreeCloseCrashTest, PrecedingInMemoryGraphLayerThenSubtreeInsertRootBatchDelete) {
    // Outer in-memory GraphLayer (mirrors `new GraphLayer()` with no path).
    database_config_t* cfg1 = database_config_default();
    cfg1->sync_only = 1;
    cfg1->worker_threads = 0;
    graph_layer_config_t layer_cfg1;
    layer_cfg1.path = NULL;
    layer_cfg1.db_config = cfg1;
    int err1 = 0;
    graph_layer_t* outer_g = graph_layer_create(NULL, &layer_cfg1, NULL, &err1);
    database_config_destroy(cfg1);
    ASSERT_NE(outer_g, nullptr) << "In-memory GraphLayer creation failed (err=" << err1 << ")";
    // Insert a triple to populate the trie (mirrors outer.insertSync).
    ASSERT_EQ(0, graph_insert_sync(outer_g, "a", "b", "c"));
    graph_layer_destroy(outer_g);

    // Inner persistent db with subtree (mirrors the inner expandTriple test).
    // The Node.js binding test uses syncOnly: true, which makes the inner db
    // sync_only — database_snapshot dispatches to hbtrie_gc_unsafe (not
    // tx_manager_gc). The crash only happens in the hbtrie_gc_unsafe path.
    database_config_t* cfg2 = database_config_default();
    cfg2->chunk_size = 4;
    cfg2->btree_node_size = 4096;
    cfg2->worker_threads = 0;
    cfg2->sync_only = 1;
    cfg2->enable_persist = 1;
    cfg2->timer_resolution_ms = 100;
    int err2 = 0;
    database_t* db = database_create_with_config(tmpdir, cfg2, &err2);
    database_config_destroy(cfg2);
    ASSERT_NE(db, nullptr) << "Inner db creation failed (err=" << err2 << ")";

    // Open a subtree at "graph" and create a GraphLayer that shares the db.
    // Mirrors `new GraphLayer(null, { subtree })` in the Node.js binding.
    database_subtree_t* st = database_subtree_open(db, "graph", '/');
    ASSERT_NE(st, nullptr);

    // Create the inner GraphLayer via graph_layer_create with the subtree
    // (mirrors the Node.js binding's GraphLayer constructor for subtree mode).
    database_config_t* cfg3 = database_config_default();
    cfg3->sync_only = 1;  // graph_layer.cc sets this when db_path is null
    cfg3->worker_threads = 0;
    graph_layer_config_t layer_cfg3;
    layer_cfg3.path = NULL;
    layer_cfg3.db_config = cfg3;
    int err3 = 0;
    graph_layer_t* inner_g = graph_layer_create(NULL, &layer_cfg3, st, &err3);
    database_config_destroy(cfg3);
    ASSERT_NE(inner_g, nullptr) << "Inner GraphLayer creation failed (err=" << err3 << ")";

    // Insert via graph_insert_sync (mirrors xgraph.insertSync).
    // This writes 4 index entries (SPO/POS/OSP/PSO) to xdb via subtree batch.
    ASSERT_EQ(0, graph_insert_sync(inner_g, "alice", "knows", "bob"));

    // Close the user's subtree reference (mirrors `subtree.close()` in JS).
    // The GraphLayer holds its own reference, so the subtree stays alive.
    database_subtree_close(st);

    // Delete via ROOT batch (mirrors xdb.batchSync(graph.expandTriple(..., {delete: true}))).
    // The keys are in the root namespace: "graph/spo/alice/knows/bob" etc.
    // expandTriple returns 4 del ops; here we delete just the SPO key to
    // reproduce the minimal crash sequence.
    raw_op_t del_ops[1];
    del_ops[0].type = 1;
    del_ops[0].key = const_cast<char*>("graph/spo/alice/knows/bob");
    del_ops[0].key_len = 25;
    del_ops[0].value = nullptr;
    del_ops[0].value_len = 0;
    ASSERT_EQ(0, database_batch_sync_raw(db, '/', del_ops, 1));

    // Close the inner GraphLayer — this triggers graph_layer_destroy →
    // database_subtree_snapshot → database_snapshot → hbtrie_gc_unsafe.
    // The crash happens here in the Node.js binding.
    graph_layer_destroy(inner_g);

    database_destroy(db);
    SUCCEED();
}