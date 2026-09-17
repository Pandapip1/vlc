#! /usr/bin/env perl
#*****************************************************************************
# aout-drift-view.pl : draws what the audio drift correction did
#*****************************************************************************
# Copyright (C) 2026 VLC authors and VideoLAN
#
# This program is free software; you can redistribute it and/or modify it
# under the terms of the GNU Lesser General Public License as published by
# the Free Software Foundation; either version 2.1 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
# GNU Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
#*****************************************************************************

# Reads the CSV that "--aout-drift-trace" writes and draws it: the device's
# reported delay and the drift against it, the four quantities the controller
# is made of, the steps upstream handed over, and where the discontinuity was
# latched - which is the one thing that makes a correction unaccountable,
# since a latch closes both drift thresholds to zero for the block after it.
#
# Core Perl only, so it runs wherever the build does.

use strict;
use warnings;

my $SELF = 'aout-drift-view.pl';

#
#   Arguments
#

my %opt = (
    mode    => 'text',      # text | svg | tui
    out     => undef,
    width   => 100,
    height  => 14,
    from    => undef,
    to      => undef,
    follow  => 0,
    loop    => 'closed',    # how a what-if is simulated
    events  => 20,          # most steps and the like to list
    chase   => 0,           # a what-if that takes the seams for drift
);
my %sim;                    # gains to try instead of the ones the run used
my $file;

while (@ARGV)
{
    my $a = shift @ARGV;

    if    ($a eq '--svg')     { $opt{mode} = 'svg'; $opt{out} = shift @ARGV }
    elsif ($a eq '--tui')     { $opt{mode} = 'tui' }
    elsif ($a eq '--text')    { $opt{mode} = 'text' }
    elsif ($a eq '--stats')   { $opt{mode} = 'stats' }
    elsif ($a eq '--follow')  { $opt{follow} = 1 }
    elsif ($a eq '--open')    { $opt{loop} = 'open' }
    elsif ($a eq '--chase')   { $opt{chase} = 1 }
    elsif ($a eq '--closed')  { $opt{loop} = 'closed' }
    elsif ($a eq '--width')   { $opt{width} = 0 + shift @ARGV }
    elsif ($a eq '--height')  { $opt{height} = 0 + shift @ARGV }
    elsif ($a eq '--from')    { $opt{from} = 0.0 + shift @ARGV }
    elsif ($a eq '--to')      { $opt{to} = 0.0 + shift @ARGV }
    elsif ($a eq '--events')  { $opt{events} = 0 + shift @ARGV }
    elsif ($a eq '--kp')      { $sim{kp} = 0.0 + shift @ARGV }
    elsif ($a eq '--ki')      { $sim{ki} = 0.0 + shift @ARGV }
    elsif ($a eq '--slew')    { $sim{slew} = 0.0 + shift @ARGV }
    elsif ($a eq '--max')     { $sim{max_cents} = 0.0 + shift @ARGV }
    elsif ($a eq '-h' || $a eq '--help') { usage (0) }
    elsif ($a =~ /^-/)        { die "$SELF: unknown option $a\n" }
    else                      { $file = $a }
}

usage (1) unless defined $file;

sub usage
{
    print <<"END";
$SELF [options] <trace.csv>

Draws the trace that VLC writes when it is run with

    --aout-drift-trace=trace.csv

which costs the player nothing unless it is set. Options:

  --text            a report on the terminal (the default)
  --stats           the numbers alone, no plots
  --svg <file>      draw it as SVG instead
  --tui             an interactive view, with the gains on sliders
  --follow          keep re-reading the file as it grows
  --from <s>        window, in seconds from the start of the trace
  --to <s>
  --width <n>       plot size, in characters or in SVG pixels
  --height <n>
  --events <n>      how many steps and the like to list (default 20)

A latched discontinuity closes both drift thresholds to zero for the reading
that follows it, so the correction it then provokes has no explanation in the
drift alone. The trace records every latch and what it was blamed on, and the
report says how many blocks apart they fell and what each one cost.

  --kp <cents/s>    replay the controller with these gains instead of the
  --ki <cents/s2>   ones the run used, and draw both
  --slew <s>
  --max <cents>
  --closed          a what-if answers back to the device it estimates from
  --chase           a what-if that does not recognise a step: what the
                    controller would have done had it chased every seam
  --open            a what-if is replayed against the recorded drift alone

END
    exit $_[0];
}

#
#   The trace
#

# Returns the header constants and the rows. A row is a hash; a field that was
# not measured is absent rather than zero, because zero is a good drift.
sub read_trace
{
    my ($path) = @_;
    my (%hdr, @rows);

    open (my $fh, '<', $path) or die "$SELF: cannot read $path: $!\n";

    while (my $line = <$fh>)
    {
        chomp $line;

        if ($line =~ /^#/)
        {
            while ($line =~ /([a-z_]+)=(-?[0-9.]+)/g) { $hdr{$1} = 0.0 + $2 }
            next;
        }
        next if $line =~ /^t_us,/ || $line !~ /,/;

        my @f = split (/,/, $line, -1);
        next unless @f >= 13 && $f[0] =~ /^-?\d+$/;

        push @rows, {
            t     => 0 + $f[0],
            ev    => $f[1],
            drift => len ($f[2]),
            delay => len ($f[3]),
            p     => len ($f[4]),
            i     => len ($f[5]),
            cmd   => len ($f[6]),
            tgt   => len ($f[7]),
            det   => len ($f[8]),
            bound => len ($f[9]),
            extra => len ($f[10]),
            codec => ($f[11] ne '' ? $f[11] : undef),
            es    => len ($f[12]),
            disc  => len ($f[13] // ''),
            pts   => len ($f[14] // ''),
            end   => len ($f[15] // ''),
            samples => len ($f[16] // ''),
            in_rate => len ($f[17] // ''),
            since => len ($f[18] // ''),
        };
    }
    close $fh;

    die "$SELF: $path holds no rows\n" unless @rows;

    my $t0 = $rows[0]{t};
    $_->{t} -= $t0 for @rows;

    $hdr{max_cents} = 9 unless defined $hdr{max_cents};
    $hdr{kp} = 150 unless defined $hdr{kp};
    $hdr{ki} = 3.75 unless defined $hdr{ki};
    $hdr{slew} = 1 unless defined $hdr{slew};

    return (\%hdr, \@rows);
}

sub len { my $s = shift; return ($s eq '') ? undef : 0.0 + $s }

sub window
{
    my ($rows) = @_;
    return $rows unless defined $opt{from} || defined $opt{to};

    my $a = defined $opt{from} ? $opt{from} * 1e6 : -1e18;
    my $b = defined $opt{to} ? $opt{to} * 1e6 : 1e18;

    return [ grep { $_->{t} >= $a && $_->{t} <= $b } @$rows ];
}

# Events that carry the timeline, rather than a reading of it.
my %BREAK = map { $_ => 1 }
    qw(step declared flush restart jump silence start stop pause resume
       droplate dropearly);

# A block that neither latched nor was read: it carries the arithmetic the
# step test ran on and nothing else, so it interrupts nothing.
sub quiet { return $_[0]{ev} eq 'block' }

# Whether this block's own reading was judged with the thresholds collapsed:
# either it found the latch already set, or it is what set it.
sub zeroed { return $_[0]{disc} || defined $_[0]{since} }

#
#   What the numbers come to
#

# A detune in cents, as the fraction of the nominal rate it amounts to.
sub offset_of { return 2 ** ($_[0] / 1200) - 1 }

# The device's own clock error, which is not what the controller is doing
# about it. The drift moves at the difference between the two, so the error is
# what is left once the correction in force is added back. Taken only over
# runs of readings with no step, flush or jump in them, since across one of
# those the drift moves for a reason that is not the clock.
sub rate_mismatch
{
    my ($rows) = @_;
    my (@seg, @out);

    my $flush = sub {
        return unless @seg >= 8;

        # The correction takes a slew time constant or two to reach whatever
        # it is going to hold, and over that stretch the offset in force is
        # not the one the drift it produced was measured against. Give the
        # start of every run away rather than average across it.
        my $settle = $seg[0]{t} + 3e6;
        my @w = grep { $_->{t} >= $settle } @seg;
        @w = @seg unless @w >= 8;

        my $span = $w[-1]{t} - $w[0]{t};
        return unless $span > 2e6;

        my ($n, $st, $sv, $stt, $stv) = (scalar @w, 0, 0, 0, 0);
        my $off = 0;
        for my $r (@w)
        {
            $st += $r->{t}; $sv += $r->{drift};
            $stt += $r->{t} ** 2; $stv += $r->{t} * $r->{drift};
            $off += offset_of ($r->{det} // 0);
        }
        my $den = $n * $stt - $st ** 2;
        return unless $den != 0;

        my $slope = ($n * $stv - $st * $sv) / $den;
        push @out, { ppm => -($slope + $off / $n) * 1e6, span => $span };
    };

    for my $r (@$rows)
    {
        next if quiet ($r);
        if ($BREAK{$r->{ev}} || $r->{ev} eq 'excluded' || !defined $r->{drift})
        {
            $flush->(); @seg = (); next;
        }
        push @seg, $r;
    }
    $flush->();

    return undef unless @out;

    my ($w, $s) = (0, 0);
    $s += $_->{ppm} * $_->{span}, $w += $_->{span} for @out;
    my $mean = $s / $w;

    my $var = 0;
    $var += ($_->{ppm} - $mean) ** 2 * $_->{span} for @out;

    return { ppm => $mean, sd => sqrt ($var / $w), segments => scalar @out };
}

# What a latch is blamed on. Everything else that sets it - a flush, a pause,
# an inserted silence, the start of a stream - is named by its own event.
my %BLAME = (step => 'step', declared => 'declared');

sub stats
{
    my ($hdr, $rows) = @_;
    my %s = (
        rows => scalar @$rows,
        span => ($rows->[-1]{t} - $rows->[0]{t}) / 1e6,
        steps => [], counts => {},
    );

    my (@drift, $bound_t, $last_t, $prev, $pending);

    for my $r (@$rows)
    {
        $s{counts}{$r->{ev}}++;

        if ($r->{ev} eq 'step')
        {
            push @{$s{steps}}, $r;
            $s{by_es}{($r->{codec} // '?') . ' ' . ($r->{es} // 0)}{n}++;
            $s{by_es}{($r->{codec} // '?') . ' ' . ($r->{es} // 0)}{sum}
                += abs ($r->{extra} // 0);
        }

        if (defined $r->{pts})
        {
            $s{blocks}++;
            $s{blocks_zeroed}++ if zeroed ($r);
        }

        if (defined $r->{drift})
        {
            $s{readings}++;
            $s{zeroed}++ if $r->{disc};

            # What the reading a latch put in front of the controller then
            # did. This is the audible one: with both thresholds at zero any
            # drift at all is answered by inserting silence or jumping.
            if ($pending) { $s{after}{$r->{ev}}++; undef $pending }
        }

        if (defined $r->{since})
        {
            $s{latches}++;
            $s{latch}{$BLAME{$r->{ev}} || 'elsewhere'}++;
            push @{$s{since}}, $r->{since} if $BLAME{$r->{ev}};
            $pending = $r;
        }
        push @drift, $r->{drift} if defined $r->{drift};

        # Time spent pinned, charged to the interval each row stands for.
        if (defined $prev && $prev->{bound})
        {
            $bound_t += $r->{t} - $prev->{t};
        }
        $prev = $r;
        $last_t = $r;
    }

    $s{bound_pc} = $s{span} > 0 ? 100 * ($bound_t // 0) / 1e6 / $s{span} : 0;
    $s{now} = $last_t;

    if (@drift)
    {
        my ($sum, $min, $max) = (0, $drift[0], $drift[0]);
        for (@drift)
        {
            $sum += $_;
            $min = $_ if $_ < $min;
            $max = $_ if $_ > $max;
        }
        $s{drift_mean} = $sum / @drift;
        my $v = 0;
        $v += ($_ - $s{drift_mean}) ** 2 for @drift;
        $s{drift_sd} = sqrt ($v / @drift);
        $s{drift_min} = $min;
        $s{drift_max} = $max;
    }

    $s{rate} = rate_mismatch ($rows);
    $s{steps_per_min} = $s{span} > 0 ? 60 * @{$s{steps}} / $s{span} : 0;

    if (@{$s{steps}})
    {
        my ($t, $alt, $prev_step) = (0, 0);
        for my $r (@{$s{steps}})
        {
            my $v = $r->{extra} // 0;
            $t += abs $v;
            $alt++ if defined $prev_step && $prev_step * $v < 0;
            $prev_step = $v;
        }
        $s{step_mean} = $t / @{$s{steps}};
        $s{step_alt} = $alt;
    }

    if ($s{since} && @{$s{since}})
    {
        my @v = sort { $a <=> $b } @{$s{since}};
        $s{since_min} = $v[0];
        $s{since_med} = $v[int (@v / 2)];
        $s{since_1} = scalar grep { $_ == 1 } @v;
        $s{since_2} = scalar grep { $_ == 2 } @v;
    }

    return \%s;
}

#
#   What a different controller would have done
#
# Replayed exactly as src/audio_output/dec.c has it, on the drift the run
# recorded. Open loop that is a fair question only about the controller: the
# drift it is fed is the one the real correction produced, so a gain that
# would have changed the drift is not answered back to. Closed loop puts the
# device back in: the clock error is estimated from the trace, the jitter is
# kept as the residual around it, and the recorded steps are re-injected, so
# the simulated correction feeds back into the simulated drift.

sub simulate
{
    my ($hdr, $rows, $par, $closed) = @_;

    my %p = (%$hdr, %$par);
    my $max = $p{max_cents};

    my ($integral, $detune, $update) = (0, 0, undef);
    my ($drift, $ppm) = (0, 0);

    if ($closed)
    {
        my $m = rate_mismatch ($rows);
        $ppm = $m ? $m->{ppm} : 0;
    }

    my @out;
    my $prev_t;

    for my $r (@$rows)
    {
        next if quiet ($r);

        my $dt = defined $prev_t ? $r->{t} - $prev_t : 0;
        $prev_t = $r->{t};

        if ($closed)
        {
            # The offset closes at the difference between the device's error
            # and the correction in force against it: settled, the two cancel
            # and the drift stands still, which is the relation the clock
            # estimate above was taken from.
            $drift -= ($ppm * 1e-6 + offset_of ($detune)) * $dt if $dt > 0;

            # A hole moves the source's dates forward under an output that has
            # not moved, and the drift is measured against those dates, so a
            # step of +N takes N off the drift.
            $drift -= $r->{extra} if $r->{ev} eq 'step' && defined $r->{extra};
        }

        # A reading the run did not answer with a detune, because the step it
        # sat on top of had already been recognised: it was either excluded
        # outright, or put right by inserting silence or jumping ahead, which
        # only happen at all because a step sets the discontinuity flag and
        # with it a threshold of zero. Nobody recognising the step leaves
        # exactly these readings in front of the controller, subject to the
        # thresholds that would then have applied.
        my $revived = $opt{chase} && defined $r->{drift}
                      && ($r->{ev} eq 'excluded' || $r->{ev} eq 'silence'
                          || $r->{ev} eq 'jump' || $r->{ev} eq 'settling')
                      && $r->{drift} < ($p{jump_us} // 180000)
                      && $r->{drift} > ($p{silence_us} // -120000);

        if (!$revived && ($BREAK{$r->{ev}} || $r->{ev} eq 'excluded'
         || $r->{ev} eq 'untimed' || !defined $r->{drift}))
        {
            $update = $r->{t};
            $drift = 0 if $closed && !$opt{chase}
                       && ($r->{ev} eq 'step' || $r->{ev} eq 'flush');
            push @out, { t => $r->{t}, ev => $r->{ev} };
            next;
        }

        # The jitter the run was read through is measurement, not state: it is
        # what the controller saw, and it does not move the offset itself.
        my $d = $closed ? $drift + ($r->{noise} // 0) : $r->{drift};

        my $prop = $p{kp} * $d / 1e6;
        my $cmd = $prop + $integral;
        my $tgt = $cmd > $max ? $max : $cmd < -$max ? -$max : $cmd;
        my $bound = abs ($cmd - $tgt) > 1e-9;

        my $gap = defined $update ? $r->{t} - $update : 0;
        $gap = 1e6 if $gap > 1e6;
        my $secs = $gap > 0 ? $gap / 1e6 : 0;

        $detune += ($tgt - $detune) * $secs / ($p{slew} + $secs) if $secs > 0;

        if ($secs > 0)
        {
            my $i = $integral + $p{ki} * $d / 1e6 * $secs;
            $i += ($tgt - $cmd) * ($p{ki} / $p{kp}) * $secs if $p{kp} > 0;
            $integral = $i > $max ? $max : $i < -$max ? -$max : $i;
        }
        $update = $r->{t};

        push @out, { t => $r->{t}, ev => $r->{ev}, drift => $d,
                     p => $prop, i => $integral,
                     cmd => $cmd, tgt => $tgt, det => $detune,
                     bound => $bound ? 1 : 0 };
    }

    return \@out;
}

# The part of the recorded drift the clock error does not explain, kept so a
# closed-loop what-if is as noisy as the run was.
sub add_noise
{
    my ($rows) = @_;
    my @seg;

    # Detrended against the drift's OWN line through each uninterrupted run,
    # not against the clock error: what the run recorded is the drift that was
    # left after the correction of the day, so its trend is whatever that
    # correction did not take out. Subtracting it leaves the part that is
    # neither clock nor controller - the reading noise, and the lateness that
    # a thread can only ever wake with - which is what a what-if should be
    # made to work through as well.
    my $flush = sub {
        return unless @seg;

        if (@seg < 4) { $_->{noise} = 0 for @seg; return }

        my ($n, $st, $sv, $stt, $stv) = (scalar @seg, 0, 0, 0, 0);
        for my $r (@seg)
        {
            $st += $r->{t}; $sv += $r->{drift};
            $stt += $r->{t} ** 2; $stv += $r->{t} * $r->{drift};
        }
        my $den = $n * $stt - $st ** 2;
        if ($den == 0) { $_->{noise} = 0 for @seg; return }

        my $slope = ($n * $stv - $st * $sv) / $den;
        my $icept = ($sv - $slope * $st) / $n;

        $_->{noise} = $_->{drift} - ($icept + $slope * $_->{t}) for @seg;
    };

    for my $r (@$rows)
    {
        next if quiet ($r);
        if ($BREAK{$r->{ev}} || !defined $r->{drift})
        {
            $r->{noise} = 0;
            $flush->(); @seg = (); next;
        }
        push @seg, $r;
    }
    $flush->();
}

#
#   Plotting, on a character grid
#

# Scaled to where the data actually lives, not to its worst excursion: one
# 1.2 s buffer at start would otherwise flatten a whole run of readings into a
# single row. The tails are drawn against the edge and the panel says so, so
# nothing is quietly dropped.
sub series_bounds
{
    my ($series) = @_;
    my @v;

    for my $s (@$series) { push @v, map { $_->[1] } @{$s->{pts}} }
    return (0, 1, 0) unless @v;

    my @sorted = sort { $a <=> $b } @v;
    my $lo = $sorted[int (0.005 * $#sorted)];
    my $hi = $sorted[int (0.995 * $#sorted + 0.5)];
    my $clipped = ($sorted[0] < $lo || $sorted[-1] > $hi) ? 1 : 0;

    # Zero is where everything here is read from, so keep it on the panel.
    $lo = 0 if $lo > 0;
    $hi = 0 if $hi < 0;

    if ($hi - $lo < 1e-9) { $lo -= 1; $hi += 1 }
    my $pad = ($hi - $lo) * 0.08;
    return ($lo - $pad, $hi + $pad, $clipped);
}

sub plot_text
{
    my (%a) = @_;
    my ($w, $h) = ($opt{width}, $a{height} || $opt{height});
    my ($t0, $t1) = ($a{t0}, $a{t1});
    $t1 = $t0 + 1 if $t1 <= $t0;

    my ($lo, $hi, $clipped) = series_bounds ($a{series});
    my @g = map { [ (' ') x $w ] } 1 .. $h;

    my $row = sub {
        my $v = shift;
        my $y = int (($hi - $v) / ($hi - $lo) * ($h - 1) + 0.5);
        return $y < 0 ? 0 : $y >= $h ? $h - 1 : $y;
    };
    my $col = sub {
        my $t = shift;
        my $x = int (($t - $t0) / ($t1 - $t0) * ($w - 1) + 0.5);
        return $x < 0 ? 0 : $x >= $w ? $w - 1 : $x;
    };

    # Rules first, so the data sits on top of them.
    for my $v (@{$a{rules} || []})
    {
        next if $v < $lo || $v > $hi;
        my $y = $row->($v);
        $g[$y][$_] = '-' for 0 .. $w - 1;
    }

    for my $s (@{$a{series}})
    {
        my @acc;
        for my $pt (@{$s->{pts}})
        {
            push @{$acc[$col->($pt->[0])]}, $pt->[1];
        }
        for my $x (0 .. $w - 1)
        {
            next unless $acc[$x] && @{$acc[$x]};
            my ($mn, $mx) = ($acc[$x][0], $acc[$x][0]);
            for (@{$acc[$x]}) { $mn = $_ if $_ < $mn; $mx = $_ if $_ > $mx }
            for my $y ($row->($mx) .. $row->($mn))
            {
                $g[$y][$x] = $s->{ch};
            }
        }
    }

    my $fmt = $a{fmt} || sub { sprintf '%8.2f', $_[0] };
    my @lines;
    for my $y (0 .. $h - 1)
    {
        my $label = ($y == 0) ? $fmt->($hi)
                  : ($y == $h - 1) ? $fmt->($lo)
                  : (' ' x 8);
        push @lines, "$label |" . join ('', @{$g[$y]});
    }

    my @note;
    push @note, 'tails against the edge' if $clipped;
    for my $v (@{$a{rules} || []})
    {
        push @note, sprintf ('%+.4g off scale', $v)
            if $v != 0 && ($v < $lo || $v > $hi);
    }
    push @lines, (' ' x 10) . '(' . join ('; ', @note) . ')' if @note;

    return @lines;
}

# One line of letters marking where each thing happened. Where two land in the
# same column the one that matters more wins, so a seam is never hidden by the
# reading that follows it.
my @MARK = ( [ step => 'S' ], [ declared => 'D' ], [ runout => 'R' ],
             [ jump => 'J' ], [ silence => 'z' ], [ droplate => 'L' ],
             [ dropearly => 'E' ], [ restart => 'X' ], [ flush => 'F' ],
             [ pause => 'P' ], [ resume => 'p' ], [ excluded => 'x' ] );
my %MARK = map { @$_ } @MARK;
my %RANK; $RANK{$MARK[$_][0]} = $#MARK - $_ for 0 .. $#MARK;

sub rail_text
{
    my ($rows, $t0, $t1) = @_;
    my $w = $opt{width};
    my (@g, @rank);
    @g = ('.') x $w;
    $t1 = $t0 + 1 if $t1 <= $t0;

    for my $r (@$rows)
    {
        my $c = $MARK{$r->{ev}} or next;
        my $x = int (($r->{t} - $t0) / ($t1 - $t0) * ($w - 1) + 0.5);
        next if $x < 0 || $x >= $w;
        next if defined $rank[$x] && $rank[$x] >= $RANK{$r->{ev}};
        $g[$x] = $c;
        $rank[$x] = $RANK{$r->{ev}};
    }
    return (' ' x 8) . ' |' . join ('', @g);
}

#
#   The text report
#

# Which branch of aout_DecSynchronize the reading after a latch took. The
# first two are heard; the rest are not.
my %AFTER = (silence => 'silences', jump => 'jumps', settling => 'settling',
             excluded => 'excluded', sync => 'corrected');

sub ms { sprintf '%8.1f', $_[0] / 1000 }

sub fmt_stats
{
    my ($hdr, $s) = @_;
    my @o;

    push @o, sprintf ("  window          %.2f s, %d rows, %d readings",
                      $s->{span}, $s->{rows},
                      ($s->{counts}{sync} // 0)
                      + ($s->{counts}{excluded} // 0));
    push @o, sprintf ("  controller      kp %.4g cents/s   ki %.4g cents/s2   "
                      . "slew %.4g s   bound +-%.4g cents",
                      $hdr->{kp}, $hdr->{ki}, $hdr->{slew}, $hdr->{max_cents});

    # Worth saying plainly rather than leaving to be worked out from a plot of
    # nothing: at a bound of zero the controller never runs, and every offset
    # that would have been detuned away is answered by inserting silence or
    # jumping ahead instead - which is heard.
    push @o, "  NOT CORRECTING  the bound is zero, so nothing in this trace "
             . "was corrected by detuning and every offset was spliced out "
             . "instead. Set aout-max-resampling - and check vlcrc, which "
             . "overrides the default."
        if $hdr->{max_cents} <= 0;

    if ($s->{rate})
    {
        push @o, sprintf ("  device clock    %+.0f ppm  (+-%.0f over %d "
                          . "uninterrupted run%s)", $s->{rate}{ppm},
                          $s->{rate}{sd}, $s->{rate}{segments},
                          $s->{rate}{segments} == 1 ? '' : 's');
    }
    else
    {
        push @o, "  device clock    not measurable: no long enough run of "
                 . "readings";
    }

    if (defined $s->{drift_mean})
    {
        push @o, sprintf ("  drift           mean %+.1f ms, sd %.1f ms, "
                          . "range %+.1f .. %+.1f ms",
                          $s->{drift_mean} / 1000, $s->{drift_sd} / 1000,
                          $s->{drift_min} / 1000, $s->{drift_max} / 1000);
    }

    my $n = $s->{now};
    push @o, sprintf ("  correction      applied %+.3f cents, integral "
                      . "%+.3f cents, %.1f%% of the window at the bound",
                      $n->{det} // 0, $n->{i} // 0, $s->{bound_pc});

    push @o, sprintf ("  timeline steps  %d  (%.1f/min, mean %.1f ms, "
                      . "%d of %d alternating in sign)",
                      scalar @{$s->{steps}}, $s->{steps_per_min},
                      ($s->{step_mean} // 0) / 1000, $s->{step_alt} // 0,
                      scalar @{$s->{steps}} - 1)
        if @{$s->{steps}};

    for my $k (sort keys %{$s->{by_es} || {}})
    {
        push @o, sprintf ("                  %s: %d step%s, mean %.1f ms",
                          $k, $s->{by_es}{$k}{n},
                          $s->{by_es}{$k}{n} == 1 ? '' : 's',
                          $s->{by_es}{$k}{sum} / $s->{by_es}{$k}{n} / 1000);
    }

    if ($s->{latches})
    {
        push @o, sprintf ("  latched         %d times: %d by a step, %d "
                          . "declared, %d elsewhere", $s->{latches},
                          $s->{latch}{step} // 0, $s->{latch}{declared} // 0,
                          $s->{latch}{elsewhere} // 0);
        push @o, sprintf ("                  %d of %d blocks and %d of %d "
                          . "readings judged with both drift thresholds "
                          . "collapsed to zero", $s->{blocks_zeroed} // 0,
                          $s->{blocks} // 0, $s->{zeroed} // 0,
                          $s->{readings} // 0);
    }

    if (defined $s->{since_min})
    {
        push @o, sprintf ("  latch spacing   %d blocks at the closest, %d at "
                          . "the median; %d on the very next block, %d every "
                          . "other one", $s->{since_min}, $s->{since_med},
                          $s->{since_1}, $s->{since_2});
    }

    if ($s->{after})
    {
        push @o, "  and then        "
                 . join (', ', map { "$s->{after}{$_} $AFTER{$_}" }
                               grep { $s->{after}{$_} }
                               qw(silence jump settling excluded sync))
                 . " on the reading it put in front of the controller";
    }

    my @other = grep { $s->{counts}{$_} }
                qw(excluded silence jump runout droplate dropearly flush
                   restart untimed uncorrected);
    push @o, "  other           "
             . join (', ', map { "$s->{counts}{$_} $_" } @other)
        if @other;

    return @o;
}

# The reading each latch put in front of the controller, keyed by the latch's
# own timestamp. With both thresholds at zero, whatever this says is what the
# latch cost: a silence inserted, a jump taken, or nothing heard at all.
sub next_readings
{
    my ($rows) = @_;
    my (%out, $latch);

    for my $r (@$rows)
    {
        if (defined $r->{drift} && $latch)
        {
            $out{$latch->{t}} = $r;
            undef $latch;
        }
        $latch = $r if defined $r->{since};
    }
    return \%out;
}

sub report_text
{
    my ($hdr, $all) = @_;
    my $rows = window ($all);
    my $s = stats ($hdr, $rows);
    my ($t0, $t1) = ($rows->[0]{t}, $rows->[-1]{t});

    my @o;
    push @o, "VLC audio drift correction - $file";
    push @o, '';
    push @o, fmt_stats ($hdr, $s);
    push @o, '';

    my @read = grep { defined $_->{drift} } @$rows;

    push @o, "  drift: how far ahead of itself the output is running (ms, "
             . "'o'; '0' where a latched discontinuity had already collapsed "
             . "both thresholds to zero)";
    push @o, plot_text (
        t0 => $t0, t1 => $t1, height => $opt{height},
        rules => [ 0, $hdr->{jump_us} / 1000, $hdr->{silence_us} / 1000 ],
        series => [
            { ch => 'o', pts => [ map { [ $_->{t}, $_->{drift} / 1000 ] }
                                  @read ] },
            { ch => '0', pts => [ map { [ $_->{t}, $_->{drift} / 1000 ] }
                                  grep { $_->{disc} } @read ] },
        ]);
    push @o, '';

    push @o, "  device delay: what the output says it still holds (ms, '-')";
    push @o, plot_text (
        t0 => $t0, t1 => $t1, height => int ($opt{height} / 2) || 4,
        rules => [ 0 ],
        series => [
            { ch => '-', pts => [ map { [ $_->{t}, $_->{delay} / 1000 ] }
                                  grep { defined $_->{delay} } @read ] },
        ]);
    push @o, '';

    if (@{$s->{steps}})
    {
        push @o, "  timeline steps: what upstream handed over, signed (ms, "
                 . "'S'). Each one latches the discontinuity.";
        push @o, plot_text (
            t0 => $t0, t1 => $t1, height => int ($opt{height} / 2) || 4,
            rules => [ 0 ],
            series => [
                { ch => 'S', pts => [ map { [ $_->{t}, $_->{extra} / 1000 ] }
                                      @{$s->{steps}} ] },
            ]);
        push @o, '';
    }

    my @cmd = grep { defined $_->{cmd} } @$rows;
    my $sim;

    if (%sim || $opt{chase})
    {
        add_noise ($rows) if $opt{loop} eq 'closed';
        $sim = simulate ($hdr, $rows, \%sim, $opt{loop} eq 'closed');
    }

    push @o, "  cents: proportional 'p', integral 'I', commanded 'c', "
             . "applied '#'" . ($sim ? ", simulated '*'" : '');
    push @o, plot_text (
        t0 => $t0, t1 => $t1, height => $opt{height},
        rules => [ 0, $hdr->{max_cents}, -$hdr->{max_cents} ],
        fmt => sub { sprintf '%8.3f', $_[0] },
        series => [
            { ch => 'p', pts => [ map { [ $_->{t}, $_->{p} ] } @cmd ] },
            { ch => 'I', pts => [ map { [ $_->{t}, $_->{i} ] }
                                  grep { defined $_->{i} } @$rows ] },
            { ch => 'c', pts => [ map { [ $_->{t}, $_->{cmd} ] } @cmd ] },
            { ch => '#', pts => [ map { [ $_->{t}, $_->{det} ] }
                                  grep { defined $_->{det} } @$rows ] },
            ($sim ? { ch => '*', pts => [ map { [ $_->{t}, $_->{det} ] }
                                          grep { defined $_->{det} } @$sim ] }
                  : ()),
        ]);

    push @o, rail_text ($rows, $t0, $t1);
    push @o, (' ' x 10)
             . "S step  D declared  z silence  J jump  R run-out  F flush  "
             . "x excluded";
    push @o, sprintf ("%8.2f  %s%8.2f s", $t0 / 1e6,
                      ' ' x ($opt{width} - 10), $t1 / 1e6);

    if (@{$s->{steps}} && $opt{events})
    {
        push @o, '';
        push @o, "  steps, as they were recognised. The block before each "
                 . "ended at its pts less the step; the arrow is what the "
                 . "latch then cost.";
        my $n = 0;
        my $next = next_readings ($rows);
        for my $r (@{$s->{steps}})
        {
            last if ++$n > $opt{events};
            my $a = $next->{$r->{t}};
            push @o, sprintf ("    %8.3f s  %-4s %-2d %s %8.3f ms after "
                              . "%3d block%s  pts %.6f  -> %s",
                              $r->{t} / 1e6, $r->{codec} // '?', $r->{es} // 0,
                              ($r->{extra} // 0) > 0 ? 'hole   ' : 'overlap',
                              abs ($r->{extra} // 0) / 1000, $r->{since} // 0,
                              ($r->{since} // 0) == 1 ? ' ' : 's',
                              ($r->{pts} // 0) / 1e6,
                              $a ? sprintf ('%s at %+.1f ms', $a->{ev},
                                            ($a->{drift} // 0) / 1000)
                                 : 'no reading');
        }
        push @o, sprintf ("    ... and %d more", @{$s->{steps}} - $opt{events})
            if @{$s->{steps}} > $opt{events};
    }

    if ($sim)
    {
        my $ss = stats ($hdr, [ grep { defined $_->{det} } @$sim ]);
        push @o, '';
        push @o, sprintf ("  what-if (%s loop%s): kp %.4g  ki %.4g  "
                          . "slew %.4g  bound +-%.4g", $opt{loop},
                          $opt{chase} ? ', steps chased rather than recognised'
                                      : '',
                          $sim{kp} // $hdr->{kp}, $sim{ki} // $hdr->{ki},
                          $sim{slew} // $hdr->{slew},
                          $sim{max_cents} // $hdr->{max_cents});

        my @d = grep { defined $_->{det} } @$sim;
        my ($mn, $mx) = ($d[0]{det} // 0, $d[0]{det} // 0);
        for (@d) { $mn = $_->{det} if $_->{det} < $mn;
                   $mx = $_->{det} if $_->{det} > $mx }

        push @o, sprintf ("    applied %+.3f .. %+.3f cents, %+.3f at the "
                          . "end, integral %+.3f, %.1f%% at the bound",
                          $mn, $mx, $d[-1]{det} // 0,
                          $d[-1]{i} // 0, $ss->{bound_pc});
    }

    return join ("\n", @o) . "\n";
}

#
#   SVG
#
# Panels stacked on one time axis, never two scales on one pair of axes.

my %COL = (
    surface => '#fcfcfb', ink => '#0b0b0b', ink2 => '#52514e',
    grid    => '#dcdcd8',
    drift   => '#2a78d6',   # 1 blue
    cmd     => '#eb6834',   # 2 orange
    det     => '#1baf7a',   # 3 aqua
    integ   => '#eda100',   # 4 yellow
    prop    => '#e87ba4',   # 5 magenta
    delay   => '#008300',   # 6 green
    alarm   => '#e34948',   # 8 red
);

sub esc { my $s = shift // ''; $s =~ s/&/&amp;/g; $s =~ s/</&lt;/g;
          $s =~ s/>/&gt;/g; return $s }

sub svg_panel
{
    my (%a) = @_;
    my ($x, $y, $w, $h) = @a{qw(x y w h)};
    my ($t0, $t1) = ($a{t0}, $a{t1});
    $t1 = $t0 + 1 if $t1 <= $t0;

    my ($lo, $hi, $clipped) = series_bounds ($a{series});
    my $sx = sub { $x + ($_[0] - $t0) / ($t1 - $t0) * $w };
    my $sy = sub {
        my $v = $_[0] > $hi ? $hi : $_[0] < $lo ? $lo : $_[0];
        return $y + ($hi - $v) / ($hi - $lo) * $h;
    };

    my @o;
    push @o, sprintf ('<rect x="%.1f" y="%.1f" width="%.1f" height="%.1f" '
                      . 'fill="none" stroke="%s"/>', $x, $y, $w, $h,
                      $COL{grid});
    push @o, sprintf ('<text x="%.1f" y="%.1f" font-size="13" '
                      . 'font-weight="600" fill="%s">%s</text>',
                      $x, $y - 28, $COL{ink}, esc ($a{title}));

    my @note;
    push @note, 'tails drawn against the edge' if $clipped;
    for my $r (@{$a{rules} || []})
    {
        push @note, sprintf ('%s at %+.4g off scale', $r->[1], $r->[0])
            if $r->[0] != 0 && ($r->[0] < $lo || $r->[0] > $hi);
    }
    push @o, sprintf ('<text x="%.1f" y="%.1f" font-size="10" fill="%s" '
                      . 'text-anchor="end">%s</text>', $x + $w, $y - 28,
                      $COL{ink2}, esc (join ('; ', @note)))
        if @note;

    # Value rules: zero and whatever bound applies.
    for my $r (@{$a{rules} || []})
    {
        next if $r->[0] < $lo || $r->[0] > $hi;
        my $yy = $sy->($r->[0]);
        push @o, sprintf ('<line x1="%.1f" y1="%.1f" x2="%.1f" y2="%.1f" '
                          . 'stroke="%s" stroke-width="1" %s/>',
                          $x, $yy, $x + $w, $yy, $r->[2] || $COL{grid},
                          $r->[2] ? 'stroke-dasharray="4 3"' : '');
        push @o, sprintf ('<text x="%.1f" y="%.1f" font-size="10" fill="%s" '
                          . 'text-anchor="end">%s</text>',
                          $x - 6, $yy + 3, $COL{ink2}, esc ($r->[1]));
    }

    my $fmt = $a{fmt} || sub { sprintf '%.1f', $_[0] };
    for my $v ($lo, ($lo + $hi) / 2, $hi)
    {
        $v = 0 if abs ($v) < 1e-9;
        push @o, sprintf ('<text x="%.1f" y="%.1f" font-size="10" fill="%s" '
                          . 'text-anchor="end">%s</text>',
                          $x - 6, $sy->($v) + 3, $COL{ink2},
                          esc ($fmt->($v)));
    }

    my @label;
    for my $s (@{$a{series}})
    {
        next unless @{$s->{pts}};

        # A step and a reading singled out are events, not a signal: joining
        # them up would draw a slope between things that never moved.
        if ($s->{mark})
        {
            for my $pt (@{$s->{pts}})
            {
                my ($xx, $yy) = ($sx->($pt->[0]), $sy->($pt->[1]));
                push @o, sprintf ('<line x1="%.1f" y1="%.1f" x2="%.1f" '
                                  . 'y2="%.1f" stroke="%s" '
                                  . 'stroke-width="1.4"/>',
                                  $xx, $sy->(0), $xx, $yy, $s->{col})
                    if $s->{mark} eq 'stem';
                push @o, sprintf ('<circle cx="%.1f" cy="%.1f" r="%s" '
                                  . 'fill="%s"/>', $xx, $yy,
                                  $s->{mark} eq 'stem' ? '2.4' : '2.8',
                                  $s->{col});
            }
            push @label, { y => $sy->($s->{pts}[-1][1]), col => $s->{col},
                           text => $s->{label} };
            next;
        }

        my $d = '';
        my $pen = 0;
        for my $pt (@{$s->{pts}})
        {
            $d .= sprintf ('%s%.1f %.1f ', $pen++ ? 'L' : 'M',
                           $sx->($pt->[0]), $sy->($pt->[1]));
        }
        push @o, sprintf ('<path d="%s" fill="none" stroke="%s" '
                          . 'stroke-width="%s" stroke-linejoin="round" '
                          . 'stroke-linecap="round" %s/>',
                          $d, $s->{col}, $s->{wide} ? '2.2' : '1.4',
                          $s->{dash} ? 'stroke-dasharray="5 3"' : '');

        push @label, { y => $sy->($s->{pts}[-1][1]), col => $s->{col},
                       text => $s->{label} };
    }

    # A label beside each line where it ends, so identity is never colour
    # alone - pushed apart where two lines finish on top of each other, which
    # a settled correction and the command behind it always do.
    @label = sort { $a->{y} <=> $b->{y} } @label;
    for my $n (1 .. $#label)
    {
        $label[$n]{y} = $label[$n - 1]{y} + 12
            if $label[$n]{y} - $label[$n - 1]{y} < 12;
    }
    for my $l (@label)
    {
        push @o, sprintf ('<text x="%.1f" y="%.1f" font-size="10" '
                          . 'font-weight="600" fill="%s">%s</text>',
                          $x + $w + 6, $l->{y} + 3, $l->{col},
                          esc ($l->{text}));
    }

    # And a legend as well, because a line that runs off the top of the panel
    # has no end to be labelled at.
    my $lx = $x;
    for my $s (@{$a{series}})
    {
        if ($s->{mark})
        {
            push @o, sprintf ('<circle cx="%.1f" cy="%.1f" r="2.8" '
                              . 'fill="%s"/>', $lx + 7, $y - 11, $s->{col});
        }
        else
        {
            push @o, sprintf ('<line x1="%.1f" y1="%.1f" x2="%.1f" y2="%.1f" '
                              . 'stroke="%s" stroke-width="2.2" %s/>',
                              $lx, $y - 11, $lx + 14, $y - 11, $s->{col},
                              $s->{dash} ? 'stroke-dasharray="4 2"' : '');
        }
        push @o, sprintf ('<text x="%.1f" y="%.1f" font-size="10" '
                          . 'fill="%s">%s</text>', $lx + 18, $y - 8,
                          $COL{ink2}, esc ($s->{label}));
        $lx += 28 + 6.2 * length ($s->{label});
    }

    # Where something happened to the timeline.
    for my $e (@{$a{events} || []})
    {
        my $xx = $sx->($e->{t});
        push @o, sprintf ('<line x1="%.1f" y1="%.1f" x2="%.1f" y2="%.1f" '
                          . 'stroke="%s" stroke-width="1" '
                          . 'stroke-dasharray="2 3" opacity="0.7"/>',
                          $xx, $y, $xx, $y + $h, $e->{col});
    }
    return @o;
}

sub report_svg
{
    my ($hdr, $all) = @_;
    my $rows = window ($all);
    my $s = stats ($hdr, $rows);
    my ($t0, $t1) = ($rows->[0]{t}, $rows->[-1]{t});

    my $W = $opt{width} > 400 ? $opt{width} : 1100;
    my ($L, $R) = (86, 118);
    my $pw = $W - $L - $R;
    my $ph = 168;
    my $gap = 62;

    my @read = grep { defined $_->{drift} } @$rows;
    my @cmd = grep { defined $_->{cmd} } @$rows;
    my @ev = map { { t => $_->{t},
                     col => $_->{ev} eq 'step' ? $COL{alarm} : $COL{ink2} } }
             grep { $MARK{$_->{ev}} && $_->{ev} ne 'excluded' } @$rows;

    my $sim;
    if (%sim || $opt{chase})
    {
        add_noise ($rows) if $opt{loop} eq 'closed';
        $sim = simulate ($hdr, $rows, \%sim, $opt{loop} eq 'closed');
    }

    my @text = fmt_stats ($hdr, $s);
    my $panels = @{$s->{steps}} ? 4 : 3;
    my $H = 84 + $panels * ($ph + $gap) + 66 + 14 * @text + 24;

    my @o;
    push @o, sprintf ('<svg xmlns="http://www.w3.org/2000/svg" width="%d" '
                      . 'height="%d" viewBox="0 0 %d %d" '
                      . 'font-family="system-ui,-apple-system,Segoe UI,'
                      . 'Helvetica,Arial,sans-serif">', $W, $H, $W, $H);
    push @o, sprintf ('<rect width="%d" height="%d" fill="%s"/>', $W, $H,
                      $COL{surface});
    push @o, sprintf ('<text x="%d" y="30" font-size="17" font-weight="700" '
                      . 'fill="%s">What the audio drift correction did</text>',
                      $L, $COL{ink});
    push @o, sprintf ('<text x="%d" y="50" font-size="11" fill="%s">%s</text>',
                      $L, $COL{ink2}, esc ($file));

    my $y = 112;

    push @o, svg_panel (
        x => $L, y => $y, w => $pw, h => $ph, t0 => $t0, t1 => $t1,
        title => 'Drift: how far ahead of its own timeline the output is '
                 . 'running (ms)',
        direct => 1, events => \@ev,
        rules => [ [ 0, '0', '' ],
                   [ ($hdr->{jump_us} // 180000) / 1000, 'jump', $COL{alarm} ],
                   [ ($hdr->{silence_us} // -120000) / 1000, 'silence',
                     $COL{alarm} ] ],
        series => [
            { label => 'drift', col => $COL{drift}, wide => 1,
              pts => [ map { [ $_->{t}, $_->{drift} / 1000 ] } @read ] },
            { label => 'thresholds zero', col => $COL{alarm}, mark => 'dot',
              pts => [ map { [ $_->{t}, $_->{drift} / 1000 ] }
                       grep { $_->{disc} } @read ] },
        ]);
    $y += $ph + $gap;

    push @o, svg_panel (
        x => $L, y => $y, w => $pw, h => $ph, t0 => $t0, t1 => $t1,
        title => 'Device delay: what the output says it is still holding (ms)',
        direct => 1, events => \@ev,
        rules => [ [ 0, '0', '' ] ],
        series => [
            { label => 'delay', col => $COL{delay}, wide => 1,
              pts => [ map { [ $_->{t}, $_->{delay} / 1000 ] }
                       grep { defined $_->{delay} } @read ] },
        ]);
    $y += $ph + $gap;

    if (@{$s->{steps}})
    {
        push @o, svg_panel (
            x => $L, y => $y, w => $pw, h => $ph, t0 => $t0, t1 => $t1,
            title => 'Timeline steps: what upstream handed over, signed (ms)'
                     . ' - each one latches the discontinuity',
            direct => 1,
            rules => [ [ 0, '0', '' ] ],
            series => [
                { label => 'step', col => $COL{alarm}, mark => 'stem',
                  pts => [ map { [ $_->{t}, $_->{extra} / 1000 ] }
                           @{$s->{steps}} ] },
            ]);
        $y += $ph + $gap;
    }

    push @o, svg_panel (
        x => $L, y => $y, w => $pw, h => $ph, t0 => $t0, t1 => $t1,
        title => 'The correction, in cents: what was asked for and what came '
                 . 'out',
        direct => 1, events => \@ev,
        fmt => sub { sprintf '%.2f', $_[0] },
        rules => [ [ 0, '0', '' ],
                   [ $hdr->{max_cents}, 'bound', $COL{alarm} ],
                   [ -$hdr->{max_cents}, 'bound', $COL{alarm} ] ],
        series => [
            { label => 'P', col => $COL{prop},
              pts => [ map { [ $_->{t}, $_->{p} ] } @cmd ] },
            { label => 'I', col => $COL{integ},
              pts => [ map { [ $_->{t}, $_->{i} ] }
                       grep { defined $_->{i} } @$rows ] },
            { label => 'commanded', col => $COL{cmd},
              pts => [ map { [ $_->{t}, $_->{cmd} ] } @cmd ] },
            { label => 'applied', col => $COL{det}, wide => 1,
              pts => [ map { [ $_->{t}, $_->{det} ] }
                       grep { defined $_->{det} } @$rows ] },
            ($sim ? { label => 'what-if', col => $COL{ink2}, dash => 1,
                      pts => [ map { [ $_->{t}, $_->{det} ] }
                               grep { defined $_->{det} } @$sim ] } : ()),
        ]);
    $y += $ph + 46;

    # The event rail, named rather than coloured.
    push @o, sprintf ('<text x="%d" y="%.1f" font-size="13" '
                      . 'font-weight="600" fill="%s">Events</text>',
                      $L, $y, $COL{ink});
    $y += 14;
    for my $r (@$rows)
    {
        my $m = $MARK{$r->{ev}} or next;
        next if $r->{ev} eq 'excluded';
        my $xx = $L + ($r->{t} - $t0) / (($t1 - $t0) || 1) * $pw;
        my $c = $r->{ev} eq 'step' ? $COL{alarm} : $COL{ink2};
        push @o, sprintf ('<line x1="%.1f" y1="%.1f" x2="%.1f" y2="%.1f" '
                          . 'stroke="%s" stroke-width="1.6"/>',
                          $xx, $y, $xx, $y + 12, $c);
        push @o, sprintf ('<text x="%.1f" y="%.1f" font-size="9" fill="%s" '
                          . 'text-anchor="middle">%s</text>',
                          $xx, $y + 23, $c, $m);
    }
    $y += 42;

    push @o, sprintf ('<text x="%d" y="%.1f" font-size="10" fill="%s">'
                      . 'S step  D declared  z silence  J jump  R run-out  '
                      . 'F flush  X restart</text>', $L, $y, $COL{ink2});
    $y += 22;

    for my $line (@text)
    {
        push @o, sprintf ('<text x="%d" y="%.1f" font-size="11" '
                          . 'font-family="ui-monospace,SFMono-Regular,'
                          . 'Menlo,Consolas,monospace" fill="%s">%s</text>',
                          $L - 10, $y, $COL{ink}, esc ($line));
        $y += 14;
    }

    push @o, '</svg>';
    return join ("\n", @o) . "\n";
}

#
#   The interactive view
#

my @PARAM = (
    { key => 'kp',        name => 'aout-drift-gain',          min => 0,
      max => 1000, step => 5,    unit => 'cents/s' },
    { key => 'ki',        name => 'aout-drift-integral-gain', min => 0,
      max => 100,  step => 0.25, unit => 'cents/s2' },
    { key => 'slew',      name => 'aout-drift-slew',          min => 0,
      max => 60,   step => 0.1,  unit => 's' },
    { key => 'max_cents', name => 'aout-max-resampling',      min => 0,
      max => 200,  step => 0.5,  unit => 'cents' },
);

sub bar
{
    my ($v, $min, $max, $w) = @_;
    my $n = $max > $min ? int (($v - $min) / ($max - $min) * $w + 0.5) : 0;
    $n = 0 if $n < 0; $n = $w if $n > $w;
    return '[' . ('=' x $n) . ('.' x ($w - $n)) . ']';
}

sub tui
{
    my ($hdr, $all) = @_;
    my %cur = map { $_->{key} => $hdr->{$_->{key}} } @PARAM;
    my $sel = 0;
    my $closed = $opt{loop} eq 'closed';

    system ('stty raw -echo 2>/dev/null');
    $SIG{INT} = $SIG{TERM} = sub { system ('stty sane 2>/dev/null'); exit 0 };
    $| = 1;

    my $quit = 0;
    while (!$quit)
    {
        my $rows = window ($all);
        my $s = stats ($hdr, $rows);
        add_noise ($rows) if $closed;
        my $sim = simulate ($hdr, $rows, \%cur, $closed);
        my $ss = stats ($hdr, [ grep { defined $_->{det} } @$sim ]);
        my ($t0, $t1) = ($rows->[0]{t}, $rows->[-1]{t});

        my @o;
        push @o, "VLC audio drift correction   $file"
                 . ($opt{follow} ? '   (following)' : '');
        push @o, '';
        push @o, fmt_stats ($hdr, $s);
        push @o, '';
        push @o, "  cents: integral 'I', commanded 'c', applied '#', "
                 . "what-if '*'";
        push @o, plot_text (
            t0 => $t0, t1 => $t1, height => $opt{height},
            rules => [ 0, $cur{max_cents}, -$cur{max_cents} ],
            fmt => sub { sprintf '%8.3f', $_[0] },
            series => [
                { ch => 'I', pts => [ map { [ $_->{t}, $_->{i} ] }
                                      grep { defined $_->{i} } @$rows ] },
                { ch => 'c', pts => [ map { [ $_->{t}, $_->{cmd} ] }
                                      grep { defined $_->{cmd} } @$rows ] },
                { ch => '#', pts => [ map { [ $_->{t}, $_->{det} ] }
                                      grep { defined $_->{det} } @$rows ] },
                { ch => '*', pts => [ map { [ $_->{t}, $_->{det} ] }
                                      grep { defined $_->{det} } @$sim ] },
            ]);
        push @o, rail_text ($rows, $t0, $t1);
        push @o, '';
        my @d = grep { defined $_->{det} } @$sim;
        my ($mn, $mx) = ($d[0]{det} // 0, $d[0]{det} // 0);
        for (@d) { $mn = $_->{det} if $_->{det} < $mn;
                   $mx = $_->{det} if $_->{det} > $mx }

        push @o, sprintf ("  what-if, %s loop%s:  applied %+.3f .. %+.3f "
                          . "cents, %+.3f at the end, integral %+.3f, "
                          . "%.1f%% at the bound",
                          $closed ? 'closed' : 'open',
                          $opt{chase} ? ', steps chased' : '',
                          $mn, $mx, $d[-1]{det} // 0, $d[-1]{i} // 0,
                          $ss->{bound_pc});
        push @o, '';

        for my $n (0 .. $#PARAM)
        {
            my $p = $PARAM[$n];
            push @o, sprintf ("  %s %-26s %10.4g %-9s %s",
                              $n == $sel ? '>' : ' ', $p->{name},
                              $cur{$p->{key}}, $p->{unit},
                              bar ($cur{$p->{key}}, $p->{min}, $p->{max}, 34));
        }
        push @o, '';
        push @o, "  up/down or j/k select   left/right or -/+ adjust   "
                 . "L open/closed   r reset   q quit";

        print "\e[H\e[2J" . join ("\r\n", @o) . "\r\n";

        # Wait for a key, but not for ever if the trace is still growing.
        my $rin = '';
        vec ($rin, fileno (STDIN), 1) = 1;
        my $ready = select (my $rout = $rin, undef, undef,
                            $opt{follow} ? 0.7 : undef);

        if ($ready)
        {
            my $c;
            sysread (STDIN, $c, 1);

            if ($c eq "\e")
            {
                my $rest = '';
                sysread (STDIN, $rest, 2);
                $c = $rest eq '[A' ? 'k' : $rest eq '[B' ? 'j'
                   : $rest eq '[D' ? '-' : $rest eq '[C' ? '+' : '';
            }

            my $p = $PARAM[$sel];
            if    ($c eq 'q' || $c eq "\003") { $quit = 1 }
            elsif ($c eq 'j') { $sel = ($sel + 1) % @PARAM }
            elsif ($c eq 'k') { $sel = ($sel - 1) % @PARAM }
            elsif ($c eq '+' || $c eq '=')
            {
                $cur{$p->{key}} += $p->{step};
                $cur{$p->{key}} = $p->{max} if $cur{$p->{key}} > $p->{max};
            }
            elsif ($c eq '-' || $c eq '_')
            {
                $cur{$p->{key}} -= $p->{step};
                $cur{$p->{key}} = $p->{min} if $cur{$p->{key}} < $p->{min};
            }
            elsif ($c eq 'L') { $closed = !$closed }
            elsif ($c eq 'r')
            {
                %cur = map { $_->{key} => $hdr->{$_->{key}} } @PARAM;
            }
        }

        if ($opt{follow})
        {
            my ($h2, $r2) = eval { read_trace ($file) };
            ($hdr, $all) = ($h2, $r2) if $r2 && @$r2 >= @$all;
        }
    }
    system ('stty sane 2>/dev/null');
    print "\n";
}

#
#   Go
#

my ($hdr, $rows) = read_trace ($file);

if ($opt{mode} eq 'tui')
{
    tui ($hdr, $rows);
}
elsif ($opt{mode} eq 'svg')
{
    my $svg = report_svg ($hdr, $rows);
    if (defined $opt{out} && $opt{out} ne '-')
    {
        open (my $fh, '>', $opt{out})
            or die "$SELF: cannot write $opt{out}: $!\n";
        print $fh $svg;
        close $fh;
        print STDERR "$SELF: wrote $opt{out}\n";
    }
    else { print $svg }
}
elsif ($opt{mode} eq 'stats')
{
    my $w = window ($rows);
    print join ("\n", fmt_stats ($hdr, stats ($hdr, $w))), "\n";
}
else
{
    do {
        print "\e[H\e[2J" if $opt{follow};
        print report_text ($hdr, $rows);
        if ($opt{follow})
        {
            select (undef, undef, undef, 0.7);
            ($hdr, $rows) = read_trace ($file);
        }
    } while ($opt{follow});
}
