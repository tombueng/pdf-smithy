/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "PagePlacementView.h"

#include <KLocalizedString>

#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace ps {

namespace {

constexpr int HandleSize = 14;
constexpr int Margin = 12;

} // namespace

PagePlacementView::PagePlacementView(QWidget *parent)
    : QWidget(parent)
{
    setMinimumSize(360, 460);
    setMouseTracking(true);
    setCursor(Qt::ArrowCursor);
}

QSize PagePlacementView::sizeHint() const
{
    return { 520, 680 };
}

void PagePlacementView::setPage(const QImage &pageImage, const QSizeF &pageSizePoints)
{
    m_page = pageImage;
    m_pageSizePoints = pageSizePoints;
    update();
}

void PagePlacementView::setOverlayImage(const QImage &image)
{
    m_overlay = image;
    clampOverlay();
    update();
    Q_EMIT overlayChanged();
}

bool PagePlacementView::hasOverlay() const
{
    return !m_overlay.isNull();
}

double PagePlacementView::overlayRelativeWidth() const
{
    return m_relativeWidth;
}

void PagePlacementView::setOverlayRelativeWidth(double fraction)
{
    m_relativeWidth = std::clamp(fraction, 0.03, 1.0);
    clampOverlay();
    update();
    Q_EMIT overlayChanged();
}

QRect PagePlacementView::pageRect() const
{
    if (m_page.isNull()) {
        return {};
    }

    const QRect available = rect().adjusted(Margin, Margin, -Margin, -Margin);
    const double pageAspect = static_cast<double>(m_page.height()) / m_page.width();

    int w = available.width();
    int h = static_cast<int>(std::lround(w * pageAspect));
    if (h > available.height()) {
        h = available.height();
        w = static_cast<int>(std::lround(h / pageAspect));
    }

    return QRect(available.left() + (available.width() - w) / 2, available.top() + (available.height() - h) / 2, w, h);
}

QRect PagePlacementView::overlayRect() const
{
    const QRect page = pageRect();
    if (page.isEmpty() || m_overlay.isNull()) {
        return {};
    }

    const double aspect = m_overlay.width() > 0 ? static_cast<double>(m_overlay.height()) / m_overlay.width() : 1.0;
    const double w = page.width() * m_relativeWidth;
    const double h = w * aspect;

    return QRect(static_cast<int>(std::lround(page.left() + m_centre.x() * page.width() - w / 2.0)),
                 static_cast<int>(std::lround(page.top() + m_centre.y() * page.height() - h / 2.0)),
                 static_cast<int>(std::lround(w)), static_cast<int>(std::lround(h)));
}

QRect PagePlacementView::resizeHandle() const
{
    const QRect box = overlayRect();
    if (box.isEmpty()) {
        return {};
    }
    return QRect(box.right() - HandleSize / 2, box.bottom() - HandleSize / 2, HandleSize, HandleSize);
}

void PagePlacementView::clampOverlay()
{
    // Keep at least the centre on the page. Letting it wander off entirely
    // would mean stamping something nobody can see.
    m_centre.setX(std::clamp(m_centre.x(), 0.0, 1.0));
    m_centre.setY(std::clamp(m_centre.y(), 0.0, 1.0));
}

QRectF PagePlacementView::overlayRectInPoints() const
{
    if (m_overlay.isNull() || m_pageSizePoints.isEmpty()) {
        return {};
    }

    const double aspect = m_overlay.width() > 0 ? static_cast<double>(m_overlay.height()) / m_overlay.width() : 1.0;
    const double w = m_pageSizePoints.width() * m_relativeWidth;
    const double h = w * aspect;

    const double centreX = m_centre.x() * m_pageSizePoints.width();
    // The widget measures downwards, PDF measures upwards.
    const double centreY = (1.0 - m_centre.y()) * m_pageSizePoints.height();

    return QRectF(centreX - w / 2.0, centreY - h / 2.0, w, h);
}

// ── Painting ──────────────────────────────────────────────────────────────

void PagePlacementView::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

    painter.fillRect(rect(), palette().color(QPalette::Window).darker(112));

    const QRect page = pageRect();
    if (page.isEmpty()) {
        painter.setPen(palette().color(QPalette::WindowText));
        painter.drawText(rect(), Qt::AlignCenter, i18n("No page"));
        return;
    }

    // A shadow so the sheet reads as paper lying on a surface.
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0, 0, 0, 40));
    painter.drawRoundedRect(page.adjusted(-1, 3, 1, 5), 3, 3);

    painter.fillRect(page, Qt::white);
    painter.drawImage(page, m_page);

    QColor border = palette().color(QPalette::WindowText);
    border.setAlpha(70);
    painter.setPen(QPen(border, 1));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(page);

    const QRect box = overlayRect();
    if (box.isEmpty()) {
        return;
    }

    painter.drawImage(box, m_overlay);

    // A dashed frame and one corner handle: enough to say "this can be moved
    // and resized" without covering the signature it belongs to.
    QColor accent = palette().color(QPalette::Highlight);
    painter.setPen(QPen(accent, 1, Qt::DashLine));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(box);

    painter.setPen(Qt::NoPen);
    painter.setBrush(accent);
    painter.drawRoundedRect(resizeHandle(), 3, 3);
}

// ── Interaction ───────────────────────────────────────────────────────────

void PagePlacementView::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton || m_overlay.isNull()) {
        return;
    }

    m_dragStart = event->pos();
    m_centreAtDragStart = m_centre;
    m_widthAtDragStart = m_relativeWidth;

    if (resizeHandle().contains(event->pos())) {
        m_drag = Drag::Resize;
    } else if (overlayRect().contains(event->pos())) {
        m_drag = Drag::Move;
    } else {
        // Clicking bare paper moves the stamp there, which is quicker than
        // dragging it across the page.
        const QRect page = pageRect();
        if (page.contains(event->pos())) {
            m_centre = QPointF(static_cast<double>(event->pos().x() - page.left()) / page.width(),
                               static_cast<double>(event->pos().y() - page.top()) / page.height());
            clampOverlay();
            update();
            Q_EMIT overlayChanged();
            m_drag = Drag::Move;
            m_centreAtDragStart = m_centre;
        }
    }
}

void PagePlacementView::mouseMoveEvent(QMouseEvent *event)
{
    const QRect page = pageRect();

    if (m_drag == Drag::None) {
        if (!m_overlay.isNull() && resizeHandle().contains(event->pos())) {
            setCursor(Qt::SizeFDiagCursor);
        } else if (!m_overlay.isNull() && overlayRect().contains(event->pos())) {
            setCursor(Qt::OpenHandCursor);
        } else {
            setCursor(Qt::ArrowCursor);
        }
        return;
    }

    if (page.isEmpty()) {
        return;
    }

    if (m_drag == Drag::Move) {
        const QPoint delta = event->pos() - m_dragStart;
        m_centre = QPointF(m_centreAtDragStart.x() + static_cast<double>(delta.x()) / page.width(),
                           m_centreAtDragStart.y() + static_cast<double>(delta.y()) / page.height());
        clampOverlay();
    } else {
        // Resizing pulls the corner away from the centre, so the distance
        // travelled is twice the change in half-width.
        const int delta = event->pos().x() - m_dragStart.x();
        m_relativeWidth = std::clamp(m_widthAtDragStart + 2.0 * delta / page.width(), 0.03, 1.0);
    }

    update();
    Q_EMIT overlayChanged();
}

void PagePlacementView::mouseReleaseEvent(QMouseEvent *)
{
    m_drag = Drag::None;
    setCursor(Qt::ArrowCursor);
}

void PagePlacementView::wheelEvent(QWheelEvent *event)
{
    if (m_overlay.isNull()) {
        QWidget::wheelEvent(event);
        return;
    }

    const int steps = event->angleDelta().y() / 120;
    if (steps != 0) {
        setOverlayRelativeWidth(m_relativeWidth * std::pow(1.1, steps));
    }
    event->accept();
}

} // namespace ps
