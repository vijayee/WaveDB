//
// Created by victor on 4/15/25.
//
#include "queue.h"
#include "../Util/allocator.h"


void work_queue_init(work_queue_t* queue) {
  queue->first = NULL;
  queue->last = NULL;
}
void work_enqueue(work_queue_t* queue, work_t* work) {
  if (queue->first == NULL && queue->last == NULL) {
    work_queue_item_t* item = get_clear_memory(sizeof(work_queue_item_t));
    item->previous = NULL;
    item->next = NULL;
    item->work = work;
    queue->first = item;
    queue->last = item;
  } else {
    work_queue_item_t* new_item = get_clear_memory(sizeof(work_queue_item_t));
    new_item->work= work;
    work_queue_item_t* current = queue->first;

    int cmp = priority_compare(&work->priority,&current->work->priority);
    while ((current->next != NULL ) && ((cmp == 1) || (cmp == 0))) {
      current = current->next;
      cmp = priority_compare(&work->priority,&current->work->priority);
    }
    if ((cmp == -1) && (current->next == NULL)) {// current is the last in the list and the new work has greater priority
      new_item->previous = current->previous;
      new_item->next = current;
      if (current->previous != NULL) { // current is not the first item in the list
        work_queue_item_t* temp = current->previous;
        temp->next = new_item;
      } else {// current is the first in the list and new work should become the new first
        queue->first= new_item;
      }
      current->previous = new_item;
    } else if ((cmp != -1) && current->next == NULL) { //current is the last in the list and the new work should come after it
      current->next = new_item;
      new_item->previous = current;
      queue->last = new_item;
    } else if (current->previous == NULL) { // work should be at the begining of a populated list
      new_item->next = current;
      current->previous = new_item;
      queue->first = new_item;
    } else { // we are somewhere in the middle of the list and work has greater priority than the current
      work_queue_item_t* temp = current->previous;
      temp->next = new_item;
      new_item->previous= temp;
      new_item->next = current;
      current->previous = new_item;
    }
  }
}
work_t* work_dequeue(work_queue_t* queue) {
  if (queue->first == NULL) {
    return NULL;
  } else {
    work_queue_item_t* temp = queue->first;
    work_t* work= temp->work;
    queue->first = temp->next;
    if(queue->first != NULL) {
      queue->first->previous = NULL;
    }
    if (queue->last == temp) {
      queue->last = NULL;
    }
    free(temp);
    return work;
  }
}
/*
work_queue_t* work_queue_destroy(work_queue_t* queue) {
  platform_lock(&queue->lock);
  refcounter_dereference((refcounter_t*) queue);
  platform_unlock(&queue->lock);
  if (refcounter_count((refcounter_t*) queue) == 0) {
    platform_lock_destroy(&queue->lock);
    work_queue_item_t *current = queue->first;
    while (current != NULL) {
      work_queue_item_t *temp = current->next;
      free(current);
      current = temp;
    }
    free(queue);
  }

  }
}*/
