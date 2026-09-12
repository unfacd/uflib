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

#ifndef UFLIB_UTILS_TIME_H
#define UFLIB_UTILS_TIME_H

#include <uflib/uflib_defs.h>

#include <uflib/standard_c_includes.h>
#include "simple_timer_type.h"
#include <time.h>

#define SECONDS_TO_NANO_SECONDS(x) ((x) *1000000000)
#define SECONDS_TO_MICRO_SECONDS(x) ((x) * 1000000)
#define SECONDS_TO_MILLI_SECONDS(x) ((x) * 1000)
#define MILLI_SECONDS_TO_SECONDS(x) ((x) / 1000) /** @brief param must be in millis */

#define DAY_TO_MILLI_SECONDS(x) ((x) * 24 * 60 * 60 * 1000)

PUBLIC_API void GetTimeNow(long *, long *);
PUBLIC_API long long GetTimeNowInMillis(void);
PUBLIC_API long long GetTimeNowInMicros(void);
PUBLIC_API void AddMillisecondsToNow(long long, long *, long *);
PUBLIC_API void set_time(struct timeval *);

PUBLIC_API bool  SimpleTimerIsExpired(SimpleTimer *t);
PUBLIC_API void SimpleTimerSet(SimpleTimer *t, size_t usecs);

#endif //UFSRV_UTILS_TIME_H
