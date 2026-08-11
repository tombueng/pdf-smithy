/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "SignatureCanvas.h"

#include <QMouseEvent>
#include <QPainter>

namespace ps {

SignatureCanvas::SignatureCanvas(QWidget *parent)
    : QWidget(parent)
{
    setMinimumSize(340, 130);
    setCursor(Qt::CrossCursor);
    setAttribute(Qt::WA_OpaquePaintEvent);
}

void SignatureCanvas::resizeEvent(QResizeEvent *event)
{
    const QSize target = size() * Supersample;
    if (m_strokes.size() != target) {
        QImage fresh(target, QImage::Format_ARGB32);
        fresh.fill(Qt::transparent);
        if (!m_strokes.isNull()) {
            QPainter painter(&fresh);
            painter.drawImage(0, 0, m_strokes);
        }
        m_strokes = fresh;
    }
    QWidget::resizeEvent(event);
}

void SignatureCanvas::clearCanvas()
{
    if (!m_strokes.isNull()) {
        m_strokes.fill(Qt::transparent);
    }
    m_touched = false;
    update();
    Q_EMIT changed();
}

bool SignatureCanvas::isEmpty() const
{
    return !m_touched;
}

QImage SignatureCanvas::signature() const
{
    if (!m_touched || m_strokes.isNull()) {
        return {};
    }
    // Already transparent where nothing was drawn, so only the empty margin
    // needs cutting away.
    return SignatureStore::trim(m_strokes);
}

void SignatureCanvas::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

    painter.fillRect(rect(), Qt::white);

    // A signature line, because a blank white box does not say "sign here".
    QColor guide(150, 160, 175);
    painter.setPen(QPen(guide, 1, Qt::DashLine));
    const int baseline = height() * 3 / 4;
    painter.drawLine(16, baseline, width() - 16, baseline);

    if (!m_strokes.isNull()) {
        painter.drawImage(rect(), m_strokes);
    }

    painter.setPen(QPen(QColor(120, 130, 145), 1));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(rect().adjusted(0, 0, -1, -1));
}

void SignatureCanvas::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) {
        return;
    }
    m_drawing = true;
    m_last = event->pos() * Supersample;
}

void SignatureCanvas::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_drawing || m_strokes.isNull()) {
        return;
    }

    const QPoint now = event->pos() * Supersample;

    QPainter painter(&m_strokes);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(QColor(20, 30, 90), 2.4 * Supersample, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.drawLine(m_last, now);

    m_last = now;
    m_touched = true;
    update();
    Q_EMIT changed();
}

void SignatureCanvas::mouseReleaseEvent(QMouseEvent *)
{
    m_drawing = false;
}

} // namespace ps
