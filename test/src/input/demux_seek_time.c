/*****************************************************************************
 * demux_seek_time.c: check that a seek lands where the demuxer then says it is
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

/* A seek states a time as well as a position: the demuxer dates the frames it
 * hands over next, and everything downstream - the reported position, the
 * remaining time, the seek bar - believes it. Nothing checked that the date
 * was true. A demuxer that locates a byte offset from an average bitrate and
 * then reads a time back out of that same average is exact only while the
 * bitrate is constant; on a variable-bitrate stream it can be seconds out,
 * and the error is invisible because it is consistent with itself.
 *
 * What is measurable without a clock or a decoder is that the data still
 * reaches the same end of the timeline. Demux a sample from one end to the
 * other and note how far its data goes. Then seek, demux what is left, and
 * demand the same answer: the frames after the seek are dated from where the
 * seek said it landed, so if that date is wrong the stream now ends somewhere
 * else - early, and the remaining time runs out before the audio does.
 *
 * samples/seek/vbr.mp3 is a 16 kHz MPEG-2 layer III stream of 236 frames
 * carrying a Xing header with a table of contents: 0.3 s encoded at 160 kb/s
 * followed by 8 s at 8 kb/s. Its mean bitrate describes neither half, so a
 * byte offset interpolated from the mean is nowhere near the time it is
 * claimed to be. */

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
#include <vlc_url.h>

#include "../lib/libvlc_internal.h"

#include "common.h"

/* How far the reported end of a stream may move when it is reached from a
 * seek instead of from the start. One frame of anything is unavoidable; the
 * errors this is looking for are whole seconds. */
#define TOLERANCE VLC_TICK_FROM_MS(250)

struct test_es_out_t
{
    struct es_out_t out;
    vlc_tick_t i_first;
    vlc_tick_t i_end;
    unsigned i_blocks;
};

struct es_out_id_t
{
    char dummy;
};

static es_out_id_t *EsOutAdd( es_out_t *out, const es_format_t *fmt )
{
    (void) out;
    if( fmt->i_cat != AUDIO_ES && fmt->i_cat != VIDEO_ES )
        return NULL;
    return malloc( sizeof( es_out_id_t ) );
}

static int EsOutSend( es_out_t *out, es_out_id_t *id, block_t *block )
{
    struct test_es_out_t *ctx = (struct test_es_out_t *) out;
    (void) id;

    vlc_tick_t i_date = block->i_dts != VLC_TICK_INVALID ? block->i_dts
                                                         : block->i_pts;
    if( i_date != VLC_TICK_INVALID )
    {
        if( ctx->i_first == VLC_TICK_INVALID )
            ctx->i_first = i_date;

        vlc_tick_t i_end = i_date + block->i_length;
        if( ctx->i_end == VLC_TICK_INVALID || i_end > ctx->i_end )
            ctx->i_end = i_end;
    }
    ctx->i_blocks++;

    block_Release( block );
    return VLC_SUCCESS;
}

static void EsOutDelete( es_out_t *out, es_out_id_t *id )
{
    (void) out;
    free( id );
}

static int EsOutControl( es_out_t *out, int query, va_list args )
{
    (void) out;

    switch( query )
    {
        case ES_OUT_GET_ES_STATE:
            va_arg( args, es_out_id_t * );
            *va_arg( args, bool * ) = true;
            break;
        case ES_OUT_GET_EMPTY:
            *va_arg( args, bool * ) = true;
            break;
        default:
            break;
    }
    return VLC_SUCCESS;
}

static void EsOutDestroy( es_out_t *out )
{
    free( (struct test_es_out_t *) out );
}

static struct test_es_out_t *test_es_out_create( void )
{
    struct test_es_out_t *ctx = malloc( sizeof( *ctx ) );
    if( unlikely( ctx == NULL ) )
        return NULL;

    ctx->i_first = VLC_TICK_INVALID;
    ctx->i_end = VLC_TICK_INVALID;
    ctx->i_blocks = 0;

    es_out_t *out = &ctx->out;
    out->pf_add = EsOutAdd;
    out->pf_send = EsOutSend;
    out->pf_del = EsOutDelete;
    out->pf_control = EsOutControl;
    out->pf_destroy = EsOutDestroy;
    out->p_sys = NULL;

    return ctx;
}

static void RunToEnd( demux_t *demux )
{
    unsigned long i = 0;

    while( demux_Demux( demux ) == VLC_DEMUXER_SUCCESS )
        if( ++i > 100000 )
            break;
}

/* Where the data of a whole pass ends, having seeked to f_pos first, or
 * VLC_TICK_INVALID if the sample could not be played at all. Passing a
 * negative position plays it from the start. */
static vlc_tick_t EndOfPass( libvlc_instance_t *vlc, const char *psz_file,
                             const char *psz_demux, double f_pos,
                             bool *pb_built )
{
    char psz_path[PATH_MAX];
    snprintf( psz_path, sizeof( psz_path ), SRCDIR "/samples/seek/%s",
              psz_file );

    char *psz_url = vlc_path2uri( psz_path, NULL );
    if( psz_url == NULL )
        return VLC_TICK_INVALID;

    stream_t *s = vlc_access_NewMRL( VLC_OBJECT( vlc->p_libvlc_int ),
                                     psz_url );
    free( psz_url );
    if( s == NULL )
    {
        fprintf( stderr, "cannot read %s\n", psz_path );
        return VLC_TICK_INVALID;
    }

    struct test_es_out_t *ctx = test_es_out_create();
    if( ctx == NULL )
    {
        vlc_stream_Delete( s );
        return VLC_TICK_INVALID;
    }

    demux_t *demux = demux_New( VLC_OBJECT( s ), psz_demux, "", s, &ctx->out );
    if( demux == NULL )
    {
        es_out_Delete( &ctx->out );
        vlc_stream_Delete( s );
        return VLC_TICK_INVALID;
    }
    *pb_built = true;

    vlc_tick_t i_end = VLC_TICK_INVALID;

    if( f_pos < 0.0 ||
        demux_Control( demux, DEMUX_SET_POSITION, f_pos, true )
        == VLC_SUCCESS )
    {
        RunToEnd( demux );
        if( ctx->i_blocks > 0 )
            i_end = ctx->i_end;
    }
    else
        fprintf( stderr, "%s: the seek to %.2f failed\n", psz_file, f_pos );

    demux_Delete( demux );
    es_out_Delete( &ctx->out );
    return i_end;
}

/* 0.0 is the seek a repeating item makes: the new path must put the stream
 * back exactly where a pass from the start begins. */
static const double pf_positions[] = { 0.0, 0.25, 0.5, 0.75, 0.9 };

static const struct
{
    const char *psz_file;
    const char *psz_demux;
} samples[] = {
    { "vbr.mp3", "mpga" },
};

int main( void )
{
    struct vlc_run_args args;
    vlc_run_args_init( &args );

    libvlc_instance_t *vlc = libvlc_create( &args );
    if( vlc == NULL )
        return 77;

    int ret = 0;
    bool b_built = false;

    for( size_t i = 0; i < ARRAY_SIZE( samples ); i++ )
    {
        const char *psz_file = samples[i].psz_file;
        const char *psz_demux = samples[i].psz_demux;

        vlc_tick_t i_whole = EndOfPass( vlc, psz_file, psz_demux, -1.0,
                                        &b_built );
        if( !b_built )
        {
            fprintf( stderr, "%-10s %-9s: SKIP, module not built\n",
                     psz_file, psz_demux );
            continue;
        }
        if( i_whole == VLC_TICK_INVALID )
        {
            fprintf( stderr, "%s (%s): nothing was demuxed\n",
                     psz_file, psz_demux );
            ret = 1;
            continue;
        }

        for( size_t j = 0; j < ARRAY_SIZE( pf_positions ); j++ )
        {
            vlc_tick_t i_end = EndOfPass( vlc, psz_file, psz_demux,
                                          pf_positions[j], &b_built );
            if( i_end == VLC_TICK_INVALID )
            {
                ret = 1;
                continue;
            }

            vlc_tick_t i_off = i_end - i_whole;
            if( i_off > TOLERANCE || i_off < -TOLERANCE )
            {
                fprintf( stderr, "%s (%s): seeking to %.2f makes the stream "
                         "end at %"PRId64" us where playing it whole ends at "
                         "%"PRId64" us, %"PRId64" us out\n",
                         psz_file, psz_demux, pf_positions[j],
                         i_end, i_whole, i_off );
                ret = 1;
            }
            else
                fprintf( stderr, "%-10s %-9s: seek to %.2f ends %"PRId64
                         " us out\n", psz_file, psz_demux, pf_positions[j],
                         i_off );
        }
    }

    if( !b_built )
    {
        fprintf( stderr, "no demuxer under test was built\n" );
        ret = 77;
    }

    libvlc_release( vlc );
    return ret;
}
