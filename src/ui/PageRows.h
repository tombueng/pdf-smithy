/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#pragma once

#include "core/Document.h"
#include "core/PageRef.h"
#include "core/Source.h"

#include <QHash>
#include <QSizeF>
#include <QString>
#include <QTransform>
#include <QVector>

namespace ps {

/**
 * A page as the file that holds it names it: which source, and which page in it.
 *
 * The type exists in order not to be an `int`. A view row and a page index in a
 * file are both small counting numbers and they are the *same* number only until
 * somebody deletes a page, drags one somewhere else or opens a second document,
 * after which every place that confused the two draws a form field on the wrong
 * sheet, offers a handle on another page's text, or writes a correction into a
 * page nobody was looking at. Each of those has happened here.
 *
 * There is no conversion to or from `int` in either direction, so the confusion
 * cannot be made by accident: a caller holding a row has to write
 * `rows.pageOf(row).pageInFile()` out loud before it can ask a file for
 * anything, and @ref PageRows is the only place that sentence is answered.
 */
class SourcePage
{
public:
    SourcePage() = default;

    /** Both halves at once; there is deliberately no way in from one number. */
    SourcePage(int sourceId, int pageInFile)
        : m_source(sourceId)
        , m_page(pageInFile)
    {
    }

    /** Index into Document::sources(), which is also the render backend's id. */
    int sourceId() const { return m_source; }

    /** Zero-based and counted inside that one file. Never a view row. */
    int pageInFile() const { return m_page; }

    bool isValid() const { return m_source >= 0 && m_page >= 0; }

    bool operator==(const SourcePage &other) const = default;

private:
    int m_source = -1;
    int m_page = -1;
};

inline size_t qHash(const SourcePage &page, size_t seed = 0)
{
    return qHashMulti(seed, page.sourceId(), page.pageInFile());
}

/**
 * Where the rows went when the page list changed, and which ones did not come back.
 *
 * Work made on a page is anchored to a row, and a row is a position in a list
 * that the organiser rearranges. Following the movement is what lets a comment
 * drawn on the first sheet survive somebody deleting the fourth, and what makes
 * the one case where it cannot be followed (the page it was made on is gone)
 * something to say out loud rather than something to do quietly.
 */
class Rebase
{
public:
    /** Where the row that used to be @p wasRow is now, or -1 when its page has gone. */
    int nowAt(int wasRow) const { return wasRow >= 0 && wasRow < m_to.size() ? m_to.at(wasRow) : -1; }

    /** How many degrees have been added to @p wasRow since, on the 0/90/180/270 grid. */
    int turnOf(int wasRow) const { return wasRow >= 0 && wasRow < m_turn.size() ? m_turn.at(wasRow) : 0; }

    /**
     * True when @p wasRow kept its number but is showing a different page now.
     *
     * What an operation leaves behind: the pages are written out, the result is
     * opened as another source and every row is pointed at it. The sheet in
     * front of the reader is the same one; what is inside the file behind it is
     * not, so anything read out of the old one has to be read again.
     */
    bool rewritten(int wasRow) const { return wasRow >= 0 && wasRow < m_rewritten.size() && m_rewritten.at(wasRow); }

    /** True when every row is where it was, unturned: there is nothing to re-base. */
    bool isIdentity() const { return m_identity; }

private:
    friend class PageRows;

    QVector<int> m_to;
    QVector<int> m_turn;
    QVector<bool> m_rewritten;
    bool m_identity = true;
};

/**
 * The turn from a page as the file measured it into the page as the view draws it.
 *
 * Both spaces are display points with the origin at the bottom left. @p rotation
 * is on the 0/90/180/270 grid (@ref Rebase::turnOf() and @ref PageRows::rotationOf()
 * both answer in it), and @p shown is the size the page has *after* the turn,
 * which is what @ref PageRows::sizeOf() answers.
 *
 * A quarter turn goes clockwise. That is not a choice: it is what `/Rotate`
 * means in the file the work is eventually written into, what
 * PdfGeometry::displayToPageTransform() therefore undoes, and what the view does
 * to the picture the work is drawn over. Anything that turns the other way puts
 * a comment, a handle or a field on a quarter-turned sheet exactly half a turn
 * from the thing it belongs to.
 *
 * It lives here, once, because it was written five times over and two of the
 * copies had the sign the other way. The two agree at a half turn, which is its
 * own inverse, so the disagreement was invisible to everything that only ever
 * tested turning a page upside down. tests/tst_pageturn.cpp settles it against
 * ink on a rendered sheet rather than against another matrix.
 */
inline QTransform pageTurn(int rotation, const QSizeF &shown)
{
    const int degrees = normalizeRotation(rotation);
    if (degrees == 0 || shown.isEmpty()) {
        return {};
    }
    // @p shown is the page after the turn, so at a quarter of one the page's own
    // width and height are the other way round from the ones it is drawn at.
    const QSizeF source = degrees % 180 == 0 ? shown : QSizeF(shown.height(), shown.width());
    switch (degrees) {
    case 90:
        // Clockwise: what was the sheet's bottom edge is now its left one, and
        // what was printed in its bottom left corner is now in its top left.
        return QTransform(0, -1, 1, 0, 0, source.width());
    case 180:
        return QTransform(-1, 0, 0, -1, source.width(), source.height());
    case 270:
        return QTransform(0, 1, -1, 0, source.height(), 0);
    default:
        return {};
    }
}

/**
 * The one place a view row becomes a page in a file, and a page in a file a row.
 *
 * A ps::Document is a list of PageRefs, each naming a page inside one of the
 * files the session has open. Two documents merged, one page deleted or one page
 * dragged elsewhere and the two numberings part company for good; a layer that
 * reads a page by its row number then reads whichever page has since moved into
 * that slot. Holding one of these, taking the conversion from it and never
 * writing a bare row where a file's own page number belongs is the whole rule.
 *
 * Not owning the document is deliberate: an overlay lives and dies with its
 * view, and the document outlives both.
 */
class PageRows
{
public:
    PageRows() = default;

    /** Not owned; must outlive this. Takes a snapshot for @ref follow(). */
    void setDocument(const Document *document)
    {
        m_document = document;
        m_was = document ? document->pages() : QVector<PageRef>();
    }

    /**
     * Takes the page list as it stands to be the state to follow from.
     *
     * For a caller that has just been handed work whose rows are true right now
     * (a form read out of the document's own file) and wants everything after
     * this instant measured from it.
     */
    void resync() { m_was = m_document ? m_document->pages() : QVector<PageRef>(); }

    /** Which page of which file @p row shows, or an invalid one. */
    SourcePage pageOf(int row) const
    {
        if (!m_document || row < 0 || row >= m_document->pageCount()) {
            return {};
        }
        const PageRef ref = m_document->pageAt(row);
        return SourcePage(ref.sourceId, ref.sourcePage);
    }

    /** How far the organiser has turned @p row, in degrees, on top of the file's own. */
    int rotationOf(int row) const
    {
        if (!m_document || row < 0 || row >= m_document->pageCount()) {
            return 0;
        }
        return normalizeRotation(m_document->pageAt(row).rotation);
    }

    /** @p row's size in points as the view shows it, turn included. */
    QSizeF sizeOf(int row) const { return m_document ? m_document->pageSizePoints(row) : QSizeF(); }

    /** The file @p page lives in, or empty when no source holds it. */
    QString fileOf(SourcePage page) const
    {
        if (!m_document || !page.isValid()) {
            return {};
        }
        const Source *source = m_document->source(page.sourceId());
        return source ? source->path() : QString();
    }

    /**
     * Notes where every row is now and answers where the old ones went.
     *
     * Rows showing the same page of the same file are the same row moved,
     * whatever has happened to the numbering in between. Turning a page is
     * deliberately not a different page (that is what makes rotating the fourth
     * sheet leave the comment on the first one alone), so the turn is reported
     * beside the movement rather than counted as a loss.
     */
    Rebase follow()
    {
        const QVector<PageRef> now = m_document ? m_document->pages() : QVector<PageRef>();

        Rebase change;
        change.m_to.fill(-1, m_was.size());
        change.m_turn.fill(0, m_was.size());
        change.m_rewritten.fill(false, m_was.size());
        change.m_identity = now.size() == m_was.size();

        QHash<SourcePage, QVector<int>> unclaimed;
        for (int row = int(now.size()) - 1; row >= 0; --row) {
            unclaimed[SourcePage(now.at(row).sourceId, now.at(row).sourcePage)].prepend(row);
        }

        for (int was = 0; was < m_was.size(); ++was) {
            const auto found = unclaimed.find(SourcePage(m_was.at(was).sourceId, m_was.at(was).sourcePage));
            if (found == unclaimed.end() || found->isEmpty()) {
                change.m_identity = false;
                continue;
            }
            // Taken rather than read, so that two rows showing one duplicated page
            // land on the two rows that still show it rather than both on the first.
            const int row = found->takeFirst();
            change.m_to[was] = row;
            change.m_turn[was] = normalizeRotation(now.at(row).rotation - m_was.at(was).rotation);
            if (row != was || change.m_turn.at(was) != 0) {
                change.m_identity = false;
            }
        }

        m_was = now;
        return change;
    }

    /**
     * Notes what the rows show now, none of them having moved.
     *
     * What Document::pagesChanged means: every row keeps its number and one of
     * them is showing something else: turned, or pointed at a freshly written
     * file by an operation that rewrote the pages. Nothing can be lost by it, so
     * nothing is reported lost; the answer says only which rows were turned and
     * which are now looking at another page.
     */
    Rebase followInPlace()
    {
        const QVector<PageRef> now = m_document ? m_document->pages() : QVector<PageRef>();

        // A shorter or longer list means a page was added or taken away and the
        // snapshot has not caught up: the two halves of this class are answered
        // at different speeds, because a page list that changed length is
        // followed from the event loop (a move is a removal and an insertion,
        // and the gap between them is not a document anybody meant), while a
        // page merely turned is answered on the spot. So a turn can arrive here
        // with a removal still waiting, and pairing row for row would then
        // report the turn against whichever sheet has since slid into that
        // number and call every row past the removal a different page. Matching
        // the rows by the page they show is exactly what follow() does, and
        // doing it now rather than a moment later costs nothing: the queued call
        // that follows finds everything already where this left it.
        if (now.size() != m_was.size()) {
            return follow();
        }

        Rebase change;
        change.m_to.fill(-1, m_was.size());
        change.m_turn.fill(0, m_was.size());
        change.m_rewritten.fill(false, m_was.size());
        change.m_identity = now.size() == m_was.size();

        for (int row = 0; row < m_was.size() && row < now.size(); ++row) {
            change.m_to[row] = row;
            change.m_turn[row] = normalizeRotation(now.at(row).rotation - m_was.at(row).rotation);
            change.m_rewritten[row]
                = now.at(row).sourceId != m_was.at(row).sourceId || now.at(row).sourcePage != m_was.at(row).sourcePage;
            if (change.m_turn.at(row) != 0 || change.m_rewritten.at(row)) {
                change.m_identity = false;
            }
        }

        m_was = now;
        return change;
    }

private:
    const Document *m_document = nullptr;

    /** The page list as it stood when it was last followed. */
    QVector<PageRef> m_was;
};

} // namespace ps
