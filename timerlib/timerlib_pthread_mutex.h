/* Copyright 2025 Wikimedia Foundation
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

/**
 * Helper functions for dealing with pthread mutexes
 */

#define timerlib_mutex_lock(mutex) timerlib_mutex_lock_func(mutex, __func__)
#define timerlib_mutex_unlock(mutex) timerlib_mutex_unlock_func(mutex, __func__)

static void timerlib_mutex_lock_func(pthread_mutex_t *mutex, const char *func)
{
	int result = pthread_mutex_lock(mutex);
	if (result != 0) {
		timerlib_abort_func(func, "pthread_mutex_lock", result);
	}
}

static void timerlib_mutex_unlock_func(pthread_mutex_t *mutex, const char *func)
{
	int result = pthread_mutex_unlock(mutex);
	if (result != 0) {
		timerlib_abort_func(func, "pthread_mutex_unlock", result);
	}
}
