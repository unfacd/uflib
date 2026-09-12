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

#ifndef UFLIB_RECYCLER_INSTANCES_LIST_TYPE_H
#define UFLIB_RECYCLER_INSTANCES_LIST_TYPE_H

#include <uflib/main_types.h>
#include "instance_type.h"

typedef struct ListItemInstance {
  InstanceHolder          item;
  struct ListItemInstance *next;
} ListItemInstance;

typedef struct InstancesList {
  size_t            size;
  ListItemInstance  *head,
                    *tail;
} InstancesList;

#define ILIST_ITEM_NEXT(x) ((x)->next)
#define ILIST_ITEM(x) ((x)->item)
#define ILIST_ITEM_PTR(x) &(ILIST_ITEM(x))//todo remove
#define ILIST_ITEM_INSTANCE(x) (ILIST_ITEM(x).holder.instance)
#define ILIST_ITEM_MARSHALLER(x) (ILIST_ITEM(x).holder.marshaller)

#define ILIST_SIZE(x) ((x)->size)
#define ILIST_HEAD(x) ((x)->head)
#define ILIST_TAIL(x) ((x)->tail)

#endif //UFSRV_INSTANCES_LIST_TYPE_H
