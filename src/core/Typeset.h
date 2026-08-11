/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#pragma once

#include <QColor>
#include <QSizeF>
#include <QString>
#include <QStringList>
#include <QVector>

#include <Qt>

namespace ps {

/**
 * Turning text into a document somebody would be willing to print.
 *
 * Every desktop can already put a text file on paper, and the result always
 * looks like a text file on paper: one size, one weight, a page break wherever
 * the ruler ran out, and a heading stranded at the foot of a page with its
 * paragraph overleaf. The distance between that and a typeset page is not
 * decoration, it is a handful of rules that professional composition has
 * followed for four centuries, and they are what this class implements.
 *
 * ## The rules it keeps
 *
 * - Lines are measured with the font's real advance widths, from
 *   Core14Widths.h, not with an average. An average is wrong by a percent or
 *   two per character, which is a whole word by the end of a line.
 * - Justification stretches the spaces between words with `Tw` and leaves the
 *   last line of a paragraph alone. A stretched last line is the one mistake
 *   that marks a page as machine-set at a glance.
 * - No paragraph leaves a single line behind at a page foot or takes a single
 *   line over to the next page. The line goes with its fellows instead.
 * - A heading never ends a page. Style::keepWithNext says so, and headings get
 *   it by default, because a heading whose text starts overleaf is the most
 *   visible typesetting failure there is.
 * - Nothing is dropped in silence. A word too wide for its column is broken
 *   and named in Report::overflows; a character the standard fonts cannot
 *   write is named there too, rather than appearing as some other glyph.
 *
 * ## What comes out
 *
 * A new PDF, one content stream per page, using the fourteen standard fonts
 * with `/WinAnsiEncoding`. That means no font is embedded, so the file stays a
 * few kilobytes, every reader on earth can show it, and `pdftotext` gets the
 * words back in order, which is the test that a "text to PDF" converter has
 * actually produced text rather than a picture of text.
 *
 * The price of standard fonts is the alphabet: WinAnsi covers western Europe
 * and no more. Greek, Cyrillic, Arabic, Hebrew and CJK need an embedded font,
 * and asking for them here gets an honest report rather than a row of wrong
 * glyphs.
 */
class Typeset
{
public:
    struct Style {
        QString family = QStringLiteral("Helvetica"); //!< one of the standard fourteen
        double fontSize = 11.0;
        double leading = 0.0; //!< 0 means 1.35 times the size
        QColor colour = Qt::black;
        bool bold = false;
        bool italic = false;
        Qt::Alignment alignment = Qt::AlignLeft; //!< AlignJustify supported
        double spaceBefore = 0.0;
        double spaceAfter = 0.0;
        double indentFirst = 0.0;
        double indentLeft = 0.0;
        double indentRight = 0.0;
        bool keepWithNext = false; //!< do not leave this line alone at a page foot
    };

    struct Document {
        QSizeF pageSize = QSizeF(595.276, 841.89); //!< A4
        double marginTop = 56.7, marginBottom = 56.7, marginLeft = 56.7, marginRight = 56.7;
        int columns = 1;
        double columnGap = 14.0;
        QString header, footer; //!< may hold {page}, {pages}, {title}, {date}
        double headerSize = 9.0;
        QString title, author;

        /**
         * Style for body text, and for heading levels one to six.
         *
         * Leaving @c headings short is the usual case and is not a mistake: the
         * levels that are missing are derived from @c body, so a caller who only
         * wants a different body font gets headings that match it.
         */
        Style body;
        QVector<Style> headings;

        bool hyphenate = false;
        QString language = QStringLiteral("en");
    };

    struct Report {
        int pages = 0;
        int paragraphs = 0;

        /**
         * Everything the reader of the finished document might otherwise notice
         * before the author does.
         *
         * Words that had to be broken because no column could hold them, and
         * characters the standard fonts cannot write and which were therefore
         * left out. Each is named once, however often it occurred.
         */
        QStringList overflows;
    };

    /** Plain text: blank lines separate paragraphs, nothing else is interpreted. */
    static bool fromPlainText(const QString &text, const QString &outPdf, const Document &document, Report *report,
                              QString *error);

    /**
     * Markdown, as much of it as makes sense on paper.
     *
     * Headings, paragraphs, bulleted and numbered lists with nesting, block
     * quotes, fenced and indented code, horizontal rules, bold, italic, inline
     * code, and links rendered as text with the target in parentheses. Tables
     * become a simple ruled grid. What is deliberately left out is said in
     * limitations().
     */
    static bool fromMarkdown(const QString &markdown, const QString &outPdf, const Document &document, Report *report,
                             QString *error);

    /** Markdown when the name says so (.md, .markdown, .mdown, .mkd), plain text otherwise. */
    static bool fromTextFile(const QString &path, const QString &outPdf, const Document &document, Report *report,
                             QString *error);

    /**
     * The width of a string in a standard font, in points. Public because callers need to measure.
     *
     * Characters WinAnsiEncoding cannot carry count as nothing, because that is
     * what happens to them on the page as well; a measurement that disagreed
     * with the output would be worse than useless.
     */
    static double textWidth(const QString &text, const QString &family, bool bold, bool italic, double fontSize);

    static QStringList limitations();
};

} // namespace ps
