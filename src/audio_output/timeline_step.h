/*****************************************************************************
 * timeline_step.h: telling a step in the source timeline from device drift
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

#ifndef LIBVLC_AUDIO_OUTPUT_TIMELINE_STEP_H
#define LIBVLC_AUDIO_OUTPUT_TIMELINE_STEP_H 1

#include <vlc_common.h>
#include <vlc_aout.h>
#include <vlc_input.h>

/**
 * Where the content of a block ends on the timeline its dates are on.
 *
 * The length is the content's own, so it takes the playback rate to say how
 * much of the timeline the block occupies: without that scaling every block
 * at 2x reads as an overlap half its own length.
 *
 * \param i_pts the date the block carries
 * \param i_length how long its content lasts at nominal rate
 * \param i_input_rate the playback rate, INPUT_RATE_DEFAULT for nominal
 */
static inline vlc_tick_t aout_TimelineEnd( vlc_tick_t i_pts,
                                           vlc_tick_t i_length,
                                           int i_input_rate )
{
    return i_pts + i_length * i_input_rate / INPUT_RATE_DEFAULT;
}

/**
 * The step between where the last block said its content ended and where this
 * one says it begins, or zero if they join up.
 *
 * A step is a position error introduced upstream - a hole at a loop seam, a
 * dropped frame, a length the demuxer had to guess, a late wakeup - and it
 * says nothing about the device's clock. Genuine drift is what accumulates
 * between blocks that do join up; it never arrives all at once. A rate
 * mismatch is therefore a SLOPE across contiguous blocks and a fault upstream
 * is a JUMP at one boundary, and it takes a 4.3% rate error to move a 23 ms
 * block's date by a millisecond, two orders of magnitude beyond any real
 * device, so the slop below cannot swallow drift.
 *
 * The size is returned rather than a verdict because the two things done with
 * a step are ONE decision taken on the same test: it is kept out of the drift
 * controller, and it is named in the log with its direction and its size. A
 * difference small enough to be the container's own timebase is neither
 * excluded nor reported - reporting it would put a line in the log for every
 * block and make the report worth nothing.
 *
 * \param i_source_end where the last block's content ended,
 *        or VLC_TICK_INVALID if there was no last block
 * \param i_pts the date this block carries
 * \param b_discontinuity whether the stream has already been declared broken
 *        here, in which case the offset is known not to be drift and there is
 *        nothing left for this to add
 * \return the step, positive for a hole and negative for an overlap, or 0 if
 *         the source timeline joins up across this boundary
 */
static inline vlc_tick_t aout_TimelineStep( vlc_tick_t i_source_end,
                                            vlc_tick_t i_pts,
                                            bool b_discontinuity )
{
    if( i_source_end == VLC_TICK_INVALID || b_discontinuity )
        return 0;

    const vlc_tick_t i_step = i_pts - i_source_end;

    return ( i_step > +AOUT_MAX_TIMELINE_SLOP
          || i_step < -AOUT_MAX_TIMELINE_SLOP ) ? i_step : 0;
}

#endif
