/* Copyright 2024 Wikimedia Foundation
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

#ifndef EXCIMER_OS_TIMER_H
#define EXCIMER_OS_TIMER_H

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#ifdef HAVE_TIMER_CREATE
#include "excimer_os_timer_posix.h"
#else
#include "excimer_os_timer_kqueue.h"
#endif

/**
 * Initialize a new timer.
 * @param event_type May be EXCIMER_REAL or EXCIMER_CPU
 * @param timer_id ID of the Excimer timer that owns this timer
 * @param os_timer Pointer to the timer object to be populated
 * @return SUCCESS if the timer was successfully initialized, FAILURE otherwise
 */
int excimer_os_timer_create(int event_type, intptr_t timer_id, excimer_os_timer_t* os_timer, excimer_os_timer_notify_function_t* notify_function);

/**
 * Start a timer.
 * @param os_timer Pointer to the timer to be started
 * @param period The interval at which the timer should fire
 * @param initial The initial delay of the timer
 * @return SUCCESS if the timer was successfully started, FAILURE otherwise
 */
int excimer_os_timer_start(excimer_os_timer_t* os_timer, struct timespec *period, struct timespec *initial);

/**
 * Stop a timer.
 * @param os_timer Pointer to the timer to be stopped.
 * @return SUCCESS if the timer was successfully stopped, FAILURE otherwise
 */
int excimer_os_timer_stop(excimer_os_timer_t* os_timer);

/**
 * Clean up resources associated with a timer.
 * @param os_timer Pointer to the timer to be cleaned up. 
 */
int excimer_os_timer_delete(excimer_os_timer_t* os_timer);

/**
 * Get the overrun count of a timer.
 * @param os_timer Pointer to the timer whose overrun count should be fetched
 * @return The number of times the timer has fired more than once in a single period
 */
zend_long excimer_os_timer_get_overrun_count(excimer_os_timer_t* os_timer);

/**
 * Get the remaining time until the next scheduled expiratioh of a timer.
 * This is an estimate based on the last reported firing time of the timer and the configured period.
 *
 * @param os_timer Pointer to the timer whose remaining time should be fetched
 * @param remaining Pointer to the timespec struct to be populated with the remaining time
 */
void excimer_os_timer_get_time(excimer_os_timer_t *timer, struct timespec *remaining);

#endif
