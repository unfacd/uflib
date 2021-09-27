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

#ifndef UFSRV_ADT_MPSC_QUEUE_TYPE_H
#define UFSRV_ADT_MPSC_QUEUE_TYPE_H

#include <uflib/main_types.h>
#include <uflib/standard_c_includes.h>

typedef void QueueContextData;

#define AS_QUEUE_CONTEXT_DATA(x) ((QueueContextData *)(x))

typedef struct mpsc_queue_node {
  _Atomic(struct mpsc_queue_node *) next;
  QueueContextData *context_data; //user payload per node
  struct {
    void (*callback)(ClientContextData *);
    ClientContextData *context_data;
  } finaliser;
} mpsc_queue_node;

typedef struct LocklessMpscQueue {
  _Atomic(struct mpsc_queue_node *) head;
  _Atomic(struct mpsc_queue_node *) tail;
  struct mpsc_queue_node stub;
} LocklessMpscQueue;

enum mpsc_queue_poll_result {
  MPSC_QUEUE_EMPTY,
  MPSC_QUEUE_ITEM,
  MPSC_QUEUE_RETRY,
};

#endif //UFSRV_ADT_MPSC_QUEUE_TYPE_H
