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

#ifdef HAVE_CONFIG_UFLIB_H
#include <config_uflib.h>
#endif

#include <assert.h>

#include <uflib/standard_defs.h>
#include <uflib/standard_c_includes.h>

#include <uflib/cdt/lockless_treiber_stack/lockless_treiber_stack.h>
#include <uflib/logger/logger.h>
#include "lockless_treiber_stack_priv.h"

// CLOSED sentinel for head: compared by address only, never dereferenced or written,
// and never linked into a chain a drain returns.
static struct LocklessTreiberStackNode s_closed_head;

LocklessTreiberStack *
lockless_treiber_stack_create_with_logger(UfLogger *logger_ptr)
{
	LocklessTreiberStack *stack = malloc(sizeof(*stack));

	if (unlikely(stack == NULL)) {
		// This module's only failure mode: the caller gets NULL and no reason.
		if (logger_ptr != NULL) {
			UF_LOGGER_ERROR(logger_ptr, "treiber stack handle allocation failed");
		}
		return NULL;
	}

	// Relaxed: no other thread can observe the stack until the caller publishes it.
	atomic_store_explicit(&stack->head, NULL, memory_order_relaxed);
	stack->uf_logger = logger_ptr;

	if (logger_ptr != NULL) {
		UF_LOGGER_DEBUG(logger_ptr, "treiber stack created");
	}
	return stack;
}

LocklessTreiberStack *
lockless_treiber_stack_create(void)
{
	return lockless_treiber_stack_create_with_logger(NULL);
}

void
lockless_treiber_stack_destroy(LocklessTreiberStack *stack)
{
	UfLogger *logger_ptr;

	if (unlikely(stack == NULL)) return;

	assert(atomic_load_explicit(&stack->head, memory_order_relaxed) == NULL
	       || atomic_load_explicit(&stack->head, memory_order_relaxed) == &s_closed_head);

	// Read the borrowed logger before freeing its handle — the caller contracted to outlive us.
	logger_ptr = stack->uf_logger;
	if (logger_ptr != NULL) {
		UF_LOGGER_DEBUG(logger_ptr, "treiber stack destroyed");
	}
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
lockless_treiber_stack_node_retain(struct LocklessTreiberStackNode *node)
{
	if (unlikely(node == NULL)) return;

	atomic_fetch_add_explicit(&node->refcount, 1, memory_order_relaxed);
}

bool
lockless_treiber_stack_push(LocklessTreiberStack *stack, struct LocklessTreiberStackNode *node)
{
	struct LocklessTreiberStackNode *head;

	if (unlikely(stack == NULL || node == NULL)) return false;

	// Classic Treiber push: the release CAS publishes the payload and the relaxed next store.
	head = atomic_load_explicit(&stack->head, memory_order_relaxed);
	do {
		if (head == &s_closed_head) return false;
		atomic_store_explicit(&node->next, head, memory_order_relaxed);
	} while (!atomic_compare_exchange_weak_explicit(
		&stack->head, &head, node,
		memory_order_release, memory_order_relaxed));

	return true;
}

struct LocklessTreiberStackNode *
lockless_treiber_stack_steal_all(LocklessTreiberStack *stack)
{
	struct LocklessTreiberStackNode *head;

	if (unlikely(stack == NULL)) return NULL;

	head = atomic_load_explicit(&stack->head, memory_order_relaxed);
	for (;;) {
		if (head == &s_closed_head) return NULL;
		if (atomic_compare_exchange_weak_explicit(
			&stack->head, &head, NULL,
			memory_order_acq_rel, memory_order_acquire)) {
			return head;
		}
	}
}

struct LocklessTreiberStackNode *
lockless_treiber_stack_steal_all_and_close(LocklessTreiberStack *stack)
{
	struct LocklessTreiberStackNode *head;

	if (unlikely(stack == NULL)) return NULL;

	head = atomic_exchange_explicit(&stack->head, &s_closed_head, memory_order_acq_rel);
	return head == &s_closed_head ? NULL : head;
}

bool
lockless_treiber_stack_claim(struct LocklessTreiberStackNode *node)
{
	if (unlikely(node == NULL)) return false;

	// Exactly-once: only the first claim wins; the node is then "processed".
	return !atomic_exchange_explicit(&node->is_claimed, true, memory_order_acq_rel);
}

bool
lockless_treiber_stack_release(struct LocklessTreiberStackNode *node)
{
	if (unlikely(node == NULL)) return false;

	// acq_rel so the freeing caller observes all prior accesses to the node.
	return atomic_fetch_sub_explicit(&node->refcount, 1, memory_order_acq_rel) == 1;
}
