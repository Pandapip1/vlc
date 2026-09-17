/*****************************************************************************
 * monitor.c : what the drift correction is doing, while it does it
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
 * The trace next door writes the same readings to a file, for a run that is
 * already over. Tuning the loop needs the other thing: what it is doing now,
 * while the gains are being moved. So the rows go to a ring as well, and an
 * interface reads them out of it whenever it gets round to it.
 *
 * Three things the audio output must not be made to do, and how each is
 * avoided here:
 *
 * - wait on an interface. The writer takes one uncontended mutex for the
 *   length of a structure copy. Nothing that can block is called under it,
 *   and the reader holds it only to copy out.
 * - pay for a reader that is not there. The ring exists only once somebody
 *   has asked for it, and the flag that says whether anybody is still looking
 *   is tested before the row is even built, exactly as the trace's is.
 * - outlive its reader, or be outlived by it. The ring is counted rather than
 *   owned by the output: whoever holds one keeps it, and an output that ends
 *   mid-read leaves the reader with a ring that has simply stopped filling.
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <stdlib.h>
#include <string.h>

#include <vlc_common.h>
#include <vlc_aout.h>

#include "aout_internal.h"

static void aout_MonitorDestroy (aout_drift_monitor_t *m)
{
    vlc_mutex_destroy (&m->lock);
    free (m);
}

/**
 * Attaches a monitor to an output, or takes another reference to the one
 * already attached, and hands it back.
 *
 * The caller must hold the output, which is what makes this safe: the ring is
 * released when the output object is, so it cannot be freed between the load
 * below and the reference this takes on it.
 */
aout_drift_monitor_t *aout_DriftMonitorHold (audio_output_t *aout)
{
    aout_owner_t *owner = aout_owner (aout);
    aout_drift_monitor_t *m =
        atomic_load_explicit (&owner->monitor, memory_order_acquire);

    if (m == NULL)
    {
        m = malloc (sizeof (*m));
        if (unlikely(m == NULL))
            return NULL;

        vlc_mutex_init (&m->lock);
        atomic_init (&m->armed, false);
        atomic_init (&m->refs, 2); /* the output's, and this caller's */
        m->written = 0;
        memset (&m->config, 0, sizeof (m->config));

        aout_drift_monitor_t *none = NULL;
        if (atomic_compare_exchange_strong (&owner->monitor, &none, m))
        {
            /* What the controller is running with was published before this
             * existed. The retune path is what publishes it, and re-reading
             * settings that have not moved costs the stream nothing. */
            atomic_store_explicit (&owner->retune, true, memory_order_relaxed);
            return m;
        }

        /* Another reader got there first; take theirs. */
        aout_MonitorDestroy (m);
        m = none;
    }

    atomic_fetch_add_explicit (&m->refs, 1, memory_order_relaxed);
    return m;
}

void aout_DriftMonitorRelease (aout_drift_monitor_t *m)
{
    if (atomic_fetch_sub_explicit (&m->refs, 1, memory_order_release) == 1)
    {
        atomic_thread_fence (memory_order_acquire);
        aout_MonitorDestroy (m);
    }
}

/**
 * Says whether anybody is still looking. Disarmed, the output pays a load and
 * a branch per row and builds nothing, which is what it pays for the trace it
 * was not asked to write.
 */
void aout_DriftMonitorArm (aout_drift_monitor_t *m, bool arm)
{
    atomic_store_explicit (&m->armed, arm, memory_order_relaxed);
}

/**
 * Copies out the points recorded since the sequence number the caller was
 * last left with, and updates it.
 *
 * A reader that has been away longer than the ring is deep has missed some.
 * It is told how many by the sequence number moving further than the number
 * of points returned, rather than by being handed a stale ring.
 */
size_t aout_DriftMonitorRead (aout_drift_monitor_t *m,
                              struct aout_drift_point *out, size_t count,
                              uint64_t *seq)
{
    vlc_mutex_lock (&m->lock);

    const uint64_t written = m->written;
    uint64_t first = *seq;

    if (first > written)
        first = written; /* A different stream's ring, or a reset counter */
    if (written - first > AOUT_MONITOR_POINTS)
        first = written - AOUT_MONITOR_POINTS;

    size_t n = written - first;
    if (n > count)
    {
        n = count;
        /* What is left stays for the next call: the caller asked for less
         * than there was, not to skip past it. */
    }

    for (size_t i = 0; i < n; i++)
        out[i] = m->ring[(first + i) % AOUT_MONITOR_POINTS];

    *seq = first + n;
    vlc_mutex_unlock (&m->lock);
    return n;
}

void aout_DriftMonitorConfig (aout_drift_monitor_t *m,
                              struct aout_drift_config *cfg)
{
    vlc_mutex_lock (&m->lock);
    *cfg = m->config;
    vlc_mutex_unlock (&m->lock);
}

/**
 * One point. Called on the audio thread with the row the trace is given, and
 * armed, both of which the caller has already established.
 */
void aout_MonitorRow (aout_owner_t *owner, aout_drift_monitor_t *m,
                      const struct aout_trace_row *row)
{
    vlc_mutex_lock (&m->lock);

    struct aout_drift_point *p =
        &m->ring[m->written % AOUT_MONITOR_POINTS];

    *p = (struct aout_drift_point){
        .date = mdate (),
        .drift = row->reading ? row->drift : 0,
        .delay = row->reading ? row->delay : 0,
        .extra = row->extra,
        .proportional = row->command ? row->p : 0.f,
        .integral = owner->sync.drift_integral,
        .commanded = row->command ? row->cmd : 0.f,
        .target = row->command ? row->tgt : 0.f,
        .detune = owner->sync.drift_detune,
        .event = row->event,
        .reading = row->reading,
        .command = row->command,
        .bound = owner->sync.drift_bound,
        .discontinuity = owner->sync.discontinuity,
        .latch = row->latch,
    };

    m->written++;
    vlc_mutex_unlock (&m->lock);
}

/**
 * Publishes what the controller about to run was built with. Whoever reads
 * the points needs these, or it cannot tell a command standing at its bound
 * from one that merely looks large - nor a bound of zero, which is not a
 * quiet run but one where every offset is spliced out instead.
 */
void aout_MonitorStream (audio_output_t *aout, float max)
{
    aout_owner_t *owner = aout_owner (aout);
    aout_drift_monitor_t *m =
        atomic_load_explicit (&owner->monitor, memory_order_acquire);

    if (m == NULL)
        return;

    vlc_mutex_lock (&m->lock);
    m->config = (struct aout_drift_config){
        .kp = owner->sync.drift_kp,
        .ki = owner->sync.drift_ki,
        .slew = owner->sync.drift_slew,
        .max_cents = max,
        .rate = owner->mixer_format.i_rate,
        .src_rate = owner->input_format.i_rate,
        .running = true,
    };
    vlc_mutex_unlock (&m->lock);
}

void aout_MonitorStop (audio_output_t *aout)
{
    aout_owner_t *owner = aout_owner (aout);
    aout_drift_monitor_t *m =
        atomic_load_explicit (&owner->monitor, memory_order_acquire);

    if (m == NULL)
        return;

    vlc_mutex_lock (&m->lock);
    m->config.running = false;
    vlc_mutex_unlock (&m->lock);
}

void aout_MonitorInit (audio_output_t *aout)
{
    atomic_init (&aout_owner (aout)->monitor, NULL);
}

/**
 * Drops the output's own reference. Done where the object is released rather
 * than where it is stopped, so that holding the output is enough to make
 * taking a reference to its monitor safe.
 */
void aout_MonitorClose (audio_output_t *aout)
{
    aout_drift_monitor_t *m = atomic_load (&aout_owner (aout)->monitor);

    if (m != NULL)
        aout_DriftMonitorRelease (m);
}
