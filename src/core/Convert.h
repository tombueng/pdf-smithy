/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

namespace ps {

/**
 * Getting the content out of a PDF, and print-ready PDF in.
 *
 * ## Why this is inference and not extraction
 *
 * There is no text in a PDF in the sense a word processor means it. There are
 * glyphs at coordinates, and nothing in the file says which glyph follows which
 * in the sentence, where a paragraph ends, or that these two blocks of glyphs
 * are the left and right columns of one article. A reader supplies all of that
 * from the picture; a converter has to reconstruct it from the geometry.
 *
 * So every method here is a reconstruction, and the honest thing is to say
 * where each one guesses. The geometry is read through TextEdit::runsOn(),
 * which already does the hard part (decoding the bytes into characters through
 * `/ToUnicode`, named encodings and `/Differences`) and hands back positioned
 * runs. What is added here is the layout reasoning on top: which runs are one
 * line, which lines are one column, which lines are one paragraph.
 *
 * ## How the layout is worked out
 *
 * By recursive cutting, the oldest idea in page segmentation and still the one
 * that behaves best on documents nobody designed for a machine. A page's runs
 * are projected onto one axis at a time; a band of white space that no run
 * crosses is a cut; the page is split there and each half is cut again. Cutting
 * across the page first peels off the headline and the running foot, and only
 * then does a full-height gap in the middle of the remainder count as the
 * gutter between two columns, which is what stops a headline that spans both
 * columns from hiding the gutter underneath it.
 *
 * A vertical cut is only accepted when both sides really look like columns:
 * each has to span most of the height of what is being cut, and its lines have
 * to reach the far side of it the way set text does. Without those two tests
 * the date in the corner of a letter becomes a column of its own, and the whole
 * letter comes out in the wrong order.
 *
 * ## Where it is wrong
 *
 * Deliberately listed in limitations() rather than buried here, but the short
 * version: text that is part of a scanned picture is not text and does not come
 * out; a table is read column by column unless findTables() is used; and
 * anything set in more than two columns with the columns interleaved by hand
 * will confuse it.
 *
 * Ghostscript, where it is needed, is run as a separate process and never
 * linked, exactly as Archival and Compressor do it, so its AGPL obligation
 * stays outside this codebase and the dependency stays optional.
 */
class Convert
{
public:
    struct TextOptions {
        /** Keep the reading order and the paragraph breaks, rather than one line per run. */
        bool preserveLayout = true;

        /** Two runs whose baselines differ by less than this are the same line. */
        double lineTolerance = 2.0;

        /** A vertical gap larger than this many times the font size starts a paragraph. */
        double paragraphFactor = 1.6;

        /** Zero-based page indexes; empty means all. */
        QVector<int> pages;
    };

    /**
     * Plain text with the paragraphs a person would see.
     *
     * One output line per line on the page, not per text-showing operator: a
     * line is routinely drawn in six pieces because of a quotation mark in
     * another font or a hyphen at the end, and one output line each would be
     * six lines of nonsense. Paragraphs are separated by a blank line, columns
     * are emitted one after the other in reading order, and pages by a form
     * feed, which is the convention every other text extractor follows.
     *
     * Turning off @ref TextOptions::preserveLayout gives the raw thing instead:
     * one line per run, top of the page downwards, with no paragraphs and no
     * columns. That is occasionally what somebody debugging a document wants,
     * and never what a reader wants.
     */
    static bool toText(const QString &pdf, const QString &outFile, const TextOptions &options, QString *error);

    /**
     * Markdown: headings guessed from font size, lists from leading characters,
     * emphasis from the font name.
     *
     * All three are inference and all three have a shape of document they get
     * wrong. The headings are findHeadings()' own, paragraph for paragraph and
     * level for level, and its documentation is where what they get wrong is
     * written down. A list item is a line that starts with a bullet or a number
     * and a full stop, so a sentence beginning "1902. Ein gutes Jahr" becomes a
     * numbered list. Emphasis comes from the embedded font's own name
     * containing "Bold" or "Italic", so a family whose bold face is named
     * "Black" or "Heavy" is caught, and one named after its designer is not.
     *
     * Unlike toText(), the lines of a paragraph are joined into flowing text,
     * because that is what Markdown is for. A word broken by a hyphen at the
     * end of a line is put back together when the next line starts in lower
     * case, which is right in German and English and wrong for a compound whose
     * hyphen was real.
     */
    static bool toMarkdown(const QString &pdf, const QString &outFile, const TextOptions &options, QString *error);

    struct Heading {
        /** Zero-based. */
        int page = 0;

        /**
         * 1 for the largest heading size in the document, 2 for the next, and so on.
         *
         * A rank among the sizes that were found, not something the file says.
         * No PDF records that a paragraph is a heading, still less how deep it
         * sits, so there is nothing here to read, only sizes to compare.
         */
        int level = 1;

        /** The heading itself, its lines joined and its white space collapsed. */
        QString text;
    };

    /**
     * The headings of a document, worked out from how big its text is set.
     *
     * The one implementation of that inference: toMarkdown() and toHtml() rank
     * their own headings through it, and Outline::fromHeadings() turns what it
     * returns into a table of contents for a document that has none.
     *
     * How it works, in full, because a caller has to know before trusting the
     * result. The size most of the document's characters are set in is its body
     * size, meaning the commonest size rather than the average, which would
     * land between the body and the headings and describe neither. Every
     * distinct size more than fifteen per cent larger than the body size is a
     * heading size; sorted largest first, a size's position in that list is its
     * level. A paragraph is a heading when it is set in one of those sizes and
     * is no more than three lines long. Sizes are rounded to the nearest half
     * point throughout, and measured from the height of the text's box rather
     * than from the `Tf` operator, which a great many producers set to 1 and
     * then scale.
     *
     * ## Where it is wrong
     *
     * - A heading distinguished by anything other than size is not found at
     *   all. Bold at body size, small capitals, letter-spacing, a rule above
     *   it, white on a coloured bar: none of that is looked at, so a document
     *   whose headings are bold and no bigger yields nothing.
     * - A document set in one size throughout, such as a contract, a plain
     *   report or a letter, likewise yields nothing. That is the honest
     *   answer and it is also no table of contents.
     * - A document with no settled body size, such as a poster, a form or a
     *   title page read on its own, has whatever size happens to be commonest
     *   taken as its body, and produces headings out of nowhere.
     * - Numbering is not read. "1.2.3 Anwendungsbereich" is a third-level
     *   heading to a reader and, here, whatever its size says it is, so a
     *   document that sets every depth of sub-heading in one size has all of
     *   them come back at one level, and one that sets a single chapter title
     *   half a point larger than its fellows gains a level nobody intended.
     * - Anything else set large is a heading too: a pull quote, a drop
     *   capital, the number on a numbered page, a word in a chart. A running
     *   head set larger than the body becomes a heading on every page it
     *   appears on.
     * - Text that is part of a scanned picture is not text and is not seen.
     *   A scan that has not been through text recognition yields nothing.
     * - There are six levels and no more; anything deeper is reported as six.
     *
     * @p pages restricts which pages are read, empty meaning all of them.
     * Restricting it also restricts what the body size is measured from, so one
     * page of a book read on its own can rank its headings differently from the
     * way the whole book ranks them.
     */
    static QVector<Heading> findHeadings(const QString &pdf, const QVector<int> &pages, QString *error);

    /**
     * One HTML file with the pictures embedded as data URIs, so it is a single
     * file.
     *
     * Deliberately not one absolutely positioned box per run. That is the easy
     * answer, it reproduces the page exactly, and it produces a file that is
     * unreadable on a telephone, unusable by a screen reader and unsearchable
     * by anything that expects paragraphs. What comes out instead is flowing
     * text, with headings, paragraphs, lists, and the pictures in the place in
     * the flow where they sat on the page, which loses the design and keeps the
     * document.
     */
    static bool toHtml(const QString &pdf, const QString &outFile, const TextOptions &options, QString *error);

    /**
     * One SVG per page, via Poppler's Cairo backend or Ghostscript, whichever is present.
     *
     * Both are separate processes: `pdftocairo` is preferred because it keeps
     * the text as text, and Ghostscript's own SVG device has been withdrawn, so
     * the fallback there goes through PostScript and is only worth having when
     * `pdftocairo` is missing.
     *
     * @p outTemplate takes a `%1` placeholder for the page number, padded so
     * the files sort correctly. A template without one gets the number inserted
     * before the extension.
     */
    static bool toSvg(const QString &pdf, const QString &outTemplate, const QVector<int> &pages, QString *error);

    struct Table {
        /** Zero-based. */
        int page = 0;

        QVector<QStringList> rows;

        /** How sure the detection is, 0..1. */
        double confidence = 0.0;
    };

    /**
     * Tables found by looking at where the text sits.
     *
     * There is no table in a PDF, only text at coordinates, so this is
     * inference, and `confidence` says how much of one. Runs that line up in
     * columns across several rows are the signal; ruled lines, when present,
     * raise it.
     *
     * The awkward case is prose in two columns, which lines up just as well as
     * a table does and is not one. What separates them is how much of its
     * column each cell fills: set text runs to the far edge of its measure and
     * a table cell does not, so a candidate whose cells fill their columns is
     * rejected. That is a judgement, and it will occasionally reject a table of
     * long sentences.
     */
    static QVector<Table> findTables(const QString &pdf, const QVector<int> &pages, QString *error);

    /** The same tables as comma-separated values, one row per line, quoted where needed. */
    static bool tablesToCsv(const QString &pdf, const QString &outFile, const QVector<int> &pages, QString *error);

    enum class XLevel {
        X1a, //!< everything converted to CMYK, no transparency; what most printers still ask for
        X3, //!< colour may stay device-independent and be converted by the printer
        X4, //!< transparency and layers survive, which modern print workflows want
    };

    /**
     * PDF/X for commercial print, via Ghostscript, with the output intent it
     * needs.
     *
     * PDF/X is the other half of PDF/A's idea: where an archive wants a file
     * that will still look like itself in fifty years, a printer wants one that
     * will come off the press looking like the proof. Both insist the file says
     * what its colours mean rather than assuming, which is what the output
     * intent is for, and an output intent for print is a CMYK profile
     * describing a real printing condition, not sRGB.
     *
     * Ghostscript does the conversion; the standard's identification in XMP is
     * written afterwards with QPDF, because Ghostscript's PDF/X mode puts
     * `/GTS_PDFXVersion` in the document information dictionary and stops
     * there, and every version of the standard from 2003 onwards wants it in
     * the metadata as well.
     *
     * @p iccProfilePath is a CMYK output profile. Left empty, a common one is
     * looked for on the system, and its absence is reported as a warning rather
     * than guessed around, because guessing a printing condition is how a job
     * comes back the wrong colour.
     */
    static bool toPdfX(const QString &in, const QString &out, XLevel level, const QString &iccProfilePath,
                       QStringList *changes, QStringList *warnings, QString *error);

    /** Whether Ghostscript, which does the PDF/X conversion, is installed. */
    static bool isPdfXAvailable();

    /**
     * Rewrites the file so a reader can show page one before it has the rest.
     *
     * Worth almost nothing on a local disk and a great deal on a slow link,
     * which is the only reason the format has the feature.
     */
    static bool linearise(const QString &in, const QString &out, QString *error);

    /**
     * Sets the claimed PDF version, refusing to claim a version older than the
     * features used.
     *
     * The version in a PDF's header is a claim about which readers can make
     * sense of it, and lowering it does not remove anything: it only tells a
     * reader that will then meet a structure it was never taught. So a request
     * to go below what the file actually needs is refused, and @p warnings says
     * which feature forced the refusal and which version it needs.
     */
    static bool setVersion(const QString &in, const QString &out, const QString &version, QStringList *warnings,
                           QString *error);

    /** Plain-language notes about what these conversions cannot promise. */
    static QStringList limitations();
};

} // namespace ps
