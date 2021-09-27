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

#ifndef UFSRV_ADT_DOUBLY_LINKEDLIST_TYPE_H
#define UFSRV_ADT_DOUBLY_LINKEDLIST_TYPE_H

/* Node, List, and Iterator are the only data structures used currently. */

typedef void DoublyListClientData;

typedef struct DoublyListNode {
  struct DoublyListNode *prev;
  struct DoublyListNode *next;
  DoublyListClientData *value;
} DoublyListNode;

typedef struct DoublyListIterator {
  DoublyListNode *next;
  int direction;
} DoublyListIterator;

typedef struct DoublyList {
  DoublyListNode *head;
  DoublyListNode *tail;
  void *(*dup)(void *ptr);
  void (*free)(void *ptr);
  int (*match)(void *ptr, void *key);
  unsigned long len;

} DoublyList;

/* Functions implemented as macros */
#define DoublyListLength(l) ((l)->len)
#define DoublyListFirst(l) ((l)->head)
#define DoublyListLast(l) ((l)->tail)
#define DoublyListPrevNode(n) ((n)->prev)
#define DoublyListNextNode(n) ((n)->next)
#define DoublyListNodeValue(n) ((n)->value)

#define DoublyListSetDupMethod(l,m) ((l)->dup = (m))
#define DoublyListSetFreeMethod(l,m) ((l)->free = (m))
#define DoublyListSetMatchMethod(l,m) ((l)->match = (m))

#define DoublyListGetDupMethod(l) ((l)->dup)
#define DoublyListGetFree(l) ((l)->free)
#define DoublyListGetMatchMethod(l) ((l)->match)

#endif //UFSRV_ADT_DOUBLY_LINKEDLIST_TYPE_H
