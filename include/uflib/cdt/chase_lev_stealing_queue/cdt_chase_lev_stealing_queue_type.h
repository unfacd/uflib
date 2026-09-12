/**
 * @file cdt_chase_lev_stealing_queue_type.h
 * @brief Type definitions for the Chase–Lev work-stealing deque.
 *
 * This module is a single-owner, multi-thief Chase–Lev deque: one thread may
 * `push`/`pop` (LIFO at the owner end); any number of threads may `steal`
 * (FIFO at the opposite end).  It is the per-worker structure of a
 * job-stealing scheduler, not a general MPMC FIFO.
 *
 * The handle is opaque.  Consumers never see `top`/`bottom`, the ring, slot
 * atomics, the retired-buffer chain, `cached_top`, or the cache-line layout.
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

#ifndef UFLIB_CDT_CHASE_LEV_STEALING_QUEUE_CDT_CHASE_LEV_STEALING_QUEUE_TYPE_H
#define UFLIB_CDT_CHASE_LEV_STEALING_QUEUE_CDT_CHASE_LEV_STEALING_QUEUE_TYPE_H

#include <uflib/main_types.h>

/**
 * @brief Opaque handle to a Chase–Lev work-stealing deque.
 *
 * The representation is private to the module.  Items are raw `void *`
 * pointers; `NULL` is reserved (not a legal item).
 */
typedef struct ChaseLevStealingQueue ChaseLevStealingQueue;

#endif /* UFLIB_CDT_CHASE_LEV_STEALING_QUEUE_CDT_CHASE_LEV_STEALING_QUEUE_TYPE_H */
