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

#include "pub_tool_basics.h"
#include "pub_tool_libcprint.h"

#include "json_handler.h"

#define PRETTY_JSON 1

typedef struct {
    UInt ident;
    UInt item_cnt; // 0 for primitive values
} JsonValue;

typedef JsonValue JsonArray;
typedef JsonValue JsonObject;

typedef struct {
    MpEventHandler mp_ev_hdl;
    VgFile*        fp;
    JsonArray      base_array;
} JsonEvHandler;

#define FP(format, args...) ({ VG_(fprintf)(fp, format, ##args); })

ULong g_curr_ev_id = 0;

static void ident(VgFile* fp, JsonValue* val)
{
#if PRETTY_JSON
    for (unsigned i = 0; i < val->ident; i++) {
        FP("\t");
    }
#else
    (void)fp;
    (void)val;
#endif
}

// only makes sense to call from composite types
static JsonValue open_value(VgFile* fp, JsonValue* from)
{
    if (from) {
#if PRETTY_JSON
        if (from->item_cnt > 0) {
            FP(",\n");
        } else {
            FP("\n");
        }
#else
        if (from->item_cnt > 0) {
            FP(",");
        }
#endif

        from->item_cnt++;

        JsonValue jval = {.ident = from->ident + 1, .item_cnt = 0};
        ident(fp, &jval);

        return jval;
    } else {
        return (JsonValue){.ident = 0, .item_cnt = 0};
    }
}

static JsonValue
open_value_in_object(VgFile* fp, JsonObject* from, char const* label)
{
    JsonValue value = open_value(fp, from);
#if PRETTY_JSON
    FP("\"%s\" : ", label);
#else
    FP("\"%s\":", label);
#endif

    return value;
}

// may be ommited on primitive values
static void close_value(VgFile* fp, JsonValue* val)
{
#if PRETTY_JSON
    if (val->item_cnt > 0) {
        FP("\n");
        ident(fp, val);
    }
#else
    (void)fp;
    (void)val;
#endif
}

static JsonArray open_array(VgFile* fp, JsonValue* from)
{
    JsonArray array = open_value(fp, from);
    FP("[");
    return array;
}

static JsonArray
open_array_in_object(VgFile* fp, JsonValue* from, char const* label)
{
    JsonArray array = open_value_in_object(fp, from, label);
    FP("[");
    return array;
}

static void close_array(VgFile* fp, JsonArray* array)
{
    close_value(fp, array);
    FP("]");
}

static JsonObject open_object(VgFile* fp, JsonValue* from)
{
    JsonObject array = open_value(fp, from);
    FP("{");
    return array;
}

static JsonObject
open_object_in_object(VgFile* fp, JsonValue* from, char const* label)
{
    JsonObject object = open_value_in_object(fp, from, label);
    FP("{");
    return object;
}

static void close_object(VgFile* fp, JsonObject* object)
{
    close_value(fp, object);
    FP("}");
}

static void print_bku(VgFile* fp, BlockUsage* bku)
{
#if PRETTY_JSON
    FP("\"r\" : %8llu, \"w\" : %8llu", bku->bytes_read, bku->bytes_write);
#else
    FP("\"r\":%llu,\"w\":%llu", bku->bytes_read, bku->bytes_write);
#endif
    bku->bytes_read  = 0;
    bku->bytes_write = 0;
}

static void print_usage(VgFile* fp, JsonArray* array, BFM bfm)
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

        open_value(fp, array);
#if PRETTY_JSON
        FP("{ \"addr\" : %8lu, \"size\" : %8lu, ", bk->payload, bk->req_szB);
#else
        FP("{\"addr\":%lu,\"size\":%lu,", bk->payload, bk->req_szB);
#endif
        print_bku(fp, bku);
        FP("}");

        bku->bytes_read  = 0;
        bku->bytes_write = 0;
    }
    doneIterBFM(bfm);
}

static void
handle_sync_event(JsonEvHandler* jhdl, JsonObject* jsev, MpEvent* ev)
{
    tl_assert(ev->type == MPEV_SYNC);
    SyncEvent* syncev = &ev->sync;
    VgFile*    fp     = jhdl->fp;

    open_value_in_object(fp, jsev, sync_event_str(syncev->type));

    switch (syncev->type) {
    case SYNCEV_FORK:
    case SYNCEV_JOIN:
#if PRETTY_JSON
        FP("%8lu", syncev->fojo.child_pthid);
#else
        FP("%lu", syncev->fojo.child_pthid);
#endif
        break;

    case SYNCEV_EXIT:
        FP("null");
        break;

    case SYNCEV_ACQ:
    case SYNCEV_REL:
#if PRETTY_JSON
        FP("%8lu", syncev->barriers.addr);
#else
        FP("%lu", syncev->barriers.addr);
#endif

        break;

    default:
        tl_assert(0);
    }

    JsonArray usage = open_array_in_object(fp, jsev, "usage");
    print_usage(fp, &usage, syncev->block_cache);
    close_array(fp, &usage);
}

static void
handle_life_event(JsonEvHandler* jhdl, JsonObject* jlev, MpEvent* ev)
{
    tl_assert(ev->type == MPEV_LIFE);
    LifeEvent* lifeev = &ev->life;
    VgFile*    fp     = jhdl->fp;

    switch (lifeev->type) {
    case LIFEEV_ALLOC:
        open_value_in_object(fp, jlev, life_event_str(lifeev->type));
#if PRETTY_JSON
        FP("{ \"addr\" : %8lu, \"size\" : %8lu }", lifeev->alloc.addr,
           lifeev->alloc.size);
#else
        FP("{\"addr\":%lu,\"size\":%lu}", lifeev->alloc.addr,
           lifeev->alloc.size);
#endif
        break;

    case LIFEEV_FREE:
        open_value_in_object(fp, jlev, life_event_str(lifeev->type));
#if PRETTY_JSON
        FP("{ \"addr\" : %8lu, \"size\" : %8lu ", lifeev->free.addr,
           lifeev->free.size);
        FP(", ");
#else
        FP("{\"addr\":%lu,\"size\":%lu", lifeev->free.addr, lifeev->free.size);
        FP(",");
#endif
        print_bku(fp, lifeev->free.bku
                          ? lifeev->free.bku
                          : &(BlockUsage){.bytes_read = 0, .bytes_write = 0});
#if PRETTY_JSON
        FP(" }");
#else
        FP("}");
#endif
        break;
    case LIFEEV_NEW_SYNC:
    case LIFEEV_DEL_SYNC:
        open_value_in_object(fp, jlev, life_event_str(lifeev->type));
#if PRETTY_JSON
        FP("{ \"prim\" : \"%s\", \"addr\" : %8lu }", lifeev->sync_life.type,
           lifeev->sync_life.addr);
#else
        FP("{\"prim\":\"%s\",\"addr\":%lu}", lifeev->sync_life.type,
           lifeev->sync_life.addr);
#endif
        break;

    default:
        tl_assert(0);
    }
}

static void handle_event(MpEventHandler* self, MpEvent* ev)
{
    JsonEvHandler* jhdl = (JsonEvHandler*)self;
    VgFile*        fp   = jhdl->fp;
    JsonValue      jev_val;

    JsonObject jev = open_object(fp, &jhdl->base_array);

    open_value_in_object(fp, &jev, "thid");
    FP("%lu", ev->pthid);

    open_value_in_object(fp, &jev, "icnt");
    FP("%lu", ev->inst_cnt);

    open_value_in_object(fp, &jev, "id");
    FP("%llu", ++g_curr_ev_id);

    switch (ev->type) {
    case MPEV_INFO:
        tl_assert(ev->info);
        jev_val = open_value_in_object(fp, &jev, mp_event_str(ev->type));

        FP("\"%s\"", ev->info);

        break;
    case MPEV_LIFE:
        jev_val = open_object_in_object(fp, &jev, mp_event_str(ev->type));
        handle_life_event(jhdl, &jev_val, ev);
        close_object(fp, &jev_val);

        break;

    case MPEV_SYNC:
        jev_val = open_object_in_object(fp, &jev, mp_event_str(ev->type));
        handle_sync_event(jhdl, &jev_val, ev);
        close_object(fp, &jev_val);

        break;
    default:
        tl_assert(0);
    }

    close_object(fp, &jev);
}

static void open_json(JsonEvHandler* jhdl)
{
    jhdl->base_array = open_array(jhdl->fp, NULL);
}

static void close_json(JsonEvHandler* jhdl)
{
    close_array(jhdl->fp, &jhdl->base_array);
}

MpEventHandler* create_json_event_handler(VgFile* fp)
{
    JsonEvHandler* jhdl = VG_(malloc)("json_ev_handler", sizeof(*jhdl));
    *jhdl = (JsonEvHandler){.mp_ev_hdl = {.handle_ev = handle_event}, .fp = fp};

    open_json(jhdl);

    return (MpEventHandler*)jhdl;
}

void delete_json_event_handler(MpEventHandler** evh)
{
    tl_assert(*evh);

    JsonEvHandler* jhdl = (JsonEvHandler*)*evh;
    close_json(jhdl);

    VG_(fclose)(jhdl->fp);
    jhdl->fp = NULL;

    VG_(free)(*evh);

    *evh = NULL;
}
