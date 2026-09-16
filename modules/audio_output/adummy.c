/*****************************************************************************
 * adummy.c : dummy audio output plugin
 *****************************************************************************
 * Copyright (C) 2002 VLC authors and VideoLAN
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

static int Open(vlc_object_t *);
static void Close(vlc_object_t *);

#define LATENCY_TEXT N_("Device latency (ms)")
#define LATENCY_LONGTEXT N_( \
    "Report this much time between a sample reaching the device and it " \
    "being heard, as an output on a slow path does." )

#define JITTER_TEXT N_("Latency jitter (ms)")
#define JITTER_LONGTEXT N_( \
    "Vary the reported position by up to this much either way, as a device " \
    "whose position can only be read coarsely does." )

#define SEED_TEXT N_("Jitter seed")
#define SEED_LONGTEXT N_( \
    "Seed for the jitter sequence. The same seed draws the same numbers in " \
    "the same order, which takes one source of variation out of a " \
    "comparison. It does not make a run repeat: which report gets which of " \
    "those numbers still depends on when the decoder hands a block over." )

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

    /* Left alone this output is an ideal device: no latency, a clock exactly
     * the system's, and any layout. Setting any of the first three makes it
     * answer as a real one does, so that the clock, its drift correction and
     * what they do at a seam can be exercised without hardware. */
    add_integer( "adummy-latency", 0, LATENCY_TEXT, LATENCY_LONGTEXT )
        change_integer_range( 0, 10000 )
    add_integer( "adummy-jitter", 0, JITTER_TEXT, JITTER_LONGTEXT )
        change_integer_range( 0, 10000 )
    add_integer( "adummy-seed", 1, SEED_TEXT, SEED_LONGTEXT )
    add_integer( "adummy-drift", 0, DRIFT_TEXT, DRIFT_LONGTEXT )
        change_integer_range( -100000, 100000 )
    add_integer( "adummy-channels", 0, CHANNELS_TEXT, CHANNELS_LONGTEXT )
        change_integer_range( 0, 8 )
vlc_module_end ()

struct aout_sys
{
    vlc_tick_t first_pts;
    vlc_tick_t first_play_date;
    vlc_tick_t last_timing_date;
    vlc_tick_t paused_date;

    /* what the device is being asked to pretend to be */
    vlc_tick_t latency;  /* between a sample reaching it and being heard */
    vlc_tick_t jitter;   /* how coarsely its position can be read */
    int64_t    drift;    /* ppm its clock is away from the system's */
    uint16_t   chans;    /* layout it insists on, 0 for any */
    bool       simulated;/* any of the three timing knobs is set */
    uint64_t   rng;      /* jitter sequence, from the seed */

    unsigned   rate;
    uint64_t   written;  /* samples handed over since the last flush */
    uint64_t   total;    /* samples handed over since the stream started */

    /* What a test wants to know afterwards. Read them from the log line at
     * stop, or from the variables of the same name on the aout object. */
    bool       dry;         /* is it out of data right now */
    uint64_t   runouts;     /* times it has run out since the stream started */
    vlc_tick_t shortfall;   /* the worst of those, as a duration */
};

/* xorshift64*: the numbers depend on nothing but the seed. */
static uint64_t NextRandom(struct aout_sys *sys)
{
    uint64_t x = sys->rng;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    sys->rng = x;
    return x * UINT64_C(2685821657736338717);
}

/* Uniform over [-jitter, +jitter]. */
static vlc_tick_t NextJitter(struct aout_sys *sys)
{
    if (sys->jitter == 0)
        return 0;

    const uint64_t span = (uint64_t)sys->jitter * 2 + 1;
    return (vlc_tick_t)(NextRandom(sys) % span) - sys->jitter;
}

static void Report(audio_output_t *aout)
{
    struct aout_sys *sys = aout->sys;

    var_SetInteger(aout, "adummy-runouts", sys->runouts);
    var_SetInteger(aout, "adummy-shortfall", sys->shortfall);
    var_SetInteger(aout, "adummy-written", sys->total);
}

/* The position the device is at: what it has got through since it started
 * playing, on its own clock, less what it still holds. Reported as the pts
 * being heard now, which is what the core asks of a timing report. */
static void ReportSimulated(audio_output_t *aout, vlc_tick_t now)
{
    struct aout_sys *sys = aout->sys;

    vlc_tick_t audible_for = now - sys->first_play_date - sys->latency;
    if (audible_for < 0)
        return; /* the first sample has not come out of the device yet */

    /* The device gets through it on its own clock, which is the system's
     * only when asked for no error. */
    vlc_tick_t played = audible_for + audible_for * sys->drift / 1000000;
    vlc_tick_t audio_ts = sys->first_pts + played;

    /* It cannot be further on than what it has been handed. */
    vlc_tick_t end_ts = sys->first_pts
                      + vlc_tick_from_samples(sys->written, sys->rate);
    if (audio_ts > end_ts)
    {
        /* Count the run of dryness once, not once per report, and keep the
         * worst of them. */
        if (!sys->dry)
        {
            sys->dry = true;
            sys->runouts++;
            msg_Dbg(aout, "ran out of data by %"PRId64" us", audio_ts - end_ts);
        }
        if (audio_ts - end_ts > sys->shortfall)
            sys->shortfall = audio_ts - end_ts;

        audio_ts = end_ts;
        Report(aout);
    }
    else
        sys->dry = false;

    aout_TimingReport(aout, now, audio_ts + NextJitter(sys));
}

static void Play(audio_output_t *aout, block_t *block, vlc_tick_t date)
{
    struct aout_sys *sys = aout->sys;

    if (unlikely(sys->first_play_date == VLC_TICK_INVALID))
    {
        assert(sys->first_pts == VLC_TICK_INVALID);
        sys->first_play_date = date;
        sys->first_pts = block->i_pts;
    }

    sys->written += block->i_nb_samples;
    sys->total += block->i_nb_samples;

    block_Release( block );

    vlc_tick_t now = vlc_tick_now();

    if (sys->simulated)
    {
        /* A real device reports every time it is written to, and the drift
         * correction needs that cadence to have anything to work on. */
        sys->last_timing_date = now;
        ReportSimulated(aout, now);
        return;
    }

    if (now < sys->first_play_date)
        return;

    if (sys->last_timing_date == VLC_TICK_INVALID ||
        now - sys->last_timing_date >= VLC_TICK_FROM_SEC(1))
    {
        sys->last_timing_date = now;
        aout_TimingReport(aout, now,
                          now - sys->first_play_date + sys->first_pts);
    }
}

static void Pause(audio_output_t *aout, bool paused, vlc_tick_t date)
{
    struct aout_sys *sys = aout->sys;
    if (paused)
        sys->paused_date = date;
    else
    {
        sys->first_play_date -= sys->paused_date - date;
        sys->paused_date = VLC_TICK_INVALID;
    }
}

static void Flush(audio_output_t *aout)
{
    struct aout_sys *sys = aout->sys;

    sys->first_play_date = sys->last_timing_date = VLC_TICK_INVALID;
    sys->first_pts = VLC_TICK_INVALID;
    sys->paused_date = VLC_TICK_INVALID;
    sys->written = 0;
    sys->dry = false;
}

static int Start(audio_output_t *aout, audio_sample_format_t *restrict fmt)
{
    struct aout_sys *sys = aout->sys;

    switch (fmt->i_format)
    {
        case VLC_CODEC_A52:
        case VLC_CODEC_EAC3:
            fmt->i_format = VLC_CODEC_SPDIFL;
            fmt->i_bytes_per_frame = 4;
            fmt->i_frame_length = 1;
            break;
        case VLC_CODEC_DTS:
        case VLC_CODEC_DTSHD:
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

            if (sys->chans != 0)
            {
                fmt->i_physical_channels = sys->chans;
                aout_FormatPrepare(fmt);
            }
            break;
    }

    Flush(aout);
    sys->rate = fmt->i_rate;
    sys->total = 0;
    sys->runouts = 0;
    sys->shortfall = 0;
    Report(aout);

    return VLC_SUCCESS;
}

static void Stop(audio_output_t *aout)
{
    struct aout_sys *sys = aout->sys;

    Report(aout);
    msg_Dbg(aout, "played %"PRIu64" samples, ran out %"PRIu64" time(s), "
            "worst shortfall %"PRId64" us",
            sys->total, sys->runouts, sys->shortfall);

    Flush(aout);
}

/* One channel more than asked for would be remixed the same way, so the
 * layouts here are only the usual ones, by count. */
static uint16_t ChansForCount(unsigned count)
{
    switch (count)
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

static void Close(vlc_object_t *obj)
{
    audio_output_t *aout = (audio_output_t *)obj;
    free(aout->sys);
}

static int Open(vlc_object_t *obj)
{
    audio_output_t *aout = (audio_output_t *)obj;

    struct aout_sys *sys = aout->sys = malloc(sizeof(*sys));
    if (!sys)
        return VLC_ENOMEM;

    sys->latency = VLC_TICK_FROM_MS(var_InheritInteger(obj, "adummy-latency"));
    sys->jitter = VLC_TICK_FROM_MS(var_InheritInteger(obj, "adummy-jitter"));
    sys->drift = var_InheritInteger(obj, "adummy-drift");
    sys->chans = ChansForCount(var_InheritInteger(obj, "adummy-channels"));
    sys->simulated = sys->latency > 0 || sys->jitter > 0 || sys->drift != 0;
    sys->rng = (uint64_t)var_InheritInteger(obj, "adummy-seed");
    if (sys->rng == 0)
        sys->rng = 1; /* xorshift stays at zero for ever otherwise */

    sys->rate = 0;
    sys->total = 0;
    sys->runouts = 0;
    sys->shortfall = 0;
    Flush(aout);

    var_Create(aout, "adummy-runouts", VLC_VAR_INTEGER);
    var_Create(aout, "adummy-shortfall", VLC_VAR_INTEGER);
    var_Create(aout, "adummy-written", VLC_VAR_INTEGER);

    aout->start = Start;
    aout->play = Play;
    aout->pause = Pause;
    aout->flush = Flush;
    aout->stop = Stop;
    aout->volume_set = NULL;
    aout->mute_set = NULL;
    return VLC_SUCCESS;
}
