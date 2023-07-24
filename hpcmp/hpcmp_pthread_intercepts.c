/* POSIX thread intercepts
 *
 * This file is part of HPCMP (High Performance Computing Memory Profiler),
 * a Valgrind tool for profiling HPC application memory behavior.
 *
 * Copyright (C) 2023 Mihai Renea
 *  mihai.renea@fu-berlin.de
 *
 * This code derived from previous work by Bart Van Assche
 * (drd/drd_clientreq.c), which is
 *
 * Copyright (C) 2006-2020 Bart Van Assche <bvanassche@acm.org>.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 *
 * The GNU General Public License is contained in the file COPYING.
 *
 *
 *
 * Some peculiarities:
 *  - VALGRIND_GET_ORIG_FN(fn) MUST be the first statement, otherwise int won't
 *    work, although it's not documented as such. Don't ask why, I don't get how
 *    it works.
 * */

/*
 * Define _GNU_SOURCE to make sure that pthread_barrier_t is available when
 * compiling with older glibc versions (2.3 or before).
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <assert.h>
#include <stdio.h>

#include "hpcmp_intercepts_common.h"

PTH_FUNCS(int,
          pthreadZucondZuinit,
          pthread_cond_init_intercept,
          (pthread_cond_t * cond, const pthread_condattr_t* attr),
          (cond, attr));

static __always_inline int pthread_cond_destroy_intercept(pthread_cond_t* cond)
{
    int    ret;
    OrigFn fn;
    VALGRIND_GET_ORIG_FN(fn);

    CALL_FN_W_W(ret, fn, cond);

    mp_hook_prim_destroy(cond, "cond");

    return ret;
}

PTH_FUNCS(int,
          pthreadZucondZudestroy,
          pthread_cond_destroy_intercept,
          (pthread_cond_t * cond),
          (cond));

static __always_inline int pthread_cond_wait_intercept(pthread_cond_t*  cond,
                                                       pthread_mutex_t* mutex)
{
    int    ret;
    OrigFn fn;
    VALGRIND_GET_ORIG_FN(fn);

    CALL_FN_W_WW(ret, fn, cond, mutex);

    mp_hook_post_acquire(cond);

    return ret;
}

PTH_FUNCS(int,
          pthreadZucondZuwait,
          pthread_cond_wait_intercept,
          (pthread_cond_t * cond, pthread_mutex_t* mutex),
          (cond, mutex));

static __always_inline int
pthread_cond_timedwait_intercept(pthread_cond_t*        cond,
                                 pthread_mutex_t*       mutex,
                                 const struct timespec* abstime)
{
    int    ret;
    OrigFn fn;
    VALGRIND_GET_ORIG_FN(fn);

    CALL_FN_W_WWW(ret, fn, cond, mutex, abstime);

    mp_hook_post_acquire(cond);

    return ret;
}

PTH_FUNCS(int,
          pthreadZucondZutimedwait,
          pthread_cond_timedwait_intercept,
          (pthread_cond_t * cond,
           pthread_mutex_t*       mutex,
           const struct timespec* abstime),
          (cond, mutex, abstime));

static __always_inline int pthread_cond_signal_intercept(pthread_cond_t* cond)
{
    int    ret;
    OrigFn fn;
    VALGRIND_GET_ORIG_FN(fn);

    mp_hook_pre_release(cond);

    CALL_FN_W_W(ret, fn, cond);
    return ret;
}

PTH_FUNCS(int,
          pthreadZucondZusignal,
          pthread_cond_signal_intercept,
          (pthread_cond_t * cond),
          (cond));

static __always_inline int
pthread_cond_broadcast_intercept(pthread_cond_t* cond)
{
    int    ret;
    OrigFn fn;
    VALGRIND_GET_ORIG_FN(fn);

    mp_hook_pre_release(cond);

    CALL_FN_W_W(ret, fn, cond);

    return ret;
}

PTH_FUNCS(int,
          pthreadZucondZubroadcast,
          pthread_cond_broadcast_intercept,
          (pthread_cond_t * cond),
          (cond));

static __always_inline int
sem_init_intercept(sem_t* sem, int pshared, unsigned int value)
{
    int    ret;
    OrigFn fn;
    VALGRIND_GET_ORIG_FN(fn);

    CALL_FN_W_WWW(ret, fn, sem, pshared, value);

    mp_hook_prim_init(sem, "sem");

    return ret;
}

PTH_FUNCS(int,
          semZuinit,
          sem_init_intercept,
          (sem_t * sem, int pshared, unsigned int value),
          (sem, pshared, value));

static __always_inline int sem_destroy_intercept(sem_t* sem)
{
    int    ret;
    OrigFn fn;
    VALGRIND_GET_ORIG_FN(fn);

    CALL_FN_W_W(ret, fn, sem);

    mp_hook_prim_destroy(sem, "sem");

    return ret;
}

PTH_FUNCS(int, semZudestroy, sem_destroy_intercept, (sem_t * sem), (sem));

static __always_inline int sem_wait_intercept(sem_t* sem)
{
    int    ret;
    OrigFn fn;
    VALGRIND_GET_ORIG_FN(fn);

    CALL_FN_W_W(ret, fn, sem);

    mp_hook_post_acquire(sem);

    return ret;
}

PTH_FUNCS(int, semZuwait, sem_wait_intercept, (sem_t * sem), (sem));

static __always_inline int sem_trywait_intercept(sem_t* sem)
{
    int    ret;
    OrigFn fn;
    VALGRIND_GET_ORIG_FN(fn);

    CALL_FN_W_W(ret, fn, sem);

    mp_hook_post_acquire(sem);

    return ret;
}

PTH_FUNCS(int, semZutrywait, sem_trywait_intercept, (sem_t * sem), (sem));

static __always_inline int
sem_timedwait_intercept(sem_t* sem, const struct timespec* abs_timeout)
{
    int    ret;
    OrigFn fn;
    VALGRIND_GET_ORIG_FN(fn);

    CALL_FN_W_WW(ret, fn, sem, abs_timeout);

    mp_hook_post_acquire(sem);

    return ret;
}

PTH_FUNCS(int,
          semZutimedwait,
          sem_timedwait_intercept,
          (sem_t * sem, const struct timespec* abs_timeout),
          (sem, abs_timeout));

static __always_inline int sem_post_intercept(sem_t* sem)
{
    int    ret;
    OrigFn fn;
    VALGRIND_GET_ORIG_FN(fn);

    mp_hook_pre_release(sem);

    CALL_FN_W_W(ret, fn, sem);

    return ret;
}

PTH_FUNCS(int, semZupost, sem_post_intercept, (sem_t * sem), (sem));

#if defined(HAVE_PTHREAD_BARRIER_INIT)
static __always_inline int
pthread_barrier_init_intercept(pthread_barrier_t*           barrier,
                               const pthread_barrierattr_t* attr,
                               unsigned                     count)
{
    int    ret;
    OrigFn fn;
    VALGRIND_GET_ORIG_FN(fn);

    CALL_FN_W_WWW(ret, fn, barrier, attr, count);

    mp_hook_prim_init(barrier, "barrier");

    return ret;
}

PTH_FUNCS(int,
          pthreadZubarrierZuinit,
          pthread_barrier_init_intercept,
          (pthread_barrier_t * barrier,
           const pthread_barrierattr_t* attr,
           unsigned                     count),
          (barrier, attr, count));

static __always_inline int
pthread_barrier_destroy_intercept(pthread_barrier_t* barrier)
{
    int    ret;
    OrigFn fn;
    VALGRIND_GET_ORIG_FN(fn);

    CALL_FN_W_W(ret, fn, barrier);

    mp_hook_prim_destroy(barrier, "barrier");

    return ret;
}

PTH_FUNCS(int,
          pthreadZubarrierZudestroy,
          pthread_barrier_destroy_intercept,
          (pthread_barrier_t * barrier),
          (barrier));

static __always_inline int
pthread_barrier_wait_intercept(pthread_barrier_t* barrier)
{
    int    ret;
    OrigFn fn;
    VALGRIND_GET_ORIG_FN(fn);

    mp_hook_pre_release(barrier);

    CALL_FN_W_W(ret, fn, barrier);

    mp_hook_post_acquire(barrier);

    return ret;
}

PTH_FUNCS(int,
          pthreadZubarrierZuwait,
          pthread_barrier_wait_intercept,
          (pthread_barrier_t * barrier),
          (barrier));
#endif