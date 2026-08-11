/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#pragma once

#include <QImage>
#include <QRectF>
#include <QSizeF>
#include <QWidget>

namespace ps {

/**
 * One page, big enough to read, with something draggable on top of it.
 *
 * This is the missing half of a page organiser: the grid tells you what order
 * the pages are in, but until now there was no way to actually look at one.
 * It doubles as the canvas for placing a signature, because "where exactly?"
 * is a question best answered by pointing rather than by typing coordinates.
 *
 * The overlay rectangle is held in normalised page coordinates, so it stays
 * where the user put it when the window is resized, and converts to PDF points
 * on demand.
 */
class PagePlacementView : public QWidget
{
    Q_OBJECT

public:
    explicit PagePlacementView(QWidget *parent = nullptr);

    /** @p pageSizePoints is the page as displayed, i.e. with /Rotate applied. */
    void setPage(const QImage &pageImage, const QSizeF &pageSizePoints);

    /** Null hides the overlay and leaves a plain page viewer. */
    void setOverlayImage(const QImage &image);

    /** Overlay position in PDF points, origin at the bottom left of the page. */
    QRectF overlayRectInPoints() const;

    /** Width as a fraction of the page width; height follows the aspect ratio. */
    void setOverlayRelativeWidth(double fraction);
    double overlayRelativeWidth() const;

    bool hasOverlay() const;

    QSize sizeHint() const override;

Q_SIGNALS:
    void overlayChanged();

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;

private:
    /** Where the page is drawn inside the widget. */
    QRect pageRect() const;

    /** The overlay in widget coordinates. */
    QRect overlayRect() const;

    /** The square handle at the corner used for resizing. */
    QRect resizeHandle() const;

    void clampOverlay();

    QImage m_page;
    QSizeF m_pageSizePoints;
    QImage m_overlay;

    /** Centre of the overlay, 0..1 across the page, y measured downwards. */
    QPointF m_centre { 0.72, 0.82 };
    double m_relativeWidth = 0.28;

    enum class Drag { None, Move, Resize };
    Drag m_drag = Drag::None;
    QPoint m_dragStart;
    QPointF m_centreAtDragStart;
    double m_widthAtDragStart = 0.0;
};

} // namespace ps
