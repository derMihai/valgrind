/* This file is part of HPCMP (High Performance Computing Memory Profiler),
 * a Valgrind tool for profiling HPC application memory behavior.
 *
 * Copyright (C) 2023 Mihai Renea
 *    mihai.renea@fu-berlin.de
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

#include "pub_tool_libcbase.h"
#include "pub_tool_libcprint.h"

#include "dbg_ev_handler.h"

static void dbg_print_bku(BlockUsage* bku, Addr a, SizeT size)
{
    VG_(dmsg)("         | %p, %8lu, r=%8llu, w=%8llu\n", (void*)a, size,
              bku->bytes_read, bku->bytes_write);
    bku->bytes_read  = 0;
    bku->bytes_write = 0;
}

static void print_usage(BFM bfm)
{
    Block*      bk  = NULL;
    BlockUsage* bku = NULL;

    initIterBFM(bfm);
    while (nextIterBFM(bfm, &bk, &bku)) {
        tl_assert(bk);
        tl_assert(bku);

        if (!block_used(bku)) {
            continue;
        }

        VG_(dmsg)("         | %p %8lu, r=%8llu, w=%8llu%c\n",
                  (void*)bk->payload, bk->req_szB, bku->bytes_read,
                  bku->bytes_write, " *#"[bk->state]);

        bku->bytes_read  = 0;
        bku->bytes_write = 0;
    }
    doneIterBFM(bfm);
}

static void dbg_handle_life_event(MpEvent* ev)
{
    tl_assert(ev->type == MPEV_LIFE);
    LifeEvent* lifeev = &ev->life;

    VG_(dmsg)("%s: ", life_event_str(lifeev->type));

    switch (lifeev->type) {
    case LIFEEV_ALLOC:
        VG_(dmsg)("%p %8lu\n", (void*)lifeev->alloc.addr, lifeev->alloc.size);
        break;
    case LIFEEV_FREE: {
        VG_(dmsg)("%p\n", (void*)lifeev->free.addr);
        if (lifeev->free.bku) {
            dbg_print_bku(lifeev->free.bku, lifeev->free.addr,
                          lifeev->free.size);
        }
        break;
    }
    case LIFEEV_NEW_SYNC:
        VG_(dmsg)("%6s %p\n", lifeev->sync_life.type,
                  (void*)lifeev->sync_life.addr);
        break;
    case LIFEEV_DEL_SYNC:
        VG_(dmsg)("%6s %p\n", lifeev->sync_life.type,
                  (void*)lifeev->sync_life.addr);
        break;
    default:
        tl_assert(0);
    }
}

static void dbg_handle_sync_event(MpEvent* ev)
{
    tl_assert(ev->type == MPEV_SYNC);
    SyncEvent* syncev = &ev->sync;

    VG_(dmsg)("%s: ", sync_event_str(syncev->type));

    switch (syncev->type) {
    case SYNCEV_FORK: {
        VG_(dmsg)("-> %8lu, usage:\n", syncev->fojo.child_pthid);
        print_usage(syncev->block_cache);
        break;
    }
    case SYNCEV_JOIN: {
        VG_(dmsg)("-> %8lu, usage:\n", syncev->fojo.child_pthid);
        print_usage(syncev->block_cache);
        break;
    }
    case SYNCEV_EXIT: {
        VG_(dmsg)("\n");
        print_usage(syncev->block_cache);
        break;
    }
    case SYNCEV_ACQ:
        VG_(dmsg)("%p\n", (void*)syncev->barriers.addr);
        print_usage(syncev->block_cache);
        break;
    case SYNCEV_REL:
        VG_(dmsg)("%p\n", (void*)syncev->barriers.addr);
        print_usage(syncev->block_cache);
        break;
    default:
        tl_assert(0);
    }
}

static void dbg_handle_event(MpEventHandler* self, MpEvent* ev)
{
    (void)self;

    VG_(dmsg)("%8lu %4s icnt=%8lu ", ev->pthid, mp_event_str(ev->type),
              ev->inst_cnt);

    switch (ev->type) {
    case MPEV_INFO:
        tl_assert(ev->info);
        VG_(dmsg)("%s\n", ev->info);
        break;
    case MPEV_LIFE:
        dbg_handle_life_event(ev);
        break;
    case MPEV_SYNC:
        dbg_handle_sync_event(ev);
        break;
    default:
        tl_assert(0);
    }
}

MpEventHandler dbg_ev_handler = {.handle_ev = dbg_handle_event};