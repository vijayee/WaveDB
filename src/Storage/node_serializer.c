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

// Chunk serialization
int chunk_serialize(chunk_t* chunk, uint8_t chunk_size, uint8_t** buf) {
    if (chunk == NULL || chunk->data == NULL) {
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

    // Create chunk from existing memory (takes ownership)
    buffer_t* data = buffer_create_from_existing_memory(buf, chunk_size);
    if (data == NULL) {
        return NULL;
    }

    // Create chunk (takes ownership of buffer)
    chunk_t* chunk = get_clear_memory(sizeof(chunk_t));
    chunk->data = data;
    // Note: chunk doesn't have refcounter, it's a simple struct

    return chunk;
}

// Identifier serialization
int identifier_serialize(identifier_t* ident, uint8_t** buf, size_t* len) {
    if (ident == NULL || buf == NULL || len == NULL) {
        return -1;
    }

    // Calculate total size: length (4 bytes) + data
    size_t total_len = sizeof(uint32_t) + ident->length;
    *buf = get_memory(total_len);
    *len = total_len;

    uint8_t* ptr = *buf;

    // Write length
    write_uint32(&ptr, (uint32_t)ident->length);

    // Write data (copy from identifier's buffer)
    if (ident->length > 0 && ident->data != NULL) {
        memcpy(ptr, ident->data->data, ident->length);
    }

    return 0;
}

// Identifier deserialization
identifier_t* identifier_deserialize(uint8_t* buf, size_t len) {
    if (buf == NULL || len < sizeof(uint32_t)) {
        return NULL;
    }

    uint8_t* ptr = buf;

    // Read length
    uint32_t length = read_uint32(&ptr);

    if (sizeof(uint32_t) + length != len) {
        return NULL;  // Malformed data
    }

    // Allocate identifier
    identifier_t* ident = get_clear_memory(sizeof(identifier_t));
    refcounter_init((refcounter_t*) ident);

    ident->length = length;

    // Create buffer and copy data
    if (length > 0) {
        ident->data = buffer_create(length);
        if (ident->data == NULL) {
            free_memory(ident);
            return NULL;
        }
        memcpy(ident->data->data, ptr, length);
    }

    return ident;
}

// B+tree node serialization
int bnode_serialize(bnode_t* node, uint8_t chunk_size, uint8_t** buf, size_t* len) {
    if (node == NULL || buf == NULL || len == NULL) {
        return -1;
    }

    // Calculate total size needed
    size_t total_size = sizeof(uint16_t);  // Number of entries

    for (size_t i = 0; i < node->entries.length; i++) {
        bnode_entry_t* entry = &node->entries.data[i];

        // Chunk data
        total_size += chunk_size;

        // has_value flag
        total_size += sizeof(uint8_t);

        if (entry->has_value) {
            // Identifier length + data
            identifier_t* ident = entry->value;
            total_size += sizeof(uint32_t);
            if (ident != NULL && ident->length > 0) {
                total_size += ident->length;
            }
        } else {
            // Child location (will be filled in during traversal)
            // For now, we serialize child nodes recursively
            // This will be changed to location tuples in Phase 2
            total_size += sizeof(uint64_t) * 2;  // section_id + block_index
        }
    }

    *buf = get_memory(total_size);
    *len = total_size;

    uint8_t* ptr = *buf;

    // Write number of entries
    write_uint16(&ptr, (uint16_t)node->entries.length);

    // Write each entry
    for (size_t i = 0; i < node->entries.length; i++) {
        bnode_entry_t* entry = &node->entries.data[i];

        // Write chunk
        if (entry->key != NULL && entry->key->data != NULL) {
            memcpy(ptr, entry->key->data->data, chunk_size);
            ptr += chunk_size;
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
                write_uint32(&ptr, (uint32_t)ident->length);
                if (ident->length > 0 && ident->data != NULL) {
                    memcpy(ptr, ident->data->data, ident->length);
                    ptr += ident->length;
                }
            } else {
                write_uint32(&ptr, 0);
            }
        } else {
            // Write placeholder for child location
            // Phase 2 will replace this with actual section_id and block_index
            write_uint64(&ptr, 0);  // section_id
            write_uint64(&ptr, 0);  // block_index
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
        free_memory(*locations);
        *locations = NULL;
        *num_locations = 0;
        return NULL;
    }

    // Read each entry
    for (size_t i = 0; i < num_entries; i++) {
        bnode_entry_t entry;

        // Read chunk
        uint8_t* chunk_buf = get_memory(chunk_size);
        memcpy(chunk_buf, ptr, chunk_size);
        entry.key = chunk_deserialize(chunk_buf, chunk_size);
        ptr += chunk_size;

        if (entry.key == NULL) {
            free_memory(chunk_buf);
            bnode_destroy(node);
            free_memory(*locations);
            *locations = NULL;
            *num_locations = 0;
            return NULL;
        }

        // Read has_value flag
        entry.has_value = read_uint8(&ptr);

        if (entry.has_value) {
            // Read identifier
            uint32_t ident_len = read_uint32(&ptr);
            if (ident_len > 0) {
                identifier_t* ident = get_clear_memory(sizeof(identifier_t));
                refcounter_init((refcounter_t*) ident);
                ident->length = ident_len;
                ident->data = buffer_create(ident_len);
                if (ident->data == NULL) {
                    free_memory(ident);
                    chunk_destroy(entry.key);
                    bnode_destroy(node);
                    free_memory(*locations);
                    *locations = NULL;
                    *num_locations = 0;
                    return NULL;
                }
                memcpy(ident->data->data, ptr, ident_len);
                ptr += ident_len;
                entry.value = ident;
            } else {
                entry.value = NULL;
            }

            // Location not applicable for values
            (*locations)[i].section_id = 0;
            (*locations)[i].block_index = 0;
        } else {
            // Read child location
            (*locations)[i].section_id = read_uint64(&ptr);
            (*locations)[i].block_index = read_uint64(&ptr);
            entry.child = NULL;  // Will be loaded lazily
        }

        // Insert entry
        bnode_insert(node, &entry);
    }

    return node;
}