/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "WatermarkView.h"

#include <QPainter>
#include <QPainterPath>

namespace ps {

WatermarkView::WatermarkView(QWidget *parent)
    : PageCanvas(parent)
{
}

void WatermarkView::setTextStamp(const Overlay::TextStamp &stamp)
{
    m_stamp = stamp;
    m_usesImage = false;
    update();
}

void WatermarkView::setImageStamp(const QImage &image, Overlay::Anchor anchor, double relativeWidth, double rotation,
                                  double opacity)
{
    m_image = image;
    m_imageAnchor = anchor;
    m_imageWidth = relativeWidth;
    m_imageRotation = rotation;
    m_imageOpacity = opacity;
    m_usesImage = !image.isNull();
    update();
}

QPointF WatermarkView::anchorPoint(Overlay::Anchor anchor, double marginPoints) const
{
    const QRect page = pageRect();
    const QSizeF size = pageSizePoints();
    if (page.isEmpty() || size.isEmpty()) {
        return page.center();
    }

    // One scale for both axes: the page is drawn to fit, so it keeps its
    // proportions and a margin in points is the same number of pixels either
    // way round.
    const double scale = page.width() / size.width();
    const double margin = marginPoints * scale;

    double x = page.center().x();
    double y = page.center().y();

    switch (anchor) {
    case Overlay::Anchor::TopLeft:
    case Overlay::Anchor::CentreLeft:
    case Overlay::Anchor::BottomLeft:
        x = page.left() + margin;
        break;
    case Overlay::Anchor::TopRight:
    case Overlay::Anchor::CentreRight:
    case Overlay::Anchor::BottomRight:
        x = page.right() - margin;
        break;
    default:
        break;
    }

    switch (anchor) {
    case Overlay::Anchor::TopLeft:
    case Overlay::Anchor::TopCentre:
    case Overlay::Anchor::TopRight:
        y = page.top() + margin;
        break;
    case Overlay::Anchor::BottomLeft:
    case Overlay::Anchor::BottomCentre:
    case Overlay::Anchor::BottomRight:
        y = page.bottom() - margin;
        break;
    default:
        break;
    }

    return { x, y };
}

void WatermarkView::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event)

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    drawPage(painter);

    if (!hasPage()) {
        return;
    }

    const QRect page = pageRect();
    const QSizeF size = pageSizePoints();
    if (page.isEmpty() || size.isEmpty()) {
        return;
    }
    const double scale = page.width() / size.width();

    painter.save();
    painter.setClipRect(page);

    if (m_usesImage) {
        const double width = std::max(1.0, m_imageWidth * page.width());
        const double height = width * m_image.height() / std::max(1, m_image.width());
        const QPointF centre = anchorPoint(m_imageAnchor, 36.0);

        painter.setOpacity(m_imageOpacity);
        painter.translate(centre);
        // Negated: the engine turns anticlockwise in the page's y-up space, and
        // the widget's y runs the other way.
        painter.rotate(-m_imageRotation);
        painter.drawImage(QRectF(-width / 2.0, -height / 2.0, width, height), m_image);
        painter.restore();
        return;
    }

    if (m_stamp.text.isEmpty()) {
        painter.restore();
        return;
    }

    QFont font = painter.font();
    font.setPointSizeF(std::max(1.0, m_stamp.fontSize * scale));
    font.setBold(true);
    painter.setFont(font);

    const QPointF centre = anchorPoint(m_stamp.anchor, m_stamp.marginPoints);
    painter.setOpacity(m_stamp.opacity);
    painter.translate(centre);
    painter.rotate(-m_stamp.rotation);

    const QFontMetricsF metrics(font);
    const QRectF box = metrics.boundingRect(m_stamp.text);
    const QPointF baseline(-box.width() / 2.0, box.height() / 2.0 - metrics.descent());

    if (m_stamp.outlineOnly) {
        QPainterPath path;
        path.addText(baseline, font, m_stamp.text);
        painter.setPen(QPen(m_stamp.colour, std::max(1.0, m_stamp.fontSize * scale / 24.0)));
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(path);
    } else {
        painter.setPen(m_stamp.colour);
        painter.drawText(baseline, m_stamp.text);
    }

    painter.restore();
}

} // namespace ps
