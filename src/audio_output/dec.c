/*****************************************************************************
 * dec.c : audio output API towards decoders
 *****************************************************************************
 * Copyright (C) 2002-2007 VLC authors and VideoLAN
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

/*****************************************************************************
 * Preamble
 *****************************************************************************/
#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <assert.h>

#include <vlc_common.h>
#include <vlc_aout.h>
#include <vlc_input.h>

#include "aout_internal.h"
#include "timeline_step.h"
#include "libvlc.h"

/**
 * Creates an audio output
 */
int aout_DecNew( audio_output_t *p_aout,
                 const audio_sample_format_t *p_format,
                 const es_format_t *p_source,
                 const audio_replay_gain_t *p_replay_gain,
                 const aout_request_vout_t *p_request_vout )
{
    if( p_format->i_bitspersample > 0 )
    {
        /* Sanitize audio format, input need to have a valid physical channels
         * layout or a valid number of channels. */
        int i_map_channels = aout_FormatNbChannels( p_format );
        if( ( i_map_channels == 0 && p_format->i_channels == 0 )
           || i_map_channels > AOUT_CHAN_MAX || p_format->i_channels > INPUT_CHAN_MAX )
        {
            msg_Err( p_aout, "invalid audio channels count" );
            return -1;
        }
    }

    if( p_format->i_rate > 384000 )
    {
        msg_Err( p_aout, "excessive audio sample frequency (%u)",
                 p_format->i_rate );
        return -1;
    }
    if( p_format->i_rate < 4000 )
    {
        msg_Err( p_aout, "too low audio sample frequency (%u)",
                 p_format->i_rate );
        return -1;
    }

    aout_owner_t *owner = aout_owner(p_aout);

    /* TODO: reduce lock scope depending on decoder's real need */
    aout_OutputLock (p_aout);

    /* Create the audio output stream */
    owner->volume = aout_volume_New (p_aout, p_replay_gain);

    atomic_store (&owner->restart, 0);
    owner->source_codec = p_source->i_codec;
    owner->source_id = p_source->i_id;
    owner->input_format = *p_format;
    owner->mixer_format = owner->input_format;
    owner->request_vout = *p_request_vout;

    var_Change (p_aout, "stereo-mode", VLC_VAR_SETVALUE,
                &(vlc_value_t) { .i_int = owner->initial_stereo_mode }, NULL);

    owner->filters_cfg = AOUT_FILTERS_CFG_INIT;
    if (aout_OutputNew (p_aout, &owner->mixer_format, &owner->filters_cfg))
        goto error;
    aout_volume_SetFormat (owner->volume, owner->mixer_format.i_format);

    /* Create the audio filtering "input" pipeline */
    owner->filters = aout_FiltersNew (p_aout, p_format, &owner->mixer_format,
                                      &owner->request_vout,
                                      &owner->filters_cfg);
    if (owner->filters == NULL)
    {
        aout_OutputDelete (p_aout);
error:
        aout_volume_Delete (owner->volume);
        owner->volume = NULL;
        aout_OutputUnlock (p_aout);
        return -1;
    }


    owner->sync.end = VLC_TICK_INVALID;
    owner->sync.source_end = VLC_TICK_INVALID;
    owner->sync.discontinuity = true;
    owner->sync.skip = 0;
    owner->sync.skip_settles = 0;
    owner->sync.update = VLC_TICK_INVALID;
    owner->sync.drift_kp = var_InheritFloat (p_aout, "aout-drift-gain");
    owner->sync.drift_ki =
        var_InheritFloat (p_aout, "aout-drift-integral-gain");
    owner->sync.drift_slew = var_InheritFloat (p_aout, "aout-drift-slew");
    owner->sync.drift_integral = 0.f;
    owner->sync.drift_detune = 0.f;
    owner->sync.drift_bound = false;
    aout_OutputUnlock (p_aout);

    atomic_init (&owner->buffers_lost, 0);
    atomic_init (&owner->buffers_played, 0);
    atomic_store (&owner->vp.update, true);
    return 0;
}

/**
 * Stops all plugins involved in the audio output.
 */
void aout_DecDelete (audio_output_t *aout)
{
    aout_owner_t *owner = aout_owner (aout);

    aout_OutputLock (aout);
    if (owner->mixer_format.i_format)
    {
        aout_FiltersDelete (aout, owner->filters);
        aout_OutputDelete (aout);
    }
    aout_volume_Delete (owner->volume);
    owner->volume = NULL;
    aout_OutputUnlock (aout);
}

static int aout_CheckReady (audio_output_t *aout)
{
    aout_owner_t *owner = aout_owner (aout);

    int status = AOUT_DEC_SUCCESS;
    int restart = atomic_exchange (&owner->restart, 0);
    if (unlikely(restart))
    {
        if (owner->mixer_format.i_format)
            aout_FiltersDelete (aout, owner->filters);

        if (restart & AOUT_RESTART_OUTPUT)
        {   /* Reinitializes the output */
            msg_Dbg (aout, "restarting output...");
            if (owner->mixer_format.i_format)
                aout_OutputDelete (aout);
            owner->mixer_format = owner->input_format;
            owner->filters_cfg = AOUT_FILTERS_CFG_INIT;
            if (aout_OutputNew (aout, &owner->mixer_format, &owner->filters_cfg))
                owner->mixer_format.i_format = 0;
            aout_volume_SetFormat (owner->volume,
                                   owner->mixer_format.i_format);

            /* Notify the decoder that the aout changed in order to try a new
             * suitable codec (like an HDMI audio format). However, keep the
             * same codec if the aout was restarted because of a stereo-mode
             * change from the user. */
            if (restart == AOUT_RESTART_OUTPUT)
                status = AOUT_DEC_CHANGED;
        }

        msg_Dbg (aout, "restarting filters...");
        owner->sync.end = VLC_TICK_INVALID;
        owner->sync.source_end = VLC_TICK_INVALID;
        /* The new filters start with no correction, but the controller keeps
         * what it had learnt and puts it back on the next update. */
        owner->sync.update = VLC_TICK_INVALID;

        if (owner->mixer_format.i_format)
        {
            owner->filters = aout_FiltersNew (aout, &owner->input_format,
                                              &owner->mixer_format,
                                              &owner->request_vout,
                                              &owner->filters_cfg);
            if (owner->filters == NULL)
            {
                aout_OutputDelete (aout);
                owner->mixer_format.i_format = 0;
            }
        }
        /* TODO: This would be a good time to call clean up any video output
         * left over by an audio visualization:
        input_resource_TerminatVout(MAGIC HERE); */
    }
    return (owner->mixer_format.i_format) ? status : AOUT_DEC_FAILED;
}

/**
 * Marks the audio output for restart, to update any parameter of the output
 * plug-in (e.g. output device or channel mapping).
 */
void aout_RequestRestart (audio_output_t *aout, unsigned mode)
{
    aout_owner_t *owner = aout_owner (aout);
    atomic_fetch_or (&owner->restart, mode);
    msg_Dbg (aout, "restart requested (%u)", mode);
}

/*
 * Buffer management
 */

/**
 * Takes i_length off the front of a buffer, or all of it if it is shorter.
 * \return what is left of the buffer, or NULL if none of it is.
 */
static block_t *aout_DecSkipBuffer (audio_output_t *aout, block_t *block,
                                    vlc_tick_t *restrict skip)
{
    aout_owner_t *owner = aout_owner (aout);
    const audio_sample_format_t *fmt = &owner->mixer_format;
    size_t frames, bytes;

    if (block->i_length <= *skip)
    {
        *skip -= block->i_length;
        block_Release (block);
        return NULL;
    }

    frames = (fmt->i_rate * *skip) / CLOCK_FREQ;
    if (frames == 0 || frames >= block->i_nb_samples)
        return block;

    bytes = frames * fmt->i_bytes_per_frame / fmt->i_frame_length;
    block->p_buffer += bytes;
    block->i_buffer -= bytes;
    block->i_nb_samples -= frames;
    block->i_pts += *skip;
    block->i_dts += *skip;
    block->i_length -= *skip;
    *skip = 0;
    return block;
}

static void aout_DecSilence (audio_output_t *aout, vlc_tick_t length, vlc_tick_t pts)
{
    aout_owner_t *owner = aout_owner (aout);
    const audio_sample_format_t *fmt = &owner->mixer_format;
    size_t frames = (fmt->i_rate * length) / CLOCK_FREQ;

    block_t *block = block_Alloc (frames * fmt->i_bytes_per_frame
                                  / fmt->i_frame_length);
    if (unlikely(block == NULL))
        return; /* uho! */

    msg_Dbg (aout, "inserting %zu zeroes", frames);
    memset (block->p_buffer, 0, block->i_buffer);
    block->i_nb_samples = frames;
    block->i_pts = pts;
    block->i_dts = pts;
    block->i_length = length;
    aout_OutputPlay (aout, block);
}

static void aout_DecSynchronize (audio_output_t *aout, vlc_tick_t dec_pts,
                                 int input_rate)
{
    aout_owner_t *owner = aout_owner (aout);
    vlc_tick_t drift;

    /**
     * Depending on the drift between the actual and intended playback times,
     * the audio core may ignore the drift, trigger upsampling or downsampling,
     * insert silence or even discard samples.
     * Future VLC versions may instead adjust the input rate.
     *
     * The audio output plugin is responsible for estimating its actual
     * playback time, or rather the estimated time when the next sample will
     * be played. (The actual playback time is always the current time, that is
     * to say mdate(). It is not an useful statistic.)
     *
     * Most audio output plugins can estimate the delay until playback of
     * the next sample to be written to the buffer, or equally the time until
     * all samples in the buffer will have been played. Then:
     *    pts = mdate() + delay
     */
    if (aout_OutputTimeGet (aout, &drift) != 0)
        return; /* nothing can be done if timing is unknown */

    const vlc_tick_t delay = drift, now = mdate ();

    drift += now - dec_pts;

    /* Late audio output.
     * This can happen due to insufficient caching, scheduling jitter
     * or bug in the decoder. Ideally, the output would seek backward. But that
     * is not portable, not supported by some hardware and often unsafe/buggy
     * where supported. Play out what is queued and drop as much from what
     * follows instead: flushing would throw away a second or more of audio,
     * heard as a hole. */
    if (drift > (owner->sync.discontinuity ? 0
                  : +3 * input_rate * AOUT_MAX_PTS_DELAY / INPUT_RATE_DEFAULT))
    {
        /* One jump at a time: it shortens what is still to come, not what
         * the output holds, so the drift reads high until that has played
         * out. */
        if (now >= owner->sync.skip_settles)
        {
            if (!owner->sync.discontinuity)
                msg_Warn (aout, "playback way too late (%"PRId64"): "
                          "jumping ahead", drift);
            else
                msg_Dbg (aout, "playback too late (%"PRId64"): "
                         "jumping ahead", drift);
            owner->sync.skip += drift;
            owner->sync.skip_settles = now + delay;
            owner->sync.end = VLC_TICK_INVALID;
        }
        return;
    }

    /* Early audio output.
     * This is rare except at startup when the buffers are still empty. */
    if (drift < (owner->sync.discontinuity ? 0
                : -3 * input_rate * AOUT_MAX_PTS_ADVANCE / INPUT_RATE_DEFAULT))
    {
        if (!owner->sync.discontinuity)
            msg_Warn (aout, "playback way too early (%"PRId64"): "
                      "playing silence", drift);
        aout_DecSilence (aout, -drift, dec_pts);

        owner->sync.discontinuity = true;
        drift = 0;
    }

    /* A PI controller on the drift. No derivative term: the drift is
     * quantised by however the output reports its delay, and differentiating
     * that would amplify the noise the slew below is there to keep out. */
    const float max = aout_FiltersGetMaxDetune (owner->filters);

    if (max <= 0.f)
        return; /* correction by resampling is disabled */

    if (owner->sync.discontinuity || owner->sync.skip > 0
     || now < owner->sync.skip_settles)
    {   /* After a jump the drift still reads the old timeline, and the offset
         * either side of a discontinuity is not drift at all. */
        owner->sync.update = now;
        return;
    }

    const float commanded = owner->sync.drift_kp * (drift / (float)CLOCK_FREQ)
                            + owner->sync.drift_integral;
    const float target = (commanded > +max) ? +max
                       : (commanded < -max) ? -max : commanded;
    const bool bound = commanded != target;

    vlc_tick_t dt = (owner->sync.update != VLC_TICK_INVALID)
                    ? now - owner->sync.update : 0;

    /* A gap in the decoder output is not evidence of drift for its whole
     * length; the drift is a reading, not an average over the gap. */
    if (dt > CLOCK_FREQ)
        dt = CLOCK_FREQ;

    const float seconds = (dt > 0) ? dt / (float)CLOCK_FREQ : 0.f;

    /* The drift reading is noisy - sixteen milliseconds on an A2DP sink - and
     * the proportional term turns that straight into detune. Following it
     * wobbles the pitch at the rate the output is fed rather than holding an
     * offset. Slew towards what the controller asks for instead, over a time
     * constant well above the noise and well below the drift being tracked. */
    if (seconds > 0.f)
        owner->sync.drift_detune += (target - owner->sync.drift_detune)
                                    * seconds / (owner->sync.drift_slew
                                                 + seconds);

    aout_FiltersSetDetune (owner->filters, owner->sync.drift_detune);

    if (seconds > 0.f)
    {
        float i = owner->sync.drift_integral
                  + owner->sync.drift_ki * (drift / (float)CLOCK_FREQ) * seconds;

        /* Integrating against what was asked rather than what the bound let
         * through would have the integral answer for a correction never made,
         * and wind up at the bound. Feed the difference back instead. The slew
         * is deliberate and is not fed back: it is a lag, not a refusal. */
        if (owner->sync.drift_kp > 0.f)
            i += (target - commanded)
                 * (owner->sync.drift_ki / owner->sync.drift_kp) * seconds;

        /* The integral on its own may not ask for more than the bound. */
        owner->sync.drift_integral = (i > +max) ? +max
                                   : (i < -max) ? -max : i;
    }
    owner->sync.update = now;

    if (bound != owner->sync.drift_bound)
    {
        owner->sync.drift_bound = bound;

        /* At the bound the drift is no longer being answered: what is left
         * accumulates until it is large enough to be jumped over, which is
         * heard. Either the device is further off nominal than the bound
         * allows, or what is being corrected is not drift. */
        if (bound)
            msg_Warn (aout, "drift correction at its limit of %.0f cents "
                      "(drift: %"PRId64" us): raise aout-max-resampling to "
                      "correct it, at the cost of audible detuning", max,
                      drift);
        else
            msg_Dbg (aout, "drift correction back within its limit "
                     "(drift: %"PRId64" us)", drift);
    }
}

/*****************************************************************************
 * aout_DecPlay : filter & mix the decoded buffer
 *****************************************************************************/
int aout_DecPlay (audio_output_t *aout, block_t *block, int input_rate)
{
    aout_owner_t *owner = aout_owner (aout);

    assert (input_rate >= INPUT_RATE_DEFAULT / AOUT_MAX_INPUT_RATE);
    assert (input_rate <= INPUT_RATE_DEFAULT * AOUT_MAX_INPUT_RATE);
    assert (block->i_pts >= VLC_TICK_0);

    block->i_length = CLOCK_FREQ * block->i_nb_samples
                                 / owner->input_format.i_rate;

    aout_OutputLock (aout);
    int ret = aout_CheckReady (aout);
    if (unlikely(ret == AOUT_DEC_FAILED))
        goto drop; /* Pipeline is unrecoverably broken :-( */

    const vlc_tick_t now = mdate (), advance = block->i_pts - now;
    if (advance < -AOUT_MAX_PTS_DELAY)
    {   /* Late buffer can be caused by bugs in the decoder, by scheduling
         * latency spikes (excessive load, SIGSTOP, etc.) or if buffering is
         * insufficient. We assume the PTS is wrong and play the buffer anyway:
         * Hopefully video has encountered a similar PTS problem as audio. */
        msg_Warn (aout, "buffer too late (%"PRId64" us): dropped", advance);
        goto drop;
    }
    if (advance > AOUT_MAX_ADVANCE_TIME)
    {   /* Early buffers can only be caused by bugs in the decoder. */
        msg_Err (aout, "buffer too early (%"PRId64" us): dropped", advance);
        goto drop;
    }
    if (block->i_flags & BLOCK_FLAG_DISCONTINUITY)
        owner->sync.discontinuity = true;

    /* Taken on the dates the decoder gives its blocks, before anything is
     * filtered, resampled or skipped: the question is whether the source
     * timeline joins up, not what the output then did with it. The length is
     * the content's own, so it takes the playback rate to say how long the
     * block occupies of the timeline the dates are on. */
    const vlc_tick_t step = aout_TimelineStep (owner->sync.source_end,
                                               block->i_pts,
                                               owner->sync.discontinuity);

    owner->sync.source_end = aout_TimelineEnd (block->i_pts, block->i_length,
                                               input_rate);

    if (unlikely(step != 0))
    {
        msg_Warn (aout, "%4.4s stream %d handed over %s of %"PRId64" us: "
                  "correcting it here rather than detuning to it, but what "
                  "made it is upstream", (const char *)&owner->source_codec,
                  owner->source_id, (step > 0) ? "a hole" : "an overlap",
                  (step > 0) ? step : -step);

        /* Same treatment as a declared one: the offset either side of it is
         * not drift, and it is put right where it is rather than worked off
         * by running the whole stream off pitch. */
        owner->sync.discontinuity = true;
    }

    if (atomic_exchange(&owner->vp.update, false))
    {
        vlc_mutex_lock (&owner->vp.lock);
        aout_FiltersChangeViewpoint (owner->filters, &owner->vp.value);
        vlc_mutex_unlock (&owner->vp.lock);
    }

    block = aout_FiltersPlay (owner->filters, block, input_rate);
    if (block == NULL)
        goto lost;

    /* Software volume */
    aout_volume_Amplify (owner->volume, block);

    /* Drift correction */
    aout_DecSynchronize (aout, block->i_pts, input_rate);

    if (unlikely(owner->sync.skip > 0))
    {
        block = aout_DecSkipBuffer (aout, block, &owner->sync.skip);
        if (block == NULL)
            goto lost;
    }

    /* Output */
    owner->sync.end = block->i_pts + block->i_length + 1;
    owner->sync.discontinuity = false;
    aout_OutputPlay (aout, block);
    atomic_fetch_add(&owner->buffers_played, 1);
out:
    aout_OutputUnlock (aout);
    return ret;
drop:
    owner->sync.discontinuity = true;
    block_Release (block);
lost:
    atomic_fetch_add(&owner->buffers_lost, 1);
    goto out;
}

vlc_tick_t aout_DecGetLatency (audio_output_t *aout)
{
    vlc_tick_t latency;

    aout_OutputLock (aout);
    if (aout_OutputLatencyGet (aout, &latency) != 0)
        latency = 0;
    aout_OutputUnlock (aout);
    return latency;
}

/**
 * How much of what was handed to the output is still to be played.
 *
 * For pacing on the end of playback without draining, which would also stop
 * the output. Whoever is waiting for the end has to demux and decode before
 * any sound reaches the output, so it decides how much of the tail to leave
 * unplayed and work in: what it queues next goes behind what is left rather
 * than in place of it.
 */
vlc_tick_t aout_DecGetRemaining (audio_output_t *aout)
{
    aout_owner_t *owner = aout_owner (aout);
    vlc_tick_t remaining;

    aout_OutputLock (aout);
    if (owner->sync.end == VLC_TICK_INVALID)
        remaining = 0;
    else
    {
        remaining = owner->sync.end - mdate ();
        if (remaining < 0)
            remaining = 0;
    }
    aout_OutputUnlock (aout);
    return remaining;
}

void aout_DecGetResetStats(audio_output_t *aout, unsigned *restrict lost,
                           unsigned *restrict played)
{
    aout_owner_t *owner = aout_owner (aout);

    *lost = atomic_exchange(&owner->buffers_lost, 0);
    *played = atomic_exchange(&owner->buffers_played, 0);
}

void aout_DecChangePause (audio_output_t *aout, bool paused, vlc_tick_t date)
{
    aout_owner_t *owner = aout_owner (aout);

    aout_OutputLock (aout);
    /* A pause moves the dates of everything still to come by its length, so
     * what has already been played has to be moved with them or the first
     * block back would read as a hole the length of the pause. */
    if (owner->sync.end != VLC_TICK_INVALID)
        owner->sync.end += paused ? -date : date;
    if (owner->sync.source_end != VLC_TICK_INVALID)
        owner->sync.source_end += paused ? -date : date;
    if (owner->mixer_format.i_format)
    {
        aout_OutputPause (aout, paused, date);
    }
    /* Nothing was played while paused: the correction stays, only the interval
     * the integral is about to be fed must not span the pause. */
    owner->sync.update = VLC_TICK_INVALID;
    aout_OutputUnlock (aout);
}

void aout_DecFlush (audio_output_t *aout, bool wait)
{
    aout_owner_t *owner = aout_owner (aout);

    aout_OutputLock (aout);
    owner->sync.end = VLC_TICK_INVALID;
    owner->sync.source_end = VLC_TICK_INVALID;
    if (owner->mixer_format.i_format)
    {
        if (wait)
        {
            block_t *block = aout_FiltersDrain (owner->filters);
            if (block)
                aout_OutputPlay (aout, block);
        }
        else
            aout_FiltersFlush (owner->filters);
        aout_OutputFlush (aout, wait);
    }

    /* The offset a flush leaves behind is not drift; do not resample to
     * catch it up. The correction accumulated so far describes the device and
     * is still right, so it is kept. */
    owner->sync.discontinuity = true;
    owner->sync.update = VLC_TICK_INVALID;
    aout_OutputUnlock (aout);
}

void aout_ChangeViewpoint(audio_output_t *aout,
                          const vlc_viewpoint_t *p_viewpoint)
{
    aout_owner_t *owner = aout_owner (aout);

    vlc_mutex_lock (&owner->vp.lock);
    owner->vp.value = *p_viewpoint;
    atomic_store(&owner->vp.update, true);
    vlc_mutex_unlock (&owner->vp.lock);
}
