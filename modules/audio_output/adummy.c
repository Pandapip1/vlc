/*****************************************************************************
 * adummy.c : dummy audio output plugin
 *****************************************************************************
 * Copyright (C) 2002 VLC authors and VideoLAN
 * $Id$
 *
 * Authors: Christophe Massiot <massiot@via.ecp.fr>
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

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_aout.h>
#include <vlc_cpu.h>

static int Open( vlc_object_t * p_this );
static void Close( vlc_object_t * p_this );

#define LATENCY_TEXT N_("Device latency (ms)")
#define LATENCY_LONGTEXT N_( \
    "Report this much time between a sample reaching the device and it " \
    "being heard, as an output on a slow path does." )

#define JITTER_TEXT N_("Latency jitter (ms)")
#define JITTER_LONGTEXT N_( \
    "Vary the reported delay by up to this much either way, as a device " \
    "whose position can only be read coarsely does." )

#define SEED_TEXT N_("Jitter seed")
#define SEED_LONGTEXT N_( \
    "Seed for the jitter sequence. The same seed draws the same numbers in " \
    "the same order, which takes one source of variation out of a " \
    "comparison. It does not make a run repeat: which reading gets which of " \
    "those numbers still depends on when the decoder asks for it." )

#define DRIFT_TEXT N_("Clock error (ppm)")
#define DRIFT_LONGTEXT N_( \
    "Run the device clock this many parts per million away from the system " \
    "clock, positive for fast. No real device is exactly nominal, and this " \
    "is what the output's drift correction exists to answer." )

#define CHANNELS_TEXT N_("Channels")
#define CHANNELS_LONGTEXT N_( \
    "Accept only this many channels, so that what is played has to be " \
    "remixed to reach the device. Zero takes whatever it is given." )

vlc_module_begin ()
    set_shortname( N_("Dummy") )
    set_description( N_("Dummy audio output") )
    set_capability( "audio output", 0 )
    set_callbacks( Open, Close )
    add_shortcut( "dummy" )

    /* Left alone this output reports no timing and takes any layout, which is
     * what it has always done. Setting any of the first four makes it answer
     * as a device would, so that the clock, its drift correction and what
     * they do at a seam can be exercised without one. */
    add_integer( "adummy-latency", 0, LATENCY_TEXT, LATENCY_LONGTEXT, true )
        change_integer_range( 0, 10000 )
    add_integer( "adummy-jitter", 0, JITTER_TEXT, JITTER_LONGTEXT, true )
        change_integer_range( 0, 10000 )
    add_integer( "adummy-seed", 1, SEED_TEXT, SEED_LONGTEXT, true )
    add_integer( "adummy-drift", 0, DRIFT_TEXT, DRIFT_LONGTEXT, true )
        change_integer_range( -100000, 100000 )
    add_integer( "adummy-channels", 0, CHANNELS_TEXT, CHANNELS_LONGTEXT, true )
        change_integer_range( 0, 8 )
vlc_module_end ()

#define A52_FRAME_NB 1536

struct aout_sys_t
{
    vlc_tick_t i_latency; /* what the device claims to hold */
    vlc_tick_t i_jitter;  /* how coarsely its position can be read */
    int64_t    i_drift;   /* ppm its clock is away from the system's */
    uint64_t   i_rng;     /* jitter sequence, from the seed */
    uint16_t   i_chans;   /* layout it insists on, 0 for any */

    vlc_tick_t i_start;   /* when the stream began draining */
    uint64_t   i_written; /* samples handed over since the last flush */
    uint64_t   i_total;   /* samples handed over since the stream started */
    unsigned   i_rate;

    /* What a test wants to know afterwards. Read them from the log line at
     * stop, or from the variables of the same name on the aout object. */
    bool       b_dry;      /* is it out of data right now */
    uint64_t   i_runouts;  /* times it has run out since the stream started */
    vlc_tick_t i_shortfall; /* the worst of those, as a duration */
};

/* xorshift64*: the numbers depend on nothing but the seed. */
static uint64_t NextRandom( struct aout_sys_t *sys )
{
    uint64_t x = sys->i_rng;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    sys->i_rng = x;
    return x * UINT64_C(2685821657736338717);
}

/* Uniform over [-i_jitter, +i_jitter]. */
static vlc_tick_t NextJitter( struct aout_sys_t *sys )
{
    if( sys->i_jitter == 0 )
        return 0;

    const uint64_t i_span = (uint64_t)sys->i_jitter * 2 + 1;
    return (vlc_tick_t)( NextRandom( sys ) % i_span ) - sys->i_jitter;
}

static void Report( audio_output_t *aout )
{
    struct aout_sys_t *sys = aout->sys;

    var_SetInteger( aout, "adummy-runouts", sys->i_runouts );
    var_SetInteger( aout, "adummy-shortfall", sys->i_shortfall );
    var_SetInteger( aout, "adummy-written", sys->i_total );
}

static void Play(audio_output_t *aout, block_t *block)
{
    struct aout_sys_t *sys = aout->sys;

    if( sys != NULL )
    {
        if( sys->i_start == VLC_TICK_INVALID )
            sys->i_start = mdate();
        sys->i_written += block->i_nb_samples;
        sys->i_total += block->i_nb_samples;
    }

    block_Release( block );
}

static void Flush(audio_output_t *aout, bool wait)
{
    struct aout_sys_t *sys = aout->sys;

    if( sys != NULL )
    {
        sys->i_start = VLC_TICK_INVALID;
        sys->i_written = 0;
        sys->b_dry = false;
    }

    (void) wait;
}

/* What is still to be heard: everything handed over, less what the device has
 * got through, plus what it holds after that. The device gets through it on
 * its own clock, which is the system's only when asked for no error. */
static int TimeGet(audio_output_t *aout, vlc_tick_t *restrict delay)
{
    struct aout_sys_t *sys = aout->sys;

    if( sys->i_start == VLC_TICK_INVALID || sys->i_rate == 0 )
        return -1; /* nothing has been handed over yet */

    const vlc_tick_t i_handed =
        (vlc_tick_t)( sys->i_written * CLOCK_FREQ / sys->i_rate );

    vlc_tick_t i_drained = mdate() - sys->i_start;
    i_drained += i_drained * sys->i_drift / 1000000;

    vlc_tick_t i_queued = i_handed - i_drained;

    if( i_queued < 0 )
    {
        /* It wanted a sample that was not there. Count the run of dryness
         * once, not once per reading, and keep the worst of them. */
        if( !sys->b_dry )
        {
            sys->b_dry = true;
            sys->i_runouts++;
            msg_Dbg( aout, "ran out of data by %"PRId64" us", -i_queued );
        }
        if( -i_queued > sys->i_shortfall )
            sys->i_shortfall = -i_queued;

        i_queued = 0;
        Report( aout );
    }
    else
        sys->b_dry = false;

    *delay = i_queued + sys->i_latency + NextJitter( sys );
    return 0;
}

static int LatencyGet(audio_output_t *aout, vlc_tick_t *restrict latency)
{
    struct aout_sys_t *sys = aout->sys;

    *latency = sys->i_latency;
    return 0;
}

static int Start(audio_output_t *aout, audio_sample_format_t *restrict fmt)
{
    struct aout_sys_t *sys = aout->sys;

    switch (fmt->i_format)
    {
        case VLC_CODEC_A52:
        case VLC_CODEC_EAC3:
            fmt->i_format = VLC_CODEC_SPDIFL;
            fmt->i_bytes_per_frame = 4;
            fmt->i_frame_length = 1;
            break;
        case VLC_CODEC_DTS:
        case VLC_CODEC_TRUEHD:
        case VLC_CODEC_MLP:
            fmt->i_format = VLC_CODEC_SPDIFL;
            fmt->i_rate = 768000;
            fmt->i_bytes_per_frame = 16;
            fmt->i_frame_length = 1;
            break;
        default:
            assert(AOUT_FMT_LINEAR(fmt));
            assert(aout_FormatNbChannels(fmt) > 0);
            fmt->i_format = HAVE_FPU ? VLC_CODEC_FL32 : VLC_CODEC_S16N;
            fmt->channel_type = AUDIO_CHANNEL_TYPE_BITMAP;

            if( sys != NULL && sys->i_chans != 0 )
            {
                fmt->i_physical_channels = sys->i_chans;
                aout_FormatPrepare( fmt );
            }
            break;
    }

    if( sys != NULL )
    {
        sys->i_start = VLC_TICK_INVALID;
        sys->i_written = 0;
        sys->i_rate = fmt->i_rate;
        sys->b_dry = false;
        sys->i_total = 0;
        sys->i_runouts = 0;
        sys->i_shortfall = 0;
        Report( aout );
    }

    return VLC_SUCCESS;
}

static void Stop(audio_output_t *aout)
{
    struct aout_sys_t *sys = aout->sys;

    Report( aout );
    msg_Dbg( aout, "played %"PRIu64" samples, ran out %"PRIu64" time(s), "
             "worst shortfall %"PRId64" us",
             sys->i_total, sys->i_runouts, sys->i_shortfall );

    Flush( aout, false );
}

/* One channel more than asked for would be remixed the same way, so the
 * layouts here are only the usual ones, by count. */
static uint16_t ChansForCount( unsigned i_count )
{
    switch( i_count )
    {
        case 1:  return AOUT_CHAN_CENTER;
        case 2:  return AOUT_CHANS_STEREO;
        case 3:  return AOUT_CHANS_3_0;
        case 4:  return AOUT_CHANS_4_0;
        case 5:  return AOUT_CHANS_5_0;
        case 6:  return AOUT_CHANS_5_1;
        case 7:  return AOUT_CHANS_7_0;
        case 8:  return AOUT_CHANS_7_1;
        default: return 0;
    }
}

static int Open(vlc_object_t *obj)
{
    audio_output_t *aout = (audio_output_t *)obj;

    const vlc_tick_t i_latency =
        VLC_TICK_FROM_MS( var_InheritInteger( obj, "adummy-latency" ) );
    const vlc_tick_t i_jitter =
        VLC_TICK_FROM_MS( var_InheritInteger( obj, "adummy-jitter" ) );
    const int64_t i_drift = var_InheritInteger( obj, "adummy-drift" );
    const uint16_t i_chans =
        ChansForCount( var_InheritInteger( obj, "adummy-channels" ) );

    aout->sys = NULL;
    aout->time_get = NULL;
    aout->latency_get = NULL;
    aout->stop = NULL;

    /* Asked for none of it, it answers nothing, as it always has. */
    if( i_latency > 0 || i_jitter > 0 || i_drift != 0 || i_chans != 0 )
    {
        struct aout_sys_t *sys = malloc( sizeof (*sys) );
        if( unlikely(sys == NULL) )
            return VLC_ENOMEM;

        sys->i_latency = i_latency;
        sys->i_jitter = i_jitter;
        sys->i_drift = i_drift;
        sys->i_chans = i_chans;
        sys->i_rng = (uint64_t)var_InheritInteger( obj, "adummy-seed" );
        if( sys->i_rng == 0 )
            sys->i_rng = 1; /* xorshift stays at zero for ever otherwise */
        sys->i_start = VLC_TICK_INVALID;
        sys->i_written = 0;
        sys->i_rate = 0;
        sys->b_dry = false;
        sys->i_total = 0;
        sys->i_runouts = 0;
        sys->i_shortfall = 0;

        aout->sys = sys;
        aout->stop = Stop;

        /* Without any of the timing set there is nothing to report that the
         * caller does not already know, and a device that answers is not the
         * same case as one that does not. */
        if( i_latency > 0 || i_jitter > 0 || i_drift != 0 )
        {
            aout->time_get = TimeGet;
            aout->latency_get = LatencyGet;
        }

        var_Create( aout, "adummy-runouts", VLC_VAR_INTEGER );
        var_Create( aout, "adummy-shortfall", VLC_VAR_INTEGER );
        var_Create( aout, "adummy-written", VLC_VAR_INTEGER );
    }

    aout->start = Start;
    aout->play = Play;
    aout->pause = NULL;
    aout->flush = Flush;
    aout->volume_set = NULL;
    aout->mute_set = NULL;
    return VLC_SUCCESS;
}

static void Close(vlc_object_t *obj)
{
    audio_output_t *aout = (audio_output_t *)obj;

    free( aout->sys );
}
