/*****************************************************************************
 * es_out_repeat.c: test where a repeated pass resumes
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

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <vlc_common.h>

#include "../../../src/input/es_out_repeat.h"

#include <stdio.h>

static int fail;

static void expect( const char *psz_what, vlc_tick_t i_last_end,
                    vlc_tick_t i_last_pcr, vlc_tick_t i_pcr_step,
                    vlc_tick_t i_expected )
{
    vlc_tick_t i_got = es_out_RepeatResume( i_last_end, i_last_pcr,
                                            i_pcr_step );
    if( i_got != i_expected )
    {
        fprintf( stderr, "%s: resumed at %"PRId64", expected %"PRId64"\n",
                 psz_what, i_got, i_expected );
        fail = 1;
    }
}

int main( void )
{
    /* The shapes below are what these demuxers actually report at the end of
     * a 4 s item, so that a change in the rule is seen here rather than as
     * detuning after several loops. */

    /* Matroska ends 48 ms past its last pcr and steps 23 ms at a time. The
     * end is what counts: bounding it by the cadence used to leave 25 ms of
     * the item unplayed at every loop, and the drift that left accumulated
     * until the output's correction saturated. */
    expect( "matroska", 4011001, 3963001, 23000, 4011001 );

    /* ts and mpeg-ps keep their pcr inside the data, so the end is already
     * within a cadence step and nothing is clamped either way. */
    expect( "ts", 9712779, 9689559, 23220, 9712779 );
    expect( "mpeg-ps", 79569606656, 79569583436, 23220, 79569606656 );

    /* wav dates a block without saying how long it is, so the end lands
     * exactly one step past the pcr. */
    expect( "wav", 4000002, 3950002, 50000, 4000002 );

    /* No end to take: the cadence stands in for it. */
    expect( "no end", VLC_TICK_INVALID, 3950002, 50000, 4000002 );

    /* An end behind the pcr is ground already covered; the pcr holds. */
    expect( "end behind pcr", 3900000, 3950002, 50000, 3950002 );

    /* An end exactly on the pcr is not moved. */
    expect( "end on pcr", 3950002, 3950002, 50000, 3950002 );

    return fail;
}
