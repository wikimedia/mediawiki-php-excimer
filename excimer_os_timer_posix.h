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

#ifndef EXCIMER_OS_TIMER_POSIX_H
#define EXCIMER_OS_TIMER_POSIX_H

#include <signal.h>
#include <stdint.h>
#include <time.h>
#include "excimer_events.h"
#include "php.h"

/** Signature for a callback to be invoked when a timer fires. */
typedef void (excimer_os_timer_notify_function_t)(union sigval sv);

/** Represents a timer backed by a POSIX timer. */
typedef struct {
	/** ID of the Excimer timer that owns this timer */
	intptr_t id;
	/** ID of the POSIX timer backing this timer */
	timer_t os_timer_id;
	/** Pointer to a callback to be invoked when this timer fires. */
	excimer_os_timer_notify_function_t* notify_function;
} excimer_os_timer_t;

#endif
