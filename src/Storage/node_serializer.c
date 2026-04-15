//
// Created by victor on 03/13/26.
//

#include "node_serializer.h"
#include "../HBTrie/identifier.h"
#include "../Util/allocator.h"
#include <string.h>
#include <stdatomic.h>
#include <arpa/inet.h>  // For htonl/ntohl

// Helper: write uint16_t in network byte order
static void write_uint16(uint8_t** buf, uint16_t val) {
    uint16_t net_val = htons(val);
    memcpy(*buf, &net_val, sizeof(uint16_t));
    *buf += sizeof(uint16_t);
}

// Helper: read uint16_t in network byte order
static uint16_t read_uint16(uint8_t** buf) {
    uint16_t net_val;
    memcpy(&net_val, *buf, sizeof(uint16_t));
    *buf += sizeof(uint16_t);
    return ntohs(net_val);
}

// Helper: write uint32_t in network byte order
static void write_uint32(uint8_t** buf, uint32_t val) {
    uint32_t net_val = htonl(val);
    memcpy(*buf, &net_val, sizeof(uint32_t));
    *buf += sizeof(uint32_t);
}

// Helper: read uint32_t in network byte order
static uint32_t read_uint32(uint8_t** buf) {
    uint32_t net_val;
    memcpy(&net_val, *buf, sizeof(uint32_t));
    *buf += sizeof(uint32_t);
    return ntohl(net_val);
}

// Helper: write uint64_t in network byte order
static void write_uint64(uint8_t** buf, uint64_t val) {
    uint32_t high = htonl((uint32_t)(val >> 32));
    uint32_t low = htonl((uint32_t)(val & 0xFFFFFFFF));
    memcpy(*buf, &high, sizeof(uint32_t));
    *buf += sizeof(uint32_t);
    memcpy(*buf, &low, sizeof(uint32_t));
    *buf += sizeof(uint32_t);
}

// Helper: read uint64_t in network byte order
static uint64_t read_uint64(uint8_t** buf) {
    uint32_t high, low;
    memcpy(&high, *buf, sizeof(uint32_t));
    *buf += sizeof(uint32_t);
    memcpy(&low, *buf, sizeof(uint32_t));
    *buf += sizeof(uint32_t);
    return ((uint64_t)ntohl(high) << 32) | ntohl(low);
}

// Helper: write uint8_t
static void write_uint8(uint8_t** buf, uint8_t val) {
    **buf = val;
    (*buf)++;
}

// Helper: read uint8_t
static uint8_t read_uint8(uint8_t** buf) {
    uint8_t val = **buf;
    (*buf)++;
    return val;
}

// Helper: write bytes
static void write_bytes(uint8_t** buf, uint8_t* data, size_t len) {
    memcpy(*buf, data, len);
    *buf += len;
}

// Helper: read bytes
static void read_bytes(uint8_t** buf, uint8_t* data, size_t len) {
    memcpy(data, *buf, len);
    *buf += len;
}

// Identifier serialization - serialize to a flat buffer
int identifier_serialize(identifier_t* ident, uint8_t** buf, size_t* len) {
    if (ident == NULL || buf == NULL || len == NULL) {
        return -1;
    }

    // Reconstruct identifier as a single buffer first
    buffer_t* data_buf = identifier_to_buffer(ident);
    if (data_buf == NULL) {
        return -1;
    }

    // Total size: length (4 bytes) + data
    size_t total_len = sizeof(uint32_t) + data_buf->size;
    *buf = get_memory(total_len);
    *len = total_len;

    uint8_t* ptr = *buf;

    // Write length
    write_uint32(&ptr, (uint32_t)data_buf->size);

    // Write data
    if (data_buf->size > 0) {
        write_bytes(&ptr, data_buf->data, data_buf->size);
    }

    buffer_destroy(data_buf);
    return 0;
}

// Identifier deserialization - deserialize from a flat buffer
identifier_t* identifier_deserialize(uint8_t* buf, size_t len, size_t chunk_size) {
    if (buf == NULL || len < sizeof(uint32_t)) {
        return NULL;
    }

    uint8_t* ptr = buf;

    // Read length
    uint32_t length = read_uint32(&ptr);

    if (sizeof(uint32_t) + length != len) {
        return NULL;  // Malformed data
    }

    // Create buffer from remaining data
    buffer_t* data_buf = buffer_create(length);
    if (data_buf == NULL) {
        return NULL;
    }

    if (length > 0) {
        memcpy(data_buf->data, ptr, length);
    }

    // Create identifier from buffer
    identifier_t* ident = identifier_create(data_buf, chunk_size);
    buffer_destroy(data_buf);
    return ident;
}

// Chunk serialization
int chunk_serialize(chunk_t* chunk, uint8_t chunk_size, uint8_t** buf) {
    if (chunk == NULL || buf == NULL) {
        return -1;
    }

    *buf = get_memory(chunk_size);
    memcpy(*buf, chunk->data, chunk_size);
    return 0;
}

// Chunk deserialization
chunk_t* chunk_deserialize(uint8_t* buf, uint8_t chunk_size) {
    if (buf == NULL) {
        return NULL;
    }

    // Create chunk with inline data
    chunk_t* chunk = chunk_create(buf, chunk_size);
    return chunk;
}

// B+tree node serialization
// Format: magic(0xB3) + level(uint16) + num_entries(uint16) + entries...
// Each entry: chunk(chunk_size) + flags(uint8) + payload
// flags: bit0=has_value, bit1=is_bnode_child, bit2=has_versions
#define BNODE_SERIALIZE_MAGIC 0xB3
#define BNODE_SERIALIZE_MAGIC_V2 0xB4  // V2: inline child bnodes

// Dynamic buffer for serialization
typedef struct {
    uint8_t* data;
    size_t size;
    size_t capacity;
} serialize_buf_t;

static void sbuf_init(serialize_buf_t* sb) {
    sb->capacity = 1024;
    sb->data = get_memory(sb->capacity);
    sb->size = 0;
}

static void sbuf_ensure(serialize_buf_t* sb, size_t needed) {
    while (sb->size + needed > sb->capacity) {
        sb->capacity *= 2;
        uint8_t* new_data = get_memory(sb->capacity);
        memcpy(new_data, sb->data, sb->size);
        free(sb->data);
        sb->data = new_data;
    }
}

static void sbuf_write_uint8(serialize_buf_t* sb, uint8_t val) {
    sbuf_ensure(sb, 1);
    sb->data[sb->size++] = val;
}

static void sbuf_write_uint16(serialize_buf_t* sb, uint16_t val) {
    sbuf_ensure(sb, 2);
    uint16_t net_val = htons(val);
    memcpy(sb->data + sb->size, &net_val, 2);
    sb->size += 2;
}

static void sbuf_write_uint32(serialize_buf_t* sb, uint32_t val) {
    sbuf_ensure(sb, 4);
    uint32_t net_val = htonl(val);
    memcpy(sb->data + sb->size, &net_val, 4);
    sb->size += 4;
}

static void sbuf_write_uint64(serialize_buf_t* sb, uint64_t val) {
    sbuf_ensure(sb, 8);
    uint32_t high = htonl((uint32_t)(val >> 32));
    uint32_t low = htonl((uint32_t)(val & 0xFFFFFFFF));
    memcpy(sb->data + sb->size, &high, 4);
    memcpy(sb->data + sb->size + 4, &low, 4);
    sb->size += 8;
}

static void sbuf_write_bytes(serialize_buf_t* sb, const uint8_t* data, size_t len) {
    sbuf_ensure(sb, len);
    memcpy(sb->data + sb->size, data, len);
    sb->size += len;
}

// Forward declaration for recursive serialization
static void bnode_serialize_entries(bnode_t* node, uint8_t chunk_size, serialize_buf_t* sb);

static void bnode_serialize_entries(bnode_t* node, uint8_t chunk_size, serialize_buf_t* sb) {
    for (int i = 0; i < node->entries.length; i++) {
        bnode_entry_t* entry = &node->entries.data[i];

        // Write chunk
        chunk_t* key = bnode_entry_get_key(entry);
        if (key != NULL) {
            sbuf_write_bytes(sb, key->data, chunk_size);
        } else {
            uint8_t zero[8] = {0};
            sbuf_write_bytes(sb, zero, chunk_size);
        }

        // Write flags byte
        uint8_t flags = 0;
        if (entry->has_value) flags |= 0x01;
        if (entry->is_bnode_child) flags |= 0x02;
        if (entry->has_value && entry->has_versions) flags |= 0x04;
        sbuf_write_uint8(sb, flags);

        if (entry->has_value) {
            if (entry->has_versions && entry->versions != NULL) {
                // MVCC: Serialize version chain
                size_t version_count = 0;
                version_entry_t* current = entry->versions;
                while (current != NULL) {
                    version_count++;
                    current = current->next;
                }
                sbuf_write_uint16(sb, (uint16_t)version_count);

                current = entry->versions;
                while (current != NULL) {
                    sbuf_write_uint8(sb, current->is_deleted);
                    sbuf_write_uint64(sb, current->txn_id.time);
                    sbuf_write_uint64(sb, current->txn_id.nanos);
                    sbuf_write_uint64(sb, current->txn_id.count);

                    if (!current->is_deleted && current->value != NULL) {
                        buffer_t* ident_buf = identifier_to_buffer(current->value);
                        if (ident_buf != NULL) {
                            sbuf_write_uint32(sb, (uint32_t)ident_buf->size);
                            if (ident_buf->size > 0) {
                                sbuf_write_bytes(sb, ident_buf->data, ident_buf->size);
                            }
                            buffer_destroy(ident_buf);
                        } else {
                            sbuf_write_uint32(sb, 0);
                        }
                    } else {
                        sbuf_write_uint32(sb, 0);
                    }
                    current = current->next;
                }
            } else {
                // Legacy: single value
                identifier_t* ident = entry->value;
                if (ident != NULL) {
                    buffer_t* ident_buf = identifier_to_buffer(ident);
                    if (ident_buf != NULL) {
                        sbuf_write_uint32(sb, (uint32_t)ident_buf->size);
                        if (ident_buf->size > 0) {
                            sbuf_write_bytes(sb, ident_buf->data, ident_buf->size);
                        }
                        buffer_destroy(ident_buf);
                    } else {
                        sbuf_write_uint32(sb, 0);
                    }
                } else {
                    sbuf_write_uint32(sb, 0);
                }
            }
        } else if (entry->is_bnode_child) {
            // Internal bnode child - serialize recursively inline
            // Write a placeholder length (will be backfilled)
            size_t length_pos = sb->size;
            sbuf_write_uint32(sb, 0);  // placeholder for child bnode size

            // Recursively serialize child bnode
            bnode_t* child = entry->child_bnode;
            if (child != NULL) {
                // Write child magic + level + num_entries + entries
                sbuf_write_uint8(sb, BNODE_SERIALIZE_MAGIC_V2);
                sbuf_write_uint16(sb, atomic_load(&child->level));
                sbuf_write_uint16(sb, (uint16_t)child->entries.length);
                bnode_serialize_entries(child, chunk_size, sb);
            } else {
                // Empty child placeholder (shouldn't happen normally)
                sbuf_write_uint8(sb, BNODE_SERIALIZE_MAGIC_V2);
                sbuf_write_uint16(sb, 1);
                sbuf_write_uint16(sb, 0);
            }

            // Backfill the length
            uint32_t child_size = (uint32_t)(sb->size - length_pos - 4);
            uint32_t net_size = htonl(child_size);
            memcpy(sb->data + length_pos, &net_size, 4);
        } else {
            // Child hbtrie_node location
            sbuf_write_uint64(sb, entry->child_disk_offset);
            sbuf_write_uint64(sb, 0);  // Reserved (was block_index, now unused)
        }
    }
}

int bnode_serialize(bnode_t* node, uint8_t chunk_size, uint8_t** buf, size_t* len) {
    if (node == NULL || buf == NULL || len == NULL) {
        return -1;
    }

    serialize_buf_t sb;
    sbuf_init(&sb);

    // Write magic byte (V2 format with inline child bnodes)
    sbuf_write_uint8(&sb, BNODE_SERIALIZE_MAGIC_V2);

    // Write level
    sbuf_write_uint16(&sb, atomic_load(&node->level));

    // Write number of entries
    sbuf_write_uint16(&sb, (uint16_t)node->entries.length);

    // Write entries (recursively includes child bnodes)
    bnode_serialize_entries(node, chunk_size, &sb);

    *buf = sb.data;
    *len = sb.size;
    return 0;
}

// Helper: recursively deserialize a bnode and its children from a buffer
static bnode_t* bnode_deserialize_recursive(uint8_t** ptr, size_t* remaining,
                                             uint8_t chunk_size, uint32_t btree_node_size,
                                             node_location_t** locations, size_t* num_locations);

static bnode_t* bnode_deserialize_recursive(uint8_t** ptr, size_t* remaining,
                                             uint8_t chunk_size, uint32_t btree_node_size,
                                             node_location_t** locations, size_t* num_locations) {
    if (*remaining < 1) return NULL;

    uint8_t first_byte = **ptr;
    int is_v2 = (first_byte == BNODE_SERIALIZE_MAGIC_V2);
    int is_v1 = (first_byte == BNODE_SERIALIZE_MAGIC);

    uint16_t level = 1;
    uint16_t num_entries;

    if (is_v2 || is_v1) {
        (*ptr)++;  // skip magic byte
        (*remaining)--;
        if (*remaining < 4) return NULL;
        level = read_uint16(ptr);
        *remaining -= 2;
        num_entries = read_uint16(ptr);
        *remaining -= 2;
    } else {
        // Old format: starts with uint16_t num_entries
        if (*remaining < 2) return NULL;
        num_entries = read_uint16(ptr);
        *remaining -= 2;
    }

    // Allocate locations array
    *locations = get_memory(num_entries * sizeof(node_location_t));
    *num_locations = num_entries;

    bnode_t* node = bnode_create_with_level(btree_node_size, level);
    if (node == NULL) {
        free(*locations);
        *locations = NULL;
        *num_locations = 0;
        return NULL;
    }

    for (uint16_t i = 0; i < num_entries; i++) {
        bnode_entry_t entry;
        memset(&entry, 0, sizeof(entry));

        // Read chunk
        if (*remaining < chunk_size) goto fail;
        uint8_t* chunk_buf = get_memory(chunk_size);
        read_bytes(ptr, chunk_buf, chunk_size);
        *remaining -= chunk_size;
        chunk_t* key = chunk_deserialize(chunk_buf, chunk_size);
        if (key == NULL) {
            free(chunk_buf);
            goto fail;
        }
        bnode_entry_set_key(&entry, key);
        chunk_destroy(key);
        free(chunk_buf);

        if (is_v2 || is_v1) {
            if (*remaining < 1) goto fail;
            uint8_t flags = read_uint8(ptr);
            (*remaining)--;
            entry.has_value = (flags & 0x01) != 0;
            entry.is_bnode_child = (flags & 0x02) != 0;
            entry.has_versions = (flags & 0x04) != 0;

            if (entry.has_value) {
                if (entry.has_versions) {
                    // MVCC: Read version chain
                    if (*remaining < 2) goto fail;
                    uint16_t version_count = read_uint16(ptr);
                    *remaining -= 2;
                    entry.versions = NULL;
                    version_entry_t* prev_version = NULL;

                    for (uint16_t j = 0; j < version_count; j++) {
                        if (*remaining < 1 + 24 + 4) goto fail;  // is_deleted + txn_id + ident_len
                        uint8_t is_deleted = read_uint8(ptr);
                        (*remaining)--;

                        transaction_id_t txn_id;
                        txn_id.time = read_uint64(ptr);
                        txn_id.nanos = read_uint64(ptr);
                        txn_id.count = read_uint64(ptr);
                        *remaining -= 24;

                        identifier_t* val = NULL;
                        uint32_t ident_len = read_uint32(ptr);
                        *remaining -= 4;
                        if (ident_len > 0 && !is_deleted) {
                            if (*remaining < ident_len) goto fail;
                            uint8_t* ident_buf = get_memory(ident_len);
                            read_bytes(ptr, ident_buf, ident_len);
                            *remaining -= ident_len;

                            buffer_t* data_buf = buffer_create_from_existing_memory(ident_buf, ident_len);
                            if (data_buf == NULL) {
                                free(ident_buf);
                                goto fail;
                            }
                            val = identifier_create(data_buf, chunk_size);
                            buffer_destroy(data_buf);
                            if (val == NULL) {
                                goto fail;
                            }
                        }

                        version_entry_t* version = version_entry_create(txn_id, val, is_deleted);
                        if (version == NULL) {
                            if (val != NULL) identifier_destroy(val);
                            goto fail;
                        }
                        if (entry.versions == NULL) {
                            entry.versions = version;
                        } else {
                            prev_version->next = version;
                            version->prev = prev_version;
                        }
                        prev_version = version;
                    }
                    (*locations)[i].offset = 0;
                } else {
                    // Legacy single value
                    if (*remaining < 4) goto fail;
                    uint32_t ident_len = read_uint32(ptr);
                    *remaining -= 4;
                    if (ident_len > 0) {
                        if (*remaining < ident_len) goto fail;
                        uint8_t* ident_buf = get_memory(ident_len);
                        read_bytes(ptr, ident_buf, ident_len);
                        *remaining -= ident_len;

                        buffer_t* data_buf = buffer_create_from_existing_memory(ident_buf, ident_len);
                        if (data_buf == NULL) {
                            free(ident_buf);
                            goto fail;
                        }
                        entry.value = identifier_create(data_buf, chunk_size);
                        buffer_destroy(data_buf);
                        if (entry.value == NULL) {
                            goto fail;
                        }
                    } else {
                        entry.value = NULL;
                    }
                    (*locations)[i].offset = 0;
                }
            } else if (entry.is_bnode_child) {
                if (is_v2) {
                    // V2: inline child bnode with length prefix
                    if (*remaining < 4) goto fail;
                    uint32_t child_size = read_uint32(ptr);
                    *remaining -= 4;

                    if (child_size > 0 && *remaining >= child_size) {
                        // Save current position to advance after child deserialization
                        uint8_t* child_start = *ptr;
                        size_t child_remaining = child_size;

                        node_location_t* child_locs = NULL;
                        size_t child_num_locs = 0;
                        bnode_t* child_bnode = bnode_deserialize_recursive(
                            ptr, &child_remaining, chunk_size, btree_node_size,
                            &child_locs, &child_num_locs);
                        free(child_locs);

                        if (child_bnode != NULL) {
                            entry.child_bnode = child_bnode;
                        }

                        // Advance past any remaining child bytes
                        size_t consumed = (size_t)(*ptr - child_start);
                        if (consumed < child_size) {
                            *ptr += (child_size - consumed);
                            *remaining -= (child_size - consumed);
                        }
                        *remaining -= consumed;
                    }
                } else {
                    // V1: section reference placeholders (now stored as disk_offset)
                    if (*remaining < 16) goto fail;
                    entry.child_disk_offset = read_uint64(ptr);
                    *remaining -= 8;
                    uint64_t reserved = read_uint64(ptr);  // Was block_index
                    *remaining -= 8;
                    (void)reserved;
                }
                (*locations)[i].offset = entry.child_disk_offset;
            } else {
                // Child hbtrie_node location
                if (*remaining < 16) goto fail;
                entry.child_disk_offset = read_uint64(ptr);
                *remaining -= 8;
                uint64_t reserved = read_uint64(ptr);  // Was block_index
                *remaining -= 8;
                (void)reserved;
                (*locations)[i].offset = entry.child_disk_offset;
            }
        } else {
            // Old format: has_value was a uint8_t
            if (*remaining < 1) goto fail;
            entry.has_value = read_uint8(ptr);
            (*remaining)--;

            if (entry.has_value) {
                if (*remaining < 4) goto fail;
                uint32_t ident_len = read_uint32(ptr);
                *remaining -= 4;
                if (ident_len > 0) {
                    if (*remaining < ident_len) goto fail;
                    uint8_t* ident_buf = get_memory(ident_len);
                    read_bytes(ptr, ident_buf, ident_len);
                    *remaining -= ident_len;

                    buffer_t* data_buf = buffer_create_from_existing_memory(ident_buf, ident_len);
                    if (data_buf == NULL) {
                        free(ident_buf);
                        goto fail;
                    }
                    entry.value = identifier_create(data_buf, chunk_size);
                    buffer_destroy(data_buf);
                    if (entry.value == NULL) {
                        goto fail;
                    }
                } else {
                    entry.value = NULL;
                }
                (*locations)[i].offset = 0;
            } else {
                if (*remaining < 16) goto fail;
                uint64_t loc_offset = read_uint64(ptr);
                *remaining -= 8;
                uint64_t reserved = read_uint64(ptr);  // Was block_index
                *remaining -= 8;
                (void)reserved;
                (*locations)[i].offset = loc_offset;
                entry.child_disk_offset = loc_offset;
                entry.child = NULL;
            }
        }

        // Insert entry
        bnode_insert(node, &entry);
    }

    return node;

fail:
    bnode_destroy_tree(node);
    free(*locations);
    *locations = NULL;
    *num_locations = 0;
    return NULL;
}

// Forward declaration for V3 deserializer
static bnode_t* bnode_deserialize_v3_impl(uint8_t** ptr, size_t* remaining,
                                           uint8_t chunk_size, uint32_t btree_node_size,
                                           node_location_t** locations, size_t* num_locations);

// B+tree node deserialization (public API)
bnode_t* bnode_deserialize(uint8_t* buf, size_t len, uint8_t chunk_size,
                           uint32_t btree_node_size,
                           node_location_t** locations, size_t* num_locations) {
    if (buf == NULL || len < 1) {
        return NULL;
    }

    // Check if V3 format
    if (buf[0] == BNODE_SERIALIZE_MAGIC_V3) {
        uint8_t* ptr = buf;
        size_t remaining = len;
        return bnode_deserialize_v3_impl(&ptr, &remaining, chunk_size, btree_node_size,
                                          locations, num_locations);
    }

    uint8_t* ptr = buf;
    size_t remaining = len;
    return bnode_deserialize_recursive(&ptr, &remaining, chunk_size, btree_node_size,
                                        locations, num_locations);
}

// ============================================================================
// V3 Serializer: flat per-bnode with file_offset references
// ============================================================================

// V3 entries serializer: same as V2 for values and inline bnodes,
// but non-inline children use child_disk_offset (8 bytes) instead of
// section_id + block_index.
static void bnode_serialize_entries_v3(bnode_t* node, uint8_t chunk_size, serialize_buf_t* sb) {
    for (int i = 0; i < node->entries.length; i++) {
        bnode_entry_t* entry = &node->entries.data[i];

        // Write chunk
        chunk_t* key = bnode_entry_get_key(entry);
        if (key != NULL) {
            sbuf_write_bytes(sb, key->data, chunk_size);
        } else {
            uint8_t zero[8] = {0};
            sbuf_write_bytes(sb, zero, chunk_size);
        }

        // Write flags byte
        uint8_t flags = 0;
        if (entry->has_value) flags |= 0x01;
        if (entry->is_bnode_child) flags |= 0x02;
        if (entry->has_value && entry->has_versions) flags |= 0x04;
        sbuf_write_uint8(sb, flags);

        if (entry->has_value) {
            if (entry->has_versions && entry->versions != NULL) {
                // MVCC: Serialize version chain (same as V2)
                size_t version_count = 0;
                version_entry_t* current = entry->versions;
                while (current != NULL) {
                    version_count++;
                    current = current->next;
                }
                sbuf_write_uint16(sb, (uint16_t)version_count);

                current = entry->versions;
                while (current != NULL) {
                    sbuf_write_uint8(sb, current->is_deleted);
                    sbuf_write_uint64(sb, current->txn_id.time);
                    sbuf_write_uint64(sb, current->txn_id.nanos);
                    sbuf_write_uint64(sb, current->txn_id.count);

                    if (!current->is_deleted && current->value != NULL) {
                        buffer_t* ident_buf = identifier_to_buffer(current->value);
                        if (ident_buf != NULL) {
                            sbuf_write_uint32(sb, (uint32_t)ident_buf->size);
                            if (ident_buf->size > 0) {
                                sbuf_write_bytes(sb, ident_buf->data, ident_buf->size);
                            }
                            buffer_destroy(ident_buf);
                        } else {
                            sbuf_write_uint32(sb, 0);
                        }
                    } else {
                        sbuf_write_uint32(sb, 0);
                    }
                    current = current->next;
                }
            } else {
                // Legacy: single value (same as V2)
                identifier_t* ident = entry->value;
                if (ident != NULL) {
                    buffer_t* ident_buf = identifier_to_buffer(ident);
                    if (ident_buf != NULL) {
                        sbuf_write_uint32(sb, (uint32_t)ident_buf->size);
                        if (ident_buf->size > 0) {
                            sbuf_write_bytes(sb, ident_buf->data, ident_buf->size);
                        }
                        buffer_destroy(ident_buf);
                    } else {
                        sbuf_write_uint32(sb, 0);
                    }
                } else {
                    sbuf_write_uint32(sb, 0);
                }
            }
        } else {
            // V3: All non-value entries (both is_bnode_child and hbtrie_node children)
            // store child_disk_offset (8 bytes) — no inline bnodes
            sbuf_write_uint64(sb, entry->child_disk_offset);
        }
    }
}

int bnode_serialize_v3(bnode_t* node, uint8_t chunk_size, uint8_t** buf, size_t* len) {
    if (node == NULL || buf == NULL || len == NULL) {
        return -1;
    }

    serialize_buf_t sb;
    sbuf_init(&sb);

    // Write V3 magic byte
    sbuf_write_uint8(&sb, BNODE_SERIALIZE_MAGIC_V3);

    // Write level
    sbuf_write_uint16(&sb, atomic_load(&node->level));

    // Write number of entries
    sbuf_write_uint16(&sb, (uint16_t)node->entries.length);

    // Write entries
    bnode_serialize_entries_v3(node, chunk_size, &sb);

    *buf = sb.data;
    *len = sb.size;
    return 0;
}

// V3 deserializer implementation
static bnode_t* bnode_deserialize_v3_impl(uint8_t** ptr, size_t* remaining,
                                            uint8_t chunk_size, uint32_t btree_node_size,
                                            node_location_t** locations, size_t* num_locations) {
    if (*remaining < 1) return NULL;

    // Skip V3 magic byte
    (*ptr)++;
    (*remaining)--;

    if (*remaining < 4) return NULL;
    uint16_t level = read_uint16(ptr);
    *remaining -= 2;
    uint16_t num_entries = read_uint16(ptr);
    *remaining -= 2;

    // Allocate locations array
    *locations = get_memory(num_entries * sizeof(node_location_t));
    *num_locations = num_entries;

    bnode_t* node = bnode_create_with_level(btree_node_size, level);
    if (node == NULL) {
        free(*locations);
        *locations = NULL;
        *num_locations = 0;
        return NULL;
    }

    for (uint16_t i = 0; i < num_entries; i++) {
        bnode_entry_t entry;
        memset(&entry, 0, sizeof(entry));

        // Read chunk
        if (*remaining < chunk_size) goto fail;
        uint8_t* chunk_buf = get_memory(chunk_size);
        read_bytes(ptr, chunk_buf, chunk_size);
        *remaining -= chunk_size;
        chunk_t* key = chunk_deserialize(chunk_buf, chunk_size);
        if (key == NULL) {
            free(chunk_buf);
            goto fail;
        }
        bnode_entry_set_key(&entry, key);
        chunk_destroy(key);
        free(chunk_buf);

        // Read flags
        if (*remaining < 1) goto fail;
        uint8_t flags = read_uint8(ptr);
        (*remaining)--;
        entry.has_value = (flags & 0x01) != 0;
        entry.is_bnode_child = (flags & 0x02) != 0;
        entry.has_versions = (flags & 0x04) != 0;

        if (entry.has_value) {
            if (entry.has_versions) {
                // MVCC: Read version chain
                if (*remaining < 2) goto fail;
                uint16_t version_count = read_uint16(ptr);
                *remaining -= 2;
                entry.versions = NULL;
                version_entry_t* prev_version = NULL;

                for (uint16_t j = 0; j < version_count; j++) {
                    if (*remaining < 1 + 24 + 4) goto fail;
                    uint8_t is_deleted = read_uint8(ptr);
                    (*remaining)--;

                    transaction_id_t txn_id;
                    txn_id.time = read_uint64(ptr);
                    txn_id.nanos = read_uint64(ptr);
                    txn_id.count = read_uint64(ptr);
                    *remaining -= 24;

                    identifier_t* val = NULL;
                    uint32_t ident_len = read_uint32(ptr);
                    *remaining -= 4;
                    if (ident_len > 0 && !is_deleted) {
                        if (*remaining < ident_len) goto fail;
                        uint8_t* ident_buf = get_memory(ident_len);
                        read_bytes(ptr, ident_buf, ident_len);
                        *remaining -= ident_len;

                        buffer_t* data_buf = buffer_create_from_existing_memory(ident_buf, ident_len);
                        if (data_buf == NULL) {
                            free(ident_buf);
                            goto fail;
                        }
                        val = identifier_create(data_buf, chunk_size);
                        buffer_destroy(data_buf);
                        if (val == NULL) {
                            goto fail;
                        }
                    }

                    version_entry_t* version = version_entry_create(txn_id, val, is_deleted);
                    if (version == NULL) {
                        if (val != NULL) identifier_destroy(val);
                        goto fail;
                    }
                    if (entry.versions == NULL) {
                        entry.versions = version;
                    } else {
                        prev_version->next = version;
                        version->prev = prev_version;
                    }
                    prev_version = version;
                }
                (*locations)[i].offset = 0;
            } else {
                // Legacy single value
                if (*remaining < 4) goto fail;
                uint32_t ident_len = read_uint32(ptr);
                *remaining -= 4;
                if (ident_len > 0) {
                    if (*remaining < ident_len) goto fail;
                    uint8_t* ident_buf = get_memory(ident_len);
                    read_bytes(ptr, ident_buf, ident_len);
                    *remaining -= ident_len;

                    buffer_t* data_buf = buffer_create_from_existing_memory(ident_buf, ident_len);
                    if (data_buf == NULL) {
                        free(ident_buf);
                        goto fail;
                    }
                    entry.value = identifier_create(data_buf, chunk_size);
                    buffer_destroy(data_buf);
                    if (entry.value == NULL) {
                        goto fail;
                    }
                } else {
                    entry.value = NULL;
                }
                (*locations)[i].offset = 0;
            }
        } else {
            // V3: All non-value entries store child_disk_offset (8 bytes)
            // Both is_bnode_child and hbtrie_node children are lazy-loaded
            if (*remaining < 8) goto fail;
            entry.child_disk_offset = read_uint64(ptr);
            *remaining -= 8;
            // child_bnode and child stay NULL for lazy loading
            (*locations)[i].offset = entry.child_disk_offset;
        }

        // Insert entry
        bnode_insert(node, &entry);
    }

    return node;

fail:
    bnode_destroy_tree(node);
    free(*locations);
    *locations = NULL;
    *num_locations = 0;
    return NULL;
}

// V3 deserializer (public API)
bnode_t* bnode_deserialize_v3(uint8_t* buf, size_t len, uint8_t chunk_size,
                               uint32_t btree_node_size,
                               node_location_t** locations, size_t* num_locations) {
    if (buf == NULL || len < 1) {
        return NULL;
    }

    uint8_t* ptr = buf;
    size_t remaining = len;
    return bnode_deserialize_v3_impl(&ptr, &remaining, chunk_size, btree_node_size,
                                      locations, num_locations);
}