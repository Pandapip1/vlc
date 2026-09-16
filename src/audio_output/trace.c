/*****************************************************************************
 * trace.c : what the drift correction is doing, written down
 *****************************************************************************
 * Copyright (C) 2026 VLC authors and VideoLAN
 * $Id$
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

/**
 * The drift correction is a feedback loop whose whole state lives for one
 * call and is then gone: a reading is taken, a command is computed, a bound
 * and a slew stand between that command and what is applied, and none of it
 * is visible from outside. Faults in it have had to be reconstructed after
 * the fact from what the warnings happened to mention.
 *
 * Set "aout-drift-trace" and every reading is written down instead, as one
 * CSV row: what the device said it held, what the drift came to, what the
 * controller asked for, what the bound and the slew let through, and the
 * steps, silences, jumps and run-outs in between. Leave it unset and this
 * file costs one predictable branch per block and writes nothing.
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

#include <vlc_common.h>
#include <vlc_aout.h>
#include <vlc_fs.h>

#include "aout_internal.h"

/* Big enough that the audio thread reaches the write() behind it about once a
 * minute at the rate blocks arrive, rather than every ninety milliseconds. */
#define AOUT_TRACE_BUFFER (256 * 1024)

/**
 * Opens the trace for the life of the output, not of one stream: an output is
 * kept across the items of a playlist, and a file per item would leave only
 * the last of them.
 */
void aout_TraceOpen (audio_output_t *aout)
{
    aout_owner_t *owner = aout_owner (aout);

    char *path = var_InheritString (aout, "aout-drift-trace");

    owner->trace = NULL;
    if (path == NULL)
        return;

    FILE *f = vlc_fopen (path, "we");

    if (f == NULL)
    {
        msg_Err (aout, "cannot write the drift trace to %s: %s", path,
                 vlc_strerror_c (errno));
        free (path);
        return;
    }
    free (path);

    /* Fully buffered and large: the point of the trace is to leave the thing
     * it measures alone, and an unbuffered write per block would not. */
    setvbuf (f, NULL, _IOFBF, AOUT_TRACE_BUFFER);

    fprintf (f, "# vlc-aout-drift-trace 1\n");
    fprintf (f, "# jump_us=%"PRId64" silence_us=%"PRId64" slop_us=%"PRId64"\n",
             (vlc_tick_t)(3 * AOUT_MAX_PTS_DELAY),
             (vlc_tick_t)(-3 * AOUT_MAX_PTS_ADVANCE),
             (vlc_tick_t)AOUT_MAX_TIMELINE_SLOP);
    fprintf (f, "t_us,event,drift_us,delay_us,p_cents,i_cents,cmd_cents,"
                "tgt_cents,detune_cents,bound,extra_us,codec,es\n");

    owner->trace = f;
}

/**
 * Says what the controller about to run was built with. Whoever reads the
 * trace needs these, or it cannot tell a command standing at its bound from
 * one that merely looks large.
 */
void aout_TraceStream (audio_output_t *aout, unsigned rate, float max)
{
    aout_owner_t *owner = aout_owner (aout);

    if (owner->trace == NULL)
        return;

    fprintf (owner->trace, "# epoch_us=%"PRId64" rate=%u\n", mdate (), rate);
    fprintf (owner->trace, "# kp=%.4f ki=%.4f slew=%.4f max_cents=%.4f\n",
             owner->sync.drift_kp, owner->sync.drift_ki,
             owner->sync.drift_slew, max);

    aout_Trace (owner, .event = "start");
}

void aout_TraceClose (audio_output_t *aout)
{
    aout_owner_t *owner = aout_owner (aout);

    if (owner->trace == NULL)
        return;

    fclose (owner->trace);
    owner->trace = NULL;
}

/**
 * One row. Whatever was not measured is left empty rather than written as a
 * zero, so that the renderer can tell "no reading" from "a reading of none".
 *
 * The integral, the applied detune and the bound flag are always known, so
 * they are on every row: that is what makes a windup legible, since the
 * question at the bound is whether the integral is standing still or still
 * climbing while nothing more comes out.
 */
void aout_TraceRow (aout_owner_t *owner, const struct aout_trace_row *row)
{
    FILE *f = owner->trace;

    fprintf (f, "%"PRId64",%s,", mdate (), row->event);

    if (row->reading)
        fprintf (f, "%"PRId64",%"PRId64",", row->drift, row->delay);
    else
        fprintf (f, ",,");

    if (row->command)
        fprintf (f, "%.4f,", row->p);
    else
        fprintf (f, ",");

    fprintf (f, "%.4f,", owner->sync.drift_integral);

    if (row->command)
        fprintf (f, "%.4f,%.4f,", row->cmd, row->tgt);
    else
        fprintf (f, ",,");

    fprintf (f, "%.4f,%d,", owner->sync.drift_detune,
             owner->sync.drift_bound ? 1 : 0);

    if (row->step)
        fprintf (f, "%"PRId64",%4.4s,%d\n", row->extra,
                 (const char *)&row->codec, row->es);
    else if (row->extra != 0)
        fprintf (f, "%"PRId64",,\n", row->extra);
    else
        fprintf (f, ",,\n");
}

/**
 * An event the output module is in a position to report and the core is not -
 * a device running dry, so far.
 *
 * Called from the output's own entry points, where the core already holds the
 * lock that serialises everything else written here.
 */
void aout_TraceEvent (audio_output_t *aout, const char *event, int64_t value)
{
    aout_owner_t *owner = aout_owner (aout);

    aout_Trace (owner, .event = event, .extra = value);
}
