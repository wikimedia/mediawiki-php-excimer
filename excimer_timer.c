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

#include <time.h>
#include <signal.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

#include "php.h"
#include "excimer_timer.h"

#if PHP_VERSION_ID >= 80200
#define excimer_timer_atomic_bool_store(dest, value) zend_atomic_bool_store(dest, value)
#else
#define excimer_timer_atomic_bool_store(dest, value) *dest = value
#endif

excimer_timer_globals_t excimer_timer_globals;
ZEND_TLS excimer_timer_tls_t excimer_timer_tls;

static void excimer_timer_handle(union sigval sv);
static void excimer_timer_interrupt(zend_execute_data *execute_data);

static inline int excimer_timer_is_zero(struct timespec *ts)
{
	return ts->tv_sec == 0 && ts->tv_nsec == 0;
}

static void excimer_mutex_init(pthread_mutex_t *mutex)
{
	int result = pthread_mutex_init(mutex, NULL);
	if (result != 0) {
		zend_error_noreturn(E_ERROR, "pthread_mutex_init(): %s", strerror(result));
	}
}

static void excimer_mutex_lock(pthread_mutex_t *mutex)
{
	int result = pthread_mutex_lock(mutex);
	if (result != 0) {
		fprintf(stderr, "pthread_mutex_lock(): %s", strerror(result));
		abort();
	}
}

static void excimer_mutex_unlock(pthread_mutex_t *mutex)
{
	int result = pthread_mutex_unlock(mutex);
	if (result != 0) {
		fprintf(stderr, "pthread_mutex_unlock(): %s", strerror(result));
		abort();
	}
}

static void excimer_mutex_destroy(pthread_mutex_t *mutex)
{
	int result = pthread_mutex_destroy(mutex);
	if (result != 0) {
		zend_error_noreturn(E_ERROR, "pthread_mutex_destroy(): %s", strerror(result));
	}
}

void excimer_timer_module_init()
{
	excimer_timer_globals.timers_by_id = malloc(sizeof(HashTable));
	zend_hash_init(excimer_timer_globals.timers_by_id, 0, NULL, NULL, 1);
	excimer_timer_globals.next_id = 1;
	excimer_mutex_init(&excimer_timer_globals.mutex);

	excimer_timer_globals.old_zend_interrupt_function = zend_interrupt_function;
	zend_interrupt_function = excimer_timer_interrupt;
}

void excimer_timer_module_shutdown()
{
	/* Wait for handler to finish, hopefully no more events are queued */
	excimer_mutex_lock(&excimer_timer_globals.mutex);
	zend_hash_destroy(excimer_timer_globals.timers_by_id);
	free(excimer_timer_globals.timers_by_id);

	/* "Attempting to destroy a locked mutex results in undefined behaviour" */
	excimer_mutex_unlock(&excimer_timer_globals.mutex);
	excimer_mutex_destroy(&excimer_timer_globals.mutex);

}

void excimer_timer_thread_init()
{
	excimer_timer_tls.event_counts = malloc(sizeof(HashTable));
	zend_hash_init(excimer_timer_tls.event_counts, 0, NULL, NULL, 1);

	excimer_mutex_init(&excimer_timer_tls.mutex);

	excimer_timer_tls.timers_by_id = malloc(sizeof(HashTable));
	zend_hash_init(excimer_timer_tls.timers_by_id, 0, NULL, NULL, 1);
}

void excimer_timer_thread_shutdown()
{
	zval *zp_thread;

	/* Destroy any timers still active in this thread */
	ZEND_HASH_FOREACH_VAL(excimer_timer_tls.timers_by_id, zp_thread) {
		excimer_timer *timer = (excimer_timer*)Z_PTR_P(zp_thread);
		excimer_timer_destroy(timer);
	}
	ZEND_HASH_FOREACH_END();

	zend_hash_destroy(excimer_timer_tls.timers_by_id);
	free(excimer_timer_tls.timers_by_id);
	excimer_timer_tls.timers_by_id = NULL;

	/* Acquire the thread so that we can write to event_counts.
	 * This will wait for the handler to finish. */
	excimer_mutex_lock(&excimer_timer_tls.mutex);
	zend_hash_destroy(excimer_timer_tls.event_counts);
	free(excimer_timer_tls.event_counts);
	excimer_timer_tls.event_counts = NULL;
	excimer_mutex_unlock(&excimer_timer_tls.mutex);

	excimer_mutex_destroy(&excimer_timer_tls.mutex);
}

int excimer_timer_init(excimer_timer *timer, int event_type,
	excimer_timer_callback callback, void *user_data)
{
	zval z_timer;
	struct sigevent ev;

	memset(timer, 0, sizeof(excimer_timer));
	ZVAL_PTR(&z_timer, timer);
	timer->vm_interrupt_ptr = &EG(vm_interrupt);
	timer->callback = callback;
	timer->user_data = user_data;
	timer->event_counts_ptr = &excimer_timer_tls.event_counts;
	timer->thread_mutex_ptr = &excimer_timer_tls.mutex;

	excimer_mutex_lock(&excimer_timer_globals.mutex);
	timer->id = excimer_timer_globals.next_id++;
	if (timer->id == 0) {
		excimer_mutex_unlock(&excimer_timer_globals.mutex);
		php_error_docref(NULL, E_WARNING, "Timer ID counter has overflowed");
		return FAILURE;
	}

	zend_hash_index_add(excimer_timer_globals.timers_by_id, timer->id, &z_timer);
	excimer_mutex_unlock(&excimer_timer_globals.mutex);

	zend_hash_index_add(excimer_timer_tls.timers_by_id, timer->id, &z_timer);

	memset(&ev, 0, sizeof(ev));
	ev.sigev_notify = SIGEV_THREAD;
	ev.sigev_notify_function = excimer_timer_handle;
	ev.sigev_value.sival_ptr = (void*)timer->id;

	if (event_type == EXCIMER_CPU) {
		if (pthread_getcpuclockid(pthread_self(), &timer->clock_id) != 0) {
			php_error_docref(NULL, E_WARNING, "Unable to get thread clock ID: %s",
				strerror(errno));
			return FAILURE;
		}
	} else {
		timer->clock_id = CLOCK_MONOTONIC;
	}

	if (timer_create(timer->clock_id, &ev, &timer->timer_id) != 0) {
		php_error_docref(NULL, E_WARNING, "Unable to create timer: %s",
			strerror(errno));
		return FAILURE;
	}

	timer->is_valid = 1;
	timer->is_running = 0;
	return SUCCESS;
}

void excimer_timer_start(excimer_timer *timer,
	struct timespec *period, struct timespec *initial)
{
	struct itimerspec its;
	its.it_interval = *period;
	its.it_value = *initial;

	if (!timer->is_valid) {
		php_error_docref(NULL, E_WARNING, "Unable to start uninitialised timer" );
		return;
	}

	/* If a periodic timer has an initial value of 0, use the period instead,
	 * since it_value=0 means disarmed */
	if (excimer_timer_is_zero(initial)) {
		its.it_value = *period;
	}
	/* If the value is still zero, flag an error */
	if (excimer_timer_is_zero(&its.it_value)) {
		php_error_docref(NULL, E_WARNING, "Unable to start timer with a value of zero "
			"duration and period");
		return;
	}

	if (timer_settime(timer->timer_id, 0, &its, NULL) == 0) {
		timer->is_running = 1;
	} else {
		php_error_docref(NULL, E_WARNING, "timer_settime(): %s", strerror(errno));
	}
}

void excimer_timer_destroy(excimer_timer *timer)
{
	struct timespec zero = {0, 0};
	struct itimerspec its;

	if (!timer->is_valid) {
		/* This could happen if the timer is manually destroyed after
		 * excimer_timer_thread_shutdown() is called */
		return;
	}
	if (timer->event_counts_ptr != &excimer_timer_tls.event_counts) {
		php_error_docref(NULL, E_WARNING,
			"Cannot delete a timer belonging to a different thread");
		return;
	}

	/* Stop the timer. This does not take effect immediately. */
	if (timer->is_running) {
		timer->is_running = 0;

		its.it_value = zero;
		its.it_interval = zero;
		if (timer_settime(timer->timer_id, 0, &its, NULL) != 0) {
			php_error_docref(NULL, E_WARNING,
				"timer_settime(): %s", strerror(errno));
		}
	}

	/* Wait for the handler to finish if it is running */
	excimer_mutex_lock(&excimer_timer_globals.mutex);
	/* Remove the ID from the global hashtable */
	zend_hash_index_del(excimer_timer_globals.timers_by_id, timer->id);
	excimer_mutex_unlock(&excimer_timer_globals.mutex);

	timer->is_valid = 0;
	timer->event_counts_ptr = NULL;

	/* Get the thread-local mutex */
	excimer_mutex_lock(&excimer_timer_tls.mutex);
	/* Remove the timer from the thread-local tables */
	zend_hash_index_del(excimer_timer_tls.event_counts, timer->id);
	zend_hash_index_del(excimer_timer_tls.timers_by_id, timer->id);
	excimer_mutex_unlock(&excimer_timer_tls.mutex);

	if (timer_delete(timer->timer_id) != SUCCESS) {
		php_error_docref(NULL, E_WARNING, "timer_delete(): %s",
			strerror(errno));
	}
}

static void excimer_timer_handle(union sigval sv)
{
	excimer_timer *timer;
	zval *zp_event_count;
	zend_long event_count;
	intptr_t id = (intptr_t)sv.sival_ptr;

	/* Acquire the global mutex, which protects timers_by_id */
	excimer_mutex_lock(&excimer_timer_globals.mutex);
	timer = (excimer_timer*)zend_hash_index_find_ptr(excimer_timer_globals.timers_by_id, id);

	if (!timer || !timer->is_running) {
		/* Timer has been deleted, ignore event */
		excimer_mutex_unlock(&excimer_timer_globals.mutex);
		return;
	}

	/* Acquire the thread-specific mutex */
	excimer_mutex_lock(timer->thread_mutex_ptr);

	/* Add the event count to the thread-local hashtable */
	event_count = timer_getoverrun(timer->timer_id) + 1;
	zp_event_count = zend_hash_index_find(*timer->event_counts_ptr, id);
	if (!zp_event_count) {
		zval tmp;
		ZVAL_LONG(&tmp, event_count);
		zend_hash_index_add_new(*timer->event_counts_ptr, id, &tmp);
	} else {
		Z_LVAL_P(zp_event_count) += event_count;
	}
	excimer_timer_atomic_bool_store(timer->vm_interrupt_ptr, 1);
	/* Release the mutexes */
	excimer_mutex_unlock(timer->thread_mutex_ptr);
	excimer_mutex_unlock(&excimer_timer_globals.mutex);
}

static void excimer_timer_interrupt(zend_execute_data *execute_data)
{
	zend_long id;
	zval *zp_count;
	HashTable *event_counts;

	excimer_mutex_lock(&excimer_timer_tls.mutex);
	event_counts = excimer_timer_tls.event_counts;
	excimer_timer_tls.event_counts = malloc(sizeof(HashTable));
	zend_hash_init(excimer_timer_tls.event_counts, 0, NULL, NULL, 1);
	excimer_mutex_unlock(&excimer_timer_tls.mutex);

	ZEND_HASH_FOREACH_NUM_KEY_VAL(event_counts, id, zp_count) {
		excimer_timer *timer = zend_hash_index_find_ptr(excimer_timer_tls.timers_by_id, id);
		/* If a previous callback destroyed this timer, then it would be
		 * missing from the timers_by_id hashtable. */
		if (timer) {
			timer->callback(Z_LVAL_P(zp_count), timer->user_data);
		}
	}
	ZEND_HASH_FOREACH_END();

	zend_hash_destroy(event_counts);
	free(event_counts);

	if (excimer_timer_globals.old_zend_interrupt_function) {
		excimer_timer_globals.old_zend_interrupt_function(execute_data);
	}
}

void excimer_timer_get_time(excimer_timer *timer, struct timespec *remaining)
{
	struct itimerspec its;

	if (!timer->is_valid || !timer->is_running) {
		remaining->tv_sec = 0;
		remaining->tv_nsec = 0;
		return;
	}

	timer_gettime(timer->timer_id, &its);
	remaining->tv_sec = its.it_value.tv_sec;
	remaining->tv_nsec = its.it_value.tv_nsec;
}
