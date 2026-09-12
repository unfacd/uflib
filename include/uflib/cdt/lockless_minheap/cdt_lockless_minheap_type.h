/**
 * @file cdt_lockless_minheap_type.h
 * @brief Lock-free integer min-priority queue — type definitions.
 *
 * This header contains only the opaque handle forward-declaration for the
 * lock-free min-PQ.  Consumers that only need to name the type (e.g. to hold
 * a pointer in another struct) can include this file without pulling in the
 * full API surface.
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

#ifndef UFLIB_CDT_LOCKLESS_MINHEAP_CDT_LOCKLESS_MINHEAP_TYPE_H
#define UFLIB_CDT_LOCKLESS_MINHEAP_CDT_LOCKLESS_MINHEAP_TYPE_H

/*!
 * Opaque handle to a lock-free integer min-priority queue.
 *
 * The struct definition is private
 * (src/cdt/lockless_minheap/cdt_lockless_minheap_priv.h).  Consumers obtain
 * an instance with LocklessMinHeapCreate() and release it with
 * LocklessMinHeapDestroy().  It is not a value type: never allocate
 * `LocklessMinHeap` on the stack, never embed it by value, and never reach
 * into its fields.
 */
typedef struct LocklessMinHeap LocklessMinHeap;

#endif /* UFLIB_CDT_LOCKLESS_MINHEAP_CDT_LOCKLESS_MINHEAP_TYPE_H */
