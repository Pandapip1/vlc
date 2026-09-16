/*****************************************************************************
 * Copyright © 2011-2012 VideoLAN
 * $Id$
 *
 * Authors: Ludovic Fauvet <etix@l0cal.com>
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
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

#include "timetooltip.hpp"

#include <QApplication>
#include <QPainter>
#include <QFontMetrics>
#include <QRegion>

#define TIP_HEIGHT 5

TimeTooltip::TimeTooltip( QWidget *parent ) :
    QWidget( parent )
{
    /* The bubble belongs to the window that owns the slider and is drawn
     * inside it, in its coordinates. A client has no business naming a
     * point on the screen: it cannot place a window there on wayland, and on
     * a compositor that does not lay its surfaces out on one flat plane -
     * a headset, a rotated or scaled output, a tiling compositor - a screen
     * coordinate does not describe anywhere in particular. The window we are
     * part of is a space we do own. */

    /* A surface of our own, so that we stack above an embedded video, which
     * has one and would otherwise cover us. It is also what makes us a
     * subsurface on wayland, which the client positions relative to its
     * parent - the one placement wayland does allow. The bubble's shape comes
     * from a mask rather than from a translucent background, which needs a
     * compositor and leaves nothing on the screen without one. */
    setAttribute( Qt::WA_NativeWindow );
    setAttribute( Qt::WA_TransparentForMouseEvents );

    // Inherit from the system default font size -5
    mFont = QFont( "Verdana", qMax( qApp->font().pointSize() - 5, 7 ) );
    mTipX = -1;
    mTipAbove = true;

    // By default the widget is unintialized and should not be displayed
    resize( 0, 0 );
}

void TimeTooltip::adjustPosition()
{
    if( mDisplayedText.isEmpty() )
    {
        resize( 0, 0 );
        return;
    }

    // Get the bounding box required to print the text and add some padding
    QFontMetrics metrics( mFont );
    QRect textbox = metrics.boundingRect( mDisplayedText );
    textbox.adjust( -2, -2, 2, 2 );
    textbox.moveTo( 0, 0 );

    // Resize the widget to fit our needs
    QSize size( textbox.width() + 1, textbox.height() + TIP_HEIGHT + 1 );

    // The desired label position is just above the target
    QPoint position( mTarget.x() - size.width() / 2,
#if defined( Q_OS_WIN )
        mTarget.y() - 2 * size.height() - TIP_HEIGHT / 2 );
#else
        mTarget.y() - size.height() - TIP_HEIGHT / 2 );
#endif

    /* Keep the bubble inside the window it is drawn in. If the slider sits so
     * close to the top of that window that the bubble does not fit above it,
     * hang it under the slider instead rather than let it be clipped away. */
    bool above = true;
    const QWidget *parent = parentWidget();
    if( parent != NULL )
    {
        position.setX( qBound( 0, position.x(),
                               qMax( 0, parent->width() - size.width() ) ) );
        if( position.y() < 0 )
        {
            above = false;
            position.setY( qBound( 0, mAnchor.bottom() + TIP_HEIGHT / 2,
                                   qMax( 0, parent->height() - size.height() ) ) );
        }
    }

    move( position );

    /* The tip hangs off whichever edge of the box faces the slider, so the
     * box makes room for it above itself when the bubble points upwards */
    QRect box = textbox.translated( 0, above ? 0 : TIP_HEIGHT );

    int tipX = mTarget.x() - position.x();
    if( mBox != box || mTipX != tipX || mTipAbove != above )
    {
        mTipAbove = above;
        mBox = box;
        mTipX = tipX;

        resize( size );
        buildPath();
    }
}

void TimeTooltip::buildPath()
{
    // Prepare the painter path for future use so
    // we only have to generate the text at runtime.

    // Draw the text box
    mPainterPath = QPainterPath();
    mPainterPath.addRect( mBox );

    // Draw the tip, on the side of the box the slider is on
    const int base = mTipAbove ? mBox.bottom() + 1 : mBox.top();
    const int apex = mTipAbove ? base + TIP_HEIGHT : base - TIP_HEIGHT;
    QPolygonF polygon;
    polygon << QPoint( qMax( 0, mTipX - 3 ), base )
            << QPoint( mTipX, apex )
            << QPoint( qMin( mTipX + 3, mBox.width() ), base );
    mPainterPath.addPolygon( polygon );

    // Store the simplified version of the path
    mPainterPath = mPainterPath.simplified();

    /* We are a child widget now, so everything we do not draw shows whatever
     * is behind us in our own window. Cut the widget down to the bubble so
     * that the corners either side of the tip are not ours to spoil. */
    setMask( QRegion( mPainterPath.toFillPolygon().toPolygon() ) );
}

void TimeTooltip::setTip( const QPoint& target, const QRect& anchor,
                          const QString& time, const QString& text )
{
    mDisplayedText = time;
    if ( !text.isEmpty() )
        mDisplayedText.append( " - " ).append( text );

    if( mTarget != target || mAnchor != anchor ||
        time.length() != mTime.length() || mText != text )
    {
        mTarget = target;
        mAnchor = anchor;
        mTime = time;
        mText = text;
        adjustPosition();
    }

    update();
    raise();
}

void TimeTooltip::show()
{
    setVisible( true );
    raise();
}

void TimeTooltip::paintEvent( QPaintEvent * )
{
    QPainter p( this );
    p.setRenderHints( QPainter::TextAntialiasing );

    p.setPen( Qt::black );
    p.setBrush( qApp->palette().base() );
    p.drawPath( mPainterPath );

    p.setFont( mFont );
    p.setPen( QPen( qApp->palette().text(), 1 ) );
    p.drawText( mBox, Qt::AlignCenter, mDisplayedText );
}
