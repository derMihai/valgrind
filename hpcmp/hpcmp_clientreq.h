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

#ifndef HPCMP_CLIENTREQ_H
#define HPCMP_CLIENTREQ_H

#include "valgrind.h"

enum {
    HPCMP_USERREQ__THREAD_CREATE = VG_USERREQ_TOOL_BASE('M', 'P'),
    HPCMP_USERREQ__THREAD_JOIN,
    HPCMP_USERREQ__POST_ACQUIRE,
    HPCMP_USERREQ__PRE_RELEASE,
    HPCMP_USERREQ__PRIM_INIT,
    HPCMP_USERREQ__PRIM_DESTROY,
    HPCMP_USERREQ__GET_VALGRIND_THREAD_ID,
    HPCMP_USERREQ__START_TRACKING,
    HPCMP_USERREQ__PAUSE_TRACKING
};

#endif /* HPCMP_CLIENTREQ_H */