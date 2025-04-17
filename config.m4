dnl config.m4 for extension excimer

PHP_ARG_ENABLE(excimer, whether to enable excimer support,
[  --enable-excimer           Enable excimer support])

if test "$PHP_EXCIMER" != "no"; then
  dnl Timers require real-time and pthread library on Linux and not
  dnl supported on other platforms
  AC_CHECK_DECL(SIGEV_THREAD_ID, [
    AC_CHECK_LIB(rt, timer_create)

    AC_CHECK_DECL(pthread_attr_setsigmask_np,[
      AC_DEFINE(HAVE_PTHREAD_ATTR_SETSIGMASK_NP, 1, [Whether pthread_attr_setsigmask_np is available])
    ],,[[
      #define _GNU_SOURCE 1
      #include <pthread.h>
    ]])

    AC_CHECK_DECL(gettid,[
      AC_DEFINE(HAVE_GETTID, 1, [Whether gettid is available])
    ],,[[
      #define _GNU_SOURCE 1
      #include <unistd.h>
    ]])

    AC_DEFINE(HAVE_SIGEV_THREAD_ID, 1, [Whether SIGEV_THREAD_ID is available on the current platform])
    PHP_EVAL_LIBLINE($LIBS, EXCIMER_SHARED_LIBADD)
    excimer_os_sources=timerlib/timerlib_posix.c
  ], [
    AC_SEARCH_LIBS([kevent], [kqueue], [
      AC_DEFINE(HAVE_KQUEUE, 1, [Whether kqueue is available on the current platform])
      PHP_EVAL_LIBLINE($LIBS, EXCIMER_SHARED_LIBADD)
      excimer_os_sources=timerlib/timerlib_kqueue.c
    ], [
      AC_MSG_ERROR([excimer requires timer_create or kevent])
    ])
  ], [[
    #include <signal.h>
  ]])
  AC_SEARCH_LIBS([pthread_mutex_lock], [pthread], [
    PHP_EVAL_LIBLINE($LIBS, EXCIMER_SHARED_LIBADD)
  ])


  PHP_SUBST(EXCIMER_SHARED_LIBADD)
  PHP_NEW_EXTENSION(excimer, excimer.c \
    excimer_mutex.c \
    excimer_timer.c \
    excimer_log.c \
    timerlib/timerlib_common.c \
    $excimer_os_sources, $ext_shared)
fi
