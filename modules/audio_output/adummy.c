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

#include <errno.h>
#include <stdio.h>

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_aout.h>
#include <vlc_atomic.h>
#include <vlc_cpu.h>
#include <vlc_fs.h>

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
    "Seed for the jitter. The same seed and the same audio draw the same " \
    "numbers: the reading taken once a given amount has been handed over is " \
    "always the same one." )

#define TRACE_TEXT N_("Write the device's side down to this file")
#define TRACE_LONGTEXT N_( \
    "One CSV row per block handed over and per reading taken: what came in, " \
    "and what was answered. Against a virtual device the file is a function " \
    "of the stream and the options, so two runs of the same thing produce " \
    "the same bytes and a diff is a test. Unset, nothing is written." )

#define VIRTUAL_TEXT N_("Advance on the data, not the clock")
#define VIRTUAL_LONGTEXT N_( \
    "Move the device position only as audio is handed over, rather than as " \
    "the system clock runs. What is reported is then a function of what was " \
    "played and of these options alone, so two runs of the same stream " \
    "answer identically however loaded the machine is. Nothing can starve " \
    "such a device, so it never reports running out." )

#define DRIFT_TEXT N_("Clock error (ppm)")
#define DRIFT_LONGTEXT N_( \
    "Run the device clock this many parts per million away from the system " \
    "clock, positive for fast. No real device is exactly nominal, and this " \
    "is what the output's drift correction exists to answer. Settable while " \
    "the stream runs, so that an error can be put there and then taken away " \
    "again; what has already been heard is not rewritten." )

#define SETTLE_TEXT N_("Take the clock error away after (ms)")
#define SETTLE_LONGTEXT N_( \
    "Once this much audio has gone through, put the clock error back to " \
    "nothing, as a device warming up to its final rate does. The drift " \
    "correction working its way out to its bound is only half of what there " \
    "is to see; whether it comes back off it is the other half. Measured in " \
    "audio rather than in elapsed time, so that it lands in the same place " \
    "every run. Zero leaves the error where it was set." )

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
    add_integer( "adummy-drift-after", 0, SETTLE_TEXT, SETTLE_LONGTEXT, true )
        change_integer_range( 0, 3600000 )
    add_integer( "adummy-channels", 0, CHANNELS_TEXT, CHANNELS_LONGTEXT, true )
        change_integer_range( 0, 8 )
    add_bool( "adummy-virtual", false, VIRTUAL_TEXT, VIRTUAL_LONGTEXT, true )
    add_string( "adummy-trace", NULL, TRACE_TEXT, TRACE_LONGTEXT, true )
vlc_module_end ()

#define A52_FRAME_NB 1536

struct aout_sys_t
{
    vlc_tick_t i_latency; /* what the device claims to hold */
    vlc_tick_t i_jitter;  /* how coarsely its position can be read */
    atomic_int_least64_t drift; /* ppm its clock is away from the system's */
    int64_t    i_ppm;     /* the last of those the position was moved on at */
    vlc_tick_t i_settle;  /* how much audio it holds that error over, 0 for all */
    uint64_t   i_seed;    /* what the jitter is drawn from */
    uint64_t   i_rng;     /* how far along that a real-time run has got */
    uint16_t   i_chans;   /* layout it insists on, 0 for any */
    bool       b_virtual; /* is the position the data's or the clock's */
    FILE      *trace;     /* where its side of the conversation goes */

    vlc_tick_t i_start;   /* when the position was last moved on */
    vlc_tick_t i_paused;  /* when it was stopped where it stood, if it was */
    vlc_tick_t i_drained; /* how much it had got through by then */
    vlc_tick_t i_origin;  /* pts the first sample handed over was due at */
    vlc_tick_t i_played;  /* how far past that the device has got */
    uint64_t   i_written; /* samples handed over since the last flush */
    uint64_t   i_total;   /* samples handed over since the stream started */
    unsigned   i_rate;

    /* What a test wants to know afterwards. Read them from the log line at
     * stop, or from the variables of the same name on the aout object. */
    bool       b_dry;      /* is it out of data right now */
    uint64_t   i_runouts;  /* times it has run out since the stream started */
    vlc_tick_t i_shortfall; /* the worst of those, as a duration */
};

/* splitmix64. A counter rather than a chain, so that a draw can be asked for
 * by where it belongs in the stream and not only by what came before it. */
static uint64_t Mix( uint64_t x )
{
    x = ( x ^ ( x >> 30 ) ) * UINT64_C(0xBF58476D1CE4E5B9);
    x = ( x ^ ( x >> 27 ) ) * UINT64_C(0x94D049BB133111EB);
    return x ^ ( x >> 31 );
}

/* Uniform over [-i_jitter, +i_jitter], drawn at the sample count when the
 * position is the data's, so the same audio draws the same number however
 * many times the decoder happened to ask, and in sequence otherwise. */
static vlc_tick_t Jitter( struct aout_sys_t *sys )
{
    if( sys->i_jitter == 0 )
        return 0;

    const uint64_t i_at = sys->b_virtual ? sys->i_total : ++sys->i_rng;
    const uint64_t i_span = (uint64_t)sys->i_jitter * 2 + 1;
    const uint64_t x = Mix( sys->i_seed + i_at * UINT64_C(0x9E3779B97F4A7C15) );

    return (vlc_tick_t)( x % i_span ) - sys->i_jitter;
}

/**
 * One row. Whatever the row has nothing to say about is left empty rather
 * than written as a zero, so a reading of none reads differently from no
 * reading. Dates are given from the first sample handed over rather than as
 * they stand, because as they stand they are on the system clock. "answer" is
 * when the next sample will be heard: from that same first date against a
 * virtual device and from the system clock against a real one, which is the
 * whole difference between the two.
 */
static void Trace( struct aout_sys_t *sys, const char *event,
                   unsigned i_samples, const vlc_tick_t *pts,
                   const vlc_tick_t *jitter, const vlc_tick_t *answer,
                   int64_t i_extra )
{
    FILE *f = sys->trace;

    fprintf( f, "%s,", event );

    if( i_samples != 0 )
        fprintf( f, "%u,", i_samples );
    else
        fprintf( f, "," );

    fprintf( f, "%"PRIu64",", sys->i_total );

    if( pts != NULL )
        fprintf( f, "%"PRId64",", *pts );
    else
        fprintf( f, "," );

    if( jitter != NULL )
        fprintf( f, "%"PRId64",", *jitter );
    else
        fprintf( f, "," );

    if( answer != NULL )
        fprintf( f, "%"PRId64",", *answer );
    else
        fprintf( f, "," );

    if( i_extra != 0 )
        fprintf( f, "%"PRId64"\n", i_extra );
    else
        fprintf( f, "\n" );
}

#define TRACE( sys, event, ... ) \
    do { \
        if( (sys)->trace != NULL ) \
            Trace( sys, event, __VA_ARGS__ ); \
    } while( 0 )

/**
 * How much the device has got through, at the error in force over each
 * stretch rather than at the one now set: changing it must move the position
 * on from where it stands, not rewrite what has already been heard.
 */
static vlc_tick_t Drained( struct aout_sys_t *sys, vlc_tick_t now )
{
    const int64_t i_ppm = atomic_load( &sys->drift );
    vlc_tick_t d = now - sys->i_start;

    if( unlikely(i_ppm != sys->i_ppm) )
    {
        sys->i_drained += d + d * sys->i_ppm / 1000000;
        sys->i_start = now;
        sys->i_ppm = i_ppm;
        d = 0;
    }

    return sys->i_drained + d + d * i_ppm / 1000000;
}

static int DriftChanged( vlc_object_t *obj, const char *var,
                         vlc_value_t old, vlc_value_t cur, void *data )
{
    struct aout_sys_t *sys = data;

    atomic_store( &sys->drift, cur.i_int );

    (void) obj; (void) var; (void) old;
    return VLC_SUCCESS;
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
        {
            sys->i_start = mdate();
            sys->i_origin = block->i_pts;
        }

        /* The whole of a virtual device's motion is here: it gets through
         * what it is handed, on its own clock and on nothing else. */
        if( sys->b_virtual && sys->i_rate != 0 )
        {
            const vlc_tick_t i_len =
                (vlc_tick_t)block->i_nb_samples * CLOCK_FREQ / sys->i_rate;

            sys->i_played += i_len
                             - i_len * atomic_load( &sys->drift ) / 1000000;
        }

        sys->i_written += block->i_nb_samples;
        sys->i_total += block->i_nb_samples;

        if( unlikely(sys->i_settle != 0) && sys->i_rate != 0
         && (vlc_tick_t)( sys->i_total * CLOCK_FREQ / sys->i_rate )
            >= sys->i_settle )
        {
            TRACE( sys, "settle", 0, NULL, NULL, NULL, sys->i_settle );
            sys->i_settle = 0;
            var_SetInteger( aout, "adummy-drift", 0 );
        }

        const vlc_tick_t i_at = block->i_pts - sys->i_origin;

        TRACE( sys, "play", block->i_nb_samples, &i_at, NULL, NULL, 0 );
    }

    block_Release( block );
}

/**
 * A pause is not a seam: nothing is thrown away, the device stops where it
 * stands and is still holding it when it is let go again. Only the dates it
 * counts from move, by the length of the pause, which is also what the pts of
 * everything still to come moves by.
 */
static void Pause(audio_output_t *aout, bool paused, vlc_tick_t date)
{
    struct aout_sys_t *sys = aout->sys;

    if( sys == NULL )
        return;

    if( paused )
        sys->i_paused = date;
    else if( sys->i_paused != VLC_TICK_INVALID )
    {
        const vlc_tick_t i_len = date - sys->i_paused;

        if( sys->i_start != VLC_TICK_INVALID )
            sys->i_start += i_len;
        if( sys->i_origin != VLC_TICK_INVALID )
            sys->i_origin += i_len;
        sys->i_paused = VLC_TICK_INVALID;
    }

    TRACE( sys, paused ? "pause" : "resume", 0, NULL, NULL, NULL, 0 );
}

static void Flush(audio_output_t *aout, bool wait)
{
    struct aout_sys_t *sys = aout->sys;

    if( sys != NULL )
    {
        /* Only one that throws something away, and not whether it was asked
         * to wait: a device holding nothing does the same thing either way,
         * and a row for it would say what the caller did, not what it did. */
        if( sys->i_written != 0 )
            TRACE( sys, "flush", 0, NULL, NULL, NULL, 0 );

        sys->i_start = VLC_TICK_INVALID;
        sys->i_paused = VLC_TICK_INVALID;
        sys->i_drained = 0;
        sys->i_ppm = atomic_load( &sys->drift );
        sys->i_origin = VLC_TICK_INVALID;
        sys->i_played = 0;
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

    if( sys->b_virtual )
    {
        /* When the next sample will be heard, said as a delay because that is
         * the unit asked for: the caller adds the clock straight back on, and
         * what it is left holding came from the data alone. */
        const vlc_tick_t i_jitter = Jitter( sys );
        const vlc_tick_t i_answer = sys->i_played + sys->i_latency + i_jitter;

        TRACE( sys, "time", 0, NULL, &i_jitter, &i_answer, 0 );

        *delay = sys->i_origin + i_answer - mdate();
        return 0;
    }

    const vlc_tick_t i_handed =
        (vlc_tick_t)( sys->i_written * CLOCK_FREQ / sys->i_rate );

    vlc_tick_t i_queued = i_handed - Drained( sys, mdate() );

    if( i_queued < 0 )
    {
        /* It wanted a sample that was not there. Count the run of dryness
         * once, not once per reading, and keep the worst of them. */
        if( !sys->b_dry )
        {
            sys->b_dry = true;
            sys->i_runouts++;
            msg_Dbg( aout, "ran out of data by %"PRId64" us", -i_queued );
            aout_TraceEvent( aout, "runout", -i_queued );
            TRACE( sys, "runout", 0, NULL, NULL, NULL, -i_queued );
        }
        if( -i_queued > sys->i_shortfall )
            sys->i_shortfall = -i_queued;

        i_queued = 0;
        Report( aout );
    }
    else
        sys->b_dry = false;

    const vlc_tick_t i_jitter = Jitter( sys );

    *delay = i_queued + sys->i_latency + i_jitter;
    TRACE( sys, "time", 0, NULL, &i_jitter, delay, 0 );
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
        sys->i_paused = VLC_TICK_INVALID;
        sys->i_drained = 0;
        sys->i_ppm = atomic_load( &sys->drift );
        sys->i_origin = VLC_TICK_INVALID;
        sys->i_played = 0;
        sys->i_written = 0;
        sys->i_rate = fmt->i_rate;
        sys->b_dry = false;
        sys->i_total = 0;
        sys->i_runouts = 0;
        sys->i_shortfall = 0;
        Report( aout );

        if( sys->trace != NULL )
        {
            fprintf( sys->trace, "# rate=%u channels=%u format=%4.4s\n",
                     fmt->i_rate, aout_FormatNbChannels( fmt ),
                     (const char *)&fmt->i_format );
            Trace( sys, "start", 0, NULL, NULL, NULL, 0 );
        }
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

    TRACE( sys, "stop", 0, NULL, NULL, NULL, sys->i_runouts );

    Flush( aout, false );

    if( sys->trace != NULL )
        fflush( sys->trace );
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
    const vlc_tick_t i_settle =
        VLC_TICK_FROM_MS( var_InheritInteger( obj, "adummy-drift-after" ) );
    const bool b_virtual = var_InheritBool( obj, "adummy-virtual" );
    char *psz_trace = var_InheritString( obj, "adummy-trace" );
    const uint16_t i_chans =
        ChansForCount( var_InheritInteger( obj, "adummy-channels" ) );

    aout->sys = NULL;
    aout->time_get = NULL;
    aout->latency_get = NULL;
    aout->stop = NULL;

    /* Asked for none of it, it answers nothing, as it always has. */
    if( i_latency > 0 || i_jitter > 0 || i_drift != 0 || i_chans != 0
     || b_virtual || i_settle > 0 || psz_trace != NULL )
    {
        struct aout_sys_t *sys = malloc( sizeof (*sys) );
        if( unlikely(sys == NULL) )
        {
            free( psz_trace );
            return VLC_ENOMEM;
        }

        sys->i_latency = i_latency;
        sys->i_jitter = i_jitter;
        atomic_init( &sys->drift, i_drift );
        sys->i_ppm = i_drift;
        sys->i_settle = i_settle;
        sys->i_chans = i_chans;
        sys->b_virtual = b_virtual;
        sys->i_seed = (uint64_t)var_InheritInteger( obj, "adummy-seed" );
        sys->i_rng = 0;
        sys->i_start = VLC_TICK_INVALID;
        sys->i_paused = VLC_TICK_INVALID;
        sys->i_drained = 0;
        sys->i_origin = VLC_TICK_INVALID;
        sys->i_played = 0;
        sys->i_written = 0;
        sys->i_rate = 0;
        sys->b_dry = false;
        sys->i_total = 0;
        sys->i_runouts = 0;
        sys->i_shortfall = 0;
        sys->trace = NULL;

        if( psz_trace != NULL )
        {
            sys->trace = vlc_fopen( psz_trace, "we" );
            if( sys->trace == NULL )
                msg_Err( aout, "cannot write the trace to %s: %s", psz_trace,
                         vlc_strerror_c( errno ) );
            else
            {
                fprintf( sys->trace, "# vlc-adummy-trace 1\n" );
                fprintf( sys->trace, "# virtual=%d latency_us=%"PRId64" "
                         "jitter_us=%"PRId64" drift_ppm=%"PRId64" "
                         "drift_after_us=%"PRId64" seed=%"PRIu64
                         " channels=0x%04x\n", b_virtual ? 1 : 0, i_latency,
                         i_jitter, i_drift, i_settle, sys->i_seed, i_chans );
                fprintf( sys->trace,
                         "event,samples,total,pts_us,jitter_us,answer_us,"
                         "extra_us\n" );
            }
        }

        aout->sys = sys;
        aout->stop = Stop;

        /* Without any of the timing set there is nothing to report that the
         * caller does not already know, and a device that answers is not the
         * same case as one that does not. */
        if( i_latency > 0 || i_jitter > 0 || i_drift != 0 || b_virtual
         || i_settle > 0 )
        {
            aout->time_get = TimeGet;
            aout->latency_get = LatencyGet;
        }

        var_Create( aout, "adummy-runouts", VLC_VAR_INTEGER );
        var_Create( aout, "adummy-shortfall", VLC_VAR_INTEGER );
        var_Create( aout, "adummy-written", VLC_VAR_INTEGER );

        var_Create( aout, "adummy-drift", VLC_VAR_INTEGER | VLC_VAR_DOINHERIT );
        var_AddCallback( aout, "adummy-drift", DriftChanged, sys );
    }
    free( psz_trace );

    aout->start = Start;
    aout->play = Play;
    aout->pause = Pause;
    aout->flush = Flush;
    aout->volume_set = NULL;
    aout->mute_set = NULL;
    return VLC_SUCCESS;
}

static void Close(vlc_object_t *obj)
{
    audio_output_t *aout = (audio_output_t *)obj;
    struct aout_sys_t *sys = aout->sys;

    if( sys == NULL )
        return;

    var_DelCallback( aout, "adummy-drift", DriftChanged, sys );

    if( sys->trace != NULL )
        fclose( sys->trace );

    free( sys );
}
