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

#ifndef UFSRV_UTILS_TIME_H
#define UFSRV_UTILS_TIME_H

#include "standard_c_includes.h"
#include "simple_timer_type.h"
#include <time.h>

#define SECONDS_TO_NANO_SECONDS(x) (x *1000000000)
#define SECONDS_TO_MICRO_SECONDS(x) (x * 1000000)
#define SECONDS_TO_MILLI_SECONDS(x) (x * 1000)

void GetTimeNow (long *, long *);
long long GetTimeNowInMillis (void);
long long GetTimeNowInMicros (void);
void AddMillisecondsToNow (long long, long *, long *);
void set_time(struct timeval *);

bool  SimpleTimerIsExpired(SimpleTimer *t);
void SimpleTimerSet(SimpleTimer *t, size_t usecs);

#endif //UFSRV_UTILS_TIME_H
