/*****************************************************************************
 * drift.hpp : what the audio drift correction is doing, while it does it
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

#ifndef QVLC_DRIFT_DIALOG_H_
#define QVLC_DRIFT_DIALOG_H_ 1

#include "util/qvlcframe.hpp"
#include "util/singleton.hpp"

#include <vlc_aout.h>

#include <QVector>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QGridLayout;
class QLabel;
class QSlider;
class QTimer;

/**
 * The readings, against time, in three panes that share a time axis: what the
 * device said it was holding, how far off the stream was, and what the
 * controller did about it.
 *
 * Drawn straight from the dialog's points rather than from a copy: the plot
 * is its child and is repainted when they change.
 */
class DriftPlot : public QWidget
{
public:
    DriftPlot( const QVector<aout_drift_point> &, const aout_drift_config &,
               QWidget *parent = NULL );

    void setWindow( double seconds ) { window = seconds; update(); }

protected:
    void paintEvent( QPaintEvent * ) Q_DECL_OVERRIDE;

private:
    const QVector<aout_drift_point> &points;
    const aout_drift_config &config;
    double window;
};

/**
 * The drift controller, live: what it is measuring, what it is asking for,
 * what the bound and the slew are letting through, and the four settings it
 * is running with, which can be moved while it runs.
 */
class DriftDialog : public QVLCFrame, public Singleton<DriftDialog>
{
    Q_OBJECT

public:
    /** How much of the past is kept, whatever window is being shown */
    static const double HISTORY;

private:
    DriftDialog( intf_thread_t * );
    virtual ~DriftDialog();

    /** One setting, as a slider and a box that say the same thing */
    struct Tunable
    {
        const char *name;
        QSlider *slider;
        QDoubleSpinBox *box;
        double step;        /**< What one slider notch is worth */
        bool integer;       /**< The setting is an integer one */
    };

    void showEvent( QShowEvent * ) Q_DECL_OVERRIDE;
    void hideEvent( QHideEvent * ) Q_DECL_OVERRIDE;

    void buildTunables( QWidget * );
    void addTunable( QGridLayout *, int row, const char *name,
                     const QString &label, double min, double max,
                     double step, bool integer, const QString &suffix,
                     const QString &tip );
    void applyTunable( int, double );
    void readTunables();

    void attach();
    void collect();
    void report();

    QVector<aout_drift_point> points;
    aout_drift_config config;
    aout_drift_monitor_t *monitor;
    uint64_t seq;
    bool dropped;           /**< The ring ran ahead of the last read */

    QVector<Tunable> tunables;
    bool applying;          /**< A box is being filled in, not moved */

    DriftPlot *plot;
    QTimer *timer;
    QLabel *state;
    QLabel *readout;
    QComboBox *windowBox;
    QCheckBox *freeze;

    friend class Singleton<DriftDialog>;

private slots:
    void poll();
    void windowChanged( int );
};

#endif
