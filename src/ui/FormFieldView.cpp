/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "FormFieldView.h"

#include <QMouseEvent>
#include <QPainter>

namespace ps {

FormFieldView::FormFieldView(QWidget *parent)
    : PageCanvas(parent)
{
    setMouseTracking(true);
    setCursor(Qt::PointingHandCursor);
}

void FormFieldView::setFields(const QVector<FormField> &fields, const QVector<int> &indices)
{
    m_fields = fields;
    m_indices = indices;
    m_current = -1;
    update();
}

void FormFieldView::setCurrentField(int index)
{
    if (m_current == index) {
        return;
    }
    m_current = index;
    update();
}

void FormFieldView::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event)

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    drawPage(painter);

    if (!hasPage()) {
        return;
    }

    for (int i = 0; i < m_fields.size(); ++i) {
        const QRect box = toWidget(fromPoints(m_fields.at(i).rect));
        if (box.isEmpty()) {
            continue;
        }

        const bool current = m_indices.value(i, -1) == m_current;
        const bool locked = m_fields.at(i).readOnly;

        // A locked field is shown, not hidden: knowing that the box exists and
        // cannot be typed into is the answer to "why can I not fill this in",
        // and leaving it off the page turns that into a hunt.
        QColor fill = locked ? QColor(128, 128, 128) : palette().color(QPalette::Highlight);
        fill.setAlpha(current ? 70 : 28);
        painter.setPen(Qt::NoPen);
        painter.setBrush(fill);
        painter.drawRect(box);

        QColor edge = locked ? QColor(110, 110, 110) : palette().color(QPalette::Highlight);
        painter.setPen(QPen(edge, current ? 2.0 : 1.0, locked ? Qt::DashLine : Qt::SolidLine));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(box);

        if (m_fields.at(i).required) {
            // A small mark rather than a word: the label already carries the
            // asterisk, and text at this size on a page thumbnail is unreadable.
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(200, 60, 60));
            painter.drawEllipse(QPoint(box.right() - 4, box.top() + 4), 3, 3);
        }
    }
}

void FormFieldView::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton || !hasPage()) {
        PageCanvas::mousePressEvent(event);
        return;
    }

    // Back to front, so that the field drawn on top of another is the one a
    // click on the overlap picks.
    for (int i = m_fields.size() - 1; i >= 0; --i) {
        if (m_fields.at(i).readOnly) {
            continue;
        }
        if (toWidget(fromPoints(m_fields.at(i).rect)).contains(event->pos())) {
            Q_EMIT fieldPicked(m_indices.value(i, -1));
            return;
        }
    }
    PageCanvas::mousePressEvent(event);
}

} // namespace ps
