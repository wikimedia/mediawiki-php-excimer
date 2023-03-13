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

#ifndef PHP_EXCIMER_H
#define PHP_EXCIMER_H

extern zend_module_entry excimer_module_entry;
#define phpext_excimer_ptr &excimer_module_entry

#define PHP_EXCIMER_VERSION "1.1.1"

#ifdef PHP_WIN32
#	define PHP_EXCIMER_API __declspec(dllexport)
#elif defined(__GNUC__) && __GNUC__ >= 4
#	define PHP_EXCIMER_API __attribute__ ((visibility("default")))
#else
#	define PHP_EXCIMER_API
#endif

#ifdef ZTS
#include "TSRM.h"
#endif

#if defined(ZTS) && defined(COMPILE_DL_EXCIMER)
ZEND_TSRMLS_CACHE_EXTERN()
#endif

static inline uint32_t excimer_safe_uint32(zend_long i) {
	if (i < 0 || i > UINT32_MAX) {
		zend_error_noreturn(E_ERROR, "Integer out of range");
	}
	return (uint32_t)i;
}

#endif	/* PHP_EXCIMER_H */


/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */
