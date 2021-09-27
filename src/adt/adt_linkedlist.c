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

#include <uflib/standard_c_includes.h>
#include <uflib/standard_defs.h>
#include <uflib/adt/adt_linkedlist.h>

 ListEntry *
 AddtoList (List *ptr)
 {
  const size_t sizeofListEntry = sizeof(ListEntry);
  ListEntry *eptr;

   eptr = calloc(1, sizeofListEntry);

  if (!ListEmpty(ptr)) {
    ptr->tail->next = eptr;
    ptr->tail = eptr;
  } else {
    ptr->head = ptr->tail = eptr;
  }

 ptr->nEntries++;

 return (ListEntry *)eptr;

 }

 ListEntry *
 AddThisToList (List *lptr, void *usrptr)
 {
   ListEntry *eptr;

   eptr = AddtoList(lptr);
   eptr->whatever = usrptr;

   return (ListEntry *)eptr;

 }

 int RemoveThisFromList (List *ptr, void *usrptr)
 {
  int found = 0;
  ListEntry *lptr, *prev = NULL;

  if (!usrptr)  return 0;

  lptr = ptr->head;

  while ((lptr != NULL) && (!found)) {
    if (IS_PRESENT(lptr->whatever) && (lptr->whatever == usrptr)) {
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

  ptr->nEntries--;

  memset (lptr, 0, sizeof(ListEntry));
  free (lptr);

  return 1;

 }

 int
 RemovefromList (List *ptr, ListEntry *eptr)
 {
  int found = 0;
  register ListEntry *lptr,
                     *prev = NULL;

   lptr = ptr->head;

    while ((lptr != NULL) && (!found)) {
      if (lptr == eptr) {
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

   ptr->nEntries--;

   memset(lptr, 0, sizeof(ListEntry));
   free(lptr);

   return 1;

 }

 int
 RemovefromListDeep (List *ptr, ListEntry *eptr, size_t payload)
 {
  int found = 0;
  register ListEntry *lptr,
                     *prev = NULL;

   lptr = ptr->head;

    while ((lptr != NULL) && (!found)) {
      if (lptr == eptr) {
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

   ptr->nEntries--;
    memset(lptr->whatever, 0, payload);
	  free(lptr->whatever); //session ptr etc...
   
	memset(lptr, 0, sizeof(ListEntry));
	free(lptr);

   return 1;

 }

 int
 RemoveListHead (List *lptr, int n, void (*free_func)(ListEntry *))

 {
  ListEntry *eptr;

    if (n>lptr->nEntries)  n = lptr->nEntries;

    while (n--) {
      eptr = lptr->head;
      lptr->head = lptr->head->next;

       if (free_func) {
         (*free_func)(eptr);
        }

      free (eptr);

      lptr->nEntries--;
     }

   return lptr->nEntries;

 }

 int
 RemoveListEntry (List *lptr, ListEntry *eptr, int how, void (*f)(void *), ...)

 {
#if 0
   switch (how)
    {
     case EVERYTHING:
      {
       int i=0;
       register ListEntry *eptr=lptr->head;
		ListEntry *aux;

	 while (eptr!=NULL)
	   {
	    ++i;
	    aux=eptr->next;
	     if (eptr->whatever)
	       {
		if (f)
		  {  
		   (*f) (eptr->whatever);
		  }
		else
		  {
		   free (eptr->whatever);
		  }
	       } 

	    free (eptr);
	    eptr=aux;
	   }

	lptr->nEntries=0;
	lptr->head=lptr->tail=NULL;

	return i;
      }

     case NAME:
      {
       register ListEntry *eptr;

	 if (eptr->whatever)
	  {
	   if (f)
	    {
	     (*f) (eptr->whatever);
	    }
	   else
	    {
	     free (eptr->whatever);
	    }
	  }

	RemovefromList (lptr, eptr);

	return 1;
      }

     case NUMBER:
	{
	 int *m=0,
	      n=0;
	 register ListEntry *eptr=lptr->head;
	  /*m=(int *)v;*/

	   if (*m>lptr->nEntries)
	    {
	     say ("You don't have that many entries.\n");

	     return 0;
	    }

	   for ( ; eptr!=NULL; eptr=eptr->next)
	    {
	      if (++n==*m)
	       {

		return 1;
	       }
	    }

	 return 0;
	}
    }
#endif

   return 0; 

 }  /**/

 List *
 ListfromArray (void *a, size_t sz, size_t n)
 {
  const size_t sizeofList = sizeof(List);
  register int i = 0, j = 0;
  List *ptr;
  ListEntry *lptr;

  if (!a)  return NULL;

   ptr = calloc(1, sizeofList);

    for ( ; i<=n-1; i++) {
       lptr = AddtoList(ptr);
       lptr->whatever = (a + j);
       j += sz;
    }

   return (List *)ptr;

 }  /**/

 void
 CleanupListfromArray (List *ptr)
 {
  register ListEntry *eptr,
                     *aux;

    if (!ptr)  return;

   eptr = ptr->head;
 
    while (eptr != NULL) {
      aux = eptr->next;
      RemovefromList(ptr, eptr);
      eptr = aux;
     }

   memset (ptr, 0, sizeof(List));
   free (ptr); 

 }  /**/

 ListEntry *
 LocateEntry (List *ptr, ListEntry *eptr)

 {
  int found=0;
  register ListEntry *aux;

   aux=ptr->head;

    while ((aux!=NULL)&&!(found))
     {
       if (aux==eptr)
	{
	 found=1;
	}
       else
	{
	 aux=aux->next;
	}
     }

    if (!found)
     {
      return (ListEntry *)NULL;
     }

   return (ListEntry *)aux;

 }  /**/  

 bool
 ListEmpty (const List *ptr)

 {
   return ((ptr->nEntries==0));

 }  /**/

#if 0
typedef void ClientPayload;
typedef struct DoublyLinkedListEntryEnvelope {
	struct dListEntry	*next,
						*prev;

	ClientPayload		*client_payload;
 } DoublyLinkedListEntryEnvelope;

 typedef struct DoublyLinkedList {
	size_t		nEntries;
	dListEntry	*head,
				*tail;
} DoublyLinkedList;

 ClientPayload *DoublyLinkedListInsert (DoublyLinkedList *dlist_ptr, ClientPayload *playload_ptr)
 {
	 void *chunk_ptr=calloc(1, sizeof(DoublyLinkedListEntryEnvelope)+sizoef(ClientPayload *));
 }
#endif

 dListEntry *
 AddtodList (dList *ptr)

 {
	dListEntry *aux = calloc(1, sizeof(dListEntry));

	if (!dListEmpty(ptr)) {
		aux->prev = ptr->tail;
		ptr->tail->next = aux;
		ptr->tail = aux;
	} else {
		ptr->head = ptr->tail = aux;
		aux->prev = NULL;
	}

	ptr->nEntries++;

	return (dListEntry *)aux;
 }

 dListEntry *
 AddThisTodList (dList *dlist_ptr, void *usrptr)
 {
	dListEntry *eptr;

	eptr = AddtodList(dlist_ptr);
	eptr->whatever = usrptr;

	return (dListEntry *)eptr;
 }

 dListEntry *
 RemovefromdList (dList *dlist_ptr, dListEntry *eptr)
 {
	dListEntry	*next,
				*prev;

	next = eptr->next;
	prev = eptr->prev;

	if (dlist_ptr->head == eptr)  dlist_ptr->head = next;
	if (dlist_ptr->tail == eptr)  dlist_ptr->tail = prev;

	if (next != NULL)  next->prev = prev;
	if (prev != NULL)  prev->next=next;

	free (eptr);
	dlist_ptr->nEntries--;

	return (next);
 }

 bool
 dListEmpty (const dList *ptr)
 {
   return (ptr->nEntries == 0);
 }