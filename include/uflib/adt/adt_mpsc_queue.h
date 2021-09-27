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

#ifndef UFSRV_ADT_MPSC_QUEUE_H
#define UFSRV_ADT_MPSC_QUEUE_H

#include <stdbool.h>
#include "adt_mpsc_queue_type.h"

/* Consumer API. */
void mpsc_queue_init(struct LocklessMpscQueue *queue);

/* Insert at the front of the queue. Only the consumer can do it. */
void mpsc_queue_push_front(struct LocklessMpscQueue *queue, struct mpsc_queue_node *node);

struct mpsc_queue_node *mpsc_queue_pop(struct LocklessMpscQueue *queue);

struct mpsc_queue_node *mpsc_queue_tail(struct LocklessMpscQueue *queue);

#define MPSC_QUEUE_FOR_EACH(node, queue) \
for (node = mpsc_queue_tail(queue); node != NULL; \
node = atomic_load_explicit(&node->next, memory_order_acquire))

#define MPSC_QUEUE_FOR_EACH_POP(node, queue) \
while ((node = mpsc_queue_pop(queue)))

/* Producer API. */
void mpsc_queue_insert(struct LocklessMpscQueue *queue, struct mpsc_queue_node *node);

#endif //UFSRV_ADT_MPSC_QUEUE_H
