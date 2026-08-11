/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#pragma once

// For Convert::Heading. A table of contents built out of a document that has
// none can only come from what the converter infers, so the dependency is real
// rather than incidental.
#include "Convert.h"

#include <QColor>
#include <QString>
#include <QVector>

class QPDF;

namespace ps {

/**
 * One entry in a document's table of contents.
 *
 * The same structure serves two purposes, and the fields say which is in play.
 * When a file is read or written, @ref page is an index into that file. While a
 * document is being edited, @ref sourceId and @ref sourcePage anchor the entry
 * to the page itself, so that reordering or deleting pages moves the bookmark
 * with its page rather than leaving it pointing at whatever now happens to sit
 * at that number.
 */
struct OutlineItem {
    QString title;

    /** Page index within the file being read or written, or -1. */
    int page = -1;

    /** Which opened file the page came from. Set while editing, otherwise -1. */
    int sourceId = -1;

    /** Page index within that file. */
    int sourcePage = -1;

    /** Whether the entry shows its children when the document is opened. */
    bool open = true;

    /**
     * How the reader draws the entry.
     *
     * PDF 1.4 lets a bookmark be bold, italic and coloured, and authors use it
     * to mark the parts of a long manual apart from its chapters. Dropping it
     * on the way through would flatten a table of contents that was designed
     * to be read at a glance into an undifferentiated list.
     */
    bool bold = false;
    bool italic = false;

    /** Invalid means the reader's own colour, which is the usual case. */
    QColor colour;

    QVector<OutlineItem> children;
};

/**
 * Reading and writing the table of contents.
 *
 * Every save until now dropped it. A three-hundred-page manual whose navigation
 * disappears the first time somebody rotates a page is not a document editor,
 * so this exists for the same reason the metadata handling does: what went in
 * has to come out.
 */
class Outline
{
public:
    /** The tree of @p pdf, with page numbers within that file. */
    static QVector<OutlineItem> read(const QString &pdf, QString *error);

    /**
     * A tree built from headings that were inferred rather than declared.
     *
     * A document with no table of contents cannot be given one out of nothing,
     * and the closest thing to an author's intent that a PDF holds is how big
     * the text is set. Convert::findHeadings() reads that; this is only the
     * shaping: a heading's level becomes the nesting, its text the title and
     * its page the destination.
     *
     * Deliberately nothing else. No entry is added, dropped, renamed, merged
     * or reordered on the way through, so whatever Convert::findHeadings()
     * misreads arrives here unaltered and is visible in the sidebar for what it
     * is, which is the point. Where the inference is wrong is stated once, on
     * Convert::findHeadings(), and callers offering this to a user should say
     * so: a wrong table of contents on a two-hundred-page manual is worse than
     * no table of contents at all.
     *
     * Levels that skip a step nest by their order rather than by their number,
     * so a level-three heading following a level-one becomes its child. A
     * bookmark tree has no way to show a missing level, and an empty placeholder
     * standing in for one would be a line in the sidebar that goes nowhere.
     * A heading with no text, or none that points at a page, is left out.
     *
     * @p headings are taken in the order given, and that order has to be the
     * document's: the nesting is worked out from what came before.
     */
    static QVector<OutlineItem> fromHeadings(const QVector<Convert::Heading> &headings);

    /** The same, from a document already open. */
    static QVector<OutlineItem> readFrom(QPDF &pdf);

    /**
     * Replaces the table of contents of an open document.
     *
     * Entries whose @ref OutlineItem::page falls outside the document are
     * dropped along with their children, because a bookmark that goes nowhere
     * is worse than one that is missing.
     */
    static void applyTo(QPDF &pdf, const QVector<OutlineItem> &items);

    /** Writes a copy of @p inputPdf carrying @p items instead of its own. */
    static bool write(const QString &inputPdf, const QString &outputPdf, const QVector<OutlineItem> &items,
                      QString *error);

    /** How many entries there are, at every level. */
    static int count(const QVector<OutlineItem> &items);

    /** The tree flattened depth-first, each entry paired with its depth. */
    static QVector<QPair<const OutlineItem *, int>> flatten(const QVector<OutlineItem> &items);
};

} // namespace ps
