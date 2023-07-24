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

#ifndef MP_BFM_H
#define MP_BFM_H

#include "pub_tool_basics.h"
#include "pub_tool_libcassert.h"
#include "pub_tool_mallocfree.h"

#include "mp.h"

static Word bi_block_cmp_fm(UWord bk1p, UWord bk2p)
{
    Block* bk1 = (Block*)bk1p;
    Block* bk2 = (Block*)bk2p;

    tl_assert(bk1->req_szB > 0);
    tl_assert(bk2->req_szB > 0);

    if (bk1->payload + bk1->req_szB <= bk2->payload)
        return -1;
    if (bk2->payload + bk2->req_szB <= bk1->payload)
        return 1;
    return 0;
}

// Some wrappers around VG_(*FM)() for type-checking
static BFM newBFM(const HChar* cc)
{
    return (BFM){.fm = VG_(newFM)(VG_(malloc), cc, VG_(free), bi_block_cmp_fm)};
}
static void deleteBFM(BFM* fm, void (*kFin)(UWord), void (*vFin)(UWord))
{
    VG_(deleteFM)(fm->fm, kFin, vFin);
    fm->fm = NULL;
}
static Bool addToBFM(BFM fm, Block* bk, BlockUsage* bku)
{
    return VG_(addToFM)(fm.fm, (UWord)bk, (UWord)bku);
}
static Bool
delFromBFM(BFM fm, Block** bk, BlockUsage** bku, Addr key, SizeT len)
{
    Block dummy = {.payload = key, .req_szB = len};
    return VG_(delFromFM)(fm.fm, (UWord*)bk, (UWord*)bku, (UWord)&dummy);
}
static Bool lookupBFM(BFM fm, Block** bk, BlockUsage** bku, Addr key)
{
    Block dummy = {.payload = key, .req_szB = 1};
    return VG_(lookupFM)(fm.fm, (UWord*)bk, (UWord*)bku, (UWord)&dummy);
}
static void initIterAtBFM(BFM fm, Addr start_at)
{
    Block dummy = {.payload = start_at, .req_szB = 1};
    VG_(initIterAtFM)(fm.fm, (UWord)&dummy);
}
static void initIterBFM(BFM fm) { VG_(initIterFM)(fm.fm); }
static Bool nextIterBFM(BFM fm, Block** bk, BlockUsage** bku)
{
    return VG_(nextIterFM)(fm.fm, (UWord*)bk, (UWord*)bku);
}
static void doneIterBFM(BFM fm) { VG_(doneIterFM)(fm.fm); }

#endif /* MP_BFM_H */