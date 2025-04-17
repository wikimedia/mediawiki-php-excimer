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

#include <pthread.h>
#include <time.h>

#define TIMERLIB_HAVE_CPU_CLOCK

typedef struct timespec timerlib_timespec_t;

typedef struct {
	// ID of the POSIX timer backing this timer
	timer_t timer;
	// True if timer is valid will need to be deleted
	int timer_valid;
	// The handler thread
	pthread_t thread;
	// True if the thread is valid
	int thread_valid;
	// The handler thread ID
	pid_t tid;
	// The clock type, TIMERLIB_REAL or TIMERLIB_CPU
	int clock;
	// Pointer to a callback to be invoked when this timer fires.
	timerlib_notify_function_t *notify_function;
	// Data to be passed to notify_function as the first argument
	void *notify_data;
	// The handler thread sets this when it is ready to receive events
	int ready;
	// A condition variable associated with "ready"
	pthread_cond_t ready_cond;
	// A mutex associated with "ready"
	pthread_mutex_t ready_mutex;
	// The main thread sets this to notify the handler thread that it should exit
	int killed;
} timerlib_timer_t;
