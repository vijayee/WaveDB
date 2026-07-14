//
// Database Iterator for HBTrie traversal
//

#include "database_iterator.h"
#include "../HBTrie/chunk.h"
#include "../Buffer/buffer.h"
#include "../Util/allocator.h"
#include <stdlib.h>
#include <string.h>
#include <limits.h>

// Initial stack size
#define INITIAL_STACK_SIZE 16

// Forward declaration — seek_to_end_path falls back to seek_to_rightmost on
// any uncertainty (defined later in this file).
static void seek_to_rightmost(database_iterator_t* iter);

/**
 * Push a frame onto the iterator stack.
 */
static int push_frame(database_iterator_t* iter, hbtrie_node_t* node, size_t path_index) {
    // Grow stack if needed
    if (iter->stack_depth >= iter->stack_size) {
        size_t new_size = iter->stack_size * 2;
        iterator_frame_t* new_stack = realloc(iter->stack,
                                               new_size * sizeof(iterator_frame_t));
        if (new_stack == NULL) {
            return -1;
        }
        iter->stack = new_stack;
        iter->stack_size = new_size;
    }

    // Reference the node
    REFERENCE(node, hbtrie_node_t);

    iter->stack[iter->stack_depth].node = node;
    iter->stack[iter->stack_depth].bnode = NULL;
    iter->stack[iter->stack_depth].entry_index = 0;
    iter->stack[iter->stack_depth].path_index = path_index;
    iter->stack[iter->stack_depth].is_bnode_frame = 0;
    iter->stack[iter->stack_depth].value_pending = 0;
    iter->stack_depth++;

    return 0;
}

/**
 * Push a B+tree internal node frame onto the iterator stack.
 * Used when descending through multi-level B+trees (is_bnode_child=1 entries).
 */
static int push_bnode_frame(database_iterator_t* iter, bnode_t* bnode, size_t path_index) {
    if (iter->stack_depth >= iter->stack_size) {
        size_t new_size = iter->stack_size * 2;
        iterator_frame_t* new_stack = realloc(iter->stack,
                                               new_size * sizeof(iterator_frame_t));
        if (new_stack == NULL) {
            return -1;
        }
        iter->stack = new_stack;
        iter->stack_size = new_size;
    }

    REFERENCE(bnode, bnode_t);

    iter->stack[iter->stack_depth].node = NULL;
    iter->stack[iter->stack_depth].bnode = bnode;
    iter->stack[iter->stack_depth].entry_index = 0;
    iter->stack[iter->stack_depth].path_index = path_index;
    iter->stack[iter->stack_depth].is_bnode_frame = 1;
    iter->stack[iter->stack_depth].value_pending = 0;
    iter->stack_depth++;

    return 0;
}

/**
 * Pop a frame from the iterator stack.
 */
static void pop_frame(database_iterator_t* iter) {
    if (iter->stack_depth > 0) {
        iter->stack_depth--;
        if (iter->stack[iter->stack_depth].is_bnode_frame) {
            if (iter->stack[iter->stack_depth].bnode) {
                DEREFERENCE(iter->stack[iter->stack_depth].bnode);
            }
        } else {
            if (iter->stack[iter->stack_depth].node) {
                DEREFERENCE(iter->stack[iter->stack_depth].node);
            }
        }
    }
}

/**
 * Check if a path is within the scan bounds.
 * Returns:
 *   1  if start_path <= path < end_path (within bounds)
 *   0  if path is before start_path (skip, but keep scanning)
 *  -1  if path >= end_path (past upper bound, stop iteration)
 */
static int within_bounds(database_iterator_t* iter, path_t* path) {
    if (path == NULL) return 0;

    // Check upper bound: if path >= end_path, iteration is done
    if (iter->end_path != NULL) {
        if (path_compare(path, iter->end_path) >= 0) {
            return -1;
        }
    }

    // Check lower bound: if path < start_path, skip this entry
    if (iter->start_path != NULL) {
        if (path_compare(path, iter->start_path) < 0) {
            return 0;
        }
    }

    return 1;
}

/**
 * Check if a path is within the scan bounds for a REVERSE (descending) scan.
 * Same half-open interval [start_path, end_path) as forward, but the STOP / SKIP
 * meanings are swapped because we walk descending:
 *   1  if start_path <= path < end_path (within bounds — emit)
 *  -1  if path < start_path (we've walked past the lower bound — STOP iteration)
 *   0  if path >= end_path (we're at or above the upper bound — SKIP; shouldn't
 *      happen post-seek but a broad seek may land at or above end)
 *
 * Mirror of within_bounds with start/end roles swapped in the return codes.
 */
static int within_bounds_reverse(database_iterator_t* iter, path_t* path) {
    if (path == NULL) return 0;

    // Lower bound: if path < start_path, we've walked past the start — stop.
    if (iter->start_path != NULL) {
        if (path_compare(path, iter->start_path) < 0) {
            return -1;
        }
    }

    // Upper bound: if path >= end_path, skip (we should be below end_path after
    // the seek, but a broad seek may land at or above end).
    if (iter->end_path != NULL) {
        if (path_compare(path, iter->end_path) >= 0) {
            return 0;
        }
    }

    return 1;
}

/**
 * Build an identifier from an array of chunks.
 *
 * Creates an identifier by concatenating chunk data.
 * If byte_length is provided (>0), builds a buffer of exactly that size,
 * copying only the real bytes from each chunk (no null padding). This
 * produces an identifier with the exact original length.
 * If byte_length is 0 or exceeds nchunks*chunk_size, falls back to padded
 * behavior (legacy compatibility).
 */
static identifier_t* build_identifier_from_chunks(chunk_t** chunks, size_t nchunks,
                                                    uint8_t chunk_size, size_t byte_length) {
    if (chunks == NULL || nchunks == 0) {
        return identifier_create_empty(chunk_size);
    }

    // Determine the real data length
    size_t total_padded = nchunks * chunk_size;
    size_t real_length = total_padded;

    // Use byte_length if it's valid (non-zero and within the padded range)
    if (byte_length > 0 && byte_length <= total_padded) {
        real_length = byte_length;
    }

    // Allocate buffer for concatenated data (exact size, no padding)
    buffer_t* buf = buffer_create(real_length);
    if (buf == NULL) {
        return NULL;
    }

    // Copy chunk data, only up to real_length bytes
    size_t offset = 0;
    size_t remaining = real_length;
    for (size_t i = 0; i < nchunks && remaining > 0; i++) {
        if (chunks[i] != NULL) {
            size_t to_copy = (remaining < chunk_size) ? remaining : chunk_size;
            memcpy(buf->data + offset, chunks[i]->data, to_copy);
            offset += to_copy;
            remaining -= to_copy;
        }
    }

    identifier_t* id = identifier_create(buf, chunk_size);
    buffer_destroy(buf);
    return id;
}

/**
 * Position a leaf frame for the seek, given the exact-match entry (or NULL) and
 * the lower_bound index `lb` at that leaf bnode.
 *
 * `leaf_frame_idx` is the stack index of the frame holding the leaf bnode: it is
 * the trie-level frame when the trie level's root bnode is itself a leaf
 * (single-level B+tree), or a pushed bnode frame when the root was internal
 * (multi-level B+tree, descended in seek_to_start_path).
 *
 * Returns:
 *   1  = positioned and done — scan_next emits from leaf_frame_idx.entry_index.
 *        entry_index is set to `lb` (emit the matched value == start, or the
 *        lower-bound subtree whose every key > start) or `lb + 1` (skip a matched
 *        value-leaf that is shorter than start_path because start continues
 *        deeper but has no deeper trie level).
 *   0  = descended one trie level — pushed a trie frame for `entry`'s child and
 *        updated *out_current / *out_trie_frame_idx to it; caller continues the
 *        seek with the next chunk. The leaf frame's entry_index is set to
 *        `lb + 1` so scan_next resumes at the next sibling after that subtree.
 *  -1  = bail — caller pops seek-pushed frames and falls back to walk-from-start.
 *
 * In every case the entries skipped (those before entry_index at this leaf) have
 * chunk < start_path's chunk at this trie level, so their subtrees are entirely
 * < start_path — skipping them never drops a key >= start_path.
 */
static int seek_position_leaf(database_iterator_t* iter, size_t leaf_frame_idx,
                               bnode_entry_t* entry, size_t lb, int is_last,
                               file_bnode_cache_t* fcache, uint8_t chunk_size,
                               uint32_t btree_node_size,
                               hbtrie_node_t** out_current,
                               size_t* out_trie_frame_idx) {
    if (entry != NULL) {
        if (is_last) {
            // start_path ends at this chunk: the matched value (if any) == start
            // and is the first result. scan_next emits it, then descends
            // entry->trie_child for the keys strictly > start that share this
            // prefix.
            iter->stack[leaf_frame_idx].entry_index = lb;
            return 1;
        }
        // start_path continues deeper. The matched entry's value-leaf (if any)
        // is a key whose path is exactly start_path's prefix through this chunk
        // — strictly shorter than start_path, hence < start — so it must be
        // skipped, not emitted. Descend to the next trie level via trie_child
        // (entry has_value) or child (entry has no value), lazy-loading on a
        // reopened database.
        hbtrie_node_t* next = NULL;
        if (entry->has_value) {
            if (entry->trie_child == NULL && entry->child_disk_offset != 0 && fcache != NULL) {
                bnode_entry_lazy_load_trie_child(entry, fcache, chunk_size, btree_node_size);
            }
            next = entry->trie_child;
        } else {
            if (entry->child == NULL && entry->child_disk_offset != 0 && fcache != NULL) {
                bnode_entry_lazy_load_hbtrie_child(entry, fcache, chunk_size, btree_node_size);
            }
            next = entry->child;
        }
        if (next == NULL) {
            // No deeper trie level but start_path continues — start_path is not
            // a stored key. The next key >= start is the matched entry's next
            // sibling (chunk > start's chunk here -> subtree > start).
            iter->stack[leaf_frame_idx].entry_index = lb + 1;
            return 1;
        }
        // Skip the matched value-leaf (< start) and descend: position this leaf
        // frame at lb + 1 so scan_next resumes at the next sibling once the
        // child subtree is exhausted, then push the child trie frame.
        iter->stack[leaf_frame_idx].entry_index = lb + 1;
        if (push_frame(iter, next, iter->stack_depth - 1) < 0) {
            return -1;
        }
        *out_current = next;
        *out_trie_frame_idx = iter->stack_depth - 1;
        return 0;
    }
    // No exact match at this trie level: lb is the first entry whose chunk >
    // start_path's chunk (or count if start's chunk exceeds every entry here).
    // Every entry before lb has chunk < start's chunk -> subtree < start -> safe
    // to skip. Position the leaf frame at lb; scan_next emits the lower-bound
    // entry's subtree (all > start), or, if lb == count, pops back to the parent
    // frame which resumes at its next sibling.
    iter->stack[leaf_frame_idx].entry_index = lb;
    return 1;
}

/**
 * Seek the iterator stack to the first leaf >= start_path.
 *
 * database_scan_next() walks the HBTrie depth-first, in-order, from the leftmost
 * leaf. With a start_path bound it relied on within_bounds() to skip every leaf
 * < start_path AFTER rebuilding its path — so a range scan paid O(keys-before-
 * prefix) before reaching the first in-range leaf (e.g. ~830 ms per late-DB
 * .out("follows") query on a 5k-episode corpus; a 0-result scan at the end of the
 * keyspace walked the entire database).
 *
 * This seek descends the trie chunk-by-chunk following start_path, positioning
 * each stack frame's entry_index so scan_next resumes at the first value-bearing
 * leaf >= start_path without visiting the leaves that precede it. Each frame's
 * entry_index skips only entries whose chunk is < start_path's chunk at that
 * trie level (provably < start), so the seek never drops a key >= start — making
 * it O(log N + results) instead of O(keys-before-prefix).
 *
 * MVCC + refcounting are preserved unchanged: the seek uses the same
 * push_frame() / push_bnode_frame() scan_next uses (each REFERENCEs the node it
 * pushes, so COW cannot free a node mid-scan), and read_txn_id is already
 * captured in database_scan_start(). Visibility is still decided by
 * version_entry_find_visible() in scan_next; the seek only changes HOW leaves
 * are reached, not whether they are visible.
 *
 * Safety: on any uncertainty (NULL child, lazy-load failure, missing trie level)
 * the seek pops every frame it pushed and leaves the root frame at entry_index
 * 0 — the original walk-from-start behavior. A seek bug therefore degrades to
 * correct-but-slow; it cannot return a result set missing keys >= start, since
 * every entry_index it sets skips only entries provably < start_path.
 */
static void seek_to_start_path(database_iterator_t* iter) {
    if (iter == NULL || iter->start_path == NULL || iter->db == NULL) {
        return;
    }
    if (iter->stack_depth == 0) {
        return;
    }
    hbtrie_t* trie = iter->db->trie;
    if (trie == NULL) {
        return;
    }
    uint8_t chunk_size = trie->chunk_size;
    file_bnode_cache_t* fcache = trie->fcache;
    uint32_t btree_node_size = trie->btree_node_size;

    size_t saved_depth = iter->stack_depth;  // only the root frame is present

    size_t path_len_ids = path_length(iter->start_path);
    if (path_len_ids == 0) {
        return;
    }

    hbtrie_node_t* current = iter->stack[0].node;  // root hbtrie_node
    if (current == NULL || current->btree == NULL) {
        return;
    }
    // Stack index of the trie-level frame for `current`: the root frame (0)
    // initially, then the trie frame pushed by the last matched-descend.
    size_t trie_frame_idx = 0;

    for (size_t i = 0; i < path_len_ids; i++) {
        identifier_t* identifier = path_get(iter->start_path, i);
        if (identifier == NULL) {
            goto bail;
        }
        size_t nchunk = identifier_chunk_count(identifier);
        if (nchunk == 0) {
            goto bail;
        }
        for (size_t j = 0; j < nchunk; j++) {
            chunk_t* chunk = identifier_get_chunk(identifier, j);
            if (chunk == NULL) {
                goto bail;
            }
            int is_last = (j == nchunk - 1) && (i == path_len_ids - 1);

            bnode_t* root_bn = current->btree;
            size_t root_index;
            bnode_entry_t* root_entry = bnode_find(root_bn, chunk, &root_index);

            if (atomic_load(&root_bn->level) > 1) {
                // Internal root: descend through the separator whose child holds
                // `chunk`. Position the trie-level frame at that separator so
                // scan_next (after this subtree is exhausted) resumes at the
                // next root separator, then push bnode frames for the internal
                // bnodes below the root down to the leaf.
                size_t root_descend;
                if (root_entry != NULL && root_entry->is_bnode_child) {
                    root_descend = root_index;
                } else {
                    root_descend = (root_index == 0) ? 0 : (root_index - 1);
                }
                // +1: skip the separator we descend through below, so when
                // scan_next pops back to this trie frame after exhausting the
                // seek-pushed child frames it resumes at the NEXT separator
                // instead of re-descending into the subtree we already
                // positioned+emitted (which would duplicate every key in it).
                // Mirrors the leaf-root branch's lb+1 skip-on-descend.
                iter->stack[trie_frame_idx].entry_index = root_descend + 1;
                bnode_entry_t* sep = bnode_get(root_bn, root_descend);
                if (sep == NULL || !sep->is_bnode_child) {
                    goto bail;
                }
                if (sep->child_bnode == NULL && sep->child_disk_offset != 0 && fcache != NULL) {
                    bnode_entry_lazy_load_bnode_child(sep, fcache, chunk_size, btree_node_size);
                }
                if (sep->child_bnode == NULL) {
                    goto bail;
                }
                bnode_t* cur_bn = sep->child_bnode;
                while (atomic_load(&cur_bn->level) > 1) {
                    size_t idx;
                    bnode_entry_t* e = bnode_find(cur_bn, chunk, &idx);
                    size_t k;
                    if (e != NULL && e->is_bnode_child) {
                        k = idx;
                    } else {
                        k = (idx == 0) ? 0 : (idx - 1);
                    }
                    bnode_entry_t* s = bnode_get(cur_bn, k);
                    if (s == NULL || !s->is_bnode_child) {
                        goto bail;
                    }
                    if (s->child_bnode == NULL && s->child_disk_offset != 0 && fcache != NULL) {
                        bnode_entry_lazy_load_bnode_child(s, fcache, chunk_size, btree_node_size);
                    }
                    if (s->child_bnode == NULL) {
                        goto bail;
                    }
                    if (push_bnode_frame(iter, cur_bn, iter->stack_depth - 1) < 0) {
                        goto bail;
                    }
                    iter->stack[iter->stack_depth - 1].entry_index = k + 1;
                    cur_bn = s->child_bnode;
                }
                // cur_bn is the leaf bnode for this trie level. Find the trie
                // entry for `chunk` and push a leaf bnode frame for it.
                size_t lb;
                bnode_entry_t* entry = bnode_find(cur_bn, chunk, &lb);
                if (push_bnode_frame(iter, cur_bn, iter->stack_depth - 1) < 0) {
                    goto bail;
                }
                size_t leaf_idx = iter->stack_depth - 1;
                int r = seek_position_leaf(iter, leaf_idx, entry, lb, is_last,
                                           fcache, chunk_size, btree_node_size,
                                           &current, &trie_frame_idx);
                if (r < 0) {
                    goto bail;
                }
                if (r == 1) {
                    return;  // positioned — scan_next emits from leaf_idx
                }
                // r == 0: descended one trie level; continue with next chunk
            } else {
                // Leaf root (single-level B+tree): the trie-level frame IS the
                // leaf frame. Apply the seek decision directly to it.
                int r = seek_position_leaf(iter, trie_frame_idx, root_entry,
                                           root_index, is_last, fcache,
                                           chunk_size, btree_node_size,
                                           &current, &trie_frame_idx);
                if (r < 0) {
                    goto bail;
                }
                if (r == 1) {
                    return;  // positioned
                }
                // r == 0: descended one trie level; continue
            }
        }
    }
    // Every chunk of start_path matched exactly with a child trie level. The
    // deepest trie frame sits at entry_index 0; its leftmost leaf shares all of
    // start_path as a prefix, so every key under it is >= start_path. Let
    // scan_next emit from the leftmost.
    return;

bail:
    // Restore the original walk-from-start state: pop every frame the seek
    // pushed and reset the root frame to the leftmost entry.
    while (iter->stack_depth > saved_depth) {
        pop_frame(iter);
    }
    if (iter->stack_depth > 0) {
        iter->stack[0].entry_index = 0;
    }
}

/**
 * Seek the iterator stack to the rightmost leaf whose key < end_path, for
 * reverse scans. Mirror of seek_to_start_path with upper-bound + rightmost
 * descent.
 *
 * For each chunk of end_path we compute the upper bound = the index of the
 * first entry whose chunk >= end's chunk (bnode_find's lower_bound). The entry
 * we want is `ub - 1` (the last entry strictly less than end's chunk). If
 * `ub == 0` there's no entry < end's chunk at this level — bail.
 *
 * On exact match with end's chunk: skip it (we want strictly less than end).
 *
 * Safety: on any uncertainty (NULL child, lazy-load failure, no entry < chunk
 * at this level), pop seek-pushed frames and fall back to seek_to_rightmost
 * (correct-but-slow; never drops a key < end_path). within_bounds_reverse
 * filters any keys >= end_path that seek_to_rightmost lands on.
 */
static void seek_to_end_path(database_iterator_t* iter) {
    if (iter == NULL || iter->end_path == NULL || iter->db == NULL) {
        return;
    }
    if (iter->stack_depth == 0) {
        return;
    }
    hbtrie_t* trie = iter->db->trie;
    if (trie == NULL) {
        return;
    }
    uint8_t chunk_size = trie->chunk_size;
    file_bnode_cache_t* fcache = trie->fcache;
    uint32_t btree_node_size = trie->btree_node_size;

    size_t saved_depth = iter->stack_depth;  // only the root frame is present

    size_t path_len_ids = path_length(iter->end_path);
    if (path_len_ids == 0) {
        seek_to_rightmost(iter);
        return;
    }

    hbtrie_node_t* current = iter->stack[0].node;  // root hbtrie_node
    if (current == NULL || current->btree == NULL) {
        return;
    }
    // Stack index of the trie-level frame for `current`: the root frame (0)
    // initially, then the trie frame pushed by the last matched-descend.
    size_t trie_frame_idx = 0;

    for (size_t i = 0; i < path_len_ids; i++) {
        identifier_t* identifier = path_get(iter->end_path, i);
        if (identifier == NULL) {
            goto bail;
        }
        size_t nchunk = identifier_chunk_count(identifier);
        if (nchunk == 0) {
            goto bail;
        }
        for (size_t j = 0; j < nchunk; j++) {
            chunk_t* chunk = identifier_get_chunk(identifier, j);
            if (chunk == NULL) {
                goto bail;
            }
            int is_last = (j == nchunk - 1) && (i == path_len_ids - 1);

            bnode_t* root_bn = current->btree;
            size_t root_index;
            bnode_entry_t* root_entry = bnode_find(root_bn, chunk, &root_index);
            size_t ub = root_index;  // first entry >= end's chunk

            if (atomic_load(&root_bn->level) > 1) {
                // Internal root: descend through the separator whose child holds
                // keys < end's chunk. Separator at ub-1 (if ub>0) is the largest
                // separator < end's chunk; its child holds all keys in
                // [sep.key, next_sep.key) which are all < end. If ub points at
                // an exact match, that separator's child holds keys == chunk,
                // so we still want the child before it (ub-1).
                if (ub == 0) {
                    goto bail;
                }
                size_t root_descend = ub - 1;
                // Position the trie-level frame at root_descend - 1 so scan_prev
                // (after exhausting this subtree) resumes at the separator
                // BEFORE the one we descended through. scan_prev decrements
                // entry_index before processing, so to skip the descended
                // separator (at root_descend) on pop-back, we must make
                // scan_prev's first read land at root_descend - 1. Mirror of
                // the forward seek's "+1 skip" (which accounts for scan_next
                // incrementing before processing). When root_descend == 0,
                // root_descend - 1 underflows to SIZE_MAX — the "frame
                // exhausted" sentinel — so scan_prev pops the trie frame
                // immediately (correct: no smaller separator at this level).
                iter->stack[trie_frame_idx].entry_index = root_descend - 1;
                bnode_entry_t* sep = bnode_get(root_bn, root_descend);
                if (sep == NULL || !sep->is_bnode_child) {
                    goto bail;
                }
                if (sep->child_bnode == NULL && sep->child_disk_offset != 0 && fcache != NULL) {
                    bnode_entry_lazy_load_bnode_child(sep, fcache, chunk_size, btree_node_size);
                }
                if (sep->child_bnode == NULL) {
                    goto bail;
                }
                bnode_t* cur_bn = sep->child_bnode;
                while (atomic_load(&cur_bn->level) > 1) {
                    size_t idx;
                    bnode_entry_t* e = bnode_find(cur_bn, chunk, &idx);
                    (void)e;
                    if (idx == 0) {
                        goto bail;  // no separator < chunk at this level
                    }
                    size_t k = idx - 1;  // last separator < chunk
                    bnode_entry_t* s = bnode_get(cur_bn, k);
                    if (s == NULL || !s->is_bnode_child) {
                        goto bail;
                    }
                    if (s->child_bnode == NULL && s->child_disk_offset != 0 && fcache != NULL) {
                        bnode_entry_lazy_load_bnode_child(s, fcache, chunk_size, btree_node_size);
                    }
                    if (s->child_bnode == NULL) {
                        goto bail;
                    }
                    if (push_bnode_frame(iter, cur_bn, iter->stack_depth - 1) < 0) {
                        goto bail;
                    }
                    // Position this internal frame at k - 1 so scan_prev's
                    // first decrement-and-process lands on the separator BEFORE
                    // the one we descended through (skipping the descended
                    // separator at k). When k == 0, k - 1 underflows to SIZE_MAX
                    // — the "frame exhausted" sentinel — so scan_prev pops this
                    // internal frame immediately (correct: no smaller separator
                    // at this level).
                    iter->stack[iter->stack_depth - 1].entry_index = k - 1;
                    cur_bn = s->child_bnode;
                }
                // cur_bn is the leaf bnode for this trie level.
                size_t ub_leaf;
                bnode_entry_t* leaf_match = bnode_find(cur_bn, chunk, &ub_leaf);
                int leaf_exact = (leaf_match != NULL);
                if (!leaf_exact && ub_leaf == 0) {
                    goto bail;  // no entry < end's chunk and no exact match
                }
                if (push_bnode_frame(iter, cur_bn, iter->stack_depth - 1) < 0) {
                    goto bail;
                }
                size_t leaf_idx = iter->stack_depth - 1;
                if (is_last) {
                    // Largest entry strictly < end's chunk. For exact match,
                    // the entry at ub_leaf == end_path -> exclude, position at
                    // ub_leaf - 1. For no exact match, ub_leaf = insertion
                    // point, position at ub_leaf - 1. Bail if ub_leaf == 0
                    // (exact match at 0: entry == end_path, excluded; or all
                    // entries > end's chunk).
                    if (ub_leaf == 0) {
                        goto bail;
                    }
                    // scan_prev reads at entry_index, then decrements, so the
                    // first emitted entry is ub_leaf - 1.
                    iter->stack[leaf_idx].entry_index = ub_leaf - 1;
                    return;  // positioned — scan_prev emits from leaf_idx
                }
                // end_path continues deeper — descend into the entry's child.
                // For exact match, descend into ub_leaf (the matching entry) to
                // check deeper chunks. For no exact match, descend into ub_leaf-1
                // (the largest entry < end's chunk); all keys under it are < end.
                size_t descend_idx = leaf_exact ? ub_leaf : (ub_leaf - 1);
                bnode_entry_t* leaf_entry = bnode_get(cur_bn, descend_idx);
                if (leaf_entry == NULL) {
                    goto bail;
                }
                hbtrie_node_t* next = NULL;
                if (leaf_entry->has_value) {
                    if (leaf_entry->trie_child == NULL && leaf_entry->child_disk_offset != 0 && fcache != NULL) {
                        bnode_entry_lazy_load_trie_child(leaf_entry, fcache, chunk_size, btree_node_size);
                    }
                    next = leaf_entry->trie_child;
                } else {
                    if (leaf_entry->child == NULL && leaf_entry->child_disk_offset != 0 && fcache != NULL) {
                        bnode_entry_lazy_load_hbtrie_child(leaf_entry, fcache, chunk_size, btree_node_size);
                    }
                    next = leaf_entry->child;
                }
                if (next == NULL) {
                    // No deeper trie level but end_path continues. The entry at
                    // descend_idx is a prefix of end_path (exact match) or < end's
                    // chunk (no exact match) — either way it's < end_path. Position
                    // at descend_idx so scan_prev emits it.
                    iter->stack[leaf_idx].entry_index = descend_idx;
                    return;
                }
                // Position the leaf frame for the descent. For has_value +
                // trie_child, set value_pending=1 and entry_index = descend_idx
                // so scan_prev emits the value AFTER the subtree is exhausted
                // (on pop-back) and chunk collection resolves to the descended
                // entry. For has_value=0 + child, set entry_index = descend_idx-1
                // so chunk collection (entry_index + 1) resolves to the descended
                // entry and scan_prev resumes at the previous entry on pop-back.
                if (leaf_entry->has_value && leaf_entry->trie_child != NULL) {
                    iter->stack[leaf_idx].entry_index = descend_idx;
                    iter->stack[leaf_idx].value_pending = 1;
                } else {
                    iter->stack[leaf_idx].entry_index =
                        (descend_idx == 0) ? SIZE_MAX : (descend_idx - 1);
                }
                if (push_frame(iter, next, iter->stack_depth - 1) < 0) {
                    goto bail;
                }
                current = next;
                trie_frame_idx = iter->stack_depth - 1;
                // Descend into the rightmost entry of the new trie level —
                // the deepest keys in this subtree are < end_path's next chunk
                // (which we'll handle on the next loop iteration). For now,
                // position at the rightmost so if the next chunk's bnode_find
                // bails (no entry < chunk at the next level), scan_prev starts
                // from the rightmost of this subtree.
                iter->stack[trie_frame_idx].entry_index = bnode_count(next->btree) - 1;
            } else {
                // Leaf root (single-level B+tree): the trie-level frame IS the
                // leaf frame. Apply the seek decision directly to it.
                int exact_match = (root_entry != NULL);
                if (is_last) {
                    // Largest entry strictly < end's chunk. For exact match,
                    // the entry at ub == end_path -> exclude, position at ub-1.
                    // For no exact match, ub = insertion point, position at ub-1.
                    // Bail if ub == 0 (exact match at 0: entry == end_path,
                    // excluded; or all entries > end's chunk).
                    if (ub == 0) {
                        goto bail;
                    }
                    iter->stack[trie_frame_idx].entry_index = ub - 1;
                    return;  // positioned — scan_prev emits from trie_frame_idx
                }
                // !is_last: end_path continues deeper — descend into the entry's
                // child. For exact match, descend into ub (the matching entry) to
                // check deeper chunks. For no exact match, descend into ub-1
                // (the largest entry < end's chunk); all keys under it are < end.
                if (!exact_match && ub == 0) {
                    goto bail;  // no entry to descend into
                }
                size_t descend_idx = exact_match ? ub : (ub - 1);
                bnode_entry_t* leaf_entry = bnode_get(root_bn, descend_idx);
                if (leaf_entry == NULL) {
                    goto bail;
                }
                hbtrie_node_t* next = NULL;
                if (leaf_entry->has_value) {
                    if (leaf_entry->trie_child == NULL && leaf_entry->child_disk_offset != 0 && fcache != NULL) {
                        bnode_entry_lazy_load_trie_child(leaf_entry, fcache, chunk_size, btree_node_size);
                    }
                    next = leaf_entry->trie_child;
                } else {
                    if (leaf_entry->child == NULL && leaf_entry->child_disk_offset != 0 && fcache != NULL) {
                        bnode_entry_lazy_load_hbtrie_child(leaf_entry, fcache, chunk_size, btree_node_size);
                    }
                    next = leaf_entry->child;
                }
                if (next == NULL) {
                    // No deeper trie level but end_path continues. The entry at
                    // descend_idx is a prefix of end_path (exact match) or < end's
                    // chunk (no exact match) — either way it's < end_path. Position
                    // at descend_idx so scan_prev emits it.
                    iter->stack[trie_frame_idx].entry_index = descend_idx;
                    return;
                }
                // Position the leaf frame for the descent. For has_value +
                // trie_child, set value_pending=1 and entry_index = descend_idx
                // so scan_prev emits the value AFTER the subtree is exhausted
                // (on pop-back) and chunk collection resolves to the descended
                // entry. For has_value=0 + child, set entry_index = descend_idx-1
                // so chunk collection (entry_index + 1) resolves to the descended
                // entry and scan_prev resumes at the previous entry on pop-back.
                if (leaf_entry->has_value && leaf_entry->trie_child != NULL) {
                    iter->stack[trie_frame_idx].entry_index = descend_idx;
                    iter->stack[trie_frame_idx].value_pending = 1;
                } else {
                    iter->stack[trie_frame_idx].entry_index =
                        (descend_idx == 0) ? SIZE_MAX : (descend_idx - 1);
                }
                if (push_frame(iter, next, iter->stack_depth - 1) < 0) {
                    goto bail;
                }
                current = next;
                trie_frame_idx = iter->stack_depth - 1;
                iter->stack[trie_frame_idx].entry_index = bnode_count(next->btree) - 1;
            }
        }
    }
    // Every chunk of end_path matched exactly with a child trie level (and we
    // positioned each trie-level frame at the rightmost entry, which scan_prev
    // will decrement-and-process). The deepest trie frame sits at the rightmost
    // entry of the last descended level; its rightmost leaf shares all of
    // end_path as a prefix, so every key under it is < end_path (since end_path
    // is a strict prefix-extension — wait, this is wrong for the equal case).
    // Actually: we got here because end_path's chunks ALL matched exactly with
    // a child descent (so end_path is a stored key OR a strict prefix of longer
    // keys). We want the largest key < end_path, which is the rightmost leaf
    // of the subtree rooted just before end_path's last chunk's match. The
    // descent above positioned the deepest trie frame at the rightmost entry
    // of the last descended child; scan_prev will decrement-and-emit, giving
    // the rightmost key of that subtree — which is < end_path. Correct.
    return;

bail:
    // Restore the original walk-from-end state: pop every frame the seek
    // pushed and reset the root frame to the rightmost entry, then let
    // seek_to_rightmost position us at the global rightmost. within_bounds_reverse
    // filters any keys >= end_path.
    while (iter->stack_depth > saved_depth) {
        pop_frame(iter);
    }
    if (iter->stack_depth > 0) {
        iter->stack[0].entry_index = 0;
    }
    seek_to_rightmost(iter);
}

// Resolve the read transaction ID a scan should snapshot.
//
// In normal (multi-writer MVCC) mode the iterator snapshots
// tx_manager_get_last_committed(), and version_entry_find_visible() walks each
// entry's version chain to pick the newest version with txn_id <= read_txn_id.
//
// In sync_only mode this breaks. Sync commits deliberately bypass
// tx_manager_commit (the fast put/get paths skip the tx_manager for speed), so
// the tx_manager's last_committed_txn_id is never advanced -- it stays at its
// zero init forever. Meanwhile the entries' versions ARE stamped with real
// txn_ids (via transaction_id_get_next). find_visible() with read_txn_id == 0
// then hides EVERY versioned entry: the fast path's `head.txn_id <= 0` test is
// false (real txn_ids are > 0), the slow path finds no version with txn_id <=
// 0, and it returns NULL. A delete-then-re-put (or any update) turns a key
// from a legacy single-value leaf into a versioned leaf, so it vanishes from
// the scan -- while point-get still sees it, because database_get_sync's
// sync_only fast path uses hbtrie_find_unsafe(), which ignores read_txn_id and
// returns the head (newest) version directly.
//
// The fix: in sync_only mode give the scan a sentinel MAX read_txn_id so
// find_visible()'s fast path (`head.txn_id <= read_txn_id`) always succeeds
// and returns the head -- live head -> visible, tombstone head -> NULL. That
// is exactly hbtrie_find_unsafe()'s sync_only visibility, so scans and point
// gets agree again. Safe because sync_only is single-writer: every stamped
// version is the latest committed state, so newest-version-wins is correct,
// and read_txn_id is consumed only by find_visible() (no sentinel check is
// ever applied to an iterator's read_txn_id).
static transaction_id_t scan_read_txn_id(database_t* db) {
    if (db != NULL && db->sync_only) {
        return TXN_ID_SENTINEL;
    }
    return tx_manager_get_last_committed(db ? db->tx_manager : NULL);
}

database_iterator_t* database_scan_start(database_t* db,
                                          path_t* start_path,
                                          path_t* end_path) {
    if (db == NULL) {
        return NULL;
    }

    // Block new cursor creation while vacuum is in progress.
    // This pairs with vacuum_auto's wait-loop: vacuum waits for open cursors
    // to close, then sets vacuum_in_progress under cursor_count_mutex so new
    // cursors block here until vacuum finishes.
    platform_lock(&db->cursor_count_mutex);
    while (atomic_load(&db->vacuum_in_progress) != 0) {
        platform_condition_timedwait(&db->cursor_count_mutex, &db->cursor_cvar, 0);  // 0 = wait forever
    }
    platform_unlock(&db->cursor_count_mutex);

    database_iterator_t* iter = get_clear_memory(sizeof(database_iterator_t));
    if (iter == NULL) {
        return NULL;
    }

    iter->db = db;
    // Register this cursor — vacuum will refuse/wait while count > 0
    atomic_fetch_add(&db->open_cursor_count, 1);
    // Copy paths — the iterator owns its copies, callers retain theirs
    iter->start_path = start_path ? path_copy(start_path) : NULL;
    iter->end_path = end_path ? path_copy(end_path) : NULL;
    iter->current_path = path_create();
    iter->finished = 0;

    // Allocate initial stack
    iter->stack = get_clear_memory(INITIAL_STACK_SIZE * sizeof(iterator_frame_t));
    if (iter->stack == NULL) {
        if (iter->start_path) path_destroy(iter->start_path);
        if (iter->end_path) path_destroy(iter->end_path);
        if (iter->current_path) path_destroy(iter->current_path);
        // Unregister — we incremented above but won't return the iterator
        atomic_fetch_sub(&db->open_cursor_count, 1);
        free(iter);
        return NULL;
    }
    iter->stack_size = INITIAL_STACK_SIZE;
    iter->stack_depth = 0;

    // Initialize read transaction ID from database's transaction manager
    // For synchronous scans, we use the current transaction context
    iter->read_txn_id = scan_read_txn_id(db);

    // Push root node onto stack
    hbtrie_node_t* root = db->trie ? atomic_load_ptr(&db->trie->root, hbtrie_node_t*) : NULL;
    if (root) {
        iter->stack[0].node = root;
        iter->stack[0].entry_index = 0;
        iter->stack[0].path_index = 0;
        iter->stack_depth = 1;

        // Reference the root node
        REFERENCE(root, hbtrie_node_t);
    }

    refcounter_init((refcounter_t*)iter);

    // If a start_path bound is given, seek the stack to the first leaf >= it so
    // the scan is O(log N + results) instead of O(keys-before-prefix). On any
    // uncertainty the seek falls back to the root frame at entry_index 0 (the
    // original walk-from-start behavior), so this never changes correctness.
    if (iter->start_path != NULL) {
        seek_to_start_path(iter);
    }

    return iter;
}

void database_scan_end(database_iterator_t* iter) {
    if (iter == NULL) return;

    // Dereference the iterator
    refcounter_dereference((refcounter_t*)iter);

    // Check if we should free
    if (refcounter_count((refcounter_t*)iter) == 0) {
        // Clean up paths
        if (iter->start_path) path_destroy(iter->start_path);
        if (iter->end_path) path_destroy(iter->end_path);
        if (iter->current_path) path_destroy(iter->current_path);

        // Dereference all nodes on stack
        for (size_t i = 0; i < iter->stack_depth; i++) {
            if (iter->stack[i].is_bnode_frame) {
                if (iter->stack[i].bnode) {
                    DEREFERENCE(iter->stack[i].bnode);
                }
            } else {
                if (iter->stack[i].node) {
                    DEREFERENCE(iter->stack[i].node);
                }
            }
        }

        // Free stack
        if (iter->stack) free(iter->stack);

        // Unregister this cursor — if we were the last one, wake any
        // vacuum waiting for cursors to close.
        if (iter->db != NULL) {
            long prev = atomic_fetch_sub(&iter->db->open_cursor_count, 1);
            if (prev == 1) {
                // Last cursor closed — broadcast cursor_cvar so any waiting
                // vacuum can proceed
                platform_lock(&iter->db->cursor_count_mutex);
                platform_broadcast_condition(&iter->db->cursor_cvar);
                platform_unlock(&iter->db->cursor_count_mutex);
            }
        }

        // Free iterator
        free(iter);
    }
}

int database_scan_next(database_iterator_t* iter,
                        path_t** out_path,
                        identifier_t** out_value) {
    if (iter == NULL || out_path == NULL || out_value == NULL) {
        return -2;
    }

    *out_path = NULL;
    *out_value = NULL;

    if (iter->finished || iter->stack_depth == 0) {
        return -1;  // End of iteration
    }

    // Get chunk_size from database trie
    uint8_t chunk_size = iter->db->trie ? iter->db->trie->chunk_size : DEFAULT_CHUNK_SIZE;

    // Depth-first traversal
    while (iter->stack_depth > 0) {
        // Get current frame
        iterator_frame_t* frame = &iter->stack[iter->stack_depth - 1];

        // Determine the bnode to iterate based on frame type
        bnode_t* btree = NULL;
        if (frame->is_bnode_frame) {
            // B+tree internal node frame — descend through multi-level B+tree
            btree = frame->bnode;
            if (btree == NULL) {
                pop_frame(iter);
                continue;
            }
        } else {
            // Trie-level frame — get btree from the hbtrie_node
            hbtrie_node_t* node = frame->node;
            if (node == NULL || node->btree == NULL) {
                pop_frame(iter);
                continue;
            }
            btree = node->btree;
        }

        size_t count = bnode_count(btree);

        // Track if we pushed a child frame (if so, don't pop this frame)
        int pushed_child = 0;

        // Process entries at current level
        while (frame->entry_index < count) {
            bnode_entry_t* entry = bnode_get(btree, frame->entry_index);
            // Don't increment entry_index yet - we may need to push a child

            if (entry == NULL) {
                frame->entry_index++;  // Skip null entries
                continue;
            }

            // Check if this entry has a value (complete path)
            if (entry->has_value) {
                frame->entry_index++;  // Move past this entry since we're processing it

                // If entry also has a trie_child, push it for later traversal
                if (entry->trie_child == NULL && entry->child_disk_offset != 0
                    && iter->db->trie != NULL && iter->db->trie->fcache != NULL) {
                    bnode_entry_lazy_load_trie_child(entry, iter->db->trie->fcache,
                                                     iter->db->trie->chunk_size,
                                                     iter->db->trie->btree_node_size);
                }
                int pushed_trie_child = 0;
                {
                    size_t parent_frame_idx = iter->stack_depth - 1;
                    if (entry->trie_child) {
                        if (push_frame(iter, entry->trie_child, iter->stack_depth - 1) < 0) {
                            return -2;
                        }
                        pushed_trie_child = 1;
                        // push_frame() may realloc iter->stack, invalidating the
                        // cached `frame` pointer. Re-fetch the parent frame by its
                        // absolute index (NOT stack_depth-2, which is wrong once
                        // more than one frame has been pushed in this pass).
                        frame = &iter->stack[parent_frame_idx];
                    }
                }

                identifier_t* value = NULL;
                int has_visible_value = 0;

                // Get visible value from version chain
                if (entry->has_versions && entry->versions) {
                    version_entry_t* visible = version_entry_find_visible(
                        entry->versions, iter->read_txn_id);
                    if (visible && !visible->is_deleted) {
                        has_visible_value = 1;
                        if (visible->value) {
                            value = REFERENCE(visible->value, identifier_t);
                        }
                    }
                } else if (entry->has_value) {
                    // Legacy single value. Note: a 0-length presence marker
                    // (e.g. graph SPO/POS/OSP/PSO index entries) is inserted
                    // in-memory as a non-NULL empty identifier, but the V3
                    // deserializer restores it as entry->value == NULL with
                    // has_value == 1. Treat has_value==1 as "the leaf exists"
                    // so such leaves are still emitted on a reopened database,
                    // matching fresh-db scan behavior.
                    has_visible_value = 1;
                    if (entry->value) {
                        value = REFERENCE(entry->value, identifier_t);
                    }
                }

                // has_value leaf whose (possibly empty) value deserialized to
                // a NULL pointer: synthesize a non-NULL empty identifier so
                // downstream callers (which hand out_value to
                // identifier_get_data_copy / identifier_destroy) see a valid
                // 0-length value instead of an uninitialized one.
                if (has_visible_value && value == NULL) {
                    uint8_t cs = iter->db->trie ? iter->db->trie->chunk_size
                                                : DEFAULT_CHUNK_SIZE;
                    value = identifier_create_empty(cs);
                    if (value == NULL) {
                        has_visible_value = 0;  // OOM — skip this leaf
                    }
                }

                if (has_visible_value) {
                    // Build path from stack using path_meta metadata
                    // The leaf entry stores per-subscript {chunk_count, byte_length}

                    // Get path metadata from the leaf entry
                    size_t num_identifiers = 0;
                    const path_subscript_meta_t* path_meta = bnode_entry_get_path_meta(entry, &num_identifiers);

                    // Collect chunks from the stack. Count exactly one chunk per
                    // HBTrie level: a multi-level B+tree (btree overflows at scale)
                    // pushes extra is_bnode_child routing frames whose key is a
                    // B+tree separator, NOT an identifier chunk. Counting those
                    // would over-count by (btree_depth - 1) per split level and
                    // misalign the collected chunks vs path_meta.chunk_count,
                    // producing NUL-padded, mis-split keys on scan. Skip them.
                    size_t nchunks = 0;
                    for (size_t i = 0; i < iter->stack_depth; i++) {
                        iterator_frame_t* f = &iter->stack[i];
                        if (f->entry_index > 0) {
                            bnode_t* f_btree = f->is_bnode_frame ? f->bnode : (f->node ? f->node->btree : NULL);
                            if (f_btree == NULL) continue;
                            bnode_entry_t* e = bnode_get(f_btree, f->entry_index - 1);
                            if (e != NULL && e->key != NULL && !e->is_bnode_child) {
                                nchunks++;
                            }
                        }
                    }

                    // Allocate chunk array
                    chunk_t** chunks = NULL;
                    if (nchunks > 0) {
                        chunks = malloc(nchunks * sizeof(chunk_t*));
                        if (chunks == NULL) {
                            identifier_destroy(value);
                            return -2;
                        }

                        size_t idx = 0;
                        for (size_t i = 0; i < iter->stack_depth; i++) {
                            iterator_frame_t* f = &iter->stack[i];
                            if (f->entry_index > 0) {
                                bnode_t* f_btree = f->is_bnode_frame ? f->bnode : (f->node ? f->node->btree : NULL);
                                if (f_btree == NULL) continue;
                                bnode_entry_t* e = bnode_get(f_btree, f->entry_index - 1);
                                if (e != NULL && e->key != NULL && !e->is_bnode_child) {
                                    chunks[idx++] = e->key;
                                }
                            }
                        }
                    }

                    // Build path from collected chunks
                    path_t* result_path = path_create();
                    if (result_path == NULL) {
                        if (chunks) free(chunks);
                        identifier_destroy(value);
                        return -2;
                    }

                    if (path_meta != NULL && num_identifiers > 0) {
                        // Use metadata to split chunks into identifiers with exact lengths
                        size_t chunk_offset = 0;
                        // Skip prefix identifiers (subtree prefix components).
                        // prefix_skip is 0 for non-subtree iterators.
                        size_t skip = iter->prefix_skip;
                        if (skip > num_identifiers) skip = num_identifiers;
                        for (size_t i = 0; i < skip; i++) {
                            chunk_offset += path_meta[i].chunk_count;
                        }
                        // Build remaining identifiers (post-prefix)
                        for (size_t i = skip; i < num_identifiers; i++) {
                            size_t id_chunks = path_meta[i].chunk_count;
                            size_t id_byte_length = path_meta[i].byte_length;
                            identifier_t* id = build_identifier_from_chunks(
                                chunks + chunk_offset, id_chunks, chunk_size, id_byte_length);
                            if (id == NULL) {
                                if (chunks) free(chunks);
                                path_destroy(result_path);
                                identifier_destroy(value);
                                return -2;
                            }
                            int rc = path_append(result_path, id);
                            identifier_destroy(id);  // path_append takes a reference
                            if (rc != 0) {
                                if (chunks) free(chunks);
                                path_destroy(result_path);
                                identifier_destroy(value);
                                return -2;
                            }
                            chunk_offset += id_chunks;
                        }
                    } else {
                        // Legacy entry without metadata - treat as single identifier (padded)
                        identifier_t* key_id = build_identifier_from_chunks(chunks, nchunks, chunk_size, 0);
                        if (key_id == NULL) {
                            if (chunks) free(chunks);
                            path_destroy(result_path);
                            identifier_destroy(value);
                            return -2;
                        }
                        int rc = path_append(result_path, key_id);
                        identifier_destroy(key_id);
                        if (rc != 0) {
                            if (chunks) free(chunks);
                            path_destroy(result_path);
                            identifier_destroy(value);
                            return -2;
                        }
                    }

                    if (chunks) free(chunks);

                    // Check bounds on the result path
                    int bounds = within_bounds(iter, result_path);
                    if (bounds < 0) {
                        // Past upper bound — stop iteration entirely
                        path_destroy(result_path);
                        identifier_destroy(value);
                        iter->finished = 1;
                        return -1;
                    }
                    if (bounds == 0) {
                        // Before lower bound — skip this entry, keep scanning.
                        // If we pushed a trie_child, descend into it now: its
                        // subtree extends this prefix and may reach the range.
                        path_destroy(result_path);
                        identifier_destroy(value);
                        if (pushed_trie_child) {
                            pushed_child = 1;
                            break;
                        }
                        continue;
                    }

                    *out_path = result_path;
                    *out_value = value;
                    return 0;  // Success
                }
                /* has_visible_value == 0: tombstone (deleted version). We must
                   still check bounds — a tombstone past the upper bound means
                   ALL subsequent entries are also past the upper bound (scan is
                   in sorted order), so we must STOP here. Without this check,
                   the scan traverses every tombstone in sibling keyspaces past
                   the end bound, pushing trie_child frames without popping
                   (the stack grows unboundedly → OOM) and wasting O(tombstones)
                   time per scan. This was the root cause of the IVF recall gate
                   OOM: centroid scans walked through ~1960 cluster tombstones
                   after the centroid range, growing the stack to 4500+ frames. */
                {
                    path_t* __tpath = path_create();
                    if (__tpath != NULL) {
                        size_t __nchunks = 0;
                        for (size_t i = 0; i < iter->stack_depth; i++) {
                            iterator_frame_t* f = &iter->stack[i];
                            if (f->entry_index > 0) {
                                bnode_t* f_btree = f->is_bnode_frame ? f->bnode : (f->node ? f->node->btree : NULL);
                                if (f_btree == NULL) continue;
                                bnode_entry_t* e = bnode_get(f_btree, f->entry_index - 1);
                                if (e != NULL && e->key != NULL && !e->is_bnode_child) __nchunks++;
                            }
                        }
                        chunk_t** __chunks = __nchunks ? malloc(__nchunks * sizeof(chunk_t*)) : NULL;
                        if (__nchunks == 0 || __chunks != NULL) {
                            size_t __idx = 0;
                            for (size_t i = 0; i < iter->stack_depth; i++) {
                                iterator_frame_t* f = &iter->stack[i];
                                if (f->entry_index > 0) {
                                    bnode_t* f_btree = f->is_bnode_frame ? f->bnode : (f->node ? f->node->btree : NULL);
                                    if (f_btree == NULL) continue;
                                    bnode_entry_t* e = bnode_get(f_btree, f->entry_index - 1);
                                    if (e != NULL && e->key != NULL && !e->is_bnode_child) __chunks[__idx++] = e->key;
                                }
                            }
                            size_t __num_ids = 0;
                            const path_subscript_meta_t* __pm = bnode_entry_get_path_meta(entry, &__num_ids);
                            if (__pm != NULL && __num_ids > 0) {
                                size_t __co = 0;
                                size_t __skip = iter->prefix_skip;
                                if (__skip > __num_ids) __skip = __num_ids;
                                for (size_t i = 0; i < __skip; i++) __co += __pm[i].chunk_count;
                                for (size_t i = __skip; i < __num_ids; i++) {
                                    identifier_t* __id = build_identifier_from_chunks(
                                        __chunks + __co, __pm[i].chunk_count, chunk_size, __pm[i].byte_length);
                                    if (__id == NULL) break;
                                    path_append(__tpath, __id);
                                    identifier_destroy(__id);
                                    __co += __pm[i].chunk_count;
                                }
                            } else {
                                identifier_t* __id = build_identifier_from_chunks(__chunks, __nchunks, chunk_size, 0);
                                if (__id != NULL) { path_append(__tpath, __id); identifier_destroy(__id); }
                            }
                            free(__chunks);
                        }
                        int __bounds = within_bounds(iter, __tpath);
                        path_destroy(__tpath);
                        if (__bounds < 0) {
                            iter->finished = 1;
                            return -1;
                        }
                        /* __bounds == 0 (before lower bound) or == 1 (within
                           bounds): skip this tombstone, continue scanning. */
                    }
                }
                if (pushed_trie_child) {
                    /* Tombstoned prefix with a live subtree: descend into the
                       pushed child NOW, exactly like the other descend branches.
                       Continuing to walk the parent here left the pushed frame
                       ignored on the stack; the stack_depth-2 re-fetch above then
                       pointed at the pushed child instead of the parent on the
                       next push, walking the parent's entries with the child's
                       entry_index — an infinite re-push loop (the "trie cycle"
                       of docs/trie-cycle-investigation.md). Descending here is
                       also the correct emit order: subtree keys extend this
                       prefix, so they sort before the parent's next entry. */
                    pushed_child = 1;
                    break;
                }
            } else if (entry->is_bnode_child) {
                // Internal B+tree node — push a bnode frame to descend
                // through the multi-level B+tree. child_bnode is a bnode_t*,
                // NOT an hbtrie_node_t* — pushing it as a trie frame would
                // cause type confusion (node->btree reads garbage).
                // On a reopened database the in-memory child_bnode pointer
                // is NULL and only child_disk_offset is set; lazy-load it
                // from the page file before descending (mirrors hbtrie.c's
                // descent paths, e.g. bnode_entry_lazy_load_bnode_child).
                if (entry->child_bnode == NULL && entry->child_disk_offset != 0
                    && iter->db->trie != NULL && iter->db->trie->fcache != NULL) {
                    bnode_entry_lazy_load_bnode_child(entry, iter->db->trie->fcache,
                                                      iter->db->trie->chunk_size,
                                                      iter->db->trie->btree_node_size);
                }
                if (entry->child_bnode == NULL) {
                    // Child unavailable (not in memory, not on disk) — skip
                    frame->entry_index++;
                    continue;
                }
                frame->entry_index++;
                pushed_child = 1;
                if (push_bnode_frame(iter, entry->child_bnode, iter->stack_depth - 1) < 0) {
                    return -2;
                }
                break;
            } else if (entry->child != NULL || entry->child_disk_offset != 0) {
                // Trie child (hbtrie_node_t*) — push for deeper traversal.
                // On a reopened database the in-memory child pointer is NULL
                // and only child_disk_offset is set; lazy-load it from the
                // page file before descending (mirrors hbtrie.c's descent
                // path, e.g. bnode_entry_lazy_load_hbtrie_child). Without
                // this the iterator never reached value-bearing leaves on a
                // reopened trie, so scans returned 0 results (and, with
                // different heap layout, segfaulted during result rebuild).
                if (entry->child == NULL && entry->child_disk_offset != 0
                    && iter->db->trie != NULL && iter->db->trie->fcache != NULL) {
                    bnode_entry_lazy_load_hbtrie_child(entry, iter->db->trie->fcache,
                                                       iter->db->trie->chunk_size,
                                                       iter->db->trie->btree_node_size);
                }
                if (entry->child == NULL) {
                    // Child not loadable — skip
                    frame->entry_index++;
                    continue;
                }
                frame->entry_index++;
                pushed_child = 1;
                if (push_frame(iter, entry->child, iter->stack_depth - 1) < 0) {
                    return -2;
                }
                break;
            } else if (entry->trie_child || entry->child_disk_offset != 0) {
                // Lazy-load trie_child if needed
                if (entry->trie_child == NULL && entry->child_disk_offset != 0
                    && iter->db->trie != NULL && iter->db->trie->fcache != NULL) {
                    bnode_entry_lazy_load_trie_child(entry, iter->db->trie->fcache,
                                                     iter->db->trie->chunk_size,
                                                     iter->db->trie->btree_node_size);
                }
                if (entry->trie_child == NULL) {
                    frame->entry_index++;
                    continue;
                }
                // Entry has both value and trie_child - push trie_child for traversal
                frame->entry_index++;
                pushed_child = 1;
                if (push_frame(iter, entry->trie_child, iter->stack_depth - 1) < 0) {
                    return -2;
                }
                break;
            } else {
                // Entry has no value and no child - skip it
                frame->entry_index++;
            }
        }

        // If we've processed all entries at this level and didn't push a child, pop the frame
        if (!pushed_child && frame->entry_index >= count) {
            pop_frame(iter);
        }
    }

    // No more entries
    iter->finished = 1;
    return -1;
}
/* Position the iterator stack at the rightmost value-bearing leaf, by always
   descending into the last entry of each bnode. Used when no end_path bound
   is given (reverse scan from the largest key in the db).

   Mirror of the no-start_path forward path (root frame at entry_index 0), but
   rightmost instead of leftmost. After descending through entry `count-1` of
   a frame, the frame's entry_index is decremented (to count-2, or SIZE_MAX
   when count==1) so that database_scan_prev — which decrements entry_index
   AFTER reading the current entry — does not re-process the descended entry
   when it pops back to this frame. Path reconstruction then uses
   `entry_index + 1` to recover the descended entry's chunk (with unsigned
   wraparound giving 0 when entry_index underflowed to SIZE_MAX). */
static void seek_to_rightmost(database_iterator_t* iter) {
    if (iter == NULL || iter->stack_depth == 0) return;
    hbtrie_t* trie = iter->db ? iter->db->trie : NULL;
    if (trie == NULL) return;
    uint8_t chunk_size = trie->chunk_size;
    file_bnode_cache_t* fcache = trie->fcache;
    uint32_t btree_node_size = trie->btree_node_size;

    while (iter->stack_depth > 0) {
        iterator_frame_t* frame = &iter->stack[iter->stack_depth - 1];
        bnode_t* btree = NULL;
        if (frame->is_bnode_frame) {
            btree = frame->bnode;
        } else {
            hbtrie_node_t* node = frame->node;
            if (node == NULL || node->btree == NULL) break;
            btree = node->btree;
        }
        if (btree == NULL) break;
        size_t count = bnode_count(btree);
        if (count == 0) break;

        frame->entry_index = count - 1;
        bnode_entry_t* entry = bnode_get(btree, count - 1);
        if (entry == NULL) break;

        /* Lazy-load trie_child if needed (reopened db). */
        if (entry->trie_child == NULL && entry->child_disk_offset != 0 && fcache != NULL) {
            bnode_entry_lazy_load_trie_child(entry, fcache, chunk_size, btree_node_size);
        }
        if (entry->trie_child != NULL) {
            /* Descend into trie_child. This covers BOTH the has_value=1 +
               trie_child case (prefix-shared longer keys sort after this
               entry's value) and the has_value=0 + trie_child case. */
            if (push_frame(iter, entry->trie_child, iter->stack_depth - 1) < 0) break;
            // push_frame() may realloc iter->stack; re-fetch the parent frame
            // (child now at stack_depth-1, parent at stack_depth-2) before
            // writing value_pending / entry_index.
            frame = &iter->stack[iter->stack_depth - 2];
            if (entry->has_value) {
                /* value_pending: keep entry_index at count-1 (do NOT decrement)
                   so scan_prev re-reads this entry on pop-back and emits its
                   value via the value_pending two-visit logic. */
                frame->value_pending = 1;
            } else {
                /* Decrement parent so scan_prev doesn't re-process this entry
                   on pop-back. Underflows to SIZE_MAX when count==1 (sentinel). */
                frame->entry_index--;
            }
            continue;
        }
        /* No trie_child. If has_value=1, this is a value-bearing leaf —
           positioned, stop. MUST check before the `child` branch: the
           `child`/`value`/`versions` fields share a union, so when
           has_value=1, entry->child reads the value pointer (not a child
           hbtrie_node) and descending would type-confuse the pointer. */
        if (entry->has_value) {
            break;
        }
        if (entry->is_bnode_child) {
            if (entry->child_bnode == NULL && entry->child_disk_offset != 0 && fcache != NULL) {
                bnode_entry_lazy_load_bnode_child(entry, fcache, chunk_size, btree_node_size);
            }
            if (entry->child_bnode == NULL) break;
            if (push_bnode_frame(iter, entry->child_bnode, iter->stack_depth - 1) < 0) break;
            frame = &iter->stack[iter->stack_depth - 2];  // re-fetch after realloc
            frame->entry_index--;
            continue;
        }
        if (entry->child == NULL && entry->child_disk_offset != 0 && fcache != NULL) {
            bnode_entry_lazy_load_hbtrie_child(entry, fcache, chunk_size, btree_node_size);
        }
        if (entry->child != NULL) {
            if (push_frame(iter, entry->child, iter->stack_depth - 1) < 0) break;
            frame = &iter->stack[iter->stack_depth - 2];  // re-fetch after realloc
            frame->entry_index--;
            continue;
        }
        /* No value, no child — empty leaf, stop. */
        break;
    }
}

database_iterator_t* database_scan_start_reverse(database_t* db,
                                                   path_t* start_path,
                                                   path_t* end_path) {
    if (db == NULL) {
        return NULL;
    }

    /* Block new cursor creation while vacuum is in progress — mirrors
       database_scan_start so vacuum's wait-loop can drain open cursors. */
    platform_lock(&db->cursor_count_mutex);
    while (atomic_load(&db->vacuum_in_progress) != 0) {
        platform_condition_timedwait(&db->cursor_count_mutex, &db->cursor_cvar, 0);
    }
    platform_unlock(&db->cursor_count_mutex);

    database_iterator_t* iter = get_clear_memory(sizeof(database_iterator_t));
    if (iter == NULL) {
        return NULL;
    }

    iter->db = db;
    iter->reverse = 1;
    atomic_fetch_add(&db->open_cursor_count, 1);
    iter->start_path = start_path ? path_copy(start_path) : NULL;
    iter->end_path = end_path ? path_copy(end_path) : NULL;
    iter->current_path = path_create();
    iter->finished = 0;

    iter->stack = get_clear_memory(INITIAL_STACK_SIZE * sizeof(iterator_frame_t));
    if (iter->stack == NULL) {
        if (iter->start_path) path_destroy(iter->start_path);
        if (iter->end_path) path_destroy(iter->end_path);
        if (iter->current_path) path_destroy(iter->current_path);
        atomic_fetch_sub(&db->open_cursor_count, 1);
        free(iter);
        return NULL;
    }
    iter->stack_size = INITIAL_STACK_SIZE;
    iter->stack_depth = 0;

    iter->read_txn_id = scan_read_txn_id(db);

    hbtrie_node_t* root = db->trie ? atomic_load_ptr(&db->trie->root, hbtrie_node_t*) : NULL;
    if (root) {
        iter->stack[0].node = root;
        iter->stack[0].bnode = NULL;
        iter->stack[0].entry_index = 0;
        iter->stack[0].path_index = 0;
        iter->stack[0].is_bnode_frame = 0;
        iter->stack[0].value_pending = 0;
        iter->stack_depth = 1;
        REFERENCE(root, hbtrie_node_t);
    }

    refcounter_init((refcounter_t*)iter);

    /* Position at the rightmost leaf. If an end_path bound is given, clamp
       to the largest key < end_path via seek_to_end_path; otherwise descend
       from the global rightmost. seek_to_end_path falls back to
       seek_to_rightmost on any uncertainty (within_bounds_reverse filters any
       keys >= end_path). */
    if (iter->end_path != NULL) {
        seek_to_end_path(iter);
    } else {
        seek_to_rightmost(iter);
    }

    return iter;
}

int database_scan_prev(database_iterator_t* iter,
                       path_t** out_path,
                       identifier_t** out_value) {
    if (iter == NULL || out_path == NULL || out_value == NULL) {
        return -2;
    }
    *out_path = NULL;
    *out_value = NULL;

    if (iter->finished || iter->stack_depth == 0) {
        return -1;
    }

    uint8_t chunk_size = iter->db->trie ? iter->db->trie->chunk_size : DEFAULT_CHUNK_SIZE;

    while (iter->stack_depth > 0) {
        iterator_frame_t* frame = &iter->stack[iter->stack_depth - 1];

        bnode_t* btree = NULL;
        if (frame->is_bnode_frame) {
            btree = frame->bnode;
            if (btree == NULL) { pop_frame(iter); continue; }
        } else {
            hbtrie_node_t* node = frame->node;
            if (node == NULL || node->btree == NULL) { pop_frame(iter); continue; }
            btree = node->btree;
        }

        size_t count = bnode_count(btree);
        int pushed_child = 0;

        /* SIZE_MAX is the underflow sentinel: entry_index decremented past 0
           means this frame is exhausted. */
        while (frame->entry_index != SIZE_MAX && frame->entry_index < count) {
            if (frame->entry_index >= count) {
                if (count == 0) break;
                frame->entry_index = count - 1;
            }
            bnode_entry_t* entry = bnode_get(btree, frame->entry_index);
            size_t this_index = frame->entry_index;
            frame->entry_index--;  /* decrement BEFORE processing (mirror of hbtrie_cursor_prev) */

            if (entry == NULL) continue;

            /* Lazy-load trie_child if needed (reopened db). Needed for the
               has_value+trie_child descent below, and harmless for the
               !has_value trie_child branch (which also lazy-loads). */
            if (entry->trie_child == NULL && entry->child_disk_offset != 0
                && iter->db->trie != NULL && iter->db->trie->fcache != NULL) {
                bnode_entry_lazy_load_trie_child(entry, iter->db->trie->fcache,
                                                 iter->db->trie->chunk_size,
                                                 iter->db->trie->btree_node_size);
            }

            /* has_value + trie_child: two-visit logic (mirror of
               hbtrie_cursor_prev). First visit descends into the subtree
               (whose keys sort AFTER this entry's value because the value is
               a prefix of them), second visit emits the value after the
               subtree is exhausted. */
            if (entry->has_value && entry->trie_child != NULL
                && frame->value_pending == 0) {
                /* First visit: re-position entry_index at this_index so we
                   revisit this entry after the subtree is exhausted, set
                   value_pending, push trie_child, descend. Do NOT emit yet. */
                frame->entry_index = this_index;
                frame->value_pending = 1;
                pushed_child = 1;
                if (push_frame(iter, entry->trie_child, iter->stack_depth - 1) < 0) {
                    return -2;
                }
                seek_to_rightmost(iter);
                break;
            }

            if (entry->has_value) {
                /* Either no trie_child, or second visit (value_pending == 1)
                   after the subtree was exhausted. Emit the value. Clear
                   value_pending (no-op if it was already 0). entry_index was
                   already decremented above (this_index - 1), which is correct
                   for both cases: the no-trie_child case resumes at the
                   previous entry, and the value_pending case has already
                   emitted the subtree's keys. */
                frame->value_pending = 0;
                identifier_t* value = NULL;
                int has_visible_value = 0;

                /* MVCC visibility. In sync_only mode has_versions==0, so the
                   legacy `else` branch is taken. Task 5 will exercise the
                   version-chain branch. */
                if (entry->has_versions && entry->versions) {
                    version_entry_t* visible = version_entry_find_visible(
                        entry->versions, iter->read_txn_id);
                    if (visible && !visible->is_deleted) {
                        has_visible_value = 1;
                        if (visible->value) {
                            value = REFERENCE(visible->value, identifier_t);
                        }
                    }
                } else {
                    has_visible_value = 1;
                    if (entry->value) {
                        value = REFERENCE(entry->value, identifier_t);
                    }
                }

                /* Synthesize a non-NULL empty identifier for presence-marker
                   leaves (has_value=1, value==NULL after V3 deserialize). */
                if (has_visible_value && value == NULL) {
                    value = identifier_create_empty(chunk_size);
                    if (value == NULL) has_visible_value = 0;
                }

                if (has_visible_value) {
                    /* Collect chunks from the stack. Top frame uses this_index
                       (the entry we just emitted). Non-top frames use
                       entry_index + 1 — the entry we descended through before
                       decrementing. When entry_index underflowed to SIZE_MAX
                       (count==1 descent), SIZE_MAX + 1 wraps to 0 via unsigned
                       arithmetic, recovering the correct index. For value_pending
                       frames (has_value+trie_child descent), entry_index was NOT
                       decremented — it points AT the descended entry — so use
                       entry_index directly. Skip is_bnode_child routing entries
                       (B+tree separators, not identifier chunks) — mirrors the
                       forward path. */
                    size_t nchunks = 0;
                    for (size_t i = 0; i < iter->stack_depth; i++) {
                        iterator_frame_t* f = &iter->stack[i];
                        bnode_t* f_btree = f->is_bnode_frame ? f->bnode
                                : (f->node ? f->node->btree : NULL);
                        if (f_btree == NULL) continue;
                        size_t idx;
                        if (i == iter->stack_depth - 1) {
                            idx = this_index;
                        } else if (f->value_pending) {
                            idx = f->entry_index;
                        } else {
                            idx = f->entry_index + 1;
                        }
                        if (idx >= bnode_count(f_btree)) continue;
                        bnode_entry_t* e = bnode_get(f_btree, idx);
                        if (e != NULL && e->key != NULL && !e->is_bnode_child) {
                            nchunks++;
                        }
                    }

                    chunk_t** chunks = NULL;
                    if (nchunks > 0) {
                        chunks = (chunk_t**)malloc(nchunks * sizeof(chunk_t*));
                        if (chunks == NULL) {
                            identifier_destroy(value);
                            return -2;
                        }
                        size_t idx = 0;
                        for (size_t i = 0; i < iter->stack_depth; i++) {
                            iterator_frame_t* f = &iter->stack[i];
                            bnode_t* f_btree = f->is_bnode_frame ? f->bnode
                                    : (f->node ? f->node->btree : NULL);
                            if (f_btree == NULL) continue;
                            size_t cidx;
                            if (i == iter->stack_depth - 1) {
                                cidx = this_index;
                            } else if (f->value_pending) {
                                cidx = f->entry_index;
                            } else {
                                cidx = f->entry_index + 1;
                            }
                            if (cidx >= bnode_count(f_btree)) continue;
                            bnode_entry_t* e = bnode_get(f_btree, cidx);
                            if (e != NULL && e->key != NULL && !e->is_bnode_child) {
                                chunks[idx++] = e->key;
                            }
                        }
                    }

                    path_t* result_path = path_create();
                    if (result_path == NULL) {
                        if (chunks) free(chunks);
                        identifier_destroy(value);
                        return -2;
                    }

                    /* Use path_meta from the leaf entry for exact byte lengths
                       (mirrors the forward scan). Without this, multi-chunk
                       keys whose length isn't a multiple of chunk_size (e.g.
                       "apple" = 5 bytes) would be null-padded and fail
                       exact-length comparisons. Falls back to the legacy
                       padded branch only when path_meta is absent. */
                    size_t num_identifiers = 0;
                    const path_subscript_meta_t* path_meta =
                        bnode_entry_get_path_meta(entry, &num_identifiers);

                    if (path_meta != NULL && num_identifiers > 0) {
                        size_t chunk_offset = 0;
                        size_t skip = iter->prefix_skip;
                        if (skip > num_identifiers) skip = num_identifiers;
                        for (size_t i = 0; i < skip; i++) {
                            chunk_offset += path_meta[i].chunk_count;
                        }
                        for (size_t i = skip; i < num_identifiers; i++) {
                            size_t id_chunks = path_meta[i].chunk_count;
                            size_t id_byte_length = path_meta[i].byte_length;
                            identifier_t* id = build_identifier_from_chunks(
                                chunks + chunk_offset, id_chunks, chunk_size, id_byte_length);
                            if (id == NULL) {
                                if (chunks) free(chunks);
                                path_destroy(result_path);
                                identifier_destroy(value);
                                return -2;
                            }
                            int rc = path_append(result_path, id);
                            identifier_destroy(id);
                            if (rc != 0) {
                                if (chunks) free(chunks);
                                path_destroy(result_path);
                                identifier_destroy(value);
                                return -2;
                            }
                            chunk_offset += id_chunks;
                        }
                    } else {
                        identifier_t* key_id = build_identifier_from_chunks(
                            chunks, nchunks, chunk_size, 0);
                        if (key_id == NULL) {
                            if (chunks) free(chunks);
                            path_destroy(result_path);
                            identifier_destroy(value);
                            return -2;
                        }
                        int rc = path_append(result_path, key_id);
                        identifier_destroy(key_id);
                        if (rc != 0) {
                            if (chunks) free(chunks);
                            path_destroy(result_path);
                            identifier_destroy(value);
                            return -2;
                        }
                    }

                    if (chunks) free(chunks);

                    /* Reverse-scan bounds check. Same [start, end) half-open
                       interval as forward, but STOP/SKIP meanings are swapped
                       for the descending walk:
                         -1 = STOP: path < start_path (walked past lower bound)
                          0 = SKIP: path >= end_path (at/above upper bound;
                                    shouldn't happen post-seek but guard)
                          1 = emit. */
                    int bounds = within_bounds_reverse(iter, result_path);
                    if (bounds < 0) {
                        path_destroy(result_path);
                        identifier_destroy(value);
                        iter->finished = 1;
                        return -1;
                    }
                    if (bounds == 0) {
                        path_destroy(result_path);
                        identifier_destroy(value);
                        continue;
                    }

                    *out_path = result_path;
                    *out_value = value;
                    return 0;
                }
                /* has_value but no visible value (MVCC tombstone) — skip.
                   entry_index already decremented; continue inner loop. */
            } else if (entry->is_bnode_child) {
                /* Multi-level B+tree internal node — push bnode frame and
                   position at its rightmost entry. Task 3's basic test (5
                   small keys) doesn't trigger multi-level B+trees; full
                   multi-level handling lands in Task 5. */
                if (entry->child_bnode == NULL && entry->child_disk_offset != 0
                    && iter->db->trie != NULL && iter->db->trie->fcache != NULL) {
                    bnode_entry_lazy_load_bnode_child(entry, iter->db->trie->fcache,
                                                      iter->db->trie->chunk_size,
                                                      iter->db->trie->btree_node_size);
                }
                if (entry->child_bnode == NULL) continue;
                pushed_child = 1;
                if (push_bnode_frame(iter, entry->child_bnode, iter->stack_depth - 1) < 0) {
                    return -2;
                }
                iterator_frame_t* child = &iter->stack[iter->stack_depth - 1];
                bnode_t* cb = child->bnode;
                child->entry_index = (bnode_count(cb) == 0) ? SIZE_MAX
                                                           : bnode_count(cb) - 1;
                break;
            } else if (entry->child != NULL || entry->child_disk_offset != 0) {
                /* Trie child (hbtrie_node_t*) — descend. entry_index already
                   decremented, so pop-back resumes at the previous entry. */
                if (entry->child == NULL && entry->child_disk_offset != 0
                    && iter->db->trie != NULL && iter->db->trie->fcache != NULL) {
                    bnode_entry_lazy_load_hbtrie_child(entry, iter->db->trie->fcache,
                                                       iter->db->trie->chunk_size,
                                                       iter->db->trie->btree_node_size);
                }
                if (entry->child == NULL) continue;
                pushed_child = 1;
                if (push_frame(iter, entry->child, iter->stack_depth - 1) < 0) {
                    return -2;
                }
                seek_to_rightmost(iter);
                break;
            } else if (entry->trie_child || entry->child_disk_offset != 0) {
                /* trie_child without has_value — descend. */
                if (entry->trie_child == NULL && entry->child_disk_offset != 0
                    && iter->db->trie != NULL && iter->db->trie->fcache != NULL) {
                    bnode_entry_lazy_load_trie_child(entry, iter->db->trie->fcache,
                                                     iter->db->trie->chunk_size,
                                                     iter->db->trie->btree_node_size);
                }
                if (entry->trie_child == NULL) continue;
                pushed_child = 1;
                if (push_frame(iter, entry->trie_child, iter->stack_depth - 1) < 0) {
                    return -2;
                }
                seek_to_rightmost(iter);
                break;
            }
            /* No value, no child — skip. entry_index already decremented. */
        }

        /* Frame exhausted (underflowed past 0) and no child pushed — pop. */
        if (!pushed_child && (frame->entry_index == SIZE_MAX || frame->entry_index >= count)) {
            pop_frame(iter);
        }
    }

    iter->finished = 1;
    return -1;
}
