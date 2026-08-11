/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#pragma once

#include "Alignment.h"

#include <QRectF>
#include <QString>
#include <QVector>
#include <QWidget>

#include <functional>

class QGridLayout;
class QToolButton;

namespace ps {

/**
 * The buttons that line a selection up, for any panel that has one.
 *
 * It is handed rectangles and answers with rectangles: it never learns what
 * they belong to, which is what lets the same row of buttons sit above the form
 * fields and above the page objects without either of them knowing about the
 * other. The arithmetic is all in Alignment; this is the part with the icons.
 *
 * Rectangles are in PDF points with the origin at the bottom left, the space
 * the overlays speak, so "top" is the larger y.
 *
 * A button that cannot do anything for the current count is disabled and says
 * why in its tooltip rather than disappearing: controls that come and go have
 * to be found again every time, and a greyed-out button with a reason teaches
 * what to select next.
 */
class AlignmentPanel : public QWidget
{
    Q_OBJECT

public:
    explicit AlignmentPanel(QWidget *parent = nullptr);

    /**
     * The rectangles the buttons act on, which also decides which of them can
     * act at all: fewer than two disables everything, fewer than three leaves
     * the spacing buttons out.
     */
    void setBoxes(const QVector<QRectF> &boxes);

    /** What the next press would work from; after one, the answer it gave. */
    QVector<QRectF> boxes() const;

Q_SIGNALS:
    /** The new rectangles, in the order they were handed to setBoxes(). */
    void changed(const QVector<QRectF> &boxes);

private:
    /** Takes the rectangles as they stand and answers with where they belong. */
    using Action = std::function<QVector<QRectF>(const QVector<QRectF> &)>;

    /**
     * Builds one button at @p row and @p column.
     *
     * @p needs is how many rectangles it takes to mean anything, and @p tip is
     * what it does, shown while it can, replaced by the reason while it
     * cannot.
     */
    void addButton(int row, int column, const QString &iconName, const QString &text, const QString &tip,
                   qsizetype needs, const Action &act);

    /** Runs one button's answer: keeps it, then tells whoever is listening. */
    void apply(const QVector<QRectF> &boxes);

    /** Follows the count: what can be pressed, and what the tooltips say. */
    void refreshButtons();

    struct Button {
        QToolButton *widget = nullptr;
        qsizetype needs = 2;
        QString tip;
    };

    QGridLayout *m_grid;
    QVector<Button> m_buttons;
    QVector<QRectF> m_boxes;
};

} // namespace ps
