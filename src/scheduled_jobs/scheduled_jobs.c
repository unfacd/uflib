/**
 * Copyright (C) 2015-2026 unfacd works
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

#include <inttypes.h>
#include <stdint.h>
#include <stdlib.h>
#include <syslog.h>
#include <string.h>
#include <pthread.h>

#include <uflib/scheduled_jobs/scheduled_jobs.h>
#include <uflib/adt/adt_minheap.h>

#define JOB_INDEX_EXPANSION_THRESHOLD 10

static size_t _ExpandJobTypesIfNecessary(ScheduledJobs *jobs_ptr);

void
InitScheduledJobsStore(ScheduledJobs *jobs_ptr, size_t count)
{
	pthread_spin_init(&(jobs_ptr->spin_lock), 0);
	jobs_ptr->scheduled_jobs_store = MinHeapCreateI64((int)count);
}

/**
 * 	@brief One-off per job type. Safe to call multiple times on the same type
 * 	@param job_type_ptr: must be heap allocated or similar by user
 * 	@locks ScheduledJobs *: to prevent concurrent expansion
 */
int
RegisterScheduledJobType(ScheduledJobs *jobs_ptr, ScheduledJobType *job_type_ptr)
{
	pthread_spin_lock(&(jobs_ptr->spin_lock));

	if (IsJobTypeNameRegistered(jobs_ptr, job_type_ptr->type_name)) {
		pthread_spin_unlock(&(jobs_ptr->spin_lock));
		return job_type_ptr->type_id;
	}

	int type_index = _ExpandJobTypesIfNecessary(jobs_ptr);
	jobs_ptr->job_types_descriptor.job_types_index[type_index]	=	job_type_ptr;
	job_type_ptr->type_id																				=	type_index;
	jobs_ptr->job_types_descriptor.job_types_size++;

	pthread_spin_unlock(&(jobs_ptr->spin_lock));

	syslog(LOG_INFO, "%s {type_id:'%d', type_name:'%s'}: SUCCESS: Initialised ScheduledJob Type...", __func__, job_type_ptr->type_id, job_type_ptr->type_name);

	return type_index;
}

/**
 * @brief Insert given job to the scheduler for first time, and set it to fire off relative to time job inserted + provided job frequency
 * @param job_ptr: Must be permanently allocated (heaps, static..)
 * @return return value from on_first_insert callback if set
 */
int
InsertScheduledJob(ScheduledJobs *jobs_ptr, ScheduledJob *job_ptr)
{
  ReInsertScheduledJob(jobs_ptr, job_ptr);

  /*if (IS_PRESENT(job_ptr->job_type_ptr->callbacks.on_first_insert)) {
    return (*job_ptr->job_type_ptr->callbacks.on_first_insert)(AS_JOB_CONTEXT(job_ptr), AS_CLIENT_CONTEXT_DATA(job_ptr->context_data));
  }*/

  return 0;//todo return is ambiguous and conflicts with return value from on_first_insert above
}

/**
 * @brief Re insert given job to the scheduler after if was fired off, and set it to fire off relative to time job inserted + provided job frequency
 * @param job_ptr: Must be permanently allocated (heaps, static..)
 */
void
ReInsertScheduledJob(ScheduledJobs *jobs_ptr, ScheduledJob *job_ptr)
{
	long long time_now = job_ptr->job_type_ptr->callbacks.on_get_time();
	long long when_to_fire = job_ptr->when_to_schedule > 0? job_ptr->when_to_schedule : job_ptr->job_type_ptr->frequency;

	pthread_spin_lock(&(jobs_ptr->spin_lock));
	job_ptr->when_scheduled = time_now;
	MinHeapInsertI64(jobs_ptr->scheduled_jobs_store, time_now + when_to_fire, (void *)job_ptr);
	pthread_spin_unlock(&(jobs_ptr->spin_lock));
}

/**
 * @brief Retrieve the job with earliest schedule time.
 * @param jobs_ptr Pre-allocated jobs store
 * @param lock_hints Whether to keep the lock on after job retrieval
 * @param context_ptr_out User allocated for returning retrieved job
 * @return Retrieved job or NULL. IMPORTANT: job returned by value, not reference, so user must allocate Job object (not just pointer)
 */
ScheduledJobContext *
GetScheduledJob(ScheduledJobs *jobs_ptr, unsigned lock_hints, ScheduledJobContext *context_ptr_out)
{
	ScheduledJob 	*job_ptr;
	int64_t time_key;

	if (!(lock_hints & LOCK_HINT_ALREADY_LOCKED)) pthread_spin_lock(&(jobs_ptr->spin_lock));

	if ((MinHeapMinI64(jobs_ptr->scheduled_jobs_store, &time_key, (void **) &job_ptr)) == 1) {
		context_ptr_out->scheduled_job_ptr=	job_ptr;
		context_ptr_out->time_key					=	time_key;

		if (!(lock_hints & LOCK_HINT_KEEP_LOCKED)) pthread_spin_unlock(&(jobs_ptr->spin_lock));

		return context_ptr_out;
	}

	if (!(lock_hints & LOCK_HINT_KEEP_LOCKED)) pthread_spin_unlock(&(jobs_ptr->spin_lock));

	return NULL;
}

/**
 * @brief Retrieve the job with earliest schedule time and detatch from the structure, therefore having the side effect
 * of promoting the next earliest job.
 * @param jobs_ptr Pre-allocated jobs store
 * @param lock_hints Whether to keep the lock on after element retrieval
 * @param context_ptr_out The retreieved job
 * @return
 */
ScheduledJobContext *
GetRemScheduledJob(ScheduledJobs *jobs_ptr, unsigned lock_hints, ScheduledJobContext *context_ptr_out)
{
	ScheduledJob 	*job_ptr;
	int64_t time_key;

	if (!(lock_hints & LOCK_HINT_ALREADY_LOCKED)) pthread_spin_lock(&(jobs_ptr->spin_lock));

	if ((MinHeapDelminI64(jobs_ptr->scheduled_jobs_store, &time_key, (void **) &job_ptr)) == 1) {
		context_ptr_out->scheduled_job_ptr=	job_ptr;
		context_ptr_out->time_key					=	time_key;

		if (!(lock_hints & LOCK_HINT_KEEP_LOCKED)) pthread_spin_unlock(&(jobs_ptr->spin_lock));
		return context_ptr_out;
	}

	if (!(lock_hints & LOCK_HINT_KEEP_LOCKED)) pthread_spin_unlock(&(jobs_ptr->spin_lock));

	return NULL;
}

__pure bool
IsJobPeriodic(ScheduledJob *job_ptr)
{
	return (job_ptr->job_type_ptr->frequency_mode == PERIODIC);
}

/*
 * WARNING: Caller must hold jobs_ptr->spin_lock before calling this function.
 *
 * The function iterates over the shared mutable job_types_index[] array.  It is
 * NOT annotated __pure — the return value depends on mutable state reachable
 * through the pointer argument, and the compiler must not cache or reorder calls.
 *
 * The only current caller (RegisterScheduledJobType) holds the lock, but this
 * function is PUBLIC_API and may be called directly by consumers.  The V2 opaque
 * handle (ScheduledJobStore) will eliminate direct struct access and make the
 * lock requirement impossible to violate.
 */
bool
IsJobTypeNameRegistered(ScheduledJobs *jobs_ptr, const char *type_name)
{
	bool type_registered = false;

	if (jobs_ptr->job_types_descriptor.job_types_size == 0)	return false;

	for (size_t i = 0; i < jobs_ptr->job_types_descriptor.job_types_size; i++) {
		if ((strcmp(type_name, jobs_ptr->job_types_descriptor.job_types_index[i]->type_name) == 0))
			return true;
	}

	return type_registered;
}

__attribute__ ((const)) CallbackOnCompareKeys
GetDefaultComparatorForTimeValue(void)
{
	return TimeValueComparator;
}

int
TimeValueComparator(void *key1, void *key2)
{
	int64_t a = (int64_t)(intptr_t)key1;
	int64_t b = (int64_t)(intptr_t)key2;
	return (a > b) - (a < b);
}

int
WorkerThreadScheduledJobExecutor(MessageContextData *context_ptr)
{
	ScheduledJob *job_ptr = (ScheduledJob *)context_ptr;
	return (*job_ptr->job_type_ptr->callbacks.on_run)(AS_JOB_CONTEXT(job_ptr), AS_CLIENT_CONTEXT_DATA(job_ptr->context_data));
}

int
WorkerThreadScheduledJobFirstInsertedExecutor(MessageContextData *context_ptr)
{
  ScheduledJob *job_ptr = (ScheduledJob *)context_ptr;
  if (IS_PRESENT(job_ptr->job_type_ptr->callbacks.on_first_insert)) {
    return (*job_ptr->job_type_ptr->callbacks.on_first_insert)(AS_JOB_CONTEXT(job_ptr), AS_CLIENT_CONTEXT_DATA(job_ptr->context_data));
  }

  return 0;//todo 9/9/24 devops: ambiguous return value, conflicting with return value from callback above
}

/**
 * 	@returns: current available type id slot indexed at 0
 * 	@locked ScheduledJobs *: jobs table must be locked user
 */
static inline size_t
_ExpandJobTypesIfNecessary(ScheduledJobs *jobs_ptr)
{
	if (unlikely(jobs_ptr->job_types_descriptor.job_types_size == 0)) {
		jobs_ptr->job_types_descriptor.job_types_index =
		    calloc(JOB_INDEX_EXPANSION_THRESHOLD, sizeof(ScheduledJobType *));
		return 0;
	}

	size_t next_index = jobs_ptr->job_types_descriptor.job_types_size;

	/* Expand the array every JOB_INDEX_EXPANSION_THRESHOLD registrations
	 * by allocating a new array, copying existing entries, and freeing
	 * the old one. */
	if ((next_index % JOB_INDEX_EXPANSION_THRESHOLD) == 0) {
		size_t new_cap = next_index + JOB_INDEX_EXPANSION_THRESHOLD;
		ScheduledJobType **new_index_ptr = calloc(new_cap, sizeof(ScheduledJobType *));
		for (size_t i = 0; i < next_index; i++) {
			new_index_ptr[i] = jobs_ptr->job_types_descriptor.job_types_index[i];
		}
		free(jobs_ptr->job_types_descriptor.job_types_index);
		jobs_ptr->job_types_descriptor.job_types_index = new_index_ptr;
	}

	return next_index;
}

void
DestructScheduledJobs(ScheduledJobs *jobs_ptr)
{
	if (IS_PRESENT(jobs_ptr)) {
		if (IS_PRESENT(jobs_ptr->job_types_descriptor.job_types_index)) {
			free(jobs_ptr->job_types_descriptor.job_types_index);
		}
		/* Guard against double-destroy: if the store pointer is NULL the heap
		 * was already destroyed (or never initialised). */
		if (IS_PRESENT(jobs_ptr->scheduled_jobs_store)) {
			MinHeapDestroy(jobs_ptr->scheduled_jobs_store);
			pthread_spin_destroy(&jobs_ptr->spin_lock);
		}
	}

	memset(jobs_ptr, 0, sizeof(ScheduledJobs));
}

size_t
GetScheduleJobsSetsize(ScheduledJobs *jobs_ptr, unsigned lock_hints)
{
  if (!(lock_hints & LOCK_HINT_ALREADY_LOCKED)) pthread_spin_lock(&(jobs_ptr->spin_lock));

  size_t setsize = (size_t)MinHeapSize(jobs_ptr->scheduled_jobs_store);

  if (!(lock_hints & LOCK_HINT_KEEP_LOCKED)) pthread_spin_unlock(&(jobs_ptr->spin_lock));

  return setsize;
}

/* ── Introspection (JSON) ──────────────────────────────────────────────── */

static void
sAppendJsonString(BufferDescriptor *bd, const char *s)
{
	BufferDescriptorAppendFormatted(bd, "\"");
	if (s) {
		for (const char *p = s; *p; p++) {
			switch (*p) {
			case '"':  BufferDescriptorAppendFormatted(bd, "\\\""); break;
			case '\\': BufferDescriptorAppendFormatted(bd, "\\\\"); break;
			case '\n': BufferDescriptorAppendFormatted(bd, "\\n");  break;
			case '\t': BufferDescriptorAppendFormatted(bd, "\\t");  break;
			default:   BufferDescriptorAppendFormatted(bd, "%c", *p); break;
			}
		}
	}
	BufferDescriptorAppendFormatted(bd, "\"");
}

struct sJobDescCtx {
	BufferDescriptor *bd;
	int               emitted;
};

static void
sDescribeJob(void *key, void *value, void *ctx)
{
	struct sJobDescCtx *c = ctx;
	int64_t fire_time = (int64_t)(intptr_t)key;
	ScheduledJob *job = (ScheduledJob *)value;
	const char *type_name = (job && job->job_type_ptr) ? job->job_type_ptr->type_name : NULL;

	BufferDescriptorAppendFormatted(c->bd, "%s{\"fire_time\":%" PRId64 ",\"type_name\":",
		c->emitted ? "," : "", fire_time);
	sAppendJsonString(c->bd, type_name ? type_name : "");
	BufferDescriptorAppendFormatted(c->bd, ",\"job\":\"0x%" PRIxPTR "\"}",
		(uintptr_t)job);
	c->emitted = 1;
}

PUBLIC_API BufferDescriptor *
DescribeScheduledJobs(ScheduledJobs *jobs_ptr, BufferDescriptor *provided)
{
	if (!provided) {
		provided = calloc(1, sizeof(BufferDescriptor));
		if (!provided)
			return NULL;
		BufferDescriptorInit(provided, 256);
	}

	if (!jobs_ptr) {
		BufferDescriptorAppendFormatted(provided, "{\"error\":\"null handle\"}\n");
		return provided;
	}

	pthread_spin_lock(&jobs_ptr->spin_lock);

	size_t n_types = jobs_ptr->job_types_descriptor.job_types_size;
	int pending = MinHeapSize(jobs_ptr->scheduled_jobs_store);  /* NULL-safe */

	BufferDescriptorAppendFormatted(provided,
		"{\"job_types_count\":%zu,\"pending_jobs\":%d,\"job_types\":[",
		n_types, pending);

	int emitted = 0;
	for (size_t i = 0; i < n_types; i++) {
		ScheduledJobType *t = jobs_ptr->job_types_descriptor.job_types_index[i];
		if (!t)
			continue;
		BufferDescriptorAppendFormatted(provided, "%s{\"index\":%zu,\"type_name\":",
			emitted ? "," : "", i);
		sAppendJsonString(provided, t->type_name);
		BufferDescriptorAppendFormatted(provided,
			",\"type_id\":%d,\"frequency_mode\":\"%s\",\"concurrency_mode\":\"%s\","
			"\"frequency\":%" PRIu64 "}",
			t->type_id,
			t->frequency_mode == ONEOFF ? "ONEOFF" : "PERIODIC",
			t->concurrency_mode == MULTI_INSTANCE ? "MULTI_INSTANCE" : "SINGLE_INSTANCE",
			(uint64_t)t->frequency);
		emitted = 1;
	}
	BufferDescriptorAppendFormatted(provided, "]");

	/* Pending jobs in heap-array order (not fire-time order). */
	BufferDescriptorAppendFormatted(provided, ",\"jobs\":[");
	struct sJobDescCtx ctx = { provided, 0 };
	MinHeapForeach(jobs_ptr->scheduled_jobs_store, sDescribeJob, &ctx);
	BufferDescriptorAppendFormatted(provided, "]}\n");

	pthread_spin_unlock(&jobs_ptr->spin_lock);
	return provided;
}