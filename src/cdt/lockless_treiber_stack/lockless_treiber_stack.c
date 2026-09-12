/**
 * @file lockless_treiber_stack.c
 * @brief Lock-free Treiber stack — implementation (C11 atomics).
 *
 * Original algorithm: R. K. Treiber, "Systems Programming: Coping with
 * Parallelism" (IBM RJ 5118, 1986). A classic lock-free LIFO: producers CAS the
 * head to their node, then publish the previous head as the node's `next`.
 *
 * The `is_claimed` flag extends the bare stack so a node pushed concurrently with
 * a drain is processed exactly once (the pusher's double-check and the drainer
 * both try to claim it). The `refcount` (2 = pusher + stack) keeps a node alive
 * until both parties have released it.
 *
 * The stack object is an opaque handle (hidden internals); the intrusive node is
 * caller-owned and embedded by value.
 *
 * Copyright (C) 2015-2026 unfacd works
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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <uflib/standard_defs.h>
#include <uflib/standard_c_includes.h>

#include <uflib/cdt/lockless_treiber_stack/lockless_treiber_stack.h>
#include "lockless_treiber_stack_priv.h"

LocklessTreiberStack *
lockless_treiber_stack_create(void)
{
	LocklessTreiberStack *stack = malloc(sizeof(*stack));

	if (unlikely(stack == NULL)) return NULL;

	/*
	 * Relaxed: initialisation happens-before any sharing of the stack with
	 * other threads (caller's responsibility) — no publication ordering needed.
	 */
	atomic_store_explicit(&stack->head, NULL, memory_order_relaxed);
	return stack;
}

void
lockless_treiber_stack_destroy(LocklessTreiberStack *stack)
{
	if (unlikely(stack == NULL)) return;

	/*
	 * The stack owns no node memory; the caller must have drained (or otherwise
	 * released) all pushed nodes before releasing the handle.
	 */
	free(stack);
}

void
lockless_treiber_stack_node_init(struct LocklessTreiberStackNode *node)
{
	if (unlikely(node == NULL)) return;

	atomic_store_explicit(&node->next, NULL, memory_order_relaxed);
	atomic_store_explicit(&node->is_claimed, false, memory_order_relaxed);
	atomic_store_explicit(&node->refcount, 2, memory_order_relaxed);
}

void
lockless_treiber_stack_push(LocklessTreiberStack *stack, struct LocklessTreiberStackNode *node)
{
	struct LocklessTreiberStackNode *head;

	if (unlikely(stack == NULL || node == NULL)) return;

	/*
	 * Classic Treiber push: publish the node by CAS'ing the head; the old head
	 * becomes the node's `next`. The CAS is release so the node's payload is
	 * visible to a consumer that observes it; the `next` store is relaxed
	 * because it is published through the successful CAS.
	 */
	head = atomic_load_explicit(&stack->head, memory_order_relaxed);
	do {
		atomic_store_explicit(&node->next, head, memory_order_relaxed);
	} while (!atomic_compare_exchange_weak_explicit(
		&stack->head, &head, node,
		memory_order_release, memory_order_relaxed));
}

struct LocklessTreiberStackNode *
lockless_treiber_stack_steal_all(LocklessTreiberStack *stack)
{
	if (unlikely(stack == NULL)) return NULL;

	/* Single atomic exchange takes the whole stack; consumers iterate via next. */
	return atomic_exchange_explicit(&stack->head, NULL, memory_order_acq_rel);
}

bool
lockless_treiber_stack_claim(struct LocklessTreiberStackNode *node)
{
	if (unlikely(node == NULL)) return false;

	/* Exactly-once: only the first claim wins; the node is then "processed". */
	return !atomic_exchange_explicit(&node->is_claimed, true, memory_order_acq_rel);
}

bool
lockless_treiber_stack_release(struct LocklessTreiberStackNode *node)
{
	if (unlikely(node == NULL)) return false;

	/* acq_rel so the freeing caller observes all prior accesses to the node. */
	return atomic_fetch_sub_explicit(&node->refcount, 1, memory_order_acq_rel) == 1;
}
