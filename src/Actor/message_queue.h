//
// Created by victor on 5/6/25.
//

#ifndef OFFS_MESSAGE_QUEUE_H
#define OFFS_MESSAGE_QUEUE_H

#include "message.h"
#include "../Util/atomic_compat.h"
#include <stdbool.h>
#include <stddef.h>

#define MAILBOX_MUTE_THRESHOLD 256

typedef struct message_node_t message_node_t;
struct message_node_t {
  message_t msg;
  ATOMIC(message_node_t*) next;
};

typedef struct message_queue_t {
  ATOMIC(message_node_t*) head;
  message_node_t* tail;
  ATOMIC(size_t) size;
} message_queue_t;

void message_queue_init(message_queue_t* queue);
bool message_queue_push(message_queue_t* queue, message_node_t* first, message_node_t* last);
// Single-producer fast path. Only safe when no concurrent producers exist.
bool message_queue_push_single(message_queue_t* queue, message_node_t* first, message_node_t* last);
message_node_t* message_queue_pop(message_queue_t* queue);
bool message_queue_markempty(message_queue_t* queue);
bool message_queue_isempty(message_queue_t* queue);
void message_queue_destroy(message_queue_t* queue);

#endif // OFFS_MESSAGE_QUEUE_H
