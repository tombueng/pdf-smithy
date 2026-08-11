/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#pragma once

#include "PageCanvas.h"
#include "core/PageCrop.h"

namespace ps {

/**
 * The page with the trim shown on it, and draggable.
 *
 * Trimming by typing four numbers means guessing, checking, and guessing
 * again, and the numbers are in points, which nobody has an instinct for.
 * Here the part being kept is bright, what goes is dimmed, and the edges are
 * dragged to where they look right. The numbers stay: they are how a trim is
 * repeated exactly, and they follow the dragging both ways.
 */
class CropView : public PageCanvas
{
    Q_OBJECT

public:
    explicit CropView(QWidget *parent = nullptr);

    PageCrop::Margins margins() const { return m_margins; }

public Q_SLOTS:
    /** In PDF points, from each edge inwards. */
    void setMargins(const PageCrop::Margins &margins);

Q_SIGNALS:
    void marginsChanged(const PageCrop::Margins &margins);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

private:
    /** Which edge the pointer is on, as a pair of flags. */
    struct Grab {
        bool left = false;
        bool right = false;
        bool top = false;
        bool bottom = false;

        bool any() const { return left || right || top || bottom; }
    };

    /** The kept area in widget pixels. */
    QRect keptRect() const;

    Grab grabAt(const QPoint &pos) const;
    Qt::CursorShape cursorFor(const Grab &grab) const;

    PageCrop::Margins m_margins;
    Grab m_dragging;
};

} // namespace ps
