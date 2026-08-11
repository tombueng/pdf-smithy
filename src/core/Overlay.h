/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#pragma once

#include <QColor>
#include <QHash>
#include <QImage>
#include <QRectF>
#include <QString>
#include <QVector>

namespace ps {

/**
 * Draws things on top of pages without disturbing what is underneath.
 *
 * Signatures, watermarks and page numbers are all the same operation: embed an
 * image or some text as a resource, then append a short content stream that
 * places it. The page's own content is never decoded or re-encoded, so a
 * signed scan is byte-for-byte the scan it was, plus a signature.
 *
 * Coordinates are given in **display** points with the origin at the bottom
 * left of the page as the user sees it; rotation is dealt with internally, so
 * callers never have to think about /Rotate.
 */
class Overlay
{
public:
    enum class Anchor {
        TopLeft,
        TopCentre,
        TopRight,
        CentreLeft,
        Centre,
        CentreRight,
        BottomLeft,
        BottomCentre,
        BottomRight,
    };

    struct ImageStamp {
        QImage image;

        /** Where it goes, in display points. */
        QRectF rect;

        /** Degrees counter-clockwise about the rectangle's own centre. */
        double rotation = 0.0;

        /** 0 is invisible, 1 is opaque. */
        double opacity = 1.0;
    };

    struct TextStamp {
        QString text;
        double fontSize = 48.0;
        QColor colour = QColor(200, 30, 30);
        double rotation = 45.0;
        double opacity = 0.25;
        Anchor anchor = Anchor::Centre;

        /** Outline instead of solid, which keeps the page underneath readable. */
        bool outlineOnly = false;

        /** Distance from the page edge for the non-centred anchors. */
        double marginPoints = 36.0;
    };

    /**
     * Stamps images onto the pages listed in @p stamps, keyed by zero-based
     * page index. Pages not listed are left completely untouched.
     */
    static bool stampImages(const QString &inputPdf, const QString &outputPdf,
                            const QHash<int, QVector<ImageStamp>> &stamps, QString *error);

    /** Puts the same text on every page in @p pages. */
    static bool stampText(const QString &inputPdf, const QString &outputPdf, const QVector<int> &pages,
                          const TextStamp &stamp, QString *error);

    /**
     * Puts a different text on each page, keyed by zero-based page index.
     *
     * What page numbers, headers and Bates stamping all need: the same
     * treatment everywhere but the words changing per page.
     */
    static bool stampTexts(const QString &inputPdf, const QString &outputPdf, const QHash<int, TextStamp> &stamps,
                           QString *error);

    /** Puts the same image on every page in @p pages, sized to @p relativeWidth of the page. */
    static bool stampImageOnPages(const QString &inputPdf, const QString &outputPdf, const QVector<int> &pages,
                                  const QImage &image, Anchor anchor, double relativeWidth, double rotation,
                                  double opacity, double marginPoints, QString *error);

    /** Places @p itemSize against @p anchor inside a page of @p pageSize. */
    static QRectF anchoredRect(Anchor anchor, const QSizeF &pageSize, const QSizeF &itemSize, double marginPoints);

    /** Rough width of @p text in Helvetica-Bold at @p fontSize, in points. */
    static double estimateTextWidth(const QString &text, double fontSize);
};

} // namespace ps
