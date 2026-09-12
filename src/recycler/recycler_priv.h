/**
 * @file recycler_priv.h
 * @brief Recycler private types — exposed for test access only, never installed
 *
 * This header is colocated with recycler.c under src/.  It exposes internal
 * implementation types (RecyclerPoolTypeEnvelop, TypePoolAllocationGroup,
 * RecyclerPoolDefinition, Recycler) for unit-test inspection.
 *
 * CONSUMERS MUST NEVER INCLUDE THIS HEADER.
 */

#ifndef UFLIB_RECYCLER_RECYCLER_PRIV_H
#define UFLIB_RECYCLER_RECYCLER_PRIV_H

#include <pthread.h>
#include <stdatomic.h>
#include <uflib/recycler/recycler_type.h>
#include <uflib/recycler/instance_type.h>
#include <uflib/recycler/instances_list_type.h>

typedef struct RecyclerPoolTypeEnvelop {
	atomic_size_t         _refcount;
	size_t                oid;            // unique across Type, fixed for lifetime
	InstancesList         instances_list; // linked list of InstanceHolders
} RecyclerPoolTypeEnvelop;

typedef struct TypePoolAllocationGroup {
	unsigned                    groupid;
	size_t                      pool_size;
	size_t                      head, tail;
	size_t                      leased_size;
	void                       *pool_memory;
	RecyclerPoolTypeEnvelop   **pool_queue;
	pthread_spinlock_t          spin_lock;
} TypePoolAllocationGroup;

typedef struct RecyclerPoolDefinition {
	RecyclerPoolHandle          pool_handle;
	pthread_spinlock_t          spin_lock;
	unsigned                    expasnion_threshold;
	RecyclerPoolOps             ops;
	void                      **pool_memory2;
	int                         allocated_groups_sz;
	size_t                      current_max_capacity;
	size_t                      group_allocation_sz;
	TypePoolAllocationGroup   **allocation_groups;
} RecyclerPoolDefinition;

typedef struct Recycler {
	RecyclerPoolDefinition **recyclers;
	unsigned                 count_typeslots;
	unsigned                 count_types;
} Recycler;

#endif /* UFLIB_RECYCLER_RECYCLER_PRIV_H */
