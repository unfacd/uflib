/**
 * Copyright (C) 2015-2024 unfacd works
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

#ifndef UFLIB_SCHEDULED_JOBS_SCHEDULED_JOBS_H
#define UFLIB_SCHEDULED_JOBS_SCHEDULED_JOBS_H

#include <uflib/uflib_defs.h>

#include <uflib/scheduled_jobs/scheduled_jobs_type.h>
#include <uflib/main_types.h>
#include <uflib/buffer_descriptor/buffer_descriptor.h>

ScheduledJobs *GetScheduledJobsStore(void);
PUBLIC_API void InitScheduledJobsStore(ScheduledJobs *jobs_ptr, size_t count);
PUBLIC_API int RegisterScheduledJobType(ScheduledJobs *jobs_ptr, ScheduledJobType *job_type_ptr);
PUBLIC_API void ReInsertScheduledJob(ScheduledJobs *jobs_ptr, ScheduledJob *job_ptr);
PUBLIC_API int InsertScheduledJob(ScheduledJobs *jobs_ptr, ScheduledJob *job_ptr);
ScheduledJobContext *GetScheduledJob(ScheduledJobs *jobs_ptr, unsigned lock_hints, ScheduledJobContext *context_ptr_out);
ScheduledJobContext *GetRemScheduledJob(ScheduledJobs *jobs_ptr, unsigned lock_hints, ScheduledJobContext *context_ptr_out);
PUBLIC_API bool IsJobPeriodic(ScheduledJob *job_ptr);
/**
 * @brief Check whether a job type name is already registered.
 *
 * @warning The caller **must** hold `jobs_ptr->spin_lock` before calling this
 *          function.  It iterates over the shared mutable job_types_index[]
 *          array without acquiring the lock internally.
 *
 * @note This requirement will be enforced structurally in V2
 *       (ScheduledJobStore) — the opaque handle will make direct lock
 *       access impossible, and the lock-hints enum will provide controlled
 *       synchronisation.
 *
 * @param jobs_ptr   The job store (must be locked by caller)
 * @param type_name  The type name to look up
 * @return true if the type name is registered, false otherwise
 *
 * @code{.c}
 * // ── CORRECT — caller holds the lock ─────────────────────────────
 * pthread_spin_lock(&jobs_ptr->spin_lock);
 * if (IsJobTypeNameRegistered(jobs_ptr, "fence_cleanup")) {
 *     // type already registered — skip
 *     pthread_spin_unlock(&jobs_ptr->spin_lock);
 *     return;
 * }
 * // ... register the type, assign type_id ...
 * pthread_spin_unlock(&jobs_ptr->spin_lock);
 *
 * // ── WRONG — data race; no lock held ─────────────────────────────
 * if (IsJobTypeNameRegistered(jobs_ptr, "fence_cleanup")) {   // ← UB!
 *     // another thread may be inside RegisterScheduledJobType()
 *     // concurrently modifying job_types_index[] and job_types_size
 * }
 * @endcode
 */
PUBLIC_API bool IsJobTypeNameRegistered(ScheduledJobs *jobs_ptr, const char *type_name);
PUBLIC_API CallbackOnCompareKeys GetDefaultComparatorForTimeValue(void);
PUBLIC_API int WorkerThreadScheduledJobExecutor(MessageContextData *context_ptr);
PUBLIC_API int WorkerThreadScheduledJobFirstInsertedExecutor(MessageContextData *context_ptr);
PUBLIC_API int TimeValueComparator(void *key1, void *key2);
PUBLIC_API void DestructScheduledJobs(ScheduledJobs *jobs_ptr);
PUBLIC_API size_t GetScheduleJobsSetsize(ScheduledJobs *jobs_ptr, unsigned lock_hints);

/* ── Introspection (JSON) ──────────────────────────────────────────────── */

/**
 * @brief Describe the scheduler's state as a JSON string.
 *
 * Composes a snapshot: the registered job-types (count + per-type index,
 * name, id, frequency/currency modes, frequency) and the pending jobs (count
 * + per-job fire-time, owning type name, and job pointer).  The function
 * takes `jobs_ptr->spin_lock`, so the snapshot is exact and must not be
 * called from a thread that already holds the lock.
 *
 * Pending jobs are emitted in the heap's backing-array order — *not*
 * fire-time order (the store is a binary min-heap; its root is the earliest
 * job but the array is not sorted).
 *
 * If @p provided is NULL, a fresh BufferDescriptor is allocated (the caller
 * must free it with BufferDescriptorRelease() + free()); otherwise the
 * caller's descriptor is appended to.  Returns @p provided, or NULL only if a
 * fresh descriptor could not be allocated.
 *
 * @param jobs_ptr  Scheduler (must be initialised via InitScheduledJobsStore;
 *                  NULL → emits `{"error":"null handle"}`).
 * @param provided  Optional caller-owned BufferDescriptor (NULL → allocate).
 * @return The populated BufferDescriptor.
 *
 * @code{.c}
 * BufferDescriptor bd;
 * BufferDescriptorInit(&bd, 512);
 * DescribeScheduledJobs(jobs, &bd);
 * printf("%s\n", bd.data);
 * BufferDescriptorRelease(&bd);
 * @endcode
 */
PUBLIC_API BufferDescriptor *
DescribeScheduledJobs(ScheduledJobs *jobs_ptr, BufferDescriptor *provided);

#endif /* UFLIB_SCHEDULED_JOBS_SCHEDULED_JOBS_H */
