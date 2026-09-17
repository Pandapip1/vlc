/*****************************************************************************
 * seek.c: drive seeks from a script and report what came out
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

/* Nothing in the suite could seek: every test either drives a demuxer with no
 * clock behind it or plays a stream from one end to the other. So anything a
 * seek is meant to do - land where it was asked to, leave nothing of what was
 * playing behind, resume at the rate that was set - could only be checked by
 * hand, and several things believed about seeking had never been measured.
 *
 * This plays a file and calls libvlc_media_player_set_time() at scripted
 * points, and it reads the answer out of the audio rather than out of the
 * player. The sample it plays is a ramp: sample n holds a value that maps
 * back to n, so every frame handed to the audio callback says where in the
 * file it came from, to a tenth of a millisecond. That turns the two
 * questions a seek raises into counts:
 *
 *   where did it land   the source position of the first frame out that is
 *                       anywhere near the target
 *   what was carried    the frames handed over after the seek was asked for
 *                       and before that one - audio from before the seek that
 *                       a stage of the pipeline was still holding
 *
 * The same ramp gives a ledger for playback rate: between two rate changes,
 * the source span consumed divided by the frames produced is the speed that
 * was actually played, whatever the clock believed. A filter that quietly
 * drops or repeats its input shows up there and nowhere else.
 *
 * With no arguments it runs the scenarios at the bottom against a ramp it
 * writes itself and asserts what they should do. Given a MRL it becomes a
 * tool: seek where you say, at the rate you say, with any libvlc options you
 * like, and print what came out.
 *
 *   test_libvlc_seek -S 500:5000 -S 1000:2000 -r 2.0 -o --no-audio-time-stretch
 *   test_libvlc_seek -S 300:60000 /path/to/movie.mkv
 */

#include "test.h"

#include <inttypes.h>
#include <stdarg.h>
#include <string.h>

#include <vlc_common.h>
#include <vlc_threads.h>

/*****************************************************************************
 * The ramp
 *****************************************************************************/

#define RAMP_RATE     48000u
#define RAMP_SECONDS  10u
#define RAMP_FRAMES   ((uint64_t)RAMP_RATE * RAMP_SECONDS)

static int16_t ramp_value( uint64_t n )
{
    return (int16_t)( -32768 + (int)( n * 65535 / ( RAMP_FRAMES - 1 ) ) );
}

/* Back to a position in the file, in microseconds. The ramp spans the whole
 * file, so a value is unambiguous - no unwrapping, and nothing to get wrong
 * when a seek makes the position jump. */
static int64_t ramp_us( int16_t v )
{
    return (int64_t)( v + 32768 ) * ( RAMP_FRAMES - 1 ) / 65535
           * INT64_C(1000000) / RAMP_RATE;
}

static void put32( uint8_t *p, uint32_t v )
{
    p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}

static void put16( uint8_t *p, uint16_t v )
{
    p[0] = v; p[1] = v >> 8;
}

static int ramp_write( const char *path )
{
    uint32_t data = (uint32_t)( RAMP_FRAMES * 2 );
    uint8_t hdr[44];
    FILE *f = fopen( path, "wb" );

    if( f == NULL )
        return -1;

    memcpy( hdr, "RIFF", 4 );      put32( hdr + 4, 36 + data );
    memcpy( hdr + 8, "WAVEfmt ", 8 );
    put32( hdr + 16, 16 );
    put16( hdr + 20, 1 );          put16( hdr + 22, 1 );
    put32( hdr + 24, RAMP_RATE );  put32( hdr + 28, RAMP_RATE * 2 );
    put16( hdr + 32, 2 );          put16( hdr + 34, 16 );
    memcpy( hdr + 36, "data", 4 ); put32( hdr + 40, data );

    if( fwrite( hdr, sizeof (hdr), 1, f ) != 1 )
        goto error;

    for( uint64_t n = 0; n < RAMP_FRAMES; n++ )
    {
        uint8_t s[2];

        put16( s, (uint16_t) ramp_value( n ) );
        if( fwrite( s, 2, 1, f ) != 1 )
            goto error;
    }

    return fclose( f );
error:
    fclose( f );
    return -1;
}

/*****************************************************************************
 * What came out
 *****************************************************************************/

#define MAX_BLOCKS 16384

struct out_block
{
    int64_t  pts;
    unsigned frames;
    int64_t  src_first;   /* position in the file of this block's first frame */
    int64_t  src_last;
};

struct capture
{
    vlc_mutex_t lock;
    struct out_block blocks[MAX_BLOCKS];
    unsigned nblocks;
    unsigned dropped;     /* blocks past MAX_BLOCKS, counted but not kept */
    uint64_t frames;
    unsigned flushes, pauses, resumes, drains;
    bool     ramp;
};

static void PlayCb( void *data, const void *samples, unsigned count,
                    int64_t pts )
{
    struct capture *c = data;
    const int16_t *s = samples;

    vlc_mutex_lock( &c->lock );
    if( count == 0 )
    {
        vlc_mutex_unlock( &c->lock );
        return;
    }
    if( c->nblocks < MAX_BLOCKS )
    {
        struct out_block *b = &c->blocks[c->nblocks++];

        b->pts = pts;
        b->frames = count;
        b->src_first = c->ramp ? ramp_us( s[0] ) : -1;
        b->src_last = c->ramp ? ramp_us( s[count - 1] ) : -1;
    }
    else
        c->dropped++;
    c->frames += count;
    vlc_mutex_unlock( &c->lock );
}

static void PauseCb( void *data, int64_t pts )
{
    struct capture *c = data;

    (void) pts;
    vlc_mutex_lock( &c->lock );
    c->pauses++;
    vlc_mutex_unlock( &c->lock );
}

static void ResumeCb( void *data, int64_t pts )
{
    struct capture *c = data;

    (void) pts;
    vlc_mutex_lock( &c->lock );
    c->resumes++;
    vlc_mutex_unlock( &c->lock );
}

static void FlushCb( void *data, int64_t pts )
{
    struct capture *c = data;

    (void) pts;
    vlc_mutex_lock( &c->lock );
    c->flushes++;
    vlc_mutex_unlock( &c->lock );
}

static void DrainCb( void *data )
{
    struct capture *c = data;

    vlc_mutex_lock( &c->lock );
    c->drains++;
    vlc_mutex_unlock( &c->lock );
}

/*****************************************************************************
 * The script
 *****************************************************************************/

#define MAX_STEPS 64

enum step_kind { STEP_SEEK, STEP_RATE, STEP_PAUSE, STEP_RESUME };

struct step
{
    int64_t        at;      /* us after the first frame came out */
    enum step_kind kind;
    int64_t        arg;     /* SEEK: us into the file. RATE: rate x 1000. */

    /* what came of it */
    unsigned first;         /* blocks already out when the step was taken */
    unsigned landed;        /* first block at the target, MAX_BLOCKS if none */
    int64_t  landed_at;
    int64_t  reported;      /* what the player said its time was, right after */
    uint64_t carried;       /* frames out between first and landed */
    int64_t  carry_from, carry_to;
};

struct run
{
    const char  *name;
    const char  *mrl;
    float        rate;
    const char  *opts[16];
    unsigned     nopts;
    struct step  steps[MAX_STEPS];
    unsigned     nsteps;
    int64_t      run_for;   /* us of playback after the last step */

    struct capture cap;
    uint64_t     frames;
    int64_t      first_out, last_out;
};

/* The source position a seek to <target> should produce, give or take. The
 * pipeline is several stages deep and a demuxer lands on whatever it can, so
 * this is deliberately loose: it is here to tell "landed" from "still playing
 * what it was", not to measure accuracy. */
#define LAND_SLOP  VLC_TICK_FROM_MS(400)

static void RunOne( struct run *r )
{
    const char *argv[32];
    unsigned argc = 0;

    argv[argc++] = "-vv";
    argv[argc++] = "--no-video";
    argv[argc++] = "--intf=dummy";
    argv[argc++] = "--no-media-library";
    /* A developer's vlcrc must not be able to change what this measures. */
    argv[argc++] = "--ignore-config";
    for( unsigned i = 0; i < r->nopts; i++ )
        argv[argc++] = r->opts[i];
    assert( argc <= sizeof (argv) / sizeof (argv[0]) );

    libvlc_instance_t *vlc = libvlc_new( argc, argv );
    assert( vlc != NULL );

    libvlc_media_t *md = libvlc_media_new_path( vlc, r->mrl );
    assert( md != NULL );

    libvlc_media_player_t *mp = libvlc_media_player_new_from_media( md );
    assert( mp != NULL );

    vlc_mutex_init( &r->cap.lock );
    libvlc_audio_set_format( mp, "S16N", RAMP_RATE, 1 );
    libvlc_audio_set_callbacks( mp, PlayCb, PauseCb, ResumeCb, FlushCb,
                                DrainCb, &r->cap );

    if( r->rate != 1.f )
        libvlc_media_player_set_rate( mp, r->rate );

    libvlc_media_player_play( mp );

    /* The script starts when the first frame comes out, not when play() is
     * called: how long a file takes to open is not what is being measured,
     * and a seek issued before the input is running is simply dropped. */
    int64_t started = 0;
    unsigned next = 0;

    for( unsigned tick = 0; tick < 4000; tick++ )
    {
        libvlc_state_t st = libvlc_media_player_get_state( mp );
        unsigned nblocks;

        if( st == libvlc_Error || st == libvlc_Ended )
            break;

        vlc_mutex_lock( &r->cap.lock );
        nblocks = r->cap.nblocks;
        vlc_mutex_unlock( &r->cap.lock );

        if( started == 0 )
        {
            if( nblocks > 0 )
                started = mdate();
        }
        else
        {
            int64_t elapsed = mdate() - started;

            while( next < r->nsteps && r->steps[next].at <= elapsed )
            {
                struct step *s = &r->steps[next++];

                s->first = nblocks;
                s->landed = MAX_BLOCKS;
                s->landed_at = -1;
                s->carried = 0;
                s->carry_from = s->carry_to = -1;

                switch( s->kind )
                {
                    case STEP_SEEK:
                        libvlc_media_player_set_time( mp,
                                            MS_FROM_VLC_TICK( s->arg ) );
                        break;
                    case STEP_RATE:
                        libvlc_media_player_set_rate( mp, s->arg / 1000.f );
                        break;
                    case STEP_PAUSE:
                        libvlc_media_player_set_pause( mp, 1 );
                        break;
                    case STEP_RESUME:
                        libvlc_media_player_set_pause( mp, 0 );
                        break;
                }
                s->reported = VLC_TICK_FROM_MS(
                                    libvlc_media_player_get_time( mp ) );
            }

            if( next >= r->nsteps
             && elapsed >= ( r->nsteps > 0 ? r->steps[r->nsteps - 1].at : 0 )
                           + r->run_for )
                break;
        }

        msleep( VLC_TICK_FROM_MS(5) );
    }

    libvlc_media_player_stop( mp );
    libvlc_media_player_release( mp );
    libvlc_media_release( md );
    libvlc_release( vlc );

    /* Charge each seek with everything that came out between the call and the
     * first block at the target. */
    for( unsigned i = 0; i < r->nsteps; i++ )
    {
        struct step *s = &r->steps[i];

        if( s->kind != STEP_SEEK )
            continue;

        for( unsigned b = s->first; b < r->cap.nblocks; b++ )
        {
            const struct out_block *ob = &r->cap.blocks[b];

            if( ob->src_first >= s->arg - LAND_SLOP
             && ob->src_first <= s->arg + LAND_SLOP )
            {
                s->landed = b;
                s->landed_at = ob->src_first;
                break;
            }

            s->carried += ob->frames;
            if( s->carry_from < 0 )
                s->carry_from = ob->src_first;
            s->carry_to = ob->src_last;
        }
    }

    r->frames = r->cap.frames;
    r->first_out = r->cap.nblocks > 0 ? r->cap.blocks[0].src_first : -1;
    r->last_out = r->cap.nblocks > 0
                ? r->cap.blocks[r->cap.nblocks - 1].src_last : -1;
    vlc_mutex_destroy( &r->cap.lock );
}

static void Report( const struct run *r )
{
    fprintf( stderr, "%s: %u blocks%s, %" PRIu64 " frames, source %"
             PRId64 " ms to %" PRId64 " ms, %u flushes\n",
             r->name, r->cap.nblocks,
             r->cap.dropped ? " (truncated)" : "",
             r->frames, r->first_out / 1000, r->last_out / 1000,
             r->cap.flushes );

    for( unsigned i = 0; i < r->nsteps; i++ )
    {
        const struct step *s = &r->steps[i];

        if( s->kind == STEP_SEEK )
        {
            fprintf( stderr, "  %6" PRId64 " ms: seek to %" PRId64
                     " ms -> player says %" PRId64 " ms, ",
                     s->at / 1000, s->arg / 1000, s->reported / 1000 );
            if( s->landed < MAX_BLOCKS )
                fprintf( stderr, "audio at %" PRId64 " ms", s->landed_at / 1000 );
            else
                fprintf( stderr, "NEVER ARRIVED" );
            if( s->carried > 0 )
                fprintf( stderr, ", carried %" PRIu64 " frames (%" PRId64
                         " ms) of %" PRId64 "-%" PRId64 " ms",
                         s->carried,
                         (int64_t) s->carried * 1000 / RAMP_RATE,
                         s->carry_from / 1000, s->carry_to / 1000 );
            fputc( '\n', stderr );
        }
        else if( s->kind == STEP_RATE )
            fprintf( stderr, "  %6" PRId64 " ms: rate %.3f\n",
                     s->at / 1000, s->arg / 1000.f );
        else
            fprintf( stderr, "  %6" PRId64 " ms: %s\n", s->at / 1000,
                     s->kind == STEP_PAUSE ? "pause" : "resume" );
    }
}

/* Source consumed against frames produced, over the blocks of one span. The
 * ratio is the speed that was played, whatever anything else reports. */
static double Ledger( const struct run *r, unsigned from, unsigned to,
                      uint64_t *pframes )
{
    uint64_t frames = 0;
    int64_t span;

    if( to > r->cap.nblocks )
        to = r->cap.nblocks;
    if( from + 1 >= to )
        return 0.;

    for( unsigned b = from; b < to; b++ )
        frames += r->cap.blocks[b].frames;

    span = r->cap.blocks[to - 1].src_last - r->cap.blocks[from].src_first;
    if( pframes != NULL )
        *pframes = frames;
    if( frames == 0 )
        return 0.;

    return (double) span * RAMP_RATE / ( (double) frames * 1000000. );
}

/* A rate change reaches the audio callback a whole output buffer after it is
 * asked for, so a span that starts where the call was made holds the tail of
 * the rate before it. Step over that much output first. */
static unsigned Settle( const struct run *r, unsigned from, int64_t us )
{
    uint64_t want = (uint64_t) us * RAMP_RATE / 1000000;
    uint64_t frames = 0;

    while( from < r->cap.nblocks && frames < want )
        frames += r->cap.blocks[from++].frames;

    return from;
}

/*****************************************************************************
 * Scenarios
 *****************************************************************************/

static int fail;

static void complain( const struct run *r, const char *what, ... )
{
    va_list ap;

    fprintf( stderr, "FAIL %s: ", r->name );
    va_start( ap, what );
    vfprintf( stderr, what, ap );
    va_end( ap );
    fputc( '\n', stderr );
    fail = 1;
}

static void CheckSeeks( struct run *r, int64_t carry_max )
{
    for( unsigned i = 0; i < r->nsteps; i++ )
    {
        const struct step *s = &r->steps[i];
        int64_t carried_us;

        if( s->kind != STEP_SEEK )
            continue;

        if( s->landed >= MAX_BLOCKS )
        {
            complain( r, "seek to %" PRId64 " ms never came out",
                      s->arg / 1000 );
            continue;
        }

        if( llabs( s->reported - s->arg ) > LAND_SLOP )
            complain( r, "seek to %" PRId64 " ms reported as %" PRId64 " ms",
                      s->arg / 1000, s->reported / 1000 );

        carried_us = (int64_t) s->carried * 1000000 / RAMP_RATE;
        if( carried_us > carry_max )
            complain( r, "seek to %" PRId64 " ms carried %" PRId64
                      " ms of %" PRId64 "-%" PRId64 " ms over it",
                      s->arg / 1000, carried_us / 1000,
                      s->carry_from / 1000, s->carry_to / 1000 );
    }
}

/* Each entry is "play this long, then seek there", so a script reads in the
 * order it happens and adding a step does not move the ones after it. */
static void Scripted( struct run *r, const int64_t (*seeks)[2], unsigned n )
{
    int64_t at = 0;

    for( unsigned i = 0; i < n; i++ )
    {
        at += VLC_TICK_FROM_MS( seeks[i][0] );
        r->steps[r->nsteps].at = at;
        r->steps[r->nsteps].kind = STEP_SEEK;
        r->steps[r->nsteps].arg = VLC_TICK_FROM_MS( seeks[i][1] );
        r->nsteps++;
    }
    r->run_for = VLC_TICK_FROM_MS(700);
}

int main( int argc, char *argv[] )
{
    char path[64];
    const char *mrl = NULL;
    float rate = 1.f;
    const char *opts[16];
    unsigned nopts = 0;
    int64_t seeks[MAX_STEPS][2];
    unsigned nseeks = 0;

    alarm( 120 );
    setenv( "VLC_PLUGIN_PATH", "../modules", 1 );

    for( int i = 1; i < argc; i++ )
    {
        if( !strcmp( argv[i], "-S" ) && i + 1 < argc )
        {
            long long at, to;

            if( sscanf( argv[++i], "%lld:%lld", &at, &to ) != 2 )
                return 1;
            seeks[nseeks][0] = at;
            seeks[nseeks][1] = to;
            nseeks++;
        }
        else if( !strcmp( argv[i], "-r" ) && i + 1 < argc )
            rate = atof( argv[++i] );
        else if( !strcmp( argv[i], "-o" ) && i + 1 < argc )
            opts[nopts++] = argv[++i];
        else
            mrl = argv[i];
    }

    snprintf( path, sizeof (path), "seek-ramp-%d.wav", (int) getpid() );
    if( ramp_write( path ) )
    {
        fprintf( stderr, "cannot write %s\n", path );
        return 77;
    }

    if( mrl != NULL || nseeks > 0 || rate != 1.f || nopts > 0 )
    {
        struct run r = { .name = "run", .rate = rate, .nopts = nopts };

        r.mrl = mrl != NULL ? mrl : path;
        r.cap.ramp = mrl == NULL;
        memcpy( r.opts, opts, nopts * sizeof (opts[0]) );
        Scripted( &r, (const int64_t (*)[2]) seeks, nseeks );
        RunOne( &r );
        Report( &r );
        unlink( path );
        return 0;
    }

    /* Forward, backward, and to a point already played. */
    {
        static const int64_t s[][2] = { { 500, 6000 }, { 500, 2000 },
                                        { 500, 8500 } };
        struct run r = { .name = "seek", .rate = 1.f, .mrl = path,
                         .cap = { .ramp = true } };

        Scripted( &r, s, 3 );
        RunOne( &r );
        Report( &r );
        CheckSeeks( &r, VLC_TICK_FROM_MS(10) );
    }

    /* The same with the rate off nominal, which is what puts scaletempo in
     * the pipeline: a filter that holds a queue across blocks has to let go
     * of it on a flush or the audio from before the seek is spliced onto the
     * audio after it. */
    {
        static const int64_t s[][2] = { { 500, 6000 }, { 500, 2000 } };
        struct run r = { .name = "seek-stretched", .rate = 2.f, .mrl = path,
                         .cap = { .ramp = true } };

        Scripted( &r, s, 2 );
        RunOne( &r );
        Report( &r );
        CheckSeeks( &r, VLC_TICK_FROM_MS(10) );

        double speed = Ledger( &r, Settle( &r, 0, VLC_TICK_FROM_MS(200) ),
                               r.steps[0].first, NULL );

        fprintf( stderr, "%s: played at %.4fx before the first seek\n",
                 r.name, speed );
        if( speed < 1.9 || speed > 2.1 )
            complain( &r, "asked for 2x, played %.4fx", speed );
    }

    /* A rate change is a scale change, and the filter has to carry what the
     * last stride committed to consuming across it. The ledger is the only
     * thing that can see it: the output of a repeated or dropped stride
     * sounds exactly like the output of the right one. */
    {
        struct run r = { .name = "rate-churn", .rate = 1.5f, .mrl = path,
                         .cap = { .ramp = true } };
        static const int rates[] = { 2000, 1250, 3000, 1500, 2500, 1100 };

        for( unsigned i = 0; i < sizeof (rates) / sizeof (rates[0]); i++ )
        {
            r.steps[r.nsteps].at = VLC_TICK_FROM_MS( 600 * ( i + 1 ) );
            r.steps[r.nsteps].kind = STEP_RATE;
            r.steps[r.nsteps].arg = rates[i];
            r.nsteps++;
        }
        r.run_for = VLC_TICK_FROM_MS(600);
        RunOne( &r );
        Report( &r );

        for( unsigned i = 0; i + 1 < r.nsteps; i++ )
        {
            uint64_t frames = 0;
            unsigned from = Settle( &r, r.steps[i].first,
                                    VLC_TICK_FROM_MS(200) );
            double speed = Ledger( &r, from, r.steps[i + 1].first, &frames );
            double want = r.steps[i].arg / 1000.;

            fprintf( stderr, "  rate %.3f: %" PRIu64 " frames out, played at "
                     "%.4fx\n", want, frames, speed );
            if( frames > 0 && ( speed < want * 0.97 || speed > want * 1.03 ) )
                complain( &r, "rate %.3f played at %.4fx", want, speed );
        }
    }

    /* The same scale, arrived at again and again. Two rates a thousandth
     * apart are the same speed to within the margin below, so whatever the
     * ledger reads here is the cost of the change itself and not of the rate
     * that was asked for. */
    {
        struct run r = { .name = "scale-churn", .rate = 2.f, .mrl = path,
                         .cap = { .ramp = true } };

        for( unsigned i = 0; i < 48; i++ )
        {
            r.steps[r.nsteps].at = VLC_TICK_FROM_MS( 60 * ( i + 1 ) );
            r.steps[r.nsteps].kind = STEP_RATE;
            r.steps[r.nsteps].arg = ( i % 2 ) ? 2000 : 1996;
            r.nsteps++;
        }
        r.run_for = VLC_TICK_FROM_MS(200);
        RunOne( &r );
        Report( &r );

        uint64_t frames = 0;
        unsigned from = Settle( &r, 0, VLC_TICK_FROM_MS(300) );
        double speed = Ledger( &r, from, r.cap.nblocks, &frames );

        fprintf( stderr, "%s: %u rate changes, %" PRIu64 " frames out, "
                 "played at %.4fx\n", r.name, r.nsteps, frames, speed );
        if( frames > 0 && ( speed < 1.99 || speed > 2.01 ) )
            complain( &r, "48 changes between 1.996x and 2x played at %.4fx",
                      speed );
    }

    unlink( path );
    return fail;
}
