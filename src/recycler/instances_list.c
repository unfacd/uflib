
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

#include <stdlib.h>
#include <memory.h>
#include <uflib/recycler/instances_list.h>
#include <uflib/recycler/instance_type.h>

/**
 *  Instances List allows type-objects provided through the type-pool to hold multiple InstanceHolders. The object is therefore
 *  multiplexed across multiple users, each via a stable reference held via an InstanceHolder. Users never really own the object reference;
 *  rather, they own the InstanceHolder reference; ie they own an "instance of the object" via the holder. The scheme allows each user to define
 *  a 'marshaller' for their InstanceHolders, therefore objects can always be bootstrapped back into user's InstanceHolder.
 *  This scheme is designed to work with tight memory requirements and high population of objects that don't always need to be fully
 *  bootstrapped, but there is a requirement for them to keep a "live" pointer references around that can be bloomed very quickly.
 *  An InstanceHolder user should therefore check for the state of their InstanceHolder which can be in 'Instance' mode or 'Marshaller' mode.
 *
 *  The mechanism to "highjack" the objects and assigning them to other IstanceHolders is not implemented yet,
 *  but the foundations are there through the list and the mechanism of marshalling id reference. A marshaller callback
 *  will need to be need to used the id to bootstrap the object back and hand it over to owner.
 */

/**
 * @brief Allocate a slot for an InstanceHolder associated with the type object
 * @param ptr LinkedList of InstanceHolders associated with type-pool object
 * @return Newly created and linked-up List item
 */
ListItemInstance *
AddtoInstancesList(InstancesList *ptr)
{
  ListItemInstance *eptr = calloc(1, sizeof(ListItemInstance));

  if (!IsInstanceListEmpty(ptr)) {
    ptr->tail->next = eptr;
    ptr->tail = eptr;
  } else {
    ptr->head = ptr->tail = eptr;
  }

  ptr->size++;

  return (ListItemInstance *)eptr;

}

/**
 * @brief Allocate an InstanceHolder list-item and assign provided object to it.
 * @param lptr LinkedList of InstanceHolders associated with type-pool object
 * @param usr_ptr User data representing the type-pool object associated with InstanceHolder
 * @return List item newly created and linked-up
 */
ListItemInstance *
AddThisInstanceToList(InstancesList *lptr, ClientContextData *usr_ptr)
{
  ListItemInstance *eptr = AddtoInstancesList(lptr);
  ILIST_ITEM_INSTANCE(eptr) = usr_ptr;
  SetInstance(ILIST_ITEM_PTR(eptr), usr_ptr);

  return eptr;

}

/**
 * @brief Add an InstanceHolder in 'marshaller' state
 * @param lptr LinkedList of InstanceHolders associated with type-pool object
 * @param marshaller user defined marshaller. Can be a pointer or id
 * @return
 */
ListItemInstance *
AddThisMarshallerToList(InstancesList *lptr, MarshallerContextData marshaller)
{
  ListItemInstance *eptr = AddtoInstancesList(lptr);
  ILIST_ITEM_MARSHALLER(eptr) = marshaller;
  SetMarshaller(ILIST_ITEM_PTR(eptr), marshaller);

  return eptr;

}

/**
 * @brief Removes a unique reference to a given pool object
 * @param ptr
 * @param instance_holder_ptr obtained via pool allocation
 * @return
 */
int
RemoveThisInstanceFromList(InstancesList *ptr, InstanceHolder *instance_holder_ptr)
{
  int found = 0;
  ListItemInstance *lptr, *prev = NULL;

  lptr = ILIST_HEAD(ptr);

  while (IS_PRESENT(lptr) && !found) {
    if (IS_PRESENT(ILIST_ITEM_INSTANCE(lptr)) && (ILIST_ITEM_PTR(lptr) == instance_holder_ptr)) {
      found = 1;
    } else {
      prev = lptr;
      lptr = lptr->next;
    }
  }

  if (!found)  return 0;

  if (prev == NULL) {
    ptr->head = lptr->next;
  } else if (lptr->next == NULL) {
    ptr->tail = prev;
    ptr->tail->next = NULL;
  } else {
    prev->next = lptr->next;
  }

  ILIST_SIZE(ptr)--;

  memset(lptr, 0, sizeof(ListItemInstance));
  free(lptr);

  return 1;

}

int
RemoveFromInstancesList (InstancesList *ptr, ListItemInstance *eptr)
{
  int found = 0;
  ListItemInstance  *lptr,
                    *prev = NULL;

  lptr = ptr->head;

  while (IS_PRESENT(lptr) && !found) {
    if (lptr == eptr) {
      found = 1;
    } else {
      prev = lptr;
      lptr = lptr->next;
    }
  }

  if (!found)  return 0;

  if (IS_EMPTY(prev)) {
    ptr->head = lptr->next;
  } else if (IS_EMPTY(lptr->next)) {
    ptr->tail = prev;
    LOAD_NULL(ptr->tail->next);
  } else {
    prev->next = lptr->next;
  }

  ILIST_SIZE(ptr)--;

  memset (lptr, 0, sizeof(ListItemInstance));
  free (lptr);

  return 1;

}