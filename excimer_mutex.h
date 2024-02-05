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

#ifndef EXCIMER_MUTEX_H
#define EXCIMER_MUTEX_H
#include <pthread.h>

/**
 * Initialize the given mutex, raising a PHP Error in case of failure.
 * @param mutex The mutex to initialize.
 */
void excimer_mutex_init(pthread_mutex_t *mutex);

/**
 * Lock the given mutex, aborting the process in case of failure.
 * @param mutex The mutex to lock.
 */
void excimer_mutex_lock(pthread_mutex_t *mutex);

/**
 * Unlock the given mutex, aborting the process in case of failure.
 * @param mutex The mutex to unlock.
 */
void excimer_mutex_unlock(pthread_mutex_t *mutex);

/**
 * Destroy the given mutex, raising a PHP Error in case of failure.
 * @param mutex The mutex to destroy.
 */
void excimer_mutex_destroy(pthread_mutex_t *mutex);

#endif
