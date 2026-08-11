/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#pragma once

#include <QSize>
#include <QSizeF>
#include <QString>

namespace ps {

/**
 * Several pages onto one sheet, as a new document rather than as a print job.
 *
 * Printing already imposes pages; this is the other half of the same idea:
 * producing a file. It is what you want for handouts, for a reading copy that
 * fits in a bag, and for anything that has to be sent rather than printed.
 *
 * Nothing is rasterised. Each source page becomes a form XObject and is placed
 * on the sheet, so the text stays text, stays selectable, and stays searchable.
 * A four-up handout of a scanned book is still the same scan, at the same
 * resolution, just arranged differently.
 */
class NUp
{
public:
    struct Options {
        int columns = 2;
        int rows = 1;

        /**
         * Empty means: take the first page's size, turned to suit the grid.
         *
         * Two pages side by side on a portrait A4 sheet waste most of it; the
         * sensible sheet for a wide grid is a landscape one, and guessing that
         * correctly is more useful than making everyone say it.
         */
        QSizeF sheetSizePoints;

        double marginPoints = 14.0;
        double gutterPoints = 8.0;

        /** A hairline around each slot, which handouts are often clearer with. */
        bool drawBorders = false;

        /** Fill downwards before moving right, instead of the usual reading order. */
        bool columnsFirst = false;
    };

    /**
     * A grid for a plain "pages per sheet" number.
     *
     * @p sheetIsLandscape decides which way round an uneven split goes: six up
     * is 3×2 on a wide sheet and 2×3 on a tall one.
     */
    static QSize gridFor(int pagesPerSheet, bool sheetIsLandscape);

    /** Returns false and fills @p error when the document cannot be rearranged. */
    static bool apply(const QString &inputPdf, const QString &outputPdf, const Options &options, QString *error);
};

} // namespace ps
