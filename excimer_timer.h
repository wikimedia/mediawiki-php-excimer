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

enum {
	/** Event type: real, wall-clock time */
	EXCIMER_REAL,
	/** Event type: CPU time */
	EXCIMER_CPU
};

typedef void (*excimer_timer_callback)(zend_long, void *);

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

	/** A unique ID identifying this object. These IDs are never reused, so that
	 * the ID can be used to identify events received for deleted objects. The
	 * type is intptr_t because it is 64 bits on a 64-bit platform, making
	 * an overflow less likely. Using a signed type means it can be converted
	 * to zend_long without changing the interpretation. We can't use int64_t
	 * or zend_long directly because the maximum width is sizeof(union sigval),
	 * which is too small on a 32-bit platform.
	 */
	intptr_t id;

	/** The clock ID passed to timer_create() etc. */
	clockid_t clock_id;

	/** The timer ID returned by timer_create() */
	timer_t timer_id;

	/** The event callback. */
	excimer_timer_callback callback;

	/** The event callback user data */
	void *user_data;

	/** A pointer to excimer_timer_tls.event_counts */
	HashTable ** event_counts_ptr;

	/** A pointer to excimer_timer_tls.mutex */
	pthread_mutex_t *thread_mutex_ptr;
} excimer_timer;

typedef struct _excimer_timer_globals_t {
	/**
	 * A hashtable mapping unique ID (excimer_timer.id) to the excimer_timer
	 * pointer. Use Z_PTR() to extract the pointer.
	 */
	HashTable *timers_by_id;

	/**
	 * The mutex protecting timers_by_id and next_id from concurrent modification.
	 */
	pthread_mutex_t mutex;

	/**
	 * The next ID to be used for excimer_timer.id
	 */
	intptr_t next_id;

	/**
	 * The old value of the zend_interrupt_function hook. If set, this must be
	 * called to allow pcntl_signal() etc. to work.
	 */
	void (*old_zend_interrupt_function)(zend_execute_data *execute_data);
} excimer_timer_globals_t;

typedef struct _excimer_timer_tls_t {
	/** A map of ID => event_count, protected by a mutex */
	HashTable *event_counts;

	/** The mutex protecting event_counts */
	pthread_mutex_t mutex;

	/** A map of ID => *timer, which is not protected, it is only accessed by
	 * the same thread */
	HashTable *timers_by_id;
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
