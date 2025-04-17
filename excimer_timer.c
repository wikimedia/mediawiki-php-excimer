/* Copyright 2018 Wikimedia Foundation
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

#include <signal.h>
#include <stdint.h>
#include <time.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

#include "php.h"
#include "excimer_mutex.h"
#include "excimer_timer.h"
#include "zend_types.h"

#if PHP_VERSION_ID >= 80200
#define excimer_timer_atomic_bool_store(dest, value) zend_atomic_bool_store(dest, value)
#else
#define excimer_timer_atomic_bool_store(dest, value) *dest = value
#endif

excimer_timer_globals_t excimer_timer_globals;
ZEND_TLS excimer_timer_tls_t excimer_timer_tls;

static void excimer_timer_handle(void * data, int overrun_count);
static void excimer_timer_interrupt(zend_execute_data *execute_data);

/**
 * Add a timer to the pending list. Unsynchronised, i.e. the caller is
 * responsible for locking the mutex if required.
 */
static void excimer_timer_list_enqueue(excimer_timer *timer)
{
	excimer_timer **head_pp = &excimer_timer_tls.pending_head;
	if (!timer->pending_next) {
		if (*head_pp) {
			timer->pending_next = *head_pp;
			timer->pending_prev = (*head_pp)->pending_prev;
			(*head_pp)->pending_prev->pending_next = timer;
			(*head_pp)->pending_prev = timer;
		} else {
			*head_pp = timer;
			timer->pending_next = timer;
			timer->pending_prev = timer;
		}
	}
}

/**
 * Remove the first (FIFO) timer from the pending list and provide a pointer
 * to it. (unsynchronised)
 *
 * @param[out] timer_pp
 * @return True if a timer was returned, false if the list was empty
 */
static int excimer_timer_list_dequeue(excimer_timer **timer_pp)
{
	excimer_timer **head_pp = &excimer_timer_tls.pending_head;
	if (*head_pp) {
		// Get the pending timer
		excimer_timer *timer = *timer_pp = *head_pp;
		if (timer->pending_next == timer) {
			// List is now empty
			*head_pp = NULL;
		} else {
			// Relink the head and neighbours
			timer->pending_next->pending_prev = timer->pending_prev;
			*head_pp = timer->pending_prev->pending_next = timer->pending_next;
		}
		// Unlink the timer being returned
		timer->pending_next = NULL;
		timer->pending_prev = NULL;
		return 1;
	} else {
		return 0;
	}
}

/**
 * Remove the specified timer from the pending list, if it is in there. If it
 * is not in the list, do nothing. (unsynchronised)
 */
static void excimer_timer_list_remove(excimer_timer *timer)
{
	excimer_timer **head_pp = &excimer_timer_tls.pending_head;
	if (timer->pending_next) {
		if (timer->pending_next == timer) {
			*head_pp = NULL;
		} else {
			timer->pending_next->pending_prev = timer->pending_prev;
			timer->pending_prev->pending_next = timer->pending_next;
			if (*head_pp == timer) {
				*head_pp = timer->pending_next;
			}
		}
		timer->pending_next = NULL;
		timer->pending_prev = NULL;
	}
}

/**
 * Atomically dequeue a timer and get its event count at the time of removal
 * from the queue. The timer may be immediately re-added to the queue by the
 * event handler.
 *
 * @param[out] timer_pp Where to put the pointer to the timer
 * @param[out] event_count_p Where to put the event count
 * @return True if a timer was removed, false if the list was empty.
 */
static int excimer_timer_pending_dequeue(excimer_timer **timer_pp, zend_long *event_count_p)
{
	excimer_mutex_lock(&excimer_timer_tls.mutex);
	int ret = excimer_timer_list_dequeue(timer_pp);
	if (ret) {
		*event_count_p = (*timer_pp)->event_count;
		(*timer_pp)->event_count = 0;
	}
	excimer_mutex_unlock(&excimer_timer_tls.mutex);
	return ret;
}

// Note: functions with external linkage are documented in the header

void excimer_timer_module_init()
{
	excimer_timer_globals.old_zend_interrupt_function = zend_interrupt_function;
	zend_interrupt_function = excimer_timer_interrupt;
}

void excimer_timer_module_shutdown()
{
}

void excimer_timer_thread_init()
{
	excimer_timer_tls = (excimer_timer_tls_t){
		.mutex = PTHREAD_MUTEX_INITIALIZER
	};
}

void excimer_timer_thread_shutdown()
{
	if (excimer_timer_tls.timers_active) {
		// If this ever happens, it means we've got the logic wrong and we need
		// to rethink. It's very bad for timers to keep existing after thread
		// termination, because the mutex will be a dangling pointer. It's not
		// much help to avoid excimer_mutex_destroy() here because the whole TLS
		// segment will be destroyed and reused.
		php_error_docref(NULL, E_WARNING, "Timer still active at thread termination");
	} else {
		excimer_mutex_destroy(&excimer_timer_tls.mutex);
	}
}

int excimer_timer_init(excimer_timer *timer, int event_type,
	excimer_timer_callback callback, void *user_data)
{
	zval z_timer;

	memset(timer, 0, sizeof(excimer_timer));
	ZVAL_PTR(&z_timer, timer);
	timer->vm_interrupt_ptr = &EG(vm_interrupt);
	timer->callback = callback;
	timer->user_data = user_data;
	timer->tls = &excimer_timer_tls;

	if (timerlib_timer_init(&timer->tl_timer, event_type, &excimer_timer_handle, timer) == FAILURE) {
		timerlib_timer_destroy(&timer->tl_timer);
		return FAILURE;
	}

	excimer_timer_tls.timers_active++;
	timer->is_valid = 1;
	timer->is_running = 0;
	return SUCCESS;
}

void excimer_timer_start(excimer_timer *timer,
	struct timespec *period, struct timespec *initial)
{
	if (!timer->is_valid) {
		php_error_docref(NULL, E_WARNING, "Unable to start uninitialised timer" );
		return;
	}

	/* If a periodic timer has an initial value of 0, use the period instead,
	 * since it_value=0 means disarmed */
	if (timerlib_timespec_is_zero(initial)) {
		initial = period;
	}
	/* If the value is still zero, flag an error */
	if (timerlib_timespec_is_zero(initial)) {
		php_error_docref(NULL, E_WARNING, "Unable to start timer with a value of zero "
			"duration and period");
		return;
	}

	if (timerlib_timer_start(&timer->tl_timer, period, initial) == SUCCESS) {
		timer->is_running = 1;
	}
}

void excimer_timer_stop(excimer_timer *timer)
{
	if (!timer->is_valid) {
		php_error_docref(NULL, E_WARNING, "Unable to start uninitialised timer" );
		return;
	}
	if (timer->is_running) {
		if (timerlib_timer_stop(&timer->tl_timer) == SUCCESS) {
			timer->is_running = 0;
		}
	}
}

void excimer_timer_destroy(excimer_timer *timer)
{
	if (!timer->is_valid) {
		/* This could happen if the timer is manually destroyed after
		 * excimer_timer_thread_shutdown() is called */
		return;
	}
	if (timer->tls != &excimer_timer_tls) {
		php_error_docref(NULL, E_WARNING,
			"Cannot delete a timer belonging to a different thread");
		return;
	}

	/* Stop the timer */
	if (timer->is_running) {
		timer->is_running = 0;
		timerlib_timer_stop(&timer->tl_timer);
	}
	/* Destroy the timer. This will wait until any events are done. */
	timerlib_timer_destroy(&timer->tl_timer);
	excimer_timer_tls.timers_active--;

	/* Remove the timer from the pending list */
	excimer_mutex_lock(&excimer_timer_tls.mutex);
	excimer_timer_list_remove(timer);
	excimer_mutex_unlock(&excimer_timer_tls.mutex);

	timer->is_valid = 0;
	timer->tls = NULL;
}

static void excimer_timer_handle(void * data, int overrun_count)
{
	excimer_timer *timer = (excimer_timer*)data;
	excimer_mutex_lock(&excimer_timer_tls.mutex);
	timer->event_count += overrun_count + 1;
	excimer_timer_list_enqueue(timer);
	excimer_mutex_unlock(&excimer_timer_tls.mutex);
	excimer_timer_atomic_bool_store(timer->vm_interrupt_ptr, 1);
}

static void excimer_timer_interrupt(zend_execute_data *execute_data)
{
	excimer_timer *timer = NULL;
	zend_long count = 0;
	while (excimer_timer_pending_dequeue(&timer, &count)) {
		timer->callback(count, timer->user_data);
	}

	if (excimer_timer_globals.old_zend_interrupt_function) {
		excimer_timer_globals.old_zend_interrupt_function(execute_data);
	}
}

void excimer_timer_get_time(excimer_timer *timer, struct timespec *remaining)
{
	if (!timer->is_valid || !timer->is_running) {
		remaining->tv_sec = 0;
		remaining->tv_nsec = 0;
		return;
	}

	timerlib_timer_get_time(&timer->tl_timer, remaining);
}
