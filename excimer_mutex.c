/* Copyright 2024 Wikimedia Foundation
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

#include "excimer_mutex.h"
#include "php.h"

void excimer_mutex_init(pthread_mutex_t *mutex)
{
	int result = pthread_mutex_init(mutex, NULL);
	if (result != 0) {
		zend_error_noreturn(E_ERROR, "pthread_mutex_init(): %s", strerror(result));
	}
}

void excimer_mutex_lock(pthread_mutex_t *mutex)
{
	int result = pthread_mutex_lock(mutex);
	if (result != 0) {
		fprintf(stderr, "pthread_mutex_lock(): %s", strerror(result));
		abort();
	}
}

void excimer_mutex_unlock(pthread_mutex_t *mutex)
{
	int result = pthread_mutex_unlock(mutex);
	if (result != 0) {
		fprintf(stderr, "pthread_mutex_unlock(): %s", strerror(result));
		abort();
	}
}

void excimer_mutex_destroy(pthread_mutex_t *mutex)
{
	int result = pthread_mutex_destroy(mutex);
	if (result != 0) {
		zend_error_noreturn(E_ERROR, "pthread_mutex_destroy(): %s", strerror(result));
	}
}
