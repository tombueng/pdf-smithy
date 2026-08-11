/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#pragma once

#include <QRectF>
#include <QSizeF>
#include <QString>
#include <QStringList>
#include <QVector>

namespace ps {

/**
 * The paper a document is on, and the boxes that describe it.
 *
 * A PDF page carries five rectangles, and almost every tool shows the reader
 * only one of them. That is fine until the document has to be printed
 * commercially, at which point the other four decide whether the job comes back
 * right: the printer trims to /TrimBox, the ink runs to /BleedBox, the press
 * needs /MediaBox to be big enough to hold the marks, and a /TrimBox that
 * escapes its /MediaBox makes an imposition program do something nobody can
 * predict. This class is where those five live.
 *
 * Everything here is done by moving numbers and by placing existing content, so
 * nothing is ever rasterised or re-encoded. Resizing a page wraps its content
 * stream in `q … Q` rather than rebuilding it; splitting a page gives each half
 * a window onto the same untouched stream. A scanned book resized to A5 is the
 * same scan at the same resolution.
 *
 * Coordinates are PDF points throughout. A box is given as a QRectF whose
 * `x()`/`y()` are its **left and bottom** edges, because PDF's y axis points up, so
 * `y() + height()` is the top. Sizes handed to resize() and unify() are the
 * paper size **as the reader sees it**, so asking a sideways page for A4 gives a
 * sideways A4 page rather than a portrait one with the content on its side.
 */
class PageLayout
{
public:
    /** All five boxes of one page, in points. An invalid rect means the box is absent. */
    struct Boxes {
        QRectF media, crop, bleed, trim, art;
    };

    /**
     * The five boxes of every page, in order.
     *
     * Absent boxes come back invalid rather than filled in with the fallback the
     * specification prescribes. Guessing here would be actively harmful: a
     * prepress check has to be able to tell "this page has no /TrimBox" from
     * "this page's /TrimBox happens to equal its /CropBox", because only one of
     * those is a document ready for a press.
     */
    static QVector<Boxes> boxesOf(const QString &pdf, QString *error);

    /**
     * Writes the boxes of the listed pages.
     *
     * An invalid rect removes that box, letting it fall back the way the
     * specification says, except for @ref Boxes::media, where an invalid rect
     * leaves the existing /MediaBox alone, because a page without one is not a
     * page.
     *
     * Refuses an arrangement whose boxes do not nest, naming the offender. See
     * the class notes for why that matters more than it looks.
     */
    static bool setBoxes(const QString &in, const QString &out, const QVector<int> &pages, const Boxes &boxes,
                         QString *error);

    /**
     * Sets TrimBox and BleedBox from the CropBox with a given bleed, the commonest prepress fix.
     *
     * The trim becomes what the page currently shows, and the bleed that grown
     * on all four sides. The /MediaBox grows with it where it has to: bleed is
     * ink that runs past the cut, and it needs paper to run onto.
     */
    static bool setBleed(const QString &in, const QString &out, const QVector<int> &pages, double bleedPoints,
                         QString *error);

    enum class Fit {
        /** Scale the content to fill the new sheet, keeping its proportions. */
        Scale,
        /** Leave the content at its own size and put it in the middle of the new sheet. */
        Centre,
        /** Scale only when the content is too big, then centre, so a small page is not blown up. */
        ScaleAndCentre,
    };

    /** Changes the paper size, either scaling the content or placing it on a larger sheet. */
    static bool resize(const QString &in, const QString &out, const QVector<int> &pages, const QSizeF &sizePoints,
                       Fit fit, QString *error);

    /**
     * Brings every page to the size of the largest, or to a named size.
     *
     * An empty @p sizePoints means "the largest page in the document", measured
     * by area. Deliberately not "the widest by the tallest": a document of
     * portrait and landscape A4 would then land on a square sheet that no paper
     * tray has ever held.
     */
    static bool unify(const QString &in, const QString &out, const QSizeF &sizePoints, Fit fit, QString *error);

    /**
     * Splits each page down the middle, or at a given fraction, into two pages.
     *
     * The classic scanned-book fix, where one photograph holds two facing pages.
     * Each half becomes a page whose boxes frame its own side of the sheet while
     * both keep pointing at the one original content stream, so no pixel is
     * resampled and no glyph is redrawn, and the file barely grows.
     *
     * @p vertical cuts top to bottom, giving a left and a right half in that
     * order; the horizontal cut gives top then bottom. Both are measured on the
     * page as displayed, so /Rotate is accounted for, and @p atFraction runs
     * from the left for a vertical cut and from the top for a horizontal one.
     */
    static bool splitPages(const QString &in, const QString &out, const QVector<int> &pages, bool vertical,
                           double atFraction, QString *error);

    /**
     * Lays pairs of pages on top of one another, for overlaying a form and its filling.
     *
     * Page one of the overlay goes on page one of the base, and so on. With
     * @p repeatOverlay the overlay starts again from its first page when it runs
     * out, which is what a letterhead or a background wants; without it the
     * remaining base pages are left exactly as they were.
     *
     * The overlay is scaled to fit the base page's visible area, so a letterhead
     * drawn a millimetre off still lands square.
     */
    static bool overlayPages(const QString &basePdf, const QString &overlayPdf, const QString &out, bool repeatOverlay,
                             QString *error);

    struct Marks {
        bool cropMarks = false;
        bool registrationMarks = false;
        bool colourBars = false;
        bool pageInformation = false;
        double offsetPoints = 6.0;
        double lengthPoints = 18.0;
        double lineWidth = 0.25;
    };

    /**
     * Adds printer's marks outside the trim area, growing the sheet if it must.
     *
     * Marks are drawn in a /Separation /All colour space, which is the only way
     * to be sure they appear on every plate: a registration mark that came out
     * on the cyan plate alone would be worse than none.
     *
     * Growing the sheet moves nothing: the /MediaBox is simply enlarged around
     * the content where it stands, so the page's own coordinates and every
     * annotation on it stay where they were. The /CropBox is enlarged to match,
     * because marks nobody can see are not marks.
     */
    static bool addMarks(const QString &in, const QString &out, const Marks &marks, QString *error);

    /**
     * One page across several sheets, with overlap, for a poster.
     *
     * The page is tiled at its own size; to make a wall-sized poster out of an
     * A4 page, resize() it first and then tile that. Splitting the two apart
     * keeps the arithmetic honest, because a single call that both enlarged and tiled
     * would have to guess which of the two the given sheet size referred to.
     *
     * @p marks draws the cut line along every edge that a neighbouring sheet
     * overlaps, and numbers each sheet, which is what makes the assembly
     * possible at all.
     */
    static bool poster(const QString &in, const QString &out, int page, const QSizeF &sheetPoints, double overlapPoints,
                       bool marks, int *sheets, QString *error);

    struct Labels {
        enum class Style { Decimal, RomanUpper, RomanLower, LetterUpper, LetterLower, None };

        /** Zero-based index of the first page this style applies to. */
        int fromPage = 0;

        Style style = Style::Decimal;
        QString prefix;
        int startAt = 1;
    };

    /**
     * The numbers a reader shows in its page box: "iv" for the fourth front-matter page.
     *
     * Each range runs until the next one begins. Written as the /PageLabels
     * number tree, which several readers treat as all-or-nothing: one malformed
     * entry and they fall back to showing physical page numbers everywhere, so
     * the tree is built through QPDF's own number-tree helper rather than by
     * hand.
     */
    static bool setLabels(const QString &in, const QString &out, const QVector<Labels> &ranges, QString *error);

    /** The label of every page, or an empty list when the document has none. */
    static QStringList labelsOf(const QString &pdf, QString *error);

    /**
     * Pages that carry no visible content, worth finding before printing them.
     *
     * Read from the content streams rather than by rendering: a page whose
     * streams hold no painting operator paints nothing, and that can be
     * established without a rasteriser and without an opinion about colour.
     *
     * @p inkThreshold is how much painting a page may still contain and count as
     * blank, counted in painting operators: 0 insists on a page that paints
     * nothing whatsoever, while 2 forgives a running head and a page number.
     */
    static QVector<int> findBlankPages(const QString &pdf, double inkThreshold, QString *error);

    static bool removePages(const QString &in, const QString &out, const QVector<int> &pages, QString *error);

    /**
     * Reader spreads to printer spreads and back.
     *
     * Eight pages become 8, 1, 2, 7, 6, 3, 4, 5, which is the order in which a
     * saddle-stitched booklet has to be laid down. The pages are reordered, not
     * paired onto sheets; run NUp two-up over the result to get the sheets
     * themselves. Keeping the two apart means the fold order can be checked
     * against a page count without also checking a placement.
     */
    static bool reorderForBinding(const QString &in, const QString &out, bool toPrinterSpreads, QString *error);

    /** Plain-language notes about what this cannot promise. */
    static QStringList limitations();
};

} // namespace ps
