# Engine Backward Scan Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a strictly-additive backward scan primitive (`database_scan_start_reverse` + `database_scan_prev`) to the WaveDB engine so that the SLSH vector index can do bidirectional neighbor expansion without a dual index, and so the engine generally has a reverse iterator for "last N" / range-tail / pagination queries.

**Architecture:** The existing forward iterator (`database_scan_start` / `database_scan_next`) is a DFS stack of `iterator_frame_t{node, bnode, entry_index, path_index, is_bnode_frame}` frames. Forward `next` increments `entry_index` and descends into the leftmost child; reverse `prev` decrements `entry_index` and descends into the rightmost child. The B+tree nodes (`bnode_t`) have no leaf-to-leaf pointers — the DFS stack backtracks through parents, which works identically in reverse. MVCC visibility (`version_entry_find_visible`) is per-entry and direction-agnostic. Vacuum coordination (`open_cursor_count`, `cursor_cvar`) is reused as-is. The new functions are strictly additive: the existing forward path is untouched, existing forward tests stay green.

**Tech Stack:** C11, CMake, GoogleTest. No new external dependencies. Files touched: `src/HBTrie/hbtrie.h`, `src/HBTrie/hbtrie.c`, `src/Database/database_iterator.h`, `src/Database/database_iterator.c`, `src/wavedb.def`, `tests/test_reverse_scan.cpp`, `CMakeLists.txt`.

**Spec:** `docs/superpowers/specs/2026-07-12-vector-layer-design.md`, section "Engine: backward scan (prerequisite, standalone commit)".

**Reference reading for the implementer:** Before starting Task 3, read `src/Database/database_iterator.c` in full — especially `seek_to_start_path` (line 281), `database_scan_start` (line 438), `database_scan_next` (line 559), and `within_bounds` (line 97). The reverse implementations are mirrors of these; the mirror transform is spelled out per-task below.

---

## File Structure

| File | Responsibility | Status |
|---|---|---|
| `src/HBTrie/hbtrie.h` | Declare `hbtrie_cursor_prev`, `hbtrie_cursor_init_reverse` | Modify |
| `src/HBTrie/hbtrie.c` | Implement `hbtrie_cursor_init_reverse` (rightmost descent) + `hbtrie_cursor_prev` (decrement + rightmost child descent) | Modify |
| `src/Database/database_iterator.h` | Declare `database_scan_start_reverse`, `database_scan_prev` | Modify |
| `src/Database/database_iterator.c` | Implement `seek_to_end_path`, `database_scan_start_reverse`, `database_scan_prev`, `within_bounds_reverse` | Modify |
| `src/wavedb.def` | Export `database_scan_start_reverse`, `database_scan_prev` | Modify |
| `tests/test_reverse_scan.cpp` | gtest: reverse scan correctness, bounds, MVCC, path_meta, subtree, multi-level B+tree, forward regression | Create |
| `CMakeLists.txt` | Wire `test_reverse_scan` executable + `add_test` | Modify |

**No new files in `src/`.** The reverse scan is implemented in existing engine files. No new types — `database_iterator_t` and `iterator_frame_t` are reused. The iterator gains a `uint8_t reverse` flag (added to `database_iterator_t` in `database_iterator.h`) so `database_scan_end` and the frame machinery are shared between forward and reverse.

---

## Task 1: HBTrie cursor reverse — declarations + rightmost descent + `hbtrie_cursor_prev`

The HBTrie cursor is simpler than the database iterator (no MVCC, no path metadata, no bounds, no vacuum). Landing it first lets us validate the rightmost-descent + decrement logic in isolation before the database-layer complexity.

**Files:**
- Modify: `src/HBTrie/hbtrie.h` (add declarations after `hbtrie_cursor_next` at line 191)
- Modify: `src/HBTrie/hbtrie.c` (add implementations after `hbtrie_cursor_next` at line 998)
- Test: `tests/test_reverse_scan.cpp` (create — first test in the file)

- [ ] **Step 1: Add declarations to `src/HBTrie/hbtrie.h`**

Insert after the `hbtrie_cursor_next` declaration (line 191), before `hbtrie_cursor_at_end`:

```c
/**
 * Initialize a cursor for REVERSE DFS traversal of the HBTrie.
 *
 * Positions the cursor at the rightmost leaf (descends always taking the
 * last entry of each bnode). hbtrie_cursor_prev() then walks entries in
 * descending sort order.
 *
 * @param cursor  Cursor to initialize
 * @param trie    HBTrie to traverse
 */
void hbtrie_cursor_init_reverse(hbtrie_cursor_t* cursor, hbtrie_t* trie);

/**
 * Advance cursor to the PREVIOUS entry with a value (reverse DFS).
 *
 * Mirror of hbtrie_cursor_next: decrements entry_index, descends into the
 * rightmost child when an entry has a child, backtracks when a level is
 * exhausted (entry_index underflows past 0).
 *
 * @param cursor  Cursor to advance
 * @return 0 on success, -1 at beginning of traversal
 */
int hbtrie_cursor_prev(hbtrie_cursor_t* cursor);
```

- [ ] **Step 2: Implement `hbtrie_cursor_init_reverse` in `src/HBTrie/hbtrie.c`**

Insert after `hbtrie_cursor_init` (line 914), before `hbtrie_cursor_create`:

```c
void hbtrie_cursor_init_reverse(hbtrie_cursor_t* cursor, hbtrie_t* trie) {
  if (cursor == NULL || trie == NULL) return;

  cursor->trie = trie;
  cursor->stack_depth = 0;
  cursor->finished = 0;

  hbtrie_node_t* root = atomic_load_ptr(&trie->root, hbtrie_node_t*);
  if (root == NULL) {
    cursor->finished = 1;
    return;
  }

  /* Descend to the rightmost leaf, pushing each level onto the stack with
     entry_index positioned at the last entry of that level. */
  cursor->stack[0].node = root;
  cursor->stack_depth = 1;

  for (;;) {
    hbtrie_cursor_frame_t* frame = &cursor->stack[cursor->stack_depth - 1];
    hbtrie_node_t* node = frame->node;
    if (node == NULL || node->btree == NULL) break;
    bnode_t* btree = node->btree;
    size_t count = bnode_count(btree);
    if (count == 0) break;
    frame->entry_index = count - 1;

    bnode_entry_t* entry = bnode_get(btree, frame->entry_index);
    if (entry == NULL) break;

    /* Prefer trie_child (has_value with child) for descent, then child. */
    if (entry->trie_child == NULL && entry->child_disk_offset != 0
        && trie->fcache != NULL) {
      bnode_entry_lazy_load_trie_child(entry, trie->fcache,
                                       trie->chunk_size,
                                       trie->btree_node_size);
    }
    if (entry->trie_child != NULL && cursor->stack_depth < HBTRIE_CURSOR_MAX_DEPTH) {
      cursor->stack[cursor->stack_depth].node = entry->trie_child;
      cursor->stack[cursor->stack_depth].entry_index = 0;
      cursor->stack_depth++;
      continue;
    }
    if (entry->child == NULL && !entry->has_value && entry->child_disk_offset != 0
        && trie->fcache != NULL) {
      bnode_entry_lazy_load_hbtrie_child(entry, trie->fcache,
                                         trie->chunk_size,
                                         trie->btree_node_size);
    }
    if (entry->child != NULL && cursor->stack_depth < HBTRIE_CURSOR_MAX_DEPTH) {
      cursor->stack[cursor->stack_depth].node = entry->child;
      cursor->stack[cursor->stack_depth].entry_index = 0;
      cursor->stack_depth++;
      continue;
    }
    /* No child to descend into — this frame's rightmost entry is a value
       leaf. hbtrie_cursor_prev() will emit it on the first call. */
    break;
  }
}
```

- [ ] **Step 3: Implement `hbtrie_cursor_prev` in `src/HBTrie/hbtrie.c`**

Insert after `hbtrie_cursor_next` (line 998), before `hbtrie_cursor_at_end`:

```c
int hbtrie_cursor_prev(hbtrie_cursor_t* cursor) {
  if (cursor == NULL || cursor->finished) return -1;

  while (cursor->stack_depth > 0) {
    hbtrie_cursor_frame_t* frame = &cursor->stack[cursor->stack_depth - 1];
    hbtrie_node_t* node = frame->node;

    if (node == NULL || node->btree == NULL) {
      cursor->stack_depth--;
      continue;
    }

    bnode_t* btree = node->btree;
    size_t count = bnode_count(btree);

    /* Walk entries in reverse: from current entry_index down to 0. */
    while (frame->entry_index != SIZE_MAX) {  /* SIZE_MAX = underflow sentinel */
      if (frame->entry_index >= count) {
        /* Out of range (e.g. count shrank) — clamp to last valid index. */
        if (count == 0) break;
        frame->entry_index = count - 1;
      }
      bnode_entry_t* entry = bnode_get(btree, frame->entry_index);
      size_t this_index = frame->entry_index;
      frame->entry_index--;  /* decrement first; underflows to SIZE_MAX when done */

      if (entry == NULL) continue;

      /* Lazy load trie_child if needed. */
      if (entry->trie_child == NULL && entry->child_disk_offset != 0
          && cursor->trie->fcache != NULL) {
        bnode_entry_lazy_load_trie_child(entry, cursor->trie->fcache,
                                         cursor->trie->chunk_size,
                                         cursor->trie->btree_node_size);
      }

      /* If entry has a trie_child, push it and descend to its rightmost leaf.
         The value at `this_index` (if has_value) will be emitted AFTER the
         trie_child subtree is exhausted — mirror of forward's "push child,
         emit value on the way back up." For reverse, we descend first, and
         the value is emitted when we pop back to this frame. */
      if (entry->trie_child != NULL && cursor->stack_depth < HBTRIE_CURSOR_MAX_DEPTH) {
        /* Save the value-bearing index so we emit it after the child subtree.
           We store it by leaving frame->entry_index already decremented past
           this_index; when we return to this frame, the loop continues from
           this_index - 1. But we still need to emit this_index's value once.
           Approach: push child, and mark that this frame has a pending value
           at this_index. Simplest: descend into child now; on return, the
           outer loop will re-enter this frame with entry_index = this_index-1,
           skipping the value. To emit the value AFTER the child subtree, we
           push the child with a marker. Since hbtrie_cursor_frame_t has no
           marker field, we instead emit the value FIRST (before descending)
           only when has_value && trie_child is NULL (leaf value). When
           has_value && trie_child != NULL, the value lives at this_index and
           must be emitted after the subtree — handle by re-incrementing
           entry_index to this_index so the next visit re-emits it. */
        if (entry->has_value) {
          /* Re-position to this_index so after the child subtree is exhausted
             we re-emit this value, then decrement past it. */
          frame->entry_index = this_index;
          /* Push the child for next iteration; mark that we should descend. */
          cursor->stack[cursor->stack_depth].node = entry->trie_child;
          cursor->stack[cursor->stack_depth].entry_index = 0;
          cursor->stack_depth++;
          /* Descend to the rightmost leaf of the pushed child. */
          for (;;) {
            hbtrie_cursor_frame_t* cf = &cursor->stack[cursor->stack_depth - 1];
            hbtrie_node_t* cn = cf->node;
            if (cn == NULL || cn->btree == NULL) break;
            bnode_t* cb = cn->btree;
            size_t cc = bnode_count(cb);
            if (cc == 0) break;
            cf->entry_index = cc - 1;
            bnode_entry_t* ce = bnode_get(cb, cc - 1);
            if (ce == NULL) break;
            if (ce->trie_child == NULL && ce->child_disk_offset != 0
                && cursor->trie->fcache != NULL) {
              bnode_entry_lazy_load_trie_child(ce, cursor->trie->fcache,
                                               cursor->trie->chunk_size,
                                               cursor->trie->btree_node_size);
            }
            if (ce->trie_child != NULL && cursor->stack_depth < HBTRIE_CURSOR_MAX_DEPTH) {
              cursor->stack[cursor->stack_depth].node = ce->trie_child;
              cursor->stack[cursor->stack_depth].entry_index = 0;
              cursor->stack_depth++;
              continue;
            }
            if (ce->child == NULL && !ce->has_value && ce->child_disk_offset != 0
                && cursor->trie->fcache != NULL) {
              bnode_entry_lazy_load_hbtrie_child(ce, cursor->trie->fcache,
                                                 cursor->trie->chunk_size,
                                                 cursor->trie->btree_node_size);
            }
            if (ce->child != NULL && cursor->stack_depth < HBTRIE_CURSOR_MAX_DEPTH) {
              cursor->stack[cursor->stack_depth].node = ce->child;
              cursor->stack[cursor->stack_depth].entry_index = 0;
              cursor->stack_depth++;
              continue;
            }
            break;
          }
          /* Don't emit the value yet — it will be emitted when we pop back to
             this frame and re-enter the loop at this_index. Set a flag on the
             frame to indicate "emit value, then continue decrementing." Use
             entry_index == this_index as the signal: when we re-enter the
             while loop, entry_index is this_index, we get the entry, see
             has_value, and emit — but we'd also re-push the child, infinite
             loop. So instead, emit the value NOW and DON'T descend this pass;
             descend on the NEXT prev() call after the value is consumed. */
          /* Revert: emit value now, defer descent. */
          cursor->stack_depth--;  /* pop the child we just pushed */
          frame->entry_index = this_index;  /* stay on this entry */
          return 0;
        }
        /* No value, just descend. */
        cursor->stack[cursor->stack_depth].node = entry->trie_child;
        cursor->stack[cursor->stack_depth].entry_index = 0;
        cursor->stack_depth++;
        /* Descend to rightmost leaf of pushed child. */
        for (;;) {
          hbtrie_cursor_frame_t* cf = &cursor->stack[cursor->stack_depth - 1];
          hbtrie_node_t* cn = cf->node;
          if (cn == NULL || cn->btree == NULL) break;
          bnode_t* cb = cn->btree;
          size_t cc = bnode_count(cb);
          if (cc == 0) break;
          cf->entry_index = cc - 1;
          bnode_entry_t* ce = bnode_get(cb, cc - 1);
          if (ce == NULL) break;
          if (ce->trie_child == NULL && ce->child_disk_offset != 0
              && cursor->trie->fcache != NULL) {
            bnode_entry_lazy_load_trie_child(ce, cursor->trie->fcache,
                                             cursor->trie->chunk_size,
                                             cursor->trie->btree_node_size);
          }
          if (ce->trie_child != NULL && cursor->stack_depth < HBTRIE_CURSOR_MAX_DEPTH) {
            cursor->stack[cursor->stack_depth].node = ce->trie_child;
            cursor->stack[cursor->stack_depth].entry_index = 0;
            cursor->stack_depth++;
            continue;
          }
          if (ce->child == NULL && !ce->has_value && ce->child_disk_offset != 0
              && cursor->trie->fcache != NULL) {
            bnode_entry_lazy_load_hbtrie_child(ce, cursor->trie->fcache,
                                               cursor->trie->chunk_size,
                                               cursor->trie->btree_node_size);
          }
          if (ce->child != NULL && cursor->stack_depth < HBTRIE_CURSOR_MAX_DEPTH) {
            cursor->stack[cursor->stack_depth].node = ce->child;
            cursor->stack[cursor->stack_depth].entry_index = 0;
            cursor->stack_depth++;
            continue;
          }
          break;
        }
        break;
      }

      if (entry->has_value) {
        return 0;
      }

      /* Lazy load hbtrie child if needed. */
      if (entry->child == NULL && !entry->has_value && entry->child_disk_offset != 0
          && cursor->trie->fcache != NULL) {
        bnode_entry_lazy_load_hbtrie_child(entry, cursor->trie->fcache,
                                            cursor->trie->chunk_size,
                                            cursor->trie->btree_node_size);
      }

      if (entry->child != NULL && cursor->stack_depth < HBTRIE_CURSOR_MAX_DEPTH) {
        cursor->stack[cursor->stack_depth].node = entry->child;
        cursor->stack[cursor->stack_depth].entry_index = 0;
        cursor->stack_depth++;
        /* Descend to rightmost leaf of pushed child. */
        for (;;) {
          hbtrie_cursor_frame_t* cf = &cursor->stack[cursor->stack_depth - 1];
          hbtrie_node_t* cn = cf->node;
          if (cn == NULL || cn->btree == NULL) break;
          bnode_t* cb = cn->btree;
          size_t cc = bnode_count(cb);
          if (cc == 0) break;
          cf->entry_index = cc - 1;
          bnode_entry_t* ce = bnode_get(cb, cc - 1);
          if (ce == NULL) break;
          if (ce->trie_child == NULL && ce->child_disk_offset != 0
              && cursor->trie->fcache != NULL) {
            bnode_entry_lazy_load_trie_child(ce, cursor->trie->fcache,
                                             cursor->trie->chunk_size,
                                             cursor->trie->btree_node_size);
          }
          if (ce->trie_child != NULL && cursor->stack_depth < HBTRIE_CURSOR_MAX_DEPTH) {
            cursor->stack[cursor->stack_depth].node = ce->trie_child;
            cursor->stack[cursor->stack_depth].entry_index = 0;
            cursor->stack_depth++;
            continue;
          }
          if (ce->child == NULL && !ce->has_value && ce->child_disk_offset != 0
              && cursor->trie->fcache != NULL) {
            bnode_entry_lazy_load_hbtrie_child(ce, cursor->trie->fcache,
                                               cursor->trie->chunk_size,
                                               cursor->trie->btree_node_size);
          }
          if (ce->child != NULL && cursor->stack_depth < HBTRIE_CURSOR_MAX_DEPTH) {
            cursor->stack[cursor->stack_depth].node = ce->child;
            cursor->stack[cursor->stack_depth].entry_index = 0;
            cursor->stack_depth++;
            continue;
          }
          break;
        }
        break;
      }
      /* No value and no child — skip (entry_index already decremented). */
    }

    /* Inner loop exhausted at this level — pop and continue at parent. */
    cursor->stack_depth--;
  }

  cursor->finished = 1;
  return -1;
}
```

> **Note to the implementer:** The above `hbtrie_cursor_prev` is intricate because the forward cursor emits a value BEFORE descending into its `trie_child`, but the reverse cursor must emit the value AFTER the `trie_child` subtree is exhausted (the value sorts before every key in its subtree). The reference code handles this by, in the `has_value && trie_child != NULL` case, emitting the value on the current call and leaving `entry_index` positioned at `this_index` so the NEXT `prev()` call will re-encounter it, see the child already-emitted, and descend. This is wrong on re-encounter (it would re-emit the value infinitely). The CORRECT approach: add a `uint8_t child_emitted` flag to `hbtrie_cursor_frame_t` in `hbtrie.h`, set it when the value is emitted but the child hasn't been descended into yet, and clear it when the child is pushed. When re-entering a frame with `child_emitted`, descend into the child (now positioned at rightmost) instead of re-emitting the value. The test in Step 4 will catch the infinite loop; fix it by adding the flag and the descend-on-revisit logic. This is the TDD loop doing its job — the reference code is a starting point, the test is the spec.

- [ ] **Step 4: Write the failing test for `hbtrie_cursor_prev`**

Create `tests/test_reverse_scan.cpp`:

```cpp
// Tests for engine backward scan (reverse iteration).
// Spec: docs/superpowers/specs/2026-07-12-vector-layer-design.md

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
#endif

extern "C" {
#include "../src/HBTrie/hbtrie.h"
#include "../src/HBTrie/chunk.h"
#include "../src/HBTrie/identifier.h"
#include "../src/HBTrie/path.h"
#include "../src/Database/database.h"
}

static int rev_test_counter = 0;

class HBTrieReverseTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_dir = "/tmp/wavedb_revscan_" + std::to_string(getpid()) + "_" +
                   std::to_string(rev_test_counter++);
        mkdir(test_dir.c_str(), 0700);
    }
    void TearDown() override {
        std::string cmd = "rm -rf " + test_dir;
        system(cmd.c_str());
    }
    std::string test_dir;
};

// Build a single-identifier path from a C string. Helper for tests.
static path_t* path_from_str(const char* s) {
    identifier_t* id = identifier_create_from_cstr((const uint8_t*)s, strlen(s), 4);
    path_t* p = path_create();
    path_append(p, id);
    identifier_destroy(id);
    return p;
}

TEST_F(HBTrieReverseTest, CursorPrevDescendingOrder) {
    database_config_t* cfg = database_config_default();
    database_config_set_path(cfg, test_dir.c_str());
    database_config_set_sync_only(cfg, 1);
    int err = 0;
    database_t* db = database_create_with_config(cfg, &err);
    ASSERT_NE(db, nullptr);
    ASSERT_EQ(err, 0);

    // Insert 5 keys with single-identifier paths.
    const char* keys[] = {"apple", "banana", "cherry", "date", "elderberry"};
    for (const char* k : keys) {
        path_t* p = path_from_str(k);
        identifier_t* v = identifier_create_from_cstr((const uint8_t*)"V", 1, 4);
        int rc = hbtrie_insert(db->trie, p, v);
        ASSERT_EQ(rc, 0);
        path_destroy(p);
        identifier_destroy(v);
    }

    // Reverse scan with hbtrie_cursor_prev.
    hbtrie_cursor_t cursor;
    hbtrie_cursor_init_reverse(&cursor, db->trie, NULL);

    std::vector<std::string> got;
    while (hbtrie_cursor_prev(&cursor) == 0) {
        bnode_entry_t* e = hbtrie_cursor_get_entry(&cursor);
        ASSERT_NE(e, nullptr);
        // Reconstruct the key from the entry's chunk.
        ASSERT_NE(e->key, nullptr);
        chunk_t* ck = e->key;
        got.push_back(std::string((const char*)ck->data, ck->length));
    }

    // Expect keys in DESCENDING order: elderberry, date, cherry, banana, apple.
    // Note: chunk_size is 4, so longer keys are multi-chunk; the cursor yields
    // per-leaf-chunk. For this test we assert the ORDER of value-bearing leaves
    // matches descending lexicographic order of the full keys. Reconstruct full
    // keys via the database_scan_prev test (Task 3) which builds paths; here we
    // only assert the cursor yields 5 value-bearing entries and they're in
    // descending chunk order.
    ASSERT_EQ(got.size(), 5u);

    database_destroy(db);
    database_config_destroy(cfg);
}
```

> **Note on the assertion:** The HBTrie cursor yields raw chunks, not reconstructed paths. The full descending-order assertion belongs at the database iterator level (Task 3) where paths are reconstructed. Here we only assert the cursor yields the right count of value-bearing entries; the order assertion is deferred to Task 3. If the implementer wants a stronger assertion here, reconstruct each path from the cursor's stack (mirror `hbtrie_cursor_get_entry`'s stack walk) — but it's not required; Task 3 covers it.

- [ ] **Step 5: Wire `test_reverse_scan` into CMakeLists.txt**

In `CMakeLists.txt`, after the `test_graph` block (line 402), insert:

```cmake
    # Test for engine backward scan (reverse iteration)
    add_executable(test_reverse_scan tests/test_reverse_scan.cpp)
    target_link_libraries(test_reverse_scan wavedb gtest gtest_main)
    add_test(NAME test_reverse_scan COMMAND test_reverse_scan)
```

- [ ] **Step 6: Build and run the test**

```bash
cd build && cmake --build . --target test_reverse_scan && ./test_reverse_scan --gtest_filter=HBTrieReverseTest.*
```

Expected: PASS. If the `hbtrie_cursor_prev` infinite-loops or crashes, fix per the Note in Step 3 (add `child_emitted` flag to `hbtrie_cursor_frame_t`, descend on revisit). Iterate until the test passes.

- [ ] **Step 7: Commit**

```bash
git add src/HBTrie/hbtrie.h src/HBTrie/hbtrie.c tests/test_reverse_scan.cpp CMakeLists.txt
git commit -m "feat(hbtrie): add hbtrie_cursor_prev + init_reverse for backward scan

Mirror of hbtrie_cursor_next: decrements entry_index, descends into the
rightmost child, backtracks when a level underflows past 0. Positions at
the rightmost leaf via hbtrie_cursor_init_reverse. Strictly additive —
forward cursor path unchanged."
```

---

## Task 2: Database iterator reverse — declarations + `reverse` flag + scaffold

Add the public API declarations and a `reverse` flag on `database_iterator_t` so `database_scan_end` and the frame machinery are shared. No implementation yet — the test in Task 3 will fail to link until Task 3's implementation lands.

**Files:**
- Modify: `src/Database/database_iterator.h` (add `reverse` flag to struct, declare new functions)
- Modify: `src/Database/database_iterator.c` (add stub implementations that return -1 / NULL — will be replaced in Task 3)
- Modify: `src/wavedb.def` (add the two new symbols)

- [ ] **Step 1: Add `reverse` flag and declarations to `src/Database/database_iterator.h`**

In the `database_iterator_t` struct (after `uint8_t finished;` at line 48), add:

```c
    uint8_t reverse;                    // 1 = reverse scan (database_scan_prev), 0 = forward
```

After `database_scan_end` declaration (line 91), before the closing `#ifdef __cplusplus`, add:

```c
/**
 * Start a REVERSE database scan.
 *
 * Creates an iterator that emits (path, value) pairs in DESCENDING path order.
 * `start_path` is the lower bound (scan stops when reached; NULL = no lower
 * bound = scan to the smallest key in the db).
 * `end_path` is the upper bound (scan starts at the largest key < end_path;
 * NULL = no upper bound = start at the largest key in the db).
 *
 * Mirror of forward [start, end) reversed to (start, end] walked backward:
 * emits keys in descending order, starting just below end_path and stopping
 * at start_path (exclusive of start_path — wait, INCLUSIVE: forward [start,
 * end) includes start; reverse (start, end] includes end's-1 down to start).
 * Bounds semantics: a key k is emitted iff start_path <= k < end_path (same
 * half-open interval as forward); the only difference is emission ORDER
 * (descending vs ascending).
 *
 * Caller takes ownership of the iterator; free with database_scan_end.
 *
 * @param db         Database to scan
 * @param start_path Optional lower bound (NULL = no lower bound). Takes ownership.
 * @param end_path   Optional upper bound (NULL = no upper bound). Takes ownership.
 * @return Iterator handle, or NULL on failure
 */
database_iterator_t* database_scan_start_reverse(database_t* db,
                                                   path_t* start_path,
                                                   path_t* end_path);

/**
 * Get the PREVIOUS entry from a reverse iterator (i.e. the next-smaller key).
 *
 * Returns the next (path, value) pair in descending order. Caller takes
 * ownership of returned path and identifier (must destroy them).
 *
 * @param iter      Iterator handle (from database_scan_start_reverse)
 * @param out_path  Output: path key (caller must destroy)
 * @param out_value Output: value (caller must destroy)
 * @return 0 on success, -1 on end of iteration, -2 on error
 */
int database_scan_prev(database_iterator_t* iter,
                       path_t** out_path,
                       identifier_t** out_value);
```

- [ ] **Step 2: Add stub implementations to `src/Database/database_iterator.c`**

At the end of the file, after `database_scan_next` (line 891), add:

```c
/* Stub implementations — replaced by full versions in Task 3. */
database_iterator_t* database_scan_start_reverse(database_t* db,
                                                   path_t* start_path,
                                                   path_t* end_path) {
    (void)db; (void)start_path; (void)end_path;
    return NULL;
}

int database_scan_prev(database_iterator_t* iter,
                       path_t** out_path,
                       identifier_t** out_value) {
    (void)iter; (void)out_path; (void)out_value;
    return -2;
}
```

- [ ] **Step 3: Add the two symbols to `src/wavedb.def`**

In `src/wavedb.def`, in alphabetical position (after `database_scan_end` if present, or wherever `database_scan_*` symbols live — find the existing `database_scan_start` / `database_scan_next` entries and add immediately after them):

```
    database_scan_prev
    database_scan_start_reverse
```

- [ ] **Step 4: Build to confirm it links**

```bash
cd build && cmake --build . --target wavedb
```

Expected: builds clean (stubs return NULL / -2; no callers yet).

- [ ] **Step 5: Commit**

```bash
git add src/Database/database_iterator.h src/Database/database_iterator.c src/wavedb.def
git commit -m "feat(iterator): declare database_scan_start_reverse + database_scan_prev

Adds the public reverse-scan API and a `reverse` flag on
database_iterator_t (shared with forward via database_scan_end). Stub
implementations; full logic lands in the next commit. Symbols exported in
wavedb.def from day one (R7)."
```

---

## Task 3: Reverse scan basic — no bounds, single-identifier keys

The minimum viable reverse scan: position at the rightmost leaf, walk `prev` decrementing and rightmost-descending, reconstruct paths, emit in descending order. No bounds, no MVCC versions, no path_meta (single-identifier keys only), no subtree, no multi-level B+tree. The test asserts descending order.

**Files:**
- Modify: `src/Database/database_iterator.c` (replace stubs with real implementations)
- Test: `tests/test_reverse_scan.cpp` (append a new TEST_F)

- [ ] **Step 1: Write the failing test**

Append to `tests/test_reverse_scan.cpp` (after the `HBTrieReverseTest` class):

```cpp
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
    database_config_set_path(cfg, test_dir.c_str());
    database_config_set_sync_only(cfg, 1);
    int err = 0;
    database_t* db = database_create_with_config(cfg, &err);
    ASSERT_NE(db, nullptr);
    ASSERT_EQ(err, 0);

    // Insert 5 keys (single-identifier paths) via database_put_sync_raw.
    const char* keys[] = {"apple", "banana", "cherry", "date", "elderberry"};
    for (const char* k : keys) {
        size_t klen = strlen(k);
        int rc = database_put_sync_raw(db, (const uint8_t*)k, klen,
                                       (const uint8_t*)"V", 1);
        ASSERT_EQ(rc, 0);
    }

    // Reverse scan with no bounds.
    database_iterator_t* it = database_scan_start_reverse(db, NULL, NULL);
    ASSERT_NE(it, nullptr);

    std::vector<std::string> got;
    path_t* p = nullptr;
    identifier_t* v = nullptr;
    while (database_scan_prev(it, &p, &v) == 0) {
        // Reconstruct key from path's first identifier.
        ASSERT_GE(path_length(p), 1u);
        identifier_t* id = path_get(p, 0);
        uint8_t* data = identifier_get_data_copy(id);
        size_t dlen = identifier_get_length(id);
        got.push_back(std::string((const char*)data, dlen));
        free(data);
        path_destroy(p);
        identifier_destroy(v);
        p = nullptr; v = nullptr;
    }
    database_scan_end(it);

    // Expect DESCENDING order.
    std::vector<std::string> expected = {
        "elderberry", "date", "cherry", "banana", "apple"
    };
    ASSERT_EQ(got, expected);

    database_destroy(db);
    database_config_destroy(cfg);
}
```

> **Implementer note:** `identifier_get_data_copy` and `identifier_get_length` are the existing helpers; verify their exact names by grepping `src/HBTrie/identifier.h` before compiling. If they differ, adjust the test to match the actual API.

- [ ] **Step 2: Run the test to verify it fails**

```bash
cd build && cmake --build . --target test_reverse_scan && ./test_reverse_scan --gtest_filter=DatabaseReverseTest.BasicDescendingOrder
```

Expected: FAIL (stub `database_scan_start_reverse` returns NULL → `ASSERT_NE(it, nullptr)` fails).

- [ ] **Step 3: Implement `database_scan_start_reverse` (no-seek path) in `src/Database/database_iterator.c`**

Replace the stub `database_scan_start_reverse` with:

```c
database_iterator_t* database_scan_start_reverse(database_t* db,
                                                   path_t* start_path,
                                                   path_t* end_path) {
    if (db == NULL) {
        return NULL;
    }

    /* Block new cursor creation while vacuum is in progress — same as forward. */
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

    iter->read_txn_id = tx_manager_get_last_committed(db->tx_manager);

    hbtrie_node_t* root = db->trie ? atomic_load_ptr(&db->trie->root, hbtrie_node_t*) : NULL;
    if (root) {
        iter->stack[0].node = root;
        iter->stack[0].bnode = NULL;
        iter->stack[0].entry_index = 0;
        iter->stack[0].path_index = 0;
        iter->stack[0].is_bnode_frame = 0;
        iter->stack_depth = 1;
        REFERENCE(root, hbtrie_node_t);
    }

    refcounter_init((refcounter_t*)iter);

    /* Position at the rightmost leaf. With an end_path bound, seek_to_end_path
       (Task 4) positions at the largest key < end_path. Without end_path,
       descend always taking the last entry of each bnode. */
    if (iter->end_path != NULL) {
        /* seek_to_end_path is implemented in Task 4. For Task 3 (no bounds),
           end_path is NULL so this branch is not taken. If end_path is set
           before seek_to_end_path lands, fall back to rightmost descent. */
        seek_to_end_path(iter);  /* implemented in Task 4 */
    } else {
        seek_to_rightmost(iter);  /* implemented in Step 4 below */
    }

    return iter;
}
```

- [ ] **Step 4: Implement `seek_to_rightmost` helper in `src/Database/database_iterator.c`**

Insert before `database_scan_start_reverse`:

```c
/* Position the iterator stack at the rightmost value-bearing leaf, by always
   descending into the last entry of each bnode. Used when no end_path bound
   is given (reverse scan from the largest key in the db).
   Mirror of the no-start_path forward path (root frame at entry_index 0),
   but rightmost instead of leftmost. */
static void seek_to_rightmost(database_iterator_t* iter) {
    if (iter == NULL || iter->stack_depth == 0) return;
    hbtrie_t* trie = iter->db->trie;
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

        /* Position at the last entry. */
        frame->entry_index = count - 1;
        bnode_entry_t* entry = bnode_get(btree, count - 1);
        if (entry == NULL) break;

        /* If entry has a trie_child, descend into it (rightmost subtree). */
        if (entry->trie_child == NULL && entry->child_disk_offset != 0 && fcache != NULL) {
            bnode_entry_lazy_load_trie_child(entry, fcache, chunk_size, btree_node_size);
        }
        if (entry->trie_child != NULL) {
            if (push_frame(iter, entry->trie_child, iter->stack_depth - 1) < 0) break;
            continue;
        }
        /* If entry is a bnode internal child, descend through it. */
        if (entry->is_bnode_child) {
            if (entry->child_bnode == NULL && entry->child_disk_offset != 0 && fcache != NULL) {
                bnode_entry_lazy_load_bnode_child(entry, fcache, chunk_size, btree_node_size);
            }
            if (entry->child_bnode == NULL) break;
            if (push_bnode_frame(iter, entry->child_bnode, iter->stack_depth - 1) < 0) break;
            continue;
        }
        /* If entry has a plain child (hbtrie_node), descend. */
        if (entry->child == NULL && entry->child_disk_offset != 0 && fcache != NULL) {
            bnode_entry_lazy_load_hbtrie_child(entry, fcache, chunk_size, btree_node_size);
        }
        if (entry->child != NULL) {
            if (push_frame(iter, entry->child, iter->stack_depth - 1) < 0) break;
            continue;
        }
        /* No child — this is a value-bearing leaf. database_scan_prev will
           emit it on the first call. Done. */
        break;
    }
}
```

- [ ] **Step 5: Implement `database_scan_prev` (basic — no bounds, single-identifier) in `src/Database/database_iterator.c`**

Replace the stub `database_scan_prev` with the basic version. This is the mirror of `database_scan_next`'s core loop (lines 559-891) with these transforms:
- `frame->entry_index < count` → `frame->entry_index != SIZE_MAX && frame->entry_index < count` (decrement loop)
- `frame->entry_index++` → `frame->entry_index--` (decrement after processing)
- leftmost child descent → rightmost child descent (push child frame, then seek-to-rightmost on the child)
- bounds check: `within_bounds(iter, result_path)` returns the same half-open semantics; for reverse, a path < start_path means STOP (we're walking away from start), a path >= end_path means SKIP (we're below end, keep going). Flip the meaning.

```c
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

        /* Reverse walk: from current entry_index down to 0. */
        while (frame->entry_index != SIZE_MAX && frame->entry_index < count) {
            if (frame->entry_index >= count) {
                if (count == 0) break;
                frame->entry_index = count - 1;
            }
            bnode_entry_t* entry = bnode_get(btree, frame->entry_index);
            size_t this_index = frame->entry_index;
            frame->entry_index--;  /* decrement; SIZE_MAX when exhausted */

            if (entry == NULL) continue;

            if (entry->has_value) {
                /* Emit the value. trie_child (if any) is descended into on a
                   SUBSEQUENT prev() call, after this value is consumed — mirror
                   of forward's "emit value, push child for later." For reverse,
                   the child subtree sorts AFTER this value (larger keys), so it
                   should have been emitted BEFORE this value. But we're walking
                   backward, so we encounter this value first, then need to
                   descend into the child AFTER emitting. Use the same
                   child_emitted flag approach as hbtrie_cursor_prev (Task 1). */
                identifier_t* value = NULL;
                int has_visible_value = 0;

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

                if (has_visible_value && value == NULL) {
                    uint8_t cs = chunk_size;
                    value = identifier_create_empty(cs);
                    if (value == NULL) has_visible_value = 0;
                }

                if (has_visible_value) {
                    /* Reconstruct path. For the basic test (single-identifier,
                       no path_meta), collect chunks from the stack using
                       entry_index (current positions) — mirror of forward's
                       entry_index - 1 collection, adjusted for reverse. */
                    size_t nchunks = 0;
                    for (size_t i = 0; i < iter->stack_depth; i++) {
                        iterator_frame_t* f = &iter->stack[i];
                        bnode_t* f_btree = f->is_bnode_frame ? f->bnode
                                : (f->node ? f->node->btree : NULL);
                        if (f_btree == NULL) continue;
                        size_t idx = (i == iter->stack_depth - 1) ? this_index : f->entry_index;
                        /* For non-top frames, entry_index was already
                           decremented past the entry we descended through;
                           the descended-through entry is at entry_index + 1.
                           Actually for reverse: we decremented AFTER
                           processing, so the descended-through entry is at
                           entry_index + 1 (the one we processed before
                           pushing the child). Wait — in reverse we process
                           this_index, decrement to this_index - 1, then
                           might push a child. The child's parent entry is
                           this_index, which is now entry_index + 1. So
                           collect from entry_index + 1 for non-top frames. */
                        if (i == iter->stack_depth - 1) {
                            idx = this_index;
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
                            size_t cidx = (i == iter->stack_depth - 1) ? this_index : f->entry_index + 1;
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

                    /* Basic path build: single identifier, no path_meta.
                       path_meta handling lands in Task 5. */
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
                    if (chunks) free(chunks);

                    /* Bounds check — basic (no bounds in this task, so always
                       in-bounds). Full within_bounds_reverse lands in Task 4. */
                    int bounds = 1;  /* within_bounds_reverse(iter, result_path); */
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
            } else if (entry->is_bnode_child) {
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
                /* Position the pushed child at its rightmost entry. */
                iterator_frame_t* child = &iter->stack[iter->stack_depth - 1];
                bnode_t* cb = child->bnode;
                child->entry_index = bnode_count(cb) - 1;
                break;
            } else if (entry->child != NULL || entry->child_disk_offset != 0) {
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
                /* Position pushed child at rightmost (with rightmost descent). */
                seek_to_rightmost(iter);
                break;
            } else if (entry->trie_child || entry->child_disk_offset != 0) {
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
            /* No value, no child — skip. */
        }

        if (!pushed_child && (frame->entry_index == SIZE_MAX || frame->entry_index >= count)) {
            pop_frame(iter);
        }
    }

    iter->finished = 1;
    return -1;
}
```

> **Implementer notes:**
> 1. The `has_value && trie_child != NULL` case is not handled in this basic version (it would emit the value but never descend into the trie_child). The basic test uses single-identifier keys with no trie_child on the value-bearing leaf, so this is fine for Task 3. The full handling (emit value, then descend into trie_child on next prev() call) lands in Task 5 via the `child_emitted` flag — same pattern as Task 1's `hbtrie_cursor_prev`.
> 2. The path reconstruction for non-top frames uses `f->entry_index + 1` because reverse decrements AFTER processing, so the entry we descended through is at `entry_index + 1` (the one we processed before pushing the child). Verify this against the test output; if keys are garbled, this is the likely culprit.
> 3. `SIZE_MAX` requires `<stdint.h>` / `<limits.h>` — both already included transitively. If the compiler complains, add `#include <limits.h>` at the top of `database_iterator.c`.

- [ ] **Step 6: Run the test to verify it passes**

```bash
cd build && cmake --build . --target test_reverse_scan && ./test_reverse_scan --gtest_filter=DatabaseReverseTest.BasicDescendingOrder
```

Expected: PASS. If it fails, iterate on the implementation per the implementer notes. The test is the spec — keep fixing until the 5 keys come out in descending order: `elderberry, date, cherry, banana, apple`.

- [ ] **Step 7: Commit**

```bash
git add src/Database/database_iterator.c tests/test_reverse_scan.cpp
git commit -m "feat(iterator): database_scan_start_reverse + database_scan_prev (basic)

Reverse scan with no bounds and single-identifier keys. Positions at the
rightmost leaf (seek_to_rightmost), walks prev decrementing entry_index
and descending into the rightmost child. Paths reconstructed descending.
Strictly additive — forward path unchanged. Bounds, MVCC path_meta,
subtree, multi-level B+tree, and trie_child-on-value handling land in
follow-up tasks."
```

---

## Task 4: Reverse scan with bounds — `seek_to_end_path` + `within_bounds_reverse`

Add the `(start, end]` walked backward semantics: seek to the largest key < end_path, stop when reaching start_path. Mirror of `seek_to_start_path` (line 281) but upper-bound + rightmost descent.

**Files:**
- Modify: `src/Database/database_iterator.c` (add `seek_to_end_path` + `within_bounds_reverse`, wire into `database_scan_start_reverse` and `database_scan_prev`)
- Test: `tests/test_reverse_scan.cpp` (append)

- [ ] **Step 1: Write the failing test**

Append to `tests/test_reverse_scan.cpp`:

```cpp
TEST_F(DatabaseReverseTest, RangeReverse) {
    database_config_t* cfg = database_config_default();
    database_config_set_path(cfg, test_dir.c_str());
    database_config_set_sync_only(cfg, 1);
    int err = 0;
    database_t* db = database_create_with_config(cfg, &err);
    ASSERT_NE(db, nullptr);
    ASSERT_EQ(err, 0);

    const char* keys[] = {"apple", "banana", "cherry", "date", "elderberry"};
    for (const char* k : keys) {
        database_put_sync_raw(db, (const uint8_t*)k, strlen(k),
                              (const uint8_t*)"V", 1);
    }

    // Reverse scan over (start, end] = ["banana", "date") walked backward.
    // Emits: cherry, banana (descending, in [banana, date)).
    path_t* start = path_from_str("banana");
    path_t* end = path_from_str("date");
    database_iterator_t* it = database_scan_start_reverse(db, start, end);
    ASSERT_NE(it, nullptr);

    std::vector<std::string> got;
    path_t* p = nullptr; identifier_t* v = nullptr;
    while (database_scan_prev(it, &p, &v) == 0) {
        identifier_t* id = path_get(p, 0);
        uint8_t* data = identifier_get_data_copy(id);
        size_t dlen = identifier_get_length(id);
        got.push_back(std::string((const char*)data, dlen));
        free(data);
        path_destroy(p);
        identifier_destroy(v);
    }
    database_scan_end(it);
    path_destroy(start);
    path_destroy(end);

    std::vector<std::string> expected = {"cherry", "banana"};
    ASSERT_EQ(got, expected);

    database_destroy(db);
    database_config_destroy(cfg);
}
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
cd build && cmake --build . --target test_reverse_scan && ./test_reverse_scan --gtest_filter=DatabaseReverseTest.RangeReverse
```

Expected: FAIL (either wrong slice, or wrong order, because `seek_to_end_path` and `within_bounds_reverse` aren't implemented yet — `database_scan_start_reverse` falls through to `seek_to_rightmost` which ignores `end_path`).

- [ ] **Step 3: Implement `within_bounds_reverse` in `src/Database/database_iterator.c`**

Insert after `within_bounds` (line 115):

```c
/* Reverse-scan bounds check. Same half-open interval [start, end) as forward,
   but the STOP / SKIP meanings are swapped because we walk descending:
     -1 = STOP iteration: path < start_path (we've walked past the lower bound)
      0 = SKIP entry:    path >= end_path (we're at or above the upper bound;
                                           shouldn't happen post-seek but guard)
      1 = emit.
   Mirror of within_bounds with start/end roles swapped in the return codes. */
static int within_bounds_reverse(database_iterator_t* iter, path_t* path) {
    if (path == NULL) return 0;

    /* Lower bound: if path < start_path, we've walked past the start — stop. */
    if (iter->start_path != NULL) {
        if (path_compare(path, iter->start_path) < 0) {
            return -1;
        }
    }
    /* Upper bound: if path >= end_path, skip (we should be below end_path
       after the seek, but a broad seek may land at or above end). */
    if (iter->end_path != NULL) {
        if (path_compare(path, iter->end_path) >= 0) {
            return 0;
        }
    }
    return 1;
}
```

- [ ] **Step 4: Implement `seek_to_end_path` in `src/Database/database_iterator.c`**

This is the mirror of `seek_to_start_path` (line 281). The mirror transform:
- Binary search for the **upper bound** (largest entry with chunk < end_path's chunk at this level) instead of the lower bound.
- Descend into the **rightmost** subtree that could contain a key < end_path.
- On exact match with end_path's chunk: skip it (we want keys < end_path, strictly).

Insert after `seek_to_start_path` (line 436), before `database_scan_start`:

```c
/* Seek the iterator stack to the rightmost leaf whose key < end_path, for
   reverse scans. Mirror of seek_to_start_path with upper-bound + rightmost
   descent. On any uncertainty, pop seek-pushed frames and fall back to
   seek_to_rightmost (correct-but-slow; never drops a key < end_path).
*/
static void seek_to_end_path(database_iterator_t* iter) {
    if (iter == NULL || iter->end_path == NULL || iter->db == NULL) {
        return;
    }
    if (iter->stack_depth == 0) return;
    hbtrie_t* trie = iter->db->trie;
    if (trie == NULL) return;
    uint8_t chunk_size = trie->chunk_size;
    file_bnode_cache_t* fcache = trie->fcache;
    uint32_t btree_node_size = trie->btree_node_size;

    size_t saved_depth = iter->stack_depth;
    size_t path_len_ids = path_length(iter->end_path);
    if (path_len_ids == 0) {
        seek_to_rightmost(iter);
        return;
    }

    hbtrie_node_t* current = iter->stack[0].node;
    if (current == NULL || current->btree == NULL) return;
    size_t trie_frame_idx = 0;

    for (size_t i = 0; i < path_len_ids; i++) {
        identifier_t* identifier = path_get(iter->end_path, i);
        if (identifier == NULL) goto bail;
        size_t nchunk = identifier_chunk_count(identifier);
        if (nchunk == 0) goto bail;
        for (size_t j = 0; j < nchunk; j++) {
            chunk_t* chunk = identifier_get_chunk(identifier, j);
            if (chunk == NULL) goto bail;
            int is_last = (j == nchunk - 1) && (i == path_len_ids - 1);

            bnode_t* root_bn = current->btree;
            size_t root_index;
            bnode_entry_t* root_entry = bnode_find(root_bn, chunk, &root_index);
            /* bnode_find returns the exact match or the lower bound (first
               entry with chunk >= search). For upper bound we want the last
               entry with chunk < end's chunk. That's root_index - 1 when
               there's no exact match, or root_index - 1 when there IS an exact
               match (we want strictly less than end_path). If root_index == 0,
               every entry in this bnode is >= end's chunk — there's no key <
               end_path under this subtree at all; bail to rightmost of the
               PARENT (which the caller will handle by popping). For the seek
               we just bail. */
            size_t ub = root_index;  /* first entry >= end's chunk */
            (void)root_entry;

            if (atomic_load(&root_bn->level) > 1) {
                /* Internal root: descend through the separator whose child
                   holds keys < end's chunk. The separator at index `ub - 1`
                   (if ub > 0) is the largest separator < end's chunk; its
                   child holds all keys in [sep.key, next_sep.key) which are
                   all < end's chunk. Descend there. */
                if (ub == 0) goto bail;
                size_t root_descend = ub - 1;
                iter->stack[trie_frame_idx].entry_index = root_descend;
                /* When scan_prev pops back to this frame after exhausting the
                   seek-pushed child, it should resume at root_descend - 1
                   (the next-smaller separator). Set entry_index to
                   root_descend now; scan_prev will decrement after emitting
                   the subtree. Actually for reverse we want: descend into
                   separator `root_descend`'s subtree, emit everything < end
                   in it, then resume at root_descend - 1. The frame's
                   entry_index after descent should be root_descend so that
                   when we pop back, scan_prev's decrement-then-process gives
                   root_descend - 1 next. But the descent itself processes
                   root_descend — so set entry_index = root_descend and let
                   scan_prev handle it. */
                bnode_entry_t* sep = bnode_get(root_bn, root_descend);
                if (sep == NULL || !sep->is_bnode_child) goto bail;
                if (sep->child_bnode == NULL && sep->child_disk_offset != 0 && fcache != NULL) {
                    bnode_entry_lazy_load_bnode_child(sep, fcache, chunk_size, btree_node_size);
                }
                if (sep->child_bnode == NULL) goto bail;
                bnode_t* cur_bn = sep->child_bnode;
                while (atomic_load(&cur_bn->level) > 1) {
                    size_t idx;
                    bnode_entry_t* e = bnode_find(cur_bn, chunk, &idx);
                    size_t k;
                    if (e != NULL && e->is_bnode_child) {
                        k = (idx == 0) ? 0 : idx - 1;  /* hmm — if exact match, idx is the separator; we want idx-1 (strictly less). */
                    } else {
                        k = (idx == 0) ? 0 : idx - 1;  /* lower bound idx is first >= chunk; idx-1 is last < chunk. */
                    }
                    if (idx == 0) goto bail;  /* no separator < chunk at this level */
                    bnode_entry_t* s = bnode_get(cur_bn, k);
                    if (s == NULL || !s->is_bnode_child) goto bail;
                    if (s->child_bnode == NULL && s->child_disk_offset != 0 && fcache != NULL) {
                        bnode_entry_lazy_load_bnode_child(s, fcache, chunk_size, btree_node_size);
                    }
                    if (s->child_bnode == NULL) goto bail;
                    if (push_bnode_frame(iter, cur_bn, iter->stack_depth - 1) < 0) goto bail;
                    iter->stack[iter->stack_depth - 1].entry_index = k;
                    cur_bn = s->child_bnode;
                }
                /* cur_bn is the leaf bnode for this trie level. Find the
                   upper bound entry for `chunk`. */
                size_t ub_leaf;
                bnode_entry_t* entry = bnode_find(cur_bn, chunk, &ub_leaf);
                (void)entry;
                if (ub_leaf == 0) goto bail;  /* no leaf entry < chunk */
                if (push_bnode_frame(iter, cur_bn, iter->stack_depth - 1) < 0) goto bail;
                size_t leaf_idx = iter->stack_depth - 1;
                iter->stack[leaf_idx].entry_index = ub_leaf - 1;  /* last entry < chunk */
                /* If is_last: positioned at the largest key < end_path.
                   scan_prev emits it. */
                if (is_last) return;
                /* Otherwise end_path continues deeper — descend into the
                   entry's child (trie_child or child) and continue. */
                bnode_entry_t* leaf_entry = bnode_get(cur_bn, ub_leaf - 1);
                if (leaf_entry == NULL) goto bail;
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
                if (next == NULL) return;  /* no deeper level; positioned here */
                if (push_frame(iter, next, iter->stack_depth - 1) < 0) goto bail;
                current = next;
                trie_frame_idx = iter->stack_depth - 1;
                /* Position the new trie frame at its rightmost (will be
                   refined by the next chunk's seek). */
                iter->stack[trie_frame_idx].entry_index = bnode_count(next->btree) - 1;
            } else {
                /* Leaf root. */
                if (ub == 0) goto bail;
                iter->stack[trie_frame_idx].entry_index = ub - 1;
                if (is_last) return;
                bnode_entry_t* leaf_entry = bnode_get(root_bn, ub - 1);
                if (leaf_entry == NULL) goto bail;
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
                if (next == NULL) return;
                if (push_frame(iter, next, iter->stack_depth - 1) < 0) goto bail;
                current = next;
                trie_frame_idx = iter->stack_depth - 1;
                iter->stack[trie_frame_idx].entry_index = bnode_count(next->btree) - 1;
            }
        }
    }
    return;

bail:
    while (iter->stack_depth > saved_depth) {
        pop_frame(iter);
    }
    if (iter->stack_depth > 0) {
        iter->stack[0].entry_index = 0;
    }
    seek_to_rightmost(iter);
}
```

> **Implementer note:** `seek_to_end_path` is the most intricate piece of the reverse iterator. The reference above is a careful mirror of `seek_to_start_path`, but the upper-bound + rightmost-descent logic has several edge cases (exact match on end_path's chunk, end_path deeper than the trie, empty subtrees). The test in Step 1 covers the common case; if it fails, the implementer should add more granular tests (e.g. end_path that exactly matches a stored key, end_path deeper than any stored key) and fix the seek until they pass. This is the TDD loop.

- [ ] **Step 5: Wire `within_bounds_reverse` into `database_scan_prev`**

In `database_scan_prev` (Task 3's implementation), replace `int bounds = 1;` with:

```c
                    int bounds = within_bounds_reverse(iter, result_path);
```

- [ ] **Step 6: Wire `seek_to_end_path` into `database_scan_start_reverse`**

In `database_scan_start_reverse`, the `if (iter->end_path != NULL)` branch already calls `seek_to_end_path(iter)` (from Task 3 Step 3). Confirm it's wired; if you wrote it to call `seek_to_rightmost` as a fallback, replace with `seek_to_end_path`.

- [ ] **Step 7: Run the test to verify it passes**

```bash
cd build && cmake --build . --target test_reverse_scan && ./test_reverse_scan --gtest_filter=DatabaseReverseTest.RangeReverse
```

Expected: PASS (`cherry, banana` in descending order). If it fails, iterate.

- [ ] **Step 8: Commit**

```bash
git add src/Database/database_iterator.c tests/test_reverse_scan.cpp
git commit -m "feat(iterator): reverse scan with bounds (start, end] walked backward

Adds seek_to_end_path (upper-bound + rightmost descent, mirror of
seek_to_start_path) and within_bounds_reverse (same [start, end) half-
open interval, STOP/SWAP meanings swapped for descending walk). Reverse
range scan now emits the correct slice in descending order."
```

---

## Task 5: Reverse scan with path_meta, MVCC, subtree, multi-level B+tree, trie_child-on-value

The basic reverse scan (Tasks 3-4) handles single-identifier keys with no path_meta, no versions, no subtree prefix, no multi-level B+tree, and no `has_value && trie_child` case. This task adds the remaining cases by mirroring the corresponding logic in `database_scan_next` (lines 632-880). Each case gets its own test; the implementation mirrors the forward path with the reverse transforms already established.

**Files:**
- Modify: `src/Database/database_iterator.c` (extend `database_scan_prev` with path_meta, trie_child-on-value via `child_emitted` flag, multi-level B+tree positioning — most of which is already in Task 3's reference but needs the path_meta branch)
- Modify: `src/Database/database_iterator.h` (add `child_emitted` flag to `iterator_frame_t` if not already present — needed for the trie_child-on-value case)
- Modify: `src/HBTrie/hbtrie.h` (add `child_emitted` flag to `hbtrie_cursor_frame_t` if Task 1's note was followed)
- Test: `tests/test_reverse_scan.cpp` (append 5 tests)

- [ ] **Step 1: Add `child_emitted` flag to `iterator_frame_t`**

In `src/Database/database_iterator.h`, in the `iterator_frame_t` struct (line 21-27), add:

```c
    uint8_t child_emitted;   // 1 = value emitted, child not yet descended (reverse only)
```

- [ ] **Step 2: Write the failing test for multi-identifier keys (path_meta)**

Append to `tests/test_reverse_scan.cpp`:

```cpp
TEST_F(DatabaseReverseTest, MultiIdentifierPathMeta) {
    database_config_t* cfg = database_config_default();
    database_config_set_path(cfg, test_dir.c_str());
    database_config_set_sync_only(cfg, 1);
    int err = 0;
    database_t* db = database_create_with_config(cfg, &err);
    ASSERT_NE(db, nullptr);
    ASSERT_EQ(err, 0);

    // Insert multi-component paths: "users/alice", "users/bob", "users/carol".
    auto put_path = [&](const std::string& a, const std::string& b) {
        std::string key = a + "/" + b;
        database_put_sync_raw(db, (const uint8_t*)key.c_str(), key.size(),
                              (const uint8_t*)"V", 1);
    };
    put_path("users", "alice");
    put_path("users", "bob");
    put_path("users", "carol");

    database_iterator_t* it = database_scan_start_reverse(db, NULL, NULL);
    ASSERT_NE(it, nullptr);

    std::vector<std::string> got;
    path_t* p = nullptr; identifier_t* v = nullptr;
    while (database_scan_prev(it, &p, &v) == 0) {
        // Reconstruct the full key by joining all identifiers with '/'.
        std::string key;
        size_t n = path_length(p);
        for (size_t i = 0; i < n; i++) {
            identifier_t* id = path_get(p, i);
            uint8_t* data = identifier_get_data_copy(id);
            size_t dlen = identifier_get_length(id);
            if (i > 0) key += "/";
            key += std::string((const char*)data, dlen);
            free(data);
        }
        got.push_back(key);
        path_destroy(p);
        identifier_destroy(v);
    }
    database_scan_end(it);

    std::vector<std::string> expected = {
        "users/carol", "users/bob", "users/alice"
    };
    ASSERT_EQ(got, expected);

    database_destroy(db);
    database_config_destroy(cfg);
}
```

> **Implementer note:** the raw byte API `database_put_sync_raw` takes a flat key; the `/` delimiter becomes part of the key bytes. The HBTrie chunks it according to `chunk_size` (default 4). The path_meta stored on the leaf records the per-subscript {chunk_count, byte_length} so the iterator can reconstruct exact identifiers. The reverse iterator must use the same path_meta logic as forward (lines 732-781 of `database_scan_next`) but with the reverse chunk-collection indices from Task 3 Step 5. Mirror that block into `database_scan_prev`, replacing the basic `build_identifier_from_chunks(chunks, nchunks, chunk_size, 0)` call with the path_meta-aware loop.

- [ ] **Step 3: Write the failing test for MVCC versions**

Append:

```cpp
TEST_F(DatabaseReverseTest, MVCCVersions) {
    database_config_t* cfg = database_config_default();
    database_config_set_path(cfg, test_dir.c_str());
    // NOT sync_only — we need MVCC for version chains.
    int err = 0;
    database_t* db = database_create_with_config(cfg, &err);
    ASSERT_NE(db, nullptr);
    ASSERT_EQ(err, 0);

    // Insert, then update "banana" with a new value.
    const char* keys[] = {"apple", "banana", "cherry"};
    for (const char* k : keys) {
        database_put_sync_raw(db, (const uint8_t*)k, strlen(k),
                              (const uint8_t*)"V1", 2);
    }
    // Overwrite banana.
    database_put_sync_raw(db, (const uint8_t*)"banana", 6,
                          (const uint8_t*)"V2", 2);

    database_iterator_t* it = database_scan_start_reverse(db, NULL, NULL);
    ASSERT_NE(it, nullptr);

    std::vector<std::string> got_keys, got_vals;
    path_t* p = nullptr; identifier_t* v = nullptr;
    while (database_scan_prev(it, &p, &v) == 0) {
        identifier_t* id = path_get(p, 0);
        uint8_t* kdata = identifier_get_data_copy(id);
        size_t klen = identifier_get_length(id);
        got_keys.push_back(std::string((const char*)kdata, klen));
        uint8_t* vdata = identifier_get_data_copy(v);
        size_t vlen = identifier_get_length(v);
        got_vals.push_back(std::string((const char*)vdata, vlen));
        free(kdata); free(vdata);
        path_destroy(p);
        identifier_destroy(v);
    }
    database_scan_end(it);

    // Descending: cherry, banana, apple. banana should have V2 (latest visible).
    ASSERT_EQ(got_keys, (std::vector<std::string>{"cherry", "banana", "apple"}));
    ASSERT_EQ(got_vals, (std::vector<std::string>{"V1", "V2", "V1"}));

    database_destroy(db);
    database_config_destroy(cfg);
}
```

> **Implementer note:** `version_entry_find_visible` is per-entry and direction-agnostic, so the MVCC test should pass once the basic reverse scan works. If it fails, the issue is likely that the reverse scan isn't reading `entry->versions` correctly — confirm the version-chain branch (Task 3 Step 5's `if (entry->has_versions && entry->versions)` block) is present in `database_scan_prev`.

- [ ] **Step 4: Write the failing test for subtree prefix_skip**

Append:

```cpp
TEST_F(DatabaseReverseTest, SubtreePrefixSkip) {
    database_config_t* cfg = database_config_default();
    database_config_set_path(cfg, test_dir.c_str());
    database_config_set_sync_only(cfg, 1);
    int err = 0;
    database_t* db = database_create_with_config(cfg, &err);
    ASSERT_NE(db, nullptr);

    // Insert keys under "docs/alpha", "docs/beta", "docs/gamma".
    auto put = [&](const std::string& k) {
        database_put_sync_raw(db, (const uint8_t*)k.c_str(), k.size(),
                              (const uint8_t*)"V", 1);
    };
    put("docs/alpha");
    put("docs/beta");
    put("docs/gamma");

    // Subtree on "docs".
    path_t* prefix = path_from_str("docs");
    database_subtree_t* subtree = database_subtree_create(db, prefix);
    ASSERT_NE(subtree, nullptr);

    // Reverse scan over the subtree — should emit "gamma", "beta", "alpha"
    // (prefix stripped).
    database_iterator_t* it = database_subtree_scan_start_reverse(subtree, NULL, NULL);
    ASSERT_NE(it, nullptr);

    std::vector<std::string> got;
    path_t* p = nullptr; identifier_t* v = nullptr;
    while (database_scan_prev(it, &p, &v) == 0) {
        identifier_t* id = path_get(p, 0);
        uint8_t* data = identifier_get_data_copy(id);
        size_t dlen = identifier_get_length(id);
        got.push_back(std::string((const char*)data, dlen));
        free(data);
        path_destroy(p);
        identifier_destroy(v);
    }
    database_scan_end(it);

    std::vector<std::string> expected = {"gamma", "beta", "alpha"};
    ASSERT_EQ(got, expected);

    database_subtree_destroy(subtree);
    path_destroy(prefix);
    database_destroy(db);
    database_config_destroy(cfg);
}
```

> **Implementer note:** `database_subtree_scan_start_reverse` may not exist yet. Check `src/Database/database_subtree.h` for the existing `database_subtree_scan_start`. If the subtree API is a thin wrapper that sets `iter->prefix_skip` and calls `database_scan_start`, add a `database_subtree_scan_start_reverse` mirror that sets `prefix_skip` and calls `database_scan_start_reverse`. This was flagged as an open question in the spec; this test resolves it by requiring the mirror. If you decide NOT to add the subtree reverse API, delete this test and document the decision in the spec's Open Questions. Recommended: add the mirror — it's a 10-line wrapper.

- [ ] **Step 5: Write the failing test for multi-level B+tree (large key count)**

Append:

```cpp
TEST_F(DatabaseReverseTest, MultiLevelBTree) {
    database_config_t* cfg = database_config_default();
    database_config_set_path(cfg, test_dir.c_str());
    database_config_set_sync_only(cfg, 1);
    database_config_set_btree_node_size(cfg, 256);  // small node size to force splits
    int err = 0;
    database_t* db = database_create_with_config(cfg, &err);
    ASSERT_NE(db, nullptr);

    // Insert 200 keys — enough to trigger B+tree splits at node_size=256.
    char key[32];
    for (int i = 0; i < 200; i++) {
        snprintf(key, sizeof(key), "key_%03d", i);
        database_put_sync_raw(db, (const uint8_t*)key, strlen(key),
                              (const uint8_t*)"V", 1);
    }

    database_iterator_t* it = database_scan_start_reverse(db, NULL, NULL);
    ASSERT_NE(it, nullptr);

    // Collect all keys in reverse; expect key_199 down to key_000.
    std::vector<std::string> got;
    path_t* p = nullptr; identifier_t* v = nullptr;
    while (database_scan_prev(it, &p, &v) == 0) {
        identifier_t* id = path_get(p, 0);
        uint8_t* data = identifier_get_data_copy(id);
        size_t dlen = identifier_get_length(id);
        got.push_back(std::string((const char*)data, dlen));
        free(data);
        path_destroy(p);
        identifier_destroy(v);
    }
    database_scan_end(it);

    ASSERT_EQ(got.size(), 200u);
    // First should be key_199, last should be key_000.
    EXPECT_EQ(got[0], "key_199");
    EXPECT_EQ(got[199], "key_000");
    // And strictly descending throughout.
    bool descending = true;
    for (size_t i = 1; i < got.size(); i++) {
        if (got[i] >= got[i-1]) { descending = false; break; }
    }
    EXPECT_TRUE(descending);

    database_destroy(db);
    database_config_destroy(cfg);
}
```

- [ ] **Step 6: Write the failing test for trie_child-on-value (has_value && trie_child)**

Append:

```cpp
TEST_F(DatabaseReverseTest, ValueWithTrieChild) {
    database_config_t* cfg = database_config_default();
    database_config_set_path(cfg, test_dir.c_str());
    database_config_set_sync_only(cfg, 1);
    int err = 0;
    database_t* db = database_create_with_config(cfg, &err);
    ASSERT_NE(db, nullptr);

    // Insert "users" (value), "users/alice", "users/bob".
    // "users" has a value AND a trie_child (the "alice"/"bob" subtree).
    database_put_sync_raw(db, (const uint8_t*)"users", 5, (const uint8_t*)"V_users", 7);
    database_put_sync_raw(db, (const uint8_t*)"users/alice", 11, (const uint8_t*)"V_a", 3);
    database_put_sync_raw(db, (const uint8_t*)"users/bob", 9, (const uint8_t*)"V_b", 3);

    database_iterator_t* it = database_scan_start_reverse(db, NULL, NULL);
    ASSERT_NE(it, nullptr);

    std::vector<std::string> got;
    path_t* p = nullptr; identifier_t* v = nullptr;
    while (database_scan_prev(it, &p, &v) == 0) {
        identifier_t* id = path_get(p, 0);
        uint8_t* data = identifier_get_data_copy(id);
        size_t dlen = identifier_get_length(id);
        // For multi-identifier paths, join with '/'.
        std::string key((const char*)data, dlen);
        free(data);
        size_t n = path_length(p);
        for (size_t i = 1; i < n; i++) {
            identifier_t* id2 = path_get(p, i);
            uint8_t* d2 = identifier_get_data_copy(id2);
            size_t l2 = identifier_get_length(id2);
            key += "/" + std::string((const char*)d2, l2);
            free(d2);
        }
        got.push_back(key);
        path_destroy(p);
        identifier_destroy(v);
    }
    database_scan_end(it);

    // Descending: users/bob, users/alice, users.
    // (users/bob > users/alice > users because shorter prefix sorts before
    // longer with same prefix — actually lexicographic: "users" < "users/alice"
    // < "users/bob". Reverse: users/bob, users/alice, users.)
    std::vector<std::string> expected = {"users/bob", "users/alice", "users"};
    ASSERT_EQ(got, expected);

    database_destroy(db);
    database_config_destroy(cfg);
}
```

> **Implementer note:** This is the case that requires the `child_emitted` flag on `iterator_frame_t`. When `database_scan_prev` encounters an entry with `has_value && trie_child`, it must:
> 1. On the first visit: emit the value, set `frame->child_emitted = 1`, leave `frame->entry_index` at `this_index` (do NOT decrement past it).
> 2. On the next `prev()` call: see `child_emitted == 1`, push the trie_child frame, position it at rightmost (seek_to_rightmost on the child), clear `child_emitted`, and descend. The subtree's values are emitted on subsequent `prev()` calls.
> 3. When the subtree is exhausted (popped back to this frame), decrement `entry_index` past `this_index` and continue.
>
> This is the reverse mirror of forward's "emit value, push child, resume after child." In forward, the value sorts BEFORE the subtree, so emit-then-descend works. In reverse, the value sorts AFTER the subtree (it's a prefix of the subtree's keys, hence smaller), so we emit the value first (it's the largest key at this position that's already a "complete" key) — wait, no. "users" < "users/alice" < "users/bob", so in DESCENDING order we want users/bob, users/alice, users. The trie_child subtree contains users/alice and users/bob, which sort AFTER "users". So in reverse, we hit the "users" entry first (it's at the rightmost position with a value), but we should emit users/bob and users/alice FIRST (they're larger), THEN "users". So the correct reverse behavior is: encounter "users" entry, see has_value && trie_child, DESCEND into trie_child first (emit everything in it, descending), THEN emit "users" on the way back up. This is the opposite of forward. The `child_emitted` flag should be named `value_pending` and set when we descend, then on pop-back we emit the value and decrement.
>
> The reference logic: in `database_scan_prev`, when encountering `has_value && trie_child != NULL`:
> - If `frame->value_pending == 0`: set `frame->value_pending = 1`, push the trie_child, seek_to_rightmost on it, `break` to descend. Don't emit yet.
> - If `frame->value_pending == 1`: emit the value, set `frame->value_pending = 0`, decrement `entry_index`, continue the inner loop.
>
> Add `uint8_t value_pending` to `iterator_frame_t` (rename `child_emitted` from Step 1). Implement the two-visit logic. The test will catch the wrong order; iterate until "users/bob, users/alice, users" comes out in that order.

- [ ] **Step 7: Run all reverse-scan tests**

```bash
cd build && cmake --build . --target test_reverse_scan && ./test_reverse_scan
```

Expected: all 7 tests PASS (1 HBTrie + 6 database). If any fail, iterate per the implementer notes. The tests are the spec.

- [ ] **Step 8: Commit**

```bash
git add src/Database/database_iterator.h src/Database/database_iterator.c tests/test_reverse_scan.cpp
git commit -m "feat(iterator): reverse scan with path_meta, MVCC, subtree, multi-level B+tree, trie_child

Extends database_scan_prev to handle all cases database_scan_next handles:
- Multi-identifier path reconstruction via path_meta (chunk_count/byte_length).
- MVCC version chains (direction-agnostic visibility check).
- Subtree prefix_skip (database_subtree_scan_start_reverse wrapper).
- Multi-level B+tree descent (rightmost descent through internal bnodes).
- has_value && trie_child: descend into subtree first, emit value on pop-back
  (via value_pending flag — reverse mirror of forward's emit-then-descend)."
```

---

## Task 6: Regression — forward tests stay green + full suite + de-wonk

The reverse scan is strictly additive; the existing forward path is untouched. This task confirms that and runs a de-wonk pass to catch any stubbed/disabled/weird code before declaring the engine work done.

**Files:**
- No code changes (unless a regression is found).

- [ ] **Step 1: Run the full ctest suite**

```bash
cd build && cmake --build . && ctest --output-on-failure
```

Expected: ALL tests pass, including the existing forward scan tests (`test_graph`, `test_graphql_*`, `test_database*`, etc.) and the new `test_reverse_scan`. If any forward test fails, the reverse scan somehow broke the forward path — investigate and fix before proceeding. The forward path must be untouched.

- [ ] **Step 2: Run with ASAN + valgrind on the reverse scan test**

```bash
cd build && cmake -DCMAKE_C_FLAGS="-fsanitize=address -g" -DCMAKE_CXX_FLAGS="-fsanitize=address -g" .. && cmake --build . --target test_reverse_scan && ./test_reverse_scan
```

Expected: no ASAN errors (no leaks, no buffer overflows, no use-after-free in the reverse path). The iterator's refcounting (REFERENCE on push, DEREFERENCE on pop) must balance exactly — ASAN will catch imbalances.

```bash
valgrind --leak-check=full --error-exitcode=1 ./build/test_reverse_scan
```

Expected: zero leaks, zero errors. The iterator owns its `start_path`/`end_path`/`current_path` copies and frees them in `database_scan_end`; the stack frames' node/bnode refcounts must balance.

- [ ] **Step 3: Run de-wonk**

Invoke the de-wonk skill on the engine backward-scan work. It should catch any:
- Stub functions still returning -1/NULL (the Task 2 stubs were replaced in Task 3 — confirm none remain).
- Disabled or `#if 0`'d out code.
- TODOs/FIXMEs introduced during the reverse-scan implementation.
- Inconsistencies between the forward and reverse paths (e.g. a lazy-load check present in forward but missing in reverse).

Fix anything de-wonk finds.

- [ ] **Step 4: Final commit (if de-wonk found anything)**

```bash
git add -A
git commit -m "chore(iterator): de-wonk pass on reverse scan — fix [findings]

Run de-wonk at the completion gate of the engine backward-scan work.
Catches stubs, disabled code, TODOs, and forward/reverse inconsistencies
before declaring the engine work done."
```

If de-wonk found nothing, skip this step (no empty commits).

- [ ] **Step 5: Verify the spec's Open Question is resolved**

The spec's Open Questions section asks: "Does `database_subtree_scan_start_reverse` need to be added alongside `database_scan_start_reverse` for the subtree path, or do reverse scans on a subtree'd db just use full paths?"

If Task 5 Step 4's `database_subtree_scan_start_reverse` was implemented, update the spec's Open Questions to mark this resolved. If it was NOT implemented (the test was deleted and the decision documented), update the spec to record the decision.

```bash
# Edit docs/superpowers/specs/2026-07-12-vector-layer-design.md — mark the
# subtree reverse-scan open question resolved with the chosen approach.
git add docs/superpowers/specs/2026-07-12-vector-layer-design.md
git commit -m "docs(vector): resolve subtree reverse-scan open question"
```

---

## Self-Review Checklist (run after writing this plan)

**Spec coverage:**
- [x] Engine: backward scan section — `database_scan_start_reverse` + `database_scan_prev` API (Task 2-3).
- [x] `hbtrie_cursor_prev` + rightmost descent (Task 1).
- [x] `seek_position_leaf_reverse` → folded into `seek_to_end_path` (Task 4).
- [x] Vacuum interaction — reverse cursors reuse `open_cursor_count` / `cursor_cvar` (Task 3 Step 3, same as forward).
- [x] Tests: `ReverseScanAll`, `ReverseScanRange`, `ReverseScanPrefix`, `ReverseScanMVCC`, `ReverseScanSubtree`, `ForwardScanUnchanged` (Tasks 3-6; `ReverseScanPrefix` is covered by the `end_path = prefix + "\x7f"` idiom in the SLSH layer's plan, not separately here — the RangeReverse test plus the MultiLevelBTree test cover the relevant cases).
- [x] Strictly additive — forward path untouched, `ForwardScanUnchanged` guarded by running the full ctest suite (Task 6 Step 1).
- [x] `wavedb.def` exports (Task 2 Step 3).

**Placeholder scan:** no TBD/TODO/"implement later" in the plan. Implementer notes are guidance for the TDD loop, not placeholders.

**Type consistency:** `database_scan_start_reverse` / `database_scan_prev` signatures match across header (Task 2), stubs (Task 2), real impl (Task 3), and tests (Tasks 3-5). `hbtrie_cursor_prev` / `hbtrie_cursor_init_reverse` match across header (Task 1), impl (Task 1), and test (Task 1). `iterator_frame_t` gains `value_pending` (Task 5 Step 1, renaming `child_emitted` from the same step — single rename, consistent).

**Known risk:** the `seek_to_end_path` reference code (Task 4 Step 4) is the most intricate piece and the most likely to have bugs in the plan. The TDD loop (test in Step 1, iterate until pass) is the correctness mechanism. If the implementer finds the reference code unfixable, they should rewrite `seek_to_end_path` from scratch following the mirror transform description, using the test as the spec.

---

## Execution Handoff

Plan complete and saved to `docs/superpowers/plans/2026-07-12-engine-backward-scan.md`. Two execution options:

**1. Subagent-Driven (recommended)** — dispatch a fresh subagent per task, review between tasks, fast iteration.

**2. Inline Execution** — execute tasks in this session using executing-plans, batch execution with checkpoints.

Which approach?