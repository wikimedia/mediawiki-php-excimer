This is a prototype for a platform-independent timer library to be shared
between Excimer and LuaSandbox.

The library doesn't have its own build system or packaging.

`timerlib_config.h` belongs to the application. It allows the application to
configure the library.

Linux is the fully tested production platform. The kqueue implementation should
support BSDs and Mac OS. We try to be generic enough to allow for future Windows
support.

In most cases, errors are handled by calling an application-defined function and
returning `TIMERLIB_FAILURE`. The return value should be ignorable if the app's
error function handled the error sufficiently well.
