/* Test sem_cbuf_MatMul intercepts
 *
 * This file is part of HPCMP (High Performance Computing Memory Profiler),
 * a Valgrind tool for profiling HPC application memory behavior.
 *
 * Copyright (C) 2023 Mihai Renea
 *  mihai.renea@fu-berlin.de
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

#include "hpcmp_intercepts_common.h"
#include "inc_cbuf/mat_buf.h"

#define MAT_CBUF_FUNC(ret_ty, uf, implf, argl_decl, argl)                      \
    ret_ty VG_WRAP_FUNCTION_ZU(NONE, uf) argl_decl;                            \
    ret_ty VG_WRAP_FUNCTION_ZU(NONE, uf) argl_decl { return implf argl; }

static __always_inline void semcbuf_pop_MatMul_intercept(SemCbuf_MatMul* cbuf,
                                                         MatMul*         mat)
{
    OrigFn fn;
    VALGRIND_GET_ORIG_FN(fn);

    CALL_FN_v_WW(fn, cbuf, mat);

    mp_hook_post_acquire(cbuf);
}

MAT_CBUF_FUNC(void,
              semcbuf_pop_MatMul,
              semcbuf_pop_MatMul_intercept,
              (SemCbuf_MatMul * cbuf, MatMul* mat),
              (cbuf, mat));

static __always_inline void semcbuf_push_MatMul_intercept(SemCbuf_MatMul* cbuf,
                                                          MatMul*         mat)
{
    OrigFn fn;
    VALGRIND_GET_ORIG_FN(fn);

    mp_hook_pre_release(cbuf);

    CALL_FN_v_WW(fn, cbuf, mat);
}

MAT_CBUF_FUNC(void,
              semcbuf_push_MatMul,
              semcbuf_push_MatMul_intercept,
              (SemCbuf_MatMul * cbuf, MatMul* mat),
              (cbuf, mat));