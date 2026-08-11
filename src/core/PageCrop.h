/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#pragma once

#include <QRectF>
#include <QString>
#include <QVector>

namespace ps {

class RenderBackend;

/**
 * Trimming the empty edges off pages.
 *
 * Done by setting /CropBox rather than by cutting anything out: the content is
 * all still there, readers simply show less of it, and undoing the crop later
 * costs nothing. That also means a scan keeps its exact pixels, because cropping by
 * re-rendering would be the third way this project could quietly degrade a
 * document, and it does not take any of them.
 */
class PageCrop
{
public:
    /**
     * How much to take off each side, in points, measured on the page **as
     * displayed**, so "left" is the left the reader sees, whatever /Rotate
     * says.
     */
    struct Margins {
        double left = 0.0;
        double right = 0.0;
        double top = 0.0;
        double bottom = 0.0;

        bool isEmpty() const { return left <= 0.0 && right <= 0.0 && top <= 0.0 && bottom <= 0.0; }
    };

    static bool crop(const QString &inputPdf, const QString &outputPdf, const QVector<int> &pages,
                     const Margins &margins, QString *error);

    /** Removes any crop, showing the full page again. */
    static bool reset(const QString &inputPdf, const QString &outputPdf, const QVector<int> &pages, QString *error);

    /**
     * Finds the white border around a page by rendering it and looking.
     *
     * @p threshold is the brightness above which a pixel counts as paper.
     * @p keepPoints is left in place so the trim does not shave the ink.
     */
    static Margins detect(RenderBackend *backend, int sourceId, int page, int threshold = 247, double keepPoints = 6.0);

    /**
     * The largest trim that is safe for *every* listed page.
     *
     * Cropping each page to its own content would make a document whose pages
     * are all slightly different sizes, which looks worse than the margins did.
     */
    static Margins detectCommon(RenderBackend *backend, int sourceId, const QVector<int> &pages, int threshold = 247,
                                double keepPoints = 6.0);
};

} // namespace ps
