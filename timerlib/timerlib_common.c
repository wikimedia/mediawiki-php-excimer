#include "timerlib.h"
#include <stdio.h>

void timerlib_timespec_add(timerlib_timespec_t * a, const timerlib_timespec_t * b)
{
	a->tv_sec += b->tv_sec;
	a->tv_nsec += b->tv_nsec;
	if (a->tv_nsec > TIMERLIB_BILLION_L) {
		a->tv_nsec -= TIMERLIB_BILLION_L;
		a->tv_sec++;
	}
}

void timerlib_timespec_subtract(timerlib_timespec_t * a, const timerlib_timespec_t * b)
{
	a->tv_sec -= b->tv_sec;
	if (a->tv_nsec < b->tv_nsec) {
		a->tv_sec--;
		a->tv_nsec += TIMERLIB_BILLION_L - b->tv_nsec;
	} else {
		a->tv_nsec -= b->tv_nsec;
	}
}

void timerlib_timespec_from_double(timerlib_timespec_t * dest, double source)
{
	double fractional, integral;
	if (source < 0) {
		dest->tv_sec = dest->tv_nsec = 0;
		return;
	}

	fractional = modf(source, &integral);
	dest->tv_sec = (time_t)integral;
	dest->tv_nsec = (long)(fractional * 1e9);
	if (dest->tv_nsec >= TIMERLIB_BILLION_L) {
		dest->tv_nsec -= TIMERLIB_BILLION_L;
		dest->tv_sec ++;
	}
}

int timerlib_timer_start_oneshot(timerlib_timer_t *timer, timerlib_timespec_t *duration)
{
	timerlib_timespec_t period = {0};
	return timerlib_timer_start(timer, &period, duration);
}

int timerlib_timer_start_periodic(timerlib_timer_t *timer, timerlib_timespec_t *period)
{
	return timerlib_timer_start(timer, period, period);
}
