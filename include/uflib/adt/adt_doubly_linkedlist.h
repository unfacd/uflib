
#ifndef SRC_INCLUDE_ADT_DOUBLY_LINKEDLIST_H_
#define SRC_INCLUDE_ADT_DOUBLY_LINKEDLIST_H_


/* adlist.h - A generic doubly linked list implementation
 *
 * Copyright (c) 2006-2010, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <uflib/standard_defs.h>
#include <stdbool.h>
#include <uflib/adt/adt_doubly_linkedlist_type.h>

DoublyList *DoublyListCreate(DoublyList *);
void DoublyListRelease(DoublyList *list);
DoublyList *DoublyListAddNodeHead(DoublyList *list, DoublyListNode	*list_node_in, void *value);
DoublyList *DoublyListAddNodeTail(DoublyList *list, DoublyListNode	*list_node_in, void *value);
DoublyList *DoublyListInsertNode(DoublyList *list, DoublyListNode	*list_node_in, DoublyListNode *old_node, void *value, int after);
void DoublyListDelNode(DoublyList *list, DoublyListNode *node, bool flag_self_destruct);
DoublyListIterator *DoublyListGetIterator(DoublyList *list, DoublyListIterator *iter_in, int direction);
DoublyListNode *DoublyListNext(DoublyListIterator *iter);
void DoublyListReleaseIterator(DoublyListIterator *iter);
DoublyList *DoublyListDup(DoublyList *orig);
DoublyListNode *DoublyListSearchKey(DoublyList *list, void *key);
DoublyListNode *DoublyListIndex(DoublyList *list, long index);
void DoublyListRewind(DoublyList *list, DoublyListIterator *li);
void DoublyListRewindTail(DoublyList *list, DoublyListIterator *li);
void DoublyListRotate(DoublyList *list);

/* Directions for iterators */
#define AL_START_HEAD 0
#define AL_START_TAIL 1


#endif /* SRC_INCLUDE_ADT_DOUBLY_LINKEDLIST_H_ */
