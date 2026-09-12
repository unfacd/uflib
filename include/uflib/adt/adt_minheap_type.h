/**
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

#ifndef UFLIB_ADT_ADT_MINHEAP_TYPE_H
#define UFLIB_ADT_ADT_MINHEAP_TYPE_H

/**
 * @brief Opaque handle to a binary min-heap (priority queue).
 *
 * The complete layout is private to `src/adt/adt_minheap.c`.  Obtain a
 * handle with MinHeapCreate() / MinHeapCreateI64() and release it with
 * MinHeapDestroy().  A heap is not a value type: never allocate `MinHeap` on the
 * stack, never embed it by value in another structure, and never reach into
 * its fields.  This is a breaking change from the pre-2026 page-mapped heap,
 * whose `struct MinHeap` was fully visible.
 */
typedef struct MinHeap MinHeap;

/**
 * @brief Key comparison function.
 *
 * Compares two keys and returns a negative value when `key1 < key2`, zero
 * when they are equal, and a positive value when `key1 > key2`.  Any
 * magnitude of negative/positive is honoured (the sift-down path treats
 * `> 0` as "greater", never `== 1`).
 *
 * The keys passed to a comparator are the raw `key` pointers stored with
 * MinHeapInsert() — the comparator must know how to read them (e.g. cast to
 * `const int *` and dereference).
 */
typedef int (*MinHeapCmpFn)(const void *key1, const void *key2);

#endif /* UFLIB_ADT_ADT_MINHEAP_TYPE_H */
