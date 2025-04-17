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

#include "timerlib.h"

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <sys/event.h>

#include "timerlib_pthread_mutex.h"

/**
 * Handle a single timer event from a kqueue-based timer.
 * @param timer The timer instance holding the kqueue-based timer.
 * @param event Pointer to a kevent struct to store the event in.
 * @return TIMERLIB_SUCCESS if event processing may continue, TIMERLIB_FAILURE otherwise.
 */
static int timerlib_handle_timer_event(timerlib_timer_t *timer, struct kevent* event) {
	int ret = kevent(timer->kq, NULL, 0, event, 1, NULL);
	if (ret == -1) {
		// EINTR merely implies that a signal was delivered before the timeout expired, so ignore it if it shows up.
		if (errno != EINTR) {
			// EBADF implies that the kqueue was closed, so we should exit the thread.
			if (errno != EBADF) {
				timerlib_abort("kevent", errno);
			}

			return TIMERLIB_FAILURE;
		}
	} else if (ret > 0) {
		// Help emulate POSIX timer_gettime by keeping track of each moment the timer fires.
		timerlib_mutex_lock(&timer->last_fired_at_mutex);
		timerlib_clock_get_time(0, &timer->last_fired_at);
		timerlib_mutex_unlock(&timer->last_fired_at_mutex);

		// Match the behavior of POSIX's timer_getoverrun, which only counts additional timer expirations.
		int overrun_count = (int)event->data - 1;

		timer->notify_function(timer->notify_data, overrun_count);
	}

	return TIMERLIB_SUCCESS;
}

/**
 * Configure a kqueue-based timer using the given flags and period.
 * @param timer Pointer to the timer this timer belongs to.
 * @param flags Flags to use when configuring the kqueue timer.
 * @param period Period to use for the timer.
 * @return TIMERLIB_SUCCESS if the timer was successfully setup, TIMERLIB_FAILURE otherwise.
 */
static int timerlib_setup_kqueue_timer(timerlib_timer_t *timer, int flags, timerlib_timespec_t* period) {
	struct kevent kev;
	EV_SET(&kev, 1, EVFILT_TIMER, flags, 
			NOTE_NSECONDS, timerlib_timespec_to_ns(period), (void*)timer);

	int ret = kevent(timer->kq, &kev, 1, NULL, 0, NULL);
	if (ret == -1) {
		timerlib_report_errno("kevent", errno);
		return TIMERLIB_FAILURE;
	}

	return TIMERLIB_SUCCESS;
}

/**
 * Main loop for a kqueue-based timer handler thread.
 * @param arg Pointer to the timer this handler belongs to.
 */
static void* timerlib_kqueue_handle(void *arg) {
	struct kevent event;

	timerlib_timer_t *timer = (timerlib_timer_t*)arg;

	// kqueue supports either periodic or one-shot timers, but not periodic timers with a delayed initial expiration.
	// So, if the initial delay is non-zero, wait for the one-shot initial timer to expire,
	// then - if needed - reconfigure the underlying kqueue as a periodic timer with the proper period going forward.
	if (!timerlib_timespec_is_zero(&timer->initial)) {
		if (timerlib_handle_timer_event(timer, &event) == TIMERLIB_FAILURE) {
			return NULL;
		}

		if (timerlib_timespec_is_zero(&timer->period)) {
			return NULL;
		}

		int ret = timerlib_setup_kqueue_timer(timer, EV_ADD | EV_ENABLE, &timer->period);
		if (ret == TIMERLIB_FAILURE) {
			return NULL;
		}
	}

	while (timerlib_handle_timer_event(timer, &event) == TIMERLIB_SUCCESS) {}

	return NULL;
}

int timerlib_timer_init(timerlib_timer_t *timer, int clock,
		timerlib_notify_function_t *notify_function, void *notify_data)
{
	*timer = (timerlib_timer_t){
		.kq = -1,
		.notify_function = notify_function,
		.notify_data = notify_data,
		.last_fired_at_mutex = PTHREAD_MUTEX_INITIALIZER
	};
	return TIMERLIB_SUCCESS;
}

int timerlib_timer_start(timerlib_timer_t* timer, timerlib_timespec_t *period, timerlib_timespec_t *initial) {
	timer->period = *period;
	timer->initial = *initial;

	int kq = kqueue();
	if (kq == -1) {
		timerlib_report_errno("kqueue", errno);
		return TIMERLIB_FAILURE;
	}
	timer->kq = kq;

	timerlib_clock_get_time(0, &timer->last_fired_at);

	int flags = EV_ADD | EV_ENABLE;
	int ret;

	// Use a non-periodic timer if an initial expiration was provided
	if (!timerlib_timespec_is_zero(initial)) {
		flags |= EV_ONESHOT;
		ret = timerlib_setup_kqueue_timer(timer, flags, initial);
	} else {
		ret = timerlib_setup_kqueue_timer(timer, flags, period);
	}

	if (ret == TIMERLIB_FAILURE) {
		return TIMERLIB_FAILURE;
	}

	ret = pthread_create(&timer->handler_thread_id, NULL, timerlib_kqueue_handle, timer);
	if (ret != 0) {
		timerlib_report_errno("pthread_create", ret);
		return TIMERLIB_FAILURE;
	}

	return TIMERLIB_SUCCESS;
}

int timerlib_timer_stop(timerlib_timer_t* timer) {
	if (timer->kq != -1) {
		timer->period.tv_sec = 0;
		timer->period.tv_nsec = 0;

		if (close(timer->kq) == -1) {
			timerlib_report_errno("close", errno);
			return TIMERLIB_FAILURE;
		}

		// Wait for the signal handler thread to finish.
		int ret = pthread_join(timer->handler_thread_id, NULL);
		if (ret != 0) {
			timerlib_report_errno("pthread_join", ret);
			return TIMERLIB_FAILURE;
		}
	}

	return TIMERLIB_SUCCESS;
}

void timerlib_timer_destroy(timerlib_timer_t *timer) {

	int ret = pthread_mutex_destroy(&timer->last_fired_at_mutex);
	if (ret != 0) {
		timerlib_report_errno("pthread_mutex_destroy", ret);
	}
}

int timerlib_timer_get_time(timerlib_timer_t *timer, timerlib_timespec_t *remaining) {
	// Get the time at which the timer last fired
	timerlib_mutex_lock(&timer->last_fired_at_mutex);
	timerlib_timespec_t last_fired_at = timer->last_fired_at;
	timerlib_mutex_unlock(&timer->last_fired_at_mutex);

	// Add the period to get the next expiry time
	timerlib_timespec_t will_fire_at = timer->period;
	timerlib_timespec_add(&will_fire_at, &last_fired_at);

	// Subtract the current time to get the remaining time
	timerlib_timespec_t now;
	timerlib_clock_get_time(0, &now);
	*remaining = will_fire_at;
	timerlib_timespec_subtract(remaining, &now);

	return TIMERLIB_SUCCESS;
}

int timerlib_clock_get_time(int clock, timerlib_timespec_t* time) {
	if (clock_gettime(CLOCK_MONOTONIC, time) == -1) {
		timerlib_report_errno("clock_gettime", errno);
		return TIMERLIB_FAILURE;
	}
	return TIMERLIB_SUCCESS;
}

