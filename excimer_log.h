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

#ifndef EXCIMER_LOG_H
#define EXCIMER_LOG_H

/**
 * Structure representing a unique location in the code and its backtrace
 */
typedef struct _excimer_log_frame {
	/** The filename, or may be fake e.g. "php shell code" */
	zend_string *filename;

	/** The executing line number within the filename */
	uint32_t lineno;

	/**
	 * If the function was a closure, the "start line" of its definition.
	 * Zero if the function was not a closure.
	 */
	uint32_t closure_line;

	/** The class name, or NULL if there was no class name. */
	zend_string *class_name;

	/**
	 * The function name, or NULL if there was no function name, or a
	 * fake thing like "eval()'d code".
	 */
	zend_string *function_name;

	/**
	 * The index within excimer_log.frames of the calling frame.
	 */
	uint32_t prev_index;
} excimer_log_frame;

/**
 * Structure representing a log entry
 */
typedef struct _excimer_log_entry {
	/**
	 * The index within excimer_log.frames of the frame associated with this event.
	 */
	uint32_t frame_index;

	/**
	 * The number of times the timer elapsed before the log entry was finally registered.
	 */
	zend_long event_count;

	/**
	 * The wall clock time at which the event occurred. The interpretation is
	 * caller-defined, but in Excimer it is the number of nanoseconds since boot.
	 */
	uint64_t timestamp;
} excimer_log_entry;

/**
 * Structure representing the entire log
 */
typedef struct _excimer_log {
	/** Array of log entries */
	excimer_log_entry *entries;

	/** Size of the "entries" array */
	size_t entries_size;

	/** Array of frames */
	excimer_log_frame *frames;

	/* Size of the "frames" array */
	size_t frames_size;

	/**
	 * A hashtable where the key is a unique frame identifier combining some
	 * elements of the frame object, and the value is the frame index. Used
	 * for deduplication of frames.
	 */
	HashTable *reverse_frames;

	/**
	 * The maximum stack depth of collected frames. If this is exceeded, the
	 * backtrace is truncated.
	 */
	zend_long max_depth;

	/**
	 * This is used by ExcimerProfiler to store the creation time of the
	 * ExcimerProfiler object.
	 */
	uint64_t epoch;

	/**
	 * The nominal period in nanoseconds
	 */
	uint64_t period;

	/**
	 * The sum of the event counts of all contained log entries
	 */
	zend_long event_count;
} excimer_log;

/**
 * Initialise the log object.
 *
 * @param log Valid memory location at which to place the object
 */
void excimer_log_init(excimer_log *log);

/**
 * Destroy the log object. This frees internal objects but does not free the
 * excimer_log itself.
 *
 * @param log The log object to destroy
 */
void excimer_log_destroy(excimer_log *log);

/**
 * Set the max depth
 *
 * @param log The log object
 * @param depth The new depth
 */
void excimer_log_set_max_depth(excimer_log *log, zend_long depth);

/**
 * Copy persistent options to another log. This is used during log rotation.
 *
 * @param dest The destination log object
 * @param src The source log object
 */
void excimer_log_copy_options(excimer_log *dest, excimer_log *src);

/**
 * Add a log entry
 *
 * @param log The log object
 * @param execute_data The VM state
 * @param event_count The number of times the timer expired
 * @param timestamp The timestamp to store in the log entry
 */
void excimer_log_add(excimer_log *log, zend_execute_data *execute_data,
		zend_long event_count, uint64_t timestamp);

/**
 * Get the number of entries in the log
 *
 * @param log The log object
 * @return The number of entries in the log
 */
zend_long excimer_log_get_size(excimer_log *log);

/**
 * Get a log entry
 *
 * @param log The log object
 * @param i The index of the entry
 * @return The log entry, or NULL if the index is out of range
 */
excimer_log_entry *excimer_log_get_entry(excimer_log *log, zend_long i);

/**
 * Get a frame by index
 *
 * @param log The log object
 * @param i The index
 * @return The frame, or NULL if the index is out of range
 */
excimer_log_frame *excimer_log_get_frame(excimer_log *log, zend_long i);

/**
 * Format the log in flamegraph.pl collapsed format
 *
 * @param log The log object
 * @return A new zend_string owned by the caller
 */
zend_string *excimer_log_format_collapsed(excimer_log *log);

/**
 * Get an array in speedscope format
 *
 * @param log The log object
 * @param zp_data The destination
 */
void excimer_log_get_speedscope_data(excimer_log *log, zval *zp_data);

/**
 * Aggregate the log producing self/inclusive statistics as an array
 */
HashTable *excimer_log_aggr_by_func(excimer_log *log);

/**
 * Convert a frame to a backtrace array for returning to the user
 *
 * @param log The log object
 * @param l_frame_index The frame index
 * @return  A new hashtable, owned by the caller
 */
HashTable *excimer_log_trace_to_array(excimer_log *log, zend_long l_frame_index);

#endif
