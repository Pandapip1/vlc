/*****************************************************************************
 * timeline_step.c: test that a seam is told apart from device drift
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

/* The output measures (now + delay) - pts and answers it by changing the
 * playback RATE, which is the right tool for a device whose clock differs
 * from the system's and the wrong one for a position error made upstream.
 * With the shipped gain the proportional term alone pins the correction at
 * its nine cent bound at 60 ms of standing offset, while the paths that fix a
 * position - jumping ahead, inserting silence - do not fire until +180 ms and
 * -120 ms. So any standing offset between those pins the corrector at full
 * authority, and nine cents closes only 5.2 ms a second: a 60 ms seam hole
 * takes 11.5 s to work off and is heard as detuning the whole way.
 *
 * What keeps a seam out of that is this one decision, taken on the dates the
 * decoder gives its blocks: does the source timeline join up across this
 * boundary. Everything the test knows about real containers is a measurement
 * from this tree, and the two that matter sit either side of the threshold
 * with a factor of seven between them. */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <vlc_common.h>

#include "../../../src/audio_output/timeline_step.h"

#include <stdio.h>

static int fail;

/* A block boundary: the last block's content ended at i_source_end and this
 * one is dated i_pts. What comes back is the step, which is at once what is
 * kept out of the drift controller and what is named in the log - one
 * decision, so a test of the classification is a test of the reporting. */
static void expect( const char *psz_what, vlc_tick_t i_source_end,
                    vlc_tick_t i_pts, vlc_tick_t i_expected )
{
    vlc_tick_t i_got = aout_TimelineStep( i_source_end, i_pts, false );

    if( i_got != i_expected )
    {
        fprintf( stderr, "%s: step %"PRId64", expected %"PRId64"\n",
                 psz_what, i_got, i_expected );
        fail = 1;
    }

    /* Nothing is left to add once the stream has been declared broken here:
     * the offset either side of a flagged discontinuity is already known not
     * to be drift. */
    i_got = aout_TimelineStep( i_source_end, i_pts, true );
    if( i_got != 0 )
    {
        fprintf( stderr, "%s: step %"PRId64" across a flagged discontinuity, "
                 "expected none\n", psz_what, i_got );
        fail = 1;
    }
}

/* Contiguous means no step, and no step means no report: a line in the log
 * for a boundary that joined up would put one there for every block. */
static void expect_drift( const char *psz_what, vlc_tick_t i_source_end,
                          vlc_tick_t i_pts )
{
    expect( psz_what, i_source_end, i_pts, 0 );
}

static void expect_end( const char *psz_what, vlc_tick_t i_pts,
                        vlc_tick_t i_length, int i_input_rate,
                        vlc_tick_t i_expected )
{
    vlc_tick_t i_got = aout_TimelineEnd( i_pts, i_length, i_input_rate );

    if( i_got != i_expected )
    {
        fprintf( stderr, "%s: content ends at %"PRId64", expected %"PRId64"\n",
                 psz_what, i_got, i_expected );
        fail = 1;
    }
}

int main( void )
{
    /* Nothing to compare against: the first block of a stream, and the first
     * one after a flush, cannot be a step. */
    expect( "no previous block", VLC_TICK_INVALID, 1000000, 0 );

    /* DRIFT. Containers date their blocks on a timebase of their own - a
     * millisecond in Matroska, AVI and ASF - so consecutive dates can be
     * rounded apart with the content still contiguous. Over twelve containers
     * the largest such step measured in this tree is 825 us, in matroska.
     * Calling that a seam would name a hole in every file that rounds. */
    expect_drift( "matroska rounding", 1000000, 1000825 );
    expect_drift( "matroska rounding, the other way", 1000000, 999175 );

    /* STEP. The smallest genuine hole measured over the same twelve is
     * 6.1 ms, seven times the largest rounding, so the threshold has a factor
     * of seven to sit in and both sides of it are pinned here. */
    expect( "smallest genuine hole", 1000000, 1006100, 6100 );

    /* The threshold itself, either side. */
    expect_drift( "exactly the slop", 1000000, 1000000 + AOUT_MAX_TIMELINE_SLOP );
    expect( "one past the slop", 1000000,
            1000000 + AOUT_MAX_TIMELINE_SLOP + 1, AOUT_MAX_TIMELINE_SLOP + 1 );
    expect_drift( "exactly the slop, overlapping",
                  1000000, 1000000 - AOUT_MAX_TIMELINE_SLOP );
    expect( "one past the slop, overlapping", 1000000,
            1000000 - AOUT_MAX_TIMELINE_SLOP - 1,
            -AOUT_MAX_TIMELINE_SLOP - 1 );

    /* The seams this series was written for, at the size they were measured.
     * The avi one belongs to the mp3 elementary stream rather than to the
     * container - the raw .mp3 of the same content hands over 78368 us - and
     * it is re-injected at every loop, which is what pins the corrector for
     * ever: nine cents would need 15 s to work off one of them. */
    expect( "avi / mp3 loop seam", 1000000, 1078367, 78367 );
    expect( "ts seam", 1000000, 1116118, 116118 );

    /* An overlap is a step too, and its sign has to survive: the next block
     * is dated before the last one's content ended, so what is handed over is
     * to be placed back, not forward. */
    expect( "overlap", 1000000, 995000, -5000 );
    expect( "overlap, a whole block", 1000000, 976780, -23220 );

    /* NOT A STEP: a rate mismatch. A device off nominal never moves a date;
     * it changes when the samples are heard, and what the output measures
     * grows a little on every block that joins up. Here is that shape at its
     * most extreme - a source losing a whole millisecond of every 23 ms block,
     * a 4.3% error, two orders of magnitude beyond any real device - and not
     * one boundary of it is a step. It reaches the controller as before,
     * which is right, and it is why the slop cannot swallow drift. */
    vlc_tick_t i_end = 1000000, i_slope = 0;
    for( unsigned i = 0; i < 100; i++ )
    {
        vlc_tick_t i_pts = i_end + 1000;

        expect_drift( "gradual rate error", i_end, i_pts );
        i_slope += 1000;
        i_end = aout_TimelineEnd( i_pts, 23220, INPUT_RATE_DEFAULT );
    }
    if( i_slope != 100000 )
    {
        fprintf( stderr, "the slope reached %"PRId64" us, expected 100000\n",
                 i_slope );
        fail = 1;
    }

    /* Where a block's content ends. The length is the content's own, so at
     * anything but nominal rate it takes the playback rate to say how much of
     * the timeline the block occupies. Without that every block at 2x read as
     * an overlap half its own length and every block at 0.5x as a hole as
     * long as itself - a real defect, found in this code and fixed. */
    expect_end( "nominal", 1000000, 23220, INPUT_RATE_DEFAULT, 1023220 );
    expect_end( "2x", 1000000, 23220, INPUT_RATE_DEFAULT / 2, 1011610 );
    expect_end( "0.5x", 1000000, 23220, INPUT_RATE_DEFAULT * 2, 1046440 );

    /* and therefore, at 2x, a stream that joins up still joins up. */
    expect_drift( "2x, contiguous",
                  aout_TimelineEnd( 1000000, 23220, INPUT_RATE_DEFAULT / 2 ),
                  1011610 );

    return fail;
}
