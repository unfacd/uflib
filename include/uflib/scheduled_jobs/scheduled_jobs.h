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

#ifndef SRC_INCLUDE_SCHEDULED_JOBS_H_
#define SRC_INCLUDE_SCHEDULED_JOBS_H_

#include <uflib/scheduled_jobs/scheduled_jobs_type.h>
#include <uflib/main_types.h>

ScheduledJobs *GetScheduledJobsStore (void);
void InitScheduledJobsStore (ScheduledJobs *jobs_ptr, size_t count);
int RegisterScheduledJobType (ScheduledJobs *jobs_ptr, ScheduledJobType *job_type_ptr);
void AddScheduledJob (ScheduledJobs *jobs_ptr, ScheduledJob *job_ptr);
ScheduledJobContext *GetScheduledJob (ScheduledJobs *jobs_ptr, unsigned lock_hints, ScheduledJobContext *context_ptr_out);
ScheduledJobContext *GetRemScheduledJob (ScheduledJobs *jobs_ptr, unsigned lock_hints, ScheduledJobContext *context_ptr_out);
void DestructScheduledJob (ScheduledJob *job_ptr);
bool IsJobPeriodic (ScheduledJob *job_ptr);
bool IsJobTypeNameRegistered (ScheduledJobs *jobs_ptr, const char *type_name);
CallbackOnCompareKeys GetDefaultComparatorForTimeValue (void);
int WorkerThreadScheduledJobExecutor (MessageContextData *context_ptr);
int TimeValueComparator (void *key1, void *key2);
void DestructScheduledJobs (ScheduledJobs *jobs_ptr);
size_t GetScheduleJobsSetsize(ScheduledJobs *jobs_ptr, unsigned lock_hints);

#endif /* SRC_INCLUDE_SCHEDULED_JOBS_H_ */
