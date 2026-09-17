/*****************************************************************************
 * drift.c: what the drift correction does, and does not, answer
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

/* The subject here is the drift correction, not any container. Both halves of
 * what it sees are simulated: the source hands its blocks to imem, so their
 * dates are exactly what this file says they are and a seam can be put at one
 * block boundary with nothing else disturbed, and the device is the dummy
 * output, whose clock runs as many parts per million away from the system's
 * as it is asked to. Neither end needs a file, a codec or hardware, and
 * neither can drift on its own.
 *
 * Six things the controller has to do, and this asserts all six, because
 * each one alone is satisfied by a controller that is wrong in the others:
 *
 *   a step in the source timeline    leaves the command where it was, and is
 *                                    named in the log with its size and sign
 *   a rate error it can answer       moves the command to match it, quietly
 *   a rate error beyond its bound    pins at the bound and says so, once, and
 *                                    stays there while the error is there
 *   a device far too fast            pins, then inserts silence, and plays on
 *   a device far too slow            pins, then jumps ahead, and plays on
 *   the offset a flush leaves        is put right where it happened, and
 *                                    never reaches the command
 *
 * "Leaves the command where it was" is the property, not "no saturation":
 * saturation is what a chased seam looks like after seconds of chasing, while
 * the command moving at all is the fault itself and shows up at once.
 *
 * This is a real-time test. It plays several streams end to end and each one
 * takes as long as its content lasts, so it is deliberately NOT in the
 * default suite - see test/Makefile.am, where it is an EXTRA_PROGRAM reached
 * by `make checkall` or `make check-aout-drift`. The margins below are wide
 * enough that a loaded machine does not fail it, and everything asserted is
 * either a count of log lines or a settled value, never a duration. */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <assert.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <unistd.h>

#include <vlc/vlc.h>

/* The source. One block is 1152 frames at 48 kHz, which is 24000 us exactly,
 * so every date below is a whole number of microseconds and a seam is the
 * size it is asked for rather than the size rounding left. */
#define RATE        48000u
#define FRAMES      1152u
#define BLOCK_US    ((int64_t)FRAMES * 1000000 / RATE)

struct source
{
    unsigned n;          /* blocks handed over so far */
    unsigned count;      /* how many to hand over in all */
    unsigned seam_block; /* the boundary the step is at, 0 for none */
    int64_t  seam_us;    /* the step, positive for a hole */
    int16_t  buffer[FRAMES * 2];
};

static int SourceGet( void *data, const char *cookie, int64_t *dts,
                      int64_t *pts, unsigned *flags, size_t *size,
                      void **output )
{
    struct source *src = data;

    (void) cookie; (void) flags;

    if( src->n >= src->count )
        return 1; /* end of stream */

    int64_t date = (int64_t)src->n * BLOCK_US;

    /* Everything from the seam on is moved bodily: a hole is what a loop
     * seam, a dropped frame or a guessed length leaves behind, and what
     * follows it carries on contiguously from where it lands. */
    if( src->seam_block != 0 && src->n >= src->seam_block )
        date += src->seam_us;

    *dts = date;
    *pts = date;
    *size = sizeof (src->buffer);
    *output = src->buffer;
    src->n++;
    return 0;
}

static void SourceRelease( void *data, const char *cookie, size_t size,
                           void *output )
{
    (void) data; (void) cookie; (void) size; (void) output;
}

/* What a run is asked to do and what came of it. */
struct run
{
    const char *name;

    /* asked */
    unsigned    seconds;
    unsigned    seam_block;
    int64_t     seam_us;
    int         drift_ppm;
    unsigned    latency_ms;
    const char *path;          /* a file to play, instead of the source above */
    unsigned    seek_at_ms;    /* where in it to ask for a seek, 0 for none */
    unsigned    seek_to_ms;    /* and where the seek is to */
    int         desync_ms;     /* audio delay, to set the offset a flush leaves */

    /* seen */
    unsigned    holes;         /* "handed over a hole of" */
    unsigned    overlaps;      /* "handed over an overlap of" */
    int64_t     hole_us;       /* the last one's size */
    unsigned    bound_hit;     /* "drift correction at its limit" */
    unsigned    bound_left;    /* "back within its limit" */
    unsigned    silences;      /* "playing silence" */
    unsigned    jumps;         /* "jumping ahead" */
    int64_t     jump_us;       /* the first one's size */
    unsigned    jump_reading;  /* readings taken before it */
    unsigned    readings;      /* how many times the command was reported */
    unsigned    marked;        /* readings taken before the step or the seek */
    double      cents[512];    /* the command, once a second */
    uint64_t    played;        /* samples the device was handed */
};

static double last_cents( const struct run *r )
{
    return r->readings > 0 ? r->cents[r->readings - 1] : 0.;
}

static struct run *current;

static void Logged( void *data, int level, const libvlc_log_t *ctx,
                    const char *fmt, va_list args )
{
    char line[512];
    double cents;
    long long v;

    (void) data; (void) level; (void) ctx;

    if( vsnprintf( line, sizeof (line), fmt, args ) < 0 )
        return;

    const char *p;

    if( (p = strstr( line, "handed over a hole of " )) != NULL
     && sscanf( p, "handed over a hole of %lld us", &v ) == 1 )
    {
        current->holes++;
        current->hole_us = v;
        current->marked = current->readings;
    }
    else if( (p = strstr( line, "handed over an overlap of " )) != NULL
          && sscanf( p, "handed over an overlap of %lld us", &v ) == 1 )
    {
        current->overlaps++;
        current->hole_us = -v;
        current->marked = current->readings;
    }
    else if( sscanf( line, "drift %lld us, detuning %lf cents", &v, &cents )
             == 2 )
    {
        /* Kept in order rather than reduced, so that what the command did
         * either side of the seam can be asked for by position in the log
         * and nothing depends on when a reading happened to be taken. */
        if( current->readings < sizeof (current->cents)
                                / sizeof (current->cents[0]) )
            current->cents[current->readings++] = cents;
    }
    else if( strstr( line, "drift correction at its limit" ) != NULL )
        current->bound_hit++;
    else if( strstr( line, "back within its limit" ) != NULL )
        current->bound_left++;
    else if( strstr( line, "playing silence" ) != NULL )
        current->silences++;
    else if( (p = strstr( line, "too late (" )) != NULL
          && strstr( line, "jumping ahead" ) != NULL
          && sscanf( p, "too late (%lld)", &v ) == 1 )
    {
        if( current->jumps++ == 0 )
        {
            current->jump_us = v;
            current->jump_reading = current->readings;
        }
    }
    else if( sscanf( line, "played %lld samples", &v ) == 1 )
        current->played = v;
}

static void put16( uint8_t *p, uint16_t v )
{
    p[0] = v; p[1] = v >> 8;
}

static void put32( uint8_t *p, uint32_t v )
{
    put16( p, v ); put16( p + 2, v >> 16 );
}

/* Something to seek in. A seek needs a demuxer and a file, which imem is
 * neither, and what the file holds does not matter to the drift correction -
 * only that it is raw PCM, so that a seek lands where it was asked with no
 * codec delay between the two. */
static void WriteFile( const char *psz_path, unsigned seconds )
{
    const uint32_t frames = seconds * RATE, data = frames * 4;
    uint8_t hdr[44];
    FILE *f = fopen( psz_path, "wb" );

    assert( f != NULL );
    memcpy( hdr, "RIFF", 4 );
    put32( hdr + 4, 36 + data );
    memcpy( hdr + 8, "WAVEfmt ", 8 );
    put32( hdr + 16, 16 );          /* fmt chunk length */
    put16( hdr + 20, 1 );           /* PCM */
    put16( hdr + 22, 2 );           /* channels */
    put32( hdr + 24, RATE );
    put32( hdr + 28, RATE * 4 );    /* bytes per second */
    put16( hdr + 32, 4 );           /* bytes per frame */
    put16( hdr + 34, 16 );          /* bits per sample */
    memcpy( hdr + 36, "data", 4 );
    put32( hdr + 40, data );
    size_t got = fwrite( hdr, 1, sizeof (hdr), f );

    assert( got == sizeof (hdr) );

    for( uint32_t n = 0; n < frames; n++ )
    {
        const int16_t v = (int16_t)(uint16_t)( n * 71 );
        const int16_t frame[2] = { v, v };

        got = fwrite( frame, 1, sizeof (frame), f );
        assert( got == sizeof (frame) );
    }
    fclose( f );
}

static void Play( struct run *r )
{
    struct source src =
    {
        .n = 0,
        .count = (unsigned)( (int64_t)r->seconds * 1000000 / BLOCK_US ),
        .seam_block = r->seam_block,
        .seam_us = r->seam_us,
    };
    char opt[8][64];
    const char *argv[24];
    unsigned argc = 0;

    memset( src.buffer, 0, sizeof (src.buffer) );

    r->holes = r->overlaps = r->bound_hit = r->bound_left = 0;
    r->silences = r->jumps = r->readings = r->marked = 0;
    r->jump_us = 0;
    r->jump_reading = 0;
    r->hole_us = 0;
    r->played = 0;

    snprintf( opt[0], sizeof (opt[0]), "--imem-get=%" PRIuPTR,
              (uintptr_t)(void *) SourceGet );
    snprintf( opt[1], sizeof (opt[1]), "--imem-release=%" PRIuPTR,
              (uintptr_t)(void *) SourceRelease );
    snprintf( opt[2], sizeof (opt[2]), "--imem-data=%" PRIuPTR,
              (uintptr_t) &src );
    snprintf( opt[3], sizeof (opt[3]), "--imem-samplerate=%u", RATE );
    snprintf( opt[4], sizeof (opt[4]), "--adummy-latency=%u", r->latency_ms );
    snprintf( opt[5], sizeof (opt[5]), "--adummy-drift=%d", r->drift_ppm );
    snprintf( opt[6], sizeof (opt[6]), "--audio-desync=%d", r->desync_ms );

    argv[argc++] = "-vv";
    argv[argc++] = "--no-video";
    argv[argc++] = "--intf=dummy";
    argv[argc++] = "--aout=dummy";
    argv[argc++] = "--imem-cat=1";
    argv[argc++] = "--imem-codec=s16l";
    argv[argc++] = "--imem-channels=2";
    argv[argc++] = "--imem-id=1";
    for( unsigned i = 0; i < 7; i++ )
        argv[argc++] = opt[i];
    assert( argc <= sizeof (argv) / sizeof (argv[0]) );

    libvlc_instance_t *vlc = libvlc_new( argc, argv );
    assert( vlc != NULL );

    current = r;
    libvlc_log_set( vlc, Logged, NULL );

    libvlc_media_t *md = ( r->path != NULL )
                         ? libvlc_media_new_path( vlc, r->path )
                         : libvlc_media_new_location( vlc, "imem://" );
    assert( md != NULL );

    libvlc_media_player_t *mp = libvlc_media_player_new_from_media( md );
    assert( mp != NULL );

    libvlc_media_player_play( mp );

    /* Until the source has handed over everything and the output has drained
     * it, with a bound so that a hang is a failure rather than a wait. */
    bool sought = false;

    for( unsigned i = 0; i < r->seconds * 40 + 400; i++ )
    {
        libvlc_state_t st = libvlc_media_player_get_state( mp );

        if( st == libvlc_Ended || st == libvlc_Error )
            break;

        if( r->seek_at_ms != 0 && !sought
         && libvlc_media_player_get_time( mp ) >= (libvlc_time_t)r->seek_at_ms )
        {
            /* The reading before the seek is what the ones after it are
             * measured against, so the mark is taken here rather than off a
             * log line the seek does not produce. */
            r->marked = r->readings;
            libvlc_media_player_set_time( mp, r->seek_to_ms );
            sought = true;
        }
        usleep( 50000 );
    }

    libvlc_media_player_stop( mp );
    libvlc_media_player_release( mp );
    libvlc_media_release( md );
    libvlc_log_unset( vlc );
    libvlc_release( vlc );
    current = NULL;
}

static int fail;

static void complain( const struct run *r, const char *what, ... )
{
    va_list ap;

    fprintf( stderr, "%s: ", r->name );
    va_start( ap, what );
    vfprintf( stderr, what, ap );
    va_end( ap );
    fputc( '\n', stderr );
    fail = 1;
}

static void report( const struct run *r )
{
    fprintf( stderr,
             "%-22s holes %u (%"PRId64" us) overlaps %u  bound %u/%u  "
             "silence %u jump %u (%"PRId64" us at %u)  played %"PRIu64
             "\n  cents:",
             r->name, r->holes, r->hole_us, r->overlaps, r->bound_hit,
             r->bound_left, r->silences, r->jumps, r->jump_us,
             r->jump_reading, r->played );

    for( unsigned i = 0; i < r->readings; i++ )
        fprintf( stderr, "%s %+.3f",
                 ( r->marked != 0 && i == r->marked ) ? " |" : "",
                 r->cents[i] );
    fputc( '\n', stderr );
}

/* The command must not move for a position error - a step in the source, or
 * a seek - so the reading taken just before that happened is the baseline and
 * every reading after it is measured against that one. Taking it from the log
 * rather than from the clock is what makes this independent of when a reading
 * happened to fall. */
static void unmoved( const struct run *r, double tolerance )
{
    if( r->marked < 2 || r->marked >= r->readings )
    {
        complain( r, "the mark fell at reading %u of %u, which leaves "
                  "nothing to compare either side of it",
                  r->marked, r->readings );
        return;
    }

    const double base = r->cents[r->marked - 1];
    double worst = 0.;

    for( unsigned i = r->marked; i < r->readings; i++ )
    {
        double d = r->cents[i] - base;

        if( d < 0 )
            d = -d;
        if( d > worst )
            worst = d;
    }

    if( worst > tolerance )
        complain( r, "the command moved %+.3f cents from %+.3f for a position "
                  "error, which is not evidence about the device's clock",
                  worst, base );
}

int main( void )
{
    /* Where this build's modules are, as a fresh tree has none anywhere else,
     * and a bound on the whole run so that a hang is a failure. */
    setenv( "VLC_PLUGIN_PATH", "../modules", 0 );
    alarm( 400 );

    /* 1. A step in the source timeline. The device is nominal and holds
     * 50 ms, and one block boundary hands over a hole of 78367 us - the size
     * the mp3 stream inside an avi leaves at every loop seam, which is what
     * pinned the corrector for ever before steps were told apart from drift.
     * The command must not move for it, and the hole must be named. */
    struct run step =
    {
        .name = "step: 78367 us hole", .seconds = 13, .latency_ms = 50,
        .seam_block = 375 /* 9 s in */, .seam_us = 78367,
    };
    Play( &step );
    report( &step );

    if( step.holes != 1 || step.hole_us != 78367 )
        complain( &step, "handed over %u hole(s) of %"PRId64" us, "
                  "expected 1 of 78367", step.holes, step.hole_us );
    if( step.bound_hit != 0 )
        complain( &step, "the correction hit its bound %u time(s)",
                  step.bound_hit );
    unmoved( &step, 2.0 );

    /* An overlap is the same fault the other way and has to be named as one. */
    struct run overlap =
    {
        .name = "step: 78367 us overlap", .seconds = 13, .latency_ms = 50,
        .seam_block = 375, .seam_us = -78367,
    };
    Play( &overlap );
    report( &overlap );

    if( overlap.overlaps != 1 || overlap.hole_us != -78367 )
        complain( &overlap, "handed over %u overlap(s) of %"PRId64" us, "
                  "expected 1 of 78367", overlap.overlaps, -overlap.hole_us );
    if( overlap.bound_hit != 0 )
        complain( &overlap, "the correction hit its bound %u time(s)",
                  overlap.bound_hit );
    unmoved( &overlap, 2.0 );

    /* 2. A rate error the bound can answer. Nine cents is 2^(9/1200) - 1, or
     * 5212 ppm, so 2000 ppm is well inside it: the command must move to match
     * the device and nothing is flagged, because resampling is the right
     * answer to a device that really is off nominal. Without this case
     * "the command does not move" would be satisfied by a controller that
     * never does anything at all. */
    struct run inside =
    {
        .name = "2000 ppm", .seconds = 16, .latency_ms = 50,
        .drift_ppm = 2000,
    };
    Play( &inside );
    report( &inside );

    if( inside.holes != 0 || inside.overlaps != 0 )
        complain( &inside, "a device clock error was reported as %u step(s) "
                  "in the source timeline", inside.holes + inside.overlaps );
    if( inside.bound_hit != 0 )
        complain( &inside, "an error the bound can answer was flagged %u "
                  "time(s)", inside.bound_hit );
    if( last_cents( &inside ) > -2.5 || last_cents( &inside ) < -5.0 )
        complain( &inside, "settled at %+.3f cents, expected about -3.7",
                  last_cents( &inside ) );

    /* 3. A rate error beyond the bound, both ways. It must pin, say so once,
     * and stay pinned: while an error larger than resampling can answer is
     * still there, the bound is the honest end state and unpinning would be
     * a refusal to correct as hard as it can. The two ends should differ in
     * nothing but sign. */
    static const int beyond[] = { 8000, -8000 };
    static const char *const names[] = { "8000 ppm", "-8000 ppm" };
    double pinned_at[2];

    for( unsigned i = 0; i < 2; i++ )
    {
        struct run r =
        {
            .name = names[i], .seconds = 20, .latency_ms = 50,
            .drift_ppm = beyond[i],
        };

        Play( &r );
        report( &r );
        pinned_at[i] = last_cents( &r );

        if( r.bound_hit != 1 )
            complain( &r, "flagged the bound %u time(s), expected once: a "
                      "burst of them is the correction flapping around it",
                      r.bound_hit );
        if( r.bound_left != 0 )
            complain( &r, "left the bound %u time(s) while the error was "
                      "still there", r.bound_left );
        if( r.holes != 0 || r.overlaps != 0 )
            complain( &r, "a device clock error was reported as %u step(s) "
                      "in the source timeline", r.holes + r.overlaps );

        /* A device that runs fast drains what it holds sooner, reports less
         * delay and so is heard early, and the answer to that is to feed it
         * faster - a negative command. Positive ppm therefore pins at -9. */
        const double sign = beyond[i] > 0 ? -1.0 : +1.0;

        if( pinned_at[i] * sign < 8.82 )
            complain( &r, "pinned at %+.3f cents, expected %+.1f",
                      pinned_at[i], 9.0 * sign );

        /* and stayed there. The command is slewed towards the bound rather
         * than jumped to it, so what is asserted is that once it has arrived
         * it does not come back while the error is still there. */
        unsigned arrived = r.readings;

        for( unsigned k = 0; k < r.readings; k++ )
            if( r.cents[k] * sign >= 8.9 )
            {
                arrived = k;
                break;
            }

        if( arrived + 3 > r.readings )
            complain( &r, "reached the bound at reading %u of %u, too late "
                      "to say whether it stays there", arrived, r.readings );
        else
            for( unsigned k = arrived; k < r.readings; k++ )
                if( r.cents[k] * sign < 8.5 )
                {
                    complain( &r, "came back to %+.3f cents while an error "
                              "it cannot answer was still there", r.cents[k] );
                    break;
                }
    }

    if( pinned_at[0] + pinned_at[1] > 0.05 || pinned_at[0] + pinned_at[1] < -0.05 )
        complain( &step, "the two ends of the bound are not symmetric: "
                  "%+.3f against %+.3f", pinned_at[0], pinned_at[1] );

    /* 4 and 5. A device so far off that the bound cannot keep up with it. The
     * error left over accumulates until it crosses the thresholds beyond the
     * resampler: 60000 ppm leaves about 55 ms a second after the bound has
     * taken its 5212, so 120 ms of earliness or 180 ms of lateness arrives
     * within a couple of seconds. Positive is a device whose clock runs fast,
     * which drains the queue sooner, reports less delay and so is heard early
     * - the silence end; negative is the slow device that has to be jumped
     * over. Both must keep playing. */
    struct run fast =
    {
        .name = "60000 ppm (fast)", .seconds = 10, .latency_ms = 50,
        .drift_ppm = 60000,
    };
    Play( &fast );
    report( &fast );

    if( fast.bound_hit < 1 )
        complain( &fast, "never flagged the bound" );
    if( fast.silences < 1 )
        complain( &fast, "never inserted silence for a device %d ppm fast",
                  fast.drift_ppm );
    if( fast.jumps != 0 )
        complain( &fast, "jumped ahead %u time(s) for a device that is too "
                  "fast", fast.jumps );
    /* Once every couple of seconds is the error crossing the threshold again,
     * which is what a device this far off means. A burst would be the
     * correction being applied over and over to the same offset. */
    if( fast.silences > 8 )
        complain( &fast, "inserted silence %u times in %u s", fast.silences,
                  fast.seconds );
    if( fast.played == 0 )
        complain( &fast, "nothing reached the device" );

    struct run slow =
    {
        .name = "-60000 ppm (slow)", .seconds = 10, .latency_ms = 50,
        .drift_ppm = -60000,
    };
    Play( &slow );
    report( &slow );

    if( slow.bound_hit < 1 )
        complain( &slow, "never flagged the bound" );
    if( slow.jumps < 1 )
        complain( &slow, "never jumped ahead for a device %d ppm slow",
                  slow.drift_ppm );
    if( slow.silences != 0 )
        complain( &slow, "inserted silence %u time(s) for a device that is "
                  "too slow", slow.silences );
    /* A jump shortens what is still to come and not what the output already
     * holds, so the drift reads high until that has played out: sync.skip
     * settles one jump at a time and a burst of them would mean that guard
     * has gone. */
    if( slow.jumps > 6 )
        complain( &slow, "jumped ahead %u times in %u s", slow.jumps,
                  slow.seconds );
    if( slow.played == 0 )
        complain( &slow, "nothing reached the device" );

    /* 6. A seek. What a flush leaves behind is a position error the size of
     * whatever the output was holding, and the first reading taken after it
     * says so in one go, which no device clock can do. It must not reach the
     * controller: the offset is put right where it happened and the command
     * carries on from where it was.
     *
     * The discontinuity a flush latches is what keeps that reading out, and
     * it was spent by the first block played whether or not that block had
     * been able to take a reading - which at the head of a flush it has not,
     * the output having nothing to time yet. Before that was fixed this case
     * pinned at the bound for the rest of the stream.
     *
     * The audio delay is the instrument: it sets the offset the flush leaves
     * to a known size, 100 ms here once the output's own queue is counted.
     * That is inside the 120 ms the early path answers with silence of its
     * own accord - so what is measured is this and not that - and well past
     * the 60 ms that saturates 9 cents of resampling.
     */
    const char *path = "drift-seek.wav";

    WriteFile( path, 12 );

    struct run seek =
    {
        .name = "seek", .seconds = 12, .latency_ms = 50, .desync_ms = 90,
        .path = path, .seek_at_ms = 4000, .seek_to_ms = 9000,
    };
    Play( &seek );
    report( &seek );
    unlink( path );

    if( seek.marked == 0 )
        complain( &seek, "never reached %u ms to seek from", seek.seek_at_ms );
    if( seek.silences < 1 )
        complain( &seek, "put none of the offset right where it happened" );
    if( seek.bound_hit != 0 )
        complain( &seek, "the correction hit its bound %u time(s) for a seek "
                  "on a device that is running at its nominal rate",
                  seek.bound_hit );
    unmoved( &seek, 2.0 );

    /* Silence insertion adds samples and a jump drops them, so the two ends
     * fall either side of what a nominal device is handed. */
    struct run nominal =
    {
        .name = "nominal", .seconds = 10, .latency_ms = 50,
    };
    Play( &nominal );
    report( &nominal );

    if( fast.played <= nominal.played )
        complain( &fast, "played %"PRIu64" samples against %"PRIu64" nominal, "
                  "expected more from inserting silence",
                  fast.played, nominal.played );
    if( slow.played >= nominal.played )
        complain( &slow, "played %"PRIu64" samples against %"PRIu64" nominal, "
                  "expected fewer from jumping ahead",
                  slow.played, nominal.played );

    return fail;
}
