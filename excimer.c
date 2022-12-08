/* Copyright 2018 Wikimedia Foundation
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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <time.h>

#include "php.h"
#include "zend_exceptions.h"
#include "zend_interfaces.h"
#include "ext/spl/spl_exceptions.h"
#include "ext/standard/php_mt_rand.h"
#include "ext/standard/info.h"

#if PHP_VERSION_ID < 70200
/* For spl_ce_Countable */
#include "ext/spl/spl_iterators.h"
#endif

#include "php_excimer.h"
#include "excimer_timer.h"
#include "excimer_log.h"

#define EXCIMER_OBJ(type, object) \
	((type ## _obj*)excimer_check_object(object, XtOffsetOf(type ## _obj, std), &type ## _handlers))

#define EXCIMER_OBJ_Z(type, zval) (Z_TYPE(zval) == IS_OBJECT ? EXCIMER_OBJ(type, Z_OBJ(zval)) : NULL)

#define EXCIMER_OBJ_ZP(type, zval_ptr) EXCIMER_OBJ(type, Z_OBJ_P(zval_ptr))

#define EXCIMER_NEW_OBJECT(type, ce) \
	excimer_object_alloc_init(sizeof(type ## _obj), &type ## _handlers, ce)

#define EXCIMER_DEFAULT_PERIOD 0.1
#define EXCIMER_BILLION 1000000000LL
/* {{{ types */

/**
 * ExcimerProfiler_obj: underlying storage for ExcimerProfiler
 */
typedef struct {
	/** The period which will be used when the timer is next started */
	struct timespec period;

	/** The initial interval */
	struct timespec initial;

	/** The event type, either EXCIMER_CPU or EXCIMER_REAL */
	zend_long event_type;

	/** The currently-attached log */
	zval z_log;

	/** The flush callback. If this is set, max_samples will also be set. */
	zval z_callback;

	/** The maximum number of samples in z_log before z_callback is called. */
	zend_long max_samples;

	/** The timer backend object */
	excimer_timer timer;
	zend_object std;
} ExcimerProfiler_obj;

/**
 * ExcimerLog_iterator: Iterator object returned by get_iterator handler, used
 * by foreach.
 */
typedef struct {
	/**
	 * FIXME Iterators use PHP 5 style inheritance. This is actually a
	 * zend_object header. Maybe it is harmless but it seems dodgy to me,
	 * should be fixed upstream.
	 */
	zend_user_iterator intern;

	/** Cached (lazy-initialised) value to use for current() */
	zval z_current;

	/** Current log index */
	zend_long index;
} ExcimerLog_iterator;

/**
 * ExcimerLog_obj: underlying storage for ExcimerLog
 */
typedef struct {
	/** The log backend object */
	excimer_log log;

	/** The cached value to use for current() */
	zval z_current;

	/** The current index, for key() etc. */
	zend_long iter_entry_index;
	zend_object std;
} ExcimerLog_obj;

/**
 * ExcimerLogEntry_obj: underlying storage for ExcimerLogEntry
 */
typedef struct {
	/**
	 * The ExcimerLog. Note that this can be a circular reference if
	 * ExcimerLog_obj.z_current points here.
	 */
	zval z_log;

	/** The index of this entry in the ExcimerLog */
	zend_long index;
	zend_object std;
} ExcimerLogEntry_obj;

/**
 * ExcimerTimer_obj: underlying storage for ExcimerTimer
 */
typedef struct {
	/** The timer backend object */
	excimer_timer timer;

	/** The timer period */
	struct timespec period;

	/** The initial expiry, or zero to use the period */
	struct timespec initial;

	/** The event type, EXCIMER_REAL or EXCIMER_CPU */
	zend_long event_type;

	/** The event function, or null for no callback */
	zval z_callback;
	zend_object std;
} ExcimerTimer_obj;
/* }}} */

/* {{{ static function declarations */
static void ExcimerProfiler_start(ExcimerProfiler_obj *profiler);
static void ExcimerProfiler_stop(ExcimerProfiler_obj *profiler);
static void ExcimerProfiler_event(zend_long event_count, void *user_data);
static void ExcimerProfiler_flush(ExcimerProfiler_obj *profiler, zval *zp_old_log);

static zend_object *ExcimerProfiler_new(zend_class_entry *ce);
static void ExcimerProfiler_free_object(zend_object *object);
static void ExcimerProfiler_dtor(zend_object *object);
static PHP_METHOD(ExcimerProfiler, setPeriod);
static PHP_METHOD(ExcimerProfiler, setEventType);
static PHP_METHOD(ExcimerProfiler, setMaxDepth);
static PHP_METHOD(ExcimerProfiler, setFlushCallback);
static PHP_METHOD(ExcimerProfiler, clearFlushCallback);
static PHP_METHOD(ExcimerProfiler, start);
static PHP_METHOD(ExcimerProfiler, stop);
static PHP_METHOD(ExcimerProfiler, getLog);
static PHP_METHOD(ExcimerProfiler, flush);

static zend_object *ExcimerLog_new(zend_class_entry *ce);
static void ExcimerLog_free_object(zend_object *object);
static zend_object_iterator *ExcimerLog_get_iterator(zend_class_entry *ce, zval *object, int by_ref);

#if PHP_VERSION_ID < 80000
static int ExcimerLog_count_elements(zval *zp_log, zend_long *lp_count);
#else
static int ExcimerLog_count_elements(zend_object *object, zend_long *lp_count);
#endif

static void ExcimerLog_init_entry(zval *zp_dest, zval *zp_log, zend_long index);

static void ExcimerLog_iterator_dtor(zend_object_iterator *iter);
static int ExcimerLog_iterator_valid(zend_object_iterator *iter);
static zval *ExcimerLog_iterator_get_current_data(zend_object_iterator *iter);
static void ExcimerLog_iterator_get_current_key(zend_object_iterator *iter, zval *key);
static void ExcimerLog_iterator_move_forward(zend_object_iterator *iter);
static void ExcimerLog_iterator_rewind(zend_object_iterator *iter);
static void ExcimerLog_iterator_invalidate_current(zend_object_iterator *iter);

static PHP_METHOD(ExcimerLog, __construct);
static PHP_METHOD(ExcimerLog, formatCollapsed);
static PHP_METHOD(ExcimerLog, getSpeedscopeData);
static PHP_METHOD(ExcimerLog, aggregateByFunction);
static PHP_METHOD(ExcimerLog, getEventCount);
static PHP_METHOD(ExcimerLog, current);
static PHP_METHOD(ExcimerLog, key);
static PHP_METHOD(ExcimerLog, next);
static PHP_METHOD(ExcimerLog, rewind);
static PHP_METHOD(ExcimerLog, valid);
static PHP_METHOD(ExcimerLog, count);
static PHP_METHOD(ExcimerLog, offsetExists);
static PHP_METHOD(ExcimerLog, offsetGet);
static PHP_METHOD(ExcimerLog, offsetSet);
static PHP_METHOD(ExcimerLog, offsetUnset);

static zend_object *ExcimerLogEntry_new(zend_class_entry *ce);
static void ExcimerLogEntry_free_object(zend_object *object);

static PHP_METHOD(ExcimerLogEntry, __construct);
static PHP_METHOD(ExcimerLogEntry, getTimestamp);
static PHP_METHOD(ExcimerLogEntry, getEventCount);
static PHP_METHOD(ExcimerLogEntry, getTrace);

static zend_object *ExcimerTimer_new(zend_class_entry *ce);
static void ExcimerTimer_free_object(zend_object *object);
static PHP_METHOD(ExcimerTimer, setEventType);
static PHP_METHOD(ExcimerTimer, setInterval);
static PHP_METHOD(ExcimerTimer, setPeriod);
static PHP_METHOD(ExcimerTimer, setCallback);
static PHP_METHOD(ExcimerTimer, start);
static PHP_METHOD(ExcimerTimer, stop);
static PHP_METHOD(ExcimerTimer, getTime);

static void ExcimerTimer_start(ExcimerTimer_obj *timer_obj);
static void ExcimerTimer_stop(ExcimerTimer_obj *timer_obj);
static void ExcimerTimer_event(zend_long event_count, void *user_data);
static int ExcimerTimer_set_callback(ExcimerTimer_obj *timer_obj, zval *zp_callback);

static PHP_FUNCTION(excimer_set_timeout);
/* }}} */

static zend_class_entry *ExcimerProfiler_ce;
static zend_class_entry *ExcimerLog_ce;
static zend_class_entry *ExcimerLogEntry_ce;
static zend_class_entry *ExcimerTimer_ce;

static zend_object_handlers ExcimerProfiler_handlers;
static zend_object_handlers ExcimerLog_handlers;
static zend_object_handlers ExcimerLogEntry_handlers;
static zend_object_handlers ExcimerTimer_handlers;

/** {{{ arginfo */
#ifndef ZEND_BEGIN_ARG_WITH_TENTATIVE_RETURN_TYPE_INFO_EX
#define ZEND_BEGIN_ARG_WITH_TENTATIVE_RETURN_TYPE_INFO_EX(name, return_reference, required_num_args, type, allow_null) \
        ZEND_BEGIN_ARG_INFO_EX(name, 0, return_reference, required_num_args)
#endif

ZEND_BEGIN_ARG_INFO(arginfo_ExcimerProfiler_setPeriod, 0)
	ZEND_ARG_INFO(0, period)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_ExcimerProfiler_setEventType, 0)
	ZEND_ARG_INFO(0, event_type)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_ExcimerProfiler_setMaxDepth, 0)
	ZEND_ARG_INFO(0, max_depth)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_ExcimerProfiler_setFlushCallback, 0)
	ZEND_ARG_INFO(0, callback)
	ZEND_ARG_INFO(0, max_samples)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_ExcimerProfiler_clearFlushCallback, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_ExcimerProfiler_start, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_ExcimerProfiler_stop, 0)
ZEND_END_ARG_INFO()

#if PHP_VERSION_ID >= 70200
ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_ExcimerProfiler_getLog, 0, 0, ExcimerLog, 0)
#else
ZEND_BEGIN_ARG_INFO(arginfo_ExcimerProfiler_getLog, 0)
#endif
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_ExcimerProfiler_flush, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_ExcimerLog___construct, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_ExcimerLog_formatCollapsed, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_ExcimerLog_getSpeedscopeData, 0)
ZEND_END_ARG_INFO()

#if PHP_VERSION_ID < 70200
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO(arginfo_ExcimerLog_aggregateByFunction, IS_ARRAY, NULL, 0)
#else
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO(arginfo_ExcimerLog_aggregateByFunction, IS_ARRAY, 0)
#endif
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_ExcimerLog_getEventCount, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_TENTATIVE_RETURN_TYPE_INFO_EX(arginfo_ExcimerLog_current, 0, 0, IS_MIXED, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_TENTATIVE_RETURN_TYPE_INFO_EX(arginfo_ExcimerLog_key, 0, 0, IS_MIXED, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_TENTATIVE_RETURN_TYPE_INFO_EX(arginfo_ExcimerLog_next, 0, 0, IS_VOID, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_TENTATIVE_RETURN_TYPE_INFO_EX(arginfo_ExcimerLog_rewind, 0, 0, IS_VOID, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_TENTATIVE_RETURN_TYPE_INFO_EX(arginfo_ExcimerLog_valid, 0, 0, _IS_BOOL, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_TENTATIVE_RETURN_TYPE_INFO_EX(arginfo_ExcimerLog_count, 0, 0, IS_LONG, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_TENTATIVE_RETURN_TYPE_INFO_EX(arginfo_ExcimerLog_offsetExists, 0, 1, _IS_BOOL, 0)
	ZEND_ARG_INFO(0, offset)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_TENTATIVE_RETURN_TYPE_INFO_EX(arginfo_ExcimerLog_offsetGet, 0, 1, IS_MIXED, 0)
	ZEND_ARG_INFO(0, offset)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_TENTATIVE_RETURN_TYPE_INFO_EX(arginfo_ExcimerLog_offsetSet, 0, 2, IS_VOID, 0)
	ZEND_ARG_INFO(0, offset)
	ZEND_ARG_INFO(0, value)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_TENTATIVE_RETURN_TYPE_INFO_EX(arginfo_ExcimerLog_offsetUnset, 0, 1, IS_VOID, 0)
	ZEND_ARG_INFO(0, offset)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_ExcimerLogEntry___construct, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_ExcimerLogEntry_getTimestamp, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_ExcimerLogEntry_getEventCount, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_ExcimerLogEntry_getTrace, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_ExcimerTimer_setEventType, 0)
	ZEND_ARG_INFO(0, event_type)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_ExcimerTimer_setInterval, 0)
	ZEND_ARG_INFO(0, interval)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_ExcimerTimer_setPeriod, 0)
	ZEND_ARG_INFO(0, period)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_ExcimerTimer_setCallback, 0)
	ZEND_ARG_INFO(0, callback)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_ExcimerTimer_start, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_ExcimerTimer_stop, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_ExcimerTimer_getTime, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_excimer_set_timeout, 0)
	ZEND_ARG_INFO(0, callback)
	ZEND_ARG_INFO(0, interval)
ZEND_END_ARG_INFO()

/* }}} */

/** {{{ function entries */
static const zend_function_entry ExcimerProfiler_methods[] = {
	PHP_ME(ExcimerProfiler, setPeriod, arginfo_ExcimerProfiler_setPeriod, 0)
	PHP_ME(ExcimerProfiler, setEventType, arginfo_ExcimerProfiler_setEventType, 0)
	PHP_ME(ExcimerProfiler, setMaxDepth, arginfo_ExcimerProfiler_setMaxDepth, 0)
	PHP_ME(ExcimerProfiler, setFlushCallback, arginfo_ExcimerProfiler_setFlushCallback, 0)
	PHP_ME(ExcimerProfiler, clearFlushCallback, arginfo_ExcimerProfiler_clearFlushCallback, 0)
	PHP_ME(ExcimerProfiler, start, arginfo_ExcimerProfiler_start, 0)
	PHP_ME(ExcimerProfiler, stop, arginfo_ExcimerProfiler_stop, 0)
	PHP_ME(ExcimerProfiler, getLog, arginfo_ExcimerProfiler_getLog, 0)
	PHP_ME(ExcimerProfiler, flush, arginfo_ExcimerProfiler_flush, 0)
	PHP_FE_END
};

static const zend_function_entry ExcimerLog_methods[] = {
	PHP_ME(ExcimerLog, __construct, arginfo_ExcimerLog___construct,
		ZEND_ACC_PRIVATE | ZEND_ACC_FINAL)
	PHP_ME(ExcimerLog, formatCollapsed, arginfo_ExcimerLog_formatCollapsed, 0)
	PHP_ME(ExcimerLog, getSpeedscopeData, arginfo_ExcimerLog_getSpeedscopeData, 0)
	PHP_ME(ExcimerLog, aggregateByFunction, arginfo_ExcimerLog_aggregateByFunction, 0)
	PHP_ME(ExcimerLog, getEventCount, arginfo_ExcimerLog_getEventCount, 0)
	PHP_ME(ExcimerLog, current, arginfo_ExcimerLog_current, 0)
	PHP_ME(ExcimerLog, key, arginfo_ExcimerLog_key, 0)
	PHP_ME(ExcimerLog, next, arginfo_ExcimerLog_next, 0)
	PHP_ME(ExcimerLog, rewind, arginfo_ExcimerLog_rewind, 0)
	PHP_ME(ExcimerLog, valid, arginfo_ExcimerLog_valid, 0)
	PHP_ME(ExcimerLog, count, arginfo_ExcimerLog_count, 0)
	PHP_ME(ExcimerLog, offsetExists, arginfo_ExcimerLog_offsetExists, 0)
	PHP_ME(ExcimerLog, offsetGet, arginfo_ExcimerLog_offsetGet, 0)
	PHP_ME(ExcimerLog, offsetSet, arginfo_ExcimerLog_offsetSet, 0)
	PHP_ME(ExcimerLog, offsetUnset, arginfo_ExcimerLog_offsetUnset, 0)
	PHP_FE_END
};

static zend_object_iterator_funcs ExcimerLog_iterator_funcs = {
	ExcimerLog_iterator_dtor,
	ExcimerLog_iterator_valid,
	ExcimerLog_iterator_get_current_data,
	ExcimerLog_iterator_get_current_key,
	ExcimerLog_iterator_move_forward,
	ExcimerLog_iterator_rewind,
	ExcimerLog_iterator_invalidate_current
};

static const zend_function_entry ExcimerLogEntry_methods[] = {
	PHP_ME(ExcimerLogEntry, __construct, arginfo_ExcimerLogEntry___construct,
		ZEND_ACC_PRIVATE | ZEND_ACC_FINAL)
	PHP_ME(ExcimerLogEntry, getTimestamp, arginfo_ExcimerLogEntry_getTimestamp, 0)
	PHP_ME(ExcimerLogEntry, getEventCount, arginfo_ExcimerLogEntry_getEventCount, 0)
	PHP_ME(ExcimerLogEntry, getTrace, arginfo_ExcimerLogEntry_getTrace, 0)
	PHP_FE_END
};

static const zend_function_entry ExcimerTimer_methods[] = {
	PHP_ME(ExcimerTimer, setEventType, arginfo_ExcimerTimer_setEventType, 0)
	PHP_ME(ExcimerTimer, setInterval, arginfo_ExcimerTimer_setInterval, 0)
	PHP_ME(ExcimerTimer, setPeriod, arginfo_ExcimerTimer_setPeriod, 0)
	PHP_ME(ExcimerTimer, setCallback, arginfo_ExcimerTimer_setCallback, 0)
	PHP_ME(ExcimerTimer, start, arginfo_ExcimerTimer_start, 0)
	PHP_ME(ExcimerTimer, stop, arginfo_ExcimerTimer_stop, 0)
	PHP_ME(ExcimerTimer, getTime, arginfo_ExcimerTimer_getTime, 0)
	PHP_FE_END
};

static const zend_function_entry excimer_functions[] = {
	PHP_FE(excimer_set_timeout, arginfo_excimer_set_timeout)
	PHP_FE_END
};
/* }}} */

static void *excimer_object_alloc_init(size_t object_size, zend_object_handlers *handlers, zend_class_entry *ce) /* {{{ */
{
#if PHP_VERSION_ID < 70300
	char *intern = ecalloc(1, object_size + zend_object_properties_size(ce));
#else
	char *intern = zend_object_alloc(object_size, ce);
#endif
	const size_t header_size = object_size - sizeof(zend_object);
	zend_object *object = (zend_object*)(intern + header_size);
	zend_object_std_init(object, ce);
	object_properties_init(object, ce);
	object->handlers = handlers;
	return intern;
}
/* }}} */

static void excimer_set_timespec(struct timespec *dest, double source) /* {{{ */
{
	double fractional, integral;
	if (source < 0) {
		dest->tv_sec = dest->tv_nsec = 0;
		return;
	}

	fractional = modf(source, &integral);
	dest->tv_sec = (time_t)integral;
	dest->tv_nsec = (long)(fractional * 1000000000.0);
	if (dest->tv_nsec >= EXCIMER_BILLION) {
		dest->tv_nsec -= EXCIMER_BILLION;
		dest->tv_sec ++;
	}
}
/* }}} */

static inline uint64_t excimer_timespec_to_ns(struct timespec *ts)
{
	return (uint64_t)ts->tv_nsec + (uint64_t)ts->tv_sec * EXCIMER_BILLION;
}

static inline double excimer_timespec_to_double(struct timespec *ts)
{
	return excimer_timespec_to_ns(ts) * 1e-9;
}

static inline void* excimer_check_object(zend_object *object, size_t offset, const zend_object_handlers *handlers)
{
	if (object->handlers != handlers) {
		return NULL;
	} else {
		return (void*)((char*)object - offset);
	}
}

/* {{{ PHP_MINIT_FUNCTION
 */
static PHP_MINIT_FUNCTION(excimer)
{
	zend_class_entry ce;

	REGISTER_LONG_CONSTANT("EXCIMER_REAL", EXCIMER_REAL, CONST_CS | CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("EXCIMER_CPU", EXCIMER_CPU, CONST_CS | CONST_PERSISTENT);

#define REGISTER_EXCIMER_CLASS(class_name) \
	INIT_CLASS_ENTRY(ce, #class_name, class_name ## _methods); \
	class_name ## _ce = zend_register_internal_class(&ce); \
	class_name ## _ce->create_object = class_name ## _new; \
	memcpy(&class_name ## _handlers, zend_get_std_object_handlers(), \
		sizeof(zend_object_handlers)); \
	class_name ## _handlers.offset = XtOffsetOf(class_name ## _obj, std); \
	class_name ## _handlers.free_obj = class_name ## _free_object;

	REGISTER_EXCIMER_CLASS(ExcimerProfiler);
	ExcimerProfiler_handlers.dtor_obj = ExcimerProfiler_dtor;

	REGISTER_EXCIMER_CLASS(ExcimerLog);
	ExcimerLog_ce->get_iterator = ExcimerLog_get_iterator;
	ExcimerLog_handlers.count_elements = ExcimerLog_count_elements;

	zend_class_implements(ExcimerLog_ce, 1, zend_ce_iterator);
#if PHP_VERSION_ID >= 70200
	zend_class_implements(ExcimerLog_ce, 1, zend_ce_countable);
	zend_class_implements(ExcimerLog_ce, 1, zend_ce_arrayaccess);
#elif defined(HAVE_SPL)
	zend_class_implements(ExcimerLog_ce, 1, spl_ce_Countable);
	zend_class_implements(ExcimerLog_ce, 1, spl_ce_ArrayAccess);
#endif

	REGISTER_EXCIMER_CLASS(ExcimerLogEntry);
	REGISTER_EXCIMER_CLASS(ExcimerTimer);

#undef REGISTER_EXCIMER_CLASS

	excimer_timer_module_init();

	return SUCCESS;
}
/* }}} */

/* {{{ PHP_MSHUTDOWN_FUNCTION
 */
static PHP_MSHUTDOWN_FUNCTION(excimer)
{
	excimer_timer_module_shutdown();
	return SUCCESS;
}
/* }}} */

/* {{{ PHP_RINIT_FUNCTION
 */
static PHP_RINIT_FUNCTION(excimer)
{
	excimer_timer_thread_init();
	return SUCCESS;
}
/* }}} */

/* {{{ ZEND_MODULE_POST_ZEND_DEACTIVATE_D */
static ZEND_MODULE_POST_ZEND_DEACTIVATE_D(excimer)
{
	excimer_timer_thread_shutdown();
	return SUCCESS;
}
/* }}} */

/* {{{ PHP_MINFO_FUNCTION
 */
static PHP_MINFO_FUNCTION(excimer)
{
	php_info_print_table_start();
	php_info_print_table_header(2, "excimer support", "enabled");
	php_info_print_table_row(2, "excimer version", PHP_EXCIMER_VERSION);
	php_info_print_table_end();
}
/* }}} */

static zend_object *ExcimerProfiler_new(zend_class_entry *ce) /* {{{ */
{
	ExcimerProfiler_obj *profiler = EXCIMER_NEW_OBJECT(ExcimerProfiler, ce);
	ExcimerLog_obj *log_obj;
	struct timespec now_ts;
	double initial;

	clock_gettime(CLOCK_MONOTONIC, &now_ts);

	object_init_ex(&profiler->z_log, ExcimerLog_ce);
	log_obj = EXCIMER_OBJ_Z(ExcimerLog, profiler->z_log);
	log_obj->log.max_depth = 0;
	log_obj->log.epoch = excimer_timespec_to_ns(&now_ts);

	ZVAL_NULL(&profiler->z_callback);
	profiler->event_type = EXCIMER_REAL;

	// Stagger start time
	initial = php_mt_rand() * EXCIMER_DEFAULT_PERIOD / UINT32_MAX;
	excimer_set_timespec(&profiler->initial, initial);
	excimer_set_timespec(&profiler->period, EXCIMER_DEFAULT_PERIOD);
	log_obj->log.period = EXCIMER_DEFAULT_PERIOD * EXCIMER_BILLION;

	return &profiler->std;
}
/* }}} */

static void ExcimerProfiler_free_object(zend_object *object) /* {{{ */
{
	ExcimerProfiler_obj *profiler = EXCIMER_OBJ(ExcimerProfiler, object);

	if (profiler->timer.is_valid) {
		excimer_timer_destroy(&profiler->timer);
	}
	zval_ptr_dtor(&profiler->z_log);
	ZVAL_UNDEF(&profiler->z_log);
	zval_ptr_dtor(&profiler->z_callback);
	ZVAL_UNDEF(&profiler->z_callback);
	zend_object_std_dtor(object);
}
/* }}} */

static void ExcimerProfiler_dtor(zend_object *object) /* {{{ */
{
	ExcimerProfiler_obj *profiler = EXCIMER_OBJ(ExcimerProfiler, object);
	ExcimerLog_obj *log_obj = EXCIMER_OBJ_Z(ExcimerLog, profiler->z_log);
	zval z_old_log;

	if (log_obj->log.entries_size) {
		ExcimerProfiler_flush(profiler, &z_old_log);
		zval_ptr_dtor(&z_old_log);
	}
}
/* }}} */

/* {{{ proto void ExcimerProfiler::setPeriod(float period)
 */
static PHP_METHOD(ExcimerProfiler, setPeriod)
{
	double period, initial;
	ExcimerProfiler_obj *profiler = EXCIMER_OBJ_ZP(ExcimerProfiler, getThis());

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_DOUBLE(period)
	ZEND_PARSE_PARAMETERS_END();

	// Stagger start time
	initial = php_mt_rand() * period / UINT32_MAX;

	excimer_set_timespec(&profiler->period, period);
	excimer_set_timespec(&profiler->initial, initial);

	ExcimerLog_obj *log = EXCIMER_OBJ_ZP(ExcimerLog, &profiler->z_log);
	log->log.period = period * EXCIMER_BILLION;
}
/* }}} */

/* {{{ proto void ExcimerProfiler::setEventType(int event_type)
 */
static PHP_METHOD(ExcimerProfiler, setEventType)
{
	zend_long event_type;
	ExcimerProfiler_obj *profiler = EXCIMER_OBJ_ZP(ExcimerProfiler, getThis());

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_LONG(event_type)
	ZEND_PARSE_PARAMETERS_END();

	if (event_type != EXCIMER_CPU && event_type != EXCIMER_REAL) {
		php_error_docref(NULL, E_WARNING, "Invalid event type");
		return;
	}

	profiler->event_type = event_type;
}
/* }}} */

/* {{{ proto void ExcimerProfiler::setMaxDepth(int max_depth)
 */
static PHP_METHOD(ExcimerProfiler, setMaxDepth)
{
	zend_long max_depth;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_LONG(max_depth)
	ZEND_PARSE_PARAMETERS_END();

	ExcimerProfiler_obj *profiler = EXCIMER_OBJ_ZP(ExcimerProfiler, getThis());
	ExcimerLog_obj *log_obj = EXCIMER_OBJ_ZP(ExcimerLog, &profiler->z_log);
	excimer_log_set_max_depth(&log_obj->log, max_depth);
}
/* }}} */

/* {{{ proto void ExcimerProfiler::setFlushCallback(callable callback, mixed max_samples)
 */
static PHP_METHOD(ExcimerProfiler, setFlushCallback)
{
	zval *z_callback;
	zend_long max_samples;
	ExcimerProfiler_obj *profiler = EXCIMER_OBJ_ZP(ExcimerProfiler, getThis());
	char *is_callable_error;

	ZEND_PARSE_PARAMETERS_START(2, 2)
		Z_PARAM_ZVAL(z_callback)
		Z_PARAM_LONG(max_samples)
	ZEND_PARSE_PARAMETERS_END();

	if (!zend_is_callable_ex(z_callback, NULL, 0, NULL, NULL, &is_callable_error)) {
		php_error_docref(NULL, E_WARNING, "flush callback is not callable: %s",
				is_callable_error);
		return;
	}

	ZVAL_COPY(&profiler->z_callback, z_callback);
	profiler->max_samples = max_samples;
}
/* }}} */

/* {{{ proto void ExcimerProfiler::clearFlushCallback()
 */
static PHP_METHOD(ExcimerProfiler, clearFlushCallback)
{
	ExcimerProfiler_obj *profiler = EXCIMER_OBJ_ZP(ExcimerProfiler, getThis());
	zval_ptr_dtor(&profiler->z_callback);
	ZVAL_NULL(&profiler->z_callback);
	profiler->max_samples = 0;
}
/* }}} */

/* {{{ proto void ExcimerProfiler::start()
 */
static PHP_METHOD(ExcimerProfiler, start)
{
	ExcimerProfiler_obj *profiler = EXCIMER_OBJ_ZP(ExcimerProfiler, getThis());

	ZEND_PARSE_PARAMETERS_START(0, 0);
	ZEND_PARSE_PARAMETERS_END();

	if (profiler->timer.is_running) {
		ExcimerProfiler_stop(profiler);
	}
	ExcimerProfiler_start(profiler);
}
/* }}} */

/* {{{ proto void ExcimerProfiler::stop()
 */
static PHP_METHOD(ExcimerProfiler, stop)
{
	ExcimerProfiler_obj *profiler = EXCIMER_OBJ_ZP(ExcimerProfiler, getThis());

	ZEND_PARSE_PARAMETERS_START(0, 0);
	ZEND_PARSE_PARAMETERS_END();

	ExcimerProfiler_stop(profiler);
}
/* }}} */

/* {{{ proto ExcimerLog ExcimerProfiler::getLog()
 */
static PHP_METHOD(ExcimerProfiler, getLog)
{
	ExcimerProfiler_obj * profiler = EXCIMER_OBJ_ZP(ExcimerProfiler, getThis());

	ZEND_PARSE_PARAMETERS_START(0, 0);
	ZEND_PARSE_PARAMETERS_END();

	RETURN_ZVAL(&profiler->z_log, 1, 0);
}
/* }}} */

/* {{{ proto ExcimerLog ExcimerProfiler::flush() */
static PHP_METHOD(ExcimerProfiler, flush)
{
	ExcimerProfiler_obj *profiler = EXCIMER_OBJ_ZP(ExcimerProfiler, getThis());

	ZEND_PARSE_PARAMETERS_START(0, 0);
	ZEND_PARSE_PARAMETERS_END();

	ExcimerProfiler_flush(profiler, return_value);
}
/* }}} */

static void ExcimerProfiler_start(ExcimerProfiler_obj *profiler) /* {{{ */
{
	if (profiler->timer.is_valid) {
		excimer_timer_destroy(&profiler->timer);
	}
	if (excimer_timer_init(&profiler->timer,
		profiler->event_type,
		ExcimerProfiler_event,
		(void*)profiler) == FAILURE)
	{
		/* Error message already sent */
		return;
	}
	excimer_timer_start(&profiler->timer,
			&profiler->period,
			&profiler->initial);
}
/* }}} */

static void ExcimerProfiler_stop(ExcimerProfiler_obj *profiler) /* {{{ */
{
	if (profiler->timer.is_valid) {
		excimer_timer_destroy(&profiler->timer);
	}
}
/* }}} */

static void ExcimerProfiler_event(zend_long event_count, void *user_data) /* {{{ */
{
	uint64_t now_ns;
	struct timespec now_ts;
	ExcimerProfiler_obj *profiler = (ExcimerProfiler_obj*)user_data;
	ExcimerLog_obj *log_obj = EXCIMER_OBJ_ZP(ExcimerLog, &profiler->z_log);
	excimer_log *log;

	log = &log_obj->log;

	clock_gettime(CLOCK_MONOTONIC, &now_ts);
	now_ns = excimer_timespec_to_ns(&now_ts);

	excimer_log_add(log, EG(current_execute_data), event_count, now_ns);

	if (profiler->max_samples && log->entries_size >= profiler->max_samples) {
		zval z_old_log;
		ExcimerProfiler_flush(profiler, &z_old_log);
		zval_ptr_dtor(&z_old_log);
	}
}
/* }}} */

static void ExcimerProfiler_flush(ExcimerProfiler_obj *profiler, zval *zp_old_log) /* {{{ */
{
	ExcimerLog_obj *log_obj = EXCIMER_OBJ_ZP(ExcimerLog, &profiler->z_log);
	excimer_log *log = &log_obj->log;

	zend_fcall_info fci;
	zend_fcall_info_cache fcc;
	char *is_callable_error = NULL;
	zval retval;
	int status;

	/* Rotate the log */
	ZVAL_COPY(zp_old_log, &profiler->z_log);
	Z_DELREF(profiler->z_log);
	object_init_ex(&profiler->z_log, ExcimerLog_ce);
	excimer_log_copy_options(&EXCIMER_OBJ_ZP(ExcimerLog, &profiler->z_log)->log, log);

	if (Z_ISNULL(profiler->z_callback)) {
		return;
	}

	/* Prepare to call the flush callback */
	if (zend_fcall_info_init(&profiler->z_callback, 0, &fci, &fcc, NULL,
		&is_callable_error) != SUCCESS)
	{
		php_error(E_WARNING, "ExcimerProfiler callback is not callable (during event): %s",
			is_callable_error);
		ExcimerProfiler_stop(profiler);
		return;
	}

	fci.retval = &retval;

	/* Call it */
	zend_fcall_info_argn(&fci, 1, zp_old_log);
	status = zend_call_function(&fci, &fcc);
	if (status == SUCCESS) {
		zval_ptr_dtor(&retval);
	}
	zend_fcall_info_args_clear(&fci, 1);
}
/* }}} */

static zend_object *ExcimerLog_new(zend_class_entry *ce) /* {{{ */
{
	ExcimerLog_obj *log_obj = EXCIMER_NEW_OBJECT(ExcimerLog, ce);
	excimer_log_init(&log_obj->log);
	/* Lazy-initialise z_current to minimise circular references */
	ZVAL_NULL(&log_obj->z_current);
	log_obj->iter_entry_index = 0;
	return &log_obj->std;
}
/* }}} */

static void ExcimerLog_free_object(zend_object *object) /* {{{ */
{
	ExcimerLog_obj *log_obj = EXCIMER_OBJ(ExcimerLog, object);
	excimer_log_destroy(&log_obj->log);
	zval_ptr_dtor(&log_obj->z_current);
	zend_object_std_dtor(object);
}
/* }}} */

/* {{{ ExcimerLog_get_iterator */
static zend_object_iterator *ExcimerLog_get_iterator(
	zend_class_entry *ce, zval *zp_log, int by_ref)
{
	ExcimerLog_iterator *iterator;

	if (by_ref) {
		zend_throw_exception(spl_ce_RuntimeException, "An iterator cannot be used with foreach by reference", 0);
		return NULL;
	}

	iterator = emalloc(sizeof(ExcimerLog_iterator));
	zend_iterator_init((zend_object_iterator*)iterator);

	ZVAL_COPY(&iterator->intern.it.data, zp_log);

	iterator->intern.it.funcs = &ExcimerLog_iterator_funcs;
	iterator->intern.ce = ce;
	iterator->index = 0;
	ZVAL_NULL(&iterator->z_current);

	return &iterator->intern.it;
}
/* }}} */

static void ExcimerLog_iterator_dtor(zend_object_iterator *iter) /* {{{ */
{
	ExcimerLog_iterator *iterator = (ExcimerLog_iterator*)iter;

	zval_ptr_dtor(&iterator->z_current);
	ZVAL_UNDEF(&iterator->z_current);
	zval_ptr_dtor(&iterator->intern.it.data);
	ZVAL_UNDEF(&iterator->intern.it.data);
}
/* }}} */

static int ExcimerLog_iterator_valid(zend_object_iterator *iter) /* {{{ */
{
	ExcimerLog_iterator *iterator = (ExcimerLog_iterator*)iter;
	ExcimerLog_obj *log_obj = EXCIMER_OBJ_Z(ExcimerLog, iterator->intern.it.data);

	if (iterator->index < log_obj->log.entries_size) {
		return SUCCESS;
	} else {
		return FAILURE;
	}
}
/* }}} */

static zval *ExcimerLog_iterator_get_current_data(zend_object_iterator *iter) /* {{{ */
{
	ExcimerLog_iterator *iterator = (ExcimerLog_iterator*)iter;
	ExcimerLog_obj *log_obj = EXCIMER_OBJ_Z(ExcimerLog, iterator->intern.it.data);

	if (Z_ISNULL(iterator->z_current)) {
		if (iterator->index < log_obj->log.entries_size) {
			ExcimerLog_init_entry(&iterator->z_current, &iterator->intern.it.data, iterator->index);
		} else {
			return NULL;
		}
	}
	return &iterator->z_current;
}
/* }}} */

static void ExcimerLog_iterator_get_current_key(zend_object_iterator *iter, zval *key) /* {{{ */
{
	ExcimerLog_iterator *iterator = (ExcimerLog_iterator*)iter;
	ExcimerLog_obj *log_obj = EXCIMER_OBJ_Z(ExcimerLog, iterator->intern.it.data);

	if (iterator->index < log_obj->log.entries_size) {
		ZVAL_LONG(key, iterator->index);
	} else {
		ZVAL_NULL(key);
	}
}
/* }}} */

static void ExcimerLog_iterator_move_forward(zend_object_iterator *iter) /* {{{ */
{
	ExcimerLog_iterator *iterator = (ExcimerLog_iterator*)iter;
	ExcimerLog_obj *log_obj = EXCIMER_OBJ_Z(ExcimerLog, iterator->intern.it.data);

	zval_ptr_dtor(&iterator->z_current);
	ZVAL_NULL(&iterator->z_current);

	if (iterator->index < log_obj->log.entries_size) {
		iterator->index++;
	}
}
/* }}} */

static void ExcimerLog_iterator_rewind(zend_object_iterator *iter) /* {{{ */
{
	ExcimerLog_iterator *iterator = (ExcimerLog_iterator*)iter;

	zval_ptr_dtor(&iterator->z_current);
	ZVAL_NULL(&iterator->z_current);
	iterator->index = 0;
}
/* }}} */

static void ExcimerLog_iterator_invalidate_current(zend_object_iterator *iter) /* {{{ */
{
	ExcimerLog_iterator *iterator = (ExcimerLog_iterator*)iter;

	zval_ptr_dtor(&iterator->z_current);
	ZVAL_NULL(&iterator->z_current);
}
/* }}} */

static void ExcimerLog_init_entry(zval *zp_dest, zval *zp_log, zend_long index) /* {{{ */
{
	ExcimerLog_obj *log_obj = EXCIMER_OBJ_ZP(ExcimerLog, zp_log);
	excimer_log_entry *entry = excimer_log_get_entry(&log_obj->log, index);
	ExcimerLogEntry_obj *entry_obj;

	if (entry) {
		object_init_ex(zp_dest, ExcimerLogEntry_ce);
		entry_obj = EXCIMER_OBJ_ZP(ExcimerLogEntry, zp_dest);
		ZVAL_COPY(&entry_obj->z_log, zp_log);
		entry_obj->index = index;
	} else {
		ZVAL_NULL(zp_dest);
	}
}
/* }}} */

#if PHP_VERSION_ID < 80000
static int ExcimerLog_count_elements(zval *zp_log, zend_long *lp_count) /* {{{ */
{
	ExcimerLog_obj *log_obj = EXCIMER_OBJ_ZP(ExcimerLog, zp_log);
	*lp_count = log_obj->log.entries_size;
	return SUCCESS;
}
/* }}} */
#else
static int ExcimerLog_count_elements(zend_object *object, zend_long *lp_count) /* {{{ */
{
	ExcimerLog_obj *log_obj = EXCIMER_OBJ(ExcimerLog, object);
	*lp_count = log_obj->log.entries_size;
	return SUCCESS;
}
/* }}} */
#endif

/* {{{ proto void ExcimerLog::__construct()
 */
static PHP_METHOD(ExcimerLog, __construct)
{
	php_error_docref(NULL, E_ERROR, "ExcimerLog cannot be constructed directly");
}
/* }}} */

/* {{{ proto string ExcimerLog::formatCollapsed()
 */
static PHP_METHOD(ExcimerLog, formatCollapsed)
{
	ExcimerLog_obj *log_obj = EXCIMER_OBJ_ZP(ExcimerLog, getThis());
	RETURN_STR(excimer_log_format_collapsed(&log_obj->log));
}
/* }}} */

/* {{{ proto string ExcimerLog::getSpeedscopeData()
 */
static PHP_METHOD(ExcimerLog, getSpeedscopeData)
{
	ExcimerLog_obj *log_obj = EXCIMER_OBJ_ZP(ExcimerLog, getThis());
	excimer_log_get_speedscope_data(&log_obj->log, return_value);
}
/* }}} */

/* {{{ proto string ExcimerLog::aggregateByFunction()
 */
static PHP_METHOD(ExcimerLog, aggregateByFunction)
{
	ExcimerLog_obj *log_obj = EXCIMER_OBJ_ZP(ExcimerLog, getThis());
	RETURN_ARR(excimer_log_aggr_by_func(&log_obj->log));
}
/* }}} */

/* {{{ proto string ExcimerLog::getEventCount()
 */
static PHP_METHOD(ExcimerLog, getEventCount)
{
	ExcimerLog_obj *log_obj = EXCIMER_OBJ_ZP(ExcimerLog, getThis());
	RETURN_LONG(log_obj->log.event_count);
}
/* }}} */

/* {{{ proto array ExcimerLog::current()
 */
static PHP_METHOD(ExcimerLog, current)
{
	ExcimerLog_obj * log_obj = EXCIMER_OBJ_ZP(ExcimerLog, getThis());

	ZEND_PARSE_PARAMETERS_START(0, 0);
	ZEND_PARSE_PARAMETERS_END();

	if (Z_ISNULL(log_obj->z_current) && log_obj->iter_entry_index < log_obj->log.entries_size) {
		ExcimerLog_init_entry(&log_obj->z_current, getThis(), log_obj->iter_entry_index);
	}

	RETURN_ZVAL(&log_obj->z_current, 1, 0);
}
/* }}} */

/* {{{ proto int ExcimerLog::key()
 */
static PHP_METHOD(ExcimerLog, key)
{
	ExcimerLog_obj * log_obj = EXCIMER_OBJ_ZP(ExcimerLog, getThis());

	ZEND_PARSE_PARAMETERS_START(0, 0);
	ZEND_PARSE_PARAMETERS_END();

	if (log_obj->iter_entry_index < log_obj->log.entries_size) {
		RETURN_LONG(log_obj->iter_entry_index);
	} else {
		RETURN_NULL();
	}
}
/* }}} */

/* {{{ proto void ExcimerLog::next()
 */
static PHP_METHOD(ExcimerLog, next)
{
	ExcimerLog_obj * log_obj = EXCIMER_OBJ_ZP(ExcimerLog, getThis());

	ZEND_PARSE_PARAMETERS_START(0, 0);
	ZEND_PARSE_PARAMETERS_END();

	zval_ptr_dtor(&log_obj->z_current);
	ZVAL_NULL(&log_obj->z_current);
	if (log_obj->iter_entry_index < log_obj->log.entries_size) {
		log_obj->iter_entry_index++;
	}
}
/* }}} */

/* {{{ proto void ExcimerLog::rewind()
 */
static PHP_METHOD(ExcimerLog, rewind)
{
	ExcimerLog_obj * log_obj = EXCIMER_OBJ_ZP(ExcimerLog, getThis());

	ZEND_PARSE_PARAMETERS_START(0, 0);
	ZEND_PARSE_PARAMETERS_END();

	log_obj->iter_entry_index = 0;
	zval_ptr_dtor(&log_obj->z_current);
	ZVAL_NULL(&log_obj->z_current);
}
/* }}} */

/* {{{ proto bool ExcimerLog::valid()
 */
static PHP_METHOD(ExcimerLog, valid)
{
	ExcimerLog_obj * log_obj = EXCIMER_OBJ_ZP(ExcimerLog, getThis());

	ZEND_PARSE_PARAMETERS_START(0, 0);
	ZEND_PARSE_PARAMETERS_END();

	if (log_obj->iter_entry_index < log_obj->log.entries_size) {
		RETURN_TRUE;
	} else {
		RETURN_FALSE;
	}
}
/* }}} */

/* {{{ proto int ExcimerLog::count()
 */
static PHP_METHOD(ExcimerLog, count)
{
	ExcimerLog_obj *log_obj = EXCIMER_OBJ_ZP(ExcimerLog, getThis());

	ZEND_PARSE_PARAMETERS_START(0, 0);
	ZEND_PARSE_PARAMETERS_END();

	RETURN_LONG(log_obj->log.entries_size);
}
/* }}} */

/* {{{ proto bool ExcimerLog::offsetExists(mixed offset) */
static PHP_METHOD(ExcimerLog, offsetExists)
{
	ExcimerLog_obj *log_obj = EXCIMER_OBJ_ZP(ExcimerLog, getThis());
	zend_long offset;

	ZEND_PARSE_PARAMETERS_START(1, 1);
		Z_PARAM_LONG(offset)
	ZEND_PARSE_PARAMETERS_END();

	if (offset >= 0 && offset < log_obj->log.entries_size) {
		RETURN_TRUE;
	} else {
		RETURN_FALSE;
	}
}
/* }}} */

/* {{{ proto mixed ExcimerLog::offsetGet(mixed offset) */
static PHP_METHOD(ExcimerLog, offsetGet)
{
	ExcimerLog_obj *log_obj = EXCIMER_OBJ_ZP(ExcimerLog, getThis());
	zend_long offset;

	ZEND_PARSE_PARAMETERS_START(1, 1);
		Z_PARAM_LONG(offset)
	ZEND_PARSE_PARAMETERS_END();

	if (offset < 0 || offset >= log_obj->log.entries_size) {
		RETURN_NULL();
	}

	ExcimerLog_init_entry(return_value, getThis(), offset);
}
/* }}} */

/* {{{ proto void ExcimerLog::offsetSet(mixed offset, mixed value) */
static PHP_METHOD(ExcimerLog, offsetSet)
{
	php_error_docref(NULL, E_WARNING, "ExcimerLog cannot be modified");
}
/* }}} */

/* {{{ proto void ExcimerLog::offsetUnset(mixed offset) */
static PHP_METHOD(ExcimerLog, offsetUnset)
{
	php_error_docref(NULL, E_WARNING, "ExcimerLog cannot be modified");
}
/* }}} */

static zend_object *ExcimerLogEntry_new(zend_class_entry *ce) /* {{{ */
{
	ExcimerLogEntry_obj *entry_obj = EXCIMER_NEW_OBJECT(ExcimerLogEntry, ce);
	ZVAL_NULL(&entry_obj->z_log);
	entry_obj->index = 0;
	return &entry_obj->std;
}
/* }}} */

static void ExcimerLogEntry_free_object(zend_object *object) /* {{{ */
{
	ExcimerLogEntry_obj *entry_obj = EXCIMER_OBJ(ExcimerLogEntry, object);
	zval_ptr_dtor(&entry_obj->z_log);
	ZVAL_UNDEF(&entry_obj->z_log);
	zend_object_std_dtor(object);
}
/* }}} */

/* {{{ proto void ExcimerLogEntry::__construct()
 */
static PHP_METHOD(ExcimerLogEntry, __construct)
{
	php_error_docref(NULL, E_ERROR, "ExcimerLogEntry cannot be constructed directly");
}
/* }}} */

/* {{{ proto float ExcimerLogEntry::getTimestamp()
 */
static PHP_METHOD(ExcimerLogEntry, getTimestamp)
{
	ExcimerLogEntry_obj *entry_obj = EXCIMER_OBJ_ZP(ExcimerLogEntry, getThis());
	ExcimerLog_obj *log_obj = EXCIMER_OBJ_ZP(ExcimerLog, &entry_obj->z_log);
	excimer_log_entry *entry = excimer_log_get_entry(&log_obj->log, entry_obj->index);

	ZEND_PARSE_PARAMETERS_START(0, 0);
	ZEND_PARSE_PARAMETERS_END();

	RETURN_DOUBLE((entry->timestamp - log_obj->log.epoch) / 1e9);
}
/* }}} */

/* {{{ proto float ExcimerLogEntry::getEventCount()
 */
static PHP_METHOD(ExcimerLogEntry, getEventCount)
{
	ExcimerLogEntry_obj *entry_obj = EXCIMER_OBJ_ZP(ExcimerLogEntry, getThis());
	ExcimerLog_obj *log_obj = EXCIMER_OBJ_ZP(ExcimerLog, &entry_obj->z_log);
	excimer_log_entry *entry = excimer_log_get_entry(&log_obj->log, entry_obj->index);

	ZEND_PARSE_PARAMETERS_START(0, 0);
	ZEND_PARSE_PARAMETERS_END();

	RETURN_LONG(entry->event_count);
}
/* }}} */

/* {{{ proto array ExcimerLogEntry::getTrace()
 */
static PHP_METHOD(ExcimerLogEntry, getTrace)
{
	ExcimerLogEntry_obj *entry_obj = EXCIMER_OBJ_ZP(ExcimerLogEntry, getThis());
	ExcimerLog_obj *log_obj = EXCIMER_OBJ_ZP(ExcimerLog, &entry_obj->z_log);
	excimer_log_entry *entry = excimer_log_get_entry(&log_obj->log, entry_obj->index);

	ZEND_PARSE_PARAMETERS_START(0, 0);
	ZEND_PARSE_PARAMETERS_END();

	RETURN_ARR(excimer_log_trace_to_array(&log_obj->log, entry->frame_index));
}
/* }}} */

static zend_object *ExcimerTimer_new(zend_class_entry *ce) /* {{{ */
{
	ExcimerTimer_obj *timer_obj = EXCIMER_NEW_OBJECT(ExcimerTimer, ce);
	ZVAL_UNDEF(&timer_obj->z_callback);
	timer_obj->event_type = EXCIMER_REAL;
	return &timer_obj->std;
}
/* }}} */

static void ExcimerTimer_free_object(zend_object *object) /* {{{ */
{
	ExcimerTimer_obj *timer_obj = EXCIMER_OBJ(ExcimerTimer, object);
	if (timer_obj->timer.is_valid) {
		excimer_timer_destroy(&timer_obj->timer);
	}
	zval_ptr_dtor(&timer_obj->z_callback);
	ZVAL_UNDEF(&timer_obj->z_callback);
}
/* }}} */

/* {{{ proto void ExcimerTimer::setEventType(int event_type)
 */
static PHP_METHOD(ExcimerTimer, setEventType)
{
	ExcimerTimer_obj *timer_obj = EXCIMER_OBJ_ZP(ExcimerTimer, getThis());
	zend_long event_type;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_LONG(event_type)
	ZEND_PARSE_PARAMETERS_END();

	if (event_type != EXCIMER_CPU && event_type != EXCIMER_REAL) {
		php_error_docref(NULL, E_WARNING, "Invalid event type");
		return;
	}

	timer_obj->event_type = event_type;
}
/* }}} */

/* {{{ proto void ExcimerTimer::setInterval(float interval)
 */
static PHP_METHOD(ExcimerTimer, setInterval)
{
	ExcimerTimer_obj *timer_obj = EXCIMER_OBJ_ZP(ExcimerTimer, getThis());
	double initial;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_DOUBLE(initial)
	ZEND_PARSE_PARAMETERS_END();

	excimer_set_timespec(&timer_obj->initial, initial);
}
/* }}} */

/* {{{ proto void ExcimerTimer::setPeriod(float period)
 */
static PHP_METHOD(ExcimerTimer, setPeriod)
{
	ExcimerTimer_obj *timer_obj = EXCIMER_OBJ_ZP(ExcimerTimer, getThis());
	double period;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_DOUBLE(period)
	ZEND_PARSE_PARAMETERS_END();

	excimer_set_timespec(&timer_obj->period, period);
}
/* }}} */

/* {{{ proto void ExcimerTimer::setCallback(callback callback)
 */
static PHP_METHOD(ExcimerTimer, setCallback)
{
	ExcimerTimer_obj *timer_obj = EXCIMER_OBJ_ZP(ExcimerTimer, getThis());
	zval *zp_callback;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_ZVAL(zp_callback)
	ZEND_PARSE_PARAMETERS_END();

	if (Z_TYPE_P(zp_callback) == IS_NULL) {
		zval_ptr_dtor(&timer_obj->z_callback);
		ZVAL_NULL(&timer_obj->z_callback);
	} else {
		ExcimerTimer_set_callback(timer_obj, zp_callback);
	}
}
/* }}} */

static int ExcimerTimer_set_callback(ExcimerTimer_obj *timer_obj, zval *zp_callback) /* {{{ */
{
	char *is_callable_error;

	if (!zend_is_callable_ex(zp_callback, NULL, 0, NULL, NULL, &is_callable_error)) {
		php_error_docref(NULL, E_WARNING, "timer callback is not callable: %s",
				is_callable_error);
		return FAILURE;
	}

	zval_ptr_dtor(&timer_obj->z_callback);
	ZVAL_COPY(&timer_obj->z_callback, zp_callback);
	return SUCCESS;
}
/* }}} */

/* {{{ proto void ExcimerTimer::start()
 */
static PHP_METHOD(ExcimerTimer, start)
{
	ExcimerTimer_obj *timer_obj = EXCIMER_OBJ_ZP(ExcimerTimer, getThis());

	ZEND_PARSE_PARAMETERS_START(0, 0);
	ZEND_PARSE_PARAMETERS_END();

	if (timer_obj->timer.is_running) {
		ExcimerTimer_stop(timer_obj);
	}
	ExcimerTimer_start(timer_obj);
}
/* }}} */

/* {{{ proto void ExcimerTimer::stop()
 */
static PHP_METHOD(ExcimerTimer, stop)
{
	ExcimerTimer_obj *timer_obj = EXCIMER_OBJ_ZP(ExcimerTimer, getThis());

	ZEND_PARSE_PARAMETERS_START(0, 0);
	ZEND_PARSE_PARAMETERS_END();

	ExcimerTimer_stop(timer_obj);
}
/* }}} */

/* {{{ proto float ExcimerTimer::getTime()
 */
static PHP_METHOD(ExcimerTimer, getTime)
{
	ExcimerTimer_obj *timer_obj = EXCIMER_OBJ_ZP(ExcimerTimer, getThis());
	struct timespec ts;

	ZEND_PARSE_PARAMETERS_START(0, 0);
	ZEND_PARSE_PARAMETERS_END();

	excimer_timer_get_time(&timer_obj->timer, &ts);
	RETURN_DOUBLE(excimer_timespec_to_double(&ts));
}
/* }}} */

static void ExcimerTimer_start(ExcimerTimer_obj *timer_obj) /* {{{ */
{
	if (timer_obj->timer.is_valid) {
		excimer_timer_destroy(&timer_obj->timer);
	}
	if (excimer_timer_init(&timer_obj->timer,
		timer_obj->event_type,
		ExcimerTimer_event,
		(void*)timer_obj) == FAILURE)
	{
		/* Error message already sent */
		return;
	}
	excimer_timer_start(&timer_obj->timer,
			&timer_obj->period,
			&timer_obj->initial);
}
/* }}} */

static void ExcimerTimer_stop(ExcimerTimer_obj *timer_obj) /* {{{ */
{
	if (timer_obj->timer.is_valid) {
		excimer_timer_destroy(&timer_obj->timer);
	}
}
/* }}} */

static void ExcimerTimer_event(zend_long event_count, void *user_data) /* {{{ */
{
	ExcimerTimer_obj *timer_obj = (ExcimerTimer_obj*)user_data;
	zend_fcall_info fci;
	zend_fcall_info_cache fcc;
	zval retval;
	zval z_event_count;
	char *is_callable_error;

	if (Z_ISNULL(timer_obj->z_callback) || Z_ISUNDEF(timer_obj->z_callback)) {
		return;
	}

	if (zend_fcall_info_init(&timer_obj->z_callback, 0, &fci, &fcc, NULL,
		&is_callable_error) != SUCCESS)
	{
		php_error(E_WARNING, "ExcimerTimer callback is not callable (during event): %s",
			is_callable_error);
		ExcimerTimer_stop(timer_obj);
		return;
	}

	fci.retval = &retval;
	ZVAL_LONG(&z_event_count, event_count);

	zend_fcall_info_argn(&fci, 1, &z_event_count);
	if (zend_call_function(&fci, &fcc) == SUCCESS) {
		zval_ptr_dtor(&retval);
	}
	zend_fcall_info_args_clear(&fci, 1);
}
/* }}} */

/* {{{ proto ExcimerTimer excimer_set_timeout(callable callback, float interval)
 */
PHP_FUNCTION(excimer_set_timeout)
{
	ExcimerTimer_obj *timer_obj;
	zval * zp_callback;
	double initial;

	ZEND_PARSE_PARAMETERS_START(2, 2)
		Z_PARAM_ZVAL(zp_callback)
		Z_PARAM_DOUBLE(initial)
	ZEND_PARSE_PARAMETERS_END();

	object_init_ex(return_value, ExcimerTimer_ce);
	timer_obj = EXCIMER_OBJ_ZP(ExcimerTimer, return_value);
	if (ExcimerTimer_set_callback(timer_obj, zp_callback) == FAILURE) {
		zval_ptr_dtor(return_value);
		ZVAL_NULL(return_value);
	}

	excimer_set_timespec(&timer_obj->initial, initial);
	ExcimerTimer_start(timer_obj);
}
/* }}} */

static const zend_module_dep excimer_deps[] = {
#if PHP_VERSION_ID < 70200
	ZEND_MOD_REQUIRED("spl")
#endif
	ZEND_MOD_END
};

/* {{{ excimer_module_entry */
zend_module_entry excimer_module_entry = {
	STANDARD_MODULE_HEADER_EX,
	NULL,
	excimer_deps,
	"excimer",
	excimer_functions,
	PHP_MINIT(excimer),
	PHP_MSHUTDOWN(excimer),
	PHP_RINIT(excimer),
	NULL, /* RSHUTDOWN */
	PHP_MINFO(excimer),
	PHP_EXCIMER_VERSION,
	NO_MODULE_GLOBALS,
	ZEND_MODULE_POST_ZEND_DEACTIVATE_N(excimer),
	STANDARD_MODULE_PROPERTIES_EX
};
/* }}} */

#ifdef COMPILE_DL_EXCIMER
#ifdef ZTS
ZEND_TSRMLS_CACHE_DEFINE()
#endif
ZEND_GET_MODULE(excimer)
#endif

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */
