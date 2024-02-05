dnl config.m4 for extension excimer

PHP_ARG_ENABLE(excimer, whether to enable excimer support,
[  --enable-excimer           Enable excimer support])

if test "$PHP_EXCIMER" != "no"; then
  dnl Timers require real-time and pthread library on Linux and not
  dnl supported on other platforms
  AC_SEARCH_LIBS([timer_create], [rt], [
    AC_DEFINE(HAVE_TIMER_CREATE, 1, [Whether timer_create is available on the current platform])
    PHP_EVAL_LIBLINE($LIBS, EXCIMER_SHARED_LIBADD)
    excimer_os_sources=excimer_os_timer_posix.c
  ], [
    AC_SEARCH_LIBS([kevent], [kqueue], [
      PHP_EVAL_LIBLINE($LIBS, EXCIMER_SHARED_LIBADD)
      excimer_os_sources=excimer_os_timer_kqueue.c
    ], [
      AC_MSG_ERROR([excimer requires timer_create or kevent])
    ])
  ])
  AC_SEARCH_LIBS([sem_init], [pthread], [
    PHP_EVAL_LIBLINE($LIBS, EXCIMER_SHARED_LIBADD)
  ])

  PHP_SUBST(EXCIMER_SHARED_LIBADD)
  PHP_NEW_EXTENSION(excimer, excimer.c excimer_mutex.c excimer_timer.c excimer_log.c $excimer_os_sources, $ext_shared)
fi
