//
// Created by victor on 5/6/25.
//

#include "actor.h"
#include "../Scheduler/scheduler.h"
#include "../Util/allocator.h"
#include <stdbool.h>
#include <stdlib.h>

void actor_init(actor_t* actor, void* state, void (*dispatch)(void* state, message_t* msg), scheduler_pool_t* pool) {
  message_queue_init(&actor->queue);
  atomic_store(&actor->flags, 0);
  atomic_store(&actor->pressured_senders, NULL);
  actor->pool = pool;
  actor->state = state;
  actor->dispatch = dispatch;
}

void actor_destroy(actor_t* actor) {
  /* Mark the actor as destroyed so the scheduler skips it and actor_send
     drops new messages. Then drain and free the message queue. */
  atomic_fetch_or(&actor->flags, ACTOR_FLAG_DESTROY);
  /* Release any senders that were muted because of this actor. */
  if (atomic_load(&actor->flags) & ACTOR_FLAG_PRESSURED) {
    backpressure_release(actor);
  }
  message_queue_destroy(&actor->queue);
}

bool actor_send(actor_t* actor, message_t* msg) {
  if (atomic_load(&actor->flags) & ACTOR_FLAG_DESTROY) {
    /* Actor is being destroyed — drop the message and clean up the payload. */
    if (msg->payload_destroy != NULL && msg->payload != NULL) {
      msg->payload_destroy(msg->payload);
    }
    return false;
  }
  message_node_t* node = get_clear_memory(sizeof(message_node_t));
  node->msg = *msg;
  bool was_empty = message_queue_push(&actor->queue, node, node);

  /* Backpressure: if target is pressured, mute the sender. */
  if (atomic_load(&actor->flags) & ACTOR_FLAG_PRESSURED) {
    scheduler_t* self = scheduler_get_current();
    if (self != NULL && self->current != NULL && self->current != actor) {
      actor_t* sender = self->current;
      if (!(atomic_load(&sender->flags) & ACTOR_FLAG_MUTED)) {
        muted_sender_node_t* msn = get_clear_memory(sizeof(muted_sender_node_t));
        msn->sender = sender;
        do {
          msn->next = atomic_load(&actor->pressured_senders);
        } while (!atomic_compare_exchange_strong(&actor->pressured_senders, &msn->next, msn));
        atomic_fetch_or(&sender->flags, ACTOR_FLAG_MUTED);
      }
    }
  }

  /* Automatic backpressure: if mailbox exceeds threshold, mark as pressured. */
  if (atomic_load(&actor->queue.size) >= MAILBOX_MUTE_THRESHOLD) {
    if (!(atomic_load(&actor->flags) & ACTOR_FLAG_PRESSURED)) {
      atomic_fetch_or(&actor->flags, ACTOR_FLAG_PRESSURED);
    }
  }

  if (was_empty) {
    atomic_fetch_or(&actor->flags, ACTOR_FLAG_SCHEDULED);
    if (actor->pool != NULL) {
      scheduler_inject(actor->pool, actor);
    }
  }
  return was_empty;
}

bool actor_run(actor_t* actor, size_t batch_size) {
  for (size_t count = 0; count < batch_size; count++) {
    if (atomic_load(&actor->flags) & (ACTOR_FLAG_DESTROY | ACTOR_FLAG_MUTED)) {
      return false;
    }
    message_node_t* node = message_queue_pop(&actor->queue);
    if (node == NULL) {
      break;
    }
    /* Save payload info before dispatch — actor_destroy may free the node.
       A dispatch that CONSUMEs the payload will set msg->payload = NULL;
       we must respect that in the normal (non-self-destruct) path. */
    void (*payload_destroy)(void*) = node->msg.payload_destroy;
    void* payload = node->msg.payload;

    actor->dispatch(actor->state, &node->msg);

    /* If dispatch requested self-destruction, stop processing immediately.
       The node may have been freed by actor_destroy — use saved payload info.
       Dispatch functions that self-destruct do not CONSUME; they leave
       normal payload cleanup to actor_run.
       The caller (typically the scheduler) must check ACTOR_FLAG_DESTROY
       and skip further operations on this actor. */
    if (atomic_load(&actor->flags) & ACTOR_FLAG_DESTROY) {
      if (payload_destroy != NULL && payload != NULL) {
        payload_destroy(payload);
      }
      return false;
    }

    /* Node is still valid — use current msg values to respect CONSUME
       (dispatch may have set msg->payload = NULL to take ownership). */
    if (node->msg.payload_destroy != NULL && node->msg.payload != NULL) {
      node->msg.payload_destroy(node->msg.payload);
    }
    /* If the actor became muted during dispatch, stop processing. */
    if (atomic_load(&actor->flags) & ACTOR_FLAG_MUTED) {
      return false;
    }
  }
  if (message_queue_markempty(&actor->queue)) {
    atomic_fetch_and(&actor->flags, ~ACTOR_FLAG_SCHEDULED);
    /* Auto-release pressure when mailbox is empty. */
    if (atomic_load(&actor->flags) & ACTOR_FLAG_PRESSURED) {
      backpressure_release(actor);
    }
    return false;
  }
  return true;
}

void backpressure_apply(actor_t* actor) {
  atomic_fetch_or(&actor->flags, ACTOR_FLAG_PRESSURED);
}

void backpressure_release(actor_t* actor) {
  atomic_fetch_and(&actor->flags, ~ACTOR_FLAG_PRESSURED);
  /* Walk the pressured_senders list and unmute each sender. */
  muted_sender_node_t* node = atomic_exchange(&actor->pressured_senders, NULL);
  while (node != NULL) {
    muted_sender_node_t* next = node->next;
    actor_t* sender = node->sender;
    atomic_fetch_and(&sender->flags, ~ACTOR_FLAG_MUTED);
    if (sender->pool != NULL && !(atomic_load(&sender->flags) & (ACTOR_FLAG_DESTROY | ACTOR_FLAG_SCHEDULED))) {
      atomic_fetch_or(&sender->flags, ACTOR_FLAG_SCHEDULED);
      scheduler_inject(sender->pool, sender);
    }
    free(node);
    node = next;
  }
}