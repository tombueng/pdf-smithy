/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#pragma once

#include "print/PrintController.h"

#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QSpinBox;

namespace ps {

/**
 * The layout controls, shown as an extra tab inside the system print dialog.
 *
 * Adding a tab rather than putting a second dialog in front of the first is
 * what keeps printing a single decision: paper, copies and imposition all sit
 * in the same window, and Cancel means cancel.
 */
class PrintOptionsWidget : public QWidget
{
    Q_OBJECT

public:
    explicit PrintOptionsWidget(QWidget *parent = nullptr);

    PrintController::Options options() const;
    void setOptions(const PrintController::Options &options);

    /** Shown live as the settings change, e.g. "12 pages on 3 sheets". */
    void setSummary(const QString &text);

Q_SIGNALS:
    void optionsChanged();

private:
    QLineEdit *m_range;
    QComboBox *m_layout;
    QComboBox *m_scaling;
    QSpinBox *m_customScale;
    QComboBox *m_subset;
    QCheckBox *m_reverse;
    QCheckBox *m_grayscale;
    QCheckBox *m_borders;
    QSpinBox *m_margin;
    QLabel *m_summary;
};

} // namespace ps
