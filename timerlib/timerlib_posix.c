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

// For gettid, pthread_attr_setsigmask_np
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include "timerlib.h"

#include <signal.h>
#include <unistd.h>

// https://sourceware.org/bugzilla/show_bug.cgi?id=27417
# ifndef sigev_notify_thread_id
# define sigev_notify_thread_id _sigev_un._tid
# endif

#ifdef HAVE_PTHREAD_ATTR_SETSIGMASK_NP
// glibc 2.32+: set the new thread's sigmask using an attribute
static inline void timerlib_block_signals(pthread_attr_t *attr, sigset_t *old_sigmask)
{
	sigset_t sigmask;
	sigfillset(&sigmask);
	pthread_attr_setsigmask_np(attr, &sigmask);
}

static inline void timerlib_unblock_signals(sigset_t *old_sigmask)
{
}

#else
// glibc before 2.32: save and restore the main thread's sigmask so that the new
// thread will inherit a sigmask with all signals blocked
static inline void timerlib_block_signals(pthread_attr_t *attr, sigset_t *old_sigmask)
{
	sigset_t sigmask;
	sigfillset(&sigmask);
	pthread_sigmask(SIG_BLOCK, &sigmask, old_sigmask);
}

static inline void timerlib_unblock_signals(sigset_t *old_sigmask)
{
	pthread_sigmask(SIG_SETMASK, old_sigmask, NULL);
}
#endif

#ifndef HAVE_GETTID
#include <sys/syscall.h>
#define gettid() ((pid_t)syscall(SYS_gettid))
#endif

#include "timerlib_pthread_mutex.h"

/**
 * This is called by the handler thread to notify the main thread that
 * timer->tid is valid.
 */
static void timerlib_notify_ready(timerlib_timer_t *timer)
{
	timerlib_mutex_lock(&timer->ready_mutex);
	timer->ready = 1;
	int error = pthread_cond_broadcast(&timer->ready_cond);
	if (error) {
		timerlib_abort("pthread_cond_broadcast", error);
	}
	timerlib_mutex_unlock(&timer->ready_mutex);
}

/**
 * Stop the handler thread (called from the main thread)
 */
static int timerlib_graceful_exit(timerlib_timer_t *timer)
{
	// We share TIMERLIB_SIGNAL between timer expiration and shutdown, mostly
	// to be less intrusive on the application. But if an expiration signal
	// is already pending, sending another will be ignored. We set a variable
	// so that the thread will terminate anyway in that case.
	timer->killed = 1;
	int error = pthread_kill(timer->thread, TIMERLIB_SIGNAL);
	if (error) {
		timerlib_report_errno("pthread_kill", error);
		return TIMERLIB_FAILURE;
	}
	return TIMERLIB_SUCCESS;
}

/**
 * Join the handler thread, wait for it to exit.
 */
static int timerlib_join(timerlib_timer_t *timer)
{
	int error = pthread_join(timer->thread, NULL);
	if (error) {
		timerlib_report_errno("pthread_join", error);
		return TIMERLIB_FAILURE;
	}
	return TIMERLIB_SUCCESS;
}

/**
 * Convert a timerlib clock constant to a POSIX clock
 * @param clock May be either TIMERLIB_REAL or TIMERLIB_CPU
 */
static clockid_t timerlib_map_clock(int clock)
{
	if (clock == TIMERLIB_REAL) {
		return CLOCK_MONOTONIC;
	} else {
		clockid_t c = CLOCK_MONOTONIC;
		int error = pthread_getcpuclockid(pthread_self(), &c);
		if (error) {
			timerlib_report_errno("pthread_getcpuclockid", error);
		}
		return c;
	}
}

/**
 * The start routine of the handler thread
 */
static void* timerlib_timer_thread_main(void *data)
{
	timerlib_timer_t *timer = data;
	timer->tid = gettid();

	// Tell the main thread we are ready to start
	timerlib_notify_ready(timer);

	// Receive only our signal
	sigset_t sigset;
	sigemptyset(&sigset);
	sigaddset(&sigset, TIMERLIB_SIGNAL);

	while (1) {
		siginfo_t si;
		// There is an identical empty loop in the glibc SIGEV_THREAD
		// implementation. The documentation indicates that EINTR is the only
		// possible error.
		while (sigwaitinfo(&sigset, &si) < 0);
		// If timer_stop() has been called, exit the thread
		if (timer->killed) {
			return NULL;
		}
		// If signal occurred due to a timer expiration, call the callback.
		if (si.si_code == SI_TIMER) {
			timer->notify_function(timer->notify_data, si.si_overrun);
		}
	}
}

int timerlib_timer_init(timerlib_timer_t *timer, int clock,
		timerlib_notify_function_t *notify_function, void *notify_data)
{
	// Initialise the data. Fields that are not named are automatically zeroed.
	*timer = (timerlib_timer_t){
		.clock = clock,
		.notify_function = notify_function,
		.notify_data = notify_data,
		.ready_cond = PTHREAD_COND_INITIALIZER,
		.ready_mutex = PTHREAD_MUTEX_INITIALIZER,
	};

	// Block all signals. This prevents the thread from receiving process-directed
	// signals which are normally handled by the main thread.
	pthread_attr_t attr;
	sigset_t old_sigset;
	pthread_attr_init(&attr);
	timerlib_block_signals(&attr, &old_sigset);

	// Create the thread
	int error = pthread_create(&timer->thread, &attr, timerlib_timer_thread_main, timer);
	timerlib_unblock_signals(&old_sigset);
	pthread_attr_destroy(&attr);
	if (error) {
		timerlib_report_errno("pthread_create", error);
		return TIMERLIB_FAILURE;
	}
	timer->thread_valid = 1;

	// Wait for timer->tid to become valid
	timerlib_mutex_lock(&timer->ready_mutex);
	while (!timer->ready) {
		error = pthread_cond_wait(&timer->ready_cond, &timer->ready_mutex);
		if (error) {
			timerlib_report_errno("pthread_cond_wait", error);
			return TIMERLIB_FAILURE;
		}
	}
	timerlib_mutex_unlock(&timer->ready_mutex);

	// Create the timer
	// This needs to be done in the main thread, otherwise it silently fails
	// to deliver any events in CPU mode.
	struct sigevent sev = {
		.sigev_signo = TIMERLIB_SIGNAL,
		.sigev_notify = SIGEV_THREAD_ID,
		.sigev_notify_thread_id = timer->tid
	};
	if (timer_create(timerlib_map_clock(timer->clock), &sev, &timer->timer)) {
		timerlib_report_errno("timer_create", errno);
		return TIMERLIB_FAILURE;
	}

	// Remember that timer->timer is valid and needs to be deleted
	timer->timer_valid = 1;

	return TIMERLIB_SUCCESS;
}

int timerlib_timer_start(timerlib_timer_t *timer, timerlib_timespec_t *period, timerlib_timespec_t *initial)
{
	struct itimerspec its = {
		.it_interval = *period,
		.it_value = *initial
	};

	if (!timer->timer_valid) {
		// No point reporting another error, since we presumably already reported
		// an error in timerlib_timer_init
		return TIMERLIB_FAILURE;
	}

	if (timerlib_timespec_is_zero(initial)) {
		// Make sure the timer is armed
		its.it_value.tv_nsec = 1;
	}

	if (timer_settime(timer->timer, 0, &its, NULL) != 0) {
		timerlib_report_errno("timer_settime", errno);
		return TIMERLIB_FAILURE;
	}

	return TIMERLIB_SUCCESS;
}

int timerlib_timer_stop(timerlib_timer_t * timer)
{
	struct itimerspec its = {0};

	if (!timer->timer_valid) {
		return TIMERLIB_FAILURE;
	}

	if (timer_settime(timer->timer, 0, &its, NULL) != 0) {
		timerlib_report_errno("timer_settime", errno);
		return TIMERLIB_FAILURE;
	}

	return TIMERLIB_SUCCESS;
}

void timerlib_timer_destroy(timerlib_timer_t * timer)
{
	if (timer->thread_valid) {
		timer->thread_valid = 0;
		if (timerlib_graceful_exit(timer) == TIMERLIB_SUCCESS) {
			timerlib_join(timer);
		}
	}
	if (timer->timer_valid) {
		timer->timer_valid = 0;
		if (timer_delete(timer->timer)) {
			timerlib_report_errno("timer_delete", errno);
		}
	}
}

int timerlib_timer_get_time(timerlib_timer_t *timer, timerlib_timespec_t *remaining)
{
	int ret = TIMERLIB_FAILURE;
	struct itimerspec its = {0};
	// Write to *remaining even on error, so that an unchecked error value will
	// not lead to the caller using uninitialised memory.
	if (timer->timer_valid) {
		if (timer_gettime(timer->timer, &its)) {
			timerlib_report_errno("timer_gettime", errno);
		} else {
			ret = TIMERLIB_SUCCESS;
		}
	}
	*remaining = its.it_value;
	return ret;
}

int timerlib_clock_get_time(int clock, timerlib_timespec_t * time)
{
	if (clock_gettime(timerlib_map_clock(clock), time)) {
		*time = (timerlib_timespec_t){0};
		timerlib_report_errno("timer_gettime", errno);
		return TIMERLIB_FAILURE;
	} else {
		return TIMERLIB_SUCCESS;
	}
}
