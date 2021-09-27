/* adlist.c - A generic doubly linked list implementation
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

#include <stdlib.h>
#include <uflib/adt/adt_doubly_linkedlist.h>

//insert node after current node: lock the current and next node, then do your insertion, rewiring the pointers between the two nodes to point at the new node.
//insert node at end: same as above, except the next node is the first node.
//move current node from one list to another: this is unlinking from one list and inserting into another. You would need to lock the node being moved, and the adjacent nodes in both lists.
//delete current node: lock the current node and the two around it. Unlink the current node.
//advance from current node to next node: lock nothing, but wait for the lock to release if moving to a locked node. After the lock releases, reevaluate what "next node" is.

/* Create a new list. The created list can be freed with
 * AlFreeList(), but private value of every node need to be freed
 * by the user before to call AlFreeList().
 *
 * On error, NULL is returned. Otherwise the pointer to the new list. */
DoublyList *DoublyListCreate(DoublyList *dlist_ptr_in)
{
	DoublyList *list;

	if (IS_PRESENT(dlist_ptr_in))	list=dlist_ptr_in;
	else if 												((list = malloc(sizeof(*list))) == NULL)	return NULL;

	list->head = list->tail = NULL;
	list->len = 0;
	list->dup = NULL;
	list->free = NULL;
	list->match = NULL;

	return list;
}

/* Free the whole list.
 *
 * This function can't fail. */
void DoublyListRelease(DoublyList *list)
{
    unsigned long len;
    DoublyListNode *current, *next;

    current = list->head;
    len = list->len;
    while(len--) {
        next = current->next;
        if (list->free) list->free(current->value);
        free(current);
        current = next;
    }

    free(list);
}

/* Add a new node to the list, to head, contaning the specified 'value'
 * pointer as value.
 *
 * On error, NULL is returned and no operation is performed (i.e. the
 * list remains unaltered).
 * On success the 'list' pointer you pass to the function is returned. */
DoublyList *DoublyListAddNodeHead(DoublyList *list, DoublyListNode	*list_node_in, void *value)
{
    DoublyListNode *node;

    if (IS_PRESENT(list_node_in))	node=list_node_in;
		else
		{
			//original semantics
			if ((node = malloc(sizeof(*node))) == NULL)	return NULL;

			node->value = value;
		}

    if (list->len == 0) {
        list->head = list->tail = node;
        node->prev = node->next = NULL;
    } else {
        node->prev = NULL;
        node->next = list->head;
        list->head->prev = node;
        list->head = node;
    }
    list->len++;
    return list;
}

/* Add a new node to the list, to tail, contaning the specified 'value'
 * pointer as value.
 *
 * On error, NULL is returned and no operation is performed (i.e. the
 * list remains unaltered).
 * On success the 'list' pointer you pass to the function is returned. */
DoublyList *DoublyListAddNodeTail(DoublyList *list, DoublyListNode	*list_node_in, void *value)
{
    DoublyListNode *node;

    if (IS_PRESENT(list_node_in))	node=list_node_in;
    else
    {
    	//original interface semantics
			if ((node = malloc(sizeof(*node))) == NULL)	return NULL;

			node->value = value;
    }

    if (list->len == 0) {
        list->head = list->tail = node;
        node->prev = node->next = NULL;
    } else {
        node->prev = list->tail;
        node->next = NULL;
        list->tail->next = node;
        list->tail = node;
    }
    list->len++;
    return list;
}

DoublyList *DoublyListInsertNode(DoublyList *list, DoublyListNode	*list_node_in, DoublyListNode *old_node, void *value, int after)
{
    DoublyListNode *node;

    if (IS_PRESENT(list_node_in))	node=list_node_in;
		else
		{
			//original semantics
			if ((node = malloc(sizeof(*node))) == NULL)	return NULL;

    	node->value = value;
		}

    if (after) {
        node->prev = old_node;
        node->next = old_node->next;
        if (list->tail == old_node) {
            list->tail = node;
        }
    } else {
        node->next = old_node;
        node->prev = old_node->prev;
        if (list->head == old_node) {
            list->head = node;
        }
    }
    if (node->prev != NULL) {
        node->prev->next = node;
    }
    if (node->next != NULL) {
        node->next->prev = node;
    }
    list->len++;
    return list;
}

/* Remove the specified node from the specified list.
 * It's up to the caller to free the private value of the node.
 *
 * This function can't fail. */
void DoublyListDelNode(DoublyList *list, DoublyListNode *node, bool flag_self_destruct)
{
    if (node->prev)
        node->prev->next = node->next;
    else
        list->head = node->next;
    if (node->next)
        node->next->prev = node->prev;
    else
        list->tail = node->prev;
    if (list->free) list->free(node->value);
    if (flag_self_destruct)	free(node);
    list->len--;
}

/* Returns a list iterator 'iter'. After the initialization every
 * call to listNext() will return the next element of the list.
 *
 * This function can't fail. */
DoublyListIterator *DoublyListGetIterator(DoublyList *list, DoublyListIterator *iter_in, int direction)
{
    DoublyListIterator *iter;

    if (IS_PRESENT(iter_in))	iter=iter_in;
    else
    if ((iter = malloc(sizeof(*iter))) == NULL) return NULL;

    if (direction == AL_START_HEAD)
        iter->next = list->head;
    else
        iter->next = list->tail;
    iter->direction = direction;
    return iter;
}

/* Release the iterator memory */
void DoublyListReleaseIterator(DoublyListIterator *iter) {
    free(iter);
}

/* Create an iterator in the list private iterator structure */
void DoublyListRewind(DoublyList *list, DoublyListIterator *li) {
    li->next = list->head;
    li->direction = AL_START_HEAD;
}

void DoublyListRewindTail(DoublyList *list, DoublyListIterator *li) {
    li->next = list->tail;
    li->direction = AL_START_TAIL;
}

/* Return the next element of an iterator.
 * It's valid to remove the currently returned element using
 * listDelNode(), but not to remove other elements.
 *
 * The function returns a pointer to the next element of the list,
 * or NULL if there are no more elements, so the classical usage patter
 * is:
 *
 * iter = listGetIterator(list,<direction>);
 * while ((node = listNext(iter)) != NULL) {
 *     doSomethingWith(listNodeValue(node));
 * }
 *
 * */
DoublyListNode *DoublyListNext(DoublyListIterator *iter)
{
    DoublyListNode *current = iter->next;

    if (current != NULL) {
        if (iter->direction == AL_START_HEAD)
            iter->next = current->next;
        else
            iter->next = current->prev;
    }
    return current;
}

/* Duplicate the whole list. On out of memory NULL is returned.
 * On success a copy of the original list is returned.
 *
 * The 'Dup' method set with listSetDupMethod() function is used
 * to copy the node value. Otherwise the same pointer value of
 * the original node is used as value of the copied node.
 *
 * The original list both on success or error is never modified. */
DoublyList *DoublyListDup(DoublyList *orig)
{
    DoublyList *copy;
    DoublyListIterator *iter;
    DoublyListNode *node;

    if ((copy = DoublyListCreate(NULL)) == NULL)
        return NULL;
    copy->dup = orig->dup;
    copy->free = orig->free;
    copy->match = orig->match;
    iter = DoublyListGetIterator(orig, NULL, AL_START_HEAD);
    while((node = DoublyListNext(iter)) != NULL) {
        void *value;

        if (copy->dup) {
            value = copy->dup(node->value);
            if (value == NULL) {
                DoublyListRelease(copy);
                DoublyListReleaseIterator(iter);
                return NULL;
            }
        } else
            value = node->value;
        if (DoublyListAddNodeTail(copy, NULL, value) == NULL) {
            DoublyListRelease(copy);
            DoublyListReleaseIterator(iter);
            return NULL;
        }
    }
    DoublyListReleaseIterator(iter);
    return copy;
}

/* Search the list for a node matching a given key.
 * The match is performed using the 'match' method
 * set with listSetMatchMethod(). If no 'match' method
 * is set, the 'value' pointer of every node is directly
 * compared with the 'key' pointer.
 *
 * On success the first matching node pointer is returned
 * (search starts from head). If no matching node exists
 * NULL is returned. */
DoublyListNode *DoublyListSearchKey(DoublyList *list, void *key)
{
    DoublyListIterator *iter;
    DoublyListNode *node;

    iter = DoublyListGetIterator(list, NULL, AL_START_HEAD);
    while((node = DoublyListNext(iter)) != NULL) {
        if (list->match) {
            if (list->match(node->value, key)) {
                DoublyListReleaseIterator(iter);
                return node;
            }
        } else {
            if (key == node->value) {
                DoublyListReleaseIterator(iter);
                return node;
            }
        }
    }
    DoublyListReleaseIterator(iter);
    return NULL;
}

/* Return the element at the specified zero-based index
 * where 0 is the head, 1 is the element next to head
 * and so on. Negative integers are used in order to count
 * from the tail, -1 is the last element, -2 the penultimante
 * and so on. If the index is out of range NULL is returned. */
DoublyListNode *DoublyListIndex(DoublyList *list, long index) {
    DoublyListNode *n;

    if (index < 0) {
        index = (-index)-1;
        n = list->tail;
        while(index-- && n) n = n->prev;
    } else {
        n = list->head;
        while(index-- && n) n = n->next;
    }
    return n;
}

/* Rotate the list removing the tail node and inserting it to the head. */
void DoublyListRotate(DoublyList *list) {
    DoublyListNode *tail = list->tail;

    if (DoublyListLength(list) <= 1) return;

    /* Detatch current tail */
    list->tail = tail->prev;
    list->tail->next = NULL;
    /* Move it as head */
    list->head->prev = tail;
    tail->prev = NULL;
    tail->next = list->head;
    list->head = tail;
}
