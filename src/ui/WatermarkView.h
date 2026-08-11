/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#pragma once

#include "PageCanvas.h"
#include "core/Overlay.h"

#include <QImage>

namespace ps {

/**
 * The real page with the watermark drawn over it.
 *
 * The old preview was a white rectangle with the word on it, which answers
 * none of the questions anyone actually has: is it over the text, is it dark
 * enough to see and light enough to read through, does it collide with the
 * logo in the corner. Those are questions about *this* page, so the preview is
 * this page.
 *
 * The placement arithmetic here mirrors what Overlay does rather than sharing
 * it (the engine writes PDF operators and this draws with QPainter), so it is
 * a faithful preview, not a guarantee. Where the two could drift, the anchor
 * and the margin are the parts kept deliberately identical, because those are
 * what people adjust.
 */
class WatermarkView : public PageCanvas
{
    Q_OBJECT

public:
    explicit WatermarkView(QWidget *parent = nullptr);

    void setTextStamp(const Overlay::TextStamp &stamp);

    /** An empty image goes back to showing the text stamp. */
    void setImageStamp(const QImage &image, Overlay::Anchor anchor, double relativeWidth, double rotation,
                       double opacity);

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    /** Where the anchor sits inside the page, in widget pixels. */
    QPointF anchorPoint(Overlay::Anchor anchor, double marginPoints) const;

    Overlay::TextStamp m_stamp;

    QImage m_image;
    Overlay::Anchor m_imageAnchor = Overlay::Anchor::Centre;
    double m_imageWidth = 0.4;
    double m_imageRotation = 0.0;
    double m_imageOpacity = 0.5;
    bool m_usesImage = false;
};

} // namespace ps
