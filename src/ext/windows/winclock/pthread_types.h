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

#ifndef _WIN_PTHREAD_TYPES_H_
#define _WIN_PTHREAD_TYPES_H_   1

/**
 * @file pthread_types.h
 * @brief POSIX Thread Support Definitions
 */

/**
 * @defgroup thread_def POSIX Thread Support Definitions
 * @ingroup libpthread
 * @{
 */

#include <errno.h> /* Adding definition of EINVAL, ETIMEDOUT, ..., etc. */
#include <fcntl.h> /* Adding O_CREAT definition. */
#include <limits.h> /* Adding INT_MAX, UINT_MAX definition.*/
#include <process.h> /* Adding intptr_t, uintptr_t definition.*/
#include <time.h> /* Adding time_t definition.  */
#include <sys/types.h> /* Adding pid_t, mode_t definition.  */

#ifndef _MSC_VER
    #include <stdint.h>
    #include <inttypes.h>
#else
    #if _MSC_VER >= 1600
        #include <stdint.h>
    #else
        /* stdint.h */
        typedef signed char int8_t;
        typedef short int16_t;
        typedef int int32_t;

        typedef unsigned char uint8_t;
        typedef unsigned short uint16_t;
        typedef unsigned int uint32_t;

        #define INT8_MAX        0x7f
        #define UINT8_MAX       0xff
        #define INT16_MAX       0x7fff
        #define UINT16_MAX      0xffff
        #define INT32_MAX       0x7fffffff
        #define UINT32_MAX      0xffffffff
        #define INT64_MAX       0x7fffffffffffffff
        #define UINT64_MAX      0xffffffffffffffffU
        /*
        #define INT64_C(c)      c ## I64
        #define UINT64_C(c)     c ## UI64
         */
        #define INT64_C(x)              ((x) + (INT64_MAX - INT64_MAX))
        #define UINT64_C(x)             ((x) + (UINT64_MAX - UINT64_MAX))
    #endif /* _MSC_VER >= 1600 */

    /* inttypes.h */
    // #define PRId64      "I64d"
    // #define PRIu64      "I64u"
    // #define PRIx64      "I64x"
    // #define PRIX64      "I64X"
#endif /* _MSC_VER */

#ifndef _PID_T_
typedef intptr_t pid_t;
#define _PID_T_     1
#endif

#ifndef _MODE_T_
    typedef unsigned short _mode_t;
    #define _MODE_T_            1

    #ifndef NO_OLDNAMES
    typedef _mode_t mode_t;
    #endif
#endif  /* _MODE_T_ */

#ifndef ENOTSUP
#define ENOTSUP         129 /* This is the value in VC 2010. */
#endif

#ifndef ETIMEDOUT
#define ETIMEDOUT       138 /* This is the value in VC 2010. */
#endif

/** @} */

/**
 * @defgroup clock POSIX Time Routines
 * @{
 */

#ifndef __clockid_t_defined
typedef int clockid_t;
#define __clockid_t_defined     1
#endif  /* __clockid_t_defined */

// #ifndef _TIMESPEC_DEFINED
// struct timespec {
//   time_t  tv_sec;       /* Seconds */
//   long    tv_nsec;      /* Nanoseconds */
// };

// struct itimerspec {
//   struct timespec  it_interval; /* Timer period */
//   struct timespec  it_value;    /* Timer expiration */
// };
// #define _TIMESPEC_DEFINED       1
// #endif  /* _TIMESPEC_DEFINED */

/** @} */

#endif /* _WIN_PTHREAD_TYPES_H_ */