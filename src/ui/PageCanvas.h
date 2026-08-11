/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#pragma once

#include <QImage>
#include <QPointF>
#include <QRectF>
#include <QSizeF>
#include <QWidget>

namespace ps {

/**
 * One page, drawn to fit, with the arithmetic for pointing at it.
 *
 * Three of the tools here (marking a redaction, placing a signature, drawing a
 * comment) all need the same three conversions between the pixels under the
 * mouse, a resize-proof fraction of the page, and the PDF points the engine
 * works in. Getting the y axis the wrong way round in one of them is a bug that
 * looks like a feature until someone rotates a page, so it lives in one place.
 *
 * Subclasses draw their own overlay in paintEvent() after calling
 * drawPage(), and use toWidget()/fromWidget() rather than doing sums.
 */
class PageCanvas : public QWidget
{
    Q_OBJECT

public:
    explicit PageCanvas(QWidget *parent = nullptr);

    /** @p pageSizePoints is the page as displayed, i.e. with /Rotate applied. */
    void setPage(const QImage &pageImage, const QSizeF &pageSizePoints);

    const QSizeF &pageSizePoints() const { return m_pageSizePoints; }

    bool hasPage() const { return !m_page.isNull(); }

    QSize sizeHint() const override;

protected:
    /** Where the page is drawn inside the widget. */
    QRect pageRect() const;

    /** Draws the page and its outline; call first from paintEvent(). */
    void drawPage(QPainter &painter) const;

    /** Normalised coordinates run 0..1 across and *down*, as images do. */
    QRect toWidget(const QRectF &normalised) const;
    QRectF fromWidget(const QRect &widgetRect) const;
    QPoint toWidget(const QPointF &normalised) const;
    QPointF fromWidget(const QPoint &widgetPoint) const;

    /** PDF points, origin bottom left: what the engine takes. */
    QRectF toPoints(const QRectF &normalised) const;
    QRectF fromPoints(const QRectF &points) const;
    QPointF toPoints(const QPointF &normalised) const;
    QPointF fromPoints(const QPointF &points) const;

private:
    QImage m_page;
    QSizeF m_pageSizePoints;
};

} // namespace ps
