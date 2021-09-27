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

#include <uflib/standard_c_includes.h>
#include <syslog.h>
#include <uflib/utils_time.h>
#include <uflib/utils_str.h>
#include <sys/time.h>

static const char *s_month[] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul",
        "Aug", "Sep", "Oct", "Nov", "Dec"
};

static const char *s_weekdays[] = {
        "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
};

//if null buf is passed an internal, nonthread safe buffer of length 128 is used
char *
CurrentTime(const time_t t, char *buf, size_t len)
{
  char *p;
  struct tm *tp;
  static char timex[128];
  size_t tlen;

#if defined(HAVE_GMTIME_R)
  struct tm tmr;
	tp = gmtime_r(&t, &tmr);
#else
  tp = gmtime(&t);
#endif

  if (buf == NULL) {
    p = timex;
    tlen = sizeof(timex);
  } else {
    p = buf;
    tlen = len;
  }

  if (tp == NULL) {
    mstrlcpy(p, "", tlen);
    return (p);
  }

  snprintf(p, tlen, "%s %s %d %02u:%02u:%02u %d",
           s_weekdays[tp->tm_wday], s_month[tp->tm_mon],
           tp->tm_mday, tp->tm_hour, tp->tm_min, tp->tm_sec, tp->tm_year + 1900);
  return (p);
}

void
set_time(struct timeval *time_in)
{
  struct timeval newtime;

  if(gettimeofday(&newtime, NULL) == -1)
  {
    syslog(LOG_ERR, "!!!! Failed to get time of the day");
    exit (-1);
  }

  //if(newtime.tv_sec < time_in->tv_sec)
  //set_back_events(time_in->tv_sec - newtime.tv_sec); //change timed events accrodingly

  memcpy(time_in, &newtime, sizeof(struct timeval));
}

void
GetTimeNow (long *seconds, long *milliseconds)
{
  struct timeval tv;

  gettimeofday(&tv, NULL);
  *seconds = tv.tv_sec;
  *milliseconds = tv.tv_usec/1000;

}

//TODO: convert to uint64_t 1000UL
long long
GetTimeNowInMillis (void)
{
  struct timeval tv = {0};

  if ((gettimeofday(&tv, NULL)) == 0)	return ((long long) tv.tv_sec * 1000L + tv.tv_usec / 1000L);

  return time(NULL) * 1000;//crude...

}

long long
GetTimeNowInMicros (void)
{
#ifdef _SC_MONOTONIC_CLOCK
  if (sysconf(_SC_MONOTONIC_CLOCK) > 0) {
		struct timespec ts = {0};

		if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
			return (long long) (ts.tv_sec * 1000000L + ts.tv_nsec / 1000L);
	}
	else
#endif
  {
    struct timeval tv = {0};

    if ((gettimeofday(&tv, NULL)) == 0) return (1000000L * tv.tv_sec + tv.tv_usec);
  }

  return 0;
}

void
AddMillisecondsToNow (long long milliseconds, long *sec, long *ms)
{
  long cur_sec, cur_ms, when_sec, when_ms;

  GetTimeNow (&cur_sec, &cur_ms);
  when_sec = cur_sec + milliseconds/1000;
  when_ms = cur_ms + milliseconds%1000;

  if (when_ms >= 1000)
  {
    when_sec ++;
    when_ms -= 1000;
  }

  *sec = when_sec;
  *ms = when_ms;

}

static size_t _GetClockTime(void);

static size_t
_GetClockTime(void)
{
  return GetTimeNowInMicros();
}

/**
 * @brief This is designed to be polled
 * @param t
 * @return
 */
bool
SimpleTimerIsExpired(SimpleTimer *t)
{
  size_t current_clock_time = _GetClockTime();
  bool timeout_reached = (current_clock_time - t->start) >= t->interval;
  return timeout_reached;
}

/**
 * @brief Seed the timer to fire off after provided interval (in micro seconds) has passed.
 * @param t pre-allocated SimpleTimer
 * @param interval amount of time to wait in micro seconds
 */
void
SimpleTimerSet(SimpleTimer *t, size_t interval)
{
  t->interval = interval;
  t->start = _GetClockTime();
}