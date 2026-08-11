/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#pragma once

#include "PageCanvas.h"

#include <QRectF>
#include <QVector>

namespace ps {

/**
 * A page you draw boxes on.
 *
 * Marking something for redaction is a pointing task, not a typing task, so
 * this is deliberately the plainest thing that works: drag to draw, click to
 * select, Delete to remove.
 *
 * The boxes are drawn solid black, because that is what the result will look
 * like: a preview that flatters is worse than none.
 */
class RedactionView : public PageCanvas
{
    Q_OBJECT

public:
    explicit RedactionView(QWidget *parent = nullptr);

    /** Rectangles in PDF points, origin at the bottom left of the page. */
    QVector<QRectF> areasInPoints() const;
    void setAreasInPoints(const QVector<QRectF> &areas);

    void removeSelected();
    void clear();
    int selectedIndex() const;
    int count() const;

Q_SIGNALS:
    void areasChanged();
    void selectionChanged();

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    /** 0..1 across and down the page, y measured downwards as images are. */
    QVector<QRectF> m_areas;

    int m_selected = -1;
    bool m_drawing = false;
    QPoint m_origin;
    QRect m_rubberBand;
};

} // namespace ps
