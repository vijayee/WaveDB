//
// Created by victor on 03/13/26.
//

#include "node_serializer.h"
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
    if (chunk == NULL || chunk->data == NULL || buf == NULL) {
        return -1;
    }

    *buf = get_memory(chunk_size);
    memcpy(*buf, chunk->data->data, chunk_size);
    return 0;
}

// Chunk deserialization
chunk_t* chunk_deserialize(uint8_t* buf, uint8_t chunk_size) {
    if (buf == NULL) {
        return NULL;
    }

    // Create buffer from existing memory (takes ownership)
    uint8_t* data_copy = get_memory(chunk_size);
    memcpy(data_copy, buf, chunk_size);

    buffer_t* data_buf = buffer_create_from_existing_memory(data_copy, chunk_size);
    if (data_buf == NULL) {
        free(data_copy);
        return NULL;
    }

    // Create chunk (shares buffer reference)
    chunk_t* chunk = get_clear_memory(sizeof(chunk_t));
    chunk->data = data_buf;

    return chunk;
}

// B+tree node serialization
int bnode_serialize(bnode_t* node, uint8_t chunk_size, uint8_t** buf, size_t* len) {
    if (node == NULL || buf == NULL || len == NULL) {
        return -1;
    }

    // Calculate total size needed
    size_t total_size = sizeof(uint16_t);  // Number of entries

    for (int i = 0; i < node->entries.length; i++) {
        bnode_entry_t* entry = &node->entries.data[i];

        // Chunk data
        total_size += chunk_size;

        // has_value flag
        total_size += sizeof(uint8_t);

        if (entry->has_value) {
            // Identifier size placeholder (we'll need to serialize the identifier)
            identifier_t* ident = entry->value;
            if (ident != NULL) {
                buffer_t* ident_buf = identifier_to_buffer(ident);
                if (ident_buf != NULL) {
                    total_size += sizeof(uint32_t) + ident_buf->size;
                    buffer_destroy(ident_buf);
                }
            } else {
                total_size += sizeof(uint32_t);  // Just length field
            }
        } else {
            // Child location (section_id + block_index)
            total_size += sizeof(uint64_t) * 2;
        }
    }

    *buf = get_memory(total_size);
    *len = total_size;

    uint8_t* ptr = *buf;

    // Write number of entries
    write_uint16(&ptr, (uint16_t)node->entries.length);

    // Write each entry
    for (int i = 0; i < node->entries.length; i++) {
        bnode_entry_t* entry = &node->entries.data[i];

        // Write chunk
        if (entry->key != NULL && entry->key->data != NULL) {
            write_bytes(&ptr, entry->key->data->data, chunk_size);
        } else {
            // Write zeros for null chunk
            memset(ptr, 0, chunk_size);
            ptr += chunk_size;
        }

        // Write has_value flag
        write_uint8(&ptr, entry->has_value);

        if (entry->has_value) {
            // Serialize identifier
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
        } else {
            // Write child location (placeholders for now - Phase 2 will fill these)
            // Phase 2 will serialize actual section_id and block_index
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
    if (buf == NULL || len < sizeof(uint16_t)) {
        return NULL;
    }

    uint8_t* ptr = buf;

    // Read number of entries
    uint16_t num_entries = read_uint16(&ptr);

    // Allocate locations array
    *locations = get_memory(num_entries * sizeof(node_location_t));
    *num_locations = num_entries;

    // Create node
    bnode_t* node = bnode_create(btree_node_size);
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

        // Read has_value flag
        entry.has_value = read_uint8(&ptr);

        if (entry.has_value) {
            // Read identifier length
            uint32_t ident_len = read_uint32(&ptr);
            if (ident_len > 0) {
                // Read identifier data
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

                // Location not applicable for values
                (*locations)[i].section_id = 0;
                (*locations)[i].block_index = 0;
            } else {
                entry.value = NULL;
                (*locations)[i].section_id = 0;
                (*locations)[i].block_index = 0;
            }
        } else {
            // Read child location
            (*locations)[i].section_id = read_uint64(&ptr);
            (*locations)[i].block_index = read_uint64(&ptr);
            entry.child = NULL;  // Will be loaded lazily in Phase 2
        }

        // Insert entry
        bnode_insert(node, &entry);
    }

    return node;
}