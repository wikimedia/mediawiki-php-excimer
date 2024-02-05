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

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "php.h"
#include <pthread.h>

int excimer_os_timer_create(int event_type, intptr_t timer_id, excimer_os_timer_t* os_timer, excimer_os_timer_notify_function_t* notify_function) {

	struct sigevent ev;

	memset(&ev, 0, sizeof(ev));
	ev.sigev_notify = SIGEV_THREAD;
	ev.sigev_notify_function = notify_function;
	ev.sigev_value.sival_ptr = (void*)timer_id;

	clockid_t clock_id;

	if (event_type == EXCIMER_CPU) {
		if (pthread_getcpuclockid(pthread_self(), &clock_id) != 0) {
			php_error_docref(NULL, E_WARNING, "Unable to get thread clock ID: %s",
				strerror(errno));
			return FAILURE;
		}
	} else {
		clock_id = CLOCK_MONOTONIC;
	}

	if (timer_create(clock_id, &ev, &os_timer->os_timer_id) != 0) {
		php_error_docref(NULL, E_WARNING, "Unable to create timer: %s",
			strerror(errno));
		return FAILURE;
	}

	os_timer->id = timer_id;

	return SUCCESS;
}

int excimer_os_timer_start(excimer_os_timer_t* os_timer, struct timespec *period, struct timespec *initial) {
	struct itimerspec its;
	its.it_interval = *period;
	its.it_value = *initial;

	if (timer_settime(os_timer->os_timer_id, 0, &its, NULL) != 0) {
		php_error_docref(NULL, E_WARNING, "timer_settime(): %s", strerror(errno));
		return FAILURE;
	}

	return SUCCESS;
}

int excimer_os_timer_stop(excimer_os_timer_t* os_timer) {
	struct itimerspec its;
	struct timespec zero = {0, 0};
	its.it_interval = zero;
	its.it_value = zero;

	if (timer_settime(os_timer->os_timer_id, 0, &its, NULL) != 0) {
		php_error_docref(NULL, E_WARNING, "timer_settime(): %s", strerror(errno));
		return FAILURE;
	}

	return SUCCESS;
}

int excimer_os_timer_delete(excimer_os_timer_t *os_timer) {
	if (timer_delete(os_timer->os_timer_id) != 0) {
		php_error_docref(NULL, E_WARNING, "timer_delete(): %s", strerror(errno));
		return FAILURE;
	}

	return SUCCESS;
}

zend_long excimer_os_timer_get_overrun_count(excimer_os_timer_t* os_timer) {
	return timer_getoverrun(os_timer->os_timer_id);
}

void excimer_os_timer_get_time(excimer_os_timer_t *timer, struct timespec *remaining) {
	struct itimerspec its;

	timer_gettime(timer->os_timer_id, &its);
	remaining->tv_sec = its.it_value.tv_sec;
	remaining->tv_nsec = its.it_value.tv_nsec;
}
