/*****************************************************************************
 * mpgv.c : MPEG-I/II Video demuxer
 *****************************************************************************
 * Copyright (C) 2001-2004 VLC authors and VideoLAN
 * $Id$
 *
 * Authors: Laurent Aimar <fenrir@via.ecp.fr>
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

/*****************************************************************************
 * Preamble
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_demux.h>
#include <vlc_codec.h>

/*****************************************************************************
 * Module descriptor
 *****************************************************************************/
static int  Open ( vlc_object_t * );
static void Close( vlc_object_t * );

vlc_module_begin ()
    set_category( CAT_INPUT )
    set_subcategory( SUBCAT_INPUT_DEMUX )
    set_description( N_("MPEG-I/II video demuxer" ) )
    set_capability( "demux", 5 )
    set_callbacks( Open, Close )
    add_shortcut( "mpgv" )
vlc_module_end ()

/*****************************************************************************
 * Local prototypes
 *****************************************************************************/
struct demux_sys_t
{
    bool  b_start;

    es_out_id_t *p_es;

    /* The last pcr handed to the output, which is what the stream has got to,
     * plus where the last seek put that point on the timeline. */
    vlc_tick_t i_pcr;
    vlc_tick_t i_time_offset;
    /* Bytes and time handed over, from which the bitrate the length and the
     * seek landing point are taken. Only the run before the first seek counts:
     * afterwards the two would be measured from an offset derived from the
     * estimate itself. */
    uint64_t i_bytes;
    int64_t  i_bitrate_avg;
    bool     b_seeked;
    /* A time seek asked for before anything had been read, which is what
     * --start-time does. There is no bitrate yet to turn it into a byte
     * position, so it is held here and answered from the first Demux(). */
    vlc_tick_t i_start_time;

    decoder_t *p_packetizer;
};

static int Demux( demux_t * );
static int Control( demux_t *, int, va_list );
static void PostSeekReset( demux_t *, vlc_tick_t );

#define MPGV_PACKET_SIZE 4096
#define MPGV_PROBE_LENGTH (2 * CLOCK_FREQ)

/*****************************************************************************
 * Close: frees unused data
 *****************************************************************************/
static void Close( vlc_object_t * p_this )
{
    demux_t     *p_demux = (demux_t*)p_this;
    demux_sys_t *p_sys = p_demux->p_sys;

    demux_PacketizerDestroy( p_sys->p_packetizer );
    free( p_sys );
}

/*****************************************************************************
 * Open: initializes demux structures
 *****************************************************************************/
static int Open( vlc_object_t * p_this )
{
    demux_t     *p_demux = (demux_t*)p_this;
    demux_sys_t *p_sys;
    bool   b_forced = false;

    const uint8_t *p_peek;

    es_format_t  fmt;

    if( vlc_stream_Peek( p_demux->s, &p_peek, 4 ) < 4 )
    {
        msg_Dbg( p_demux, "cannot peek" );
        return VLC_EGENERIC;
    }

    if( p_demux->obj.force )
        b_forced = true;

    if( p_peek[0] != 0x00 || p_peek[1] != 0x00 || p_peek[2] != 0x01 )
    {
        if( !b_forced ) return VLC_EGENERIC;

        msg_Err( p_demux, "this doesn't look like an MPEG ES stream, continuing" );
    }

    if( p_peek[3] > 0xb9 )
    {
        if( !b_forced ) return VLC_EGENERIC;
        msg_Err( p_demux, "this seems to be a system stream (PS plug-in ?), but continuing" );
    }

    p_demux->pf_demux  = Demux;
    p_demux->pf_control= Control;
    p_demux->p_sys     = p_sys = malloc( sizeof( demux_sys_t ) );
    p_sys->b_start     = true;
    p_sys->p_es        = NULL;
    p_sys->i_pcr       = VLC_TICK_INVALID;
    p_sys->i_time_offset = 0;
    p_sys->i_bytes     = 0;
    p_sys->i_bitrate_avg = 0;
    p_sys->b_seeked    = false;
    p_sys->i_start_time = VLC_TICK_INVALID;

    /* Load the mpegvideo packetizer */
    es_format_Init( &fmt, VIDEO_ES, VLC_CODEC_MPGV );
    p_sys->p_packetizer = demux_PacketizerNew( p_demux, &fmt, "mpeg video" );
    if( !p_sys->p_packetizer )
    {
        free( p_sys );
        return VLC_EGENERIC;
    }

    /* create the output */
    p_sys->p_es = es_out_Add( p_demux->out, &fmt );
    if( p_sys->p_es == NULL )
    {
        Close( p_this );
        return VLC_EGENERIC;
    }

    return VLC_SUCCESS;
}

/* Where in the bytes the held seek wants to be, measured with the bitrate the
 * probe has just established. */
static void StartSeek( demux_t *p_demux )
{
    demux_sys_t *p_sys = p_demux->p_sys;
    vlc_tick_t i_time = p_sys->i_start_time;
    uint64_t i_pos = p_sys->i_bitrate_avg * i_time / INT64_C(8000000);
    uint64_t i_size = stream_Size( p_demux->s );

    p_sys->i_start_time = VLC_TICK_INVALID;
    if( i_size > 0 && i_pos >= i_size )
        i_pos = i_size - 1;

    if( p_sys->i_bitrate_avg <= 0 || vlc_stream_Seek( p_demux->s, i_pos ) )
        i_time = 0;
    else
        i_time = INT64_C(8000000) * vlc_stream_Tell( p_demux->s )
                 / p_sys->i_bitrate_avg;

    msg_Dbg( p_demux, "starting at %" PRId64 " us, byte %" PRIu64
             " of an estimated %" PRId64 " bit/s",
             i_time, i_pos, p_sys->i_bitrate_avg );

    PostSeekReset( p_demux, i_time );
}

/*****************************************************************************
 * Demux: reads and demuxes data packets
 *****************************************************************************
 * Returns -1 in case of error, 0 in case of EOF, 1 otherwise
 *****************************************************************************/
static int Demux( demux_t *p_demux )
{
    demux_sys_t  *p_sys = p_demux->p_sys;
    block_t *p_block_in, *p_block_out;
    bool b_eof = false;
    /* Nothing goes out while a held seek is being measured for: the stream is
     * about to be somewhere else, and what is read on the way there was never
     * asked for. */
    const bool b_probing = p_sys->i_start_time != VLC_TICK_INVALID;

    if( ( p_block_in = vlc_stream_Block( p_demux->s, MPGV_PACKET_SIZE ) ) == NULL )
    {
        b_eof = true;
    }

    if( p_block_in )
    {
        p_block_in->i_pts =
        p_block_in->i_dts = ( p_sys->b_start ) ? VLC_TICK_0 : VLC_TICK_INVALID;
    }

    while( (p_block_out = p_sys->p_packetizer->pf_packetize( p_sys->p_packetizer,
                                                             p_block_in ? &p_block_in : NULL )) )
    {
        p_sys->b_start = false;

        while( p_block_out )
        {
            block_t *p_next = p_block_out->p_next;

            if( !b_probing )
                es_out_SetPCR( p_demux->out, p_block_out->i_dts );
            p_sys->i_pcr = p_block_out->i_dts;
            p_sys->i_bytes += p_block_out->i_buffer;

            /* What has gone out over how long it lasts is the bitrate, which
             * is the only thing a stream with no timestamps of its own has to
             * answer for its length with, or to date a seek from. */
            if( !p_sys->b_seeked && p_sys->i_pcr > VLC_TICK_0 )
                p_sys->i_bitrate_avg = INT64_C(8000000) * p_sys->i_bytes /
                                       ( p_sys->i_pcr - VLC_TICK_0 );

            p_block_out->p_next = NULL;
            if( b_probing )
                block_Release( p_block_out );
            else
                es_out_Send( p_demux->out, p_sys->p_es, p_block_out );

            p_block_out = p_next;
        }
    }

    if( b_probing &&
        ( b_eof || p_sys->i_pcr - VLC_TICK_0 >= MPGV_PROBE_LENGTH ) )
        StartSeek( p_demux );

    return (b_eof) ? VLC_DEMUXER_EOF : VLC_DEMUXER_SUCCESS;
}

/*****************************************************************************
 * Control:
 *****************************************************************************/
/*****************************************************************************
 * PostSeekReset: put the parser and the timeline where the stream now is
 *****************************************************************************
 * A seek leaves the packetizer holding bytes that no longer join onto what
 * comes next, and a date that no longer says anything about where the stream
 * has landed. Empty it and give it the start of a stream again, so that the
 * first picture after the seek is dated from the position sought to.
 *****************************************************************************/
static void PostSeekReset( demux_t *p_demux, vlc_tick_t i_time )
{
    demux_sys_t *p_sys = p_demux->p_sys;

    if( p_sys->p_packetizer->pf_flush )
        p_sys->p_packetizer->pf_flush( p_sys->p_packetizer );
    else
    {
        block_t *p_block_out;
        while( ( p_block_out = p_sys->p_packetizer->pf_packetize(
                                   p_sys->p_packetizer, NULL ) ) )
            block_ChainRelease( p_block_out );
    }

    p_sys->b_start = true;
    p_sys->i_pcr = VLC_TICK_INVALID;
    p_sys->i_time_offset = i_time;
    p_sys->b_seeked = true;
}

static int Control( demux_t *p_demux, int i_query, va_list args )
{
    demux_sys_t *p_sys = p_demux->p_sys;

    switch( i_query )
    {
        case DEMUX_GET_TIME:
            /* The helper would answer from the byte position, which is not
             * where the output has got to. Answer with the pcr just published
             * and where the last seek pinned it. */
            if( p_sys->i_pcr == VLC_TICK_INVALID )
                return VLC_EGENERIC;
            *va_arg( args, int64_t * ) = p_sys->i_time_offset + p_sys->i_pcr;
            return VLC_SUCCESS;

        case DEMUX_SET_TIME:
        case DEMUX_SET_POSITION:
        {
            /* Both are byte seeks through the helper, and only the bitrate
             * says where in the bytes a time is. Without one a time seek
             * cannot be answered at all; a position seek still can. */
            if( i_query == DEMUX_SET_TIME && p_sys->i_bitrate_avg <= 0 )
            {
                /* --start-time is issued before the first Demux(), when there
                 * is nothing to measure a bitrate over yet. Hold it rather
                 * than refuse it, and let Demux() answer it. */
                if( p_sys->i_bytes > 0 )
                    return VLC_EGENERIC;
                p_sys->i_start_time = va_arg( args, int64_t );
                return VLC_SUCCESS;
            }

            int i_ret = demux_vaControlHelper( p_demux->s, 0, -1,
                                               p_sys->i_bitrate_avg, 1,
                                               i_query, args );
            if( i_ret == VLC_SUCCESS )
            {
                vlc_tick_t i_time = 0;
                if( p_sys->i_bitrate_avg > 0 )
                    i_time = INT64_C(8000000) * vlc_stream_Tell( p_demux->s ) /
                             p_sys->i_bitrate_avg;
                PostSeekReset( p_demux, i_time );
            }
            return i_ret;
        }

        default:
            return demux_vaControlHelper( p_demux->s, 0, -1,
                                          p_sys->i_bitrate_avg, 1,
                                          i_query, args );
    }
}

