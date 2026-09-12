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

#include <sys/stat.h>
#include <time.h>
#include <pthread.h>
#include <linux/prctl.h>
#include <sys/prctl.h>
#include <uflib/utils_threads.h>

/**
 * @brief Cause the current thread to "sleep" for the specified time in seconds. The "sleep mechanism is based on
 * condition blocking.
 * @param[in] seconds amount of seconds to sleep for
 * @return 0 upon successful sleep
 */
int ThreadSleep(long seconds) {
  pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
  pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
  int rval = 0;

  if (seconds > 0) {
    struct timespec time = {0};
    clock_gettime(CLOCK_REALTIME, &time);
    time.tv_sec += seconds;

    rval = pthread_mutex_lock(&mutex);
    if (rval != 0) {
      char *err_str = thread_error(errno);
      syslog(LOG_DEBUG, "%s (pid:'%lu'): ERROR: COULD NOT ACQUIRE LOCK (errno='%d'): '%s'", __func__, pthread_self(), errno, err_str);
    } else {
      //handle spurious wakepups
      do {
        rval = pthread_cond_timedwait(&cond, &mutex, &time);  // expects ETIMEDOUT
      } while (rval == 0);
      if (rval != ETIMEDOUT) {
        syslog(LOG_DEBUG, "%s (pid:'%lu'): ERROR: DID NOT RECEIVE TIMEOUT (return value='%d' desc:'%s')", __func__, pthread_self(), rval, strerror(rval));
      }
      rval = pthread_mutex_unlock(&mutex);
      //handle any error from unlock...
    }
  }

  return rval;
}

/**
 * @warning pthread_cond_timedwait doesn't seem to work well with millisecond resolution
 */
int ThreadSleepMs(long milliseconds) {
  pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
  pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
  int rval = 0;

  if (milliseconds > 0) {
    struct timespec time = {0};
    timespec_get(&time, TIME_UTC);//TODO if (rval != TIME_UTC)

//    char buff[100] ={0};
//    strftime(buff, sizeof buff, "%D %T", gmtime(&time.tv_sec));
//    syslog(LOG_DEBUG, "Current time: %s.%09ld UTC", buff, time.tv_nsec);
//    syslog(LOG_DEBUG, "Raw timespec.time_t: %jd. Raw timespec.tv_nsec: %09ld", (intmax_t)time.tv_sec, time.tv_nsec);

    time.tv_sec += milliseconds / 1000;
    time.tv_nsec += (milliseconds % 1000) * 1000000;

//    memset(buff, '\0', sizeof buff);
//    strftime(buff, sizeof buff, "%D %T", gmtime(&time.tv_sec));
//    syslog(LOG_DEBUG, "AFTER Current time: %s.%09ld UTC\n", buff, time.tv_nsec);
//    syslog(LOG_DEBUG, "AFTER Raw timespec.time_t: %jd. Raw timespec.tv_nsec: %09ld", (intmax_t)time.tv_sec, time.tv_nsec);

    rval = pthread_mutex_lock(&mutex);
    if (rval != 0) {
      char *err_str = thread_error(errno);
      syslog(LOG_DEBUG, "%s (pid:'%lu'): ERROR: COULD NOT ACQUIRE LOCK (errno='%d'): '%s'", __func__, pthread_self(), errno, err_str);
    } else {
      //handle spurious wakepups
      do {
        rval = pthread_cond_timedwait(&cond, &mutex, &time);  // expects ETIMEDOUT
      } while (rval == 0);
      if (rval != ETIMEDOUT) {
        syslog(LOG_DEBUG, "%s (pid:'%lu'): ERROR: DID NOT RECEIVE TIMEOUT (return value='%d' desc:'%s')", __func__, pthread_self(), rval, strerror(rval));
      }
      rval = pthread_mutex_unlock(&mutex);
      //handle any error from unlock...
    }
  }

  return rval;
}

/**
 * @brief Set the thread name for the current calling thread. This gives the thread a descriptive name when looked up
 * in system listing and gdb. There is currently max size lime of 15-bytes.
 * @param thread_name desired name of thread
 * @return 0 on success as per prctl() system call.
 */
int  __attribute__((nonnull(1), access(read_only, 1)))
SetThreadName(const char *thread_name)
{
#define MAX_NAME_LEN 15
  char proc_name [MAX_NAME_LEN + 1] = {0};	/* Name must be <= 15 characters + a null */

  strncpy(proc_name, thread_name, MAX_NAME_LEN);
  int result = prctl(PR_SET_NAME, (unsigned long)&proc_name);

  return result;
#undef MAX_NAME_LEN
}

char *
thread_error(int error)
{
  switch (error) {
    case EAGAIN:
      return("'EAGAIN': ' maximum number of recursive locks for mutex has been exceeded.");

    case EINVAL:
      return("'EINVAL	'the mutex was created with the protocol attribute having the value PTHREAD_PRIO_PROTECT and the calling thread's priority is higher than the mutex's current priority ceiling.'");

    case ENOTRECOVERABLE:
      return("'ENOTRECOVERABLE' The state protected by the mutex is not recoverable.'");

    case EOWNERDEAD:
      return("'EOWNERDEAD' 'The mutex is a robust mutex and the process containing the previous owning thread terminated while holding the mutex lock. The mutex lock shall be acquired by the calling thread and it is up to the new owner to make the state consistent'");

   case EDEADLK:
      return("'EDEADLK' 'The mutex type is PTHREAD_MUTEX_ERRORCHECK and the current thread already owns the mutex. A deadlock condition was detected.'");

    case EBUSY:
      return("'EBUSY' 'The mutex could not be acquired because it was already locked.'");

    case EPERM:
      return("'EPERM' 'The mutex type is PTHREAD_MUTEX_ERRORCHECK or PTHREAD_MUTEX_RECURSIVE, or the mutex is a robust mutex, and the current thread does not own the mutex.'");

    default:
     return("'DEFAULT' 'UNKNOWN ERROR CODE");
  }
}

char *
thread_error_wrlock(int error)
{
  switch (error) {
    case EINVAL:
      return("'EINVAL	'The value specified by rwlock does not refer to an initialized read-write lock object.'");

    case EDEADLK:
      return("'EDEADLK' 'he current thread already owns the read-write lock for writing or reading.'");

    case EBUSY:
      return("'EBUSY' 'Mutex already locked for reading or writing.'");

    case EPERM:
      return("'EBUSY' 'The current thread does not hold a lock on the read-write lock.'");

    default:
      return("'DEFAULT' 'UNKNOWN ERROR CODE");
  }
}