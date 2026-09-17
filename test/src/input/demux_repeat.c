/*****************************************************************************
 * demux_repeat.c: check that a demuxer hands over the same span twice
 *****************************************************************************
 * Copyright (C) 2026 VLC authors and VideoLAN
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

/* When an item repeats in place the input seeks the demuxer back to the start
 * and keeps the clock running, so a pass that hands over less than the one
 * before it is not heard as a gap - the next pass simply begins where the last
 * one stopped. The sliver that went missing is never played, and the drift it
 * leaves behind gathers loop after loop until the output's correction
 * saturates and the tone detunes. The only thing worth checking is therefore
 * the data itself: demux a sample to the end, note where each elementary
 * stream starts and how far its data reaches, seek back the way a repeat does,
 * and demand the same two numbers the second time. No clock, no output and no
 * decoder take part. */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

#include <vlc_common.h>
#include <vlc_access.h>
#include <vlc_block.h>
#include <vlc_demux.h>
#include <vlc_es_out.h>
#include <vlc_arrays.h>
#include <vlc_url.h>

#include "../lib/libvlc_internal.h"

#include "common.h"

#define PASSES 2

/* One elementary stream, followed across both passes. Streams are matched by
 * the order in which the demuxer declares them, which covers both the
 * demuxers that keep their elementary streams over a seek and those that drop
 * and declare them again. */
struct stream_record
{
    vlc_tick_t i_first[PASSES];   /* date on the first block handed over */
    vlc_tick_t i_end[PASSES];     /* furthest point the data reaches */
    unsigned   i_blocks[PASSES];
    int        i_cat;
};

struct es_out_id_t
{
    struct es_out_id_t *next;
    unsigned slot;
};

struct test_es_out_t
{
    struct es_out_t out;
    struct es_out_id_t *ids;

    struct stream_record *records;
    unsigned i_records;
    unsigned i_added;             /* streams declared so far in this pass */
    unsigned i_pass;
};

static struct stream_record *RecordGet( struct test_es_out_t *ctx,
                                        unsigned slot )
{
    if( slot >= ctx->i_records )
    {
        struct stream_record *n = realloc( ctx->records,
                                           ( slot + 1 ) * sizeof( *n ) );
        if( unlikely( n == NULL ) )
            abort();
        ctx->records = n;

        while( ctx->i_records <= slot )
        {
            struct stream_record *r = &ctx->records[ctx->i_records++];
            for( unsigned i = 0; i < PASSES; i++ )
            {
                r->i_first[i] = VLC_TICK_INVALID;
                r->i_end[i] = VLC_TICK_INVALID;
                r->i_blocks[i] = 0;
            }
            r->i_cat = UNKNOWN_ES;
        }
    }
    return &ctx->records[slot];
}

static es_out_id_t *EsOutAdd( es_out_t *out, const es_format_t *fmt )
{
    struct test_es_out_t *ctx = (struct test_es_out_t *) out;

    if( fmt->i_group < 0 )
        return NULL;

    es_out_id_t *id = malloc( sizeof( *id ) );
    if( unlikely( id == NULL ) )
        return NULL;

    id->slot = ctx->i_added++;
    id->next = ctx->ids;
    ctx->ids = id;

    struct stream_record *r = RecordGet( ctx, id->slot );
    r->i_cat = fmt->i_cat;
    return id;
}

static int EsOutSend( es_out_t *out, es_out_id_t *id, block_t *block )
{
    struct test_es_out_t *ctx = (struct test_es_out_t *) out;
    struct stream_record *r = RecordGet( ctx, id->slot );
    const unsigned p = ctx->i_pass;

    /* A block carries a decode date, a presentation date, or both. Whichever
     * is there is where this data sits on the timeline. */
    vlc_tick_t i_date = block->i_dts != VLC_TICK_INVALID ? block->i_dts
                                                         : block->i_pts;

    if( i_date != VLC_TICK_INVALID )
    {
        if( r->i_first[p] == VLC_TICK_INVALID )
            r->i_first[p] = i_date;

        vlc_tick_t i_end = i_date + block->i_length;
        if( r->i_end[p] == VLC_TICK_INVALID || i_end > r->i_end[p] )
            r->i_end[p] = i_end;
    }
    r->i_blocks[p]++;

    block_Release( block );
    return VLC_SUCCESS;
}

static void EsOutDelete( es_out_t *out, es_out_id_t *id )
{
    struct test_es_out_t *ctx = (struct test_es_out_t *) out;
    es_out_id_t **pp = &ctx->ids;

    while( *pp != id )
    {
        if( *pp == NULL )
            abort();
        pp = &( ( *pp )->next );
    }

    *pp = id->next;
    free( id );
}

static int EsOutControl( es_out_t *out, int query, va_list args )
{
    (void) out;

    switch( query )
    {
        case ES_OUT_SET_ES:
        case ES_OUT_SET_ES_DEFAULT:
        case ES_OUT_SET_ES_STATE:
        case ES_OUT_SET_ES_CAT_POLICY:
        case ES_OUT_RESTART_ES:
        case ES_OUT_SET_PCR:
        case ES_OUT_SET_GROUP_PCR:
        case ES_OUT_RESET_PCR:
        case ES_OUT_SET_ES_FMT:
        case ES_OUT_SET_NEXT_DISPLAY_TIME:
        case ES_OUT_SET_GROUP_META:
        case ES_OUT_SET_GROUP_EPG:
        case ES_OUT_DEL_GROUP:
        case ES_OUT_SET_GROUP:
        case ES_OUT_SET_ES_SCRAMBLED_STATE:
        case ES_OUT_SET_META:
            break;
        case ES_OUT_GET_ES_STATE:
            va_arg( args, es_out_id_t * );
            *va_arg( args, bool * ) = true;
            break;
        case ES_OUT_GET_EMPTY:
            *va_arg( args, bool * ) = true;
            break;
        default:
            return VLC_EGENERIC;
    }
    return VLC_SUCCESS;
}

static void EsOutDestroy( es_out_t *out )
{
    struct test_es_out_t *ctx = (struct test_es_out_t *) out;
    es_out_id_t *id;

    while( ( id = ctx->ids ) != NULL )
    {
        ctx->ids = id->next;
        free( id );
    }
    free( ctx->records );
    free( ctx );
}

static struct test_es_out_t *test_es_out_create( vlc_object_t *parent )
{
    struct test_es_out_t *ctx = malloc( sizeof( *ctx ) );
    if( unlikely( ctx == NULL ) )
        return NULL;

    ctx->ids = NULL;
    ctx->records = NULL;
    ctx->i_records = 0;
    ctx->i_added = 0;
    ctx->i_pass = 0;

    es_out_t *out = &ctx->out;
    out->pf_add = EsOutAdd;
    out->pf_send = EsOutSend;
    out->pf_del = EsOutDelete;
    out->pf_control = EsOutControl;
    out->pf_destroy = EsOutDestroy;
    out->p_sys = (void *) parent;

    return ctx;
}

static const char *CatName( int i_cat )
{
    switch( i_cat )
    {
        case VIDEO_ES: return "video";
        case AUDIO_ES: return "audio";
        case SPU_ES:   return "spu";
        default:       return "es";
    }
}

/* Demux everything the demuxer has left to give. */
static int RunPass( demux_t *demux, struct test_es_out_t *ctx, unsigned pass )
{
    ctx->i_pass = pass;
    ctx->i_added = 0;

    int val;
    unsigned long i = 0;

    while( ( val = demux_Demux( demux ) ) == VLC_DEMUXER_SUCCESS )
        if( ++i > 100000 )
            break;      /* a demuxer that never ends is a failure of its own */

    return val == VLC_DEMUXER_EOF ? 0 : -1;
}

/* Seek back to the start exactly the way a repeating item does. */
static int SeekToStart( demux_t *demux )
{
    if( demux_Control( demux, DEMUX_SET_POSITION, 0.0, true ) == VLC_SUCCESS )
        return 0;
    if( demux_Control( demux, DEMUX_SET_TIME, (vlc_tick_t) 0, true )
        == VLC_SUCCESS )
        return 0;
    return -1;
}

/* A quarter of a second of a 440 Hz tone, one container each, small enough to
 * live in the tree. Each is demuxed by the module named beside it rather than
 * by whichever module wins the probe, so that this covers the demuxers it
 * means to cover however the tree was configured. */
static const struct
{
    const char *psz_file;
    const char *psz_demux;
} samples[] = {
    { "tone.wav",  "wav"      },
    { "tone.aiff", "aiff"     },
    { "tone.au",   "au"       },
    { "tone.voc",  "voc"      },
    { "tone.flac", "flacsys"  },
    { "tone.tta",  "tta"      },
    { "tone.mp3",  "mpga"     },
    { "tone.ogg",  "ogg"      },
    { "tone.opus", "ogg"      },
    /* Two complete ogg streams one after the other, which is all a chained
     * file is. The second group's timeline carries on from the first, so a
     * repeat has to put it back where it began. */
    { "chain.opus", "ogg"     },
    { "tone.m4a",  "mp4"      },
    { "tone.mka",  "mkv"      },
    { "tone.avi",  "avi"      },
    { "tone.asf",  "asf"      },
    { "tone.ts",   "ts"       },
    { "tone.mpg",  "ps"       },
    /* The library demuxer carries most of what people actually play, so it
     * gets a turn of its own. */
    { "tone.mp3",  "avformat" },
};

static int TestSample( libvlc_instance_t *vlc, const char *psz_file,
                       const char *psz_demux, bool *pb_ran )
{
    char psz_path[PATH_MAX];
    snprintf( psz_path, sizeof( psz_path ), SRCDIR "/samples/repeat/%s",
              psz_file );

    char *psz_url = vlc_path2uri( psz_path, NULL );
    if( psz_url == NULL )
        return -1;

    stream_t *s = vlc_access_NewMRL( VLC_OBJECT( vlc->p_libvlc_int ),
                                     psz_url );
    free( psz_url );
    if( s == NULL )
    {
        fprintf( stderr, "%s (%s): cannot read %s\n",
                 psz_file, psz_demux, psz_path );
        return -1;
    }

    struct test_es_out_t *ctx = test_es_out_create( VLC_OBJECT( s ) );
    if( ctx == NULL )
    {
        vlc_stream_Delete( s );
        return -1;
    }

    demux_t *demux = demux_New( VLC_OBJECT( s ), psz_demux, "", s, &ctx->out );
    if( demux == NULL )
    {
        es_out_Delete( &ctx->out );
        vlc_stream_Delete( s );
        /* A module the tree was not configured to build is not a failure of
         * the demuxer, but say so loudly: a run where everything is skipped
         * proves nothing. */
        fprintf( stderr, "%-10s %-9s: SKIP, module not built\n",
                 psz_file, psz_demux );
        return 0;
    }

    *pb_ran = true;

    int ret = 0;
    bool b_can_seek = false;
    demux_Control( demux, DEMUX_CAN_SEEK, &b_can_seek );

    if( RunPass( demux, ctx, 0 ) != 0 )
    {
        fprintf( stderr, "%s (%s): the first pass did not reach the end\n",
                 psz_file, psz_demux );
        ret = -1;
    }
    else if( !b_can_seek )
    {
        fprintf( stderr, "%s (%s): cannot seek, so it cannot repeat in "
                 "place\n", psz_file, psz_demux );
        ret = -1;
    }
    else if( SeekToStart( demux ) != 0 )
    {
        fprintf( stderr, "%s (%s): the seek back to the start failed\n",
                 psz_file, psz_demux );
        ret = -1;
    }
    else if( RunPass( demux, ctx, 1 ) != 0 )
    {
        fprintf( stderr, "%s (%s): the repeated pass did not reach the end\n",
                 psz_file, psz_demux );
        ret = -1;
    }
    else for( unsigned i = 0; i < ctx->i_records; i++ )
    {
        const struct stream_record *r = &ctx->records[i];

        if( r->i_blocks[0] == 0 )
            continue;   /* a stream the demuxer declared but never fed */

        if( r->i_blocks[1] == 0 )
        {
            fprintf( stderr, "%s (%s) %s: the repeated pass handed over "
                     "nothing at all\n",
                     psz_file, psz_demux, CatName( r->i_cat ) );
            ret = -1;
            continue;
        }

        if( r->i_first[0] != r->i_first[1] )
        {
            fprintf( stderr, "%s (%s) %s: the repeated pass begins at "
                     "%"PRId64" us where the first began at %"PRId64" us, so "
                     "%"PRId64" us of the item is never played (%u blocks "
                     "against %u)\n",
                     psz_file, psz_demux, CatName( r->i_cat ),
                     r->i_first[1], r->i_first[0],
                     r->i_first[1] - r->i_first[0],
                     r->i_blocks[1], r->i_blocks[0] );
            ret = -1;
        }

        if( r->i_end[0] != r->i_end[1] )
        {
            fprintf( stderr, "%s (%s) %s: the repeated pass reaches "
                     "%"PRId64" us where the first reached %"PRId64" us, a "
                     "%"PRId64" us difference (%u blocks against %u)\n",
                     psz_file, psz_demux, CatName( r->i_cat ),
                     r->i_end[1], r->i_end[0], r->i_end[1] - r->i_end[0],
                     r->i_blocks[1], r->i_blocks[0] );
            ret = -1;
        }
    }

    if( ret == 0 && ctx->i_records > 0 )
        fprintf( stderr, "%-10s %-9s: %"PRId64" us .. %"PRId64" us, twice "
                 "over\n",
                psz_file, psz_demux,
                ctx->records[0].i_first[0], ctx->records[0].i_end[0] );

    demux_Delete( demux );
    es_out_Delete( &ctx->out );
    return ret;
}

int main( void )
{
    struct vlc_run_args args;
    vlc_run_args_init( &args );

    libvlc_instance_t *vlc = libvlc_create( &args );
    if( vlc == NULL )
        return 77;

    int ret = 0;
    bool b_ran = false;

    for( size_t i = 0; i < ARRAY_SIZE( samples ); i++ )
        if( TestSample( vlc, samples[i].psz_file, samples[i].psz_demux,
                        &b_ran ) != 0 )
            ret = 1;

    if( !b_ran )
    {
        fprintf( stderr, "no demuxer under test was built\n" );
        ret = 77;
    }

    libvlc_release( vlc );
    return ret;
}
