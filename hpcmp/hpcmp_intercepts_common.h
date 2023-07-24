#include "config.h"         /* HAVE_PTHREAD_MUTEX_ADAPTIVE_NP etc. */
#include "pub_tool_redir.h" /* VG_WRAP_FUNCTION_ZZ() */
#include <assert.h>
#include <pthread.h>
#include <semaphore.h> /* sem_t */

#include "hpcmp_client_hooks.h"

#ifndef HPCMP_INTERCEPTS_COMMON_H
#define HPCMP_INTERCEPTS_COMMON_H

#define PTH_FUNC(ret_ty, zf, implf, argl_decl, argl)                           \
    ret_ty VG_WRAP_FUNCTION_ZZ(VG_Z_LIBC_SONAME, zf) argl_decl;                \
    ret_ty VG_WRAP_FUNCTION_ZZ(VG_Z_LIBC_SONAME, zf) argl_decl                 \
    {                                                                          \
        return implf argl;                                                     \
    }                                                                          \
    ret_ty VG_WRAP_FUNCTION_ZZ(VG_Z_LIBPTHREAD_SONAME, zf) argl_decl;          \
    ret_ty VG_WRAP_FUNCTION_ZZ(VG_Z_LIBPTHREAD_SONAME, zf) argl_decl           \
    {                                                                          \
        return implf argl;                                                     \
    }

/**
 * Macro for generating three Valgrind interception functions: one with the
 * Z-encoded name zf, one with ZAZa ("@*") appended to the name zf and one
 * with ZDZa ("$*") appended to the name zf. The second generated interception
 * function will intercept versioned symbols on Linux, and the third will
 * intercept versioned symbols on Darwin.
 */
#define PTH_FUNCS(ret_ty, zf, implf, argl_decl, argl)                          \
    PTH_FUNC(ret_ty, zf, implf, argl_decl, argl);                              \
    PTH_FUNC(ret_ty, zf##ZAZa, implf, argl_decl, argl);
// PTH_FUNC(ret_ty, zf##ZDZa, implf, argl_decl, argl);

struct enter_arg {
    void* (*start_fn)(void*);
    void*  arg;
    sem_t* started;
    VgTid  parent;
};

static void* pthread_enter(void* arg)
{
    struct enter_arg wrapper_arg = *((struct enter_arg*)arg);

    mp_hook_thread_create(wrapper_arg.parent, pthread_self());

    int res = sem_post(wrapper_arg.started);
    assert(!res);

    MP_START_TRACKING;

    return wrapper_arg.start_fn(wrapper_arg.arg);
}

static __always_inline int pthread_create_intercept(pthread_t* thread,
                                                    const pthread_attr_t* attr,
                                                    void* (*start)(void*),
                                                    void* arg)
{
    int    ret, res;
    OrigFn fn;
    VALGRIND_GET_ORIG_FN(fn);

    MP_PAUSE_TRACKING;

    sem_t enter_started;
    res = sem_init(&enter_started, 0, 0);
    assert(!res);

    struct enter_arg enter_arg = {.start_fn = start,
                                  .arg      = arg,
                                  .started  = &enter_started,
                                  .parent   = MP_GET_VALGRIND_THREADID};

    CALL_FN_W_WWWW(ret, fn, thread, attr, pthread_enter, &enter_arg);

    if (ret == 0) {
        res = sem_wait(&enter_started);
        assert(!res);
    }

    res = sem_destroy(&enter_started);
    assert(!res);

    MP_START_TRACKING;

    return ret;
}

PTH_FUNCS(int,
          pthreadZucreate,
          pthread_create_intercept,
          (pthread_t * thread,
           const pthread_attr_t* attr,
           void* (*start)(void*),
           void* arg),
          (thread, attr, start, arg));

static __always_inline int pthread_join_intercept(pthread_t pt_joinee,
                                                  void**    thread_return)
{
    int    ret;
    OrigFn fn;
    VALGRIND_GET_ORIG_FN(fn);

    CALL_FN_W_WW(ret, fn, pt_joinee, thread_return);

    if (ret == 0) {
        mp_hook_thread_join(pt_joinee);
    }

    return ret;
}

PTH_FUNCS(int,
          pthreadZujoin,
          pthread_join_intercept,
          (pthread_t pt_joinee, void** thread_return),
          (pt_joinee, thread_return));

static __always_inline int
pthread_cond_init_intercept(pthread_cond_t*           cond,
                            const pthread_condattr_t* attr)
{
    int    ret;
    OrigFn fn;
    VALGRIND_GET_ORIG_FN(fn);

    CALL_FN_W_WW(ret, fn, cond, attr);

    mp_hook_prim_init(cond, "cond");
    return ret;
}

#endif