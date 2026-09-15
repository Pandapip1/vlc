/*****************************************************************************
 * pulse.c : Pulseaudio output plugin for vlc
 *****************************************************************************
 * Copyright (C) 2008 VLC authors and VideoLAN
 * Copyright (C) 2009-2011 Rémi Denis-Courmont
 *
 * Authors: Martin Hamrle <hamrle @ post . cz>
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

#include <assert.h>
#include <math.h>
#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_aout.h>
#include <vlc_cpu.h>

#include <pulse/pulseaudio.h>
#include "audio_output/vlcpulse.h"

static int  Open        ( vlc_object_t * );
static void Close       ( vlc_object_t * );

vlc_module_begin ()
    set_shortname( "PulseAudio" )
    set_description( N_("Pulseaudio audio output") )
    set_capability( "audio output", 160 )
    set_category( CAT_AUDIO )
    set_subcategory( SUBCAT_AUDIO_AOUT )
    add_shortcut( "pulseaudio", "pa" )
    set_callbacks( Open, Close )
vlc_module_end ()

/* NOTE:
 * Be careful what you do when the PulseAudio mainloop is held, which is to say
 * within PulseAudio callbacks, or after pa_threaded_mainloop_lock().
 * In particular, a VLC variable callback cannot be triggered nor deleted with
 * the PulseAudio mainloop lock held, if the callback acquires the lock. */

struct sink
{
    struct sink *next;
    uint32_t index;
    char name[1];
};

struct aout_sys_t
{
    pa_stream *stream; /**< PulseAudio playback stream object */
    pa_context *context; /**< PulseAudio connection context */
    pa_threaded_mainloop *mainloop; /**< PulseAudio thread */
    pa_time_event *trigger; /**< Deferred stream trigger */
    pa_cvolume cvolume; /**< actual sink input volume */
    vlc_tick_t first_pts; /**< Play time of buffer start */

    /* Real audio held back while the device latency is being measured. It
     * cannot be known before the stream runs - PulseAudio reports sink_usec
     * as zero while corked - so the stream is run on silence until it reads
     * true, and whatever arrives meanwhile waits here. */
    struct {
        block_t *first;
        block_t **last;
    } fifo;
    vlc_tick_t device_latency; /**< What the device adds, or VLC_TICK_INVALID */

    pa_volume_t volume_force; /**< Forced volume (stream must be NULL) */
    pa_stream_flags_t flags_force; /**< Forced flags (stream must be NULL) */
    char *sink_force; /**< Forced sink name (stream must be NULL) */

    struct sink *sinks; /**< Locally-cached list of sinks */
};

static void VolumeReport(audio_output_t *aout)
{
    aout_sys_t *sys = aout->sys;
    pa_volume_t volume = pa_cvolume_max(&sys->cvolume);

    aout_VolumeReport(aout, (float)volume / PA_VOLUME_NORM);
}

/*** Sink ***/
static void sink_add_cb(pa_context *ctx, const pa_sink_info *i, int eol,
                        void *userdata)
{
    audio_output_t *aout = userdata;
    aout_sys_t *sys = aout->sys;

    if (eol)
    {
        pa_threaded_mainloop_signal(sys->mainloop, 0);
        return;
    }
    (void) ctx;

    msg_Dbg(aout, "adding sink %"PRIu32": %s (%s)", i->index, i->name,
            i->description);
    aout_HotplugReport(aout, i->name, i->description);

    size_t namelen = strlen(i->name);
    struct sink *sink = malloc(sizeof (*sink) + namelen);
    if (unlikely(sink == NULL))
        return;

    sink->next = sys->sinks;
    sink->index = i->index;
    memcpy(sink->name, i->name, namelen + 1);
    sys->sinks = sink;
}

static void sink_mod_cb(pa_context *ctx, const pa_sink_info *i, int eol,
                        void *userdata)
{
    audio_output_t *aout = userdata;

    if (eol)
        return;
    (void) ctx;

    msg_Dbg(aout, "changing sink %"PRIu32": %s (%s)", i->index, i->name,
            i->description);
    aout_HotplugReport(aout, i->name, i->description);
}

static void sink_del(uint32_t index, audio_output_t *aout)
{
    aout_sys_t *sys = aout->sys;
    struct sink **pp = &sys->sinks, *sink;

    msg_Dbg(aout, "removing sink %"PRIu32, index);

    while ((sink = *pp) != NULL)
        if (sink->index == index)
        {
            *pp = sink->next;
            aout_HotplugReport(aout, sink->name, NULL);
            free(sink);
        }
        else
            pp = &sink->next;
}

static void sink_event(pa_context *ctx, unsigned type, uint32_t idx,
                       audio_output_t *aout)
{
    pa_operation *op = NULL;

    switch (type)
    {
        case PA_SUBSCRIPTION_EVENT_NEW:
            op = pa_context_get_sink_info_by_index(ctx, idx, sink_add_cb,
                                                   aout);
            break;
        case PA_SUBSCRIPTION_EVENT_CHANGE:
            op = pa_context_get_sink_info_by_index(ctx, idx, sink_mod_cb,
                                                   aout);
            break;
        case PA_SUBSCRIPTION_EVENT_REMOVE:
            sink_del(idx, aout);
            break;
    }
    if (op != NULL)
        pa_operation_unref(op);
}


/*** Latency management and lip synchronization ***/
static void stream_start_now(pa_stream *s, audio_output_t *aout)
{
    pa_operation *op;

    assert (aout->sys->trigger == NULL);

    op = pa_stream_cork(s, 0, NULL, NULL);
    if (op != NULL)
        pa_operation_unref(op);
    op = pa_stream_trigger(s, NULL, NULL);
    if (likely(op != NULL))
        pa_operation_unref(op);
}

/* Defined below; frees the block whose address data_convert() stored in
 * front of the data. */
static void data_free(void *data);

/**
 * Writes up to i_duration of silence, returning what it managed to write.
 *
 * Used to keep the stream running until its clock can be trusted.
 */
static vlc_tick_t stream_silence(pa_stream *s, audio_output_t *aout,
                                 vlc_tick_t i_duration)
{
    const pa_sample_spec *ss = pa_stream_get_sample_spec(s);
    vlc_tick_t written = 0;

    while (written < i_duration)
    {
        size_t len = pa_usec_to_bytes(i_duration - written, ss);
        void *ptr;

        if (len == 0)
            break;

        if (pa_stream_begin_write(s, &ptr, &len) < 0 || ptr == NULL || len == 0)
            break;

        memset(ptr, 0, len);

        if (pa_stream_write(s, ptr, len, NULL, 0, PA_SEEK_RELATIVE) < 0)
        {
            vlc_pa_error(aout, "cannot write silence", aout->sys->context);
            pa_stream_cancel_write(s);
            break;
        }

        written += pa_bytes_to_usec(len, ss);
    }

    return written;
}

/** How much has been handed to the server but not yet played. */
static vlc_tick_t stream_queued(pa_stream *s)
{
    const pa_timing_info *ti = pa_stream_get_timing_info(s);
    const pa_sample_spec *ss = pa_stream_get_sample_spec(s);
    int64_t queued;

    if (ti == NULL || ti->write_index_corrupt || ti->read_index_corrupt)
        return VLC_TICK_INVALID;

    queued = ti->write_index - ti->read_index;

    return queued > 0 ? pa_bytes_to_usec(queued, ss) : 0;
}

/**
 * How long after the stream is started its first sample would be heard.
 *
 * Only the device is in the way of it; whatever else is queued sits behind it.
 * What PulseAudio reports counts that queue in, and leaves the device out
 * while corked, so it is wrong in both directions for timing a start.
 */
static vlc_tick_t stream_start_delay(audio_output_t *aout, pa_stream *s)
{
    aout_sys_t *sys = aout->sys;

    if (sys->device_latency != VLC_TICK_INVALID)
        return sys->device_latency;

    /* Not yet known: nothing better than what the stream says. */
    return vlc_pa_get_latency(aout, sys->context, s);
}

/**
 * Whether the stream clock can be trusted yet.
 *
 * calc_time() folds the sink and transport latencies in only while the stream
 * runs ("if (!corked && !suspended)"), and then clamps the playback position
 * at zero, so what PulseAudio reports is short by the device latency until the
 * stream has played as much as the device holds. That is a couple of
 * milliseconds on a local sink and over two hundred on A2DP.
 */
static bool stream_clock_is_synced(pa_stream *s)
{
    const pa_timing_info *ti = pa_stream_get_timing_info(s);
    const pa_sample_spec *ss = pa_stream_get_sample_spec(s);

    if (pa_stream_is_corked(s) != 0 || ti == NULL || !ti->playing
     || ti->read_index_corrupt || ti->read_index <= 0)
        return false;

    /* A device that really costs nothing clears this as soon as it has played
     * anything; demanding a non-zero cost would never let such a sink start. */
    return pa_bytes_to_usec(ti->read_index, ss)
         > ti->sink_usec + ti->transport_usec;
}

/** Hands one queued block to PulseAudio at the given position. */
static void stream_write_block(pa_stream *s, audio_output_t *aout,
                               block_t *block, int64_t offset,
                               pa_seek_mode_t seek)
{
    if (pa_stream_write(s, block->p_buffer, block->i_buffer, data_free, offset,
                        seek) < 0)
    {
        vlc_pa_error(aout, "cannot write", aout->sys->context);
        block_Release(block);
    }
}

/**
 * Writes out everything held back, i_wait from where the stream has got to.
 *
 * A positive wait leaves a hole, which PulseAudio plays as silence. A negative
 * one means playback is already due, so seek on the read index instead: the
 * audio goes in as soon as the server can take it, over the silence written
 * to get here rather than behind it.
 */
static void stream_drain_fifo(pa_stream *s, audio_output_t *aout,
                              vlc_tick_t i_wait)
{
    aout_sys_t *sys = aout->sys;
    const pa_sample_spec *ss = pa_stream_get_sample_spec(s);
    pa_seek_mode_t seek = PA_SEEK_RELATIVE;
    int64_t offset = 0;

    if (i_wait > 0)
        offset = pa_usec_to_bytes(i_wait, ss);
    else
        seek = PA_SEEK_RELATIVE_ON_READ;

    for (block_t *b = sys->fifo.first; b != NULL; )
    {
        block_t *next = b->p_next;

        b->p_next = NULL;
        stream_write_block(s, aout, b, offset, seek);
        offset = 0;
        seek = PA_SEEK_RELATIVE;
        b = next;
    }

    sys->fifo.first = NULL;
    sys->fifo.last = &sys->fifo.first;
}

/**
 * Drops anything held back, which a flush makes stale.
 *
 * What the device costs is kept: that belongs to the device, which a flush
 * does not move, and it is what lets the next start be timed without running
 * the stream on silence again.
 */
static void stream_drop_held(audio_output_t *aout)
{
    aout_sys_t *sys = aout->sys;

    block_ChainRelease(sys->fifo.first);
    sys->fifo.first = NULL;
    sys->fifo.last = &sys->fifo.first;
}

/** Forgets the device too, for a stream that is going away or is new. */
static void stream_forget_device(audio_output_t *aout)
{
    stream_drop_held(aout);
    aout->sys->device_latency = VLC_TICK_INVALID;
}

static void stream_stop(pa_stream *s, audio_output_t *aout)
{
    aout_sys_t *sys = aout->sys;
    pa_operation *op;

    if (sys->trigger != NULL) {
        vlc_pa_rttime_free(sys->mainloop, sys->trigger);
        sys->trigger = NULL;
    }

    op = pa_stream_cork(s, 1, NULL, NULL);
    if (op != NULL)
        pa_operation_unref(op);
}

static void stream_trigger_cb(pa_mainloop_api *api, pa_time_event *e,
                              const struct timeval *tv, void *userdata)
{
    audio_output_t *aout = userdata;
    aout_sys_t *sys = aout->sys;

    assert (sys->trigger == e);

    msg_Dbg(aout, "starting deferred");
    vlc_pa_rttime_free(sys->mainloop, sys->trigger);
    sys->trigger = NULL;
    stream_start_now(sys->stream, aout);
    (void) api; (void) e; (void) tv;
}

/**
 * Starts or resumes the playback stream.
 * Tries start playing back audio samples at the most accurate time
 * in order to minimize desync and resampling during early playback.
 * @note PulseAudio lock required.
 */
static void stream_start(pa_stream *s, audio_output_t *aout)
{
    aout_sys_t *sys = aout->sys;
    vlc_tick_t delta, queued;

    assert (sys->first_pts != VLC_TICK_INVALID);

    /* prebuf is zero so that the start can be timed here rather than left to
     * the server, which also means the server starts on whatever is queued.
     * A flush empties the stream, so the first block back is on its own and
     * the device runs dry the moment it plays. Wait until there is enough
     * behind it to keep going; Play() comes back with the next one. */
    queued = stream_queued(s);
    if (queued != VLC_TICK_INVALID && queued < AOUT_MIN_PREPARE_TIME)
        return;

    if (sys->trigger != NULL) {
        vlc_pa_rttime_free(sys->mainloop, sys->trigger);
        sys->trigger = NULL;
    }

    delta = stream_start_delay(aout, s);
    if (unlikely(delta == VLC_TICK_INVALID)) {
        msg_Dbg(aout, "cannot synchronize start");
        delta = 0; /* screwed */
    }

    delta = (sys->first_pts - mdate()) - delta;
    if (delta > 0) {
        msg_Dbg(aout, "deferring start (%"PRId64" us)", delta);
        delta += pa_rtclock_now();
        sys->trigger = pa_context_rttime_new(sys->context, delta,
                                             stream_trigger_cb, aout);
    } else {
        msg_Warn(aout, "starting late (%"PRId64" us)", delta);
        stream_start_now(s, aout);
    }
}

static void stream_latency_cb(pa_stream *s, void *userdata)
{
    audio_output_t *aout = userdata;
    aout_sys_t *sys = aout->sys;

    /* This callback is _never_ called while paused. */
    if (sys->first_pts == VLC_TICK_INVALID)
        return; /* nothing to do if buffers are (still) empty */
    if (pa_stream_is_corked(s) > 0)
        stream_start(s, aout);
}


/*** Stream helpers ***/
static void stream_state_cb(pa_stream *s, void *userdata)
{
    pa_threaded_mainloop *mainloop = userdata;

    switch (pa_stream_get_state(s)) {
        case PA_STREAM_READY:
        case PA_STREAM_FAILED:
        case PA_STREAM_TERMINATED:
            pa_threaded_mainloop_signal(mainloop, 0);
        default:
            break;
    }
}

static void stream_buffer_attr_cb(pa_stream *s, void *userdata)
{
    audio_output_t *aout = userdata;
    const pa_buffer_attr *pba = pa_stream_get_buffer_attr(s);

    msg_Dbg(aout, "changed buffer metrics: maxlength=%u, tlength=%u, "
            "prebuf=%u, minreq=%u",
            pba->maxlength, pba->tlength, pba->prebuf, pba->minreq);
}

static void stream_event_cb(pa_stream *s, const char *name, pa_proplist *pl,
                            void *userdata)
{
    audio_output_t *aout = userdata;

    if (!strcmp(name, PA_STREAM_EVENT_REQUEST_CORK))
        aout_PolicyReport(aout, true);
    else
    if (!strcmp(name, PA_STREAM_EVENT_REQUEST_UNCORK))
        aout_PolicyReport(aout, false);
    else
    /* FIXME: expose aout_Restart() directly */
    if (!strcmp(name, PA_STREAM_EVENT_FORMAT_LOST)) {
        msg_Dbg (aout, "format lost");
        aout_RestartRequest (aout, AOUT_RESTART_OUTPUT);
    } else
        msg_Warn (aout, "unhandled stream event \"%s\"", name);
    (void) s;
    (void) pl;
}

static void stream_moved_cb(pa_stream *s, void *userdata)
{
    audio_output_t *aout = userdata;
    const char *name = pa_stream_get_device_name(s);

    msg_Dbg(aout, "connected to sink %s", name);
    aout_DeviceReport(aout, name);
}

static void stream_overflow_cb(pa_stream *s, void *userdata)
{
    audio_output_t *aout = userdata;
    aout_sys_t *sys = aout->sys;
    pa_operation *op;

    msg_Err(aout, "overflow, flushing");
    op = pa_stream_flush(s, NULL, NULL);
    if (unlikely(op == NULL))
        return;
    pa_operation_unref(op);
    sys->first_pts = VLC_TICK_INVALID;
    stream_drop_held(aout);
}

static void stream_started_cb(pa_stream *s, void *userdata)
{
    audio_output_t *aout = userdata;

    msg_Dbg(aout, "started");
    (void) s;
}

static void stream_suspended_cb(pa_stream *s, void *userdata)
{
    audio_output_t *aout = userdata;

    msg_Dbg(aout, "suspended");
    (void) s;
}

static void stream_underflow_cb(pa_stream *s, void *userdata)
{
    audio_output_t *aout = userdata;

    msg_Dbg(aout, "underflow");
    (void) s;
}

static int stream_wait(pa_stream *stream, pa_threaded_mainloop *mainloop)
{
    pa_stream_state_t state;

    while ((state = pa_stream_get_state(stream)) != PA_STREAM_READY) {
        if (state == PA_STREAM_FAILED || state == PA_STREAM_TERMINATED)
            return -1;
        pa_threaded_mainloop_wait(mainloop);
    }
    return 0;
}


/*** Sink input ***/
static void sink_input_info_cb(pa_context *ctx, const pa_sink_input_info *i,
                               int eol, void *userdata)
{
    audio_output_t *aout = userdata;
    aout_sys_t *sys = aout->sys;

    if (eol)
        return;
    (void) ctx;

    sys->cvolume = i->volume; /* cache volume for balance preservation */
    VolumeReport(aout);
    aout_MuteReport(aout, i->mute);
}

static void sink_input_event(pa_context *ctx,
                             pa_subscription_event_type_t type,
                             uint32_t idx, audio_output_t *aout)
{
    pa_operation *op;

    /* Gee... PA will not provide the infos directly in the event. */
    switch (type)
    {
        case PA_SUBSCRIPTION_EVENT_REMOVE:
            msg_Err(aout, "sink input killed!");
            break;

        default:
            op = pa_context_get_sink_input_info(ctx, idx, sink_input_info_cb,
                                                aout);
            if (likely(op != NULL))
                pa_operation_unref(op);
            break;
    }
}


/*** Context ***/
static void context_cb(pa_context *ctx, pa_subscription_event_type_t type,
                       uint32_t idx, void *userdata)
{
    audio_output_t *aout = userdata;
    aout_sys_t *sys = aout->sys;
    unsigned facility = type & PA_SUBSCRIPTION_EVENT_FACILITY_MASK;

    type &= PA_SUBSCRIPTION_EVENT_TYPE_MASK;
    switch (facility)
    {
        case PA_SUBSCRIPTION_EVENT_SINK:
            sink_event(ctx, type, idx, userdata);
            break;

        case PA_SUBSCRIPTION_EVENT_SINK_INPUT:
            /* only interested in our sink input */
            if (sys->stream != NULL && idx == pa_stream_get_index(sys->stream))
                sink_input_event(ctx, type, idx, userdata);
            break;

        default: /* unsubscribed facility?! */
            vlc_assert_unreachable();
    }
}


/*** VLC audio output callbacks ***/

static int LatencyGet(audio_output_t *aout, vlc_tick_t *restrict latency)
{
    aout_sys_t *sys = aout->sys;
    int ret = -1;

    pa_threaded_mainloop_lock(sys->mainloop);
    if (sys->device_latency != VLC_TICK_INVALID)
    {
        *latency = sys->device_latency;
        ret = 0;
    }
    pa_threaded_mainloop_unlock(sys->mainloop);
    return ret;
}

static int TimeGet(audio_output_t *aout, vlc_tick_t *restrict delay)
{
    aout_sys_t *sys = aout->sys;
    pa_stream *s = sys->stream;
    int ret = -1;

    pa_threaded_mainloop_lock(sys->mainloop);
    /* Anything held back is not queued yet, so the stream does not account
     * for it. Answering with the silence it is running on instead describes
     * neither: the core reads audio that is about to be let go all at once as
     * a second of earliness, pads a second of silence to meet it, then finds
     * that same second late and jumps over it. Say nothing until it is in. */
    if (sys->fifo.first == NULL && stream_clock_is_synced(s))
    {
        vlc_tick_t delta = vlc_pa_get_latency(aout, sys->context, s);
        if (delta != VLC_TICK_INVALID)
        {
            *delay = delta;
            ret = 0;
        }
    }
    pa_threaded_mainloop_unlock(sys->mainloop);
    return ret;
}

/* Memory free callback. The block_t address is in front of the data. */
static void data_free(void *data)
{
    block_t **pp = data, *block;

    memcpy(&block, pp - 1, sizeof (block));
    block_Release(block);
}

static void *data_convert(block_t **pp)
{
    block_t *block = *pp;
    /* In most cases, there is enough head room, and this is really cheap: */
    block = block_Realloc(block, sizeof (block), block->i_buffer);
    *pp = block;
    if (unlikely(block == NULL))
        return NULL;

    memcpy(block->p_buffer, &block, sizeof (block));
    block->p_buffer += sizeof (block);
    block->i_buffer -= sizeof (block);
    return block->p_buffer;
}

/**
 * Lets the held audio go once the stream clock can be trusted.
 * @note PulseAudio lock required.
 */
static void stream_try_start(pa_stream *s, audio_output_t *aout)
{
    aout_sys_t *sys = aout->sys;
    const pa_timing_info *ti;
    vlc_tick_t latency, wait;

    if (sys->device_latency != VLC_TICK_INVALID || !stream_clock_is_synced(s))
        return;

    ti = pa_stream_get_timing_info(s);

    latency = vlc_pa_get_latency(aout, sys->context, s);
    if (latency == VLC_TICK_INVALID || latency <= 0
     || latency >= VLC_TICK_FROM_SEC(5))
        return;

    sys->device_latency = ti->sink_usec + ti->transport_usec;
    msg_Dbg(aout, "clock synced: device %"PRId64" us", sys->device_latency);

    if (sys->first_pts == VLC_TICK_INVALID)
        return; /* nothing held back; the start can be timed from here */

    /* The silence written to get here sits in front of the audio, so drop
     * whatever of it is not needed to reach the start. */
    wait = (sys->first_pts - mdate()) - latency;

    msg_Dbg(aout, "starting in %"PRId64" us", wait);
    stream_drain_fifo(s, aout, wait);
}

/**
 * Feeds the stream while it is running on silence.
 *
 * Reaching the sync point takes as much silence as the device holds, which is
 * far more than priming writes. It cannot be topped up from Play(): holding
 * the audio back fills the core's FIFO, which stalls the decoder, which stops
 * Play() being called at all. Let the server ask for what it needs instead.
 */
static void stream_write_cb(pa_stream *s, size_t nbytes, void *userdata)
{
    audio_output_t *aout = userdata;
    aout_sys_t *sys = aout->sys;
    vlc_tick_t queued, room;

    if (sys->device_latency != VLC_TICK_INVALID)
        return;

    /* Only ever up to the target fill. The server will take as much as
     * maxlength allows, which is seconds of it, and every one of those is a
     * second of delay the audio has to sit behind. */
    queued = stream_queued(s);
    room = 3 * AOUT_MIN_PREPARE_TIME
           - (queued == VLC_TICK_INVALID ? 0 : queued);

    if (room > 0)
    {
        vlc_tick_t asked = pa_bytes_to_usec(nbytes,
                                            pa_stream_get_sample_spec(s));

        stream_silence(s, aout, room < asked ? room : asked);
    }

    stream_try_start(s, aout);
}

/**
 * Queue one audio frame to the playback stream
 */
static void Play(audio_output_t *aout, block_t *block)
{
    aout_sys_t *sys = aout->sys;
    pa_stream *s = sys->stream;

    const void *ptr = data_convert(&block);
    if (unlikely(ptr == NULL))
        return;

    size_t len = block->i_buffer;

    /* Note: The core already holds the output FIFO lock at this point.
     * Therefore we must not under any circumstances (try to) acquire the
     * output FIFO lock while the PulseAudio threaded main loop lock is held
     * (including from PulseAudio stream callbacks). Otherwise lock inversion
     * will take place, and sooner or later a deadlock. */
    pa_threaded_mainloop_lock(sys->mainloop);

    if (sys->first_pts == VLC_TICK_INVALID)
        sys->first_pts = block->i_pts;

    if (sys->device_latency == VLC_TICK_INVALID)
    {
        /* The device latency cannot be read until the stream has run for
         * as long as the device holds (see stream_clock_is_synced), and this
         * sink reports nothing for itself either. Run it on silence until
         * then, holding the audio back meanwhile: starting on the corked
         * figure put playback a fifth of a second late on A2DP. */
        if (pa_stream_is_corked(s) > 0)
        {
            /* Prime it so the server has something to ask about, then let it
             * run; stream_write_cb() keeps it fed from there. */
            stream_silence(s, aout, AOUT_MIN_PREPARE_TIME);
            stream_start_now(s, aout);
        }

        block->p_next = NULL;
        *sys->fifo.last = block;
        sys->fifo.last = &block->p_next;

        stream_try_start(s, aout);

        pa_threaded_mainloop_unlock(sys->mainloop);
        return;
    }

    if (pa_stream_is_corked(s) > 0)
        stream_start(s, aout);


#if 0 /* Fault injector to test underrun recovery */
    static volatile unsigned u = 0;
    if ((++u % 1000) == 0) {
        msg_Err(aout, "fault injection");
        pa_operation_unref(pa_stream_flush(s, NULL, NULL));
    }
#endif

    if (pa_stream_write(s, ptr, len, data_free, 0, PA_SEEK_RELATIVE) < 0) {
        vlc_pa_error(aout, "cannot write", sys->context);
        block_Release(block);
    }

    pa_threaded_mainloop_unlock(sys->mainloop);
}

/**
 * Cork or uncork the playback stream
 */
static void Pause(audio_output_t *aout, bool paused, vlc_tick_t date)
{
    aout_sys_t *sys = aout->sys;
    pa_stream *s = sys->stream;

    pa_threaded_mainloop_lock(sys->mainloop);

    if (paused) {
        pa_stream_set_latency_update_callback(s, NULL, NULL);
        stream_stop(s, aout);
    } else {
        pa_stream_set_latency_update_callback(s, stream_latency_cb, aout);
        if (likely(sys->first_pts != VLC_TICK_INVALID))
            stream_start_now(s, aout);
    }

    pa_threaded_mainloop_unlock(sys->mainloop);
    (void) date;
}

/**
 * Flush or drain the playback stream
 */
static void Flush(audio_output_t *aout, bool wait)
{
    aout_sys_t *sys = aout->sys;
    pa_stream *s = sys->stream;
    pa_operation *op;

    pa_threaded_mainloop_lock(sys->mainloop);

    if (unlikely(pa_stream_is_corked(s) > 0))
    {
        /* Drain while the stream is corked. It happens with very small input
         * when the stream is drained while the start is still being deferred.
         * In that case, we need start the stream before we actually drain it.
         * */
        if (sys->trigger != NULL)
        {
            vlc_pa_rttime_free(sys->mainloop, sys->trigger);
            sys->trigger = NULL;
        }
        stream_start_now(s, aout);
    }

    if (wait)
    {
        op = pa_stream_drain(s, NULL, NULL);

        /* XXX: Loosy drain emulation.
         * See #18141: drain callback is never received */
        vlc_tick_t delay;
        if (TimeGet(aout, &delay) == 0 && delay <= INT64_C(5000000))
            msleep(delay);
    }
    else
        op = pa_stream_flush(s, NULL, NULL);
    if (op != NULL)
        pa_operation_unref(op);
    sys->first_pts = VLC_TICK_INVALID;
    stream_drop_held(aout);

    /* A drain plays the queue out; it does not finish with the device. What
     * follows one is usually the next pass of the same item, and stopping the
     * stream means starting it again for that. The idle timeout parks it if
     * nothing more arrives. */
    if (!wait)
        stream_stop(s, aout);

    pa_threaded_mainloop_unlock(sys->mainloop);
}

static int VolumeSet(audio_output_t *aout, float vol)
{
    aout_sys_t *sys = aout->sys;
    pa_stream *s = sys->stream;
    pa_operation *op;
    pa_volume_t volume;

    /* VLC provides the software volume so convert directly to PulseAudio
     * software volume, pa_volume_t. This is not a linear amplification factor
     * so do not use PulseAudio linear amplification! */
    vol *= PA_VOLUME_NORM;
    if (unlikely(vol >= (float)PA_VOLUME_MAX))
        volume = PA_VOLUME_MAX;
    else
        volume = lroundf(vol);

    if (s == NULL)
    {
        sys->volume_force = volume;
        aout_VolumeReport(aout, (float)volume / (float)PA_VOLUME_NORM);
        return 0;
    }

    pa_threaded_mainloop_lock(sys->mainloop);

    if (!pa_cvolume_valid(&sys->cvolume))
    {
        const pa_sample_spec *ss = pa_stream_get_sample_spec(s);

        msg_Warn(aout, "balance clobbered by volume change");
        pa_cvolume_set(&sys->cvolume, ss->channels, PA_VOLUME_NORM);
    }

    /* Preserve the balance (VLC does not support it). */
    pa_cvolume cvolume = sys->cvolume;
    pa_cvolume_scale(&cvolume, PA_VOLUME_NORM);
    pa_sw_cvolume_multiply_scalar(&cvolume, &cvolume, volume);
    assert(pa_cvolume_valid(&cvolume));

    op = pa_context_set_sink_input_volume(sys->context, pa_stream_get_index(s),
                                          &cvolume, NULL, NULL);
    if (likely(op != NULL))
        pa_operation_unref(op);
    pa_threaded_mainloop_unlock(sys->mainloop);
    return likely(op != NULL) ? 0 : -1;
}

static int MuteSet(audio_output_t *aout, bool mute)
{
    aout_sys_t *sys = aout->sys;

    if (sys->stream == NULL)
    {
        sys->flags_force &= ~(PA_STREAM_START_MUTED|PA_STREAM_START_UNMUTED);
        sys->flags_force |=
            mute ? PA_STREAM_START_MUTED : PA_STREAM_START_UNMUTED;
        aout_MuteReport(aout, mute);
        return 0;
    }

    pa_operation *op;
    uint32_t idx = pa_stream_get_index(sys->stream);
    pa_threaded_mainloop_lock(sys->mainloop);
    op = pa_context_set_sink_input_mute(sys->context, idx, mute, NULL, NULL);
    if (likely(op != NULL))
        pa_operation_unref(op);
    pa_threaded_mainloop_unlock(sys->mainloop);

    return 0;
}

static int StreamMove(audio_output_t *aout, const char *name)
{
    aout_sys_t *sys = aout->sys;

    if (sys->stream == NULL)
    {
        msg_Dbg(aout, "will connect to sink %s", name);
        free(sys->sink_force);
        sys->sink_force = strdup(name);
        aout_DeviceReport(aout, name);
        return 0;
    }

    pa_operation *op;
    uint32_t idx = pa_stream_get_index(sys->stream);

    pa_threaded_mainloop_lock(sys->mainloop);
    op = pa_context_move_sink_input_by_name(sys->context, idx, name,
                                            NULL, NULL);
    if (likely(op != NULL)) {
        pa_operation_unref(op);
        msg_Dbg(aout, "moving to sink %s", name);
    } else
        vlc_pa_error(aout, "cannot move sink input", sys->context);
    pa_threaded_mainloop_unlock(sys->mainloop);

    return (op != NULL) ? 0 : -1;
}

static void Stop(audio_output_t *);

static int strcmp_void(const void *a, const void *b)
{
    const char *const *entry = b;
    return strcmp(a, *entry);
}

static const char *str_map(const char *key, const char *const table[][2],
                           size_t n)
{
     const char **r = bsearch(key, table, n, sizeof (*table), strcmp_void);
     return (r != NULL) ? r[1] : NULL;
}

/**
 * Create a PulseAudio playback stream, a.k.a. a sink input.
 */
static int Start(audio_output_t *aout, audio_sample_format_t *restrict fmt)
{
    aout_sys_t *sys = aout->sys;

    /* Sample format specification */
    struct pa_sample_spec ss = { .format = PA_SAMPLE_INVALID };
    pa_encoding_t encoding = PA_ENCODING_PCM;

    switch (fmt->i_format)
    {
        case VLC_CODEC_FL64:
            fmt->i_format = VLC_CODEC_FL32;
            /* fall through */
        case VLC_CODEC_FL32:
            ss.format = PA_SAMPLE_FLOAT32NE;
            break;
        case VLC_CODEC_S32N:
            ss.format = PA_SAMPLE_S32NE;
            break;
        case VLC_CODEC_S16N:
            ss.format = PA_SAMPLE_S16NE;
            break;
        case VLC_CODEC_U8:
            ss.format = PA_SAMPLE_U8;
            break;
        case VLC_CODEC_A52:
            fmt->i_format = VLC_CODEC_SPDIFL;
            fmt->i_bytes_per_frame = 4;
            fmt->i_frame_length = 1;
            fmt->i_physical_channels = AOUT_CHANS_2_0;
            fmt->i_channels = 2;
            encoding = PA_ENCODING_AC3_IEC61937;
            ss.format = PA_SAMPLE_S16NE;
            break;
        case VLC_CODEC_EAC3:
            fmt->i_format = VLC_CODEC_SPDIFL;
            fmt->i_bytes_per_frame = 4;
            fmt->i_frame_length = 1;
            fmt->i_physical_channels = AOUT_CHANS_2_0;
            fmt->i_channels = 2;
            encoding = PA_ENCODING_EAC3_IEC61937;
            ss.format = PA_SAMPLE_S16NE;
            break;
        /* case VLC_CODEC_MPGA:
            fmt->i_format = VLC_CODEC_SPDIFL FIXME;
            encoding = PA_ENCODING_MPEG_IEC61937;
            break;*/
        case VLC_CODEC_DTS:
            fmt->i_format = VLC_CODEC_SPDIFL;
            fmt->i_bytes_per_frame = 4;
            fmt->i_frame_length = 1;
            fmt->i_physical_channels = AOUT_CHANS_2_0;
            fmt->i_channels = 2;
            encoding = PA_ENCODING_DTS_IEC61937;
            ss.format = PA_SAMPLE_S16NE;
            break;
        default:
            if (!AOUT_FMT_LINEAR(fmt) || aout_FormatNbChannels(fmt) == 0)
                return VLC_EGENERIC;

            if (HAVE_FPU)
            {
                fmt->i_format = VLC_CODEC_FL32;
                ss.format = PA_SAMPLE_FLOAT32NE;
            }
            else
            {
                fmt->i_format = VLC_CODEC_S16N;
                ss.format = PA_SAMPLE_S16NE;
            }
            break;
    }

    ss.rate = fmt->i_rate;
    ss.channels = fmt->i_channels;
    if (!pa_sample_spec_valid(&ss)) {
        msg_Err(aout, "unsupported sample specification");
        return VLC_EGENERIC;
    }

    /* Stream parameters */
    pa_stream_flags_t flags = sys->flags_force
                            | PA_STREAM_START_CORKED
                            | PA_STREAM_INTERPOLATE_TIMING
                            | PA_STREAM_NOT_MONOTONIC
                            | PA_STREAM_AUTO_TIMING_UPDATE
                            | PA_STREAM_FIX_RATE;

    struct pa_buffer_attr attr;
    attr.maxlength = -1;
    /* PulseAudio goes berserk if the target length (tlength) is not
     * significantly longer than 2 periods (minreq), or when the period length
     * is unspecified and the target length is short. */
    attr.tlength = pa_usec_to_bytes(3 * AOUT_MIN_PREPARE_TIME, &ss);
    attr.prebuf = 0; /* trigger manually */
    attr.minreq = pa_usec_to_bytes(AOUT_MIN_PREPARE_TIME, &ss);
    attr.fragsize = 0; /* not used for output */

    pa_cvolume *cvolume = NULL, cvolumebuf;
    if (PA_VOLUME_IS_VALID(sys->volume_force))
    {
        cvolume = &cvolumebuf;
        pa_cvolume_set(cvolume, ss.channels, sys->volume_force);
    }

    sys->trigger = NULL;
    pa_cvolume_init(&sys->cvolume);
    sys->first_pts = VLC_TICK_INVALID;
    stream_forget_device(aout);

    pa_format_info *formatv = pa_format_info_new();
    formatv->encoding = encoding;
    pa_format_info_set_rate(formatv, ss.rate);
    if (ss.format != PA_SAMPLE_INVALID)
        pa_format_info_set_sample_format(formatv, ss.format);

    if (fmt->channel_type == AUDIO_CHANNEL_TYPE_AMBISONICS)
    {
        fmt->channel_type = AUDIO_CHANNEL_TYPE_BITMAP;

        /* Setup low latency in order to quickly react to ambisonics
         * filters viewpoint changes. */
        flags |= PA_STREAM_ADJUST_LATENCY;
        attr.tlength = pa_usec_to_bytes(3 * AOUT_MIN_PREPARE_TIME, &ss);
    }

    if (encoding != PA_ENCODING_PCM)
    {
        pa_format_info_set_channels(formatv, ss.channels);

        /* FIX flags are only permitted for PCM, and there is no way to pass
         * different flags for different formats... */
        flags &= ~(PA_STREAM_FIX_FORMAT
                 | PA_STREAM_FIX_RATE
                 | PA_STREAM_FIX_CHANNELS);
    }
    else
    {
        /* Channel mapping (order defined in vlc_aout.h) */
        struct pa_channel_map map;
        map.channels = 0;

        if (fmt->i_physical_channels & AOUT_CHAN_LEFT)
            map.map[map.channels++] = PA_CHANNEL_POSITION_FRONT_LEFT;
        if (fmt->i_physical_channels & AOUT_CHAN_RIGHT)
            map.map[map.channels++] = PA_CHANNEL_POSITION_FRONT_RIGHT;
        if (fmt->i_physical_channels & AOUT_CHAN_MIDDLELEFT)
            map.map[map.channels++] = PA_CHANNEL_POSITION_SIDE_LEFT;
        if (fmt->i_physical_channels & AOUT_CHAN_MIDDLERIGHT)
            map.map[map.channels++] = PA_CHANNEL_POSITION_SIDE_RIGHT;
        if (fmt->i_physical_channels & AOUT_CHAN_REARLEFT)
            map.map[map.channels++] = PA_CHANNEL_POSITION_REAR_LEFT;
        if (fmt->i_physical_channels & AOUT_CHAN_REARRIGHT)
            map.map[map.channels++] = PA_CHANNEL_POSITION_REAR_RIGHT;
        if (fmt->i_physical_channels & AOUT_CHAN_REARCENTER)
            map.map[map.channels++] = PA_CHANNEL_POSITION_REAR_CENTER;
        if (fmt->i_physical_channels & AOUT_CHAN_CENTER)
        {
            if (ss.channels == 1)
                map.map[map.channels++] = PA_CHANNEL_POSITION_MONO;
            else
                map.map[map.channels++] = PA_CHANNEL_POSITION_FRONT_CENTER;
        }
        if (fmt->i_physical_channels & AOUT_CHAN_LFE)
            map.map[map.channels++] = PA_CHANNEL_POSITION_LFE;

        static_assert(AOUT_CHAN_MAX == 9, "Missing channels");

        for (unsigned i = 0; map.channels < ss.channels; i++) {
            map.map[map.channels++] = PA_CHANNEL_POSITION_AUX0 + i;
            msg_Warn(aout, "mapping channel %"PRIu8" to AUX%u", map.channels, i);
        }

        if (!pa_channel_map_valid(&map)) {
            msg_Err(aout, "unsupported channel map");
            return VLC_EGENERIC;
        } else {
            const char *name = pa_channel_map_to_name(&map);
            msg_Dbg(aout, "using %s channel map", (name != NULL) ? name : "?");
        }

        pa_format_info_set_channels(formatv, ss.channels);
        pa_format_info_set_channel_map(formatv, &map);
    }

    /* Create a playback stream */
    pa_proplist *props = pa_proplist_new();
    if (likely(props != NULL))
    {
        /* TODO: set other stream properties */
        char *str = var_InheritString(aout, "role");
        if (str != NULL)
        {
            static const char *const role_map[][2] = {
                { "accessibility", "a11y"       },
                { "animation",     "animation"  },
                { "communication", "phone"      },
                { "game",          "game"       },
                { "music",         "music"      },
                { "notification",  "event"      },
                { "production",    "production" },
                { "test",          "test"       },
                { "video",         "video"      },
            };
            const char *role = str_map(str, role_map, ARRAY_SIZE(role_map));
            if (role != NULL)
                pa_proplist_sets(props, PA_PROP_MEDIA_ROLE, role);
            free(str);
       }
    }

    pa_threaded_mainloop_lock(sys->mainloop);
    pa_stream *s = pa_stream_new_extended(sys->context, "audio stream",
                                          &formatv, 1, props);

    if (likely(props != NULL))
        pa_proplist_free(props);
    pa_format_info_free(formatv);

    if (s == NULL) {
        pa_threaded_mainloop_unlock(sys->mainloop);
        vlc_pa_error(aout, "stream creation failure", sys->context);
        return VLC_EGENERIC;
    }
    assert(sys->stream == NULL);
    sys->stream = s;
    pa_stream_set_state_callback(s, stream_state_cb, sys->mainloop);
    pa_stream_set_buffer_attr_callback(s, stream_buffer_attr_cb, aout);
    pa_stream_set_event_callback(s, stream_event_cb, aout);
    pa_stream_set_latency_update_callback(s, stream_latency_cb, aout);
    pa_stream_set_moved_callback(s, stream_moved_cb, aout);
    pa_stream_set_overflow_callback(s, stream_overflow_cb, aout);
    pa_stream_set_started_callback(s, stream_started_cb, aout);
    pa_stream_set_suspended_callback(s, stream_suspended_cb, aout);
    pa_stream_set_underflow_callback(s, stream_underflow_cb, aout);
    pa_stream_set_write_callback(s, stream_write_cb, aout);

    if (pa_stream_connect_playback(s, sys->sink_force, &attr, flags,
                                   cvolume, NULL) < 0
     || stream_wait(s, sys->mainloop)) {
        if (encoding != PA_ENCODING_PCM)
            vlc_pa_error(aout, "digital pass-through stream connection failure",
                         sys->context);
        else
            vlc_pa_error(aout, "stream connection failure", sys->context);
        goto fail;
    }
    sys->volume_force = PA_VOLUME_INVALID;
    sys->flags_force = PA_STREAM_NOFLAGS;
    free(sys->sink_force);
    sys->sink_force = NULL;

    /* Settle the clock before playback rather than during it. Measuring takes
     * as long as the device holds, and doing it once audio is flowing leaves
     * that audio late by the measurement - which the core makes up by dropping
     * a chunk of it, heard as a splice. Nothing is playing yet, so the wait
     * only adds to start-up latency. */
    if (sys->device_latency == VLC_TICK_INVALID)
    {
        stream_silence(s, aout, AOUT_MIN_PREPARE_TIME);
        stream_start_now(s, aout);

        for (unsigned i = 0; i < 100; i++)
        {
            if (sys->device_latency != VLC_TICK_INVALID)
                break;

            pa_threaded_mainloop_unlock(sys->mainloop);
            msleep(VLC_TICK_FROM_MS(10));
            pa_threaded_mainloop_lock(sys->mainloop);
            stream_try_start(s, aout);
        }

        if (sys->device_latency == VLC_TICK_INVALID)
            msg_Dbg(aout, "clock did not settle; measuring as it plays");
        else
            stream_stop(s, aout); /* start it again when the audio is due */
    }

    if (encoding == PA_ENCODING_PCM)
    {
        const struct pa_sample_spec *spec = pa_stream_get_sample_spec(s);
        fmt->i_rate = spec->rate;
    }

    stream_buffer_attr_cb(s, aout);
    stream_moved_cb(s, aout);
    pa_threaded_mainloop_unlock(sys->mainloop);

    return VLC_SUCCESS;

fail:
    pa_threaded_mainloop_unlock(sys->mainloop);
    Stop(aout);
    return VLC_EGENERIC;
}

/**
 * Removes a PulseAudio playback stream
 */
static void Stop(audio_output_t *aout)
{
    aout_sys_t *sys = aout->sys;
    pa_stream *s = sys->stream;

    pa_threaded_mainloop_lock(sys->mainloop);
    stream_forget_device(aout); /* nothing held back outlives the stream */
    if (unlikely(sys->trigger != NULL))
        vlc_pa_rttime_free(sys->mainloop, sys->trigger);
    pa_stream_disconnect(s);

    /* Clear all callbacks */
    pa_stream_set_state_callback(s, NULL, NULL);
    pa_stream_set_buffer_attr_callback(s, NULL, NULL);
    pa_stream_set_event_callback(s, NULL, NULL);
    pa_stream_set_latency_update_callback(s, NULL, NULL);
    pa_stream_set_moved_callback(s, NULL, NULL);
    pa_stream_set_overflow_callback(s, NULL, NULL);
    pa_stream_set_started_callback(s, NULL, NULL);
    pa_stream_set_suspended_callback(s, NULL, NULL);
    pa_stream_set_underflow_callback(s, NULL, NULL);
    pa_stream_set_write_callback(s, NULL, NULL);

    pa_stream_unref(s);
    sys->stream = NULL;
    pa_threaded_mainloop_unlock(sys->mainloop);
}

static int Open(vlc_object_t *obj)
{
    audio_output_t *aout = (audio_output_t *)obj;
    aout_sys_t *sys = malloc(sizeof (*sys));
    pa_operation *op;

    if (unlikely(sys == NULL))
        return VLC_ENOMEM;

    /* Allocate structures */
    pa_context *ctx = vlc_pa_connect(obj, &sys->mainloop);
    if (ctx == NULL)
    {
        free(sys);
        return VLC_EGENERIC;
    }
    sys->stream = NULL;
    sys->context = ctx;
    sys->volume_force = PA_VOLUME_INVALID;
    sys->flags_force = PA_STREAM_NOFLAGS;
    sys->sink_force = NULL;
    sys->sinks = NULL;
    sys->fifo.first = NULL;
    sys->fifo.last = &sys->fifo.first;
    sys->device_latency = VLC_TICK_INVALID;

    aout->sys = sys;
    aout->start = Start;
    aout->stop = Stop;
    aout->time_get = TimeGet;
    aout->latency_get = LatencyGet;
    aout->play = Play;
    aout->pause = Pause;
    aout->flush = Flush;
    aout->volume_set = VolumeSet;
    aout->mute_set = MuteSet;
    aout->device_select = StreamMove;

    pa_threaded_mainloop_lock(sys->mainloop);
    /* Sinks (output devices) list */
    op = pa_context_get_sink_info_list(sys->context, sink_add_cb, aout);
    if (likely(op != NULL))
    {
        while (pa_operation_get_state(op) == PA_OPERATION_RUNNING)
            pa_threaded_mainloop_wait(sys->mainloop);
        pa_operation_unref(op);
    }

    /* Context events */
    const pa_subscription_mask_t mask = PA_SUBSCRIPTION_MASK_SINK
                                      | PA_SUBSCRIPTION_MASK_SINK_INPUT;
    pa_context_set_subscribe_callback(sys->context, context_cb, aout);
    op = pa_context_subscribe(sys->context, mask, NULL, NULL);
    if (likely(op != NULL))
       pa_operation_unref(op);
    pa_threaded_mainloop_unlock(sys->mainloop);

    return VLC_SUCCESS;
}

static void Close(vlc_object_t *obj)
{
    audio_output_t *aout = (audio_output_t *)obj;
    aout_sys_t *sys = aout->sys;
    pa_context *ctx = sys->context;

    pa_threaded_mainloop_lock(sys->mainloop);
    pa_context_set_subscribe_callback(sys->context, NULL, NULL);
    pa_threaded_mainloop_unlock(sys->mainloop);
    vlc_pa_disconnect(obj, ctx, sys->mainloop);

    for (struct sink *sink = sys->sinks, *next; sink != NULL; sink = next)
    {
        next = sink->next;
        free(sink);
    }
    free(sys->sink_force);
    free(sys);
}
