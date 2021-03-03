dnl config.m4 for extension excimer

PHP_ARG_ENABLE(excimer, whether to enable excimer support,
[  --enable-excimer           Enable excimer support])

if test "$PHP_EXCIMER" != "no"; then
  dnl Timers require real-time and pthread library on Linux and not
  dnl supported on other platforms
  AC_SEARCH_LIBS([timer_create], [rt], [
    PHP_EVAL_LIBLINE($LIBS, EXCIMER_SHARED_LIBADD)
  ])
  AC_SEARCH_LIBS([sem_init], [pthread], [
    PHP_EVAL_LIBLINE($LIBS, EXCIMER_SHARED_LIBADD)
  ])

  PHP_SUBST(EXCIMER_SHARED_LIBADD)
  PHP_NEW_EXTENSION(excimer, excimer.c excimer_timer.c excimer_log.c, $ext_shared)
fi
