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

#ifndef MP_EV_H
#define MP_EV_H

#include "mp.h"
#include "mp_bfm.h"

typedef enum {
    MPEV_INFO = 0,
    MPEV_LIFE,
    MPEV_SYNC,

    MPEV_ENUM_SIZE
} MpEventType;

static char const* mp_event_str(MpEventType t)
{
    char const* const str[MPEV_ENUM_SIZE] = {
        [MPEV_INFO] = "info", [MPEV_LIFE] = "life", [MPEV_SYNC] = "sync"};

    tl_assert((UInt)t < MPEV_ENUM_SIZE);
    return str[t];
}

typedef enum {
    SYNCEV_FORK = 0,
    SYNCEV_JOIN,
    SYNCEV_EXIT,
    SYNCEV_ACQ,
    SYNCEV_REL,

    SYNCEV_ENUM_SIZE
} SyncEvType;

static char const* sync_event_str(SyncEvType t)
{
    char const* const str[SYNCEV_ENUM_SIZE] = {[SYNCEV_FORK] = "fork",
                                               [SYNCEV_JOIN] = "join",
                                               [SYNCEV_EXIT] = "exit",
                                               [SYNCEV_ACQ]  = "acq",
                                               [SYNCEV_REL]  = "rel"};

    tl_assert((UInt)t < SYNCEV_ENUM_SIZE);
    return str[t];
}

typedef enum {
    LIFEEV_ALLOC = 0,
    LIFEEV_FREE,
    LIFEEV_NEW_SYNC,
    LIFEEV_DEL_SYNC,

    LIFEEV_ENUM_SIZE
} LifeEvType;

static char const* life_event_str(LifeEvType t)
{
    char const* const str[LIFEEV_ENUM_SIZE] = {[LIFEEV_ALLOC]    = "alloc",
                                               [LIFEEV_FREE]     = "free",
                                               [LIFEEV_NEW_SYNC] = "newsync",
                                               [LIFEEV_DEL_SYNC] = "delsync"};

    tl_assert((UInt)t < LIFEEV_ENUM_SIZE);
    return str[t];
}

typedef struct {
    SyncEvType type;
    BFM        block_cache;
    union {
        struct {
            PThreadId child_pthid;
        } fojo;
        struct {
            Addr addr;
        } barriers;
    };
} SyncEvent;

typedef struct {
    LifeEvType type;
    union {
        struct {
            Addr  addr;
            SizeT size;
        } alloc;
        struct {
            Addr        addr;
            SizeT       size;
            BlockUsage* bku; // may be NULL
        } free;
        struct {
            Addr        addr;
            char const* type;
        } sync_life;
    };
} LifeEvent;

typedef struct {
    MpEventType type;
    PThreadId   pthid;
    SizeT       inst_cnt; // since last event on this thread
    union {
        HChar const* info;
        SyncEvent    sync;
        LifeEvent    life;
    };
} MpEvent;

typedef struct MpEventHandler MpEventHandler;
struct MpEventHandler {
    void (*handle_ev)(MpEventHandler*, MpEvent*);
};

#endif /* MP_EV_H */