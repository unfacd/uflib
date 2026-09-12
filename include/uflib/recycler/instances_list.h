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


#ifndef UFLIB_RECYCLER_INSTANCES_LIST_H
#define UFLIB_RECYCLER_INSTANCES_LIST_H

#include <uflib/uflib_defs.h>

#include <uflib/main_types.h>
#include "instance_type.h"
#include "instances_list_type.h"

ListItemInstance *AddtoInstancesList (InstancesList *ptr);
ListItemInstance *AddThisInstanceToList (InstancesList *lptr, ClientContextData  *usrptr);
ListItemInstance *AddThisMarshallerToList (InstancesList *lptr, MarshallerContextData marshaller);
PUBLIC_API int RemoveThisInstanceFromList (InstancesList *ptr, InstanceHolder *instance_holder_ptr);
PUBLIC_API int RemoveFromInstancesList (InstancesList *ptr, ListItemInstance *eptr);

inline static bool
IsInstanceListEmpty (const InstancesList *ptr)
{
  return (ILIST_SIZE(ptr) == 0);

}

#endif //UFSRV_INSTANCES_LIST_H
