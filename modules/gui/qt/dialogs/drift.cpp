/*****************************************************************************
 * drift.cpp : what the audio drift correction is doing, while it does it
 ****************************************************************************
 * Copyright (C) 2026 the VideoLAN team
 * $Id$
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

/**
 * The correction is a feedback loop whose whole state lives for one call and
 * is then gone. Tuning it by ear means moving a gain, waiting for the next
 * stream, and guessing from the pitch what happened - so this shows the
 * readings as they are taken, and moves the settings under the running loop.
 *
 * Nothing here is on the audio path. The output writes its readings into a
 * ring and never waits on this; the ring is counted, so a stream ending while
 * the dialog is open leaves it with a ring that has stopped filling rather
 * than with a dangling output. Closed, the dialog disarms the ring and the
 * output is back to a load and a branch per reading.
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QSlider>
#include <QTimer>
#include <QVBoxLayout>

#include <cmath>
#include <cstring>

#include <vlc_common.h>

#include "dialogs/drift.hpp"
#include "input_manager.hpp"

const double DriftDialog::HISTORY = 150.;

/** How often the ring is drained. Blocks arrive an order of magnitude faster,
 * so this decides how smoothly the plot moves and nothing else. */
static const int DRIFT_POLL_MS = 100;

/** A detune of c cents, as the rate offset it is: what is actually wanted of
 * the device's clock, and the unit a clock is specified in. */
static double centsToPpm( double cents )
{
    return ( exp2( cents / 1200. ) - 1. ) * 1e6;
}

static bool isDeclared( const aout_drift_point &p )
{
    return !qstrcmp( p.event, "declared" );
}

static bool isStep( const aout_drift_point &p )
{
    return !qstrcmp( p.event, "step" );
}

/** Blue for a discontinuity the stream declared, red for one nobody declared
 * and the dates gave away, grey for the rest - a flush, a jump, a silence. */
static QColor latchColour( const aout_drift_point &p )
{
    if( isDeclared( p ) ) return QColor( 90, 140, 255 );
    if( isStep( p ) )     return QColor( 235, 70, 70 );
    return QColor( 150, 150, 150 );
}

/*****************************************************************************
 * The plot
 *****************************************************************************/

DriftPlot::DriftPlot( const QVector<aout_drift_point> &p,
                      const aout_drift_config &c, QWidget *parent )
    : QWidget( parent ), points( p ), config( c ), window( 30. )
{
    setMinimumHeight( 330 );
    setSizePolicy( QSizePolicy::Expanding, QSizePolicy::Expanding );
}

namespace {

/** One pane of the plot: a value range mapped onto a band of the widget. */
struct Pane
{
    QRectF box;
    double lo, hi;

    double y( double v ) const
    {
        if( hi <= lo )
            return box.center().y();
        return box.bottom() - ( v - lo ) / ( hi - lo ) * box.height();
    }
};

/** Rounds a range out to something with labels worth reading, and never to
 * nothing: a flat line belongs in the middle of a pane, not on its edge. */
static void fit( Pane &pane, double lo, double hi, double least )
{
    if( !( hi > lo ) ) { hi = lo + least; lo -= least; }

    const double pad = ( hi - lo ) * 0.15;
    pane.lo = lo - pad;
    pane.hi = hi + pad;
    if( pane.hi - pane.lo < least )
    {
        const double mid = ( pane.hi + pane.lo ) / 2.;
        pane.lo = mid - least / 2.;
        pane.hi = mid + least / 2.;
    }
}

static void plotLine( QPainter &p, const Pane &pane, double t0, double span,
                      const QVector<aout_drift_point> &points,
                      double (*value)( const aout_drift_point & ),
                      bool (*has)( const aout_drift_point & ),
                      const QColor &colour, double width, Qt::PenStyle style )
{
    QPainterPath path;
    bool started = false;

    for( int i = 0; i < points.size(); i++ )
    {
        const aout_drift_point &pt = points.at( i );
        if( has != NULL && !has( pt ) )
            continue;

        const double t = ( pt.date - t0 ) / (double)CLOCK_FREQ;
        const QPointF at( pane.box.left() + t / span * pane.box.width(),
                          pane.y( value( pt ) ) );

        if( started ) path.lineTo( at );
        else { path.moveTo( at ); started = true; }
    }

    if( !started )
        return;

    p.save();
    p.setClipRect( pane.box );
    p.setPen( QPen( colour, width, style ) );
    p.drawPath( path );
    p.restore();
}

} /* namespace */

static double vDelay( const aout_drift_point &p ) { return p.delay / 1000.; }
static double vDrift( const aout_drift_point &p ) { return p.drift / 1000.; }
static double vCommanded( const aout_drift_point &p ) { return p.commanded; }
static double vTarget( const aout_drift_point &p ) { return p.target; }
static double vDetune( const aout_drift_point &p ) { return p.detune; }
static double vIntegral( const aout_drift_point &p ) { return p.integral; }
static bool hasReading( const aout_drift_point &p ) { return p.reading; }
static bool hasCommand( const aout_drift_point &p ) { return p.command; }

void DriftPlot::paintEvent( QPaintEvent * )
{
    QPainter p( this );
    p.setRenderHint( QPainter::Antialiasing );

    const QColor ink = palette().color( QPalette::Text );
    const QColor faint( ink.red(), ink.green(), ink.blue(), 60 );

    p.fillRect( rect(), palette().color( QPalette::Base ) );

    if( points.isEmpty() )
    {
        p.setPen( faint );
        p.drawText( rect(), Qt::AlignCenter,
                    qtr( "Nothing has been measured yet." ) );
        return;
    }

    const int left = 62, right = 8, top = 14, gap = 22;
    const double band = ( height() - top - 10 - 2 * gap ) / 3.;

    Pane latency, drift, cents;
    latency.box = QRectF( left, top, width() - left - right, band );
    drift.box = QRectF( left, top + band + gap, width() - left - right, band );
    cents.box = QRectF( left, top + 2 * ( band + gap ), width() - left - right,
                        band );

    /* The window ends where the last reading is, not at the present moment:
     * a stream that has stopped should leave its tail where it is rather than
     * slide off the left while nothing replaces it. */
    const vlc_tick_t last = points.last().date;
    const vlc_tick_t t0 = last - (vlc_tick_t)( window * CLOCK_FREQ );

    double dlo = 0., dhi = 0., rlo = 0., rhi = 0., clo = 0., chi = 0.;
    bool anyReading = false, anyCommand = false;

    for( int i = 0; i < points.size(); i++ )
    {
        const aout_drift_point &pt = points.at( i );
        if( pt.date < t0 )
            continue;

        if( pt.reading )
        {
            const double d = vDelay( pt ), r = vDrift( pt );
            if( !anyReading ) { dlo = dhi = d; rlo = rhi = r; anyReading = true; }
            dlo = qMin( dlo, d ); dhi = qMax( dhi, d );
            rlo = qMin( rlo, r ); rhi = qMax( rhi, r );
        }

        const double vals[] = { pt.detune, pt.integral,
                                pt.command ? pt.commanded : 0.f };
        for( size_t j = 0; j < sizeof(vals) / sizeof(vals[0]); j++ )
        {
            clo = qMin( clo, vals[j] ); chi = qMax( chi, vals[j] );
            anyCommand = true;
        }
    }

    fit( latency, anyReading ? dlo : 0., anyReading ? dhi : 0., 2. );
    fit( drift, anyReading ? rlo : 0., anyReading ? rhi : 0., 4. );
    /* To the readings rather than to the bound: what the loop is doing is
     * usually a fraction of a cent, and a pane scaled to a bound of nine
     * would show that as a flat line. The bound is drawn where it falls and
     * said to be off the scale where it does not. */
    fit( cents, anyCommand ? clo : 0., anyCommand ? chi : 0., 0.1 );

    const bool boundShown = config.max_cents > 0.f
                         && config.max_cents <= cents.hi
                         && -config.max_cents >= cents.lo;

    const Pane *panes[] = { &latency, &drift, &cents };
    const QString titles[] = {
        qtr( "Device latency (ms)" ), qtr( "Drift (ms)" ),
        ( config.max_cents > 0.f && !boundShown )
            ? qtr( "Detune (cents), bound at %1 off the scale" )
                  .arg( config.max_cents, 0, 'f', 0 )
            : qtr( "Detune (cents)" ) };

    QFont small = font();
    small.setPointSize( qMax( 7, small.pointSize() - 2 ) );
    p.setFont( small );

    for( size_t i = 0; i < 3; i++ )
    {
        const Pane &pane = *panes[i];

        p.setPen( faint );
        p.drawRect( pane.box );

        if( pane.lo < 0. && pane.hi > 0. )
        {
            p.setPen( QPen( faint, 1, Qt::DashLine ) );
            p.drawLine( QPointF( pane.box.left(), pane.y( 0. ) ),
                        QPointF( pane.box.right(), pane.y( 0. ) ) );
        }

        p.setPen( ink );
        p.drawText( QRectF( 0, pane.box.top() - 2, left - 5, 14 ),
                    Qt::AlignRight | Qt::AlignVCenter,
                    QString::number( pane.hi, 'f', 1 ) );
        p.drawText( QRectF( 0, pane.box.bottom() - 12, left - 5, 14 ),
                    Qt::AlignRight | Qt::AlignVCenter,
                    QString::number( pane.lo, 'f', 1 ) );
        p.drawText( QPointF( pane.box.left() + 4, pane.box.top() - 3 ),
                    titles[i] );
    }

    const double span = window;

    /* The latches first, so that the lines are read over them rather than
     * the other way round. A correction that fires just after one has no
     * explanation in the drift alone. */
    p.save();
    p.setClipRect( QRectF( latency.box.left(), latency.box.top(),
                           latency.box.width(),
                           cents.box.bottom() - latency.box.top() ) );
    for( int i = 0; i < points.size(); i++ )
    {
        const aout_drift_point &pt = points.at( i );
        if( !pt.latch || pt.date < t0 )
            continue;

        const double t = ( pt.date - t0 ) / (double)CLOCK_FREQ;
        const double x = latency.box.left() + t / span * latency.box.width();
        QColor c = latchColour( pt );
        c.setAlpha( 130 );
        p.setPen( QPen( c, 1, Qt::DotLine ) );
        p.drawLine( QPointF( x, latency.box.top() ),
                    QPointF( x, cents.box.bottom() ) );
    }
    p.restore();

    plotLine( p, latency, t0, span, points, vDelay, hasReading,
              QColor( 80, 190, 200 ), 1.4, Qt::SolidLine );
    plotLine( p, drift, t0, span, points, vDrift, hasReading,
              QColor( 230, 170, 50 ), 1.4, Qt::SolidLine );

    if( boundShown )
    {
        p.setPen( QPen( QColor( 235, 70, 70, 120 ), 1, Qt::DashLine ) );
        p.drawLine( QPointF( cents.box.left(), cents.y( config.max_cents ) ),
                    QPointF( cents.box.right(), cents.y( config.max_cents ) ) );
        p.drawLine( QPointF( cents.box.left(), cents.y( -config.max_cents ) ),
                    QPointF( cents.box.right(), cents.y( -config.max_cents ) ) );
    }

    plotLine( p, cents, t0, span, points, vCommanded, hasCommand,
              QColor( 150, 150, 150 ), 1.0, Qt::DashLine );
    plotLine( p, cents, t0, span, points, vTarget, hasCommand,
              QColor( 200, 120, 220 ), 1.0, Qt::SolidLine );
    plotLine( p, cents, t0, span, points, vIntegral, NULL,
              QColor( 120, 200, 120 ), 1.2, Qt::SolidLine );
    plotLine( p, cents, t0, span, points, vDetune, NULL,
              QColor( 90, 140, 255 ), 2.0, Qt::SolidLine );
}

/*****************************************************************************
 * The dialog
 *****************************************************************************/

DriftDialog::DriftDialog( intf_thread_t *_p_intf )
    : QVLCFrame( _p_intf ), monitor( NULL ), seq( 0 ), dropped( false ),
      applying( false )
{
    memset( &config, 0, sizeof( config ) );

    setWindowTitle( qtr( "Audio Drift Correction" ) );
    setWindowRole( "vlc-drift" );

    QVBoxLayout *layout = new QVBoxLayout( this );

    state = new QLabel;
    state->setWordWrap( true );
    layout->addWidget( state );

    plot = new DriftPlot( points, config );
    layout->addWidget( plot, 1 );

    QLabel *key = new QLabel( qtr(
        "<b><font color='#5078c8'>Applied detune</font></b> &middot; "
        "<font color='#78c878'>integral term</font> &middot; "
        "<font color='#c878d8'>after the bound</font> &middot; "
        "<font color='#969696'>commanded</font> &nbsp;|&nbsp; latches: "
        "<font color='#5a8cff'>declared</font> &middot; "
        "<font color='#eb4646'>detected step</font> &middot; "
        "<font color='#969696'>flush, jump or silence</font>" ) );
    key->setWordWrap( true );
    layout->addWidget( key );

    QHBoxLayout *bar = new QHBoxLayout;
    bar->addWidget( new QLabel( qtr( "Averaged over" ) ) );
    windowBox = new QComboBox;
    windowBox->addItem( qtr( "5 s" ), 5 );
    windowBox->addItem( qtr( "10 s" ), 10 );
    windowBox->addItem( qtr( "30 s" ), 30 );
    windowBox->addItem( qtr( "60 s" ), 60 );
    windowBox->addItem( qtr( "120 s" ), 120 );
    windowBox->setCurrentIndex( 2 );
    bar->addWidget( windowBox );
    freeze = new QCheckBox( qtr( "&Freeze" ) );
    freeze->setToolTip( qtr( "Stop collecting, to read what is on screen. "
                             "The output keeps recording; what was missed is "
                             "picked up again on release." ) );
    bar->addWidget( freeze );
    bar->addStretch( 1 );
    layout->addLayout( bar );

    readout = new QLabel;
    readout->setTextFormat( Qt::RichText );
    layout->addWidget( readout );

    QGroupBox *settings = new QGroupBox( qtr( "Settings" ) );
    buildTunables( settings );
    layout->addWidget( settings );

    QDialogButtonBox *buttons = new QDialogButtonBox;
    buttons->addButton( new QPushButton( qtr( "&Close" ) ),
                        QDialogButtonBox::RejectRole );
    connect( buttons, &QDialogButtonBox::rejected, this, &DriftDialog::hide );
    layout->addWidget( buttons );

    connect( windowBox, QOverload<int>::of( &QComboBox::currentIndexChanged ),
             this, &DriftDialog::windowChanged );

    timer = new QTimer( this );
    timer->setInterval( DRIFT_POLL_MS );
    connect( timer, &QTimer::timeout, this, &DriftDialog::poll );
    timer->start();

    windowChanged( windowBox->currentIndex() );
    readTunables();
    poll();

    restoreWidgetPosition( "Drift", QSize( 720, 760 ) );
}

/**
 * Hidden is closed: the ring is disarmed, and the output goes back to paying
 * a load and a branch per reading for an instrument nobody is reading.
 */
void DriftDialog::showEvent( QShowEvent *event )
{
    if( monitor != NULL )
        aout_DriftMonitorArm( monitor, true );
    timer->start();
    poll();
    QVLCFrame::showEvent( event );
}

void DriftDialog::hideEvent( QHideEvent *event )
{
    timer->stop();
    if( monitor != NULL )
        aout_DriftMonitorArm( monitor, false );
    QVLCFrame::hideEvent( event );
}

DriftDialog::~DriftDialog()
{
    saveWidgetPosition( "Drift" );

    if( monitor != NULL )
    {
        aout_DriftMonitorArm( monitor, false );
        aout_DriftMonitorRelease( monitor );
    }
}

void DriftDialog::addTunable( QGridLayout *grid, int row, const char *name,
                              const QString &label, double min, double max,
                              double step, bool integer,
                              const QString &suffix, const QString &tip )
{
    Tunable t;
    t.name = name;
    t.step = step;
    t.integer = integer;

    QLabel *text = new QLabel( label );
    text->setToolTip( tip );

    t.slider = new QSlider( Qt::Horizontal );
    t.slider->setRange( (int)( min / step ), (int)( max / step ) );
    t.slider->setToolTip( tip );

    t.box = new QDoubleSpinBox;
    t.box->setDecimals( integer ? 0 : 3 );
    t.box->setRange( min, max );
    t.box->setSingleStep( step );
    t.box->setSuffix( suffix );
    t.box->setToolTip( tip );

    grid->addWidget( text, row, 0 );
    grid->addWidget( t.slider, row, 1 );
    grid->addWidget( t.box, row, 2 );

    const int index = tunables.size();
    tunables.append( t );

    connect( t.slider, &QSlider::valueChanged,
             [this, index]( int v ) {
                 applyTunable( index, v * tunables.at( index ).step ); } );
    connect( t.box, QOverload<double>::of( &QDoubleSpinBox::valueChanged ),
             [this, index]( double v ) { applyTunable( index, v ); } );
}

void DriftDialog::buildTunables( QWidget *parent )
{
    QGridLayout *grid = new QGridLayout( parent );
    grid->setColumnStretch( 1, 1 );

    addTunable( grid, 0, "aout-max-resampling", qtr( "Bound" ),
                0, AOUT_MAX_RESAMPLING_CENTS_MAX, 1, true, qtr( " cents" ),
                qtr( "How far the correction may detune the stream. Zero does "
                     "not mean a quiet run: it means every offset is spliced "
                     "out instead, by inserting silence or jumping ahead." ) );
    addTunable( grid, 1, "aout-drift-gain", qtr( "Gain" ),
                0, 1000, 1, false, qtr( " cents/s" ),
                qtr( "What one second of drift is worth in detune. Raising it "
                     "answers an offset sooner and follows the noise in the "
                     "reading further." ) );
    addTunable( grid, 2, "aout-drift-integral-gain", qtr( "Integral gain" ),
                0, 100, 0.05, false, qtr( " cents/s\xc2\xb2" ),
                qtr( "What answers a standing rate error, which the "
                     "proportional term cannot: it needs a drift to hold a "
                     "detune, and the integral is what holds one at no "
                     "drift." ) );
    addTunable( grid, 3, "aout-drift-slew", qtr( "Slew" ),
                0, 10, 0.05, false, qtr( " s" ),
                qtr( "The time constant over which the detune follows what "
                     "the controller asks for. It keeps the noise in the "
                     "reading out of the pitch, and has to sit well above "
                     "that noise and well below the drift being tracked." ) );
}

/**
 * Both to the running output, which takes it on its next block, and to the
 * configuration, which is where the next output will read it from.
 */
void DriftDialog::applyTunable( int index, double value )
{
    if( applying )
        return;

    const Tunable &t = tunables.at( index );

    applying = true;
    t.box->setValue( value );
    t.slider->setValue( (int)lround( value / t.step ) );
    applying = false;

    audio_output_t *aout = THEMIM->getAout();

    if( t.integer )
    {
        config_PutInt( p_intf, t.name, (int64_t)lround( value ) );
        if( aout != NULL )
            var_SetInteger( VLC_OBJECT(aout), t.name, (int64_t)lround( value ) );
    }
    else
    {
        config_PutFloat( p_intf, t.name, (float)value );
        if( aout != NULL )
            var_SetFloat( VLC_OBJECT(aout), t.name, (float)value );
    }

    if( aout != NULL )
        vlc_object_release( aout );
}

/** What the settings are, asking the output first and the configuration for
 * want of one. */
void DriftDialog::readTunables()
{
    audio_output_t *aout = THEMIM->getAout();

    applying = true;
    for( int i = 0; i < tunables.size(); i++ )
    {
        const Tunable &t = tunables.at( i );
        double value;

        if( t.integer )
            value = ( aout != NULL ) ? var_GetInteger( VLC_OBJECT(aout), t.name )
                                     : config_GetInt( p_intf, t.name );
        else
            value = ( aout != NULL ) ? var_GetFloat( VLC_OBJECT(aout), t.name )
                                     : config_GetFloat( p_intf, t.name );

        t.box->setValue( value );
        t.slider->setValue( (int)lround( value / t.step ) );
    }
    applying = false;

    if( aout != NULL )
        vlc_object_release( aout );
}

/**
 * Finds the ring of whatever output is playing now.
 *
 * The output the dialog was opened on may be long gone, and there may have
 * been none at all. Holding the ring rather than the output is what makes
 * that safe: a ring nobody else holds cannot be handed out twice, so a ring
 * that compares equal to the one already held really is the same one.
 */
void DriftDialog::attach()
{
    audio_output_t *aout = THEMIM->getAout();

    if( aout == NULL )
        return;

    aout_drift_monitor_t *m = aout_DriftMonitorHold( aout );
    vlc_object_release( aout );

    if( m == NULL || m == monitor )
    {
        if( m != NULL )
            aout_DriftMonitorRelease( m );
        return;
    }

    if( monitor != NULL )
    {
        aout_DriftMonitorArm( monitor, false );
        aout_DriftMonitorRelease( monitor );
    }

    monitor = m;
    seq = 0;
    dropped = false;
    points.clear();
    aout_DriftMonitorArm( monitor, true );
    readTunables();
}

void DriftDialog::collect()
{
    aout_drift_point batch[256];
    size_t n;

    do
    {
        const uint64_t was = seq;

        n = aout_DriftMonitorRead( monitor, batch,
                                   sizeof(batch) / sizeof(batch[0]), &seq );
        if( was != 0 && seq - was > n )
            dropped = true;

        for( size_t i = 0; i < n; i++ )
            points.append( batch[i] );
    }
    while( n == sizeof(batch) / sizeof(batch[0]) );

    aout_DriftMonitorConfig( monitor, &config );

    if( points.isEmpty() )
        return;

    const vlc_tick_t cut = points.last().date
                         - (vlc_tick_t)( HISTORY * CLOCK_FREQ );
    int keep = 0;
    while( keep < points.size() && points.at( keep ).date < cut )
        keep++;
    if( keep > 0 )
        points.remove( 0, keep );
}

void DriftDialog::poll()
{
    if( freeze->isChecked() )
        return;

    attach();

    if( monitor != NULL )
        collect();

    report();
    plot->update();
}

/** Least squares on the drift against time: the slope is what the device's
 * clock is doing that the correction has not answered, and the scatter about
 * it is the noise the slew exists to keep out of the pitch. */
void DriftDialog::report()
{
    const double window = windowBox->currentData().toDouble();

    if( points.isEmpty() )
    {
        state->setText( monitor != NULL
            ? qtr( "<b>Waiting for the first reading.</b>" )
            : qtr( "<b>No audio output.</b> Start playing something with "
                   "audio and the readings appear here." ) );
        readout->clear();
        return;
    }

    const vlc_tick_t t0 = points.last().date
                        - (vlc_tick_t)( window * CLOCK_FREQ );

    double n = 0., st = 0., sd = 0., stt = 0., std_ = 0.;
    double detune = 0., integral = 0., delayLo = 0., delayHi = 0.;
    double driftLo = 0., driftHi = 0., held = 0., bound = 0.;
    int declared = 0, steps = 0, latches = 0;

    for( int i = 0; i < points.size(); i++ )
    {
        const aout_drift_point &p = points.at( i );
        if( p.date < t0 )
            continue;

        if( p.latch )
        {
            latches++;
            if( isDeclared( p ) ) declared++;
            else if( isStep( p ) ) steps++;
        }

        held++;
        detune += p.detune;
        integral += p.integral;
        if( p.bound )
            bound++;

        if( !p.reading )
            continue;

        const double t = ( p.date - t0 ) / (double)CLOCK_FREQ;
        const double d = p.drift / 1000., l = p.delay / 1000.;

        if( n == 0. ) { delayLo = delayHi = l; driftLo = driftHi = d; }
        delayLo = qMin( delayLo, l ); delayHi = qMax( delayHi, l );
        driftLo = qMin( driftLo, d ); driftHi = qMax( driftHi, d );

        n++; st += t; sd += d; stt += t * t; std_ += t * d;
    }

    if( held == 0. )
        held = 1.;

    const double meanDetune = detune / held;
    const double meanIntegral = integral / held;

    QString text;

    if( !config.running )
        text += qtr( "<b>Stopped.</b> What is shown is the last stream's. " );
    else if( config.max_cents <= 0.f )
        text += qtr( "<b>The bound is zero, so the loop is off.</b> Nothing "
                     "is being detuned - every offset is spliced out instead, "
                     "by inserting silence or jumping ahead, which is heard. "
                     "Raise the bound below to correct by resampling. " );
    else
        text += qtr( "<b>Running.</b> " );

    if( config.rate > 0 )
        text += qtr( "Source %1 Hz into %2 Hz, bound %3 cents." )
                    .arg( config.src_rate ).arg( config.rate )
                    .arg( config.max_cents, 0, 'f', 0 );
    if( dropped )
        text += qtr( " Some readings were missed." );
    state->setText( text );

    QString line;

    if( n >= 3. && ( n * stt - st * st ) != 0. )
    {
        const double slope = ( n * std_ - st * sd ) / ( n * stt - st * st );
        const double intercept = ( sd - slope * st ) / n;

        double residual = 0.;
        for( int i = 0; i < points.size(); i++ )
        {
            const aout_drift_point &p = points.at( i );
            if( p.date < t0 || !p.reading )
                continue;
            const double t = ( p.date - t0 ) / (double)CLOCK_FREQ;
            const double e = p.drift / 1000. - ( intercept + slope * t );
            residual += e * e;
        }
        residual = sqrt( residual / n );

        /* What the device is doing is the two together: a drift closing at
         * the rate the detune is closing it says nothing about the device.
         * Both terms are against the drift, which runs the other way - a
         * device playing fast makes the stream early, not late - so a device
         * faster than the source reads positive here. */
        const double uncorrected = -slope * 1000.;
        const double corrected = -centsToPpm( meanDetune );

        line += qtr( "<b>Rate mismatch %1 ppm</b>, device against source "
                     "(%2 ppm of it still showing as drift, %3 ppm made up "
                     "by the detune)<br/>" )
                    .arg( uncorrected + corrected, 0, 'f', 1 )
                    .arg( uncorrected, 0, 'f', 1 )
                    .arg( corrected, 0, 'f', 1 );
        line += qtr( "Drift %1 to %2 ms, noise about the trend %3 ms. " )
                    .arg( driftLo, 0, 'f', 2 ).arg( driftHi, 0, 'f', 2 )
                    .arg( residual, 0, 'f', 2 );
    }
    else
        line += qtr( "Not enough readings yet. " );

    line += qtr( "Latency %1 to %2 ms.<br/>" )
                .arg( delayLo, 0, 'f', 1 ).arg( delayHi, 0, 'f', 1 );
    line += qtr( "Detune %1 cents, integral %2 cents, at the bound for %3% "
                 "of the window.<br/>" )
                .arg( meanDetune, 0, 'f', 3 ).arg( meanIntegral, 0, 'f', 3 )
                .arg( 100. * bound / held, 0, 'f', 0 );
    line += qtr( "%1 discontinuities: %2 declared, %3 detected as steps, "
                 "%4 flushes, jumps or silences." )
                .arg( latches ).arg( declared ).arg( steps )
                .arg( latches - declared - steps );

    readout->setText( line );
}

void DriftDialog::windowChanged( int index )
{
    plot->setWindow( windowBox->itemData( index ).toDouble() );
    report();
}
