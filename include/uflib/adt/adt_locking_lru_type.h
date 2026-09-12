
/*

 Copyright (c) 2015-20266 unfacd works

 This program is free software: you can redistribute it and/or modify
 it under the terms of the GNU Affero General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU Affero General Public License for more details.

 You should have received a copy of the GNU Affero General Public License
 along with this program.  If not, see <http://www.gnu.org/licenses/>.

 */

#ifndef UFLIB_ADT_ADT_LOCKING_LRU_TYPE_H
#define UFLIB_ADT_ADT_LOCKING_LRU_TYPE_H

#include <uflib/adt/adt_hashtable.h>
#include <uflib/adt/adt_doubly_linkedlist.h>

#ifdef CONFIG_USE_ANDERSON_SPINLOCK
#include <uflib/cdt/cdt_anderson_spinlock.h>
#else
#include <pthread.h>
#endif

typedef void LruClientData;
typedef LruClientData * (*ItemExtractor)(LruClientData *);
typedef char * (*ItemPrinter)(LruClientData *, size_t );

#define AS_LRU_CLIENT_DATA(x) ((LruClientData *)(x))
#define LRU_CLIENT_DATA_EMPTY NULL

typedef struct LockingLruItem {
		LruClientData 	*data;
		DoublyListNode	list_node;
} LockingLruItem;

/**
 * The structure that encapsulates a locking lru data
 */
typedef struct LockingLru {
	DoublyList			list;
	size_t					lru_size;//before we start evicting
	const char 			*lru_name;
	HashTable 			*hashtable;
  ItemExtractor   item_extractor_callback;
  ItemPrinter     item_printer_callback;

#ifdef CONFIG_USE_ANDERSON_SPINLOCK
	spinlock_anderson_t	spinlock_list;
#else
	pthread_spinlock_t spinlock_list;
#endif
} LockingLru;



#endif /* UFLIB_ADT_ADT_LOCKING_LRU_TYPE_H */
