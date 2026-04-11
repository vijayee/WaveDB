//
// Created by victor on 03/13/26.
//

#include "node_serializer.h"
#include "../HBTrie/identifier.h"
#include "../Util/allocator.h"
#include <string.h>
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

int bnode_serialize(bnode_t* node, uint8_t chunk_size, uint8_t** buf, size_t* len) {
    if (node == NULL || buf == NULL || len == NULL) {
        return -1;
    }

    // Calculate total size needed
    size_t total_size = 1 + sizeof(uint16_t) + sizeof(uint16_t);  // magic + level + num_entries

    for (int i = 0; i < node->entries.length; i++) {
        bnode_entry_t* entry = &node->entries.data[i];

        // Chunk data
        total_size += chunk_size;

        // Flags byte
        total_size += sizeof(uint8_t);

        if (entry->has_value) {
            if (entry->has_versions && entry->versions != NULL) {
                // MVCC: version chain
                // Count versions
                size_t version_count = 0;
                version_entry_t* current = entry->versions;
                while (current != NULL) {
                    version_count++;
                    current = current->next;
                }
                total_size += sizeof(uint16_t);  // version_count
                current = entry->versions;
                while (current != NULL) {
                    // is_deleted + txn_id (3 uint64) + identifier
                    total_size += sizeof(uint8_t);  // is_deleted
                    total_size += sizeof(uint64_t) * 3;  // txn_id
                    if (!current->is_deleted && current->value != NULL) {
                        buffer_t* ident_buf = identifier_to_buffer(current->value);
                        if (ident_buf != NULL) {
                            total_size += sizeof(uint32_t) + ident_buf->size;
                            buffer_destroy(ident_buf);
                        } else {
                            total_size += sizeof(uint32_t);
                        }
                    } else {
                        total_size += sizeof(uint32_t);  // 0 length
                    }
                    current = current->next;
                }
            } else {
                // Legacy: single value
                identifier_t* ident = entry->value;
                if (ident != NULL) {
                    buffer_t* ident_buf = identifier_to_buffer(ident);
                    if (ident_buf != NULL) {
                        total_size += sizeof(uint32_t) + ident_buf->size;
                        buffer_destroy(ident_buf);
                    } else {
                        total_size += sizeof(uint32_t);
                    }
                } else {
                    total_size += sizeof(uint32_t);  // Just length field
                }
            }
        } else if (entry->is_bnode_child) {
            // Internal bnode child - will be serialized recursively by caller
            // We store a placeholder for the child bnode
            total_size += sizeof(uint64_t) * 2;  // section_id + block_index placeholder
        } else {
            // Child hbtrie_node location
            total_size += sizeof(uint64_t) * 2;
        }
    }

    *buf = get_memory(total_size);
    *len = total_size;

    uint8_t* ptr = *buf;

    // Write magic byte
    write_uint8(&ptr, BNODE_SERIALIZE_MAGIC);

    // Write level
    write_uint16(&ptr, node->level);

    // Write number of entries
    write_uint16(&ptr, (uint16_t)node->entries.length);

    // Write each entry
    for (int i = 0; i < node->entries.length; i++) {
        bnode_entry_t* entry = &node->entries.data[i];

        // Write chunk
        if (entry->key != NULL) {
            write_bytes(&ptr, entry->key->data, chunk_size);
        } else {
            memset(ptr, 0, chunk_size);
            ptr += chunk_size;
        }

        // Write flags byte
        uint8_t flags = 0;
        if (entry->has_value) flags |= 0x01;
        if (entry->is_bnode_child) flags |= 0x02;
        if (entry->has_value && entry->has_versions) flags |= 0x04;
        write_uint8(&ptr, flags);

        if (entry->has_value) {
            if (entry->has_versions && entry->versions != NULL) {
                // MVCC: Serialize version chain
                size_t version_count = 0;
                version_entry_t* current = entry->versions;
                while (current != NULL) {
                    version_count++;
                    current = current->next;
                }
                write_uint16(&ptr, (uint16_t)version_count);

                current = entry->versions;
                while (current != NULL) {
                    // is_deleted
                    write_uint8(&ptr, current->is_deleted);

                    // txn_id
                    write_uint64(&ptr, current->txn_id.time);
                    write_uint64(&ptr, current->txn_id.nanos);
                    write_uint64(&ptr, current->txn_id.count);

                    // value (or 0 length if deleted or null)
                    if (!current->is_deleted && current->value != NULL) {
                        buffer_t* ident_buf = identifier_to_buffer(current->value);
                        if (ident_buf != NULL) {
                            write_uint32(&ptr, (uint32_t)ident_buf->size);
                            if (ident_buf->size > 0) {
                                write_bytes(&ptr, ident_buf->data, ident_buf->size);
                            }
                            buffer_destroy(ident_buf);
                        } else {
                            write_uint32(&ptr, 0);
                        }
                    } else {
                        write_uint32(&ptr, 0);
                    }
                    current = current->next;
                }
            } else {
                // Legacy: single value
                identifier_t* ident = entry->value;
                if (ident != NULL) {
                    buffer_t* ident_buf = identifier_to_buffer(ident);
                    if (ident_buf != NULL) {
                        write_uint32(&ptr, (uint32_t)ident_buf->size);
                        if (ident_buf->size > 0) {
                            write_bytes(&ptr, ident_buf->data, ident_buf->size);
                        }
                        buffer_destroy(ident_buf);
                    } else {
                        write_uint32(&ptr, 0);
                    }
                } else {
                    write_uint32(&ptr, 0);
                }
            }
        } else if (entry->is_bnode_child) {
            // Internal bnode child - write location placeholders
            write_uint64(&ptr, entry->child_section_id);
            write_uint64(&ptr, entry->child_block_index);
        } else {
            // Child hbtrie_node location
            write_uint64(&ptr, entry->child_section_id);
            write_uint64(&ptr, entry->child_block_index);
        }
    }

    return 0;
}

// B+tree node deserialization
bnode_t* bnode_deserialize(uint8_t* buf, size_t len, uint8_t chunk_size,
                           uint32_t btree_node_size,
                           node_location_t** locations, size_t* num_locations) {
    if (buf == NULL || len < 1) {
        return NULL;
    }

    uint8_t* ptr = buf;
    uint16_t level = 1;  // Default for old format

    // Check for magic byte (new format)
    uint8_t first_byte = *ptr;
    int is_new_format = (first_byte == BNODE_SERIALIZE_MAGIC);

    uint16_t num_entries;
    if (is_new_format) {
        // New format: magic + level + num_entries
        ptr++;  // skip magic byte
        if (len < 1 + sizeof(uint16_t) + sizeof(uint16_t)) {
            return NULL;
        }
        level = read_uint16(&ptr);
        num_entries = read_uint16(&ptr);
    } else {
        // Old format: starts with uint16_t num_entries in network byte order
        if (len < sizeof(uint16_t)) {
            return NULL;
        }
        num_entries = read_uint16(&ptr);
    }

    // Allocate locations array
    *locations = get_memory(num_entries * sizeof(node_location_t));
    *num_locations = num_entries;

    // Create node with the correct level
    bnode_t* node = bnode_create_with_level(btree_node_size, level);
    if (node == NULL) {
        free(*locations);
        *locations = NULL;
        *num_locations = 0;
        return NULL;
    }

    // Read each entry
    for (uint16_t i = 0; i < num_entries; i++) {
        bnode_entry_t entry;
        memset(&entry, 0, sizeof(entry));

        // Read chunk
        uint8_t* chunk_buf = get_memory(chunk_size);
        read_bytes(&ptr, chunk_buf, chunk_size);
        entry.key = chunk_deserialize(chunk_buf, chunk_size);
        if (entry.key == NULL) {
            free(chunk_buf);
            bnode_destroy(node);
            free(*locations);
            *locations = NULL;
            *num_locations = 0;
            return NULL;
        }
        free(chunk_buf);

        if (is_new_format) {
            // New format: read flags byte
            uint8_t flags = read_uint8(&ptr);
            entry.has_value = (flags & 0x01) != 0;
            entry.is_bnode_child = (flags & 0x02) != 0;
            entry.has_versions = (flags & 0x04) != 0;

            if (entry.has_value) {
                if (entry.has_versions) {
                    // MVCC: Read version chain
                    uint16_t version_count = read_uint16(&ptr);
                    entry.versions = NULL;
                    version_entry_t* prev_version = NULL;

                    for (uint16_t j = 0; j < version_count; j++) {
                        uint8_t is_deleted = read_uint8(&ptr);

                        transaction_id_t txn_id;
                        txn_id.time = read_uint64(&ptr);
                        txn_id.nanos = read_uint64(&ptr);
                        txn_id.count = read_uint64(&ptr);

                        identifier_t* val = NULL;
                        uint32_t ident_len = read_uint32(&ptr);
                        if (ident_len > 0 && !is_deleted) {
                            uint8_t* ident_buf = get_memory(ident_len);
                            read_bytes(&ptr, ident_buf, ident_len);

                            buffer_t* data_buf = buffer_create_from_existing_memory(ident_buf, ident_len);
                            if (data_buf == NULL) {
                                free(ident_buf);
                                chunk_destroy(entry.key);
                                bnode_destroy_tree(node);
                                free(*locations);
                                *locations = NULL;
                                *num_locations = 0;
                                return NULL;
                            }

                            val = identifier_create(data_buf, chunk_size);
                            if (val == NULL) {
                                buffer_destroy(data_buf);
                                chunk_destroy(entry.key);
                                bnode_destroy_tree(node);
                                free(*locations);
                                *locations = NULL;
                                *num_locations = 0;
                                return NULL;
                            }
                        }

                        version_entry_t* version = version_entry_create(txn_id, val, is_deleted);
                        if (version == NULL) {
                            if (val != NULL) identifier_destroy(val);
                            chunk_destroy(entry.key);
                            bnode_destroy_tree(node);
                            free(*locations);
                            *locations = NULL;
                            *num_locations = 0;
                            return NULL;
                        }

                        if (entry.versions == NULL) {
                            entry.versions = version;
                        } else {
                            prev_version->next = version;
                            version->prev = prev_version;
                        }
                        prev_version = version;
                    }

                    (*locations)[i].section_id = 0;
                    (*locations)[i].block_index = 0;
                } else {
                    // Legacy single value
                    uint32_t ident_len = read_uint32(&ptr);
                    if (ident_len > 0) {
                        uint8_t* ident_buf = get_memory(ident_len);
                        read_bytes(&ptr, ident_buf, ident_len);

                        buffer_t* data_buf = buffer_create_from_existing_memory(ident_buf, ident_len);
                        if (data_buf == NULL) {
                            free(ident_buf);
                            chunk_destroy(entry.key);
                            bnode_destroy(node);
                            free(*locations);
                            *locations = NULL;
                            *num_locations = 0;
                            return NULL;
                        }

                        entry.value = identifier_create(data_buf, chunk_size);
                        if (entry.value == NULL) {
                            buffer_destroy(data_buf);
                            chunk_destroy(entry.key);
                            bnode_destroy(node);
                            free(*locations);
                            *locations = NULL;
                            *num_locations = 0;
                            return NULL;
                        }
                    } else {
                        entry.value = NULL;
                    }

                    (*locations)[i].section_id = 0;
                    (*locations)[i].block_index = 0;
                }
            } else if (entry.is_bnode_child) {
                // Internal bnode child - location placeholder
                entry.child_section_id = read_uint64(&ptr);
                entry.child_block_index = read_uint64(&ptr);
                (*locations)[i].section_id = entry.child_section_id;
                (*locations)[i].block_index = entry.child_block_index;
            } else {
                // Child hbtrie_node location
                entry.child_section_id = read_uint64(&ptr);
                entry.child_block_index = read_uint64(&ptr);
                (*locations)[i].section_id = entry.child_section_id;
                (*locations)[i].block_index = entry.child_block_index;
            }
        } else {
            // Old format: has_value was a uint8_t
            entry.has_value = first_byte;  // Already read as part of num_entries parsing
            // In old format, first_byte was the high byte of num_entries which we already
            // handled above. The actual has_value flag is next.
            entry.has_value = read_uint8(&ptr);

            if (entry.has_value) {
                uint32_t ident_len = read_uint32(&ptr);
                if (ident_len > 0) {
                    uint8_t* ident_buf = get_memory(ident_len);
                    read_bytes(&ptr, ident_buf, ident_len);

                    buffer_t* data_buf = buffer_create_from_existing_memory(ident_buf, ident_len);
                    if (data_buf == NULL) {
                        free(ident_buf);
                        chunk_destroy(entry.key);
                        bnode_destroy(node);
                        free(*locations);
                        *locations = NULL;
                        *num_locations = 0;
                        return NULL;
                    }

                    entry.value = identifier_create(data_buf, chunk_size);
                    if (entry.value == NULL) {
                        buffer_destroy(data_buf);
                        chunk_destroy(entry.key);
                        bnode_destroy(node);
                        free(*locations);
                        *locations = NULL;
                        *num_locations = 0;
                        return NULL;
                    }
                } else {
                    entry.value = NULL;
                }

                (*locations)[i].section_id = 0;
                (*locations)[i].block_index = 0;
            } else {
                (*locations)[i].section_id = read_uint64(&ptr);
                (*locations)[i].block_index = read_uint64(&ptr);
                entry.child = NULL;
            }
        }

        // Insert entry
        bnode_insert(node, &entry);
    }

    return node;
}