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

#include "php.h"
#include "Zend/zend_smart_str.h"
#include "php_excimer.h"
#include "excimer_log.h"

static const char excimer_log_truncated_name[] = "excimer_truncated";
static const char excimer_log_fake_filename[] = "excimer fake file";

static uint32_t excimer_log_find_or_add_frame(excimer_log *log,
		zend_execute_data *execute_data, zend_long depth);

/* {{{ Compatibility functions and macros */

#if PHP_VERSION_ID >= 70300
#define excimer_log_new_array zend_new_array
#else
static inline HashTable *excimer_log_new_array(uint32_t nSize)
{
	HashTable *ht = emalloc(sizeof(HashTable));
	zend_hash_init(ht, nSize, NULL, ZVAL_PTR_DTOR, 0);
	return ht;
}
#endif

#if PHP_VERSION_ID >= 70200
#define excimer_log_smart_str_get_len smart_str_get_len
#define excimer_log_smart_str_extract smart_str_extract
#define excimer_log_smart_str_append_printf smart_str_append_printf
#define excimer_log_known_string ZSTR_KNOWN
#else
static inline size_t excimer_log_smart_str_get_len(smart_str *str)
{
	return str->s ? ZSTR_LEN(str->s) : 0;
}

static inline zend_string *excimer_log_smart_str_extract(smart_str *str)
{
	if (str->s) {
		zend_string *res;
		smart_str_0(str);
		res = str->s;
		str->s = NULL;
		return res;
	} else {
		return ZSTR_EMPTY_ALLOC();
	}
}


static void excimer_log_smart_str_append_printf(smart_str *dest, const char *format, ...)
{
	va_list arg;
	size_t len;
	char *buf;

	va_start(arg, format);
	len = vspprintf(&buf, 0, format, arg);
	va_end(arg);
	smart_str_appendl(dest, buf, len);
	efree(buf);
}
#define excimer_log_known_string(index) CG(known_strings)[index]
#endif

#if PHP_VERSION_ID >= 80100
#define excimer_log_add_assoc_array add_assoc_array
#else
static inline void excimer_log_add_assoc_array(zval *dest, const char *key, HashTable *arr)
{
	zval z_tmp;
	ZVAL_ARR(&z_tmp, arr);
	add_assoc_zval(dest, key, &z_tmp);
}
#endif

/* }}} */

void excimer_log_init(excimer_log *log)
{
	log->entries_size = 0;
	log->entries = NULL;
	log->frames = ecalloc(1, sizeof(excimer_log_frame));
	log->frames_size = 1;
	log->reverse_frames = excimer_log_new_array(0);
	log->epoch = 0;
	log->event_count = 0;
}

void excimer_log_destroy(excimer_log *log)
{
	if (log->entries) {
		efree(log->entries);
	}
	if (log->frames) {
		int i;
		for (i = 0; i < log->frames_size; i++) {
			if (log->frames[i].filename) {
				zend_string_delref(log->frames[i].filename);
			}
			if (log->frames[i].class_name) {
				zend_string_delref(log->frames[i].class_name);
			}
			if (log->frames[i].function_name) {
				zend_string_delref(log->frames[i].function_name);
			}
		}
		efree(log->frames);
	}
	zend_hash_destroy(log->reverse_frames);
	efree(log->reverse_frames);
}

void excimer_log_set_max_depth(excimer_log *log, zend_long depth)
{
	log->max_depth = depth;
}

void excimer_log_copy_options(excimer_log *dest, excimer_log  *src)
{
	dest->max_depth = src->max_depth;
	dest->epoch = src->epoch;
	dest->period = src->period;
}

void excimer_log_add(excimer_log *log, zend_execute_data *execute_data,
	zend_long event_count, uint64_t timestamp)
{
	uint32_t frame_index = excimer_log_find_or_add_frame(log, execute_data, 0);
	excimer_log_entry *entry;

	log->entries = safe_erealloc(log->entries, log->entries_size + 1,
		sizeof(excimer_log_entry), 0);
	entry = &log->entries[log->entries_size++];
	entry->frame_index = frame_index;
	entry->event_count = event_count;
	log->event_count += event_count;
	entry->timestamp = timestamp;
}

static uint32_t excimer_log_get_truncation_marker(excimer_log *log) {
	zval* zp_index;
	zval z_new_index;
	excimer_log_frame *p_frame;
	
	zp_index = zend_hash_str_find(log->reverse_frames,
		excimer_log_truncated_name, sizeof(excimer_log_truncated_name) - 1);
	if (zp_index) {
		return excimer_safe_uint32(Z_LVAL_P(zp_index));
	}

	ZVAL_LONG(&z_new_index, log->frames_size);
	zend_hash_str_add(log->reverse_frames,
		excimer_log_truncated_name, sizeof(excimer_log_truncated_name) - 1,
		&z_new_index);
	log->frames = safe_erealloc(log->frames, log->frames_size + 1,
		sizeof(excimer_log_frame), 0);
	p_frame = &log->frames[log->frames_size++];

	p_frame->filename = zend_string_init(excimer_log_fake_filename,
		sizeof(excimer_log_fake_filename) - 1, 0);
	p_frame->lineno = 1;
	p_frame->closure_line = 0;
	p_frame->class_name = NULL;
	p_frame->function_name = zend_string_init(excimer_log_truncated_name,
		sizeof(excimer_log_truncated_name) - 1, 0);
	p_frame->prev_index = 0;

	return excimer_safe_uint32(Z_LVAL(z_new_index));
}

static uint32_t excimer_log_find_or_add_frame(excimer_log *log,
	zend_execute_data *execute_data, zend_long depth)
{
	uint32_t prev_index;
	if (!execute_data) {
		return 0;
	} else if (!execute_data->prev_execute_data) {
		prev_index = 0;
	} else if (log->max_depth && depth >= log->max_depth) {
		prev_index = excimer_log_get_truncation_marker(log);
	} else {
		prev_index = excimer_log_find_or_add_frame(log,
			execute_data->prev_execute_data, depth + 1);
	}
	if (!execute_data->func
		|| !ZEND_USER_CODE(execute_data->func->common.type))
	{
		return prev_index;
	} else {
		zend_function *func = execute_data->func;
		excimer_log_frame frame = {NULL};
		smart_str ss_key = {NULL};
		zend_string *str_key;
		zval* zp_index;

		frame.filename = func->op_array.filename;
		zend_string_addref(frame.filename);

		if (func->common.scope && func->common.scope->name) {
			frame.class_name = func->common.scope->name;
			zend_string_addref(frame.class_name);
		}

		if (func->common.function_name) {
			frame.function_name = func->common.function_name;
			zend_string_addref(frame.function_name);
		}

		if (func->op_array.fn_flags & ZEND_ACC_CLOSURE) {
			frame.closure_line = func->op_array.line_start;
		}

		frame.lineno = execute_data->opline->lineno;
		frame.prev_index = prev_index;

		/* Make a key for reverse lookup */
		smart_str_append(&ss_key, frame.filename);
		smart_str_appendc(&ss_key, '\0');
		excimer_log_smart_str_append_printf(&ss_key, "%d", frame.lineno);
		smart_str_appendc(&ss_key, '\0');
		excimer_log_smart_str_append_printf(&ss_key, "%d", frame.prev_index);
		str_key = excimer_log_smart_str_extract(&ss_key);

		/* Look for a matching frame in the reverse hashtable */
		zp_index = zend_hash_find(log->reverse_frames, str_key);
		if (zp_index) {
			zend_string_free(str_key);
			zend_string_delref(frame.filename);
			if (frame.class_name) {
				zend_string_delref(frame.class_name);
			}
			if (frame.function_name) {
				zend_string_delref(frame.function_name);
			}

			return excimer_safe_uint32(Z_LVAL_P(zp_index));
		} else {
			zval z_new_index;

			/* Create a new entry in the array and reverse hashtable */
			ZVAL_LONG(&z_new_index, log->frames_size);
			zend_hash_add(log->reverse_frames, str_key, &z_new_index);
			log->frames = safe_erealloc(log->frames, log->frames_size + 1,
				sizeof(excimer_log_frame), 0);
			memcpy(&log->frames[log->frames_size++], &frame, sizeof(excimer_log_frame));

			zend_string_delref(str_key);
			return excimer_safe_uint32(Z_LVAL(z_new_index));
		}
	}
}

zend_long excimer_log_get_size(excimer_log *log)
{
	return log->entries_size;
}

excimer_log_entry *excimer_log_get_entry(excimer_log *log, zend_long i)
{
	if (i >= 0 && i < log->entries_size) {
		return &log->entries[i];
	} else {
		return NULL;
	}
}

excimer_log_frame *excimer_log_get_frame(excimer_log *log, zend_long i)
{
	if (i > 0 && i < log->frames_size) {
		return &log->frames[i];
	} else {
		return NULL;
	}
}

static void excimer_log_append_no_spaces(smart_str *dest, zend_string *src)
{
	size_t new_len = smart_str_alloc(dest, ZSTR_LEN(src), 0);
	size_t prev_len = ZSTR_LEN(dest->s);
	size_t i;
	for (i = 0; i < ZSTR_LEN(src); i++) {
		char c = ZSTR_VAL(src)[i];
		if (c == ' ' || c == '\0') {
			c = '_';
		}
		ZSTR_VAL(dest->s)[prev_len + i] = c;
	}
	ZSTR_LEN(dest->s) = new_len;
}

static void excimer_log_append_frame_name(smart_str *ss, excimer_log_frame *frame) {
	if (frame->closure_line != 0) {
		/* Annotate anonymous functions with their source location.
		 * Example: {closure:/path/to/file.php(123)}
		 */
		smart_str_appends(ss, "{closure:");
		excimer_log_append_no_spaces(ss, frame->filename);
		excimer_log_smart_str_append_printf(ss, "(%d)}", frame->closure_line);
	} else if (frame->function_name == NULL) {
		/* For file-scope code, use the file name */
		excimer_log_append_no_spaces(ss, frame->filename);
	} else {
		if (frame->class_name) {
			excimer_log_append_no_spaces(ss, frame->class_name);
			smart_str_appends(ss, "::");
		}
		excimer_log_append_no_spaces(ss, frame->function_name);
	}
}

zend_string *excimer_log_format_collapsed(excimer_log *log)
{
	zend_long entry_index;
	zend_long frame_index;
	zval *zp_count;
	zval z_count;
	smart_str ss_out = {NULL};
	HashTable frame_counts_storage, lines_storage;
	HashTable *ht_frame_counts, *ht_lines;

	ht_frame_counts = &frame_counts_storage;
	memset(ht_frame_counts, 0, sizeof(HashTable));
	zend_hash_init(ht_frame_counts, 0, NULL, NULL, 0);

	ht_lines = &lines_storage;
	memset(ht_lines, 0, sizeof(HashTable));
	zend_hash_init(ht_lines, 0, NULL, NULL, 0);

	excimer_log_frame ** frame_ptrs = NULL;
	size_t frames_capacity = 0;
	zend_string *str_line;

	/* Collate frame counts */
	for (entry_index = 0; entry_index < log->entries_size; entry_index++) {
		excimer_log_entry *entry = excimer_log_get_entry(log, entry_index);
		zp_count = zend_hash_index_find(ht_frame_counts, entry->frame_index);
		if (!zp_count) {
			ZVAL_LONG(&z_count, 0);
			zp_count = zend_hash_index_add(ht_frame_counts, entry->frame_index, &z_count);
		}

		Z_LVAL_P(zp_count) += entry->event_count;
	}

	/* Format traces, and deduplicate frames that differ only in hidden line numbers */
	ZEND_HASH_FOREACH_NUM_KEY_VAL(ht_frame_counts, frame_index, zp_count) {
		zend_long current_frame_index = frame_index;
		zend_long num_frames = 0;
		excimer_log_frame *frame;
		zend_long i;
		int line_start = 1; /* TODO use bool when PHP 7.4 support is dropped */
		smart_str ss_line = {NULL};

		/* Build the array of frame pointers */
		while (current_frame_index) {
			frame = excimer_log_get_frame(log, current_frame_index);
			if (num_frames >= frames_capacity) {
				if (frames_capacity >= ZEND_LONG_MAX - 1) {
					/* Probably unreachable */
					zend_error_noreturn(E_ERROR, "Too many Excimer frames");
				}
				frames_capacity++;
				frame_ptrs = safe_erealloc(frame_ptrs, frames_capacity, sizeof(*frame_ptrs), 0);
			}
			frame_ptrs[num_frames++] = frame;
			current_frame_index = frame->prev_index;
		}

		/* Run through the array in reverse */
		for (i = num_frames - 1; i >= 0; i--) {
			frame = frame_ptrs[i];

			if (line_start) {
				line_start = 0;
			} else {
				smart_str_appends(&ss_line, ";");
			}
			excimer_log_append_frame_name(&ss_line, frame);
		}

		/* ht_lines[ss_line] += zp_count */
		str_line = excimer_log_smart_str_extract(&ss_line);
		zval *zp_line_count = zend_hash_find(ht_lines, str_line);
		if (!zp_line_count) {
			ZVAL_LONG(&z_count, 0);
			zp_line_count = zend_hash_add(ht_lines, str_line, &z_count);
		}
		Z_LVAL_P(zp_line_count) += Z_LVAL_P(zp_count);
	}
	ZEND_HASH_FOREACH_END();

	/* Concatenate lines */
	ZEND_HASH_FOREACH_STR_KEY_VAL(ht_lines, str_line, zp_count) {
		smart_str_append(&ss_out, str_line);
		excimer_log_smart_str_append_printf(&ss_out, " %ld\n", Z_LVAL_P(zp_count));
	}
	ZEND_HASH_FOREACH_END();

	zend_hash_destroy(ht_frame_counts);
	zend_hash_destroy(ht_lines);
	efree(frame_ptrs);
	return excimer_log_smart_str_extract(&ss_out);
}

static HashTable *excimer_log_frame_to_speedscope_array(excimer_log_frame *frame) {
	HashTable *ht_func = excimer_log_new_array(0);
	zval tmp;
	smart_str ss_name = {NULL};
	
	excimer_log_append_frame_name(&ss_name, frame);
	ZVAL_STR(&tmp, excimer_log_smart_str_extract(&ss_name));
	zend_hash_str_add(ht_func, "name", sizeof("name")-1, &tmp);

	if (frame->filename) {
		ZVAL_STR_COPY(&tmp, frame->filename);
		zend_hash_add_new(ht_func, excimer_log_known_string(ZEND_STR_FILE), &tmp);
		/* Don't include the line number since it causes speedscope to split functions */
	}
	return ht_func;
}

static zend_string *excimer_log_get_speedscope_frame_key(excimer_log_frame *frame) {
	smart_str ss = {NULL};
	
	excimer_log_append_frame_name(&ss, frame);
	smart_str_appendc(&ss, '\0');
	smart_str_append(&ss, frame->filename);
	return excimer_log_smart_str_extract(&ss);
}

static uint32_t excimer_log_count_frames(excimer_log *log, uint32_t frame_index) {
	uint32_t n = 0;
	while (frame_index) {
		n++;
		frame_index = log->frames[frame_index].prev_index;
	}
	return n;
}

void excimer_log_get_speedscope_data(excimer_log *log, zval *zp_data) {
	array_init(zp_data);
	add_assoc_string(zp_data, "$schema", "https://www.speedscope.app/file-format-schema.json");
	add_assoc_string(zp_data, "exporter", "Excimer");

	HashTable *ht_frames = excimer_log_new_array(0);
	HashTable *ht_indexes_by_key = excimer_log_new_array(0);
	zend_long *lp_frame_indexes = ecalloc(log->frames_size, sizeof(zend_long));
	zend_long i;
	zval *zp_frame_index;
	zend_string *str_key;
	zval z_tmp, *zp_tmp;

	/* Build the frames array */
	for (i = 1; i < log->frames_size; i++) {
		zend_long index;
		excimer_log_frame *frame = &log->frames[i];
		str_key = excimer_log_get_speedscope_frame_key(frame);
		zp_frame_index = zend_hash_find(ht_indexes_by_key, str_key);
		if (!zp_frame_index) {
			/* Add the frame to ht_frames */
			index = zend_hash_num_elements(ht_frames);
			ZVAL_ARR(&z_tmp, excimer_log_frame_to_speedscope_array(frame));
			zend_hash_next_index_insert_new(ht_frames, &z_tmp);
			/* Add the frame index to ht_indexes_by_key */
			ZVAL_LONG(&z_tmp, index);
			zp_frame_index = zend_hash_add_new(ht_indexes_by_key, str_key, &z_tmp);
		}
		lp_frame_indexes[i] = Z_LVAL_P(zp_frame_index);
	}

	/* zp_data["shared"] = ["frames" => ht_frames] */
	zval z_shared;
	array_init(&z_shared);
	excimer_log_add_assoc_array(&z_shared, "frames", ht_frames);
	add_assoc_zval(zp_data, "shared", &z_shared);

	/* Build the samples and weights arrays */
	HashTable *ht_samples = excimer_log_new_array(log->entries_size);
	HashTable *ht_weights = excimer_log_new_array(log->entries_size);
	uint64_t first_timestamp = 0;
	uint64_t last_timestamp = 0;
	for (i = 0; i < log->entries_size; i++) {
		excimer_log_entry *entry = &log->entries[i];
		uint32_t frame_index = entry->frame_index;

		if (i == 0) {
			first_timestamp = entry->timestamp;
		}
		last_timestamp = entry->timestamp;

		uint32_t num_frames = excimer_log_count_frames(log, frame_index);
		uint32_t j;

		/* Create the array with ZEND_HASH_FILL_PACKED. This is just a fast way
		 * to get it into the right state, with num_frames elements. */
		HashTable *ht_stack = excimer_log_new_array(num_frames);
		zend_hash_extend(ht_stack, num_frames, 1);
		ZEND_HASH_FILL_PACKED(ht_stack) {
#if PHP_VERSION_ID < 70400
			zval new_val;

			ZVAL_LONG(&new_val, 0);
			for (j = 0; j < num_frames; j++) {
				ZEND_HASH_FILL_ADD(&new_val);
			}
#else
			for (j = 0; j < num_frames; j++) {
				ZEND_HASH_FILL_SET_LONG(0);
				ZEND_HASH_FILL_NEXT();
			}
#endif
		} ZEND_HASH_FILL_END();

		/* Write the values in reverse order */
		ZEND_HASH_REVERSE_FOREACH_VAL(ht_stack, zp_tmp) {
			ZVAL_LONG(zp_tmp, lp_frame_indexes[frame_index]);
			frame_index = log->frames[frame_index].prev_index;
		}
		ZEND_HASH_FOREACH_END();

		ZVAL_ARR(&z_tmp, ht_stack);
		zend_hash_next_index_insert_new(ht_samples, &z_tmp);

		ZVAL_LONG(&z_tmp, entry->event_count * log->period);
		zend_hash_next_index_insert_new(ht_weights, &z_tmp);
	}

	/* Build the profile array */
	zval z_profile;
	array_init(&z_profile);
	add_assoc_string(&z_profile, "type", "sampled");
	add_assoc_string(&z_profile, "name", "");
	add_assoc_string(&z_profile, "unit", "nanoseconds");
	add_assoc_long(&z_profile, "startValue", 0);
	add_assoc_long(&z_profile, "endValue", last_timestamp - first_timestamp);
	excimer_log_add_assoc_array(&z_profile, "samples", ht_samples);
	excimer_log_add_assoc_array(&z_profile, "weights", ht_weights);

	/* zp_data["profiles"] = [profile] */
	zval z_profiles;
	array_init(&z_profiles);
	add_next_index_zval(&z_profiles, &z_profile);
	add_assoc_zval(zp_data, "profiles", &z_profiles);

	efree(lp_frame_indexes);
}

HashTable *excimer_log_frame_to_array(excimer_log_frame *frame) {
	HashTable *ht_func = excimer_log_new_array(0);
	zval tmp;

	if (frame->filename) {
		ZVAL_STR_COPY(&tmp, frame->filename);
		zend_hash_add_new(ht_func, excimer_log_known_string(ZEND_STR_FILE), &tmp);
		ZVAL_LONG(&tmp, frame->lineno);
		zend_hash_add_new(ht_func, excimer_log_known_string(ZEND_STR_LINE), &tmp);
	}

	if (frame->class_name) {
		ZVAL_STR_COPY(&tmp, frame->class_name);
		zend_hash_add_new(ht_func, excimer_log_known_string(ZEND_STR_CLASS), &tmp);
	}

	if (frame->function_name) {
		ZVAL_STR_COPY(&tmp, frame->function_name);
		zend_hash_add_new(ht_func, excimer_log_known_string(ZEND_STR_FUNCTION), &tmp);
	}

	if (frame->closure_line) {
		zend_string *s = zend_string_init("closure_line", sizeof("closure_line") - 1, 0);
		ZVAL_LONG(&tmp, frame->closure_line);
		zend_hash_add_new(ht_func, s, &tmp);
		zend_string_delref(s);
	}

	return ht_func;
}

HashTable *excimer_log_trace_to_array(excimer_log *log, zend_long l_frame_index)
{
	HashTable *ht_trace = excimer_log_new_array(0);
	uint32_t frame_index = excimer_safe_uint32(l_frame_index);
	while (frame_index) {
		excimer_log_frame *frame = excimer_log_get_frame(log, frame_index);
		HashTable *ht_func = excimer_log_frame_to_array(frame);
		zval tmp;

		ZVAL_ARR(&tmp, ht_func);
		zend_hash_next_index_insert(ht_trace, &tmp);

		frame_index = frame->prev_index;
	}

	return ht_trace;
}

/**
 * ht[key] += term;
 */
static void excimer_log_array_incr(HashTable *ht, zend_string *sp_key, zend_long term)
{
	zval *zp_value = zend_hash_find(ht, sp_key);
	if (!zp_value) {
		zval z_tmp;
		ZVAL_LONG(&z_tmp, term);
		zend_hash_add_new(ht, sp_key, &z_tmp);
	} else {
		Z_LVAL_P(zp_value) += term;
	}
}

#if PHP_VERSION_ID < 80000
static int excimer_log_aggr_compare(const void *a, const void *b)
{
	zval *zp_a = &((Bucket*)a)->val;
	zval *zp_b = &((Bucket*)b)->val;
#else
static int excimer_log_aggr_compare(Bucket *a, Bucket *b)
{
	zval *zp_a = &a->val;
	zval *zp_b = &b->val;
#endif

	zval *zp_a_incl = zend_hash_str_find(Z_ARRVAL_P(zp_a), "inclusive", sizeof("inclusive")-1);
	zval *zp_b_incl = zend_hash_str_find(Z_ARRVAL_P(zp_b), "inclusive", sizeof("inclusive")-1);

	return ZEND_NORMALIZE_BOOL(Z_LVAL_P(zp_b_incl) - Z_LVAL_P(zp_a_incl));
}

HashTable *excimer_log_aggr_by_func(excimer_log *log)
{
	HashTable *ht_result = excimer_log_new_array(0);
	zend_string *sp_inclusive = zend_string_init("inclusive", sizeof("inclusive")-1, 0);
	zend_string *sp_self = zend_string_init("self", sizeof("self")-1, 0);
	HashTable *ht_unique_names = excimer_log_new_array(0);
	size_t entry_index;
	zval z_zero;

	ZVAL_LONG(&z_zero, 0);

	for (entry_index = 0; entry_index < log->entries_size; entry_index++) {
		excimer_log_entry *entry = excimer_log_get_entry(log, entry_index);
		uint32_t frame_index = entry->frame_index;
		int is_top = 1;

		while (frame_index) {
			excimer_log_frame *frame = excimer_log_get_frame(log, frame_index);
			smart_str ss_name = {NULL};
			zend_string *sp_name;
			zval *zp_info;
			zval z_tmp;

			/* Make a human-readable name */
			if (frame->closure_line != 0) {
				/* Annotate anonymous functions with their source location.
				 * Example: {closure:/path/to/file.php(123)}
				 */
				smart_str_appends(&ss_name, "{closure:");
				smart_str_append(&ss_name, frame->filename);
				excimer_log_smart_str_append_printf(&ss_name, "(%d)}", frame->closure_line);
			} else if (frame->function_name == NULL) {
				/* For file-scope code, use the file name */
				smart_str_append(&ss_name, frame->filename);
			} else {
				if (frame->class_name) {
					smart_str_append(&ss_name, frame->class_name);
					smart_str_appends(&ss_name, "::");
				}
				smart_str_append(&ss_name, frame->function_name);
			}
			sp_name = excimer_log_smart_str_extract(&ss_name);

			/* If it is not in ht_result, add it, along with frame info */
			zp_info = zend_hash_find(ht_result, sp_name);
			if (!zp_info) {
				ZVAL_ARR(&z_tmp, excimer_log_frame_to_array(frame));
				zend_hash_add_new(Z_ARRVAL(z_tmp), sp_self, &z_zero);
				zend_hash_add_new(Z_ARRVAL(z_tmp), sp_inclusive, &z_zero);
				zp_info = zend_hash_add(ht_result, sp_name, &z_tmp);
			}

			/* If this is the top frame of a log entry, increment the "self" key */
			if (is_top) {
				excimer_log_array_incr(Z_ARRVAL_P(zp_info), sp_self, entry->event_count);
			}

			/* If this is the first instance of a function in an entry, i.e.
			 * counting recursive functions only once, increment the "inclusive" key */
			if (zend_hash_find(ht_unique_names, sp_name) == NULL) {
				excimer_log_array_incr(Z_ARRVAL_P(zp_info), sp_inclusive, entry->event_count);
				/* Add the function to the unique_names array */
				zend_hash_add_new(ht_unique_names, sp_name, &z_zero);
			}

			is_top = 0;
			frame_index = frame->prev_index;
			zend_string_delref(sp_name);
		}
		zend_hash_clean(ht_unique_names);
	}
	zend_hash_destroy(ht_unique_names);
	zend_string_delref(sp_self);
	zend_string_delref(sp_inclusive);

	/* Sort the result in descending order by inclusive */
	zend_hash_sort(ht_result, excimer_log_aggr_compare, 0);

	return ht_result;
}
