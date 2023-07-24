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

#ifndef MP_H
#define MP_H

#include "pub_tool_wordfm.h"

typedef struct {
    WordFM* fm;
} BFM;

typedef enum { BLOCK_ALIVE = 0, BLOCK_FREED, BLOCK_REALLOC } BlockState;

typedef struct {
    Addr       payload;
    SizeT      req_szB;
    BlockState state;
    // Note: we refc in host's logic
    UInt refc;
} Block;

typedef struct {
    ULong bytes_read;
    ULong bytes_write;
} BlockUsage;

static Bool block_used(BlockUsage const* bku)
{
    return bku->bytes_read > 0 || bku->bytes_write > 0;
}

typedef UWord PThreadId;
#define INVALID_POSIX_THREADID ((PThreadId)0)

#endif /* MP_H */