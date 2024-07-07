/*
 * Copyright (c) 2011, Dongsheng Song <songdongsheng@live.cn>
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef _WIN_PTHREAD_CLOCK_H_
#define _WIN_PTHREAD_CLOCK_H_   1

/**
 * @file pthread_clock.h
 * @brief POSIX Time Routines
 */

/**
 * @defgroup clock POSIX Time Routines
 * @{
 */

#include <pthread_types.h>

// #ifdef __cplusplus
// extern "C" {
// #endif

/* We have POSIX timers.  */
#ifndef _POSIX_TIMERS
#define _POSIX_TIMERS           200809L
#endif

/* The monotonic clock might be available.  */
#ifndef _POSIX_MONOTONIC_CLOCK
#define _POSIX_MONOTONIC_CLOCK  0
#endif

/* The CPU-time clocks are available.  */
#ifndef _POSIX_CPUTIME
#define _POSIX_CPUTIME          200809L
#endif

/* The Clock support in threads are available.  */
#ifndef _POSIX_THREAD_CPUTIME
#define _POSIX_THREAD_CPUTIME   200809L
#endif

#ifndef TIMER_ABSTIME
#define TIMER_ABSTIME           1
#endif

#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME              0
#endif

#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC             1
#endif

#ifndef CLOCK_PROCESS_CPUTIME_ID
#define CLOCK_PROCESS_CPUTIME_ID    2
#endif

#ifndef CLOCK_THREAD_CPUTIME_ID
#define CLOCK_THREAD_CPUTIME_ID     3
#endif

int nanosleep(const struct timespec *request, struct timespec *remain);

int clock_getres(clockid_t clock_id, struct timespec *res);
int clock_gettime(clockid_t clock_id, struct timespec *tp);
int clock_settime(clockid_t clock_id, const struct timespec *tp);
int clock_nanosleep(clockid_t clock_id, int flags,
                           const struct timespec *request,
                           struct timespec *remain);

// #ifdef __cplusplus
// }
// #endif

/** @} */

#endif /* _WIN_PTHREAD_CLOCK_H_ */