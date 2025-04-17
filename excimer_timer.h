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

#ifndef EXCIMER_TIMER_H
#define EXCIMER_TIMER_H

#include "excimer_events.h"
#include "timerlib/timerlib.h"

typedef void (*excimer_timer_callback)(zend_long, void *);

/* Forward declaration */
typedef struct _excimer_timer_tls_t excimer_timer_tls_t;

typedef struct _excimer_timer {
	/** True if the object has been initialised and not destroyed */
	int is_valid;

	/** True if the timer has started */
	int is_running;

	/** &EG(vm_interrupt) in the relevant thread */
#if PHP_VERSION_ID >= 80200
	zend_atomic_bool *vm_interrupt_ptr;
#else
	zend_bool *vm_interrupt_ptr;
#endif

	/** The underlying timerlib timer */
	timerlib_timer_t tl_timer;

	/** The event callback. */
	excimer_timer_callback callback;

	/** The event callback user data */
	void *user_data;

	/** 
	 * The next pending timer, in a circular doubly-linked list of pending
	 * timers, or NULL if the timer is not in the list.
	 */
	struct _excimer_timer *pending_next;
	/** The previous pending timer */
	struct _excimer_timer *pending_prev;

	zend_long event_count;

	/** The thread-local data associated with the thread that created the timer */
	excimer_timer_tls_t *tls;
} excimer_timer;

typedef struct _excimer_timer_globals_t {
	/**
	 * The old value of the zend_interrupt_function hook. If set, this must be
	 * called to allow pcntl_signal() etc. to work.
	 */
	void (*old_zend_interrupt_function)(zend_execute_data *execute_data);
} excimer_timer_globals_t;

typedef struct _excimer_timer_tls_t {
	/** The mutex protecting the pending list */
	pthread_mutex_t mutex;

	/**
	 * The head of the list of pending timers. This is a doubly-linked list
	 * because we need to randomly delete members when timers are destroyed.
	 * It's circular, with the last element pointing back to the first element,
	 * because that makes it a bit easier to check whether an element is in the
	 * list. A circular list means that objects have a non-NULL prev/next if and
	 * only if they are in the list.
	 */
	excimer_timer *pending_head;

	/** The number of active timers in this thread */
	unsigned long timers_active;
} excimer_timer_tls_t;

/**
 * Global initialisation of the timer module
 */
void excimer_timer_module_init();

/**
 * Global shutdown of the timer module
 */
void excimer_timer_module_shutdown();

/**
 * Thread-local initialisation of the timer module. This must be called before
 * any timer objects are created.
 */
void excimer_timer_thread_init();

/**
 * Thread-local shutdown of the timer module. After calling this,
 * excimer_timer_thread_init() may be called again to reinitialise the module.
 */
void excimer_timer_thread_shutdown();

/**
 * Initialise a timer object allocated by the caller
 *
 * @param timer The timer object pointer
 * @param event_type May be EXCIMER_REAL or EXCIMER_CPU
 * @param callback The callback to call during VM interrupt
 * @param user_data An arbitrary pointer passed to the callback
 * @return SUCCESS or FAILURE
 */
int excimer_timer_init(excimer_timer *timer, int event_type,
	excimer_timer_callback callback, void *user_data);

/**
 * Start a timer. If there is no error, timer->is_running will be set to 1.
 *
 * @param timer The timer object
 * @param period The period (it_interval) of the timer
 * @param initial The initial timer value (it_value)
 */
void excimer_timer_start(excimer_timer *timer,
	struct timespec *period, struct timespec *initial);

/**
 * Stop a timer. If there is no error, timer->is_running will be set to 0.
 *
 * @param timer The timer object
 */
void excimer_timer_stop(excimer_timer *timer);

/**
 * Destroy the contents of a timer object
 *
 * @param timer The timer object pointer, memory owned by the caller
 */
void excimer_timer_destroy(excimer_timer *timer);

/**
 * Get remaining time
 *
 * @param timer The timer object
 * @param remaining This struct will be filled with the time remaining
 */
void excimer_timer_get_time(excimer_timer *timer, struct timespec *remaining);

#endif
