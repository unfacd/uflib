/*

 Copyright (c) 2015-2026 unfacd works

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

#ifdef HAVE_CONFIG_UFLIB_H
#include <config_uflib.h>
#endif

#include <string.h>
#include <syslog.h>
#include <uflib/adt/adt_locking_lru.h>
#include <uflib/recycler/recycler.h>

#ifdef CONFIG_USE_ANDERSON_SPINLOCK
#define	SPIN_LOCK() \
		spinlock_anderson_thread_t *spinlock = NULL;\
		spinlock_anderson_lock(&(lru_ptr->spinlock_list), &spinlock);

#define SPIN_UNLOCK()	\
		spinlock_anderson_unlock(&(lru_ptr->spinlock_list), spinlock);
#else
#define		SPIN_LOCK()	\
	pthread_spin_lock(&(lru_ptr->spinlock_list));

#define SPIN_UNLOCK()	\
		pthread_spin_unlock(&(lru_ptr->spinlock_list));
#endif

//extern ufsrv *const masterptr;

static LruClientData *_DefaultLruItemExtractor(LruClientData *client_data_ptr);
static char *_DefaultLruItemPrinter(LruClientData *client_data_ptr, size_t);
static LockingLruItem *_LruItemFromListNode(DoublyListNode *list_node_ptr);
static LruClientData *_LockingLruRemoveListNode(LockingLru *lru_ptr, DoublyListNode *list_node_ptr);
static LruClientData *_EvictLeastUsedCallback(ContextData *);
static LruClientData *_LockingLruResolveItemInsertion(LockingLru *lru_ptr, const void *item_key, LruClientData *client_data_ptr);

#if 1//def CONFIF_USE_TYPEPOOL_FOR_LOCKINGLRUITEM
//type recycler pool for LockingLruItem
/////>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
#if 1
//assigned when the typepool is initialised
static RecyclerPoolHandle *LockingLruItemPoolHandle;

static int	TypePoolInitCallback_LockingLruItem(ClientContextData *data_ptr, size_t oid);
static int	TypePoolGetInitCallback_LockingLruItem(InstanceHolder *data_ptr, ContextData *context_data, size_t oid, unsigned long call_flags);
static int	TypePoolPutInitCallback_LockingLruItem(InstanceHolder *data_ptr, ContextData *context_data, unsigned long call_flags);
static char	*TypePoolPrintCallback_LockingLruItem(InstanceHolder *data_ptr, ContextData *context_data, unsigned long call_flags);
static int	TypePoolDestructCallback_LockingLruItem(InstanceHolder *data_ptr, ContextData *context_data, unsigned long call_flags);

static RecyclerPoolOps ops_lockinglruitem_descriptor = {
		TypePoolInitCallback_LockingLruItem,
		TypePoolGetInitCallback_LockingLruItem,
		TypePoolPutInitCallback_LockingLruItem,
		TypePoolPrintCallback_LockingLruItem,
		TypePoolDestructCallback_LockingLruItem,
    NULL,
};
#endif
/////>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
#endif

LockingLru *InitLockingLru(LockingLru *lru_ptr_in, const char *lru_name, size_t lru_sz, size_t allocation_sz,
                           HashTable *hash_table_ptr, hashtable_init_callback hashinit_callback,
                           ItemExtractor item_extractor_callback, ItemPrinter item_printer_callback)
{
	LockingLru *lru_ptr;

	if (IS_PRESENT(lru_ptr_in))	lru_ptr = lru_ptr_in;
	else												lru_ptr = calloc(1, sizeof(LockingLru));

	lru_ptr->lru_name = strdup(lru_name);
	lru_ptr->lru_size = lru_sz;
	lru_ptr->hashtable= hash_table_ptr;

	hashinit_callback(hash_table_ptr, lru_sz, 0);
	DoublyListCreate(&lru_ptr->list);

  if (IS_EMPTY(item_extractor_callback))  lru_ptr->item_extractor_callback = _DefaultLruItemExtractor;
  else lru_ptr->item_extractor_callback = item_extractor_callback;

  if (IS_EMPTY(item_printer_callback))  lru_ptr->item_printer_callback = _DefaultLruItemPrinter;
  else lru_ptr->item_printer_callback = item_printer_callback;

#ifdef CONFIG_USE_ANDERSON_SPINLOCK
	spinlock_anderson_init(&(lru_ptr->spinlock_list), malloc(sizeof(spinlock_anderson_thread_t) * allocation_sz), allocation_sz); //anderson's spinlocks use queue for as many competing threads
#else
	pthread_spin_init(&(lru_ptr->spinlock_list), 0);
#endif

	syslog(LOG_INFO, "%s: SUCCESS: %s LockingLru Cache Instantiated: size: '%ld'", __func__, lru_ptr->lru_name, lru_ptr->lru_size);

	return lru_ptr;
}

/**
 * 	@brief Main interface function for caching a NEW item into the lru cache.
 * 	User's responsibility to ensure item is not already in.
 * 	@WARNING must deal with the returned evicted item where returned value != client_data_ptr
 */
void *
__attribute__((nonnull(1))) LockingLruSet(LockingLru *lru_ptr, LruClientData *client_data_ptr)
{
	return (LockingLruPromote(lru_ptr, NULL, client_data_ptr));//technically not a promotion. NULL indicates a new item

}

/**
 * 	@brief	Basic interface for querying the local LruCache for the existence of a given basicauth value.
 * 	If item is present in the hash, the items is promoted to the front of the list, unless it is already at the head
 *
 *	@param item_key	the hashed item's key into the cache
 *
 *	@param client_data_ptr_evicted in instance where 1)the item had to be promoted (e.g. wasn't near head), 2) and table was at capacity
 *	3) and item_key was not in hash already: other item would have needed to be evicted to make room for the newly promoted item
 *
 * 	@return LruClientData * for promoted item, or NULL if \p item_key was not in cache.
 */
void *
LockingLruGet(LockingLru *lru_ptr, const void *item_key, LruClientData **client_data_ptr_evicted)
{
	LruClientData  *client_data_ptr = (LruClientData *)HashLookup(lru_ptr->hashtable, (void *)item_key, true);
	if (IS_PRESENT(client_data_ptr)) {
		LruClientData  *client_data_ptr_returned = LockingLruPromote(lru_ptr, item_key, client_data_ptr);

		if (IS_PRESENT(client_data_ptr_returned) && client_data_ptr_returned != client_data_ptr) {
#if UF_DEBUG_BUILD
			syslog(LOG_ERR, "%s (pid:'%lu'): Promoted item with eviction side-effect of another item", __func__, pthread_self());
#endif
			if (IS_PRESENT(client_data_ptr_evicted)) *client_data_ptr_evicted = client_data_ptr_returned;
		}

		return client_data_ptr;
	}

	return NULL;
}

/**
 * @brief Default extractor simply returns self.
 * @return unmodified passed param
 */
static LruClientData *
_DefaultLruItemExtractor(LruClientData *client_data_ptr)
{
  return client_data_ptr;
}

/**
 * @brief Generic LRU item printer.
 * @param client_data_ptr
 * @param index position in the LRU
 * @return
 */
static char *
_DefaultLruItemPrinter(LruClientData *client_data_ptr, size_t index)
{
  syslog(LOG_ERR, "%s (pid:'%lu', o_item:'%p', idx:'%lu'): ListItem Client Data", __func__, pthread_self(), client_data_ptr, index);
  return NULL;
}

/**
 * 	@brief This is designed to facilitate the promotion of recently accessed hashed items in a Lru structure, as such it locks the entire structure
 * 	and invokes a client callback to help efficient manipulation of the structure whilst it is locked.
 *
 *	@param item_key If present, indicates the item is already  in the cache, otherwise this is a brand new item being inserted,
 *	in which case \a client_data_ptr would be used by the Hashtable machinery to extract the key/value pair for this lru item
 *	@param client_data_ptr
 *	@return LruClientData * is an item that was evicted and caller must deal with deallocating its memory.
 *	If item was at head position NULL is returned.
 *
 * 	@locks LockingLru
 * 	@unlocks  LockingLru
 */
void *
LockingLruPromote(LockingLru *lru_ptr, const void *item_key, LruClientData *client_data_ptr)
{
	LruClientData *client_data_ptr_returned = NULL;

	SPIN_LOCK()

	DoublyListNode *list_node_ptr = DoublyListFirst(&lru_ptr->list);
	if (IS_PRESENT(list_node_ptr)) {
		LruClientData *client_data_ptr_stored = _LruItemFromListNode(list_node_ptr)->data;
		if (client_data_ptr_stored == client_data_ptr) {
#if UF_DEBUG_BUILD
			syslog(LOG_ERR, "%s (pid:'%lu'): Won't promote: Item already at head position", __func__, pthread_self());
#endif

			SPIN_UNLOCK()
			return NULL;
		} else {
			//if hashtable is at capacity retrieve an item from the rear and sequentially remove that item from list and hashtable
			//This may also evict this very item we are looking to promote if:
			//the hash was at capacity and this item was at rear
			client_data_ptr_returned = _LockingLruResolveItemInsertion(lru_ptr, item_key, client_data_ptr);
			SPIN_UNLOCK()
		}
	} else {
		client_data_ptr_returned = _LockingLruResolveItemInsertion(lru_ptr, item_key, client_data_ptr);
		SPIN_UNLOCK()
	}

	return client_data_ptr_returned;
}

/**
 * 	@brief Helper routine to generalise putting items to lru cache
 * 	@return LruClientData * which maybe an evicted item, in which case caller must deal with memory management.
 * 	@locked LockingLru
 */
static LruClientData *
__attribute__((nonnull(1))) _LockingLruResolveItemInsertion(LockingLru *lru_ptr, const void *item_key, LruClientData *client_data_ptr)
{
	LruClientData *evicted_item_ptr = NULL;

	if (AddToHashEvictIfNecessary(lru_ptr->hashtable, item_key, (void *)client_data_ptr, _EvictLeastUsedCallback, (ContextData *)lru_ptr, (void **)&evicted_item_ptr)) {
		//this will no-op if the LruNode reference is not set in the unaliased LruClientData type (ie including the hidden pointer region),
		//indicating item was not on the list
    LockingLruRemoveThis(lru_ptr, client_data_ptr);
		LockingLruAddToFront(lru_ptr, client_data_ptr, NULL, false);

#if UF_DEBUG_BUILD
    LockingLruDescribeItems(lru_ptr);
#endif

		if 	(IS_PRESENT(evicted_item_ptr))	return (evicted_item_ptr);
		else																return  client_data_ptr;
	}

	return NULL;
}

/**
 * 	@brief callback for evicting least active basicauth entry from the LruList (linkedlist specifically)
 * 	@lock NONE.This is very important: being called from within a callback context it should not lock
 */
static LruClientData *
_EvictLeastUsedCallback(ContextData *ctx_ptr)
{
	return (LockingLruRemoveFromRear((LockingLru *) ctx_ptr));
}

LockingLru *
LockingLruAddToFront(LockingLru *lru_ptr, LruClientData *data, LockingLruItem *lru_item_in, bool flag_lock)
{
	LockingLruItem *lru_item;

	if (IS_PRESENT(lru_item_in))	lru_item = lru_item_in;
	else													lru_item = calloc(1, sizeof(LockingLruItem));

	lru_item->data = data;

	uintptr_t p = (uintptr_t)LOCKINGLRU_EXTRACT_ITEM(lru_ptr, data);
	p -= sizeof(uintptr_t);

	LockingLruItem **lru_item_save = (LockingLruItem **)p;
	*lru_item_save = lru_item;

	//if (flag_lock)	SPIN_LOCK()
	DoublyListAddNodeHead(&(lru_ptr->list), &(lru_item->list_node), NULL);//value already contained in lru_item
	//if (flag_lock)	SPIN_UNLOCK()

	return lru_ptr;
}

LockingLru *
LockingLruAddToRear(LockingLru *lru_ptr, LruClientData *data, LockingLruItem *lru_item_in, bool flag_lock)
{
	LockingLruItem *lru_item;

	if (IS_PRESENT(lru_item_in))	lru_item = lru_item_in;
	else													lru_item = calloc(1, sizeof(LockingLruItem));

	lru_item->data = data;

	uintptr_t p = (uintptr_t)LOCKINGLRU_EXTRACT_ITEM(lru_ptr, data);
	p -= sizeof(uintptr_t);

	LockingLruItem **lru_item_save = (LockingLruItem **)p;
	*lru_item_save = lru_item;

	//if (flag_lock)	SPIN_LOCK()
	DoublyListAddNodeTail (&(lru_ptr->list), &(lru_item->list_node), NULL);
	//if (flag_lock)	SPIN_UNLOCK()

	return lru_ptr;
}

/**
 * 	@locked LockingLru must be locked by the caller
 * 	@return Client data stored in removed cache item. NULL indicates item was not cached or cache empty.
 * 	@dynamic_memory DEALLOCATES list item 'LockingLruItem *'
 */
static LruClientData *
_LockingLruRemoveListNode(LockingLru *lru_ptr, DoublyListNode *list_node_ptr)
{
	DoublyListDelNode(&(lru_ptr->list), list_node_ptr, false/*self_destruct*/);//we don't free list_node_ptr because it is statically allocated in the lru_item container

	if (IS_PRESENT(list_node_ptr)) {
		LockingLruItem *lru_item_ptr = (struct LockingLruItem *)((uintptr_t)list_node_ptr -	offsetof(LockingLruItem, list_node));
		LruClientData 	*item_returned = lru_item_ptr->data;
		free(lru_item_ptr); lru_item_ptr = NULL;

		//TODO: probably good idea to NULL out the saved reference in ClientData
		return item_returned;
	} else {
		syslog(LOG_ERR, "%s (pid:'%lu'): ERROR: DoublyListNode was empty", __func__, pthread_self());
	}

	return NULL;
}

void *
LockingLruRemoveFromFront(LockingLru *lru_ptr)
{
	return (_LockingLruRemoveListNode(lru_ptr, DoublyListFirst(&lru_ptr->list)));

}

void *
LockingLruRemoveFromRear(LockingLru *lru_ptr)
{
	return (_LockingLruRemoveListNode(lru_ptr, DoublyListLast(&lru_ptr->list)));

}

/**
 * @brief De-link a lru item from the containing list and return client data. Storage for the actual list item is managed by teh underlying list
 * implementation, in this instance \p DoublyLinkedList. Call is responsible for deallocating client data stored in the item.
 * @param lru_ptr The structure containing whole-of Lru cache.
 * @param client_data_ptr the actual client data payload stored in the list. This be pointer offset (aka extracted) for further processing.
 * @return
 * @locked LockingLru by the caller
 */
void *
LockingLruRemoveThis(LockingLru *lru_ptr, LruClientData *client_data_ptr)
{
	uintptr_t p = (uintptr_t)LOCKINGLRU_EXTRACT_ITEM(lru_ptr, client_data_ptr);

	//retrieve the saved pointer
	LockingLruItem **lru_item_saved = (LockingLruItem **)(p - sizeof(uintptr_t));
	LockingLruItem *lru_item = *lru_item_saved;

	if (IS_PRESENT(lru_item)) _LockingLruRemoveListNode(lru_ptr, &(lru_item->list_node));
	else {
		//item could be NULL
		syslog(LOG_DEBUG, "%s (pid:'%lu'): ClientData contained NULL reference to its LruListNode (unattached item)", __func__, pthread_self());
	}

	return client_data_ptr;
}

/**
 * @brief A diagnostic lru list iterator
 * @return iterations count
 * @locks None. Ideally, ''LockingLru *' should be locked by user to guard against corruption
 */
size_t
LockingLruDescribeItems(LockingLru *lru_ptr)
{
	size_t i = 0;
	DoublyListIterator iter = {0};
	DoublyListNode *list_node;
	LockingLruItem *lru_item;

//SPIN_LOCK();

	DoublyListGetIterator(&(lru_ptr->list), &iter, AL_START_HEAD);
	while ((list_node = DoublyListNext(&iter))) {
		lru_item = _LruItemFromListNode(list_node);
    LOCKINGLRU_PRINT_ITEM(lru_ptr, lru_item->data, i);
    i++;
	}

//SPIN_UNLOCK();

	return i;
}

/**
 * @brief Iterate though the list and find an item based on user-provided matching function.
 * @param matcher user call back which receives \p LockingLruItem and return true if match found.
 * @param client_data_ptr_provided callback will be passed this key to compare stored item with as list is iterated
 * @return matched item or NULL
 * @locks LockingLru *
 * @unlocks LockingLru *
 */
__unused LockingLruItem *
__attribute__((nonnull(2))) FindLruItemWithMatcher(LockingLru *lru_ptr, lru_item_matcher matcher, LruClientData *client_data_ptr_provided)
{
  DoublyListIterator iter = {0};
  DoublyListNode *list_node;
  LockingLruItem *lru_item;

  SPIN_LOCK();

  DoublyListGetIterator(&(lru_ptr->list), &iter, AL_START_HEAD);
  while ((list_node = DoublyListNext(&iter))) {
    lru_item = _LruItemFromListNode(list_node);
    if (LOCKINGLRU_MATCH_ITEM(matcher, lru_item->data, client_data_ptr_provided)) {
      return lru_item;
    }
  }

  SPIN_UNLOCK();

  return NULL;
}

/**
 * @brief
 * @param lru_ptr
 * @param matcher_callback user callback which receives iterated \p LruClientData and return true if match found.
 * @param client_data_ptr_provided callback will be passed this key to compare stored item with as list is iterated
 * @return Lru client data cache item originally allocated by user. NULL indicates item was not cached.
 * @locks LockingLru *
 * @unlocks LockingLru *
 */
LruClientData *
__attribute__((nonnull(1, 2, 3))) FindLruItemWithMatcherAndDelink(LockingLru *lru_ptr, lru_item_matcher matcher_callback, LruClientData *client_data_ptr_provided)
{
  DoublyListIterator iter = {0};
  DoublyListNode *list_node;
  LockingLruItem *lru_item;
  LruClientData *client_data_ptr;

  SPIN_LOCK();

  DoublyListGetIterator(&(lru_ptr->list), &iter, AL_START_HEAD);
  while ((list_node = DoublyListNext(&iter))) {
    lru_item = _LruItemFromListNode(list_node);
    client_data_ptr = lru_item->data;
    if (LOCKINGLRU_MATCH_ITEM(matcher_callback, lru_item->data, client_data_ptr_provided)) {
      LruClientData *client_data_returned = _LockingLruRemoveListNode(lru_ptr, &(lru_item->list_node));
      assert(client_data_returned == client_data_ptr);
      RemoveFromHash(lru_ptr->hashtable, client_data_ptr);

#if UF_DEBUG_BUILD
      LockingLruDescribeItems(lru_ptr);
#endif

      SPIN_UNLOCK();

      return client_data_ptr;
    }
  }

  SPIN_UNLOCK();

  return NULL;
}

__pure size_t
LockingLruSize(LockingLru *lru_ptr)
{
	//SPIN_LOCK();
	size_t list_sz = lru_ptr->list.len;
	//SPIN_UNLOCK();

	return list_sz;
}

/**
 * 	@lock LockingLru:
 * 	@unlocks LockingLru:
 */
LruClientData *
LockingLruRearItemGet(LockingLru *lru_ptr)
{
	SPIN_LOCK()
	DoublyListNode *list_node_ptr = DoublyListLast(&lru_ptr->list);
	SPIN_UNLOCK()

	if (IS_PRESENT(list_node_ptr)) {
		return (_LruItemFromListNode(list_node_ptr))->data;
	}

	return NULL;

}

/**
 * 	@brief: return item at the head of the LruList. Not the configuration of the LruList might change by the time this function returned
 * 	@lock LockingLru:
 * 	@unlocks LockingLru:
 */
LruClientData *
LockingLruHeadItemGet(LockingLru *lru_ptr)
{
	SPIN_LOCK()
	DoublyListNode *list_node_ptr = DoublyListFirst(&lru_ptr->list);
	SPIN_UNLOCK()

	if (IS_PRESENT(list_node_ptr)) {
		return (_LruItemFromListNode(list_node_ptr))->data;
	}

	return NULL;

}

void
LockingLruLock(LockingLru *lru_ptr, void **lock_hook)
{
#ifdef CONFIG_USE_ANDERSON_SPINLOCK
		spinlock_anderson_lock(&(lru_ptr->spinlock_list), (spinlock_anderson_thread_t **)lock_hook);
#else
		pthread_spin_lock(&(lru_ptr->spinlock_list));
#endif
}

void LockingLruUnlock(LockingLru *lru_ptr, void *lock_hook)
{
#ifdef CONFIG_USE_ANDERSON_SPINLOCK
	spinlock_anderson_unlock(&(lru_ptr->spinlock_list), (spinlock_anderson_thread_t *)lock_hook);
#else
	pthread_spin_unlock(&(lru_ptr->spinlock_list));
#endif
}

/**
 * 	@brief: Extract the container object of a given raw list node item
 */
__pure static LockingLruItem *
_LruItemFromListNode(DoublyListNode *list_node_ptr)
{
	return (LockingLruItem *)((uintptr_t)list_node_ptr -	offsetof(LockingLruItem, list_node));
}

//----------- Recycer Type Pool LockingLruItem ---- //

void
InitLockingLruItemRecyclerTypePool(size_t group_allocation_sz)
{
	#define _LockingLruItem_EXPANSION_THRESHOLD (1024 * 10)
	if (IS_EMPTY(LockingLruItemPoolHandle)) {
		LockingLruItemPoolHandle = RecyclerInitTypePool("LockingLruItem", sizeof(LockingLruItem), group_allocation_sz,
                                                    _LockingLruItem_EXPANSION_THRESHOLD,
                                                    &ops_lockinglruitem_descriptor);

		syslog(LOG_INFO, "%s: Initialised TypePool: '%s'. TypeNumber:'%d', Block Size:'%lu'", __func__, LockingLruItemPoolHandle->type_name, LockingLruItemPoolHandle->type, LockingLruItemPoolHandle->blocksz);
	}
	else	syslog(LOG_INFO, "%s: TypePool already initialised", __func__);

}

void
LockingLruItemIncrementReference(InstanceHolderForLruItem *instance_descriptor_ptr, int multiples)
{
	RecyclerTypeReferenced(LockingLruItemPoolTypeNumber(), GetInstance(instance_descriptor_ptr), multiples);
}

void
LockingLruItemDecrementReference(InstanceHolderForLruItem *instance_descriptor_ptr, int multiples)
{
	RecyclerTypeUnReferenced(LockingLruItemPoolTypeNumber(), GetInstance(instance_descriptor_ptr), multiples);
}

__pure unsigned
LockingLruItemPoolTypeNumber()
{
	unsigned  type = LockingLruItemPoolHandle->type;
	return type;
}

InstanceHolder *
LockingLruItemGetInstance(ContextData *ctx_data_ptr, unsigned long call_flags)
{
	InstanceHolder *instance_holder_ptr = RecyclerGet(LockingLruItemPoolTypeNumber(), ctx_data_ptr, call_flags);
	if (unlikely(IS_EMPTY(instance_holder_ptr)))	goto return_error;

	return instance_holder_ptr;

	return_error:
	syslog(LOG_DEBUG, "%s {pid:'%lu', o:'%p', cid:'%lu', e:'%d'}: NOTICE: INCONSISTENT STATE: '%s'", __func__, pthread_self(), (void *)0, 0UL, 0, "Could not get LockingLruItem instance");
	return NULL;

}

void
LockingLruItemReturnToRecycler(InstanceHolder *instance_holder_ptr, ContextData *ctx_data_ptr, unsigned long call_flags)
{
	RecyclerPut(LockingLruItemPoolTypeNumber(), instance_holder_ptr, (ContextData *)ctx_data_ptr, call_flags);
}

/**
 * 	@brief: "constructor" type intialiser for newly instantiated objects just before attaching them to the recycler.
 * 	One off for the object's lifetime. No InstanceHolder ref yet.
 *
 */
static int
TypePoolInitCallback_LockingLruItem(ClientContextData *data_ptr, size_t oid)
{
	return 0;//success
}

/**
 * 	@param ContextData: whatever  context data we might have passed to the recycler when we issued Get().
 */
static int
TypePoolGetInitCallback_LockingLruItem(InstanceHolder *data_ptr, ContextData *context_data, size_t oid, unsigned long call_flags)
{

	return 0;//success
}

/**
 * 	@param ContextData: whatever  context data we might havepassed to the recycler when we issued Put In this instance Fence *
 */
static int
TypePoolPutInitCallback_LockingLruItem(InstanceHolder *data_ptr, ContextData *context_data, unsigned long call_flags)
{
	return 0;//success
}

static char *
TypePoolPrintCallback_LockingLruItem(InstanceHolder *data_ptr, ContextData *context_data, unsigned long call_flags)
{
	return NULL;
}

static int
TypePoolDestructCallback_LockingLruItem(InstanceHolder *data_ptr, ContextData *context_data, unsigned long call_flags)
{
	return 0;//success

}

////end typePool  /////////////////////