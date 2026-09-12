/**
 * @file lockless_treiber_stack_type.h
 * @brief Public types for the lockless_treiber_stack module.
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

#ifndef LOCKLESS_TREIBER_STACK_TYPE_H
#define LOCKLESS_TREIBER_STACK_TYPE_H

#include <uflib/standard_c_includes.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Intrusive node of a lock-free Treiber stack.
 *
 * Callers embed this as the first member of their own node struct and initialise
 * it with lockless_treiber_stack_node_init(). `is_claimed` guarantees each node
 * is processed exactly once; `refcount` (2 = pusher + stack) keeps the node alive
 * until both the pusher and the drainer release it.
 */
typedef struct LocklessTreiberStackNode {
    _Atomic(struct LocklessTreiberStackNode *) next;     /*!< Successor link. */
    _Atomic(bool)  is_claimed;                           /*!< Exactly-once guard. */
    _Atomic(int)   refcount;                             /*!< Pusher + stack refs. */
} LocklessTreiberStackNode;

/*!
 * Opaque handle to a lock-free Treiber stack.
 *
 * The struct is private (src/cdt/lockless_treiber_stack/lockless_treiber_stack_priv.h).
 * Consumers obtain an instance with lockless_treiber_stack_create() and release it
 * with lockless_treiber_stack_destroy(). The stack owns no node memory — nodes are
 * caller-allocated and embedded by value in the caller's own structs.
 */
typedef struct LocklessTreiberStack LocklessTreiberStack;

#ifdef __cplusplus
}
#endif

#endif /* LOCKLESS_TREIBER_STACK_TYPE_H */
