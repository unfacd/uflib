/**
 * Copyright (C) 2015-2022 unfacd works
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

#ifndef UFLIB_UTILS_THREADS_H
#define UFLIB_UTILS_THREADS_H

#include <uflib/uflib_defs.h>

#include <uflib/standard_defs.h>
#include <uflib/standard_c_includes.h>

PUBLIC_API int ThreadSleep(long milliseconds);
PUBLIC_API int ThreadSleepMs(long milliseconds);
PUBLIC_API int SetThreadName(const char *thread_name);
char *thread_error(int error);
char *thread_error_wrlock(int error);

/**
 * @short MACROS FOR CALLING A FUNCTION ONCE AT MOST EVERY X SECONDS IN EACH THREAD.
 * @{
 *
 * This is especially useful for preventing log-spamming, and possible DoS attacks
 * that can happen as a consequence of threads having to write out a massive log.
 * To use them you MUST include <time.h>.
 */
/**
 * @short Call a function once at most every X seconds in each thread - don't count or pass the calls that happened in between.
 * @ingroup low
 */
#define ONION_CALL_MAX_ONCE_PER_T(seconds, func, ...) do { \
  static _Thread_local time_t last_func_call = 0; \
  if (difftime(time(0), last_func_call) >= seconds) { \
    func(__VA_ARGS__); \
    time(&last_func_call); \
  } \
} while (0)
/**
 * @short Call a function once at most every X seconds in each thread and pass the number of ignored calls + 1 as the last argument (an unsigned int).
 * @ingroup low
 */
#define ONION_CALL_MAX_ONCE_PER_T_COUNT(seconds, func, ...) do { \
  static _Thread_local time_t last_func_call = 0; \
  static _Thread_local unsigned int func_calls_since = 0; \
  if (difftime(time(0), last_func_call) >= seconds) { \
    func(__VA_ARGS__, func_calls_since + 1); \
    func_calls_since = 0; \
    time(&last_func_call); \
  } \
  else \
    ++func_calls_since; \
} while (0)
/// @}

#endif //UFSRV_UTILS_THREADS_H