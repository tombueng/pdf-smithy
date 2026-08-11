/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "CropView.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>

namespace ps {

namespace {

/** How near an edge counts as being on it. */
constexpr int Reach = 7;

/** The kept area never shrinks below this, or the handles overlap. */
constexpr double MinimumKeptPoints = 18.0;

} // namespace

CropView::CropView(QWidget *parent)
    : PageCanvas(parent)
{
    setMouseTracking(true);
}

void CropView::setMargins(const PageCrop::Margins &margins)
{
    m_margins = margins;
    update();
}

QRect CropView::keptRect() const
{
    const QSizeF page = pageSizePoints();
    if (page.isEmpty()) {
        return {};
    }

    // Points measured from each edge inwards, turned into the normalised,
    // y-downwards space the canvas works in. Top and bottom swap on the way,
    // which is the one thing worth being careful about here.
    const double left = std::clamp(m_margins.left / page.width(), 0.0, 1.0);
    const double right = std::clamp(m_margins.right / page.width(), 0.0, 1.0);
    const double top = std::clamp(m_margins.top / page.height(), 0.0, 1.0);
    const double bottom = std::clamp(m_margins.bottom / page.height(), 0.0, 1.0);

    const QRectF normalised(left, top, std::max(0.0, 1.0 - left - right), std::max(0.0, 1.0 - top - bottom));
    return toWidget(normalised);
}

void CropView::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event)

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    drawPage(painter);

    if (!hasPage()) {
        return;
    }

    const QRect page = pageRect();
    const QRect kept = keptRect();
    if (!kept.isValid()) {
        return;
    }

    // What goes is dimmed rather than hidden: seeing how much is being thrown
    // away is the whole point of doing this on the page.
    QPainterPath outside;
    outside.addRect(page);
    QPainterPath inside;
    inside.addRect(kept);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0, 0, 0, 110));
    painter.drawPath(outside.subtracted(inside));

    painter.setPen(QPen(palette().color(QPalette::Highlight), 2));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(kept);

    // Corner marks, so the edges read as something to take hold of.
    painter.setBrush(palette().color(QPalette::Highlight));
    painter.setPen(Qt::NoPen);
    const int arm = 10;
    for (const QPoint &corner : { kept.topLeft(), kept.topRight(), kept.bottomLeft(), kept.bottomRight() }) {
        painter.drawRect(QRect(corner.x() - 2, corner.y() - 2, 4, 4).adjusted(-1, -1, 1, 1));
    }
    painter.setPen(QPen(palette().color(QPalette::Highlight), 3));
    painter.drawLine(kept.left(), kept.top(), kept.left() + arm, kept.top());
    painter.drawLine(kept.right() - arm, kept.bottom(), kept.right(), kept.bottom());
}

CropView::Grab CropView::grabAt(const QPoint &pos) const
{
    Grab grab;
    const QRect kept = keptRect();
    if (!kept.isValid()) {
        return grab;
    }

    const bool inRows = pos.y() > kept.top() - Reach && pos.y() < kept.bottom() + Reach;
    const bool inColumns = pos.x() > kept.left() - Reach && pos.x() < kept.right() + Reach;

    grab.left = inRows && std::abs(pos.x() - kept.left()) <= Reach;
    grab.right = inRows && std::abs(pos.x() - kept.right()) <= Reach;
    grab.top = inColumns && std::abs(pos.y() - kept.top()) <= Reach;
    grab.bottom = inColumns && std::abs(pos.y() - kept.bottom()) <= Reach;
    return grab;
}

Qt::CursorShape CropView::cursorFor(const Grab &grab) const
{
    if ((grab.left && grab.top) || (grab.right && grab.bottom)) {
        return Qt::SizeFDiagCursor;
    }
    if ((grab.right && grab.top) || (grab.left && grab.bottom)) {
        return Qt::SizeBDiagCursor;
    }
    if (grab.left || grab.right) {
        return Qt::SizeHorCursor;
    }
    if (grab.top || grab.bottom) {
        return Qt::SizeVerCursor;
    }
    return Qt::ArrowCursor;
}

void CropView::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton || !hasPage()) {
        PageCanvas::mousePressEvent(event);
        return;
    }
    m_dragging = grabAt(event->pos());
}

void CropView::mouseMoveEvent(QMouseEvent *event)
{
    if (!hasPage()) {
        return;
    }

    if (!m_dragging.any()) {
        setCursor(cursorFor(grabAt(event->pos())));
        return;
    }

    const QSizeF page = pageSizePoints();
    const QPointF here = fromWidget(event->pos()); // normalised, y downwards
    PageCrop::Margins margins = m_margins;

    const double minX = MinimumKeptPoints / std::max(1.0, page.width());
    const double minY = MinimumKeptPoints / std::max(1.0, page.height());

    if (m_dragging.left) {
        const double limit = 1.0 - (margins.right / page.width()) - minX;
        margins.left = std::clamp(here.x(), 0.0, std::max(0.0, limit)) * page.width();
    }
    if (m_dragging.right) {
        const double limit = 1.0 - (margins.left / page.width()) - minX;
        margins.right = std::clamp(1.0 - here.x(), 0.0, std::max(0.0, limit)) * page.width();
    }
    if (m_dragging.top) {
        const double limit = 1.0 - (margins.bottom / page.height()) - minY;
        margins.top = std::clamp(here.y(), 0.0, std::max(0.0, limit)) * page.height();
    }
    if (m_dragging.bottom) {
        const double limit = 1.0 - (margins.top / page.height()) - minY;
        margins.bottom = std::clamp(1.0 - here.y(), 0.0, std::max(0.0, limit)) * page.height();
    }

    m_margins = margins;
    update();
    Q_EMIT marginsChanged(m_margins);
}

void CropView::mouseReleaseEvent(QMouseEvent *event)
{
    Q_UNUSED(event)
    m_dragging = {};
}

} // namespace ps
