#ifndef TIMERLIB_KQUEUE_H
#define TIMERLIB_KQUEUE_H

#include <sys/event.h>
#include <sys/time.h>

typedef struct timespec timerlib_timespec_t;

/** Represents a timer backed by kqueue. */
typedef struct {
	/** File descriptor of the kqueue backing this timer */
	int kq;
	/** The overrun count for this timer */
	volatile int overrun_count;
	/** The period of this timer */
	struct timespec period;
	/** The initial expiration time of this timer */
	struct timespec initial;
	/** Pointer to a callback to be invoked when this timer fires. */
	timerlib_notify_function_t *notify_function;
	/** Data to be passed to the callback */
	void *notify_data;

	/** Thread ID of the kqueue signal handler thread for this timer. */
	pthread_t handler_thread_id;
	/** The time at which this timer last fired. */
	struct timespec last_fired_at;
	/** Mutex to protect last_fired_at from concurrent access */
	pthread_mutex_t last_fired_at_mutex;
} timerlib_timer_t;

#endif
