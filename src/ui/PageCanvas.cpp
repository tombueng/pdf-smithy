/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "PageCanvas.h"

#include <KLocalizedString>

#include <QPainter>
#include <QPalette>

namespace ps {

PageCanvas::PageCanvas(QWidget *parent)
    : QWidget(parent)
{
    setMinimumSize(320, 420);
    setFocusPolicy(Qt::StrongFocus);
    setBackgroundRole(QPalette::Base);
    setAutoFillBackground(true);
}

void PageCanvas::setPage(const QImage &pageImage, const QSizeF &pageSizePoints)
{
    m_page = pageImage;
    m_pageSizePoints = pageSizePoints;
    update();
}

QSize PageCanvas::sizeHint() const
{
    return { 560, 720 };
}

QRect PageCanvas::pageRect() const
{
    if (m_page.isNull()) {
        return rect().adjusted(8, 8, -8, -8);
    }
    const QSize scaled = m_page.size().scaled(size().shrunkBy(QMargins(8, 8, 8, 8)), Qt::KeepAspectRatio);
    return QRect(QPoint((width() - scaled.width()) / 2, (height() - scaled.height()) / 2), scaled);
}

void PageCanvas::drawPage(QPainter &painter) const
{
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.setRenderHint(QPainter::Antialiasing);

    if (m_page.isNull()) {
        painter.setPen(palette().color(QPalette::PlaceholderText));
        painter.drawText(rect(), Qt::AlignCenter, i18n("No page to show."));
        return;
    }

    const QRect page = pageRect();
    painter.drawImage(page, m_page);
    painter.setPen(palette().color(QPalette::Mid));
    painter.drawRect(page.adjusted(0, 0, -1, -1));
}

QRect PageCanvas::toWidget(const QRectF &normalised) const
{
    const QRect page = pageRect();
    return QRect(page.left() + qRound(normalised.x() * page.width()),
                 page.top() + qRound(normalised.y() * page.height()), qRound(normalised.width() * page.width()),
                 qRound(normalised.height() * page.height()));
}

QPoint PageCanvas::toWidget(const QPointF &normalised) const
{
    const QRect page = pageRect();
    return QPoint(page.left() + qRound(normalised.x() * page.width()),
                  page.top() + qRound(normalised.y() * page.height()));
}

QRectF PageCanvas::fromWidget(const QRect &widgetRect) const
{
    const QRect page = pageRect();
    if (page.width() <= 0 || page.height() <= 0) {
        return {};
    }
    const QRect clipped = widgetRect.intersected(page);
    return QRectF(double(clipped.left() - page.left()) / page.width(),
                  double(clipped.top() - page.top()) / page.height(), double(clipped.width()) / page.width(),
                  double(clipped.height()) / page.height());
}

QPointF PageCanvas::fromWidget(const QPoint &widgetPoint) const
{
    const QRect page = pageRect();
    if (page.width() <= 0 || page.height() <= 0) {
        return {};
    }
    return QPointF(double(widgetPoint.x() - page.left()) / page.width(),
                   double(widgetPoint.y() - page.top()) / page.height());
}

QRectF PageCanvas::toPoints(const QRectF &normalised) const
{
    // The flip: normalised y grows downwards like an image, PDF y grows upwards
    // from the bottom left.
    return QRectF(normalised.x() * m_pageSizePoints.width(),
                  (1.0 - normalised.y() - normalised.height()) * m_pageSizePoints.height(),
                  normalised.width() * m_pageSizePoints.width(), normalised.height() * m_pageSizePoints.height());
}

QRectF PageCanvas::fromPoints(const QRectF &points) const
{
    if (m_pageSizePoints.isEmpty()) {
        return {};
    }
    return QRectF(points.x() / m_pageSizePoints.width(),
                  1.0 - (points.y() + points.height()) / m_pageSizePoints.height(),
                  points.width() / m_pageSizePoints.width(), points.height() / m_pageSizePoints.height());
}

QPointF PageCanvas::toPoints(const QPointF &normalised) const
{
    return QPointF(normalised.x() * m_pageSizePoints.width(), (1.0 - normalised.y()) * m_pageSizePoints.height());
}

QPointF PageCanvas::fromPoints(const QPointF &points) const
{
    if (m_pageSizePoints.isEmpty()) {
        return {};
    }
    return QPointF(points.x() / m_pageSizePoints.width(), 1.0 - points.y() / m_pageSizePoints.height());
}

} // namespace ps
