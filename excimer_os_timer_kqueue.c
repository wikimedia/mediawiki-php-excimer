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

#include "excimer_os_timer.h"
#include "excimer_mutex.h"

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "php.h"
#include "zend_types.h"
#include <pthread.h>

/**
 * Get the current time using a monotonic clock, raising a PHP Warning on failure.
 * @param time Pointer to a timespec struct to store the current time in.
 */
static void os_timer_get_current_time(struct timespec* time) {
	if (clock_gettime(CLOCK_MONOTONIC, time) == -1) {
		php_error_docref(NULL, E_WARNING, "clock_gettime(): %s", strerror(errno));
	}
}

/**
 * Check if a timespec is zero.
 * @param ts Pointer to the timespec to check.
 * @return 1 if the timespec is zero, 0 otherwise.
 */
static inline zend_bool os_timer_is_timespec_zero(struct timespec *ts) {
	return ts->tv_sec == 0 && ts->tv_nsec == 0;
}

/**
 * Handle a single timer event from a kqueue-based timer.
 * @param timer The os_timer instance holding the kqueue-based timer.
 * @param event Pointer to a kevent struct to store the event in.
 * @return SUCCESS if event processing may continue, FAILURE otherwise.
 */
static int os_timer_handle_timer_event(excimer_os_timer_t *timer, struct kevent* event) {
	int ret = kevent(timer->kq, NULL, 0, event, 1, NULL);
	if (ret == -1) {
		// EINTR merely implies that a signal was delivered before the timeout expired, so ignore it if it shows up.
		if (errno != EINTR) {
			// EBADF implies that the kqueue was closed, so we should exit the thread.
			if (errno != EBADF) {
				php_error_docref(NULL, E_WARNING, "kevent(): %s", strerror(errno));
			}

			return FAILURE;
		}
	} else if (ret > 0) {
		// Help emulate POSIX timer_gettime by keeping track of each moment the timer fires.
		excimer_mutex_lock(&timer->last_fired_at_mutex);
		os_timer_get_current_time(&timer->last_fired_at);
		excimer_mutex_unlock(&timer->last_fired_at_mutex);

		// Match the behavior of POSIX's timer_getoverrun, which only counts additional timer expirations.
		timer->overrun_count = event->data - 1;

		// Forward the timer ID received in the event to the notify function.
		// This uses a sigval so that excimer_timer_handle can be used as the notify function
		// irrespective of the platform.
		union sigval sv;
		sv.sival_ptr = event->udata;

		timer->notify_function(sv);
	}

	return SUCCESS;
}

/**
 * Configure a kqueue-based timer using the given flags and period.
 * @param os_timer Pointer to the os_timer this timer belongs to.
 * @param flags Flags to use when configuring the kqueue timer.
 * @param period Period to use for the timer.
 * @return SUCCESS if the timer was successfully setup, FAILURE otherwise.
 */
static int os_timer_setup_kqueue_timer(excimer_os_timer_t *os_timer, int flags, struct timespec* period) {
	zend_long timer_period_nanos = period->tv_sec * 1000000000 + period->tv_nsec;
	EV_SET(&os_timer->kev, os_timer->id, EVFILT_TIMER, flags, NOTE_NSECONDS, timer_period_nanos, (void*)os_timer->id);

	int ret = kevent(os_timer->kq, &os_timer->kev, 1, NULL, 0, NULL);
	if (ret == -1) {
		php_error_docref(NULL, E_WARNING, "kevent(): %s", strerror(errno));
		return FAILURE;
	}

	return SUCCESS;
}

/**
 * Main loop for a kqueue-based timer handler thread.
 * @param arg Pointer to the os_timer this handler belongs to.
 */
static void* os_timer_kqueue_handle(void *arg) {
	struct kevent event;

	excimer_os_timer_t *timer = (excimer_os_timer_t*)arg;

	// kqueue supports either periodic or one-shot timers, but not periodic timers with a delayed initial expiration.
	// So, if the initial delay is non-zero, wait for the one-shot initial timer to expire,
	// then - if needed - reconfigure the underlying kqueue as a periodic timer with the proper period going forward.
	if (!os_timer_is_timespec_zero(&timer->initial)) {
		if (os_timer_handle_timer_event(timer, &event) == FAILURE) {
			return NULL;
		}

		if (os_timer_is_timespec_zero(&timer->period)) {
			return NULL;
		}

		int ret = os_timer_setup_kqueue_timer(timer, EV_ADD | EV_ENABLE, &timer->period);
		if (ret == FAILURE) {
			return NULL;
		}
	}

	while (os_timer_handle_timer_event(timer, &event) == SUCCESS) {}

	return NULL;
}

int excimer_os_timer_create(int event_type, intptr_t timer_id, excimer_os_timer_t* os_timer, excimer_os_timer_notify_function_t* notify_function) {
	if (event_type == EXCIMER_CPU) {
		php_error_docref(NULL, E_WARNING, "CPU timers are not supported on this platform");
	}

	os_timer->kq = -1;
	os_timer->overrun_count = 0;
	os_timer->period.tv_nsec = 0;
	os_timer->period.tv_sec = 0;

	os_timer->last_fired_at.tv_sec = 0;
	os_timer->last_fired_at.tv_nsec = 0;

	os_timer->id = timer_id;
	os_timer->notify_function = notify_function;

	excimer_mutex_init(&os_timer->last_fired_at_mutex);

	return SUCCESS;
}

int excimer_os_timer_start(excimer_os_timer_t* os_timer, struct timespec *period, struct timespec *initial) {
	os_timer->period = *period;
	os_timer->initial = *initial;

	int kq = kqueue();
	if (kq == -1) {
		php_error_docref(NULL, E_WARNING, "kqueue(): %s", strerror(errno));
		return FAILURE;
	}
	os_timer->kq = kq;

	os_timer_get_current_time(&os_timer->last_fired_at);

	int flags = EV_ADD | EV_ENABLE;
	int ret;
	
	// Use a non-periodic timer if an initial expiration was provided
	if (!os_timer_is_timespec_zero(initial)) {
		flags |= EV_ONESHOT;
		ret = os_timer_setup_kqueue_timer(os_timer, flags, initial);
	} else {
		ret = os_timer_setup_kqueue_timer(os_timer, flags, period);
	}

	if (ret == FAILURE) {
		return FAILURE;
	}

	ret = pthread_create(&os_timer->handler_thread_id, NULL, os_timer_kqueue_handle, os_timer);
	if (ret != 0) {
		php_error_docref(NULL, E_WARNING, "pthread_create(): %s", strerror(ret));
		return FAILURE;
	}

	return SUCCESS;
}

int excimer_os_timer_stop(excimer_os_timer_t* os_timer) {
	if (os_timer->kq != -1) {
		os_timer->period.tv_sec = 0;
		os_timer->period.tv_nsec = 0;

		if (close(os_timer->kq) == -1) {
			php_error_docref(NULL, E_WARNING, "close(): %s", strerror(errno));
			return FAILURE;
		}

		// Wait for the signal handler thread to finish.
		int ret = pthread_join(os_timer->handler_thread_id, NULL);
		if (ret != 0) {
			php_error_docref(NULL, E_WARNING, "pthread_join(): %s", strerror(ret));
			return FAILURE;
		}
	}

	return SUCCESS;
}

int excimer_os_timer_delete(excimer_os_timer_t *os_timer) {
	excimer_mutex_destroy(&os_timer->last_fired_at_mutex);
	return SUCCESS;
}

zend_long excimer_os_timer_get_overrun_count(excimer_os_timer_t* os_timer) {
	return os_timer->overrun_count;
}

void excimer_os_timer_get_time(excimer_os_timer_t *timer, struct timespec *remaining) {
	struct timespec will_fire_at, now;

	excimer_mutex_lock(&timer->last_fired_at_mutex);
	will_fire_at.tv_sec = timer->last_fired_at.tv_sec + timer->period.tv_sec;
	will_fire_at.tv_nsec = timer->last_fired_at.tv_nsec + timer->period.tv_nsec;
	excimer_mutex_unlock(&timer->last_fired_at_mutex);

	os_timer_get_current_time(&now);
	remaining->tv_sec = will_fire_at.tv_sec - now.tv_sec;
	remaining->tv_nsec = will_fire_at.tv_nsec - now.tv_nsec;

	if (remaining->tv_nsec < 0) {
		remaining->tv_sec--;
		remaining->tv_nsec += 1000000000;
	}

	if (remaining->tv_sec < 0) {
		remaining->tv_sec = 0;
		remaining->tv_nsec = 0;
	}
}
