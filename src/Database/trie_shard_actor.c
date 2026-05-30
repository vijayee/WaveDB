#include "trie_shard_actor.h"
#include "../RefCounter/refcounter.h"
#include "../Util/allocator.h"
#include "../Workers/error.h"
#include "../Util/threadding.h"
#include <cbor.h>
#include <stdlib.h>
#include <string.h>

static size_t get_shard_index(trie_shard_actor_t** shards, size_t count, path_t* path) {
    (void)shards;
    uint64_t hash = path_hash(path);
    return (size_t)(hash % count);
}

static void trie_shard_dispatch(void* state, message_t* msg) {
    trie_shard_actor_t* shard = (trie_shard_actor_t*)state;

    switch (msg->type) {
        case SHARD_PUT: {
            db_op_payload_t* p = (db_op_payload_t*)msg->payload;

            // Begin transaction
            txn_desc_t* txn = tx_manager_begin(shard->tx_manager);
            if (txn == NULL) {
                if (p->promise) {
                    async_error_t err = {0};
                    promise_reject(p->promise, &err);
                }
                break;
            }

            // Write to WAL BEFORE insert (write-ahead logging: log first, then apply)
            if (shard->wal) {
                cbor_item_t* wal_arr = cbor_new_definite_array(2);
                if (wal_arr) {
                    cbor_item_t* path_cbor = path_to_cbor(p->path);
                    cbor_item_t* value_cbor = identifier_to_cbor(p->value);
                    if (path_cbor && value_cbor) {
                        cbor_array_push(wal_arr, path_cbor);
                        cbor_decref(&path_cbor);
                        path_cbor = NULL;
                        cbor_array_push(wal_arr, value_cbor);
                        cbor_decref(&value_cbor);
                        value_cbor = NULL;
                        unsigned char* cbor_data = NULL;
                        size_t cbor_size = 0;
                        size_t alen = cbor_serialize_alloc(wal_arr, &cbor_data, &cbor_size);
                        if (alen > 0 && cbor_data) {
                            buffer_t* wal_buf = buffer_create(cbor_size);
                            if (wal_buf) {
                                memcpy(wal_buf->data, cbor_data, cbor_size);
                                wal_buf->size = cbor_size;
                                wal_actor_write(shard->wal, platform_self(),
                                                txn->txn_id, 'p', wal_buf, NULL);
                            }
                            free(cbor_data);
                        }
                    }
                    if (path_cbor) { cbor_decref(&path_cbor); path_cbor = NULL; }
                    if (value_cbor) { cbor_decref(&value_cbor); value_cbor = NULL; }
                    cbor_decref(&wal_arr);
                }
            }

            // Insert into trie (single-threaded, no locks needed)
            int result = hbtrie_insert(shard->trie, p->path, p->value, txn->txn_id);

            if (result != 0) {
                tx_manager_abort(shard->tx_manager, txn);
                if (p->promise) {
                    async_error_t err = {0};
                    promise_reject(p->promise, &err);
                }
                DESTROY(p->path, path);
                DESTROY(p->value, identifier);
                break;
            }

            // Commit transaction
            tx_manager_commit(shard->tx_manager, txn);

            // Update atomic root snapshot for lock-free reads
            ATOMIC_STORE(&shard->root, shard->trie->root);

            // Resolve promise
            if (p->promise) {
                promise_resolve(p->promise, NULL);
            }
            DESTROY(p->path, path);
            DESTROY(p->value, identifier);
            break;
        }
        case SHARD_GET: {
            db_op_payload_t* p = (db_op_payload_t*)msg->payload;

            // Read last committed transaction ID for MVCC snapshot
            transaction_id_t read_txn_id = tx_manager_get_last_committed(shard->tx_manager);

            // Find in trie (single-threaded, no locks needed)
            identifier_t* value = hbtrie_find(shard->trie, p->path, read_txn_id);

            if (p->promise) {
                promise_resolve(p->promise, value);
            }
            break;
        }
        case SHARD_DELETE: {
            db_op_payload_t* p = (db_op_payload_t*)msg->payload;

            // Begin transaction
            txn_desc_t* txn = tx_manager_begin(shard->tx_manager);
            if (txn == NULL) {
                if (p->promise) {
                    async_error_t err = {0};
                    promise_reject(p->promise, &err);
                }
                break;
            }

            // Write to WAL BEFORE delete (write-ahead logging)
            if (shard->wal) {
                cbor_item_t* wal_arr = cbor_new_definite_array(1);
                if (wal_arr) {
                    cbor_item_t* path_cbor = path_to_cbor(p->path);
                    if (path_cbor) {
                        cbor_array_push(wal_arr, path_cbor);
                        cbor_decref(&path_cbor);
                        path_cbor = NULL;
                        unsigned char* cbor_data = NULL;
                        size_t cbor_size = 0;
                        size_t alen = cbor_serialize_alloc(wal_arr, &cbor_data, &cbor_size);
                        if (alen > 0 && cbor_data) {
                            buffer_t* wal_buf = buffer_create(cbor_size);
                            if (wal_buf) {
                                memcpy(wal_buf->data, cbor_data, cbor_size);
                                wal_buf->size = cbor_size;
                                wal_actor_write(shard->wal, platform_self(),
                                                txn->txn_id, 'd', wal_buf, NULL);
                            }
                            free(cbor_data);
                        }
                    }
                    cbor_decref(&wal_arr);
                }
            }

            // Delete from trie (single-threaded, no locks needed)
            identifier_t* deleted = hbtrie_delete(shard->trie, p->path, txn->txn_id);

            if (deleted == NULL) {
                // Not found - still commit the tombstone insertion
                // hbtrie_delete creates a tombstone, so even if "last visible" is NULL
                // the tombstone was added (if the entry existed)
            }

            // Commit transaction
            tx_manager_commit(shard->tx_manager, txn);

            // Update atomic root snapshot for lock-free reads
            ATOMIC_STORE(&shard->root, shard->trie->root);

            // Resolve promise with the deleted value (or NULL if not found)
            if (p->promise) {
                promise_resolve(p->promise, deleted);
            }
            DESTROY(p->path, path);
            break;
        }
        default:
            break;
    }
}

// Payload destructors for actor messages
static void db_op_payload_destroy(void* payload) {
    if (payload == NULL) return;
    db_op_payload_t* p = (db_op_payload_t*)payload;
    if (p->path != NULL) {
        path_destroy(p->path);
    }
    free(p);
}

trie_shard_actor_t** trie_shard_actors_create(size_t count, uint8_t chunk_size,
                                               uint32_t btree_node_size,
                                               tx_manager_t* tx_manager,
                                               wal_actor_t* wal) {
    if (count == 0) return NULL;

    trie_shard_actor_t** shards = get_clear_memory(count * sizeof(trie_shard_actor_t*));
    if (shards == NULL) return NULL;

    for (size_t i = 0; i < count; i++) {
        trie_shard_actor_t* shard = get_clear_memory(sizeof(trie_shard_actor_t));
        if (shard == NULL) {
            // Clean up previously created shards
            for (size_t j = 0; j < i; j++) {
                hbtrie_destroy(shards[j]->trie);
                actor_destroy(&shards[j]->actor);
                free(shards[j]);
            }
            free(shards);
            return NULL;
        }

        shard->trie = hbtrie_create(chunk_size, btree_node_size);
        if (shard->trie == NULL) {
            free(shard);
            for (size_t j = 0; j < i; j++) {
                hbtrie_destroy(shards[j]->trie);
                actor_destroy(&shards[j]->actor);
                free(shards[j]);
            }
            free(shards);
            return NULL;
        }

        shard->tx_manager = tx_manager;
        shard->wal = wal;
        shard->chunk_size = chunk_size == 0 ? 4 : chunk_size;
        shard->btree_node_size = btree_node_size;
        ATOMIC_STORE(&shard->root, shard->trie->root);

        actor_init(&shard->actor, shard, trie_shard_dispatch, NULL);
        shards[i] = shard;
    }

    return shards;
}

void trie_shard_actors_destroy(trie_shard_actor_t** shards, size_t count) {
    if (shards == NULL) return;

    for (size_t i = 0; i < count; i++) {
        if (shards[i] != NULL) {
            hbtrie_destroy(shards[i]->trie);
            actor_destroy(&shards[i]->actor);
            free(shards[i]);
        }
    }
    free(shards);
}

void trie_shard_put(trie_shard_actor_t** shards, size_t count, path_t* path, identifier_t* value, promise_t* promise) {
    if (shards == NULL || path == NULL || value == NULL) {
        if (promise) {
            async_error_t err = {0};
            promise_reject(promise, &err);
        }
        return;
    }

    size_t idx = get_shard_index(shards, count, path);

    db_op_payload_t* p = get_clear_memory(sizeof(db_op_payload_t));
    p->path = path_reference(path);    // Share reference - actor owns it
    p->value = value;                    // Transfer ownership
    p->promise = promise;

    message_t msg = { .type = SHARD_PUT, .payload = p, .payload_destroy = db_op_payload_destroy };
    actor_send(&shards[idx]->actor, &msg);
}

void trie_shard_get(trie_shard_actor_t** shards, size_t count, path_t* path, promise_t* promise) {
    if (shards == NULL || path == NULL) {
        if (promise) {
            async_error_t err = {0};
            promise_reject(promise, &err);
        }
        return;
    }

    size_t idx = get_shard_index(shards, count, path);

    db_op_payload_t* p = get_clear_memory(sizeof(db_op_payload_t));
    p->path = path_reference(path);    // Share reference - actor owns it
    p->value = NULL;                     // Not used for reads
    p->promise = promise;

    message_t msg = { .type = SHARD_GET, .payload = p, .payload_destroy = db_op_payload_destroy };
    actor_send(&shards[idx]->actor, &msg);
}

void trie_shard_delete(trie_shard_actor_t** shards, size_t count, path_t* path, promise_t* promise) {
    if (shards == NULL || path == NULL) {
        if (promise) {
            async_error_t err = {0};
            promise_reject(promise, &err);
        }
        return;
    }

    size_t idx = get_shard_index(shards, count, path);

    db_op_payload_t* p = get_clear_memory(sizeof(db_op_payload_t));
    p->path = path_reference(path);    // Share reference - actor owns it
    p->value = NULL;                     // Not used for deletes
    p->promise = promise;

    message_t msg = { .type = SHARD_DELETE, .payload = p, .payload_destroy = db_op_payload_destroy };
    actor_send(&shards[idx]->actor, &msg);
}

identifier_t* trie_shard_read_sync(trie_shard_actor_t** shards, size_t count,
                                    path_t* path, transaction_id_t txn_id) {
    if (shards == NULL || path == NULL) return NULL;

    size_t idx = get_shard_index(shards, count, path);
    trie_shard_actor_t* shard = shards[idx];

    // Lock-free read: bypasses the actor queue.
    // Reads from the trie directly using the provided transaction ID for MVCC visibility.
    // Safe when called between actor dispatch cycles (no concurrent writes).
    return hbtrie_find(shard->trie, path, txn_id);
}
