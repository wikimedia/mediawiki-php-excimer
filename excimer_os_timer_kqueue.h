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

#ifndef EXCIMER_OS_TIMER_KQUEUE_H
#define EXCIMER_OS_TIMER_KQUEUE_H

#include <sys/event.h>
#include <unistd.h>
#include <signal.h>
#include <stdint.h>
#include <time.h>
#include <pthread.h>
#include "excimer_mutex.h"
#include "excimer_events.h"
#include "php.h"

/** Signature for a callback to be invoked when a timer fires. */
typedef void (excimer_os_timer_notify_function_t)(union sigval sv);

/** Represents a timer backed by kqueue. */
typedef struct {
	/** File descriptor of the kqueue backing this timer */
	int kq;
	/** ID of the Excimer timer that owns this timer */
	intptr_t id;
	/** The overrun count for this timer */
	volatile int overrun_count;
	/** The kevent structure controlling this timer */
	struct kevent kev;
	/** The period of this timer */
	struct timespec period;
	/** The initial expiration time of this timer */
	struct timespec initial;
	/** Pointer to a callback to be invoked when this timer fires. */
	excimer_os_timer_notify_function_t* notify_function;
	/** Thread ID of the kqueue signal handler thread for this timer. */
	pthread_t handler_thread_id;
	/** The time at which this timer last fired. */
	struct timespec last_fired_at;
	/** Mutex to protect last_fired_at from concurrent access */
	pthread_mutex_t last_fired_at_mutex;
} excimer_os_timer_t;

#endif
