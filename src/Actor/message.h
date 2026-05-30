//
// Minimal message.h for WaveDB — message_t struct for the lock-free message queue.
// This will be extended by Task 1.3 (Import actor core) with the full message type catalog
// and WaveDB-specific payload structs.
//

#ifndef WAVEDB_MESSAGE_H
#define WAVEDB_MESSAGE_H

#include <stdint.h>

typedef struct message_t {
  uint32_t type;
  void* payload;
  void (*payload_destroy)(void*);
} message_t;

#endif // WAVEDB_MESSAGE_H
