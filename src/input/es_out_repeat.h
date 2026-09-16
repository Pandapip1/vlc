/*****************************************************************************
 * es_out_repeat.h: where a repeated pass resumes on the timeline
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

#ifndef LIBVLC_INPUT_ES_OUT_REPEAT_H
#define LIBVLC_INPUT_ES_OUT_REPEAT_H 1

#include <vlc_common.h>
#include <vlc_tick.h>

/**
 * Where a repeated pass should carry on from.
 *
 * Repeating an item in place seeks the demuxer back to the start and keeps the
 * clock, so the new pass has to be placed where the last one stopped. How far
 * the data already handed over reaches says where that is.
 *
 * A cadence step past the last pcr only stands in for it when there is no such
 * end to take: it is where the pass ended only when the cadence happens to
 * divide the item, and mp4 reading a pcr every 250 ms was a hole of exactly
 * that at every loop.
 *
 * The end is not bounded above by the cadence. A container carrying its
 * timestamps in cluster or packet headers emits the pcr before the blocks it
 * covers have all been handed over, so an end further out than a step past it
 * is ordinary rather than the mark of a demuxer running ahead - matroska ends
 * 48 ms past a pcr it steps 23 ms at a time. A demuxer whose pcr genuinely
 * outruns its data is held back in the demuxer.
 *
 * It is bounded below by the pcr, which is ground already covered.
 *
 * \param i_last_end how far the data handed over reaches, or VLC_TICK_INVALID
 * \param i_last_pcr the last pcr of the pass that is ending
 * \param i_pcr_step the interval this demuxer reads pcrs at
 */
static inline vlc_tick_t es_out_RepeatResume( vlc_tick_t i_last_end,
                                              vlc_tick_t i_last_pcr,
                                              vlc_tick_t i_pcr_step )
{
    vlc_tick_t i_resume = i_last_end;

    if( i_resume <= VLC_TICK_INVALID )
        i_resume = i_last_pcr + i_pcr_step;
    if( i_resume < i_last_pcr )
        i_resume = i_last_pcr;

    return i_resume;
}

#endif
