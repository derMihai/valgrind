/* This file is part of HPCMP (High Performance Computing Memory Profiler),
 * a Valgrind tool for profiling HPC application memory behavior.
 *
 * Copyright (C) 2023 Mihai Renea
 *    mihai.renea@fu-berlin.de
 *
 * This code derived from previous work by Mozilla Foundation (dhat/dh_main.c),
 * which is
 *
 * Copyright (C) 2010-2018 Mozilla Foundation
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
 * */

#include "coregrind/pub_core_threadstate.h"

#include "pub_tool_basics.h"
#include "pub_tool_clientstate.h"
#include "pub_tool_clreq.h"
#include "pub_tool_libcassert.h"
#include "pub_tool_libcbase.h"
#include "pub_tool_libcfile.h"
#include "pub_tool_libcprint.h"
#include "pub_tool_libcproc.h"
#include "pub_tool_machine.h" // VG_(fnptr_to_fnentry)
#include "pub_tool_mallocfree.h"
#include "pub_tool_options.h"
#include "pub_tool_replacemalloc.h"
#include "pub_tool_tooliface.h"
#include "pub_tool_wordfm.h"

#include "dbg_ev_handler.h"
#include "hpcmp_clientreq.h"
#include "json_handler.h"
#include "mp.h"
#include "mp_bfm.h"
#include "mp_ev.h"

typedef struct {
    ThreadId  tid;
    ThreadId  parent;
    BFM       blocks;
    PThreadId pthid;
    SizeT     inst_cnt; // since last event
    Bool      trackable;
} MpThreadInfo;

//------------------------------------------------------------//
//--- Globals                                              ---//
//------------------------------------------------------------//

#define MAIN_TID 1

/* May not contain zero-sized blocks.  May not contain
   overlapping blocks. */
static BFM g_block_list = {.fm = NULL};

static ULong g_curr_instrs = 0; // incremented from generated code

static ThreadId g_curr_tid = VG_INVALID_THREADID;
static ThreadId g_prev_tid = VG_INVALID_THREADID;
// indexed by g_curr_tid
static MpThreadInfo* g_thd_info_a = NULL;

static MpEventHandler* g_ev_handler    = NULL;
static HChar const*    clo_mp_out_file = NULL;

//------------------------------------------------------------//
//--- Declarations                                         ---//
//------------------------------------------------------------//

static MpThreadInfo* get_thread_info(ThreadId tid);
static MpThreadInfo* try_get_thread_info(ThreadId tid);
static void          record_event(ThreadId tid, MpEvent* ev);
static void          prune_block_cache(ThreadId tid, Bool prune_unused);
static PThreadId     get_pthid(ThreadId tid);

//------------------------------------------------------------//
//--- block instrumentation                                ---//
//------------------------------------------------------------//
//
// There is one global block list, `g_block_list`. Each thread has it's own list
// (thread-local cache) with references to a subset of these blocks. Any change
// (block added, removed, resized) is performed on the global list only. To
// manage inconsistencies when a block is removed from the global list (either
// freed or enlarged with realloc), the respective block is marked as freed, and
// it's reference count is decreased. Later on, when a thread finds a "freed"
// block in it's local cache, it will discard it and search for a new matching
// block in the global list. "Freed" blocks will get de-allocated once discarded
// from all local caches.
//
// - bi_*() are instrumentation private functions
// - *_c() are "contains" functions, called with an address that can be mapped
//   to a block. The address may be invalid. These functions are called with a
//   valid TID.
// - app_*() reflect application's intention - e.g. application calls malloc,
//   free, etc.
//
// TODO: implement a small block cache

static void bi_rel_block(Block** bk)
{
    Block* p = *bk;
    if (!p) {
        return;
    }

    tl_assert(p->refc > 0);
    if (--p->refc == 0) {
        tl_assert(p->state != BLOCK_ALIVE);
        VG_(free)(p);
    }

    *bk = NULL;
}

// only for VG_(deleteFM)()
static void bi_rel_block_fm(UWord bkp) { bi_rel_block((Block**)&bkp); }
// only for VG_(deleteFM)()
// Assumes this is the last reference.
// Used ONLY by `g_block_list_destroy` to assert caches are well behaved
static void bi_rel_block_last_fm(UWord bkp)
{
    Block* bk = (Block*)bkp;

    tl_assert(bk->refc == 1);
    bi_rel_block(&bk);
}

static Block* bi_ref_block(Block* bk)
{
    if (bk) {
        tl_assert(bk->refc > 0);
        bk->refc++;
    }

    return bk;
}

static Block* bi_clone_block(Block* bk)
{
    if (!bk) {
        return NULL;
    }

    tl_assert(bk->refc > 0);

    Block* new_bk = VG_(malloc)("mp.clone_block", sizeof(*new_bk));
    *new_bk       = *bk;

    new_bk->refc = 1;

    return new_bk;
}

static Block* bi_find_block_fm(Addr a, BFM fm, BlockUsage** bku)
{
    Block*      bk   = NULL;
    BlockUsage* bkup = NULL;

    if (!lookupBFM(fm, &bk, &bkup, a)) {
        return NULL;
    }

    tl_assert(bk->refc > 0);
    tl_assert(a >= bk->payload && a < bk->payload + bk->req_szB);

    if (bku) {
        *bku = bkup;
    }

    return bk;
}

static Block* bi_remove_block_fm(Addr a, BFM fm, BlockUsage** bku)
{
    Block* bk = NULL;
    delFromBFM(fm, &bk, bku, a, 1);

    return bk;
}

static void bi_free_block_usage(BlockUsage** bku)
{
    VG_(free)(*bku);
    *bku = NULL;
}

static void bi_free_block_usage_fm(UWord bkp) { VG_(free)((void*)bkp); }

static void bi_prune_overlap(MpThreadInfo* ti, Addr a, SizeT len)
{
    Block*      bk  = NULL;
    BlockUsage* bku = NULL;

    while (delFromBFM(ti->blocks, &bk, &bku, a, len)) {
        tl_assert(bk);
        tl_assert(bku);
        tl_assert(bk->state != BLOCK_ALIVE);

        if (block_used(bku)) {
            // when freeing, we record the block usage of the thread that does
            // it, so that resets the block (unused). If the block is used,
            // this means it was freed by another thread, but after last sync
            // because that would reset the block too. Something fishy is
            // happening...
            record_event(ti->tid, &(MpEvent){.type  = MPEV_INFO,
                                             .pthid = ti->pthid,
                                             .info  = "used dead block"});
        }

        bi_rel_block(&bk);
        bi_free_block_usage(&bku);
    }
}

static Block* find_block_c(ThreadId tid, Addr a, BlockUsage** bkupp)
{
    BFM         thd_cache = get_thread_info(tid)->blocks;
    BlockUsage* bku       = NULL;
    Bool        not_found = True;

    // first, search thread-local cache
    Block* bk = bi_find_block_fm(a, thd_cache, &bku);
    if (bk) {
        tl_assert(bku);
        not_found = False;
        if (bk->state != BLOCK_ALIVE) {
            // Block no longer exists, this is probably a new one that
            // overlaps
            bk = bi_remove_block_fm(a, thd_cache, &bku);
            tl_assert(bku);
            tl_assert(bk);

            if (block_used(bku)) {
                // when freeing, we record the block usage of the thread that
                // does it, so that resets the block (unused). If the block is
                // used, this means it was freed by another thread, but after
                // last sync because that would reset the block too. Something
                // fishy is happening...
                record_event(tid, &(MpEvent){.type  = MPEV_INFO,
                                             .pthid = get_pthid(tid),
                                             .info  = "used dead block"});
            }

            bi_rel_block(&bk);
            bi_free_block_usage(&bku);
        }
    }

    if (!bk) {
        tl_assert(!bku);
        // search globally
        bk = bi_ref_block(bi_find_block_fm(a, g_block_list, NULL));

        if (!bk) {
            // static data or use after free
            return NULL;
        }
        // Before inserting the new block into the local cache, make sure there
        // are no dead blocks (i.e. freed, realloc'd) that overlap
        bi_prune_overlap(get_thread_info(tid), bk->payload, bk->req_szB);

        bku = VG_(calloc)("mp.find_block_c", 1, sizeof(*bku));
        tl_assert(bku);
        Bool present = addToBFM(thd_cache, bk, bku);
        tl_assert2(!present, "%p %d\n", (void*)a, not_found);
    }

    tl_assert(bk->state == BLOCK_ALIVE);
    tl_assert(bk->refc > 1);

    if (bkupp) {
        *bkupp = bku;
    }
    return bk;
}

static BlockUsage* find_block_usage_c(ThreadId tid, Addr a)
{
    BlockUsage* bku = NULL;
    Block*      bk  = find_block_c(tid, a, &bku);
    if (!bk) {
        return NULL;
    }

    tl_assert(bku);

    return bku;
}

static void*
app_new_block(ThreadId tid, SizeT req_szB, SizeT req_alignB, Bool is_zeroed)
{
    SizeT actual_szB;

    if ((SSizeT)req_szB < 0)
        return NULL;

    if (req_szB == 0) {
        req_szB = 1; /* can't allow zero-sized blocks in the interval tree */
    }

    // Allocate and zero if necessary
    void* p = VG_(cli_malloc)(req_alignB, req_szB);
    if (!p) {
        return NULL;
    }

    record_event(tid, &(MpEvent){.pthid = get_pthid(tid),
                                 .type  = MPEV_LIFE,
                                 .life  = {.type       = LIFEEV_ALLOC,
                                           .alloc.addr = (Addr)p,
                                           .alloc.size = req_szB}});

    if (is_zeroed)
        VG_(memset)(p, 0, req_szB);

    actual_szB = VG_(cli_malloc_usable_size)(p);
    tl_assert(actual_szB >= req_szB);

    // Make new Block, add to interval_tree.
    Block* bk   = VG_(malloc)("mp.app_new_block", sizeof(Block));
    bk->payload = (Addr)p;
    bk->req_szB = req_szB;
    bk->state   = BLOCK_ALIVE;
    bk->refc    = 1;

    Bool present = addToBFM(g_block_list, bk, NULL /*no val*/);
    tl_assert(!present);

    return p;
}

static void app_free_block(ThreadId tid, void* p)
{
    VG_(cli_free)(p);

    BlockUsage* bku = NULL;
    Block*      bk  = bi_remove_block_fm((Addr)p, g_block_list, &bku);
    if (!bk) {
        // bogus free
        VG_(dmsg)("!!! bogus free %p\n", p);
        return;
    }

    tl_assert(!bku);
    bku = find_block_usage_c(tid, (Addr)p);

    // We free the block, so we record any usage left from this thread. We
    // assume other threads should have already done so on the last sync event.
    record_event(tid, &(MpEvent){.pthid = get_pthid(tid),
                                 .type  = MPEV_LIFE,
                                 .life  = {.type      = LIFEEV_FREE,
                                           .free.addr = bk->payload,
                                           .free.size = bk->req_szB,
                                           .free.bku  = bku}});

    bk->state = BLOCK_FREED;
    bi_rel_block(&bk);
}

static void* app_resize_block(ThreadId tid, void* p_old, SizeT new_req_szB)
{
    void* p_new = NULL;

    tl_assert(new_req_szB > 0); // map 0 to 1

    // Find the old block.
    Block* bk = bi_find_block_fm((Addr)p_old, g_block_list, NULL);
    if (!bk) {
        VG_(dmsg)("!!! bogus realloc %p\n", p_old);
        return NULL; // bogus realloc
    }
    if (bk->payload != (Addr)p_old) {
        VG_(dmsg)("!!! bogus realloc 2 %p\n", p_old);
        return NULL; // bogus realloc
    }

    BlockUsage* bku = find_block_usage_c(tid, (Addr)p_old);
    tl_assert(bk->req_szB > 0);
    // Assert the block finder is behaving sanely.
    tl_assert(bk->payload <= (Addr)p_old);
    tl_assert((Addr)p_old < bk->payload + bk->req_szB);

    // We free the block, so we record any usage left from this thread. We
    // assume other threads should have already done so on the last sync event.
    record_event(tid, &(MpEvent){.pthid = get_pthid(tid),
                                 .type  = MPEV_LIFE,
                                 .life  = {.type      = LIFEEV_FREE,
                                           .free.addr = bk->payload,
                                           .free.size = bk->req_szB,
                                           .free.bku  = bku}});

    // Actually do the allocation, if necessary.
    if (new_req_szB <= bk->req_szB) {
        // New size is smaller or same; block not moved.
        bk->req_szB = new_req_szB;

        p_new = p_old;
    } else {
        // New size is bigger;  make new block, copy shared contents, free
        // old.
        p_new = VG_(cli_malloc)(VG_(clo_alignment), new_req_szB);
        if (!p_new) {
            // Nb: if realloc fails, NULL is returned but the old block is
            // not touched.  What an awful function.
            return NULL;
        }
        tl_assert(p_new != p_old);

        VG_(memcpy)(p_new, p_old, bk->req_szB);
        VG_(cli_free)(p_old);

        // Since the block has moved, we need to re-insert it into the
        // interval tree at the new place. It also needs to be a new block,
        // since other threads might cache it.
        Block* bk_new = bi_clone_block(bk);

        bk_new->payload = (Addr)p_new;
        bk_new->req_szB = new_req_szB;

        bk = bi_remove_block_fm((Addr)p_old, g_block_list, NULL);
        tl_assert(bk);

        bk->state = BLOCK_REALLOC;
        bi_rel_block(&bk);

        // add the new block to the global block list
        Bool present = addToBFM(g_block_list, bk_new, NULL);
        tl_assert(!present);
    }

    record_event(tid, &(MpEvent){.pthid = get_pthid(tid),
                                 .type  = MPEV_LIFE,
                                 .life  = {.type  = LIFEEV_ALLOC,
                                           .alloc = {
                                               .addr = (Addr)p_new,
                                               .size = new_req_szB,
                                          }}});

    return p_new;
}

static void prune_block_cache(ThreadId tid, Bool prune_unused)
{
    MpThreadInfo* ti  = get_thread_info(tid);
    Block*        bk  = NULL;
    BlockUsage*   bku = NULL;

    initIterBFM(ti->blocks);
    while (nextIterBFM(ti->blocks, &bk, &bku)) {
        tl_assert(bk);
        tl_assert(bku);

        if (bk->state != BLOCK_ALIVE) {
            goto prune;
        }
        if (prune_unused && !block_used(bku)) {
            goto prune;
        }
        continue;

    prune:
        doneIterBFM(ti->blocks);

        bk = bi_remove_block_fm(bk->payload, ti->blocks, &bku);
        tl_assert(bk && bku);

        initIterAtBFM(ti->blocks, bk->payload);

        bi_rel_block(&bk);
        bi_free_block_usage(&bku);
    }
    doneIterBFM(ti->blocks);
}

static void reset_block_cache(ThreadId tid)
{
    MpThreadInfo* ti  = get_thread_info(tid);
    Block*        bk  = NULL;
    BlockUsage*   bku = NULL;

    initIterBFM(ti->blocks);
    while (nextIterBFM(ti->blocks, &bk, &bku)) {
        tl_assert(bk);
        tl_assert(bku);

        bku->bytes_read  = 0;
        bku->bytes_write = 0;
    }
    doneIterBFM(ti->blocks);
}

static void g_block_list_create(void)
{
    g_block_list = newBFM("mp.g_block_list");
}

static void g_block_list_destroy(void)
{
    deleteBFM(&g_block_list, bi_rel_block_last_fm, NULL /*no vals*/);
}
//------------------------------------------------------------//
//--- Events                                               ---//
//------------------------------------------------------------//

static void record_event_force(ThreadId tid, MpEvent* ev)
{
    tl_assert(g_ev_handler);

    MpThreadInfo* ti = try_get_thread_info(tid);

    if (!ti) {
        return;
    }

    ev->inst_cnt = g_curr_instrs + ti->inst_cnt;

    g_curr_instrs = 0;
    ti->inst_cnt  = 0;

    tl_assert(ti->pthid != INVALID_POSIX_THREADID);
    g_ev_handler->handle_ev(g_ev_handler, ev);
}

static void record_event(ThreadId tid, MpEvent* ev)
{
    tl_assert(g_ev_handler);

    MpThreadInfo* ti = try_get_thread_info(tid);

    if (!ti) {
        return;
    }

    if (!ti->trackable) {
        // possibly in pthread init/deinit phase
        return;
    }

    ev->inst_cnt = g_curr_instrs + ti->inst_cnt;

    g_curr_instrs = 0;
    ti->inst_cnt  = 0;

    tl_assert(ti->pthid != INVALID_POSIX_THREADID);
    g_ev_handler->handle_ev(g_ev_handler, ev);
}

//------------------------------------------------------------//
//--- thread instrumentation                               ---//
//------------------------------------------------------------//

static void context_switch(ThreadId from, ThreadId tid)
{
    // record_event(
    //     &(MpEvent){.tid = tid, .type = MPEV_CONTEXT_SWITCH, .csw.from =
    //     from});
    (void)tid;
    (void)from;

    if (from != VG_INVALID_THREADID) {
    }
}

static MpThreadInfo* try_get_thread_info(ThreadId tid)
{
    tl_assert(tid < VG_N_THREADS);
    tl_assert(tid != VG_INVALID_THREADID);

    MpThreadInfo* ti = &g_thd_info_a[tid];
    if (ti->tid != tid) {
        tl_assert(ti->tid == VG_INVALID_THREADID);
        return NULL;
    }

    return ti;
}

static MpThreadInfo* get_thread_info(ThreadId tid)
{
    tl_assert(tid < VG_N_THREADS);
    tl_assert(tid != VG_INVALID_THREADID);

    MpThreadInfo* ti = &g_thd_info_a[tid];

    tl_assert2(ti->tid == tid, "ti->tid=%u, tid=%u\n", ti->tid, tid);

    return ti;
}

static PThreadId get_pthid(ThreadId tid) { return get_thread_info(tid)->pthid; }

static void set_thread_info(ThreadId parent, ThreadId tid)
{
    tl_assert(parent < VG_N_THREADS);
    tl_assert(tid < VG_N_THREADS);

    MpThreadInfo* ti = &g_thd_info_a[tid];

    tl_assert(ti->tid == VG_INVALID_THREADID);
    tl_assert(ti->parent == VG_INVALID_THREADID);

    ti->tid    = tid;
    ti->parent = parent;

    tl_assert(ti->blocks.fm == NULL);
    ti->blocks = newBFM("mp.ti.blocks");
}

static void init_thread_info(ThreadId tid)
{
    MpThreadInfo* ti = &g_thd_info_a[tid];

    VG_(memset)(ti, 0, sizeof(*ti));

    ti->parent = VG_INVALID_THREADID;
    ti->tid    = VG_INVALID_THREADID;
    ti->pthid  = INVALID_POSIX_THREADID;
}

static void unset_thread_info(ThreadId tid)
{
    MpThreadInfo* ti = get_thread_info(tid);

    deleteBFM(&ti->blocks, bi_rel_block_fm, bi_free_block_usage_fm);

    init_thread_info(tid);
}

//------------------------------------------------------------//
//--- need_malloc_replacement handlers                     ---//
//------------------------------------------------------------//

static void* mp_malloc(ThreadId tid, SizeT szB)
{
    return app_new_block(tid, szB, VG_(clo_alignment), /*is_zeroed*/ False);
}

static void* mp___builtin_new(ThreadId tid, SizeT szB)
{
    return app_new_block(tid, szB, VG_(clo_alignment), /*is_zeroed*/ False);
}

static void* mp___builtin_new_aligned(ThreadId tid, SizeT szB, SizeT alignB)
{
    return app_new_block(tid, szB, alignB, /*is_zeroed*/ False);
}

static void* mp___builtin_vec_new(ThreadId tid, SizeT szB)
{
    return app_new_block(tid, szB, VG_(clo_alignment), /*is_zeroed*/ False);
}

static void* mp___builtin_vec_new_aligned(ThreadId tid, SizeT szB, SizeT alignB)
{
    return app_new_block(tid, szB, alignB, /*is_zeroed*/ False);
}

static void* mp_calloc(ThreadId tid, SizeT m, SizeT szB)
{
    return app_new_block(tid, m * szB, VG_(clo_alignment),
                         /*is_zeroed*/ True);
}

static void* mp_memalign(ThreadId tid, SizeT alignB, SizeT szB)
{
    return app_new_block(tid, szB, alignB, False);
}

static void mp_free(ThreadId tid __attribute__((unused)), void* p)
{
    app_free_block(tid, p);
}

static void mp___builtin_delete(ThreadId tid, void* p)
{
    app_free_block(tid, p);
}

static void mp___builtin_delete_aligned(ThreadId tid, void* p, SizeT align)
{
    (void)align;
    app_free_block(tid, p);
}

static void mp___builtin_vec_delete(ThreadId tid, void* p)
{
    app_free_block(tid, p);
}

static void mp___builtin_vec_delete_aligned(ThreadId tid, void* p, SizeT align)
{
    (void)align;
    app_free_block(tid, p);
}

static void* mp_realloc(ThreadId tid, void* p_old, SizeT new_szB)
{
    if (p_old == NULL) {
        return mp_malloc(tid, new_szB);
    }
    if (new_szB == 0) {
        if (VG_(clo_realloc_zero_bytes_frees) == True) {
            mp_free(tid, p_old);
            return NULL;
        }
        new_szB = 1;
    }
    return app_resize_block(tid, p_old, new_szB);
}

static SizeT mp_malloc_usable_size(ThreadId tid, void* p)
{
    Block* bk = find_block_c(tid, (Addr)p, NULL);
    return bk ? bk->req_szB : 0;
}

//------------------------------------------------------------//
//--- thread-tracking handlers                             ---//
//------------------------------------------------------------//

static void mp_start_client_code(const ThreadId tid, const ULong bbs_done)
{
    (void)bbs_done;
    tl_assert(tid != VG_INVALID_THREADID);
    tl_assert(g_curr_tid == VG_INVALID_THREADID);

    g_curr_tid = tid;

    if (tid != g_prev_tid) {
        context_switch(g_prev_tid, tid);
    }
}

static void mp_stop_client_code(const ThreadId tid, const ULong bbs_done)
{
    (void)bbs_done;
    tl_assert(g_curr_tid != VG_INVALID_THREADID);
    g_curr_tid = VG_INVALID_THREADID;
    g_prev_tid = tid;

    // next time we might enter from another thread, store current thread
    // progress
    get_thread_info(tid)->inst_cnt += g_curr_instrs;
    g_curr_instrs = 0;
}

static void mp_pre_thread_ll_create(ThreadId parent, ThreadId child)
{
    set_thread_info(parent, child);
    if (child == MAIN_TID) {
        // main thread is no POSIX thread, so no pthread_create intercept
        // will be called for it to initialize it's pthread id.
        MpThreadInfo* ti = get_thread_info(child);
        ti->trackable    = True;
        ti->pthid        = MAIN_TID;
    }
}

static void mp_pre_thread_ll_exit(ThreadId tid)
{
    MpEvent ev = {.pthid = get_pthid(tid),
                  .type  = MPEV_SYNC,
                  .sync  = {.type        = SYNCEV_EXIT,
                            .block_cache = get_thread_info(tid)->blocks}};
    record_event(tid, &ev);

    // The main thread does some stuff after it exits, so instrumentation
    // keeps going. We therefore defer info destruction until `mp_fini()`
    if (tid != MAIN_TID) {
        unset_thread_info(tid);
        if (g_prev_tid == tid) {
            g_prev_tid = VG_INVALID_THREADID;
        }
    } else {
        // However, we should stop tracking...
        MpThreadInfo* ti = get_thread_info(MAIN_TID);
        ti->pthid        = INVALID_POSIX_THREADID;
        ti->trackable    = False;
    }
}
//------------------------------------------------------------//
//--- sync-tracking handlers                               ---//
//------------------------------------------------------------//
static void track_new_primitive(ThreadId tid, char const* type, Addr a)
{
    MpEvent ev = {.pthid = get_pthid(tid),
                  .type  = MPEV_LIFE,
                  .life  = {.type      = LIFEEV_NEW_SYNC,
                            .sync_life = {.type = type, .addr = a}}};
    record_event(tid, &ev);
}

static void track_del_primitive(ThreadId tid, char const* type, Addr a)
{
    MpEvent ev = {.pthid = get_pthid(tid),
                  .type  = MPEV_LIFE,
                  .life  = {.type      = LIFEEV_DEL_SYNC,
                            .sync_life = {.type = type, .addr = a}}};
    record_event(tid, &ev);
}

static void track_fork(ThreadId parent, PThreadId child)
{
    prune_block_cache(parent, True);

    MpEvent ev = {.pthid = get_pthid(parent),
                  .type  = MPEV_SYNC,
                  .sync  = {.type             = SYNCEV_FORK,
                            .block_cache      = get_thread_info(parent)->blocks,
                            .fojo.child_pthid = child}};

    record_event_force(parent, &ev);
}

static void track_join(ThreadId parent, PThreadId child)
{
    MpEvent ev = {.pthid = get_pthid(parent),
                  .type  = MPEV_SYNC,
                  .sync  = {.type             = SYNCEV_JOIN,
                            .block_cache      = get_thread_info(parent)->blocks,
                            .fojo.child_pthid = child}};

    record_event_force(parent, &ev);
}

static void track_sync_acq(ThreadId tid, Addr a)
{
    MpEvent ev = {.pthid = get_pthid(tid),
                  .type  = MPEV_SYNC,
                  .sync  = {.type          = SYNCEV_ACQ,
                            .block_cache   = get_thread_info(tid)->blocks,
                            .barriers.addr = a}};
    record_event(tid, &ev);
}

static void track_sync_rel(ThreadId tid, Addr a)
{
    MpEvent ev = {.pthid = get_pthid(tid),
                  .type  = MPEV_SYNC,
                  .sync  = {.type          = SYNCEV_REL,
                            .block_cache   = get_thread_info(tid)->blocks,
                            .barriers.addr = a}};
    record_event(tid, &ev);
}

//------------------------------------------------------------//
//--- Client requests                                      ---//
//------------------------------------------------------------//

static Bool handle_client_request(ThreadId tid, UWord* arg, UWord* ret)
{
    UWord retval = 0;

    switch (arg[0]) {
    case HPCMP_USERREQ__THREAD_CREATE: {
        // note, this is called from the child thread, as the parent is
        // still waiting
        ThreadId const  parent      = arg[1];
        ThreadId const  child       = tid;
        PThreadId const child_pthid = arg[2] + MAIN_TID; // avoid ID clash

        MpThreadInfo* child_ti = get_thread_info(child);
        tl_assert(child_ti->pthid == INVALID_POSIX_THREADID);
        child_ti->pthid = child_pthid;

        reset_block_cache(child);

        track_fork(parent, child_pthid);
        break;
    }
    case HPCMP_USERREQ__THREAD_JOIN: {
        PThreadId const child_pthid = arg[1] + MAIN_TID;
        track_join(tid, child_pthid);
        break;
    }

    case HPCMP_USERREQ__PRE_RELEASE:
        track_sync_rel(tid, (Addr)arg[1]);
        break;

    case HPCMP_USERREQ__POST_ACQUIRE:
        track_sync_acq(tid, (Addr)arg[1]);
        break;

    case HPCMP_USERREQ__PRIM_INIT:
        track_new_primitive(tid, (char const*)arg[2], (Addr)arg[1]);
        break;

    case HPCMP_USERREQ__PRIM_DESTROY:
        track_del_primitive(tid, (char const*)arg[2], (Addr)arg[1]);
        break;

    case HPCMP_USERREQ__GET_VALGRIND_THREAD_ID:
        retval = tid;
        break;

    case HPCMP_USERREQ__START_TRACKING: {
        MpThreadInfo* ti = get_thread_info(tid);
        tl_assert(!ti->trackable);
        ti->trackable = True;
        break;
    }
    case HPCMP_USERREQ__PAUSE_TRACKING: {
        MpThreadInfo* ti = get_thread_info(tid);
        tl_assert(ti->trackable);
        ti->trackable = False;
        break;
    }
    default:
        tl_assert(0);
        return False;
        break;
    }

    *ret = retval;
    return True;
}

//------------------------------------------------------------//
//--- memory references                                    ---//
//------------------------------------------------------------//
static void mp_handle_write(ThreadId tid, Addr addr, UWord szB)
{
    BlockUsage* bku = find_block_usage_c(tid, addr);
    if (!bku) {
        // VG_(dmsg)("%p %luw no block\n", (void*)addr, szB);
        return;
    }

    bku->bytes_write += szB;
}

static void mp_handle_insn_write(Addr addr, UWord szB)
{
    tl_assert(g_curr_tid != VG_INVALID_THREADID);
    mp_handle_write(g_curr_tid, addr, szB);
}

static void mp_handle_read(ThreadId tid, Addr addr, UWord szB)
{
    BlockUsage* bku = find_block_usage_c(tid, addr);
    if (!bku) {
        return;
    }

    bku->bytes_read += szB;
}

static void mp_handle_insn_read(Addr addr, UWord szB)
{
    tl_assert(g_curr_tid != VG_INVALID_THREADID);
    mp_handle_read(g_curr_tid, addr, szB);
}

// Handle reads and writes by syscalls (read == kernel
// reads user space, write == kernel writes user space).
// Assumes no such read or write spans a heap block
// boundary and so we can treat it just as one giant
// read or write.
static void mp_handle_noninsn_read(
    CorePart part, ThreadId tid, const HChar* s, Addr base, SizeT size)
{
    (void)s;
    switch (part) {
    case Vg_CoreSysCall:
        mp_handle_read(tid, base, size);
        break;
    case Vg_CoreSysCallArgInMem:
        break;
    case Vg_CoreTranslate:
        break;
    default:
        tl_assert(0);
    }
}

static void mp_handle_noninsn_read_asciiz(CorePart     part,
                                          ThreadId     tid,
                                          const HChar* s,
                                          Addr         str)
{
    tl_assert(part == Vg_CoreSysCall);
    mp_handle_noninsn_read(part, tid, s, str,
                           VG_(strlen)((const HChar*)str + 1));
}

static void
mp_handle_noninsn_write(CorePart part, ThreadId tid, Addr base, SizeT size)
{
    switch (part) {
    case Vg_CoreSysCall:
    case Vg_CoreClientReq:
        mp_handle_write(tid, base, size);
        break;
    case Vg_CoreSignal:
        break;
    default:
        tl_assert(0);
    }
}

//------------------------------------------------------------//
//--- Instrumentation                                      ---//
//------------------------------------------------------------//

#define binop(_op, _arg1, _arg2) IRExpr_Binop((_op), (_arg1), (_arg2))
#define mkexpr(_tmp)             IRExpr_RdTmp((_tmp))
#define mkU32(_n)                IRExpr_Const(IRConst_U32(_n))
#define mkU64(_n)                IRExpr_Const(IRConst_U64(_n))
#define assign(_t, _e)           IRStmt_WrTmp((_t), (_e))

static void add_counter_update(IRSB* sbOut, Int n)
{
#if defined(VG_BIGENDIAN)
#define END Iend_BE
#elif defined(VG_LITTLEENDIAN)
#define END Iend_LE
#else
#error "Unknown endianness"
#endif
    // Add code to increment 'g_curr_instrs' by 'n', like this:
    //   WrTmp(t1, Load64(&g_curr_instrs))
    //   WrTmp(t2, Add64(RdTmp(t1), Const(n)))
    //   Store(&g_curr_instrs, t2)
    IRTemp  t1           = newIRTemp(sbOut->tyenv, Ity_I64);
    IRTemp  t2           = newIRTemp(sbOut->tyenv, Ity_I64);
    IRExpr* counter_addr = mkIRExpr_HWord((HWord)&g_curr_instrs);

    IRStmt* st1 = assign(t1, IRExpr_Load(END, Ity_I64, counter_addr));
    IRStmt* st2 = assign(t2, binop(Iop_Add64, mkexpr(t1), mkU64(n)));
    IRStmt* st3 = IRStmt_Store(END, counter_addr, mkexpr(t2));

    addStmtToIRSB(sbOut, st1);
    addStmtToIRSB(sbOut, st2);
    addStmtToIRSB(sbOut, st3);
}

static void
addMemEvent(IRSB* sbOut, Bool isWrite, Int szB, IRExpr* addr, Int goff_sp)
{
    IRType       tyAddr = Ity_INVALID;
    const HChar* hName  = NULL;
    void*        hAddr  = NULL;
    IRExpr**     argv   = NULL;
    IRDirty*     di     = NULL;

    const Int THRESH = 4096 * 4; // somewhat arbitrary
    const Int rz_szB = VG_STACK_REDZONE_SZB;

    tyAddr = typeOfIRExpr(sbOut->tyenv, addr);
    tl_assert(tyAddr == Ity_I32 || tyAddr == Ity_I64);

    if (isWrite) {
        hName = "mp_handle_insn_write";
        hAddr = &mp_handle_insn_write;
    } else {
        hName = "mp_handle_insn_read";
        hAddr = &mp_handle_insn_read;
    }

    argv = mkIRExprVec_2(addr, mkIRExpr_HWord(szB));

    /* Add the helper. */
    tl_assert(hName);
    tl_assert(hAddr);
    tl_assert(argv);
    di = unsafeIRDirty_0_N(2 /*regparms*/, hName, VG_(fnptr_to_fnentry)(hAddr),
                           argv);

    /* Generate the guard condition: "(addr - (SP - RZ)) >u N", for
    some arbitrary N.  If that fails then addr is in the range (SP -
    RZ .. SP + N - RZ).  If N is smallish (a page?) then we can say
    addr is within a page of SP and so can't possibly be a heap
    access, and so can be skipped. */
    IRTemp sp = newIRTemp(sbOut->tyenv, tyAddr);
    addStmtToIRSB(sbOut, assign(sp, IRExpr_Get(goff_sp, tyAddr)));

    IRTemp sp_minus_rz = newIRTemp(sbOut->tyenv, tyAddr);
    addStmtToIRSB(
        sbOut,
        assign(sp_minus_rz, tyAddr == Ity_I32
                                ? binop(Iop_Sub32, mkexpr(sp), mkU32(rz_szB))
                                : binop(Iop_Sub64, mkexpr(sp), mkU64(rz_szB))));

    IRTemp diff = newIRTemp(sbOut->tyenv, tyAddr);
    addStmtToIRSB(
        sbOut, assign(diff, tyAddr == Ity_I32
                                ? binop(Iop_Sub32, addr, mkexpr(sp_minus_rz))
                                : binop(Iop_Sub64, addr, mkexpr(sp_minus_rz))));

    IRTemp guard = newIRTemp(sbOut->tyenv, Ity_I1);
    addStmtToIRSB(
        sbOut,
        assign(guard, tyAddr == Ity_I32
                          ? binop(Iop_CmpLT32U, mkU32(THRESH), mkexpr(diff))
                          : binop(Iop_CmpLT64U, mkU64(THRESH), mkexpr(diff))));
    di->guard = mkexpr(guard);

    addStmtToIRSB(sbOut, IRStmt_Dirty(di));
}

static IRSB* mp_instrument(VgCallbackClosure*     closure,
                           IRSB*                  sbIn,
                           const VexGuestLayout*  layout,
                           const VexGuestExtents* vge,
                           const VexArchInfo*     archinfo_host,
                           IRType                 gWordTy,
                           IRType                 hWordTy)
{
    (void)closure;
    (void)vge;
    (void)(archinfo_host);
    (void)(gWordTy);
    (void)(hWordTy);
    Int        i, n = 0;
    IRSB*      sbOut;
    IRTypeEnv* tyenv = sbIn->tyenv;

    const Int goff_sp = layout->offset_SP;

    // We increment the instruction count in two places:
    // - just before any Ist_Exit statements;
    // - just before the IRSB's end.
    // In the former case, we zero 'n' and then continue instrumenting.

    sbOut = deepCopyIRSBExceptStmts(sbIn);

    // Copy verbatim any IR preamble preceding the first IMark
    i = 0;
    while (i < sbIn->stmts_used && sbIn->stmts[i]->tag != Ist_IMark) {
        addStmtToIRSB(sbOut, sbIn->stmts[i]);
        i++;
    }

    for (/*use current i*/; i < sbIn->stmts_used; i++) {
        IRStmt* st = sbIn->stmts[i];

        if (!st || st->tag == Ist_NoOp)
            continue;

        switch (st->tag) {
        case Ist_IMark: {
            n++;
            break;
        }

        case Ist_Exit: {
            if (n > 0) {
                // Add an increment before the Exit statement, then reset
                // 'n'.
                add_counter_update(sbOut, n);
                n = 0;
            }
            break;
        }

        case Ist_WrTmp: {
            IRExpr* data = st->Ist.WrTmp.data;
            if (data->tag == Iex_Load) {
                IRExpr* aexpr = data->Iex.Load.addr;
                // Note also, endianness info is ignored.  I guess
                // that's not interesting.
                addMemEvent(sbOut, False /*!isWrite*/,
                            sizeofIRType(data->Iex.Load.ty), aexpr, goff_sp);
            }
            break;
        }

        case Ist_Store: {
            IRExpr* data  = st->Ist.Store.data;
            IRExpr* aexpr = st->Ist.Store.addr;
            addMemEvent(sbOut, True /*isWrite*/,
                        sizeofIRType(typeOfIRExpr(tyenv, data)), aexpr,
                        goff_sp);
            break;
        }

        case Ist_Dirty: {
            Int      dataSize;
            IRDirty* d = st->Ist.Dirty.details;
            if (d->mFx != Ifx_None) {
                /* This dirty helper accesses memory.  Collect the details.
                 */
                tl_assert(d->mAddr != NULL);
                tl_assert(d->mSize != 0);
                dataSize = d->mSize;
                // Large (eg. 28B, 108B, 512B on x86) data-sized
                // instructions will be done inaccurately, but they're
                // very rare and this avoids errors from hitting more
                // than two cache lines in the simulation.
                if (d->mFx == Ifx_Read || d->mFx == Ifx_Modify)
                    addMemEvent(sbOut, False /*!isWrite*/, dataSize, d->mAddr,
                                goff_sp);
                if (d->mFx == Ifx_Write || d->mFx == Ifx_Modify)
                    addMemEvent(sbOut, True /*isWrite*/, dataSize, d->mAddr,
                                goff_sp);
            } else {
                tl_assert(d->mAddr == NULL);
                tl_assert(d->mSize == 0);
            }
            break;
        }

        case Ist_CAS: {
            /* We treat it as a read and a write of the location.  I
               think that is the same behaviour as it was before IRCAS
               was introduced, since prior to that point, the Vex
               front ends would translate a lock-prefixed instruction
               into a (normal) read followed by a (normal) write. */
            Int    dataSize;
            IRCAS* cas = st->Ist.CAS.details;
            tl_assert(cas->addr != NULL);
            tl_assert(cas->dataLo != NULL);
            dataSize = sizeofIRType(typeOfIRExpr(tyenv, cas->dataLo));
            if (cas->dataHi != NULL)
                dataSize *= 2; /* since it's a doubleword-CAS */
            addMemEvent(sbOut, False /*!isWrite*/, dataSize, cas->addr,
                        goff_sp);
            addMemEvent(sbOut, True /*isWrite*/, dataSize, cas->addr, goff_sp);
            break;
        }

        case Ist_LLSC: {
            IRType dataTy;
            if (st->Ist.LLSC.storedata == NULL) {
                /* LL */
                dataTy = typeOfIRTemp(tyenv, st->Ist.LLSC.result);
                addMemEvent(sbOut, False /*!isWrite*/, sizeofIRType(dataTy),
                            st->Ist.LLSC.addr, goff_sp);
            } else {
                /* SC */
                dataTy = typeOfIRExpr(tyenv, st->Ist.LLSC.storedata);
                addMemEvent(sbOut, True /*isWrite*/, sizeofIRType(dataTy),
                            st->Ist.LLSC.addr, goff_sp);
            }
            break;
        }

        default:
            break;
        }

        addStmtToIRSB(sbOut, st);
    }

    if (n > 0) {
        // Add an increment before the SB end.
        add_counter_update(sbOut, n);
    }
    return sbOut;
}

#undef binop
#undef mkexpr
#undef mkU32
#undef mkU64
#undef assign

static Bool mp_process_cmd_line_option(const HChar* arg)
{
    if VG_STR_CLO (arg, "--hpcmp-out-file", clo_mp_out_file) {
    } else {
        return VG_(replacement_malloc_process_cmd_line_option)(arg);
    }

    return True;
}

static void mp_print_usage(void)
{
    VG_(printf)("    --hpcmp-out-file=<file>    output file name\n");
}

static void mp_print_debug_usage(void) { VG_(printf)("    (none)\n"); }

static void mp_post_clo_init(void)
{
    VG_(track_pre_mem_read)(mp_handle_noninsn_read);
    VG_(track_pre_mem_read_asciiz)(mp_handle_noninsn_read_asciiz);
    VG_(track_post_mem_write)(mp_handle_noninsn_write);

    if (clo_mp_out_file) {
        VgFile* out_file = VG_(fopen)(clo_mp_out_file,
                                      VKI_O_CREAT | VKI_O_TRUNC | VKI_O_WRONLY,
                                      VKI_S_IRUSR | VKI_S_IWUSR);
        tl_assert(out_file);
        g_ev_handler = create_json_event_handler(out_file);
    } else {
        g_ev_handler = &dbg_ev_handler;
    }
}

static void mp_fini(Int exit_status)
{
    (void)exit_status;

    unset_thread_info(MAIN_TID);
    g_block_list_destroy();

    if (clo_mp_out_file) {
        delete_json_event_handler(&g_ev_handler);
    }

    if (VG_(clo_verbosity) == 0) {
        return;
    }
}

static void mp_pre_clo_init(void)
{
    VG_(details_name)("HPCMP");
    VG_(details_version)(NULL);
    VG_(details_description)("HPC memory profiler");
    VG_(details_copyright_author)
    ("Copyright (C) 2023, and GNU GPL'd by Mihai Renea.");
    VG_(details_bug_reports_to)("m.renea@fu-berlin.de");

    VG_(details_avg_translation_sizeB)(275);

    VG_(basic_tool_funcs)(mp_post_clo_init, mp_instrument, mp_fini);
    // Needs.
    VG_(needs_libc_freeres)();
    VG_(needs_cxx_freeres)();
    VG_(needs_command_line_options)(mp_process_cmd_line_option, mp_print_usage,
                                    mp_print_debug_usage);

    VG_(needs_malloc_replacement)
    (mp_malloc, mp___builtin_new, mp___builtin_new_aligned,
     mp___builtin_vec_new, mp___builtin_vec_new_aligned, mp_memalign, mp_calloc,
     mp_free, mp___builtin_delete, mp___builtin_delete_aligned,
     mp___builtin_vec_delete, mp___builtin_vec_delete_aligned, mp_realloc,
     mp_malloc_usable_size, 0);

    // TODO: Check why not init in mp_post_clo_init()
    VG_(track_start_client_code)(mp_start_client_code);
    VG_(track_stop_client_code)(mp_stop_client_code);
    VG_(track_pre_thread_ll_create)(mp_pre_thread_ll_create);
    VG_(track_pre_thread_ll_exit)(mp_pre_thread_ll_exit);

    VG_(needs_client_requests)(handle_client_request);

    g_block_list_create();

    g_thd_info_a =
        VG_(calloc)("mp.g_thd_info_a", VG_N_THREADS, sizeof(*g_thd_info_a));
}

VG_DETERMINE_INTERFACE_VERSION(mp_pre_clo_init)
