/* Copyright 2025 Wikimedia Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#ifndef TIMERLIB_TIMERLIB_H
#define TIMERLIB_TIMERLIB_H

#include "timerlib_config.h"

/**
 * The function pointer type used for notifying the caller of timer events
 */
typedef void (timerlib_notify_function_t)(void *data, int overrun_count);

#if defined(TIMERLIB_USE_POSIX)
#include "timerlib_posix.h"
#elif defined(TIMERLIB_USE_KQUEUE)
#include "timerlib_kqueue.h"
#else
#error "No timer implementation available"
#endif

#define timerlib_abort(libfunc, error_number) timerlib_abort_func(__func__, (libfunc), (error_number))

//--------------------------------------------------------------------------------
// Timer functions
//--------------------------------------------------------------------------------

/**
 * Initialize a new timer.
 *
 * Regardless of the return value, use timerlib_timer_destroy() to destroy
 * the structure.
 *
 * @param[out] timer Pointer to the timer object to be populated
 * @param clock May be TIMERLIB_REAL for wall-clock time, or TIMERLIB_CPU for CPU time
 * @param notify_function Function to be called when the timer expires
 * @param notify_data The first parameter sent to notify_function
 * @return TIMERLIB_SUCCESS if the timer was successfully initialized, TIMERLIB_FAILURE otherwise
 */
int timerlib_timer_init(timerlib_timer_t *timer, int clock,
		timerlib_notify_function_t *notify_function, void *notify_data);

/**
 * Start a one-shot timer
 *
 * @param[in,out] timer
 * @param[in] duration How long before the timer expires
 * @return TIMERLIB_SUCCESS if the timer was successfully started, TIMERLIB_FAILURE otherwise
 */
int timerlib_timer_start_oneshot(timerlib_timer_t *timer, timerlib_timespec_t *duration);

/**
 * Start a periodic timer
 *
 * @param[in,out] timer
 * @param[in] period The interval at which the timer should fire
 * @return TIMERLIB_SUCCESS if the timer was successfully started, TIMERLIB_FAILURE otherwise
 */
int timerlib_timer_start_periodic(timerlib_timer_t *timer, timerlib_timespec_t *period);

/**
 * Start a generic timer
 *
 * @param[in,out] timer
 * @param[in] period The period at which the timer should fire, or zero for a one-shot timer
 * @param[in] initial The initial delay of the timer
 * @return TIMERLIB_SUCCESS if the timer was successfully started, TIMERLIB_FAILURE otherwise
 */
int timerlib_timer_start(timerlib_timer_t *timer, timerlib_timespec_t *period, timerlib_timespec_t *initial);

/**
 * Stop a timer.
 *
 * @param[in,out] timer
 * @return TIMERLIB_SUCCESS if the timer was successfully stopped, TIMERLIB_FAILURE otherwise
 */
int timerlib_timer_stop(timerlib_timer_t *timer);

/**
 * Clean up resources associated with a timer.
 *
 * If a timer callback is executing, wait until it returns.
 *
 * It is guaranteed that the callback will not be called again after this
 * function returns.
 *
 * @param[in,out] timer
 */
void timerlib_timer_destroy(timerlib_timer_t *timer);

/**
 * Get the remaining time until the next scheduled expiratioh of a timer.
 * This is an estimate based on the last reported firing time of the timer and the configured period.
 *
 * @param[in] timer
 * @param[out] remaining Pointer to the timespec struct to be populated with the remaining time
 * @return TIMERLIB_SUCCESS or TIMERLIB_FAILURE
 */
int timerlib_timer_get_time(timerlib_timer_t *timer, timerlib_timespec_t *remaining);

//--------------------------------------------------------------------------------
// Clock functions
//--------------------------------------------------------------------------------

/**
 * Get the current time relative to some implementation-dependent epoch
 * @param clock Either TIMERLIB_REAL or TIMERLIB_CPU
 * @param[out] time Pointer to the timespec to be populated with the current time
 * @return TIMERLIB_SUCCESS or TIMERLIB_FAILURE
 */
int timerlib_clock_get_time(int clock, timerlib_timespec_t * time);

//--------------------------------------------------------------------------------
// Timespec functions
//--------------------------------------------------------------------------------

/**
 * A long billion
 */
#define TIMERLIB_BILLION_L 1000000000L

/**
 * A long long billion
 */
#define TIMERLIB_BILLION_LL 1000000000LL

/**
 * Determine if a timespec is zero
 * @param[in] ts
 * @return 1 if the seconds and nanoseconds parts are both zero, 0 otherwise
 */
static inline int timerlib_timespec_is_zero(timerlib_timespec_t *ts)
{
	return ts->tv_sec == 0 && ts->tv_nsec == 0;
}

/**
 * Convert a timespec to a number of nanoseconds
 *
 * Overflow will silently wrap around after 585 years.
 *
 * @param[in] ts
 * @return The number of nanoseconds
 */
static inline uint64_t timerlib_timespec_to_ns(timerlib_timespec_t *ts)
{
	return (uint64_t)ts->tv_nsec + (uint64_t)ts->tv_sec * TIMERLIB_BILLION_LL;
}

/**
 * Convert a timespec to a floating-point number of seconds
 *
 * Some precision will be lost if the timespec is larger than about 104 days.
 *
 * @param[in] ts
 * @return The number of seconds
 */
static inline double timerlib_timespec_to_double(timerlib_timespec_t *ts)
{
	return timerlib_timespec_to_ns(ts) * 1e-9;
}

/**
 * Add two timespecs like a += b
 *
 * @param[in,out] a The destination and left operand
 * @param[in] b The right operand
 */
void timerlib_timespec_add(timerlib_timespec_t * a, const timerlib_timespec_t * b);

/**
 * Subtract timespecs like a -= b
 *
 * @param[in,out] a The destination and left operand
 * @param[in] b The right operand
 */
void timerlib_timespec_subtract(timerlib_timespec_t * a, const timerlib_timespec_t * b);

/**
 * Populate a timespec from a floating-point number of seconds
 *
 * @param[out] dest
 * @param[in] source
 */
void timerlib_timespec_from_double(timerlib_timespec_t * dest, double source);

#endif
