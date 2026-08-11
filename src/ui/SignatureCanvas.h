/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#pragma once

#include "SignatureStore.h"

#include <QImage>
#include <QWidget>

namespace ps {

/**
 * A small sheet of paper to sign on with the mouse or a trackpad.
 *
 * Not everyone has a scanner to hand, and a signature drawn once and stored is
 * still a signature the user chose to apply. Strokes are drawn at four times
 * the displayed size and scaled down, which is what stops a mouse-drawn line
 * looking like a staircase.
 */
class SignatureCanvas : public QWidget
{
    Q_OBJECT

public:
    explicit SignatureCanvas(QWidget *parent = nullptr);

    /** The drawing, trimmed and with a transparent background. */
    QImage signature() const;

    bool isEmpty() const;

public Q_SLOTS:
    void clearCanvas();

Q_SIGNALS:
    void changed();

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    static constexpr int Supersample = 4;

    QImage m_strokes;
    QPoint m_last;
    bool m_drawing = false;
    bool m_touched = false;
};

} // namespace ps
