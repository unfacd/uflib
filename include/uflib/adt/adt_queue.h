
#ifndef INCLUDE_QUEUE_H_
#define INCLUDE_QUEUE_H_

/*
** IRCIT Copyright (c) 1998 Ayman Akt
**
** This file is part of the IRCIT (c) source distribution.
** See the COPYING file for terms of use and conditions.
**
MODULEID("$Id: queue.h,v 1.2 1998/02/20 14:58:44 ayman Beta $")
**
*/

#include <stdbool.h>

struct QueueEntry {
  void *whatever;
  struct QueueEntry *next;
};
typedef struct QueueEntry QueueEntry;

 struct Queue {
  unsigned long queue_id;
  unsigned long nEntries; //current entries
  unsigned long rolling_counter_add; //counter of add operations
  unsigned long rolling_counter_de; 	//counter of de operations
  QueueEntry *front,
              *rear;
 };
 typedef struct Queue Queue;

 typedef void (*queue_entry_reclaimer)(void *);

#define USE_DEFAULT_RECLAIMER NULL

 QueueEntry *AddQueue(Queue *);
 QueueEntry *deQueue(Queue *);
 bool QueueEmpty(const Queue *);
 int AddToQueue(Queue *, void *);
 void *RemoveFromQueue(Queue *, int, queue_entry_reclaimer reclaim_memory);

#define QUEUE_ENTRIES_COUNT(x)	x->nEntries
#define QUEUE_ADDITIONS_COUNT(x)	x->rolling_counter_add
#define QUEUE_DELETIONS_COUNT(x)	x->rolling_counter_de
#define QUEUE_FRONT(x)	x->front
#define QUEUE_REAR(x)	x->rear
#define QUEUE_ID(x)	x->queue_id

#endif /* INCLUDE_QUEUE_H_ */
