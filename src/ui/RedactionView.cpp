/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "RedactionView.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPalette>

namespace ps {

namespace {
/** Below this a drag is a click, not a rectangle worth keeping. */
constexpr int minimumDragPixels = 4;
}

RedactionView::RedactionView(QWidget *parent)
    : PageCanvas(parent)
{
    setCursor(Qt::CrossCursor);
}

QVector<QRectF> RedactionView::areasInPoints() const
{
    QVector<QRectF> points;
    points.reserve(m_areas.size());
    for (const QRectF &area : m_areas) {
        points.append(toPoints(area));
    }
    return points;
}

void RedactionView::setAreasInPoints(const QVector<QRectF> &areas)
{
    m_areas.clear();
    if (pageSizePoints().isEmpty()) {
        return;
    }
    for (const QRectF &area : areas) {
        m_areas.append(fromPoints(area));
    }
    m_selected = -1;
    update();
    Q_EMIT areasChanged();
}

int RedactionView::selectedIndex() const
{
    return m_selected;
}

int RedactionView::count() const
{
    return int(m_areas.size());
}

void RedactionView::removeSelected()
{
    if (m_selected < 0 || m_selected >= m_areas.size()) {
        return;
    }
    m_areas.remove(m_selected);
    m_selected = -1;
    update();
    Q_EMIT selectionChanged();
    Q_EMIT areasChanged();
}

void RedactionView::clear()
{
    if (m_areas.isEmpty()) {
        return;
    }
    m_areas.clear();
    m_selected = -1;
    update();
    Q_EMIT selectionChanged();
    Q_EMIT areasChanged();
}

void RedactionView::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event)
    QPainter painter(this);
    drawPage(painter);
    if (!hasPage()) {
        return;
    }

    for (int i = 0; i < m_areas.size(); ++i) {
        const QRect box = toWidget(m_areas.at(i));
        painter.fillRect(box, Qt::black);
        // The selected one is outlined rather than tinted, so that what is
        // shown stays exactly what will be produced.
        painter.setPen(QPen(i == m_selected ? palette().color(QPalette::Highlight) : QColor(255, 255, 255, 90),
                            i == m_selected ? 2 : 1));
        painter.drawRect(box.adjusted(0, 0, -1, -1));
    }

    if (m_drawing && m_rubberBand.width() > 0 && m_rubberBand.height() > 0) {
        painter.fillRect(m_rubberBand, QColor(0, 0, 0, 160));
        painter.setPen(QPen(palette().color(QPalette::Highlight), 1, Qt::DashLine));
        painter.drawRect(m_rubberBand.adjusted(0, 0, -1, -1));
    }
}

void RedactionView::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton || !hasPage()) {
        PageCanvas::mousePressEvent(event);
        return;
    }

    // Topmost first, so overlapping boxes select the one that looks on top.
    const int previous = m_selected;
    m_selected = -1;
    for (int i = int(m_areas.size()) - 1; i >= 0; --i) {
        if (toWidget(m_areas.at(i)).contains(event->pos())) {
            m_selected = i;
            break;
        }
    }
    if (m_selected != previous) {
        Q_EMIT selectionChanged();
    }

    if (m_selected < 0 && pageRect().contains(event->pos())) {
        m_drawing = true;
        m_origin = event->pos();
        m_rubberBand = QRect(m_origin, QSize());
    }
    update();
}

void RedactionView::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_drawing) {
        PageCanvas::mouseMoveEvent(event);
        return;
    }
    m_rubberBand = QRect(m_origin, event->pos()).normalized().intersected(pageRect());
    update();
}

void RedactionView::mouseReleaseEvent(QMouseEvent *event)
{
    if (!m_drawing || event->button() != Qt::LeftButton) {
        PageCanvas::mouseReleaseEvent(event);
        return;
    }

    m_drawing = false;
    if (m_rubberBand.width() >= minimumDragPixels && m_rubberBand.height() >= minimumDragPixels) {
        m_areas.append(fromWidget(m_rubberBand));
        m_selected = int(m_areas.size()) - 1;
        Q_EMIT selectionChanged();
        Q_EMIT areasChanged();
    }
    m_rubberBand = QRect();
    update();
}

void RedactionView::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) {
        removeSelected();
        return;
    }
    PageCanvas::keyPressEvent(event);
}

} // namespace ps
