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

#ifndef UFLIB_RECYCLER_RECYCLER_LOG_STRINGS_H
#define UFLIB_RECYCLER_RECYCLER_LOG_STRINGS_H

enum {
  LOGCODE_RECYCLER=1300,
  LOGCODE_RECYCLER_ENQUE_REFCNT_SHORT=1301,LOGCODE_RECYCLER_GET_FULLY_LEASED=1302,LOGCODE_RECYCLER_PUT_ON_FULL=1303,LOGCODE_RECYCLER_ENQUE_SUCCESS=1304,LOGCODE_RECYCLER_DEQUE_SUCCESS=1305,
  LOGCODE_RECYCLER_ENQUE_ERROR=1306,LOGCODE_RECYCLER_DEQUE_ERROR=1307,LOGCODE_RECYCLER_UNDEFINED_GROUPALLOC=1308,LOGCODE_RECYCLER_UNDEFINED_POOLDEF=1309,
  LOGCODE_RECYCLER_QUEUE_ITEM_REFCOUNT_ERROR=1310, LOGCODE_RECEYCLER_ALLOC_GROUP_IDX_ERR=1311,LOGCODE_RECYCLER_MAX_ALLOC_CAPACITY_REACHED=1312,
  LOGCODE_RECYCLER_ALLOC_GROUP_EXPANDED=1313, LOGCODE_RECYCLER_UNCOLLECTED_INSTANCES=1314, LOGCODE_RECYCLER_INSTANCE_NOT_ON_LIST=1315, LOGCODE_RECYCLER_MARSHALLER_INSTANCE=1316,
  LOGCODE_RECYCLER_NEW_INSTANCE_ERROR=1217,
};

#define LOGSTR_RECYCLER_ENQUE_REFCNT_SHORT	"%s {tail_pos:'%lu', refcount:'%lu', groupid:'%u', o:'%p', rid:'%lu', type_name:'%s, leased_sz:'%lu', e:'%d'}: NOTICE: REFCOUNT DID NOT REACH 1: COULD NOT ENQUEUE"
#define LOGSTR_RECYCLER_GET_FULLY_LEASED	"%s: {pid:'%lu' type_name:'%s, allocated_groups_sz:'%d', e:'%d'}: NOTICE: Type POOL FULLY LEASED: EXPAPD..."
#define LOGSTR_RECYCLER_PUT_ON_FULL	"%s {pid:'%lu', head_pos:'%lu', tail_pos:'%lu', type_name:'%s', leased_sz:'%lu' e:'%d'}: ERROR: POOL QUEUE WOULD OVER FLOW ON PUT REQUEST"
#define LOGSTR_RECYCLER_ENQUE_SUCCESS	"%s {pid:'%lu', groupid:'%u', tail_pos:'%lu', head_pos:'%lu', o:'%p', rid:'%lu', type_name:'%s, leased_sz:'%lu', e:'%d'}: Recycler: EnQueued item..."
#define LOGSTR_RECYCLER_DEQUE_SUCCESS	"%s {pid:'%lu', groupid:'%u', tail_pos:'%lu', head_pos:'%lu', o:'%p', rid:'%lu', type_name:'%s, leased_sz:'%lu', e:'%d'}: Recycler: DeQueued item..."
#define LOGSTR_RECYCLER_ENQUE_ERROR	"%s {pid:'%lu', tail_pos:'%lu', head_pos:'%lu', o:'%p', rid:'%lu', type_name:'%s, leased_sz:'%lu', e:'%d'}: ERROR: Recycler: COULD NOT ENQUEUE ELEMENT..."
#define LOGSTR_RECYCLER_DEQUE_ERROR	"%s {pid:'%lu', tail_pos:'%lu', head_pos:'%lu', o:'%p', rid:'%lu', type_name:'%s, leased_sz:'%lu', e:'%d'}: ERROR: Recycler: COULD NOT DEQUEUE ELEMENT..."
#define LOGSTR_RECYCLER_QUEUE_ITEM_REFCOUNT_ERROR	"%s {pid:'%lu', tail_pos:'%lu', head_pos:'%lu', o:'%p', grpipid:'%u', rid:'%lu', refcount:'%lu', type_name:'%s, leased_sz:'%lu', e:'%d'}: ERROR: QUEUE ITEM HAS UNEXPECTED REFCOUNT: '%s'"
#define LOGSTR_RECEYCLER_ALLOC_GROUP_IDX_ERR	"%s {pid:'%lu, rid:'%lu', pool_sz:'%u', allocation_group_idx:'%lu', e:'%d'}: ERROR: COULD NOT RETRIEVE AllocationGroup index..."
#define LOGSTR_RECYCLER_MAX_ALLOC_CAPACITY_REACHED "%s {pid:'%lu' allocated_groups:'%lu' expansion_size:'%d', e:'%d'}: REACHED MAXIMUM ALLOCATION CAPACITY..."
#define LOGSTR_RECYCLER_ALLOC_GROUP_EXPANDED	"%s {type_name:'%s', groupid:'%u', head_pos:'%lu', tail_pos:'%lu', queue_alloc_sz:'%d', block_sz:'%lu', alloc_groups_sz:'%lu', max_capacity:'%lu', e:'%d'}: AllocationGroup Expanded..."
#define LOGSTR_RECYCLER_UNCOLLECTED_INSTANCE "%s pid:'%lu', groupid:'%u', tail_pos:'%lu', head_pos:'%lu', o:'%p', rid:'%lu', type_name:'%s, leased_sz:'%lu', instances_sz:'%lu', e:'%d'}: ERROR: UNCOLLECTED INSTANCES FOUND: SHOULD MAX OF 1"
#define LOGSTR_RECYCLER_INSTANCE_NOT_ON_LIST "%s pid:'%lu', env:'%p', o:'%p', e:'%d'}: ERROR: INSTANCE NOT FOUND ON OWN TYPE INSTANCES LIST"
#define LOGSTR_RECYCLER_MARSHALLER_INSTANCE "%s pid:'%lu', o:'%p', marshaller_id:'%lu', e:'%d'}: ERROR: INSTANCE IS IN MARSHALLER STATE"
#define LOGSTR_RECYCLER_NEW_INSTANCE_ERROR	"%s: {pid:'%lu' o:'%p, e:'%d'}: ERROR: COULD NOT CREATE NEW INSTANCE HOLDER"

#endif //UFSRV_RECYCLER_LOG_STRINGS_H
