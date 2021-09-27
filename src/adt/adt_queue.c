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

#include <uflib/standard_defs.h>
#include <uflib/standard_c_includes.h>
#include <uflib/adt/adt_queue.h>

 QueueEntry *AddQueue (Queue *ptr)

 {
  QueueEntry *qptr = NULL;

   qptr = (QueueEntry *) calloc(1, sizeof(QueueEntry));

    if (qptr == NULL) {
        return (QueueEntry *)NULL;
       }

    if (ptr->nEntries) {
        ptr->rear->next = qptr;
        ptr->rear = qptr;
       } else {
    	//ptr->rolling_counter_add=ptr->rolling_counter_de=0; //just to make sure state is initalised
        ptr->front=ptr->rear=qptr;
       }

   ptr->nEntries++;
   ptr->rolling_counter_add++;

   return (QueueEntry *)qptr;

 }  /**/

 int AddToQueue(Queue *ptr, void *user_data_ptr)
{
  QueueEntry *qptr = calloc(1, sizeof(QueueEntry));

  if (unlikely(qptr == NULL)) {
   return -1;
  }

  qptr->whatever = user_data_ptr;

  if (ptr->nEntries) {
   ptr->rear->next = qptr;
   ptr->rear = qptr;
  } else {
   ptr->front = ptr->rear = qptr;
  }

  ptr->nEntries++;
  ptr->rolling_counter_add++;

  return ptr->nEntries;

}

void *RemoveFromQueue(Queue *ptr, int flag, queue_entry_reclaimer memory_reclaimer)
{
   if (flag) {
     return ptr->front->whatever;
   } else {
     QueueEntry *qptr = ptr->front;
     ptr->front = ptr->front->next;

     ptr->nEntries--;
     ptr->rolling_counter_de++;

     void *ptr_aux = qptr->whatever;

     if (!IS_PRESENT(memory_reclaimer)) free(qptr);
     else memory_reclaimer(qptr);

     return ptr_aux;
   }

   return NULL;

}  /**/

 QueueEntry *deQueue (Queue *ptr)

 {
  QueueEntry *qptr;

   qptr=ptr->front;
   ptr->front=ptr->front->next;

   ptr->nEntries--;
   ptr->rolling_counter_de++;

   return qptr;

 }  /**/

 bool QueueEmpty (const Queue *ptr)

 {
   return ((ptr->nEntries==0));

 }  /**/

