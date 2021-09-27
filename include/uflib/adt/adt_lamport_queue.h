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

#ifndef SRC_INCLUDE_ADT_LAMPORT_QUEUE_H_
#define SRC_INCLUDE_ADT_LAMPORT_QUEUE_H_

typedef void QueueClientData;

#define AS_QUEUE_CLIENT_DATA(x) ((QueueClientData *)(x))

typedef struct LamportQueue
{
    atomic_size_t front_;
    atomic_size_t back_;
    atomic_size_t	leased;
    size_t	queue_sz;
    size_t cached_front_;
    size_t cached_back_;
    QueueClientData **payload;
} LamportQueue;

typedef struct LamportQueue LocklessSpscQueue;

void LamportQueueInit(LocklessSpscQueue *queue, QueueClientData **payload, size_t queue_sz);
bool LamportQueuePush(LocklessSpscQueue *queue, QueueClientData *elem);
bool LamportQueuePop(LocklessSpscQueue *queue, QueueClientData **elem);
size_t LamportQueueLeasedSize (LocklessSpscQueue *queue);


#endif /* SRC_INCLUDE_ADT_LAMPORT_QUEUE_H_ */
