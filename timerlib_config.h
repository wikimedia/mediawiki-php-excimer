#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <php.h>
#include <signal.h>
#include "excimer_events.h"

#if defined(HAVE_SIGEV_THREAD_ID)
#define TIMERLIB_USE_POSIX
#elif defined(HAVE_KQUEUE)
#define TIMERLIB_USE_KQUEUE
#else
#error "No timer implementation available"
#endif

#define TIMERLIB_REAL EXCIMER_REAL
#define TIMERLIB_CPU EXCIMER_CPU
#define TIMERLIB_FAILURE FAILURE
#define TIMERLIB_SUCCESS SUCCESS

// PHP uses SIGRTMIN for request timeouts
#define TIMERLIB_SIGNAL (SIGRTMIN + 1)

/**
 * Report an error from a C library function in the main thread
 * @param func The function which encountered the error
 * @param error_number The error number, e.g. EINVAL
 */
inline static void timerlib_report_errno(const char *func, int error_number)
{
	php_error_docref(NULL, E_WARNING, "Error in %s(): %s", func, strerror(error_number));
}

/**
 * Report an error from a C library function and abort the program
 * @param tlfunc The timerlib function which caused the error
 * @param libfunc The C library function which caused the error
 * @param error_number The error number, e.g. EINVAL
 */
inline static void timerlib_abort_func(const char *tlfunc, const char *libfunc, int error_number) {
	fprintf(stderr, "Fatal error in %s/%s(): %s\n", tlfunc, libfunc, strerror(error_number));
	abort();
}

