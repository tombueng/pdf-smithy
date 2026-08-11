/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#pragma once

#include "PageRef.h"

#include <QSizeF>
#include <QString>
#include <QStringList>
#include <QVector>

namespace ps {

class RenderBackend;

/** Pictures in, pictures out. */
class ImageIO
{
public:
    // ── Images to PDF ─────────────────────────────────────────────────────

    struct ImportOptions {
        /**
         * Page size in points. When empty, each page is made exactly the size
         * of its image at @p assumedDpi, which is what someone importing a
         * stack of scans almost always wants, and what stops a 300 dpi A4 scan
         * arriving as a poster.
         */
        QSizeF pageSizePoints;

        /** Resolution assumed for images that do not say what theirs is. */
        int assumedDpi = 300;

        /** White space around the image when a fixed page size is used. */
        double marginPoints = 0.0;

        /**
         * Re-encode as JPEG at this quality. Zero keeps the pixels exactly as
         * they are, which is right for line art and much larger for photos.
         */
        int jpegQuality = 85;
    };

    /** One page per image, in the order given. */
    static bool imagesToPdf(const QStringList &imagePaths, const QString &outputPdf, const ImportOptions &options,
                            QString *error);

    /** Image formats Qt can read on this machine, as file dialog filters. */
    static QStringList readableImageFilters();

    // ── PDF to images ─────────────────────────────────────────────────────

    struct ExportOptions {
        int dpi = 150;

        /** "png", "jpeg" or "tiff". */
        QString format = QStringLiteral("png");

        /** For the lossy formats, 1 to 100. */
        int quality = 90;
    };

    /**
     * Renders @p pages, writing files named from @p outputTemplate.
     *
     * Takes whole PageRefs rather than a single source and a list of page
     * numbers, because a document assembled from several files has a different
     * source per page. The older shape could only ever be right for a document
     * that came from one file, and quietly rendered the wrong pages of the
     * wrong file for every other, and nothing reported it, because the pages it
     * could not find were skipped rather than counted.
     *
     * The template takes a %1 placeholder for the page number, padded so the
     * files sort correctly. Names actually written are appended to @p written.
     * Pages that could not be rendered are appended to @p skipped as their
     * position in @p pages, one-based: a partial export must be able to say so,
     * since a caller that only counts what it got cannot tell the difference
     * between eight pages and the two of them that worked.
     */
    static bool pagesToImages(RenderBackend *backend, const QVector<PageRef> &pages, const QString &outputTemplate,
                              const ExportOptions &options, QStringList *written, QVector<int> *skipped,
                              QString *error);

    /** The same for a document that came from one file. */
    static bool pagesToImages(RenderBackend *backend, int sourceId, const QVector<int> &pages,
                              const QVector<int> &rotations, const QString &outputTemplate,
                              const ExportOptions &options, QStringList *written, QString *error);
};

} // namespace ps
