//
// Created by victor on 3/11/26.
//

#include "database.h"
#include "database_config.h"
#include "database_iterator.h"
#include "wal_manager.h"
#include "batch.h"
#include "eviction_queue.h"
#include "../Workers/work.h"
#include "../Workers/error.h"
#include "../Util/allocator.h"
#include "../Util/mkdir_p.h"
#include "../Util/path_join.h"
#include "../Util/log.h"
#include "../Storage/node_serializer.h"
#include "../Util/offset_remap.h"
#include "../Storage/page_file.h"
#include "../Storage/encryption.h"
#include "../Storage/bnode_cache.h"
#include "../Util/memory_pool.h"
#include "../Util/endian.h"
#include <cbor.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include "Util/dirent_compat.h"
#include <sys/stat.h>
#if _WIN32
#include "Util/unistd_compat.h"
#define fsync(fd) _commit(fd)
#define open _open
#define O_RDONLY _O_RDONLY
#define O_WRONLY _O_WRONLY
#define O_RDWR _O_RDWR
#define O_CREAT _O_CREAT
#define O_TRUNC _O_TRUNC
#define O_BINARY _O_BINARY
#else
#include <unistd.h>
#endif

typedef struct {
    database_t* db;
    path_t* path;
    identifier_t* value;
    promise_t* promise;
} database_put_ctx_t;

typedef struct {
    database_t* db;
    path_t* path;
    promise_t* promise;
} database_get_ctx_t;

typedef struct {
    database_t* db;
    path_t* path;
    promise_t* promise;
} database_delete_ctx_t;

typedef struct {
    database_t* db;
    batch_t* batch;
    promise_t* promise;
} batch_work_t;

// Forward declarations
static void _database_put(database_put_ctx_t* ctx);
static void _database_get(database_get_ctx_t* ctx);
static void _database_delete(database_delete_ctx_t* ctx);

// Abort functions for cleanup when work is not executed
static void abort_database_put(void* ctx) {
    database_put_ctx_t* put_ctx = (database_put_ctx_t*)ctx;
    if (put_ctx->path) path_destroy(put_ctx->path);
    if (put_ctx->value) identifier_destroy(put_ctx->value);
    free(put_ctx);
}

static void abort_database_get(void* ctx) {
    database_get_ctx_t* get_ctx = (database_get_ctx_t*)ctx;
    if (get_ctx->path) path_destroy(get_ctx->path);
    free(get_ctx);
}

static void abort_database_delete(void* ctx) {
    database_delete_ctx_t* del_ctx = (database_delete_ctx_t*)ctx;
    if (del_ctx->path) path_destroy(del_ctx->path);
    free(del_ctx);
}

// Block the calling writer/reader if a vacuum is in progress.
// Returns 0 on resume (vacuum finished), -ETIMEDOUT if writer_block_timeout_ms
// elapsed (only when timeout is non-zero).
static int database_vacuum_block_if_in_progress(database_t* db) {
    if (db == NULL) return -1;
    if (atomic_load(&db->vacuum_in_progress) == 0) return 0;

    uint32_t timeout_ms = 0;
    if (db->active_config != NULL) {
        timeout_ms = db->active_config->vacuum_config.writer_block_timeout_ms;
    }

    platform_lock(&db->vacuum_writer_lock);
    while (atomic_load(&db->vacuum_in_progress) != 0) {
        int rc = platform_condition_timedwait(&db->vacuum_writer_lock, &db->vacuum_cvar, timeout_ms);
        if (rc != 0 && timeout_ms > 0) {
            // Timed out (ETIMEDOUT or error)
            platform_unlock(&db->vacuum_writer_lock);
            return -ETIMEDOUT;
        }
        // rc == 0 means signal/broadcast — recheck the flag in loop
    }
    platform_unlock(&db->vacuum_writer_lock);
    return 0;
}

// Helper to encode path+value for WAL using binary format
// Binary payload format:
//   [0xB1 magic][path_count:2B BE][path_len:4B BE]
//   For each identifier: [id_len:2B BE][id_data:id_len bytes]
//   [value_len:4B BE][value:value_len bytes]
static buffer_t* encode_put_entry_binary(path_t* path, identifier_t* value) {
    // Calculate total size
    size_t path_count = path_length(path);
    size_t total_size = 1 + 2 + 4;  // magic + path_count + path_len

    // Calculate size for each identifier and accumulate path_len
    size_t path_byte_len = 0;
    for (size_t i = 0; i < path_count; i++) {
        identifier_t* id = path_get(path, i);
        size_t id_len = id->length;
        total_size += 2 + id_len;  // id_len field + id_data
        path_byte_len += id_len;
    }

    // Value
    size_t value_len = (value != NULL) ? value->length : 0;
    total_size += 4 + value_len;  // value_len field + value_data

    // Allocate buffer
    buffer_t* buffer = buffer_create(total_size);
    if (buffer == NULL) return NULL;

    uint8_t* pos = buffer->data;

    // Write magic byte for unambiguous format detection
    *pos++ = WAL_BINARY_MAGIC;

    // Write path_count (2 bytes BE)
    write_be16(pos, (uint16_t)path_count);
    pos += 2;

    // Write path_len (4 bytes BE) - total byte length of all identifier data
    write_be32(pos, (uint32_t)path_byte_len);
    pos += 4;

    // Write each identifier
    for (size_t i = 0; i < path_count; i++) {
        identifier_t* id = path_get(path, i);
        size_t id_len = id->length;

        write_be16(pos, (uint16_t)id_len);
        pos += 2;

        // Get raw identifier data
        buffer_t* id_buf = identifier_to_buffer(id);
        if (id_buf == NULL) {
            buffer_destroy(buffer);
            return NULL;
        }
        memcpy(pos, id_buf->data, id_len);
        pos += id_len;
        buffer_destroy(id_buf);
    }

    // Write value
    write_be32(pos, (uint32_t)value_len);
    pos += 4;

    if (value_len > 0) {
        buffer_t* val_buf = identifier_to_buffer(value);
        if (val_buf == NULL) {
            buffer_destroy(buffer);
            return NULL;
        }
        memcpy(pos, val_buf->data, value_len);
        pos += value_len;
        buffer_destroy(val_buf);
    }

    buffer->size = (size_t)(pos - buffer->data);
    return buffer;
}

// Helper to encode path for WAL delete using binary format
// Binary payload format (no value):
//   [0xB1 magic][path_count:2B BE][path_len:4B BE]
//   For each identifier: [id_len:2B BE][id_data:id_len bytes]
static buffer_t* encode_delete_entry_binary(path_t* path) {
    // Calculate total size
    size_t path_count = path_length(path);
    size_t total_size = 1 + 2 + 4;  // magic + path_count + path_len

    // Calculate size for each identifier
    size_t path_byte_len = 0;
    for (size_t i = 0; i < path_count; i++) {
        identifier_t* id = path_get(path, i);
        size_t id_len = id->length;
        total_size += 2 + id_len;  // id_len field + id_data
        path_byte_len += id_len;
    }

    // Allocate buffer
    buffer_t* buffer = buffer_create(total_size);
    if (buffer == NULL) return NULL;

    uint8_t* pos = buffer->data;

    // Write magic byte for unambiguous format detection
    *pos++ = WAL_BINARY_MAGIC;

    // Write path_count (2 bytes BE)
    write_be16(pos, (uint16_t)path_count);
    pos += 2;

    // Write path_len (4 bytes BE)
    write_be32(pos, (uint32_t)path_byte_len);
    pos += 4;

    // Write each identifier
    for (size_t i = 0; i < path_count; i++) {
        identifier_t* id = path_get(path, i);
        size_t id_len = id->length;

        write_be16(pos, (uint16_t)id_len);
        pos += 2;

        // Get raw identifier data
        buffer_t* id_buf = identifier_to_buffer(id);
        if (id_buf == NULL) {
            buffer_destroy(buffer);
            return NULL;
        }
        memcpy(pos, id_buf->data, id_len);
        pos += id_len;
        buffer_destroy(id_buf);
    }

    buffer->size = (size_t)(pos - buffer->data);
    return buffer;
}

// Helper to serialize batch for WAL
static buffer_t* serialize_batch(batch_t* batch) {
    if (batch == NULL || batch->count == 0) {
        return NULL;
    }

    // Create CBOR array: [ [type, path, value?], ... ]
    cbor_item_t* batch_array = cbor_new_definite_array(batch->count);
    if (batch_array == NULL) {
        return NULL;
    }

    // Lock batch for reading
    platform_lock(&batch->lock);

    // Serialize each operation
    for (size_t i = 0; i < batch->count; i++) {
        batch_op_t* op = &batch->ops[i];

        // Create operation entry: [type, path, value?]
        cbor_item_t* op_array = cbor_new_definite_array(op->type == WAL_PUT ? 3 : 2);
        if (op_array == NULL) {
            platform_unlock(&batch->lock);
            cbor_decref(&batch_array);
            return NULL;
        }

        // Add type
        cbor_item_t* type_item = cbor_build_uint8(op->type);
        if (type_item == NULL) {
            platform_unlock(&batch->lock);
            cbor_decref(&op_array);
            cbor_decref(&batch_array);
            return NULL;
        }
        cbor_array_push(op_array, type_item);
        cbor_decref(&type_item);

        // Add path
        cbor_item_t* path_cbor = path_to_cbor(op->path);
        if (path_cbor == NULL) {
            platform_unlock(&batch->lock);
            cbor_decref(&op_array);
            cbor_decref(&batch_array);
            return NULL;
        }
        cbor_array_push(op_array, path_cbor);
        cbor_decref(&path_cbor);

        // Add value for PUT operations
        if (op->type == WAL_PUT && op->value != NULL) {
            cbor_item_t* value_cbor = identifier_to_cbor(op->value);
            if (value_cbor == NULL) {
                platform_unlock(&batch->lock);
                cbor_decref(&op_array);
                cbor_decref(&batch_array);
                return NULL;
            }
            cbor_array_push(op_array, value_cbor);
            cbor_decref(&value_cbor);
        }

        cbor_array_push(batch_array, op_array);
        cbor_decref(&op_array);
    }

    platform_unlock(&batch->lock);

    // Serialize to buffer
    unsigned char* buf = NULL;
    size_t buf_size = 0;
    cbor_serialize_alloc(batch_array, &buf, &buf_size);
    cbor_decref(&batch_array);

    if (buf == NULL) {
        return NULL;
    }

    buffer_t* buffer = buffer_create_from_existing_memory(buf, buf_size);
    return buffer;
}

// Create index file path
static char* create_index_path(const char* location, uint64_t id, uint32_t crc) {
    char buf[256];
    snprintf(buf, sizeof(buf), "%s/index_%lu_%08x.cbor", location, (unsigned long)id, crc);
    return strdup(buf);
}

// Save HBTrie to disk (CBOR format)
static int save_index(database_t* db) {
    uint32_t crc = hbtrie_compute_hash(db->trie);
    uint64_t index_id = db->next_index_id++;

    char* index_file = create_index_path(db->location, index_id, crc);
    if (index_file == NULL) {
        log_error("Failed to create index path");
        return -1;
    }

    uint8_t* buf = NULL;
    size_t len = 0;
    if (hbtrie_serialize(db->trie, &buf, &len) != 0) {
        log_error("Failed to serialize HBTrie");
        free(index_file);
        return -1;
    }

    int fd = open(index_file, O_WRONLY | O_CREAT | O_TRUNC
#if _WIN32
        | O_BINARY
#endif
        , 0644);
    if (fd < 0) {
        log_error("Failed to open index file: %s", index_file);
        free(buf);
        free(index_file);
        return -1;
    }

    ssize_t written = write(fd, buf, len);
    close(fd);
    free(buf);
    free(index_file);

    if (written != (ssize_t)len) {
        log_error("Failed to write index file: wrote %zd of %zu bytes", written, len);
        return -1;
    }

    log_info("Saved index file successfully");
    return 0;
}

// Load HBTrie from disk (CBOR format)
static hbtrie_t* load_index(const char* location, uint8_t chunk_size,
                             uint32_t btree_node_size) {
    // Find most recent CBOR index file
    DIR* dir = opendir(location);
    if (dir == NULL) return NULL;

    char* latest_file = NULL;
    uint64_t latest_id = 0;

    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "index_", 6) == 0 && strstr(entry->d_name, ".cbor") != NULL) {
            // Parse ID from filename: index_<id>_<crc>.cbor
            uint64_t id = 0;
            if (sscanf(entry->d_name, "index_%lu", &id) == 1) {
                if (id >= latest_id) {
                    latest_id = id;
                    free(latest_file);
                    latest_file = path_join(location, entry->d_name);
                }
            }
        }
    }
    closedir(dir);

    if (latest_file == NULL) {
        return hbtrie_create(chunk_size, btree_node_size);
    }

    // Read file
    int fd = open(latest_file, O_RDONLY
#if _WIN32
        | O_BINARY
#endif
        );
    if (fd < 0) {
        free(latest_file);
        return hbtrie_create(chunk_size, btree_node_size);
    }

    struct stat st;
    if (fstat(fd, &st) != 0) {
        close(fd);
        free(latest_file);
        return hbtrie_create(chunk_size, btree_node_size);
    }

    uint8_t* buf = malloc(st.st_size);
    if (buf == NULL) {
        close(fd);
        free(latest_file);
        return hbtrie_create(chunk_size, btree_node_size);
    }

    ssize_t bytes_read = read(fd, buf, st.st_size);
    close(fd);
    free(latest_file);

    if (bytes_read != st.st_size) {
        free(buf);
        return hbtrie_create(chunk_size, btree_node_size);
    }

    hbtrie_t* trie = hbtrie_deserialize(buf, bytes_read, chunk_size, btree_node_size);
    free(buf);

    return trie ? trie : hbtrie_create(chunk_size, btree_node_size);
}

// ============================================================================
// Phase 2: Per-bnode CoW flush
// ============================================================================

typedef struct {
    bnode_t* bnode;
    hbtrie_node_t* hbnode;         // Owning hbtrie_node
    bnode_t* parent_bnode;         // Parent bnode (or NULL for root)
    size_t parent_entry_index;      // Index of this bnode in parent's entries
    uint16_t level;                // B+tree level (for sorting)
    int hbtrie_depth;              // HBTrie depth (root=0, child=1, etc.) for sorting
} dirty_bnode_info_t;

static int compare_dirty_by_level(const void* a, const void* b) {
    const dirty_bnode_info_t* da = (const dirty_bnode_info_t*)a;
    const dirty_bnode_info_t* db = (const dirty_bnode_info_t*)b;
    // Deeper hbtrie nodes first (children before parents across hbtrie levels)
    if (da->hbtrie_depth > db->hbtrie_depth) return -1;
    if (da->hbtrie_depth < db->hbtrie_depth) return 1;
    // Within same hbtrie depth, leaves first (lower level = leaf)
    if (da->level < db->level) return -1;
    if (da->level > db->level) return 1;
    return 0;
}

// Propagate is_dirty upward through the btree so all ancestors of dirty
// leaves are also marked dirty. This ensures they get collected and flushed.
static void propagate_dirty_upward(bnode_t* root) {
    if (root == NULL) return;
    for (size_t i = 0; i < root->entries.length; i++) {
        bnode_entry_t* entry = &root->entries.data[i];
        if (entry->is_bnode_child && entry->child_bnode != NULL) {
            propagate_dirty_upward(entry->child_bnode);
            if (entry->child_bnode->is_dirty) {
                root->is_dirty = 1;
            }
        }
    }
}

// Stack item for DFS walk of bnode tree with parent tracking
typedef struct {
    bnode_t* bnode;
    bnode_t* parent_bnode;         // Within-B+tree parent (NULL for root)
    size_t parent_entry_index;     // Index in parent's entries
} bnode_stack_item_t;

// Walk the trie to collect all dirty bnodes with parent tracking.
// The DFS uses a stack that carries within-B+tree parent information,
// so each bnode knows its actual parent within the B+tree (not just
// the cross-hbtrie-node boundary parent).
static void collect_dirty_bnodes_from_hbnode(
    hbtrie_node_t* hbnode,
    bnode_t* cross_parent_bnode,
    size_t cross_parent_entry_index,
    int hbtrie_depth,
    vec_t(dirty_bnode_info_t)* dirty_list)
{
    if (hbnode == NULL) return;

    // PHASE 1 — Recurse into child hbtrie_nodes and propagate dirty UPWARD
    // across hbtrie boundaries BEFORE collecting this hbnode's own bnodes.
    //
    // In a multi-level B+tree, hbtrie child pointers live in leaf bnodes,
    // not the root, so we walk the whole btree to find every leaf entry.
    // When a child hbtrie_node is dirty, the leaf bnode (`bn`) holding the
    // pointer to it must be re-serialized after the child is CoW'd (its
    // child_disk_offset changes). We mark `bn` and this `hbnode` dirty NOW,
    // so the collection phase below (which runs `propagate_dirty_upward` +
    // walks the btree for dirty bnodes) picks `bn` up in THIS pass.
    //
    // Recursing before marking makes this post-order: each child's marking
    // completes before we test `entry->child->is_dirty` here, so a dirty
    // grandchild propagates up through every ancestor hbtrie level in a
    // single collect. Without this ordering the parent leaf bnode would be
    // marked dirty only AFTER the btree walk passed it, and — because the
    // parent hbtrie_node is usually already dirty at scale (it has other
    // dirty children) and gets its `is_dirty` cleared at end-of-pass — the
    // loop would never re-collect it, leaving the on-disk parent blob with
    // a stale child_disk_offset and losing the child subtree on reopen.
    vec_t(bnode_t*) leaf_stack;
    vec_init(&leaf_stack);
    vec_push(&leaf_stack, hbnode->btree);

    while (leaf_stack.length > 0) {
        bnode_t* bn = vec_pop(&leaf_stack);

        for (size_t i = 0; i < bn->entries.length; i++) {
            bnode_entry_t* entry = &bn->entries.data[i];

            if (entry->is_bnode_child && entry->child_bnode != NULL) {
                // Internal node — descend further within this btree
                vec_push(&leaf_stack, entry->child_bnode);
            } else {
                // Leaf entry — recurse into hbtrie children, then mark.
                if (!entry->is_bnode_child && !entry->has_value && entry->child != NULL) {
                    collect_dirty_bnodes_from_hbnode(entry->child, bn, i, hbtrie_depth + 1, dirty_list);
                    if (entry->child->is_dirty) {
                        bn->is_dirty = 1;
                        hbnode->is_dirty = 1;
                    }
                }
                if (entry->trie_child != NULL) {
                    collect_dirty_bnodes_from_hbnode(entry->trie_child, bn, i, hbtrie_depth + 1, dirty_list);
                    if (entry->trie_child->is_dirty) {
                        bn->is_dirty = 1;
                        hbnode->is_dirty = 1;
                    }
                }
            }
        }
    }

    vec_deinit(&leaf_stack);

    // PHASE 2 — Collect dirty bnodes from this hbtrie_node. By now any leaf
    // bnode holding a pointer to a dirty child hbtrie_node has already been
    // marked dirty above, so it is collected here in the same pass.
    if (hbnode->is_dirty) {
        // Propagate dirty upward within this hbnode's btree (child bnode
        // dirty -> parent internal bnode dirty, so the parent's
        // child_disk_offset gets re-serialized too).
        propagate_dirty_upward(hbnode->btree);

        // Walk all bnodes in this hbtrie_node's btree using DFS with parent tracking
        vec_t(bnode_stack_item_t) stack;
        vec_init(&stack);

        // Root of this hbnode's B+tree: parent is the cross-hbtrie-node parent
        bnode_stack_item_t root_item;
        root_item.bnode = hbnode->btree;
        root_item.parent_bnode = cross_parent_bnode;
        root_item.parent_entry_index = cross_parent_entry_index;
        vec_push(&stack, root_item);

        while (stack.length > 0) {
            bnode_stack_item_t item = vec_pop(&stack);
            bnode_t* bn = item.bnode;

            if (bn->is_dirty) {
                dirty_bnode_info_t info;
                info.bnode = bn;
                info.hbnode = hbnode;
                info.parent_bnode = item.parent_bnode;
                info.parent_entry_index = item.parent_entry_index;
                info.level = atomic_load(&bn->level);
                info.hbtrie_depth = hbtrie_depth;
                vec_push(dirty_list, info);
            }

            // Push child bnodes for internal nodes, tracking within-B+tree parent
            for (size_t i = 0; i < bn->entries.length; i++) {
                bnode_entry_t* entry = &bn->entries.data[i];
                if (entry->is_bnode_child && entry->child_bnode != NULL) {
                    bnode_stack_item_t child_item;
                    child_item.bnode = entry->child_bnode;
                    child_item.parent_bnode = bn;        // Within-B+tree parent
                    child_item.parent_entry_index = i;    // Index in parent's entries
                    vec_push(&stack, child_item);
                }
            }
        }

        vec_deinit(&stack);
    }
}

int database_flush_dirty_bnodes(database_t* db) {
    if (db == NULL || db->page_file == NULL || db->trie == NULL) return -1;

    hbtrie_node_t* root = atomic_load_ptr(&db->trie->root, hbtrie_node_t*);
    if (root == NULL) return 0;

    // Iterate collect -> sort -> flush until a full pass collects zero dirty
    // bnodes. collect_dirty_bnodes_from_hbnode now propagates dirty upward
    // across hbtrie boundaries in post-order BEFORE collecting, so a single
    // collect pass gathers every ancestor of every dirty hbtrie_node. The
    // loop still serves a purpose: when a cross-hbtrie parent leaf bnode and
    // the child hbtrie_node's root bnode sit at the SAME bnode level, the
    // level-sort may flush the parent before the child; the child's flush
    // then re-points the parent entry's child_disk_offset and marks the
    // parent dirty again, so the next pass re-flushes the parent with the
    // updated offset. Convergence is bounded by hbtrie depth.
    for (;;) {
        // 1. Collect all dirty bnodes (including child hbtrie_nodes)
        vec_t(dirty_bnode_info_t) dirty_list;
        vec_init(&dirty_list);
        collect_dirty_bnodes_from_hbnode(root, NULL, 0, 0, &dirty_list);

        if (dirty_list.length == 0) {
            vec_deinit(&dirty_list);
            break;
        }

        // 2. Sort by level (leaves first, root last)
        qsort(dirty_list.data, dirty_list.length, sizeof(dirty_bnode_info_t),
              compare_dirty_by_level);

        // 3. Flush each dirty bnode
        for (size_t i = 0; i < dirty_list.length; i++) {
            dirty_bnode_info_t* info = &dirty_list.data[i];
            bnode_t* bn = info->bnode;

            // Serialize with V3 format
            uint8_t* buf = NULL;
            size_t len = 0;
            int rc = bnode_serialize_v3(bn, db->trie->chunk_size, &buf, &len);
            if (rc != 0) {
                free(buf);
                vec_deinit(&dirty_list);
                return -1;
            }

            // Write to page file at new offset (CoW)
            uint64_t new_offset = 0;
            uint64_t bids[64] = {0};
            size_t num_bids = 0;
            rc = page_file_write_node(db->page_file, buf, len,
                                       &new_offset, bids, 64, &num_bids);
            free(buf);
            if (rc != 0) {
                vec_deinit(&dirty_list);
                return -1;
            }

            // Mark old offset as stale (if previously persisted)
            if (bn->disk_offset != UINT64_MAX) {
                page_file_mark_stale(db->page_file, bn->disk_offset, len);
            }

            // Update bnode's disk_offset
            bn->disk_offset = new_offset;
            bn->is_dirty = 0;

            // Update parent entry's child_disk_offset
            if (info->parent_bnode != NULL && info->parent_entry_index < info->parent_bnode->entries.length) {
                bnode_entry_t* parent_entry = &info->parent_bnode->entries.data[info->parent_entry_index];
                parent_entry->child_disk_offset = new_offset;
                // Parent is now dirty (its entry changed)
                info->parent_bnode->is_dirty = 1;
            }
        }

        // 4. Clear is_dirty on every hbtrie_node that had a bnode collected
        //    this pass. A hbtrie_node re-marked dirty during the flush (step 3
        //    set a parent bnode dirty after re-pointing its child_disk_offset)
        //    is NOT in this pass's list under that re-marking, so it stays dirty
        //    and the next pass re-collects it — this is what drives the loop.
        for (size_t i = 0; i < dirty_list.length; i++) {
            hbtrie_node_t* hn = dirty_list.data[i].hbnode;
            if (hn != NULL) hn->is_dirty = 0;
        }

        vec_deinit(&dirty_list);
    }

    // 5. Update root hbtrie_node disk_offset
    root->disk_offset = root->btree->disk_offset;
    root->is_dirty = 0;

    // 6. Write superblock with new root offset and transaction ID
    transaction_id_t last_txn;
    if (db->tx_manager != NULL) {
        last_txn = tx_manager_get_last_committed(db->tx_manager);
    } else {
        last_txn.time = 0;
        last_txn.nanos = 0;
        last_txn.count = 0;
    }
    int sb_rc = page_file_write_superblock(db->page_file, root->disk_offset, 0, &last_txn);
    if (sb_rc != 0) {
        return -1;
    }

    return 0;
}

// ============================================================================
// Eviction pipeline: callback + background task
// ============================================================================

static void database_eviction_task_abort(void* ctx);

static void database_on_bnode_evict(uint64_t disk_offset, void* user_data) {
    database_t* db = (database_t*)user_data;
    eviction_queue_push(&db->eviction_queue, disk_offset);
}

static void database_eviction_task_execute(void* ctx) {
    database_t* db = (database_t*)ctx;
    if (db == NULL || db->destroying || db->trie == NULL || db->bnode_cache == NULL) {
        // Work item lifecycle ends — decrement counter so database_destroy can proceed
        if (db != NULL) {
            atomic_fetch_sub(&db->eviction_in_flight, 1);
        }
        return;
    }

    // Skip the body during vacuum — nulling entries would race with the
    // vacuum walk. The eviction_queue is left untouched; queued offsets
    // are processed by database_vacuum_drain before the page-file swap,
    // and any offsets still queued after vacuum will be re-evaluated
    // against the new page file on the next eviction task iteration
    // (the bnode_cache_complete_evict path drops offsets that no longer
    // match a live cache entry).
    if (atomic_load(&db->vacuum_in_progress)) {
        // Reschedule without doing work.
        if (db->pool != NULL) {
            work_t* task = work_create(database_eviction_task_execute,
                                        database_eviction_task_abort,
                                        db);
            if (task != NULL) {
                atomic_fetch_add(&db->eviction_in_flight, 1);
                refcounter_yield((refcounter_t*) task);
                if (work_pool_enqueue(db->pool, task) != 0) {
                    work_destroy(task);
                    work_destroy(task);
                    atomic_fetch_sub(&db->eviction_in_flight, 1);
                }
            }
        }
        // This work item's lifecycle ends — decrement counter.
        atomic_fetch_sub(&db->eviction_in_flight, 1);
        return;
    }

    uint64_t offsets[64];
    size_t n = eviction_queue_drain(&db->eviction_queue, offsets, 64);

    for (size_t i = 0; i < n; i++) {
        hbtrie_null_entries_by_offset(db->trie, offsets[i]);
        bnode_cache_complete_evict(db->bnode_cache, offsets[i]);
    }

    // Reschedule — increment for new work item BEFORE decrementing for
    // the current one, so eviction_in_flight never hits 0 while a new
    // work item is still pending in the pool queue.
    if (!db->destroying && db->pool != NULL) {
        work_t* task = work_create(database_eviction_task_execute,
                                    database_eviction_task_abort,
                                    db);
        if (task != NULL) {
            atomic_fetch_add(&db->eviction_in_flight, 1);
            refcounter_yield((refcounter_t*) task);
            if (work_pool_enqueue(db->pool, task) != 0) {
                // Pool stopped — must consume the yield credit AND decrement
                // count. A single work_destroy only consumes the yield (refcounter
                // consumes yield first, returns early without decrementing count).
                // Call work_destroy twice: first consumes yield, second decrements
                // count to 0 and frees the work_t.
                work_destroy(task);
                work_destroy(task);
                atomic_fetch_sub(&db->eviction_in_flight, 1);
            }
        }
    }

    // This work item's lifecycle ends — decrement counter
    atomic_fetch_sub(&db->eviction_in_flight, 1);
}

static void database_eviction_task_abort(void* ctx) {
    database_t* db = (database_t*)ctx;
    if (db != NULL) {
        atomic_fetch_sub(&db->eviction_in_flight, 1);
    }
}

// ============================================================================
// Background vacuum worker
// ============================================================================

typedef struct {
    database_t* db;
} vacuum_task_ctx_t;

static void database_vacuum_schedule_background(database_t* db);

static void database_vacuum_task_abort(void* arg) {
    // Frees the vacuum_task_ctx_t and decrements the in-flight counter.
    // Called by hierarchical_timing_wheel_cancel_timer (when database_destroy
    // cancels a pending timer) and by hierarchical_timing_wheel_stop (for
    // owns_wheel teardown). Either way, the counter incremented in
    // schedule_background is consumed here so the drain loop can exit.
    if (arg == NULL) return;
    vacuum_task_ctx_t* ctx = (vacuum_task_ctx_t*)arg;
    database_t* db = ctx->db;
    free(ctx);
    if (db != NULL) {
        atomic_fetch_sub(&db->vacuum_in_flight, 1);
    }
}

static void database_vacuum_task_execute(void* arg) {
    vacuum_task_ctx_t* ctx = (vacuum_task_ctx_t*)arg;
    database_t* db = ctx ? ctx->db : NULL;
    free(ctx);

    if (db == NULL) return;

    // If destruction is in progress (or the page_file is already gone),
    // don't run the vacuum and don't reschedule. The drain loop in
    // database_destroy is waiting on vacuum_in_flight to hit 0.
    if (db->page_file == NULL || db->destroying) {
        atomic_fetch_sub(&db->vacuum_in_flight, 1);
        return;
    }

    vacuum_mode_t mode = VACUUM_MODE_STRICT;
    if (db->active_config != NULL) {
        mode = db->active_config->vacuum_config.mode;
    }

    // MANUAL_ONLY: don't run, but still reschedule (in case config changes later)
    if (mode == VACUUM_MODE_MANUAL_ONLY) {
        database_vacuum_schedule_background(db);
        atomic_fetch_sub(&db->vacuum_in_flight, 1);
        return;
    }

    // ADAPTIVE: skip if work_pool is busy
    if (mode == VACUUM_MODE_ADAPTIVE && db->pool != NULL) {
        // Approximate "busy" by checking if any workers are active.
        // (idleCount < size means work is in-flight)
        if (db->pool->idleCount < db->pool->size) {
            uint32_t threshold = 32;
            if (db->active_config != NULL) {
                threshold = db->active_config->vacuum_config.adaptive_busy_threshold;
            }
            size_t active = db->pool->size - db->pool->idleCount;
            if (active > threshold) {
                database_vacuum_schedule_background(db);
                atomic_fetch_sub(&db->vacuum_in_flight, 1);
                return;
            }
        }
    }

    // Check threshold before running (avoid no-op vacuums)
    double ratio = page_file_stale_ratio(db->page_file);
    double threshold = 0.30;
    if (db->active_config != NULL) {
        threshold = db->active_config->vacuum_config.stale_threshold;
    }
    if (ratio < threshold) {
        database_vacuum_schedule_background(db);
        atomic_fetch_sub(&db->vacuum_in_flight, 1);
        return;
    }

    // Run vacuum
    database_vacuum_auto(db);

    // Reschedule next tick
    database_vacuum_schedule_background(db);

    atomic_fetch_sub(&db->vacuum_in_flight, 1);
}

static void database_vacuum_schedule_background(database_t* db) {
    if (db == NULL || db->wheel == NULL || db->destroying) return;
    uint32_t interval_ms = 60000;
    if (db->active_config != NULL) {
        interval_ms = db->active_config->vacuum_config.background_interval_ms;
    }
    if (interval_ms == 0) return;  // disabled

    vacuum_task_ctx_t* ctx = get_clear_memory(sizeof(vacuum_task_ctx_t));
    if (ctx == NULL) return;
    ctx->db = db;

    timer_duration_t delay = {0};
    delay.milliseconds = interval_ms;

    // Increment in-flight BEFORE set_timer so the counter reflects the
    // pending timer. The execute callback decrements at its end (after
    // reschedule); the abort callback decrements when stop cancels the
    // pending timer (owns_wheel case). For !owns_wheel, the timer fires
    // and execute self-skips via the destroying check, decrementing.
    atomic_fetch_add(&db->vacuum_in_flight, 1);
    db->vacuum_task_id = hierarchical_timing_wheel_set_timer(
        db->wheel, ctx, database_vacuum_task_execute, database_vacuum_task_abort, delay);
}

database_t* database_create_with_config(const char* location,
                                        database_config_t* config,
                                        int* error_code) {
    if (error_code) *error_code = 0;

    // Use defaults if no config provided
    database_config_t* effective_config = NULL;
    bool owns_config = false;

    if (config == NULL) {
        effective_config = database_config_default();
        owns_config = true;
    } else {
        effective_config = config;
    }

    bool is_memory_only = (location == NULL);

    // Check if database already exists (skip for in-memory)
    if (!is_memory_only) {
        char config_path[1024];
        snprintf(config_path, sizeof(config_path), "%s/.config", location);
        struct stat st;
        bool db_exists = (stat(config_path, &st) == 0);

        if (db_exists) {
            // Load saved config and merge
            database_config_t* saved_config = database_config_load(location);
            if (saved_config != NULL) {
                // If saved config has encryption enabled, the caller must provide
                // an encryption context via database_create_encrypted.
                // Exception: if the passed config already has encryption enabled,
                // the caller is database_create_encrypted and has already verified
                // the key — allow the merge to proceed.
                if (saved_config->encryption.has_encryption &&
                    saved_config->encryption.type != ENCRYPTION_NONE &&
                    !(effective_config->encryption.has_encryption &&
                      effective_config->encryption.type != ENCRYPTION_NONE)) {
                    database_config_destroy(saved_config);
                    if (owns_config) database_config_destroy(effective_config);
                    if (error_code) *error_code = DATABASE_ERR_ENCRYPTION_REQUIRED;
                    return NULL;
                }

                database_config_t* merged = database_config_merge(saved_config, effective_config);
                if (owns_config) {
                    database_config_destroy(effective_config);
                }
                database_config_destroy(saved_config);
                effective_config = merged;
                owns_config = true;
            }
        }

        // Create directory if needed
        if (mkdir_p((char*)location) != 0) {
            if (error_code) *error_code = errno;
            if (owns_config) database_config_destroy(effective_config);
            return NULL;
        }
    }

    // Initialize memory pool (call once per process)
    memory_pool_init();

    // Initialize transaction ID generator (call once per process)
    transaction_id_init();

    database_t* db = get_clear_memory(sizeof(database_t));
    if (db == NULL) {
        if (error_code) *error_code = ENOMEM;
        if (owns_config) database_config_destroy(effective_config);
        return NULL;
    }

    db->location = location ? strdup(location) : strdup("");
    db->lru_memory_mb = effective_config->lru_memory_mb;
    db->chunk_size = effective_config->chunk_size;
    db->btree_node_size = effective_config->btree_node_size;
    db->sync_only = effective_config->sync_only;
    db->is_rebuilding = 0;

    // Handle pool ownership
    if (effective_config->external_pool != NULL) {
        db->pool = effective_config->external_pool;
        db->owns_pool = false;
    } else if (effective_config->worker_threads > 0) {
        db->pool = work_pool_create(effective_config->worker_threads);
        db->owns_pool = (db->pool != NULL);
        if (db->pool == NULL) {
            if (error_code) *error_code = ENOMEM;
            free(db->location);
            free(db);
            if (owns_config) database_config_destroy(effective_config);
            return NULL;
        }
        work_pool_launch(db->pool);
    } else if (effective_config->sync_only) {
        // Sync-only mode: no pool needed
        db->pool = NULL;
        db->owns_pool = false;
    } else {
        // No pool available
        if (error_code) *error_code = EINVAL;
        free(db->location);
        free(db);
        if (owns_config) database_config_destroy(effective_config);
        return NULL;
    }

    // Handle wheel ownership
    if (effective_config->external_wheel != NULL) {
        db->wheel = effective_config->external_wheel;
        db->owns_wheel = false;
    } else if (effective_config->sync_only) {
        // Sync-only mode: no wheel needed
        db->wheel = NULL;
        db->owns_wheel = false;
    } else if (effective_config->timer_resolution_ms > 0) {
        db->wheel = hierarchical_timing_wheel_create(effective_config->timer_resolution_ms, db->pool);
        db->owns_wheel = (db->wheel != NULL);
        if (db->wheel == NULL) {
            if (db->owns_pool) work_pool_destroy(db->pool);
            free(db->location);
            free(db);
            if (owns_config) database_config_destroy(effective_config);
            if (error_code) *error_code = ENOMEM;
            return NULL;
        }
    } else {
        // No wheel available
        if (error_code) *error_code = EINVAL;
        if (db->owns_pool) work_pool_destroy(db->pool);
        free(db->location);
        free(db);
        if (owns_config) database_config_destroy(effective_config);
        return NULL;
    }

    // Create LRU cache
    db->lru = database_lru_cache_create(db->lru_memory_mb * 1024 * 1024, effective_config->lru_shards);
    if (db->lru == NULL) {
        if (db->owns_pool) work_pool_destroy(db->pool);
        if (db->owns_wheel) hierarchical_timing_wheel_destroy(db->wheel);
        free(db->location);
        free(db);
        if (owns_config) database_config_destroy(effective_config);
        if (error_code) *error_code = ENOMEM;
        return NULL;
    }

    // Initialize write locks (only needed for concurrent mode)
    if (!db->sync_only) {
        for (int i = 0; i < WRITE_LOCK_SHARDS; i++) {
            spinlock_init(&db->write_locks[i]);
        }
    }

    // Create storage directories if persistent (skip for in-memory)
    if (!is_memory_only && effective_config->enable_persist) {
        char* data_path = path_join(location, "data");
        char* meta_path = path_join(location, "meta");
        mkdir_p(data_path);
        mkdir_p(meta_path);
        free(data_path);
        free(meta_path);
    }

    // Load or create trie (skip loading from disk for in-memory)
    db->trie = is_memory_only ? NULL : load_index(location, db->chunk_size, db->btree_node_size);
    if (db->trie == NULL) {
        db->trie = hbtrie_create(db->chunk_size, db->btree_node_size);
    }

    // Create page file and bnode cache (Phase 2) (skip for in-memory)
    if (!is_memory_only && effective_config->enable_persist) {
        char page_path[1024];
        snprintf(page_path, sizeof(page_path), "%s/data.wdbp", location);

        db->page_file = page_file_create(page_path,
            PAGE_FILE_DEFAULT_BLOCK_SIZE, PAGE_FILE_DEFAULT_NUM_SUPERBLOCKS, db->encryption);
        if (db->page_file != NULL) {
            int pf_rc = page_file_open(db->page_file, 1);
            if (pf_rc == 0) {
                // Try to load root from superblock
                page_superblock_t sb;
                int sb_rc = page_file_read_superblock(db->page_file, &sb, NULL);
                if (sb_rc == 0 && sb.root_offset != 0) {
                    // Root exists on disk — load it
                    size_t root_len = 0;
                    uint8_t* root_data = page_file_read_node(db->page_file, sb.root_offset, &root_len);
                    if (root_data != NULL) {
                        node_location_t* locations = NULL;
                        size_t num_locations = 0;
                        /* page_file_read_node returns buffer with 4-byte size prefix.
                           root_len is the size WITHOUT the prefix.
                           bnode_deserialize_v3 expects data starting with V3 magic byte. */
                        bnode_t* root_bnode = bnode_deserialize_v3(
                            root_data + 4, root_len,
                            db->trie->chunk_size, db->trie->btree_node_size,
                            &locations, &num_locations);
                        free(root_data);

                        if (root_bnode != NULL) {
                            // Set child_disk_offset on entries
                            for (size_t li = 0; li < num_locations && li < root_bnode->entries.length; li++) {
                                root_bnode->entries.data[li].child_disk_offset = locations[li].offset;
                            }
                            root_bnode->disk_offset = sb.root_offset;
                            root_bnode->is_dirty = 0;

                            // Create hbtrie_node wrapper for loaded root
                            hbtrie_node_t* root_hbnode = hbtrie_node_create(db->trie->btree_node_size);
                            if (root_hbnode != NULL) {
                                bnode_deinit(root_hbnode->btree);
                                root_hbnode->btree = root_bnode;
                                root_hbnode->btree_height = atomic_load(&root_bnode->level);
                                root_hbnode->disk_offset = sb.root_offset;
                                root_hbnode->is_loaded = 1;
                                root_hbnode->is_dirty = 0;

                                hbtrie_node_t* old_root = atomic_load_ptr(&db->trie->root, hbtrie_node_t*);
                                atomic_store_ptr(&db->trie->root, root_hbnode);
                                hbtrie_node_destroy(old_root);
                            } else {
                                bnode_destroy_tree(root_bnode);
                            }
                            free(locations);
                        }
                    }

                    // Save txn ID for later — tx_manager hasn't been created yet
                    // We'll apply it after tx_manager_create
                    if (sb.last_txn_time != 0 || sb.last_txn_nanos != 0 || sb.last_txn_count != 0) {
                        // Store in db's pending field for later application
                        db->pending_txn_id.time = sb.last_txn_time;
                        db->pending_txn_id.nanos = sb.last_txn_nanos;
                        db->pending_txn_id.count = sb.last_txn_count;
                        db->has_pending_txn_id = 1;
                    }
                }

                // Create bnode cache
                bnode_cache_mgr_t* cache_mgr = bnode_cache_mgr_create(
                    (size_t)effective_config->bnode_cache_memory_mb * 1024 * 1024,
                    effective_config->bnode_cache_shards);
                if (cache_mgr != NULL) {
                    db->bnode_cache = bnode_cache_create_file_cache(
                        cache_mgr, db->page_file, page_path);
                    if (db->bnode_cache == NULL) {
                        bnode_cache_mgr_destroy(cache_mgr);
                    } else if (db->trie != NULL) {
                        // Wire bnode cache for lazy loading
                        db->trie->fcache = db->bnode_cache;
                    }
                }

                // Wire eviction callback for deferred free
                eviction_queue_init(&db->eviction_queue);
                if (db->bnode_cache != NULL) {
                    db->bnode_cache->on_evict = database_on_bnode_evict;
                    db->bnode_cache->on_evict_data = db;

                    // Start background eviction task (only in concurrent mode)
                    if (!db->sync_only && db->pool != NULL) {
                        work_t* task = work_create(database_eviction_task_execute,
                                                    database_eviction_task_abort,
                                                    db);
                        if (task != NULL) {
                            atomic_fetch_add(&db->eviction_in_flight, 1);
                            refcounter_yield((refcounter_t*) task);
                            if (work_pool_enqueue(db->pool, task) != 0) {
                                // Pool stopped — consume yield + decrement count
                                work_destroy(task);
                                work_destroy(task);
                                atomic_fetch_sub(&db->eviction_in_flight, 1);
                            }
                        }
                    }
                }
            } else {
                // Failed to open page file — log warning but continue
                log_warn("Failed to open page file: %s", page_path);
                page_file_destroy(db->page_file);
                db->page_file = NULL;
            }
        }
    }

    if (db->trie == NULL) {
        database_lru_cache_destroy(db->lru);
        if (db->owns_pool) work_pool_destroy(db->pool);
        if (db->owns_wheel) hierarchical_timing_wheel_destroy(db->wheel);
        if (!db->sync_only) {
            for (int i = 0; i < WRITE_LOCK_SHARDS; i++) {
                spinlock_destroy(&db->write_locks[i]);
            }
        }
        free(db->location);
        free(db);
        if (owns_config) database_config_destroy(effective_config);
        if (error_code) *error_code = ENOMEM;
        return NULL;
    }

    // Create transaction manager (only needed for concurrent mode)
    if (!db->sync_only) {
        db->tx_manager = tx_manager_create(db->trie, db->pool, db->wheel, 100);

        // Apply pending MVCC transaction ID from page file superblock
        // (page file loads before tx_manager is created, so we deferred this)
        if (db->has_pending_txn_id && db->tx_manager != NULL) {
            transaction_id_advance_to(&db->pending_txn_id);
            atomic_store_txn(&db->tx_manager->last_committed_txn_id, db->pending_txn_id);
            db->has_pending_txn_id = 0;
        }
    } else {
        db->tx_manager = NULL;
    }

    if (db->tx_manager == NULL && !db->sync_only) {
        hbtrie_destroy(db->trie);
        database_lru_cache_destroy(db->lru);
        if (db->owns_pool) work_pool_destroy(db->pool);
        if (db->owns_wheel) hierarchical_timing_wheel_destroy(db->wheel);
        for (int i = 0; i < WRITE_LOCK_SHARDS; i++) {
            spinlock_destroy(&db->write_locks[i]);
        }
        free(db->location);
        free(db);
        if (owns_config) database_config_destroy(effective_config);
        if (error_code) *error_code = ENOMEM;
        return NULL;
    }

    // Create WAL manager (skip for in-memory databases)
    if (!is_memory_only) {
        db->wal_manager = wal_manager_create(db->location, &effective_config->wal_config, db->wheel, db->encryption, error_code);
        if (db->wal_manager == NULL) {
            tx_manager_destroy(db->tx_manager);
            hbtrie_destroy(db->trie);
            database_lru_cache_destroy(db->lru);
            if (db->owns_pool) work_pool_destroy(db->pool);
            if (db->owns_wheel) hierarchical_timing_wheel_destroy(db->wheel);
            if (!db->sync_only) {
                for (int i = 0; i < WRITE_LOCK_SHARDS; i++) {
                    spinlock_destroy(&db->write_locks[i]);
                }
            }
            free(db->location);
            free(db);
            if (owns_config) database_config_destroy(effective_config);
            if (error_code && *error_code == 0) {
                *error_code = ENOMEM;
            }
            return NULL;
        }
    } else {
        db->wal_manager = NULL;
    }

    // Set legacy WAL to NULL
    db->wal = NULL;

    // Store active config
    db->active_config = database_config_copy(effective_config);

    // Save config (skip for in-memory)
    if (!is_memory_only) {
        database_config_save(location, effective_config);
    }

    // Replay WAL for recovery (skip for in-memory)
    if (db->wal_manager != NULL) {
        db->is_rebuilding = 1;
        wal_manager_recover(db->wal_manager, db);
        db->is_rebuilding = 0;
    }

    refcounter_init((refcounter_t*)db);

    // Initialize vacuum infrastructure
    atomic_store(&db->vacuum_in_progress, 0);
    atomic_store(&db->open_cursor_count, 0);
    atomic_store(&db->vacuum_in_flight, 0);
    db->vacuum_task_id = 0;
    platform_lock_init(&db->vacuum_writer_lock);
    platform_condition_init(&db->vacuum_cvar);
    platform_lock_init(&db->cursor_count_mutex);
    platform_condition_init(&db->cursor_cvar);

    if (owns_config) {
        database_config_destroy(effective_config);
    }

    // Schedule background vacuum worker (only when enabled and not MANUAL_ONLY)
    if (db->wheel != NULL && db->active_config != NULL &&
        db->active_config->vacuum_config.background_interval_ms > 0 &&
        db->active_config->vacuum_config.mode != VACUUM_MODE_MANUAL_ONLY) {
        database_vacuum_schedule_background(db);
    }

    return db;
}

database_t* database_create_encrypted(const char* location,
                                       encrypted_database_config_t* config,
                                       int* error_code) {
    if (error_code) *error_code = 0;

    if (config == NULL) {
        if (error_code) *error_code = EINVAL;
        return NULL;
    }

    // Validate encryption type and key material
    if (config->type == ENCRYPTION_NONE) {
        // If type is NONE but key material was provided, that's an error
        if ((config->symmetric.key != NULL && config->symmetric.key_length > 0) ||
            (config->asymmetric.public_key_der != NULL && config->asymmetric.public_key_len > 0)) {
            if (error_code) *error_code = DATABASE_ERR_ENCRYPTION_KEY_INVALID;
            return NULL;
        }
        // No encryption requested — delegate to database_create_with_config
        return database_create_with_config(location, &config->config, error_code);
    }

    // Create the encryption context based on type
    encryption_t* encryption = NULL;

    if (config->type == ENCRYPTION_SYMMETRIC) {
        if (config->symmetric.key == NULL || config->symmetric.key_length == 0) {
            if (error_code) *error_code = DATABASE_ERR_ENCRYPTION_KEY_INVALID;
            return NULL;
        }
        encryption = encryption_create_symmetric(config->symmetric.key,
                                                  config->symmetric.key_length);
    } else if (config->type == ENCRYPTION_ASYMMETRIC) {
        if (config->asymmetric.public_key_der == NULL ||
            config->asymmetric.public_key_len == 0) {
            if (error_code) *error_code = DATABASE_ERR_ENCRYPTION_KEY_INVALID;
            return NULL;
        }
        encryption = encryption_create_asymmetric(
            config->asymmetric.private_key_der, config->asymmetric.private_key_len,
            config->asymmetric.public_key_der, config->asymmetric.public_key_len);
    } else {
        if (error_code) *error_code = DATABASE_ERR_ENCRYPTION_UNSUPPORTED;
        return NULL;
    }

    if (encryption == NULL) {
        if (error_code) *error_code = DATABASE_ERR_ENCRYPTION_KEY_INVALID;
        return NULL;
    }

    // Check if database already exists
    char config_path[1024];
    snprintf(config_path, sizeof(config_path), "%s/.config", location);
    struct stat st;
    bool db_exists = (stat(config_path, &st) == 0);

    if (db_exists) {
        // Load saved config to get stored salt/check for key verification
        database_config_t* saved_config = database_config_load(location);
        if (saved_config != NULL && saved_config->encryption.has_encryption &&
            saved_config->encryption.type != ENCRYPTION_NONE) {
            // Database is encrypted — verify the provided key matches
            encryption_t* verify_enc = encryption_create_from_config(
                config->type,
                config->symmetric.key, config->symmetric.key_length,
                config->asymmetric.private_key_der, config->asymmetric.private_key_len,
                config->asymmetric.public_key_der, config->asymmetric.public_key_len,
                saved_config->encryption.salt,
                saved_config->encryption.check);

            if (verify_enc == NULL) {
                // Key verification failed
                encryption_destroy(encryption);
                database_config_destroy(saved_config);
                if (error_code) *error_code = DATABASE_ERR_ENCRYPTION_KEY_INVALID;
                return NULL;
            }
            // Key verified — use the verified context
            encryption_destroy(encryption);
            encryption = verify_enc;

            database_config_destroy(saved_config);
        } else {
            // Database exists but is not encrypted — cannot add encryption
            // to an existing unencrypted database
            if (saved_config != NULL) {
                database_config_destroy(saved_config);
            }
            encryption_destroy(encryption);
            if (error_code) *error_code = DATABASE_ERR_ENCRYPTION_UNSUPPORTED;
            return NULL;
        }
    }

    // Store encryption key material in the config before saving
    // This ensures the config saved to disk includes salt/check for re-open
    database_config_t effective_config = config->config;
    effective_config.encryption.type = config->type;
    effective_config.encryption.has_encryption = 1;

    // Copy salt and check from the encryption context
    const uint8_t* salt = encryption_get_salt(encryption);
    const uint8_t* check = encryption_get_check(encryption);
    if (salt != NULL) {
        memcpy(effective_config.encryption.salt, salt, 16);
    }
    if (check != NULL) {
        memcpy(effective_config.encryption.check, check, 44);
    }

    // Delegate to database_create_with_config for the rest
    database_t* db = database_create_with_config(location, &effective_config, error_code);

    if (db != NULL) {
        db->encryption = encryption;
        if (db->wal_manager != NULL) {
            db->wal_manager->encryption = encryption;
        }
    } else {
        // database_create_with_config failed — clean up encryption context
        encryption_destroy(encryption);
    }

    return db;
}

database_t* database_create(const char* location, size_t lru_memory_mb,
                            wal_config_t* wal_config,
                            uint8_t chunk_size, uint32_t btree_node_size,
                            uint8_t enable_persist,
                            work_pool_t* pool, hierarchical_timing_wheel_t* wheel,
                            int* error_code) {
    // Create config from parameters
    database_config_t* config = database_config_default();
    if (config == NULL) {
        if (error_code) *error_code = ENOMEM;
        return NULL;
    }

    // Set values from parameters
    if (lru_memory_mb > 0) config->lru_memory_mb = lru_memory_mb;
    if (chunk_size > 0) config->chunk_size = chunk_size;
    if (btree_node_size > 0) config->btree_node_size = btree_node_size;
    config->enable_persist = enable_persist;

    // Set WAL config if provided
    if (wal_config != NULL) {
        config->wal_config = *wal_config;
    }

    // Set external resources
    config->external_pool = pool;
    config->external_wheel = wheel;
    if (pool != NULL) config->worker_threads = 0;  // Using external pool
    if (wheel != NULL) config->timer_resolution_ms = 0;  // Using external wheel

    database_t* db = database_create_with_config(location, config, error_code);
    database_config_destroy(config);

    return db;
}

// Persist the trie to disk (CBOR snapshot, without GC -- safe for teardown).
// Returns 0 on success, -1 if persist failed.
static int database_persist(database_t* db) {
    if (db == NULL) return -1;
    if (db->page_file != NULL) {
        return database_flush_dirty_bnodes(db);
    }
    return save_index(db);
}

void database_destroy(database_t* db) {
    if (db == NULL) return;

    refcounter_dereference((refcounter_t*)db);
    uint_fast32_t count = refcounter_count((refcounter_t*)db);

    if (count == 0) {
        // Signal that destruction is in progress so eviction task won't reschedule,
        // and so the vacuum task's destroying-check skips (no reschedule, just
        // decrements vacuum_in_flight).
        db->destroying = true;

        // Cancel any scheduled background vacuum task. cancel_timer invokes
        // the abort callback (database_vacuum_task_abort), which frees ctx and
        // decrements vacuum_in_flight. If the timer already fired (callback
        // running or queued), the cancel is a no-op on the timer ID — the
        // callback will see destroying=true, skip, and decrement.
        if (db->vacuum_task_id != 0 && db->wheel != NULL) {
            hierarchical_timing_wheel_cancel_timer(db->wheel, db->vacuum_task_id);
            db->vacuum_task_id = 0;
        }

        // Unregister eviction callback so no new eviction offsets are queued.
        if (db->bnode_cache != NULL) {
            db->bnode_cache->on_evict = NULL;
            db->bnode_cache->on_evict_data = NULL;
        }

        // Stop the timing wheel and worker pool BEFORE destroying data structures.
        // If we destroy the trie/LRU while workers are still running, a worker
        // thread might access freed memory or hold a lock on a destroyed mutex.
        // Note: Do NOT call wait_for_idle_signal here — debouncers reschedule
        // timers indefinitely, so the idle condition is never reached. The wheel
        // stop cancels all timers and the pool join ensures all workers complete.
        if (db->owns_wheel && db->wheel != NULL) {
            hierarchical_timing_wheel_stop(db->wheel);
        }
        if (db->owns_pool && db->pool != NULL) {
            work_pool_shutdown(db->pool);
            work_pool_join_all(db->pool);
        }

        // With an external pool, we can't shut it down. The eviction task
        // is self-rescheduling — once it sees destroying=true it won't
        // reschedule, but an in-flight instance may still be executing.
        // Wait for it to finish before we free db.
        if (!db->owns_pool && db->pool != NULL) {
            uint64_t offsets[64];
            eviction_queue_drain(&db->eviction_queue, offsets, 64);
            while (atomic_load(&db->eviction_in_flight) > 0) {
                // Yield to the worker thread executing the eviction task
                sched_yield();
            }
        }

        // Wait for any in-flight vacuum callback to complete before destroying
        // the structures it accesses. cancel_timer (called above) invokes the
        // abort callback for pending timers, which decrements vacuum_in_flight.
        // hierarchical_timing_wheel_stop (owns_wheel) also invokes abort for any
        // remaining timers. If the timer already fired (callback running on the
        // pool), the cancel is a no-op and we wait for the callback to finish
        // (it sees destroying=true, skips, and decrements). A timeout prevents
        // an infinite hang if something goes wrong.
        int vacuum_wait_ms = 0;
        while (atomic_load(&db->vacuum_in_flight) > 0 && vacuum_wait_ms < 5000) {
            usleep(1000);  // 1ms
            vacuum_wait_ms++;
        }

        // Flush all thread-local WALs to disk before destroying
        if (db->wal_manager) {
            wal_manager_flush(db->wal_manager);
        }

        // Persist trie before teardown (CBOR snapshot)
        // This ensures data survives across database_destroy/create cycles.
        {
            int persist_rc = database_persist(db);
            if (persist_rc == 0 && db->wal_manager != NULL) {
                // Durably commit the page-file checkpoint BEFORE discarding
                // the WAL. seal_and_compact below drops every sealed WAL
                // entry (mark_compacted=1) on the assumption that the
                // checkpoint is on disk; without this fsync a crash in the
                // window between persist and WAL-drop could lose both the
                // not-yet-flushed page file writes and the dropped WAL.
                if (db->page_file != NULL && db->page_file->fd >= 0) {
                    fsync(db->page_file->fd);
                }
                // Seal and compact WAL so entries already captured in the
                // snapshot are not replayed on next database creation.
                wal_manager_seal_and_compact(db->wal_manager);
            } else if (persist_rc != 0) {
                log_warn("database_destroy: persist failed (rc=%d), "
                         "WAL entries will be replayed on next open", persist_rc);
            }
        }

        // Destroy WAL manager (thread-local WAL)
        if (db->wal_manager) wal_manager_destroy(db->wal_manager);

        // Legacy WAL (should be NULL, but check for safety)
        if (db->wal) wal_destroy(db->wal);

        // Destroy LRU cache before trie — LRU holds REFERENCES to identifiers
        // that are also stored in the trie. Destroying LRU first decrements
        // refcounts (2→1), then trie destroy decrements (1→0) and frees.
        // Reversing the order causes use-after-free since trie frees identifiers
        // that the LRU still references.
        if (db->lru) database_lru_cache_destroy(db->lru);

        // Process any remaining eviction callbacks before destroying trie/bnode cache.
        // The pool is already shut down, so no new eviction callbacks will arrive.
        if (db->bnode_cache != NULL) {
            uint64_t offsets[64];
            size_t n;
            do {
                n = eviction_queue_drain(&db->eviction_queue, offsets, 64);
                for (size_t i = 0; i < n; i++) {
                    if (db->trie != NULL) {
                        hbtrie_null_entries_by_offset(db->trie, offsets[i]);
                    }
                    bnode_cache_complete_evict(db->bnode_cache, offsets[i]);
                }
            } while (n > 0);

            // Unregister callback before destroying
            db->bnode_cache->on_evict = NULL;
            db->bnode_cache->on_evict_data = NULL;
        }

        // Destroy trie (frees all identifiers and bnode entries)
        if (db->trie) hbtrie_destroy(db->trie);

        // Destroy page-based persistence (Phase 2)
        if (db->bnode_cache != NULL) {
            bnode_cache_mgr_t* mgr = db->bnode_cache->mgr;
            bnode_cache_destroy_file_cache(db->bnode_cache);
            if (mgr != NULL) bnode_cache_mgr_destroy(mgr);
        }
        if (db->page_file != NULL) {
            page_file_destroy(db->page_file);
        }

        // Destroy encryption context after WAL and page file are done
        // (both use encryption during seal/compact and final persist)
        if (db->encryption) {
            encryption_destroy(db->encryption);
            db->encryption = NULL;
        }

        // Destroy transaction manager
        if (db->tx_manager) tx_manager_destroy(db->tx_manager);

        // Destroy write lock shards (only concurrent mode initialized them)
        if (!db->sync_only) {
            for (size_t i = 0; i < WRITE_LOCK_SHARDS; i++) {
                spinlock_destroy(&db->write_locks[i]);
            }
        }

        // Wake any waiters before destroying vacuum/cursor condvars, then
        // destroy the vacuum synchronization primitives.
        platform_broadcast_condition(&db->vacuum_cvar);
        platform_broadcast_condition(&db->cursor_cvar);
        platform_lock_destroy(&db->vacuum_writer_lock);
        platform_condition_destroy(&db->vacuum_cvar);
        platform_lock_destroy(&db->cursor_count_mutex);
        platform_condition_destroy(&db->cursor_cvar);

        // Destroy active config
        if (db->active_config != NULL) {
            database_config_destroy(db->active_config);
        }

        // Destroy owned pool/wheel (already stopped/joined above)
        if (db->owns_pool && db->pool != NULL) {
            work_pool_destroy(db->pool);
        }
        if (db->owns_wheel && db->wheel != NULL) {
            hierarchical_timing_wheel_destroy(db->wheel);
        }

        free(db->location);
        free(db);

        // Drain TLS cache back to global pool
        memory_pool_tls_drain();
    }
    // If count > 0, other references exist and will handle cleanup when they dereference
}



// Helper: Get write lock shard from path
static inline size_t get_write_lock_shard(path_t* path) {
    if (path == NULL || path->identifiers.length == 0) {
        return 0;
    }

    // Hash the first identifier to get shard
    identifier_t* first_id = path->identifiers.data[0];
    if (first_id == NULL) {
        return 0;
    }

    // Simple hash: combine all chunks
    size_t hash = 0;
    for (size_t i = 0; i < first_id->chunks.length; i++) {
        chunk_t* chunk = first_id->chunks.data[i];
        if (chunk != NULL) {
            for (size_t j = 0; j < chunk->size; j++) {
                hash = hash * 31 + chunk->data[j];
            }
        }
    }

    return hash % WRITE_LOCK_SHARDS;
}

static void _database_put(database_put_ctx_t* ctx) {
    database_t* db = ctx->db;
    path_t* path = ctx->path;
    identifier_t* value = ctx->value;
    promise_t* promise = ctx->promise;

    // Begin MVCC transaction
    txn_desc_t* txn = tx_manager_begin(db->tx_manager);
    if (txn == NULL) {
        path_destroy(path);
        identifier_destroy(value);
        free(ctx);
        promise_resolve(promise, NULL);
        return;
    }

    // Write to thread-local WAL first (durability)
    buffer_t* entry = encode_put_entry_binary(path, value);
    if (entry != NULL) {
        thread_wal_t* twal = get_thread_wal(db->wal_manager);
        if (twal != NULL) {
            int result = thread_wal_write(twal, txn->txn_id, WAL_PUT, entry);
            if (result != 0) {
                log_warn("Failed to write to thread-local WAL");
            }
        }
        buffer_destroy(entry);
    }

    // Acquire sharded write lock (prevents root-node contention)
    size_t shard = get_write_lock_shard(path);
    spinlock_lock(&db->write_locks[shard]);

    // Apply to trie with MVCC
    hbtrie_insert(db->trie, path, value, txn->txn_id);

    // Release write lock
    spinlock_unlock(&db->write_locks[shard]);

    // Commit transaction
    tx_manager_commit(db->tx_manager, txn);

    // Update LRU cache
    path_t* copied_path = path_copy(path);
    identifier_t* value_ref = REFERENCE(value, identifier_t);
    identifier_t* ejected = database_lru_cache_put(db->lru, copied_path, value_ref);
    if (ejected) {
        identifier_destroy(ejected);
    }

    // Clean up
    path_destroy(path);
    identifier_destroy(value);
    txn_desc_destroy(txn);
    free(ctx);

    promise_resolve(promise, NULL);
}

static void _database_get(database_get_ctx_t* ctx) {
    database_t* db = ctx->db;
    path_t* path = ctx->path;
    promise_t* promise = ctx->promise;

    // Check LRU cache first
    identifier_t* value = database_lru_cache_get(db->lru, path);
    if (value != NULL) {
        path_destroy(path);
        free(ctx);
        // database_lru_cache_get REFERENCE'd the value (count+1) for the
        // caller. CONSUME transfers this reference to the callback: sets
        // yield=1. The callback must REFERENCE (which consumes the yield)
        // then identifier_destroy (which decrements the count).
        identifier_t* consumed = (identifier_t*)CONSUME(value, identifier_t);
        promise_resolve(promise, consumed);
        return;
    }

    // Get last committed transaction ID (lock-free read)
    transaction_id_t read_txn_id = tx_manager_get_last_committed(db->tx_manager);

    // Look up in trie with MVCC (lock-free!)
    value = hbtrie_find(db->trie, path, read_txn_id);

    // Add to LRU cache if found
    if (value != NULL) {
        path_t* copied_path = path_copy(path);
        identifier_t* cached = REFERENCE(value, identifier_t);
        identifier_t* ejected = database_lru_cache_put(db->lru, copied_path, cached);
        if (ejected) {
            identifier_destroy(ejected);
        }
    }

    path_destroy(path);
    free(ctx);

    // hbtrie_find returns a reference the caller owns (count+1).
    // REFERENCE above created a separate reference for LRU.
    // CONSUME transfers the caller's reference to the callback: sets
    // yield=1. The callback must REFERENCE (which consumes the yield)
    // then identifier_destroy (which decrements the count).
    if (value != NULL) {
        identifier_t* consumed = (identifier_t*)CONSUME(value, identifier_t);
        promise_resolve(promise, consumed);
    } else {
        promise_resolve(promise, NULL);
    }
}

static void _database_delete(database_delete_ctx_t* ctx) {
    database_t* db = ctx->db;
    path_t* path = ctx->path;
    promise_t* promise = ctx->promise;

    // Begin transaction
    txn_desc_t* txn = tx_manager_begin(db->tx_manager);
    if (txn == NULL) {
        path_destroy(path);
        promise_resolve(promise, NULL);
        free(ctx);
        return;
    }

    // Write to thread-local WAL
    buffer_t* entry = encode_delete_entry_binary(path);
    if (entry != NULL) {
        thread_wal_t* twal = get_thread_wal(db->wal_manager);
        if (twal != NULL) {
            int result = thread_wal_write(twal, txn->txn_id, WAL_DELETE, entry);
            if (result != 0) {
                log_warn("Failed to write to thread-local WAL");
            }
        }
        buffer_destroy(entry);
    }

    // Acquire sharded write lock (prevents root-node contention)
    size_t shard = get_write_lock_shard(path);
    spinlock_lock(&db->write_locks[shard]);

    // Remove from trie with MVCC (creates tombstone)
    identifier_t* removed = hbtrie_delete(db->trie, path, txn->txn_id);

    // Release write lock
    spinlock_unlock(&db->write_locks[shard]);

    // Commit transaction
    tx_manager_commit(db->tx_manager, txn);
    txn_desc_destroy(txn);

    // Remove from LRU cache
    database_lru_cache_delete(db->lru, path);

    path_destroy(path);
    free(ctx);

    if (removed) {
        identifier_destroy(removed);
    }

    promise_resolve(promise, NULL);
}

void database_put(database_t* db, path_t* path,
                   identifier_t* value, promise_t* promise) {
    if (db == NULL || path == NULL || value == NULL || promise == NULL) {
        if (path) path_destroy(path);
        if (value) identifier_destroy(value);
        if (promise) {
            promise_resolve(promise, NULL);
        }
        return;
    }

    // Async operations require a worker pool — not available in sync_only mode
    if (db->sync_only) {
        int rc = database_put_sync(db, path, value);
        int* result = malloc(sizeof(int));
        if (result) *result = rc;
        promise_resolve(promise, result);
        return;
    }

    database_put_ctx_t* ctx = get_clear_memory(sizeof(database_put_ctx_t));
    if (ctx == NULL) {
        path_destroy(path);
        identifier_destroy(value);
        promise_resolve(promise, NULL);
        return;
    }

    ctx->db = db;
    ctx->path = path;
    ctx->value = value;
    ctx->promise = promise;

    work_t* work = work_create(
        (void (*)(void*))_database_put,
        (void (*)(void*))abort_database_put,
        ctx);
    if (work == NULL) {
        free(ctx);
        path_destroy(path);
        identifier_destroy(value);
        promise_resolve(promise, NULL);
        return;
    }

    refcounter_yield((refcounter_t*) work);
    work_pool_enqueue(db->pool, work);
}

void database_get(database_t* db, path_t* path, promise_t* promise) {
    if (db == NULL || path == NULL || promise == NULL) {
        if (path) path_destroy(path);
        promise_resolve(promise, NULL);
        return;
    }

    // Async operations require a worker pool — not available in sync_only mode
    if (db->sync_only) {
        identifier_t* result = NULL;
        database_get_sync(db, path, &result);
        promise_resolve(promise, result);
        return;
    }

    database_get_ctx_t* ctx = get_clear_memory(sizeof(database_get_ctx_t));
    if (ctx == NULL) {
        path_destroy(path);
        promise_resolve(promise, NULL);
        return;
    }

    ctx->db = db;
    ctx->path = path;
    ctx->promise = promise;

    work_t* work = work_create(
        (void (*)(void*))_database_get,
        (void (*)(void*))abort_database_get,
        ctx);
    if (work == NULL) {
        free(ctx);
        path_destroy(path);
        promise_resolve(promise, NULL);
        return;
    }

    refcounter_yield((refcounter_t*) work);
    work_pool_enqueue(db->pool, work);
}

void database_delete(database_t* db, path_t* path, promise_t* promise) {
    if (db == NULL || path == NULL || promise == NULL) {
        if (path) path_destroy(path);
        promise_resolve(promise, NULL);
        return;
    }

    // Async operations require a worker pool — not available in sync_only mode
    if (db->sync_only) {
        int rc = database_delete_sync(db, path);
        int* result = malloc(sizeof(int));
        if (result) *result = rc;
        promise_resolve(promise, result);
        return;
    }

    database_delete_ctx_t* ctx = get_clear_memory(sizeof(database_delete_ctx_t));
    if (ctx == NULL) {
        path_destroy(path);
        promise_resolve(promise, NULL);
        return;
    }

    ctx->db = db;
    ctx->path = path;
    ctx->promise = promise;

    work_t* work = work_create(
        (void (*)(void*))_database_delete,
        (void (*)(void*))abort_database_delete,
        ctx);
    if (work == NULL) {
        free(ctx);
        path_destroy(path);
        promise_resolve(promise, NULL);
        return;
    }

    refcounter_yield((refcounter_t*) work);
    work_pool_enqueue(db->pool, work);
}

int database_snapshot(database_t* db) {
    if (db == NULL) return -1;

    // Flush all thread-local WALs to ensure all data is persisted
    // This must be done before saving the index to guarantee consistency
    if (db->wal_manager != NULL) {
        wal_manager_flush(db->wal_manager);
    }

    // MVCC: Trigger GC to clean up old versions
    if (db->tx_manager != NULL) {
        tx_manager_gc(db->tx_manager);
    } else if (db->sync_only) {
        // Sync-only mode: no tx_manager, but version chains still accumulate.
        // Prune all but the newest version (safe in single-threaded context).
        hbtrie_gc_unsafe(db->trie);
    }

    // Save index to disk
    if (db->page_file != NULL) {
        // Vacuum threshold check (only if mode allows auto-trigger).
        // STRICT: always auto-vacuums when threshold crossed.
        // ADAPTIVE: snapshot-triggered vacuum runs even though background
        //   vacuum is disabled — the caller already paid for the snapshot,
        //   so this is a cheap opportunity to reclaim space.
        // MANUAL_ONLY: never auto-vacuums; falls through to flush.
        vacuum_mode_t mode = VACUUM_MODE_STRICT;  // default
        double threshold = 0.30;  // default
        if (db->active_config != NULL) {
            mode = db->active_config->vacuum_config.mode;
            threshold = db->active_config->vacuum_config.stale_threshold;
        }
        if (mode == VACUUM_MODE_STRICT || mode == VACUUM_MODE_ADAPTIVE) {
            double ratio = page_file_stale_ratio(db->page_file);
            if (ratio >= threshold) {
                // Snapshot-triggered vacuum waits for cursors if any are open.
                return database_vacuum_auto(db);
            }
        }
        return database_flush_dirty_bnodes(db);
    }
    return save_index(db);
}

size_t database_count(database_t* db) {
    if (db == NULL) return 0;

    // Returns LRU cache entry count, which may undercount after eviction.
    // This is a fast approximation; for exact counts, walk the trie.
    if (db->sync_only) {
        return database_lru_cache_size_unsafe(db->lru);
    }
    return database_lru_cache_size(db->lru);
}

// Forward declarations for vacuum walk helpers.
static int database_vacuum_walk_hbnode(database_t* db, hbtrie_node_t* hn,
                                         bnode_t* parent_bnode, size_t parent_entry_index,
                                         page_file_t* new_pf, offset_remap_t* remap);
static int database_vacuum_walk_bnode(database_t* db, bnode_t* bn,
                                       bnode_t* parent_bnode, size_t parent_entry_index,
                                       page_file_t* new_pf, offset_remap_t* remap);
static int database_vacuum_rewrite(database_t* db, const char* tmp_path);

// Walk an hbtrie_node post-order: recurse into child hbtrie_nodes (via leaf
// entries) first, then walk this hbnode's btree and write each bnode.
// Patching parent's child_disk_offset happens in two places:
//   - For hbtrie-children: patched by the recursive walk_hbnode call (which
//     writes the child's btree root and calls walk_bnode's parent-patch on
//     the cross-hbnode boundary).
//   - For interior bnode-children: patched by walk_bnode as we descend.
static int database_vacuum_walk_hbnode(database_t* db, hbtrie_node_t* hn,
                                         bnode_t* parent_bnode, size_t parent_entry_index,
                                         page_file_t* new_pf, offset_remap_t* remap) {
    if (hn == NULL || hn->btree == NULL) return 0;

    // PHASE 1 — recurse into child hbtrie_nodes via leaf entries (post-order).
    // Walk the whole btree of this hbnode to find every leaf entry that points
    // to a child hbtrie_node, and recurse into each before writing any bnode
    // in this hbnode's btree.
    //
    // After a reopen, child bnodes/hbtrie_nodes may be lazy-loaded (in-memory
    // pointer NULL, child_disk_offset set). We must load them from disk here
    // so the recursive walk can patch the parent's child_disk_offset to the
    // new file's offset. Otherwise the parent's stale offset would point to
    // garbage in the new file after the swap.
    vec_t(bnode_t*) leaf_stack;
    vec_init(&leaf_stack);
    vec_push(&leaf_stack, hn->btree);

    while (leaf_stack.length > 0) {
        bnode_t* bn = vec_pop(&leaf_stack);

        for (size_t i = 0; i < bn->entries.length; i++) {
            bnode_entry_t* entry = &bn->entries.data[i];

            if (entry->is_bnode_child) {
                // Interior bnode — descend within this btree. We'll write it
                // in PHASE 2; just keep walking to find leaf entries.
                if (entry->child_bnode == NULL && entry->child_disk_offset != 0
                        && db->trie->fcache != NULL) {
                    if (bnode_entry_lazy_load_bnode_child(entry, db->trie->fcache,
                                                           db->trie->chunk_size,
                                                           db->trie->btree_node_size) != 0) {
                        vec_deinit(&leaf_stack);
                        return -1;
                    }
                }
                if (entry->child_bnode != NULL) {
                    vec_push(&leaf_stack, entry->child_bnode);
                }
            } else {
                // Leaf entry — recurse into hbtrie children.
                if (!entry->is_bnode_child && !entry->has_value
                        && entry->child == NULL && entry->child_disk_offset != 0
                        && db->trie->fcache != NULL) {
                    if (bnode_entry_lazy_load_hbtrie_child(entry, db->trie->fcache,
                                                            db->trie->chunk_size,
                                                            db->trie->btree_node_size) != 0) {
                        vec_deinit(&leaf_stack);
                        return -1;
                    }
                }
                if (!entry->is_bnode_child && !entry->has_value && entry->child != NULL) {
                    int rc = database_vacuum_walk_hbnode(db, entry->child, bn, i, new_pf, remap);
                    if (rc != 0) { vec_deinit(&leaf_stack); return rc; }
                }
                if (entry->trie_child == NULL && entry->child_disk_offset != 0
                        && entry->has_value && db->trie->fcache != NULL) {
                    if (bnode_entry_lazy_load_trie_child(entry, db->trie->fcache,
                                                          db->trie->chunk_size,
                                                          db->trie->btree_node_size) != 0) {
                        vec_deinit(&leaf_stack);
                        return -1;
                    }
                }
                if (entry->trie_child != NULL) {
                    int rc = database_vacuum_walk_hbnode(db, entry->trie_child, bn, i, new_pf, remap);
                    if (rc != 0) { vec_deinit(&leaf_stack); return rc; }
                }
            }
        }
    }
    vec_deinit(&leaf_stack);

    // PHASE 2 — walk this hbnode's btree (all bnodes in it) and write each
    // post-order. The leaf entries pointing to child hbtrie_nodes already
    // have their child_disk_offset patched by the recursive walks above.
    // Interior bnode child_disk_offset fields are patched as we walk down.
    return database_vacuum_walk_bnode(db, hn->btree, parent_bnode, parent_entry_index, new_pf, remap);
}

// Walk a bnode tree (within one hbtrie_node) post-order: children first, then
// this bnode. After writing this bnode, patch the parent's child_disk_offset.
static int database_vacuum_walk_bnode(database_t* db, bnode_t* bn,
                                       bnode_t* parent_bnode, size_t parent_entry_index,
                                       page_file_t* new_pf, offset_remap_t* remap) {
    if (bn == NULL) return 0;

    // Recurse into child bnodes first (post-order).
    // Lazy-load any interior child bnodes that are on disk but not in memory
    // (happens after reopen when those bnodes were never accessed). Loading
    // them here lets us patch this entry's child_disk_offset to the new file
    // offset during the post-order write below.
    for (size_t i = 0; i < bn->entries.length; i++) {
        bnode_entry_t* entry = &bn->entries.data[i];
        if (entry->is_bnode_child && entry->child_bnode == NULL
                && entry->child_disk_offset != 0 && db->trie->fcache != NULL) {
            if (bnode_entry_lazy_load_bnode_child(entry, db->trie->fcache,
                                                   db->trie->chunk_size,
                                                   db->trie->btree_node_size) != 0) {
                return -1;
            }
        }
        if (entry->is_bnode_child && entry->child_bnode != NULL) {
            int rc = database_vacuum_walk_bnode(db, entry->child_bnode, bn, i, new_pf, remap);
            if (rc != 0) return rc;
        }
    }

    // Serialize + write this bnode. All child_disk_offset fields on interior
    // entries are now up-to-date (patched by the recursive calls above), and
    // leaf entries pointing to hbtrie children were patched by walk_hbnode.
    uint8_t* buf = NULL;
    size_t len = 0;
    int rc = bnode_serialize_v3(bn, db->trie->chunk_size, &buf, &len);
    if (rc != 0) return rc;

    uint64_t new_offset = 0;
    uint64_t bids[64] = {0};
    size_t num_bids = 0;
    rc = page_file_write_node(new_pf, buf, len, &new_offset, bids, 64, &num_bids);
    free(buf);
    if (rc != 0) return rc;

    // Record old -> new in remap (for any future offset fixups).
    if (bn->disk_offset != UINT64_MAX && bn->disk_offset != 0) {
        offset_remap_put(remap, bn->disk_offset, new_offset);
    }
    bn->disk_offset = new_offset;
    bn->is_dirty = 0;

    // Patch parent's child_disk_offset in memory so the parent's serialized
    // bytes (written next, since we're post-order) contain the new offset.
    if (parent_bnode != NULL && parent_entry_index < parent_bnode->entries.length) {
        parent_bnode->entries.data[parent_entry_index].child_disk_offset = new_offset;
    }

    return 0;
}

static int database_vacuum_rewrite(database_t* db, const char* tmp_path) {
    // Create a fresh page_file_t for the tmp file with same block_size,
    // num_superblocks, encryption as the live file.
    page_file_t* new_pf = page_file_create(tmp_path,
                                           db->page_file->block_size,
                                           db->page_file->num_superblocks,
                                           db->page_file->encryption);
    if (new_pf == NULL) return -1;
    if (page_file_open(new_pf, 1) != 0) {
        page_file_destroy(new_pf);
        return -1;
    }

    offset_remap_t* remap = offset_remap_create(1024);
    if (remap == NULL) {
        page_file_destroy(new_pf);
        return -1;
    }

    hbtrie_node_t* root = atomic_load_ptr(&db->trie->root, hbtrie_node_t*);
    if (root == NULL) {
        // Empty trie — just write an empty superblock.
        page_file_write_superblock(new_pf, 0, 0, NULL);
        page_file_destroy(new_pf);
        offset_remap_destroy(remap);
        return 0;
    }

    // Post-order walk: write each bnode, patch parent's child_disk_offset.
    int rc = database_vacuum_walk_hbnode(db, root, NULL, 0, new_pf, remap);

    if (rc == 0) {
        // Root hbtrie_node's btree root bnode is the new root offset.
        // Match database_flush_dirty_bnodes: root->disk_offset = root->btree->disk_offset.
        root->disk_offset = root->btree->disk_offset;

        // Write superblock with new root offset + last committed txn.
        transaction_id_t last_txn;
        if (db->tx_manager != NULL) {
            last_txn = tx_manager_get_last_committed(db->tx_manager);
        } else {
            last_txn.time = 0;
            last_txn.nanos = 0;
            last_txn.count = 0;
        }
        rc = page_file_write_superblock(new_pf, root->disk_offset, 0, &last_txn);
    }

    if (rc != 0) {
        page_file_destroy(new_pf);
        unlink(tmp_path);
        offset_remap_destroy(remap);
        return rc;
    }

    // fsync the new file so the swap is durable.
    fsync(new_pf->fd);

    offset_remap_destroy(remap);
    page_file_destroy(new_pf);  // closes the tmp fd; swap will reopen.
    return 0;
}

// Drain in-flight work before vacuum. Returns 0 on success, -EBUSY on timeout.
//
// For non-sync_only mode we must ensure no async work modifies the trie
// during the vacuum walk, and that queued eviction offsets don't survive
// the page-file swap with stale offsets:
//   1. The eviction task checks vacuum_in_progress (already set by the
//      caller) and skips its body — no polling of eviction_in_flight is
//      needed because the task self-reschedules forever (the counter
//      never reaches 0 for a live db). The race window (task mid-body
//      when vacuum_in_progress is set) is microseconds and the task body
//      only nulls entries whose child_disk_offset matches a queued
//      offset — those children are not in the in-memory trie anyway.
//   2. Drain the eviction_queue ourselves so queued offsets are nulled
//      before the page-file swap renders them invalid.
//   3. tx_manager_gc — prune unreachable versions from MVCC chains so
//      vacuum does not copy them as live data.
//
// Note: polling work_pool idleCount == size is incompatible with a
// running timing wheel (the wheel's tick re-enqueues itself forever, so
// idleCount never reaches size). We skip that check — the eviction-task
// skip + queue drain covers the trie-modifying async work.
//
// timeout_ms is currently unused (kept in the signature for forward
// compatibility with future bounded-wait extensions).
static int database_vacuum_drain(database_t* db, uint32_t timeout_ms) {
    (void)timeout_ms;
    if (db == NULL) return -1;

    // Drain queued eviction offsets here so they don't survive the page
    // file swap with stale offsets. The eviction task skips its body
    // while vacuum_in_progress is set, so it won't race with us.
    if (db->trie != NULL && db->bnode_cache != NULL) {
        uint64_t offsets[64];
        size_t n;
        do {
            n = eviction_queue_drain(&db->eviction_queue, offsets, 64);
            for (size_t i = 0; i < n; i++) {
                hbtrie_null_entries_by_offset(db->trie, offsets[i]);
                bnode_cache_complete_evict(db->bnode_cache, offsets[i]);
            }
        } while (n > 0);
    }

    // GC version chains (prunes unreachable versions so they're not
    // copied as live data during the rewrite).
    if (db->tx_manager != NULL) {
        tx_manager_gc(db->tx_manager);
    }

    return 0;
}

// Shared core: assumes vacuum_in_progress is already set and cursors are
// already checked/handled by the caller. Returns 0 on success, <0 on error.
static int database_vacuum_run(database_t* db) {
    int rc = 0;
    char* tmp_path = NULL;

    do {
        // For sync_only: no work pool / no tx_manager to drain.
        // For non-sync_only: drain in-flight work with bounded timeout.
        // Abort with -EBUSY if drain times out (vacuum_in_progress is
        // cleared by the caller's epilogue).
        if (!db->sync_only) {
            uint32_t drain_timeout = 0;
            if (db->active_config != NULL) {
                drain_timeout = db->active_config->vacuum_config.drain_timeout_ms;
            }
            rc = database_vacuum_drain(db, drain_timeout);
            if (rc != 0) break;
        }

        // Build tmp path: <page_file->path>.vacuum.tmp
        size_t plen = strlen(db->page_file->path);
        tmp_path = get_clear_memory(plen + 12);
        if (tmp_path == NULL) { rc = -1; break; }
        memcpy(tmp_path, db->page_file->path, plen);
        memcpy(tmp_path + plen, ".vacuum.tmp", 11);
        tmp_path[plen + 11] = '\0';

        rc = database_vacuum_rewrite(db, tmp_path);
        if (rc != 0) break;

        // Atomic swap: rename tmp over live, reopen fd, reset stale_mgr.
        stale_region_mgr_t* new_mgr = stale_region_mgr_create();
        rc = page_file_vacuum_file_swap(db->page_file, tmp_path, new_mgr);
        if (rc != 0) {
            // Swap failed; delete the tmp file and destroy the unused new_mgr.
            unlink(tmp_path);
            if (new_mgr != NULL) stale_region_mgr_destroy(new_mgr);
        }
        // On success, tmp_path was consumed by rename — free the buffer but
        // the unlink below is a no-op (ENOENT).
    } while (0);

    // Clean up tmp_path buffer; unlink is a no-op if rename already consumed it.
    if (tmp_path != NULL) {
        unlink(tmp_path);
        free(tmp_path);
    }
    return rc;
}

int database_vacuum(database_t* db) {
    if (db == NULL) return -1;
    // Nothing to vacuum if no page file (in-memory mode).
    if (db->page_file == NULL) return 0;

    // Manual trigger: refuse if cursors are open.
    if (atomic_load(&db->open_cursor_count) > 0) {
        return -EBUSY;
    }

    // Min file size gate — too small to bother.
    uint64_t fsz = page_file_size(db->page_file);
    if (db->active_config != NULL &&
        fsz < db->active_config->vacuum_config.min_file_size_bytes) {
        return 0;
    }

    // Min stale bytes gate — not enough to reclaim.
    uint64_t stale = stale_region_total(db->page_file->stale_mgr);
    if (db->active_config != NULL &&
        stale < db->active_config->vacuum_config.min_stale_bytes) {
        return 0;
    }

    // Set vacuum_in_progress — writers will block from here.
    atomic_store(&db->vacuum_in_progress, 1);

    int rc = database_vacuum_run(db);

    // Resume writers and wake any new-cursor-creation waiters.
    atomic_store(&db->vacuum_in_progress, 0);
    platform_lock(&db->vacuum_writer_lock);
    platform_broadcast_condition(&db->vacuum_cvar);
    platform_unlock(&db->vacuum_writer_lock);
    platform_lock(&db->cursor_count_mutex);
    platform_broadcast_condition(&db->cursor_cvar);
    platform_unlock(&db->cursor_count_mutex);

    return rc;
}

int database_vacuum_auto(database_t* db) {
    if (db == NULL) return -1;
    // Nothing to vacuum if no page file (in-memory mode).
    if (db->page_file == NULL) return 0;

    // Min file size gate — too small to bother.
    uint64_t fsz = page_file_size(db->page_file);
    if (db->active_config != NULL &&
        fsz < db->active_config->vacuum_config.min_file_size_bytes) {
        return 0;
    }

    // Min stale bytes gate — not enough to reclaim.
    uint64_t stale = stale_region_total(db->page_file->stale_mgr);
    if (db->active_config != NULL &&
        stale < db->active_config->vacuum_config.min_stale_bytes) {
        return 0;
    }

    // Wait for cursors to close (wait-loop to handle races / spurious wakes).
    uint32_t cursor_wait_ms = 60000;  // default
    if (db->active_config != NULL) {
        cursor_wait_ms = db->active_config->vacuum_config.cursor_close_wait_ms;
    }

    platform_lock(&db->cursor_count_mutex);
    while (atomic_load(&db->open_cursor_count) > 0) {
        int rc = platform_condition_timedwait(&db->cursor_count_mutex,
                                               &db->cursor_cvar,
                                               cursor_wait_ms);
        if (rc != 0) {
            // Timeout (ETIMEDOUT) or error — give up this tick.
            platform_unlock(&db->cursor_count_mutex);
            return -EBUSY;
        }
    }
    // Hold the mutex while setting vacuum_in_progress to block new cursors
    // before the race window reopens.
    atomic_store(&db->vacuum_in_progress, 1);
    platform_unlock(&db->cursor_count_mutex);

    int rc = database_vacuum_run(db);

    // Resume writers and wake any new-cursor-creation waiters.
    atomic_store(&db->vacuum_in_progress, 0);
    platform_lock(&db->vacuum_writer_lock);
    platform_broadcast_condition(&db->vacuum_cvar);
    platform_unlock(&db->vacuum_writer_lock);
    platform_lock(&db->cursor_count_mutex);
    platform_broadcast_condition(&db->cursor_cvar);
    platform_unlock(&db->cursor_count_mutex);

    return rc;
}

// ============================================================================
// Synchronous API Implementation
// ============================================================================

int database_put_sync(database_t* db, path_t* path, identifier_t* value) {
    // Validation (shared)
    if (db == NULL || path == NULL || value == NULL) {
        if (path) path_destroy(path);
        if (value) identifier_destroy(value);
        return -1;
    }

    // Block if a vacuum is in progress
    int vb_rc = database_vacuum_block_if_in_progress(db);
    if (vb_rc != 0) {
        path_destroy(path);
        identifier_destroy(value);
        return vb_rc;
    }

    // Fast path: sync-only mode — skip MVCC, locks, and write_locks
    if (db->sync_only) {
        transaction_id_t txn_id = transaction_id_get_next();

        // Write to thread-local WAL (same durability as concurrent path)
        buffer_t* entry = encode_put_entry_binary(path, value);
        if (entry != NULL) {
            thread_wal_t* twal = get_thread_wal(db->wal_manager);
            if (twal != NULL) {
                int result = thread_wal_write(twal, txn_id, WAL_PUT, entry);
                if (result != 0) {
                    log_error("Failed to write to thread-local WAL (result=%d, txn_id=%lu.%09lu.%lu)",
                             result, txn_id.time, txn_id.nanos, txn_id.count);
                } else {
                    log_debug("Wrote PUT to WAL (txn_id=%lu.%09lu.%lu)",
                             txn_id.time, txn_id.nanos, txn_id.count);
                }
            } else {
                log_error("get_thread_wal returned NULL - cannot write to WAL");
            }
            buffer_destroy(entry);
        } else {
            log_error("encode_put_entry_binary returned NULL - cannot write to WAL");
        }

        // Apply to trie without per-node locks or MVCC version tracking
        hbtrie_insert_unsafe(db->trie, path, value, txn_id);

        // Update LRU cache (unsafe — no mutex)
        path_t* copied_path = path_copy(path);
        identifier_t* value_ref = REFERENCE(value, identifier_t);
        identifier_t* ejected = database_lru_cache_put_unsafe(db->lru, copied_path, value_ref);
        if (ejected) {
            identifier_destroy(ejected);
        }

        // Cleanup
        path_destroy(path);
        identifier_destroy(value);

        return 0;
    }

    // --- Concurrent path (unchanged) ---

    // Begin MVCC transaction
    txn_desc_t* txn = tx_manager_begin(db->tx_manager);
    if (txn == NULL) {
        path_destroy(path);
        identifier_destroy(value);
        return -1;
    }

    // Write to thread-local WAL
    buffer_t* entry = encode_put_entry_binary(path, value);
    if (entry != NULL) {
        thread_wal_t* twal = get_thread_wal(db->wal_manager);
        if (twal != NULL) {
            int result = thread_wal_write(twal, txn->txn_id, WAL_PUT, entry);
            if (result != 0) {
                log_error("Failed to write to thread-local WAL (result=%d, txn_id=%lu.%09lu.%lu)",
                         result, txn->txn_id.time, txn->txn_id.nanos, txn->txn_id.count);
            } else {
                log_debug("Wrote PUT to WAL (txn_id=%lu.%09lu.%lu)",
                         txn->txn_id.time, txn->txn_id.nanos, txn->txn_id.count);
            }
        } else {
            log_error("get_thread_wal returned NULL - cannot write to WAL");
        }
        buffer_destroy(entry);
    } else {
        log_error("encode_put_entry_binary returned NULL - cannot write to WAL");
    }

    // Acquire sharded write lock. Per-node spinlocks in hbtrie_insert
    // provide fine-grained exclusion within the trie, but the shard lock
    // prevents root-node contention when many threads write concurrently.
    // Without it, all writers contend on the root spinlock, serializing
    // access regardless of key distribution.
    size_t shard = get_write_lock_shard(path);
    spinlock_lock(&db->write_locks[shard]);

    // Apply to trie with MVCC
    hbtrie_insert(db->trie, path, value, txn->txn_id);

    // Release write lock
    spinlock_unlock(&db->write_locks[shard]);

    // Commit transaction
    tx_manager_commit(db->tx_manager, txn);

    // Update LRU cache
    path_t* copied_path = path_copy(path);
    identifier_t* value_ref = REFERENCE(value, identifier_t);
    identifier_t* ejected = database_lru_cache_put(db->lru, copied_path, value_ref);
    if (ejected) {
        identifier_destroy(ejected);
    }

    // Cleanup
    path_destroy(path);
    identifier_destroy(value);
    txn_desc_destroy(txn);

    return 0;
}

int database_get_sync(database_t* db, path_t* path, identifier_t** result) {
    // Initialize output
    if (result == NULL) {
        if (path) path_destroy(path);
        return -1;
    }
    *result = NULL;

    // Validation
    if (db == NULL || path == NULL) {
        if (path) path_destroy(path);
        return -1;
    }

    // Block if a vacuum is in progress
    int vb_rc = database_vacuum_block_if_in_progress(db);
    if (vb_rc != 0) {
        path_destroy(path);
        return vb_rc;
    }

    // Fast path: sync-only mode — skip seqlocks and tx_manager
    if (db->sync_only) {
        // Check LRU cache first (unsafe — no mutex)
        identifier_t* value = database_lru_cache_get_unsafe(db->lru, path);
        if (value != NULL) {
            path_destroy(path);
            *result = value;
            return 0;
        }

        // Look up in trie without seqlocks or MVCC version resolution
        value = hbtrie_find_unsafe(db->trie, path);

        // Add to LRU cache if found (unsafe — no mutex)
        if (value != NULL) {
            path_t* copied_path = path_copy(path);
            identifier_t* cached = REFERENCE(value, identifier_t);
            identifier_t* ejected = database_lru_cache_put_unsafe(db->lru, copied_path, cached);
            if (ejected) {
                identifier_destroy(ejected);
            }
        }

        path_destroy(path);

        if (value != NULL) {
            *result = value;
            return 0;
        } else {
            return -2;  // Not found
        }
    }

    // --- Concurrent path (unchanged) ---

    // Check LRU cache first
    identifier_t* value = database_lru_cache_get(db->lru, path);
    if (value != NULL) {
        path_destroy(path);
        // Transfer ownership of the reference from database_lru_cache_get to the caller.
        // database_lru_cache_get already incremented the refcount, so we pass it directly.
        *result = value;
        return 0;
    }

    // Get last committed transaction ID (lock-free read)
    transaction_id_t read_txn_id = tx_manager_get_last_committed(db->tx_manager);

    // Look up in trie with MVCC (lock-free!)
    value = hbtrie_find(db->trie, path, read_txn_id);

    // Add to LRU cache if found
    if (value != NULL) {
        path_t* copied_path = path_copy(path);
        identifier_t* cached = REFERENCE(value, identifier_t);
        identifier_t* ejected = database_lru_cache_put(db->lru, copied_path, cached);
        if (ejected) {
            identifier_destroy(ejected);
        }
    }

    path_destroy(path);

    if (value != NULL) {
        // Transfer ownership of the reference from hbtrie_find to the caller.
        // hbtrie_find already incremented the refcount, so we pass it directly.
        *result = value;
        return 0;
    } else {
        return -2;  // Not found
    }
}

int database_delete_sync(database_t* db, path_t* path) {
    // Validation
    if (db == NULL || path == NULL) {
        if (path) path_destroy(path);
        return -1;
    }

    // Block if a vacuum is in progress
    int vb_rc = database_vacuum_block_if_in_progress(db);
    if (vb_rc != 0) {
        path_destroy(path);
        return vb_rc;
    }

    // Fast path: sync-only mode — skip MVCC, locks, and write_locks
    if (db->sync_only) {
        transaction_id_t txn_id = transaction_id_get_next();

        // Write to thread-local WAL
        buffer_t* entry = encode_delete_entry_binary(path);
        if (entry != NULL) {
            thread_wal_t* twal = get_thread_wal(db->wal_manager);
            if (twal != NULL) {
                int result = thread_wal_write(twal, txn_id, WAL_DELETE, entry);
                if (result != 0) {
                    log_warn("Failed to write to thread-local WAL");
                }
            }
            buffer_destroy(entry);
        }

        // Remove from trie without per-node locks or MVCC version tracking
        identifier_t* removed = hbtrie_delete_unsafe(db->trie, path, txn_id);

        // Remove from LRU cache (unsafe — no mutex)
        database_lru_cache_delete_unsafe(db->lru, path);

        path_destroy(path);

        if (removed) {
            identifier_destroy(removed);
        }

        return 0;
    }

    // --- Concurrent path (unchanged) ---

    // Begin transaction
    txn_desc_t* txn = tx_manager_begin(db->tx_manager);
    if (txn == NULL) {
        path_destroy(path);
        return -1;
    }

    // Write to thread-local WAL
    buffer_t* entry = encode_delete_entry_binary(path);
    if (entry != NULL) {
        thread_wal_t* twal = get_thread_wal(db->wal_manager);
        if (twal != NULL) {
            int result = thread_wal_write(twal, txn->txn_id, WAL_DELETE, entry);
            if (result != 0) {
                log_warn("Failed to write to thread-local WAL");
            }
        }
        buffer_destroy(entry);
    }

    // Acquire sharded write lock (prevents root-node contention)
    size_t shard = get_write_lock_shard(path);
    spinlock_lock(&db->write_locks[shard]);

    // Remove from trie with MVCC (creates tombstone)
    identifier_t* removed = hbtrie_delete(db->trie, path, txn->txn_id);

    // Release write lock
    spinlock_unlock(&db->write_locks[shard]);

    // Commit transaction
    tx_manager_commit(db->tx_manager, txn);
    txn_desc_destroy(txn);

    // Remove from LRU cache
    database_lru_cache_delete(db->lru, path);

    path_destroy(path);

    if (removed) {
        identifier_destroy(removed);
    }

    return 0;
}

int64_t database_increment_sync(database_t* db, path_t* path, int64_t delta) {
    if (db == NULL || path == NULL) return -1;

    // Fast path: sync-only mode — skip MVCC, locks, and write_locks
    if (db->sync_only) {
        transaction_id_t txn_id = transaction_id_get_next();
        identifier_t* current = hbtrie_find_unsafe(db->trie, path);
        int64_t old_val = 0;
        if (current != NULL) {
            buffer_t* buf = identifier_to_buffer(current);
            if (buf != NULL && buf->size > 0) {
                char* tmp = malloc(buf->size + 1);
                if (tmp != NULL) {
                    memcpy(tmp, buf->data, buf->size);
                    tmp[buf->size] = '\0';
                    old_val = strtoll(tmp, NULL, 10);
                    free(tmp);
                }
                buffer_destroy(buf);
            }
            identifier_destroy(current);
        }

        int64_t new_val = old_val + delta;

        char val_str[32];
        snprintf(val_str, sizeof(val_str), "%lld", (long long)new_val);

        path_t* ins_path = path_copy(path);
        buffer_t* val_buf = buffer_create(strlen(val_str));
        if (val_buf == NULL || ins_path == NULL) {
            if (val_buf) buffer_destroy(val_buf);
            if (ins_path) path_destroy(ins_path);
            return -1;
        }
        memcpy(val_buf->data, val_str, strlen(val_str));
        val_buf->size = strlen(val_str);
        identifier_t* new_id = identifier_create(val_buf, 0);
        buffer_destroy(val_buf);
        if (new_id == NULL) {
            path_destroy(ins_path);
            return -1;
        }

        // Write to WAL
        buffer_t* entry = encode_put_entry_binary(ins_path, new_id);
        if (entry != NULL) {
            thread_wal_t* twal = get_thread_wal(db->wal_manager);
            if (twal != NULL) {
                thread_wal_write(twal, txn_id, WAL_PUT, entry);
            }
            buffer_destroy(entry);
        }

        hbtrie_insert_unsafe(db->trie, ins_path, new_id, txn_id);

        path_t* cache_path = path_copy(ins_path);
        identifier_t* cache_val = REFERENCE(new_id, identifier_t);
        identifier_t* ejected = database_lru_cache_put_unsafe(db->lru, cache_path, cache_val);
        if (ejected) identifier_destroy(ejected);

        path_destroy(ins_path);
        identifier_destroy(new_id);

        return new_val;
    }

    // Read current value (lock-free via MVCC — no shard lock needed)
    transaction_id_t read_txn_id = tx_manager_get_last_committed(db->tx_manager);
    identifier_t* current = hbtrie_find(db->trie, path, read_txn_id);
    int64_t old_val = 0;
    if (current != NULL) {
        buffer_t* buf = identifier_to_buffer(current);
        if (buf != NULL && buf->size > 0) {
            // Null-terminate for strtoll
            char* tmp = malloc(buf->size + 1);
            if (tmp != NULL) {
                memcpy(tmp, buf->data, buf->size);
                tmp[buf->size] = '\0';
                old_val = strtoll(tmp, NULL, 10);
                free(tmp);
            }
            buffer_destroy(buf);
        }
        identifier_destroy(current);
    }

    int64_t new_val = old_val + delta;

    // Prepare new value (outside shard lock — no blocking under spinlock)
    char val_str[32];
    snprintf(val_str, sizeof(val_str), "%lld", (long long)new_val);

    path_t* ins_path = path_copy(path);
    buffer_t* val_buf = buffer_create(strlen(val_str));
    if (val_buf == NULL || ins_path == NULL) {
        if (val_buf) buffer_destroy(val_buf);
        if (ins_path) path_destroy(ins_path);
        return -1;
    }
    memcpy(val_buf->data, val_str, strlen(val_str));
    val_buf->size = strlen(val_str);
    identifier_t* new_id = identifier_create(val_buf, 0);
    buffer_destroy(val_buf);
    if (new_id == NULL) {
        path_destroy(ins_path);
        return -1;
    }

    // Begin transaction (outside shard lock)
    txn_desc_t* txn = tx_manager_begin(db->tx_manager);
    if (txn == NULL) {
        path_destroy(ins_path);
        identifier_destroy(new_id);
        return -1;
    }

    // Acquire shard lock only for the trie mutation
    size_t shard = get_write_lock_shard(path);
    spinlock_lock(&db->write_locks[shard]);

    // Insert into trie
    hbtrie_insert(db->trie, ins_path, new_id, txn->txn_id);

    // Update LRU cache
    path_t* cache_path = path_copy(ins_path);
    identifier_t* cache_val = REFERENCE(new_id, identifier_t);
    identifier_t* ejected = database_lru_cache_put(db->lru, cache_path, cache_val);
    if (ejected) identifier_destroy(ejected);

    // Release shard lock
    spinlock_unlock(&db->write_locks[shard]);

    // Commit and cleanup (outside shard lock)
    tx_manager_commit(db->tx_manager, txn);
    txn_desc_destroy(txn);
    path_destroy(ins_path);
    identifier_destroy(new_id);

    return new_val;
}

database_iterator_t* database_scan_range(database_t* db,
                                          const char* start,
                                          const char* end) {
    if (db == NULL) return NULL;

    path_t* start_path = NULL;
    path_t* end_path = NULL;

    // Build start path from string
    if (start != NULL) {
        start_path = path_create();
        if (start_path == NULL) return NULL;
        const char* s = start;
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
                        path_append(start_path, id);
                        identifier_destroy(id);
                    }
                }
            }
            if (e) { s = e + 1; } else { break; }
        }
    }

    // Build end path from string
    if (end != NULL) {
        end_path = path_create();
        if (end_path == NULL) {
            if (start_path) path_destroy(start_path);
            return NULL;
        }
        const char* s = end;
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
                        path_append(end_path, id);
                        identifier_destroy(id);
                    }
                }
            }
            if (e) { s = e + 1; } else { break; }
        }
    }

    database_iterator_t* iter = database_scan_start(db, start_path, end_path);
    // database_scan_start copies the paths, so free our local copies
    if (start_path) path_destroy(start_path);
    if (end_path) path_destroy(end_path);
    return iter;
}

int database_write_batch_sync(database_t* db, batch_t* batch) {
    // Validate inputs
    if (db == NULL || batch == NULL) {
        return -1;
    }

    // Check if batch is empty
    platform_lock(&batch->lock);
    if (batch->count == 0) {
        platform_unlock(&batch->lock);
        return -3;
    }

    // Check if already submitted
    if (batch->submitted) {
        platform_unlock(&batch->lock);
        return -6;
    }

    platform_unlock(&batch->lock);

    // Fast path: sync-only mode — skip MVCC, locks, and write_locks
    if (db->sync_only) {
        transaction_id_t txn_id = transaction_id_get_next();

        platform_lock(&batch->lock);
        batch->submitted = 1;
        platform_unlock(&batch->lock);

        // Serialize and write to WAL
        buffer_t* data = serialize_batch(batch);
        if (data != NULL) {
            thread_wal_t* twal = get_thread_wal(db->wal_manager);
            if (twal != NULL) {
                thread_wal_write(twal, txn_id, WAL_BATCH, data);
            }
            buffer_destroy(data);
        }

        // Apply to trie without per-node locks
        for (size_t i = 0; i < batch->count; i++) {
            if (batch->ops[i].type == WAL_PUT) {
                hbtrie_insert_unsafe(db->trie, batch->ops[i].path, batch->ops[i].value, txn_id);
                database_lru_cache_delete_unsafe(db->lru, batch->ops[i].path);
            } else {
                identifier_t* removed = hbtrie_delete_unsafe(db->trie, batch->ops[i].path, txn_id);
                if (removed) identifier_destroy(removed);
                database_lru_cache_delete_unsafe(db->lru, batch->ops[i].path);
            }
        }

        return 0;
    }

    // Check size against WAL max size
    if (db->wal_manager != NULL) {
        size_t size = batch_estimate_size(batch);
        if (size > db->wal_manager->config.max_file_size) {
            return -5;
        }
    }

    // Begin MVCC transaction
    txn_desc_t* txn = tx_manager_begin(db->tx_manager);
    if (txn == NULL) {
        return -1;
    }

    // Mark batch as submitted
    platform_lock(&batch->lock);
    batch->submitted = 1;
    platform_unlock(&batch->lock);

    // Serialize batch
    buffer_t* data = serialize_batch(batch);
    if (data == NULL) {
        txn_desc_destroy(txn);
        return -1;
    }

    // Write to thread-local WAL. Skip if there's no WAL manager (e.g.
    // in_memory mode where location=NULL) — mirrors the sync_only fast path
    // above. The trie apply below still happens, so the batch is visible
    // in-memory for the lifetime of the database.
    thread_wal_t* twal = get_thread_wal(db->wal_manager);
    if (twal != NULL) {
        int result = thread_wal_write(twal, txn->txn_id, WAL_BATCH, data);
        buffer_destroy(data);
        if (result != 0) {
            txn_desc_destroy(txn);
            return result;
        }
    } else {
        buffer_destroy(data);
    }

    // Apply to trie with MVCC (acquire per-key shard lock per operation)
    platform_lock(&batch->lock);
    for (size_t i = 0; i < batch->count; i++) {
        int op_result;
        if (batch->ops[i].type == WAL_PUT) {
            size_t shard = get_write_lock_shard(batch->ops[i].path);
            spinlock_lock(&db->write_locks[shard]);
            op_result = hbtrie_insert(db->trie, batch->ops[i].path, batch->ops[i].value, txn->txn_id);
            spinlock_unlock(&db->write_locks[shard]);
            // Invalidate LRU cache so subsequent reads see the new value
            database_lru_cache_delete(db->lru, batch->ops[i].path);
        } else {
            size_t shard = get_write_lock_shard(batch->ops[i].path);
            spinlock_lock(&db->write_locks[shard]);
            identifier_t* removed = hbtrie_delete(db->trie, batch->ops[i].path, txn->txn_id);
            spinlock_unlock(&db->write_locks[shard]);
            op_result = 0; // Delete always succeeds
            if (removed) {
                identifier_destroy(removed);
            }
            // Invalidate LRU cache for deleted keys
            database_lru_cache_delete(db->lru, batch->ops[i].path);
        }

        if (op_result != 0) {
            platform_unlock(&batch->lock);
            log_error("Batch apply failed at operation %zu", i);
            txn_desc_destroy(txn);
            return -1;
        }
    }
    platform_unlock(&batch->lock);

    // Commit transaction
    tx_manager_commit(db->tx_manager, txn);
    txn_desc_destroy(txn);

    return 0;
}

static void batch_execute_work(void* ctx) {
    batch_work_t* work = (batch_work_t*)ctx;

    int result = database_write_batch_sync(work->db, work->batch);

    // Drop the reference taken by database_write_batch
    refcounter_dereference((refcounter_t*)work->batch);

    // Allocate result on heap for promise handoff
    int* result_ptr = malloc(sizeof(int));
    if (result_ptr) {
        *result_ptr = result;
        promise_resolve(work->promise, result_ptr);
    } else {
        promise_resolve(work->promise, NULL);
    }

    free(work);
}

/* --- Raw API implementations --- */

int database_put_sync_raw(database_t* db,
    const char* key, size_t key_len, char delimiter,
    const uint8_t* value, size_t value_len) {
    if (!db || !key || key_len == 0 || !value) return -1;

    path_t* path = path_create_from_raw(key, key_len, delimiter, db->chunk_size);
    if (!path) return -1;

    identifier_t* id = identifier_create_from_raw(value, value_len, db->chunk_size);
    if (!id) { path_destroy(path); return -1; }

    return database_put_sync(db, path, id);
}

int database_get_sync_raw(database_t* db,
    const char* key, size_t key_len, char delimiter,
    uint8_t** value_out, size_t* value_len_out) {
    if (!db || !key || key_len == 0 || !value_out || !value_len_out) return -1;

    path_t* path = path_create_from_raw(key, key_len, delimiter, db->chunk_size);
    if (!path) return -1;

    identifier_t* result = NULL;
    int rc = database_get_sync(db, path, &result);

    if (rc == 0 && result) {
        *value_out = identifier_get_data_copy(result, value_len_out);
        identifier_destroy(result);
        // identifier_get_data_copy returns NULL for a valid empty value
        // (length 0, with *value_len_out set to 0) — that is success, not a
        // copy failure. Only treat a NULL pointer as failure when a non-empty
        // value failed to allocate. Conflating the two made get_sync raise on
        // every empty-valued key (e.g. graph index SPO/POS/OSP entries).
        if (!*value_out && *value_len_out > 0) return -1;
        return 0;
    }

    *value_out = NULL;
    *value_len_out = 0;
    return rc;
}

int database_delete_sync_raw(database_t* db,
    const char* key, size_t key_len, char delimiter) {
    if (!db || !key || key_len == 0) return -1;

    path_t* path = path_create_from_raw(key, key_len, delimiter, db->chunk_size);
    if (!path) return -1;

    return database_delete_sync(db, path);
}

void database_raw_value_free(uint8_t* value) {
    free(value);
}

/* --- Raw async context and workers --- */

typedef struct {
    database_t* db;
    char* key_buf;
    size_t key_len;
    char delimiter;
    uint8_t* value_buf;
    size_t value_len;
    promise_t* promise;
    int op_type;   /* 0=put, 1=get, 2=delete */
} raw_async_ctx_t;

static void _raw_put_worker(void* ctx_ptr) {
    raw_async_ctx_t* ctx = (raw_async_ctx_t*)ctx_ptr;
    path_t* path = path_create_from_raw(ctx->key_buf, ctx->key_len,
                                         ctx->delimiter,
                                         ctx->db->chunk_size);
    if (!path) {
        async_error_t* err = ERROR("Failed to create path from raw key");
        promise_reject(ctx->promise, err);
        free(ctx->key_buf);
        free(ctx->value_buf);
        free(ctx);
        return;
    }

    identifier_t* value = identifier_create_from_raw(
        ctx->value_buf, ctx->value_len, ctx->db->chunk_size);
    if (!value) {
        path_destroy(path);
        async_error_t* err = ERROR("Failed to create value from raw data");
        promise_reject(ctx->promise, err);
        free(ctx->key_buf);
        free(ctx->value_buf);
        free(ctx);
        return;
    }

    free(ctx->key_buf);
    free(ctx->value_buf);

    database_put_ctx_t* put_ctx = get_clear_memory(sizeof(database_put_ctx_t));
    put_ctx->db = ctx->db;
    put_ctx->path = path;
    put_ctx->value = value;
    put_ctx->promise = ctx->promise;
    free(ctx);

    _database_put(put_ctx);
}

static void abort_raw_put(void* ctx_ptr) {
    raw_async_ctx_t* ctx = (raw_async_ctx_t*)ctx_ptr;
    free(ctx->key_buf);
    free(ctx->value_buf);
    free(ctx);
}

static void _raw_get_worker(void* ctx_ptr) {
    raw_async_ctx_t* ctx = (raw_async_ctx_t*)ctx_ptr;
    path_t* path = path_create_from_raw(ctx->key_buf, ctx->key_len,
                                         ctx->delimiter,
                                         ctx->db->chunk_size);
    free(ctx->key_buf);

    if (!path) {
        async_error_t* err = ERROR("Failed to create path from raw key");
        promise_reject(ctx->promise, err);
        free(ctx);
        return;
    }

    database_get_ctx_t* get_ctx = get_clear_memory(sizeof(database_get_ctx_t));
    get_ctx->db = ctx->db;
    get_ctx->path = path;
    get_ctx->promise = ctx->promise;
    free(ctx);

    _database_get(get_ctx);
}

static void abort_raw_get(void* ctx_ptr) {
    raw_async_ctx_t* ctx = (raw_async_ctx_t*)ctx_ptr;
    free(ctx->key_buf);
    free(ctx);
}

static void _raw_delete_worker(void* ctx_ptr) {
    raw_async_ctx_t* ctx = (raw_async_ctx_t*)ctx_ptr;
    path_t* path = path_create_from_raw(ctx->key_buf, ctx->key_len,
                                         ctx->delimiter,
                                         ctx->db->chunk_size);
    free(ctx->key_buf);

    if (!path) {
        async_error_t* err = ERROR("Failed to create path from raw key");
        promise_reject(ctx->promise, err);
        free(ctx);
        return;
    }

    database_delete_ctx_t* del_ctx = get_clear_memory(sizeof(database_delete_ctx_t));
    del_ctx->db = ctx->db;
    del_ctx->path = path;
    del_ctx->promise = ctx->promise;
    free(ctx);

    _database_delete(del_ctx);
}

static void abort_raw_delete(void* ctx_ptr) {
    raw_async_ctx_t* ctx = (raw_async_ctx_t*)ctx_ptr;
    free(ctx->key_buf);
    free(ctx);
}

int database_put_raw(database_t* db,
    const char* key, size_t key_len, char delimiter,
    const uint8_t* value, size_t value_len,
    promise_t* promise) {
    if (!db || !key || key_len == 0 || !value || !promise) {
        if (promise) promise_resolve(promise, NULL);
        return -1;
    }

    // Async operations require a worker pool — not available in sync_only mode
    if (db->sync_only) {
        int* error = malloc(sizeof(int));
        if (error) *error = -1;
        promise_resolve(promise, error);
        return -1;
    }

    raw_async_ctx_t* ctx = calloc(1, sizeof(raw_async_ctx_t));
    if (!ctx) { promise_resolve(promise, NULL); return -1; }

    ctx->db = db;
    ctx->key_len = key_len;
    ctx->key_buf = malloc(key_len);
    if (!ctx->key_buf) { free(ctx); promise_resolve(promise, NULL); return -1; }
    memcpy(ctx->key_buf, key, key_len);

    ctx->delimiter = delimiter;
    ctx->value_len = value_len;
    ctx->value_buf = malloc(value_len);
    if (!ctx->value_buf) { free(ctx->key_buf); free(ctx); promise_resolve(promise, NULL); return -1; }
    memcpy(ctx->value_buf, value, value_len);

    ctx->promise = promise;
    ctx->op_type = 0;

    work_t* work = work_create(_raw_put_worker, abort_raw_put, ctx);
    if (!work) {
        free(ctx->value_buf);
        free(ctx->key_buf);
        free(ctx);
        promise_resolve(promise, NULL);
        return -1;
    }

    refcounter_yield((refcounter_t*)work);
    work_pool_enqueue(db->pool, work);
    return 0;
}

int database_get_raw(database_t* db,
    const char* key, size_t key_len, char delimiter,
    promise_t* promise) {
    if (!db || !key || key_len == 0 || !promise) {
        if (promise) promise_resolve(promise, NULL);
        return -1;
    }

    // Async operations require a worker pool — not available in sync_only mode
    if (db->sync_only) {
        promise_resolve(promise, NULL);
        return -1;
    }

    raw_async_ctx_t* ctx = calloc(1, sizeof(raw_async_ctx_t));
    if (!ctx) { promise_resolve(promise, NULL); return -1; }

    ctx->db = db;
    ctx->key_len = key_len;
    ctx->key_buf = malloc(key_len);
    if (!ctx->key_buf) { free(ctx); promise_resolve(promise, NULL); return -1; }
    memcpy(ctx->key_buf, key, key_len);

    ctx->delimiter = delimiter;
    ctx->promise = promise;
    ctx->op_type = 1;

    work_t* work = work_create(_raw_get_worker, abort_raw_get, ctx);
    if (!work) {
        free(ctx->key_buf);
        free(ctx);
        promise_resolve(promise, NULL);
        return -1;
    }

    refcounter_yield((refcounter_t*)work);
    work_pool_enqueue(db->pool, work);
    return 0;
}

int database_delete_raw(database_t* db,
    const char* key, size_t key_len, char delimiter,
    promise_t* promise) {
    if (!db || !key || key_len == 0 || !promise) {
        if (promise) promise_resolve(promise, NULL);
        return -1;
    }

    // Async operations require a worker pool — not available in sync_only mode
    if (db->sync_only) {
        int* error = malloc(sizeof(int));
        if (error) *error = -1;
        promise_resolve(promise, error);
        return -1;
    }

    raw_async_ctx_t* ctx = calloc(1, sizeof(raw_async_ctx_t));
    if (!ctx) { promise_resolve(promise, NULL); return -1; }

    ctx->db = db;
    ctx->key_len = key_len;
    ctx->key_buf = malloc(key_len);
    if (!ctx->key_buf) { free(ctx); promise_resolve(promise, NULL); return -1; }
    memcpy(ctx->key_buf, key, key_len);

    ctx->delimiter = delimiter;
    ctx->promise = promise;
    ctx->op_type = 2;

    work_t* work = work_create(_raw_delete_worker, abort_raw_delete, ctx);
    if (!work) {
        free(ctx->key_buf);
        free(ctx);
        promise_resolve(promise, NULL);
        return -1;
    }

    refcounter_yield((refcounter_t*)work);
    work_pool_enqueue(db->pool, work);
    return 0;
}

/* --- Batch raw implementations --- */

int database_batch_sync_raw(database_t* db, char delimiter,
    const raw_op_t* ops, size_t count) {
    if (!db || !ops || count == 0) return -1;

    batch_t* batch = batch_create(count);
    if (!batch) return -1;

    for (size_t i = 0; i < count; i++) {
        path_t* path = path_create_from_raw(ops[i].key, ops[i].key_len,
                                             delimiter,
                                             db->chunk_size);
        if (!path) { batch_destroy(batch); return -1; }

        if (ops[i].type == 0) {
            if (!ops[i].value) {
                path_destroy(path);
                batch_destroy(batch);
                return -1;
            }
            identifier_t* value = identifier_create_from_raw(
                ops[i].value, ops[i].value_len,
                db->chunk_size);
            if (!value) { path_destroy(path); batch_destroy(batch); return -1; }

            int rc = batch_add_put(batch, path, value);
            if (rc != 0) {
                path_destroy(path);
                identifier_destroy(value);
                batch_destroy(batch);
                return -1;
            }
        } else {
            int rc = batch_add_delete(batch, path);
            if (rc != 0) {
                path_destroy(path);
                batch_destroy(batch);
                return -1;
            }
        }
    }

    int rc = database_write_batch_sync(db, batch);
    batch_destroy(batch);
    return rc;
}

int database_batch_raw(database_t* db, char delimiter,
    const raw_op_t* ops, size_t count,
    promise_t* promise) {
    if (!db || !ops || count == 0 || !promise) return -1;

    batch_t* batch = batch_create(count);
    if (!batch) return -1;

    for (size_t i = 0; i < count; i++) {
        path_t* path = path_create_from_raw(ops[i].key, ops[i].key_len,
                                             delimiter,
                                             db->chunk_size);
        if (!path) { batch_destroy(batch); return -1; }

        if (ops[i].type == 0) {
            if (!ops[i].value) {
                path_destroy(path);
                batch_destroy(batch);
                return -1;
            }
            identifier_t* value = identifier_create_from_raw(
                ops[i].value, ops[i].value_len,
                db->chunk_size);
            if (!value) { path_destroy(path); batch_destroy(batch); return -1; }

            int rc = batch_add_put(batch, path, value);
            if (rc != 0) {
                path_destroy(path);
                identifier_destroy(value);
                batch_destroy(batch);
                return -1;
            }
        } else {
            int rc = batch_add_delete(batch, path);
            if (rc != 0) {
                path_destroy(path);
                batch_destroy(batch);
                return -1;
            }
        }
    }

    database_write_batch(db, batch, promise);
    return 0;
}

void database_write_batch(database_t* db, batch_t* batch, promise_t* promise) {
    if (db == NULL || batch == NULL || promise == NULL) {
        int* error = malloc(sizeof(int));
        if (error) {
            *error = -1;
            promise_resolve(promise, error);
        } else {
            promise_resolve(promise, NULL);
        }
        return;
    }

    // Async batch requires a worker pool — not available in sync_only mode
    if (db->sync_only) {
        int* error = malloc(sizeof(int));
        if (error) *error = -1;
        promise_resolve(promise, error);
        return;
    }

    batch_work_t* work = malloc(sizeof(batch_work_t));
    if (work == NULL) {
        int* error = malloc(sizeof(int));
        if (error) {
            *error = -1;
            promise_resolve(promise, error);
        } else {
            promise_resolve(promise, NULL);
        }
        return;
    }

    work->db = db;
    work->batch = batch;
    work->promise = promise;

    refcounter_reference((refcounter_t*)batch);
    work_t* task = work_create(batch_execute_work, NULL, work);
    refcounter_yield((refcounter_t*) task);
    work_pool_enqueue(db->pool, task);
}

/* --- Scan raw implementations --- */

int database_scan_sync_raw(database_t* db,
    const char* prefix, size_t prefix_len, char delimiter,
    raw_result_t** results, size_t* count) {
    if (!db || !results || !count) return -1;

    *results = NULL;
    *count = 0;

    path_t* start_path = NULL;
    if (prefix && prefix_len > 0) {
        start_path = path_create_from_raw(prefix, prefix_len, delimiter,
                                           db->chunk_size);
        if (!start_path) return -1;
    }

    database_iterator_t* iter = database_scan_start(db, start_path, NULL);
    if (start_path) path_destroy(start_path);
    if (!iter) return 0;

    size_t capacity = 64;
    size_t n = 0;
    raw_result_t* out = malloc(capacity * sizeof(raw_result_t));
    if (!out) { database_scan_end(iter); return -1; }

    while (true) {
        path_t* out_path = NULL;
        identifier_t* out_value = NULL;
        int rc = database_scan_next(iter, &out_path, &out_value);
        if (rc != 0) break;

        if (n >= capacity) {
            capacity *= 2;
            raw_result_t* new_out = realloc(out, capacity * sizeof(raw_result_t));
            if (!new_out) {
                for (size_t j = 0; j < n; j++) {
                    free(out[j].key);
                    free(out[j].value);
                }
                free(out);
                path_destroy(out_path);
                identifier_destroy(out_value);
                database_scan_end(iter);
                return -1;
            }
            out = new_out;
        }

        // Assemble key string from path identifiers
        size_t key_total = 0;
        size_t path_len = path_length(out_path);
        for (size_t i = 0; i < path_len; i++) {
            identifier_t* id = path_get(out_path, i);
            key_total += id->length;
            if (i > 0) key_total++;  // delimiter
        }

        out[n].key = malloc(key_total + 1);
        if (!out[n].key) {
            for (size_t j = 0; j < n; j++) {
                free(out[j].key);
                free(out[j].value);
            }
            free(out);
            path_destroy(out_path);
            identifier_destroy(out_value);
            database_scan_end(iter);
            return -1;
        }

        size_t pos = 0;
        for (size_t i = 0; i < path_len; i++) {
            identifier_t* id = path_get(out_path, i);
            if (i > 0) out[n].key[pos++] = delimiter;
            size_t id_len;
            uint8_t* id_data = identifier_get_data_copy(id, &id_len);
            if (id_data) {
                memcpy(out[n].key + pos, id_data, id_len);
                pos += id_len;
                free(id_data);
            }
        }
        out[n].key[pos] = '\0';
        out[n].key_len = pos;

        // Copy value
        out[n].value = identifier_get_data_copy(out_value, &out[n].value_len);

        path_destroy(out_path);
        identifier_destroy(out_value);
        n++;
    }

    database_scan_end(iter);
    *results = out;
    *count = n;
    return 0;
}

int database_scan_range_sync_raw(database_t* db,
    const char* start_prefix, size_t start_prefix_len,
    const char* end_prefix, size_t end_prefix_len,
    char delimiter,
    raw_result_t** results, size_t* count) {
    if (!db || !results || !count) return -1;

    *results = NULL;
    *count = 0;

    path_t* start_path = NULL;
    if (start_prefix && start_prefix_len > 0) {
        start_path = path_create_from_raw(start_prefix, start_prefix_len,
                                           delimiter, db->chunk_size);
        if (!start_path) return -1;
    }

    path_t* end_path = NULL;
    if (end_prefix && end_prefix_len > 0) {
        end_path = path_create_from_raw(end_prefix, end_prefix_len,
                                         delimiter, db->chunk_size);
        if (!end_path) {
            if (start_path) path_destroy(start_path);
            return -1;
        }
    }

    database_iterator_t* iter = database_scan_start(db, start_path, end_path);
    if (start_path) path_destroy(start_path);
    if (end_path) path_destroy(end_path);
    if (!iter) return 0;

    size_t capacity = 64;
    size_t n = 0;
    raw_result_t* out = malloc(capacity * sizeof(raw_result_t));
    if (!out) { database_scan_end(iter); return -1; }

    while (true) {
        path_t* out_path = NULL;
        identifier_t* out_value = NULL;
        int rc = database_scan_next(iter, &out_path, &out_value);
        if (rc != 0) break;

        if (n >= capacity) {
            capacity *= 2;
            raw_result_t* new_out = realloc(out, capacity * sizeof(raw_result_t));
            if (!new_out) {
                for (size_t j = 0; j < n; j++) {
                    free(out[j].key);
                    free(out[j].value);
                }
                free(out);
                path_destroy(out_path);
                identifier_destroy(out_value);
                database_scan_end(iter);
                return -1;
            }
            out = new_out;
        }

        size_t key_total = 0;
        size_t path_len = path_length(out_path);
        for (size_t i = 0; i < path_len; i++) {
            identifier_t* id = path_get(out_path, i);
            key_total += id->length;
            if (i > 0) key_total++;
        }

        out[n].key = malloc(key_total + 1);
        if (!out[n].key) {
            for (size_t j = 0; j < n; j++) {
                free(out[j].key);
                free(out[j].value);
            }
            free(out);
            path_destroy(out_path);
            identifier_destroy(out_value);
            database_scan_end(iter);
            return -1;
        }

        size_t pos = 0;
        for (size_t i = 0; i < path_len; i++) {
            identifier_t* id = path_get(out_path, i);
            if (i > 0) out[n].key[pos++] = delimiter;
            size_t id_len;
            uint8_t* id_data = identifier_get_data_copy(id, &id_len);
            if (id_data) {
                memcpy(out[n].key + pos, id_data, id_len);
                pos += id_len;
                free(id_data);
            }
        }
        out[n].key[pos] = '\0';
        out[n].key_len = pos;

        out[n].value = identifier_get_data_copy(out_value, &out[n].value_len);

        path_destroy(out_path);
        identifier_destroy(out_value);
        n++;
    }

    database_scan_end(iter);
    *results = out;
    *count = n;
    return 0;
}

void database_raw_results_free(raw_result_t* results, size_t count) {
    if (!results) return;
    for (size_t i = 0; i < count; i++) {
        free(results[i].key);
        free(results[i].value);
    }
    free(results);
}