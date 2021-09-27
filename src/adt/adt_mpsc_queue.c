/**
 * Copyright (C) 2015-2021 unfacd works
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
/**
 * https://u256.net/posts/mpsc-queue.html
 * Based on https://github.com/grivet/mpsc-queue/blob/main/mpsc-queue.h
 * and https://github.com/winksaville/test-mpscfifo-intrusive/blob/master/mpscfifo.c
 * and original algorithm https://www.1024cores.net/home/lock-free-algorithms/queues/intrusive-mpsc-node-based-queue
 * https://docs.google.com/presentation/d/1t_9JiTZjN6Gqz_eTruAY_GYiUpFA99ChiRRavIkoHCk/edit?usp=sharing
 *
 * Multi-producer: multiple threads can write concurrently. Insertion is thread safe and costs one atomic exchange.

Single-consumer: only one thread can safely remove nodes from the queue.

Unbounded: The queue is a linked-list and does not limit the number of elements.

Wait-free writes: Writers will never wait for queue state sync when enqueuing.

Obstruction-free reads: The reader does not wait to see is a node is available in the queue. Peeking takes a bounded number of instructions. There is however no removal forward-guarantee, as it relies on other threads progressing. Livelock must be avoided with an out-of-band mechanism.

Intrusive: Queue elements are allocated as part of larger objects. Objects are retrieved by offset manipulation.

per-producer FIFO: Elements in the queue are kept in the order their producer inserted them. The consumer retrieves them in the same insertion order. When multiple producers insert at the same time, either will proceed.

This queue is well-suited for message passing between threads, where any number of thread can insert a message and a single thread is meant to receive and process them.

It could be used to implement the Actor concurrency model.

Note: this queue is serializable but not linearizable. After a series of insertion, the queue state remains consistent and the insertion order is compatible with their precedence. However, because one insertion consists in two separate memory transaction, the queue state can be found inconsistent within the series.

This has important implication regarding the concurrency environment this queue can be used with. To avoid livelocking the consumer thread, one must ensure that producer threads cannot be cancelled when inserting elements in the queue. Either cooperative threads should be used or insertions should be done outside cancellable sections.
 */

#include <uflib/adt/adt_mpsc_queue.h>
#include <stdlib.h>

static inline enum mpsc_queue_poll_result mpsc_queue_poll(struct LocklessMpscQueue *queue, struct mpsc_queue_node **node);

/* Consumer API. */

void
mpsc_queue_init(struct LocklessMpscQueue *queue)
{
  atomic_store_explicit(&queue->head, &queue->stub, memory_order_relaxed);
  atomic_store_explicit(&queue->tail, &queue->stub, memory_order_relaxed);
  atomic_store_explicit(&queue->stub.next, NULL, memory_order_relaxed);
}

/* Insert at the front of the queue. Only the consumer can do it. */
void
mpsc_queue_push_front(struct LocklessMpscQueue *queue, struct mpsc_queue_node *node)
{
  struct mpsc_queue_node *tail;

  tail = atomic_load_explicit(&queue->tail, memory_order_relaxed);
  atomic_store_explicit(&node->next, tail, memory_order_relaxed);
  atomic_store_explicit(&queue->tail, node, memory_order_relaxed);
}

static inline enum mpsc_queue_poll_result
mpsc_queue_poll(struct LocklessMpscQueue *queue, struct mpsc_queue_node **node)
{
  struct mpsc_queue_node *tail;
  struct mpsc_queue_node *next;
  struct mpsc_queue_node *head;

  tail = atomic_load_explicit(&queue->tail, memory_order_relaxed);
  next = atomic_load_explicit(&tail->next, memory_order_acquire);

  if (tail == &queue->stub) {
    if (next == NULL) {
      return MPSC_QUEUE_EMPTY;
    }

    // Advance tail to real "tail"
    atomic_store_explicit(&queue->tail, next, memory_order_relaxed);
    tail = next;
    next = atomic_load_explicit(&tail->next, memory_order_acquire);
  }

  if (next != NULL) {
    atomic_store_explicit(&queue->tail, next, memory_order_relaxed);
    *node = tail;
    return MPSC_QUEUE_ITEM;
  }

  //next == null;
  // two conditions now exist, either this is the last element or
  // a producer was preempted and next hasn't yet been updated.
  // We can tell the difference by testing if tail != head.
  head = atomic_load_explicit(&queue->head, memory_order_acquire);
  if (tail != head) {
    return MPSC_QUEUE_RETRY;
  }

  //(tail == head): last element
  // when we remove it the fifo will be empty, therefore we need to add the stub to the fifo for this case.
  mpsc_queue_insert(queue, &queue->stub);

  next = atomic_load_explicit(&tail->next, memory_order_acquire);
  if (next != NULL) {
    atomic_store_explicit(&queue->tail, next, memory_order_relaxed);
    *node = tail;
    return MPSC_QUEUE_ITEM;
  }

  return MPSC_QUEUE_EMPTY;
}

struct mpsc_queue_node *
mpsc_queue_pop(struct LocklessMpscQueue *queue)
{
  enum mpsc_queue_poll_result result;
  struct mpsc_queue_node *node;

  do {
    result = mpsc_queue_poll(queue, &node);
    if (result == MPSC_QUEUE_EMPTY) {
      return NULL;
    }
  } while (result == MPSC_QUEUE_RETRY);

  return node;
}

struct mpsc_queue_node *
mpsc_queue_tail(struct LocklessMpscQueue *queue)
{
  struct mpsc_queue_node *tail;
  struct mpsc_queue_node *next;

  tail = atomic_load_explicit(&queue->tail, memory_order_relaxed);
  next = atomic_load_explicit(&tail->next, memory_order_acquire);

  if (tail == &queue->stub) {
    if (next == NULL) {
      return NULL;
    }

    atomic_store_explicit(&queue->tail, next, memory_order_relaxed);
    tail = next;
  }

  return tail;
}

/* Producer API. */
void
mpsc_queue_insert(struct LocklessMpscQueue *queue, struct mpsc_queue_node *node)
{
  struct mpsc_queue_node *prev;

  atomic_store_explicit(&node->next, NULL, memory_order_relaxed);
  prev = atomic_exchange_explicit(&queue->head, node, memory_order_acq_rel);
  atomic_store_explicit(&prev->next, node, memory_order_release);
}
