#include "hpcmp_clientreq.h"
#include "valgrind.h"

#ifndef HPCMP_CLIENT_HOOKS_H
#define HPCMP_CLIENT_HOOKS_H

#define MP_PAUSE_TRACKING                                                      \
    VALGRIND_DO_CLIENT_REQUEST_STMT(HPCMP_USERREQ__PAUSE_TRACKING, 0, 0, 0, 0, \
                                    0);

#define MP_START_TRACKING                                                      \
    VALGRIND_DO_CLIENT_REQUEST_STMT(HPCMP_USERREQ__START_TRACKING, 0, 0, 0, 0, \
                                    0);

/** Obtain the thread ID assigned by Valgrind's core. */
#define MP_GET_VALGRIND_THREADID                                               \
    (unsigned)VALGRIND_DO_CLIENT_REQUEST_EXPR(                                 \
        0, HPCMP_USERREQ__GET_VALGRIND_THREAD_ID, 0, 0, 0, 0, 0)

typedef unsigned VgTid;

static inline void mp_hook_prim_init(void* addr, char const* name)
{
    VALGRIND_DO_CLIENT_REQUEST_STMT(HPCMP_USERREQ__PRIM_INIT, addr, name, 0, 0,
                                    0);
}

static inline void mp_hook_prim_destroy(void* addr, char const* name)
{
    VALGRIND_DO_CLIENT_REQUEST_STMT(HPCMP_USERREQ__PRIM_DESTROY, addr, name, 0,
                                    0, 0);
}

static inline void mp_hook_thread_create(VgTid parent, VgTid child)
{
    VALGRIND_DO_CLIENT_REQUEST_STMT(HPCMP_USERREQ__THREAD_CREATE, parent, child,
                                    0, 0, 0);
}

static inline void mp_hook_thread_join(VgTid child)
{
    VALGRIND_DO_CLIENT_REQUEST_STMT(HPCMP_USERREQ__THREAD_JOIN, child, 0, 0, 0,
                                    0);
}

static inline void mp_hook_post_acquire(void* addr)
{
    VALGRIND_DO_CLIENT_REQUEST_STMT(HPCMP_USERREQ__POST_ACQUIRE, addr, 0, 0, 0,
                                    0);
}

static inline void mp_hook_pre_release(void* addr)
{
    VALGRIND_DO_CLIENT_REQUEST_STMT(HPCMP_USERREQ__PRE_RELEASE, addr, 0, 0, 0,
                                    0);
}

#endif /* HPCMP_CLIENT_HOOKS_H */