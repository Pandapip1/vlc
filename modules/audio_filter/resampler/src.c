/*****************************************************************************
 * src.c : Secret Rabbit Code (a.k.a. libsamplerate) resampler
 *****************************************************************************
 * Copyright (C) 2011-2012 Rémi Denis-Courmont
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
 * NOTA BENE: this module requires the linking against a library which is
 * known to require licensing under the GNU General Public License version 2
 * (or later). Therefore, the result of compiling this module will normally
 * be subject to the terms of that later license.
 *****************************************************************************/


#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_aout.h>
#include <vlc_filter.h>
#include <samplerate.h>
#include <math.h>

#define SRC_CONV_TYPE_TEXT N_("Sample rate converter type")
#define SRC_CONV_TYPE_LONGTEXT N_( \
    "Different resampling algorithms are supported. " \
    "The best one is slower, while the fast one exhibits low quality.")
static const int conv_type_values[] = {
    SRC_SINC_BEST_QUALITY, SRC_SINC_MEDIUM_QUALITY, SRC_SINC_FASTEST,
    SRC_ZERO_ORDER_HOLD, SRC_LINEAR,
};
static const char *const conv_type_texts[] = {
    N_("Sinc function (best quality)"), N_("Sinc function (medium quality)"),
    N_("Sinc function (fast)"), N_("Zero Order Hold (fastest)"), N_("Linear (fastest)"),
};

static int Open (vlc_object_t *);
static int OpenResampler (vlc_object_t *);
static void Close (filter_t *);

vlc_module_begin ()
    set_shortname (N_("SRC resampler"))
    set_description (N_("Secret Rabbit Code (libsamplerate) resampler") )
    set_subcategory (SUBCAT_AUDIO_RESAMPLER)
    add_integer ("src-converter-type", SRC_SINC_FASTEST,
                 SRC_CONV_TYPE_TEXT, SRC_CONV_TYPE_LONGTEXT)
        change_integer_list (conv_type_values, conv_type_texts)
    set_capability ("audio converter", 50)
    set_callback (Open)

    add_submodule ()
    set_capability ("audio resampler", 50)
    set_callback (OpenResampler)
vlc_module_end ()

typedef struct
{
    SRC_STATE *state;
    bool dirty; /**< Whether the resampler holds buffered input frames */
} filter_sys_t;

/* Upper bound, in frames, on the amount of input that libsamplerate keeps in
 * hand while resampling, i.e. half the length of its interpolation filter:
 * 20, 47 and 144 frames for the fast, medium and best sinc converters
 * respectively, and none for the linear and zero order hold ones. */
#define SRC_LOOKAHEAD 256

static block_t *Resample (filter_t *, block_t *);

static int Open (vlc_object_t *obj)
{
    filter_t *filter = (filter_t *)obj;

    /* Will change rate */
    if (filter->fmt_in.audio.i_rate == filter->fmt_out.audio.i_rate)
        return VLC_EGENERIC;
    return OpenResampler (obj);
}

static int OpenResampler (vlc_object_t *obj)
{
    filter_t *filter = (filter_t *)obj;

    /* Only float->float */
    if (filter->fmt_in.audio.i_format != VLC_CODEC_FL32
     || filter->fmt_out.audio.i_format != VLC_CODEC_FL32
    /* No channels remapping */
     || filter->fmt_in.audio.i_channels != filter->fmt_out.audio.i_channels )
        return VLC_EGENERIC;

    filter_sys_t *sys = malloc (sizeof (*sys));
    if (unlikely(sys == NULL))
        return VLC_ENOMEM;

    int type = var_InheritInteger (obj, "src-converter-type");
    int err;

    SRC_STATE *s = src_new (type, filter->fmt_in.audio.i_channels, &err);
    if (s == NULL)
    {
        msg_Err (obj, "cannot initialize resampler: %s", src_strerror (err));
        free (sys);
        return VLC_EGENERIC;
    }

    sys->state = s;
    sys->dirty = false; /* a fresh state holds nothing */

    static const struct vlc_filter_operations filter_ops =
    {
        .filter_audio = Resample, .close = Close,
    };
    filter->ops = &filter_ops;
    filter->p_sys = sys;

    return VLC_SUCCESS;
}

static void Close (filter_t *filter)
{
    filter_sys_t *sys = filter->p_sys;

    src_delete (sys->state);
    free (sys);
}

static block_t *Resample (filter_t *filter, block_t *in)
{
    block_t *out = NULL;
    const size_t framesize = filter->fmt_out.audio.i_bytes_per_frame;
    const unsigned irate = filter->fmt_in.audio.i_rate;
    const unsigned orate = filter->fmt_out.audio.i_rate;

    filter_sys_t *sys = filter->p_sys;
    SRC_STATE *s = sys->state;
    SRC_DATA src;

    if (irate == orate && !sys->dirty)
    {   /* Nothing to do: the ratio is exactly one and the resampler holds no
         * buffered frames. The audio output instantiates this filter for
         * drift correction alone, with equal rates, and only offsets the
         * input rate while it is actually correcting a drift. Running the
         * interpolation filter in the meantime would burn CPU and lowpass
         * the samples for nothing. */
        in->i_length = vlc_tick_from_samples(in->i_nb_samples, orate);
        return in;
    }

    src.src_ratio = (double)orate / (double)irate;
    /* If the ratio is back to one, this is the last pass: flush the frames
     * that the resampler still holds, rather than dropping them when the fast
     * path above takes over. */
    src.end_of_input = (irate == orate);

    int err = src_set_ratio (s, src.src_ratio);
    if (err != 0)
    {
        msg_Err (filter, "cannot update resampling ratio: %s",
                 src_strerror (err));
        goto error;
    }

    src.input_frames = in->i_nb_samples;
    src.output_frames = ceil (src.src_ratio * src.input_frames);
    if (src.end_of_input)
        src.output_frames += SRC_LOOKAHEAD; /* room for the flushed frames */

    out = block_Alloc (src.output_frames * framesize);
    if (unlikely(out == NULL))
        goto error;

    src.data_in = (float *)in->p_buffer;
    src.data_out = (float *)out->p_buffer;

    err = src_process (s, &src);

    if (src.end_of_input)
    {   /* A terminated state cannot be fed again as is. Resetting it also
         * keeps the invariant that the resampler holds nothing at all while
         * the fast path bypasses it, so that re-engaging it later cannot
         * replay frames buffered an arbitrarily long time ago. */
        src_reset (s);
        sys->dirty = false;
    }
    else
        sys->dirty = true;

    if (err != 0)
    {
        msg_Err (filter, "cannot resample: %s", src_strerror (err));
        block_Release (out);
        out = NULL;
        goto error;
    }

    if (src.input_frames_used < src.input_frames)
        msg_Err (filter, "lost %ld of %ld input frames",
                 src.input_frames - src.input_frames_used, src.input_frames);

    out->i_buffer = src.output_frames_gen * framesize;
    out->i_nb_samples = src.output_frames_gen;
    out->i_pts = in->i_pts;
    out->i_length = vlc_tick_from_samples(src.output_frames_gen, orate);
error:
    block_Release (in);
    return out;
}
