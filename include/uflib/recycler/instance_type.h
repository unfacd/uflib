/*

 Copyright (c) 2015-2025 unfacd works

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

#ifndef UFLIB_RECYCLER_INSTANCE_TYPE_H
#define UFLIB_RECYCLER_INSTANCE_TYPE_H

#include <uflib/main_types.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>

//classic tagged pointer implementation
//for pointer representation a 2bit tag is permissible
//pppppppp|pppppppp|pppppppp|pppppppp|pppppppp|pppppppp|pppppppp|pppppTT0
//^- actual pointer                                     two tag bits -^ ^- last bit 0
//for marshaller the last bit is turned on, thus only allowing for 63bit worth of value
//iiiiiiii|iiiiiiii|iiiiiiii|iiiiiiii|iiiiiiii|iiiiiiii|iiiiiiii|iiiiiii1
//^- actual marshaller                                                  ^- last bit 1
//isMarshaller: 1, isInstance:'0',  Marshaller value: 100
//isMarshalller: 0, isInstance:'1',  tag:'1', Type value: ayman

typedef uintptr_t MarshallerContextData;

//This allows the application to use stable object references. However, the underlying object is not always guaranteed
//to be allocated to the 'instance'. Where that is so, the marshaller remembers an application specific id-reference that is used
//to bootstrap the instance back again. This allows memory constrained apps to recycle objects around.
// Therefore user may need to check the actual state of the instance with macros IsInstance() or IsMarshaller()
typedef struct InstanceHolder {
  union {
    ClientContextData     *instance; //actual allocated object (typically via typepool)
    MarshallerContextData marshaller; //instance marshaller id eg cid, fid etc... used to bootstrap the object again after it's been "highjacked" for other use
  } holder;
} InstanceHolder;

//convenient type that combines InstanceHolder and its resolved instance reference
typedef struct InstanceContext {
  InstanceHolder *instance_holder;
  ClientContextData *instance;
} InstanceContext;

#define AS_INSTANCE_HOLDER(x) ((InstanceHolder *)(x))
#define AS_INSTANCE_CONTEXT(x) ((InstanceContext *)(x))

//8-byte alignment. In the general case must be  > 1 to store a 2byte-aligned int
//In the general case should be power of 2: BYTE_ALIGNMENT != 0 && ((BYTE_ALIGNMENT & (BYTE_ALIGNMENT - 1)) == 0)
#define BYTE_ALIGNMENT 8

//for 8-byte alignment (8-1) aka 0b00000000000111
#define TAG_MASK (BYTE_ALIGNMENT-1)

#define INSTANCE_MASK (~TAG_MASK)

#define DEFAULT_TAG 1

inline static bool IsInstance(InstanceHolder *holder_ptr) {
  return (holder_ptr->holder.marshaller & 1) == 0;
}

inline static bool IsMarshaller(InstanceHolder *holder_ptr)  {
  return (holder_ptr->holder.marshaller & 1) == 1;
}

/**
 * @brief Tag Instance with max value of 2-bit. Tagging doesn't apply when InstanceHolder is in "Marshaller" mode.
 * Under current implementation all Instances are tagged with the value '1'
 * @param holder_ptr Pre-allocated InstanceHolder
 * @param instance_ptr Pe-alocated object to assign as Instance
 * @param tag 2-bit max value.
 */
static inline void SetInstanceWithTag(InstanceHolder *holder_ptr, void *instance_ptr, uint8_t tag)
{
  assert(((uintptr_t)(instance_ptr) & TAG_MASK) == 0);// make sure that the pointer really is aligned
  assert(((tag << 1) & INSTANCE_MASK) == 0);// make sure that the tag isn't too large

  // last bit isn't part of tag anymore, but just zero, thus the << 1
  holder_ptr->holder.marshaller = (uintptr_t)instance_ptr | (tag << 1);
}

/**
 * @brief Assign object to the InstanceHolder's instance type. The InstanceHolder is now in 'Instance'
 *  mode.
 * @param holder_ptr Pre-allocated InstanceHolder
 * @param instance_ptr Actual object
 */
static inline void SetInstance(InstanceHolder *holder_ptr, void *instance_ptr)
{
  SetInstanceWithTag(holder_ptr, instance_ptr, DEFAULT_TAG);
}

/**
 * @brief Assign a user-defined marshaller to the InstanceHolder. This InstanceHolder is now in 'Marshaller' mode
 * therefore its object cannot be used without bootstrapping it again through its marshaller.
 * @param holder_ptr Pre-allocated InstanceHolder
 * @param marshaller_id user defined id reference
 */
static inline void SetMarshaller(InstanceHolder *holder_ptr, uintptr_t marshaller_id) {
  // make sure that when we << 1 there will be no data loss, i.e. make sure that it's a 31 bit / 63 bit integer
  assert(((marshaller_id << 1) >> 1) == marshaller_id);

  holder_ptr->holder.marshaller = (marshaller_id << 1) | 1; // shift the number to the left and set the lowest bit to 1
}

/**
 * @brief Retrieve the object associated with the InstanceHolder
 * @param holder_ptr Pre-allocated InstanceHolder
 * @return object reference
 */
static inline void *GetInstance(InstanceHolder *holder_ptr)  {
#if UF_DEBUG_BUILD
  assert(IsInstance(holder_ptr));
#endif

  return (void *)(holder_ptr->holder.marshaller & INSTANCE_MASK);
}

static inline int GetTag(InstanceHolder *holder_ptr) {
#if UF_DEBUG_BUILD
  assert(IsInstance(holder_ptr));
#endif

  return (holder_ptr->holder.marshaller & TAG_MASK) >> 1;
}

static inline uintptr_t GetMarshaller(InstanceHolder *holder_ptr)  {
#if UF_DEBUG_BUILD
  assert(IsMarshaller(holder_ptr));
#endif

  return holder_ptr->holder.marshaller >> 1;
}

#endif //UFSRV_INSTANCE_TYPE_H
