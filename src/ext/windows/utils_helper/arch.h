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
 * URL: https://github.com/songdongsheng/libpthread/blob/master/src/arch.h
 */

#ifndef _ARCH_THREAD_H_
#define _ARCH_THREAD_H_ 1

/**
 * @file arch.h
 * @brief Implementation-related Definitions
 */

/**
 * @defgroup impl Implementation-related Definitions and Code
 * @ingroup libpthread
 * @{
 */

#include <winsock2.h>
#include <pthread.h>

typedef struct
{
    HANDLE handle;
} arch_sem_t;

struct arch_thread_cleanup_node {
    void (* cleaner)(void *);
    void *arg;
    struct arch_thread_cleanup_node *next, *prev;
};

typedef struct arch_thread_cleanup_node arch_thread_cleanup_list;

typedef struct
{
    int detach_state;
    size_t guard_size;
    int inherit_sched;
    int sched_policy;
    struct sched_param sched_param;
    int scope;
    void *stack_addr;
    size_t stack_size;
} arch_thread_attr;

typedef struct {
    HANDLE handle;
    void *(* worker)(void *);
    void *arg;
    void *return_value;
    unsigned int state;
    arch_thread_cleanup_list *cleanup_list;
} arch_thread_info;

/*
    On 32-bit OS:
    sizeof(pthread_attr_t): 4
    sizeof(CRITICAL_SECTION): 24
    sizeof(SRWLOCK): 4
    sizeof(CONDITION_VARIABLE): 4

    On 64-bit OS:
    sizeof(pthread_attr_t): 8
    sizeof(CRITICAL_SECTION): 40
    sizeof(SRWLOCK): 8
    sizeof(CONDITION_VARIABLE): 8
 */

typedef struct {

    /* PTHREAD_MUTEX_STALLED or PTHREAD_MUTEX_ROBUST */
    int robust;

    /*
     * PTHREAD_MUTEX_NORMAL, PTHREAD_MUTEX_ERRORCHECK,
     * PTHREAD_MUTEX_RECURSIVE, or PTHREAD_MUTEX_DEFAULT
     */
    int type;

    /* PTHREAD_PROCESS_PRIVATE or PTHREAD_PROCESS_SHARED */
    int pshared;

    /* PTHREAD_PRIO_NONE, PTHREAD_PRIO_INHERIT or PTHREAD_PRIO_PROTECT */
    int protocol;

    /* from __sched_fifo_min_prio to __sched_fifo_max_prio */
    int prioceiling;
} arch_mutex_attr;

typedef struct {
    long wait;
    long lock_status; /* 0:unlocked, 1:locked */
    /* long thread_id; debug only */
    long spin_count;
    HANDLE sync;
} arch_mutex;

typedef struct {
    int pshared;
} arch_barrier_attr;

typedef struct {
    long count;
    long total;
    long index;
    HANDLE semaphore[2];
} arch_barrier;

// typedef struct {
//     int pshared;
//     clockid_t *clock_id;
// } arch_cond_attr;

typedef struct {
    char cond[8]; /* InitializeConditionVariable */
} arch_cond;

typedef struct {
    int pshared;
} arch_rwlock_attr;

typedef struct {
    char rwlock[8]; /* InitializeSRWLock */
} arch_rwlock;

/** @} */

#endif