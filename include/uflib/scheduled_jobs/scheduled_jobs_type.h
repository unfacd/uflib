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

#ifndef SRC_INCLUDE_SCHEDULED_JOBS_TYPE_H_
#define SRC_INCLUDE_SCHEDULED_JOBS_TYPE_H_

#include <uflib/main_types.h>
#include <pthread.h>
#include <uflib/adt/adt_doubly_linkedlist.h>
#include <uflib/adt/adt_minheap_type.h>

#define LOCK_HINT_NONE            (0x0) //use standard semantics
#define LOCK_HINT_ALREADY_LOCKED  (0x1 << 1)
#define LOCK_HINT_KEEP_LOCKED     (0x1 << 2)

enum ScheduledJobExecutionFrequencyMode {
	PERIODIC = 0,
	ONEOFF
};

enum ScheduledJobExecutionConcurrencyMode {
	SINGLE_INSTANCE	=	0, //only one single instance of the job can exit in the scheduler at any given time
	MULTI_INSTANCE
};
typedef void JobContext;

typedef int (*CallbackOnRun)(JobContext *, ClientContextData *);
typedef int (*CallbackOnError)(ClientContextData *);
typedef int (*CallbackOnCompareKeys)(void *, void *);

#define AS_JOB_CONTEXT(x) ((JobContext *)(x))

typedef struct ScheduledJobType {
	const char 	*type_name;
	int 				type_id;
	enum ScheduledJobExecutionFrequencyMode 	frequency_mode;
	enum ScheduledJobExecutionConcurrencyMode concurrency_mode;
	uint64_t 																	frequency; //relative interval in microseconds. Can be overriden on invocation-basis if ScheduledJob.when_scheduled is set by user

	struct {
		int (*on_compare_keys)(void *, void *); //how to compare time values. Default provided
		int (*on_error)(ClientContextData *);
		int (*on_run)(JobContext *, ClientContextData *); //callback when the job is ready to run
		long long (*on_get_time)(void);//calculate current time value using user's chosen precision
	} callbacks;

} ScheduledJobType;

typedef struct ScheduledJob {
	ScheduledJobType 	*job_type_ptr;
	long long					when_scheduled, //time at which job was inserted into the scheduler
	                  when_to_schedule; //user-provided relative interval (in microseconds) to override ScheduledJobType's specified frequency.
	ClientContextData *context_data;//optional runtime data
} ScheduledJob;

typedef struct ScheduledJobs {
	struct {
		size_t 						job_types_size;//how many jobs types currently recognised
		ScheduledJobType 	**job_types_index;//index array of job types provided by user
	} job_types_descriptor;
	pthread_spinlock_t spin_lock;//coarse lock over store
	heap scheduled_jobs_store;//actual heap-sorted store of jobs
} ScheduledJobs;

//hold the return of stored job from the store
typedef struct ScheduledJobContext {
		long long 		time_key; //time in microseconds when job would/have triggered
		ScheduledJob	*scheduled_job_ptr;//original user-provided ScheduledJob
} ScheduledJobContext;

#endif /* SRC_INCLUDE_SCHEDULED_JOBS_TYPE_H_ */
