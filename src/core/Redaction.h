/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#pragma once

#include <QImage>
#include <QRectF>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>

namespace ps {

/**
 * Removing content, not covering it.
 *
 * A black rectangle drawn over a name is not redaction: the name is still in
 * the file, and anyone who selects the text or opens it in an editor has it.
 * That mistake has ended careers and lost court cases, so this class does the
 * only thing that counts: it takes the content out of the page's instruction
 * stream, and only then paints the black box.
 *
 * ## How it decides what goes
 *
 * The content stream is walked operator by operator, tracking the two matrices
 * that decide where anything lands: the graphics state (`cm`, `q`, `Q`) and
 * the text state (`Tm`, `Td`, `TD`, `T*`, `TL`, `Tf`, `Tz`, `Tc`, `Tw`, `Ts`).
 * Every glyph gets a box of its own, and glyphs whose box touches a redaction
 * area are dropped.
 *
 * Dropping happens per glyph rather than per operator, because a whole line of
 * text is usually drawn by one `Tj`: throwing away the operator would take the
 * rest of the line with it. Each dropped glyph is replaced by the equivalent
 * `TJ` spacing number, so everything that survives stays exactly where it was
 * and the text matrix ends up where the original left it.
 *
 * ## Images
 *
 * An image that overlaps an area is decoded, the area is painted out in its
 * pixels, and it is re-embedded under a fresh name, so a second placement of
 * the same image elsewhere on the page keeps its pixels. Images in formats
 * this cannot decode (CCITT and JBIG2 fax, JPEG 2000) leave a choice between
 * destroying the page and not redacting it; rather than pick silently, supply
 * Options::pageRasteriser and the page is flattened to a picture instead.
 * Without one, the image is removed and the report says so.
 *
 * ## Where it errs
 *
 * Deliberately towards removing too much. Character widths come from the
 * font's own `/Widths` where the font has them, and from a generous estimate
 * where it does not: an over-wide estimate deletes a character more than
 * asked, an under-wide one leaves half a name behind. Only one of those two
 * failures matters.
 */
class Redaction
{
public:
    struct Area {
        /** Zero-based page index. */
        int page = 0;

        /** In display points, origin at the bottom left of the page as shown. */
        QRectF rect;
    };

    struct Options {
        /**
         * Renders a page, for the case where a picture on it cannot be decoded.
         *
         * Called with a zero-based page index and a resolution; the image is
         * expected in display orientation, the same way a viewer shows it.
         * Leaving this unset is safe: it costs the picture rather than the
         * page's text.
         */
        std::function<QImage(int pageIndex, double dpi)> pageRasteriser;

        double rasterDpi = 300.0;

        /** Draw the black box. Off leaves the hole, which is occasionally what is wanted. */
        bool paintBox = true;
    };

    struct Report {
        int areasApplied = 0;
        int glyphsRemoved = 0;
        int imagesEdited = 0;
        int imagesRemoved = 0;
        int formsRemoved = 0;
        int annotationsRemoved = 0;
        QVector<int> pagesRasterised;

        /** Anything the user should know before trusting the result. */
        QStringList warnings;
    };

    /**
     * Removes everything inside @p areas and paints them black.
     *
     * The black box is drawn last and is cosmetic: the content is gone whether
     * or not anyone looks at the result.
     */
    static bool apply(const QString &inputPdf, const QString &outputPdf, const QVector<Area> &areas,
                      const Options &options, Report *report, QString *error);

    /** Plain-language notes about what this cannot promise. */
    static QStringList limitations();
};

} // namespace ps
