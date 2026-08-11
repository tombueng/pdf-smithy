/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#pragma once

#include "Inspector.h"
#include "PageRows.h"
#include "core/Metadata.h"
#include "core/PageLayout.h"

#include <QByteArray>
#include <QFutureWatcher>
#include <QMetaObject>
#include <QObject>
#include <QSizeF>
#include <QString>
#include <QStringList>
#include <QVector>

#include <memory>

class QAbstractItemView;
class QTimer;

namespace ps {

class Document;
class InspectorDock;

/**
 * One page, as the file that holds it can answer without reading it through.
 *
 * Everything here comes out of the page dictionary of a ps::Source that is
 * already open, which is why it is all in one struct: on a 180-page magazine
 * gathering the lot for one page is under a millisecond and for all 180 of them
 * 88 ms, so the panel can afford it on every click. Reading the same boxes with
 * PageLayout::boxesOf() costs 63 ms for one page, because it opens the file
 * afresh, which is why they are read from the open source instead.
 *
 * Anything that needs the content stream parsed or the text laid out is in
 * @ref DeepFacts instead.
 */
struct PageFacts {
    /** The view row, which is the sheet the reader is looking at. */
    int row = -1;

    /** The same sheet as the file names it: which source, which page in it. */
    SourcePage page;

    /** The file that source was opened from, for the readers that want a path. */
    QString file;

    /** What the reader sees, in points, with the turn already applied. */
    QSizeF sizePoints;

    /** The file's own /Rotate plus whatever the organiser added: 0/90/180/270. */
    int rotation = 0;

    /** All five, an invalid rect meaning the page does not carry that box. */
    PageLayout::Boxes boxes;

    /** What the file spends on this page; see weighPage() for what that counts. */
    qint64 bytes = 0;

    /** The annotations, split the way a person would split them. */
    int comments = 0;
    int links = 0;
    int fields = 0;

    bool isValid() const { return row >= 0 && page.isValid(); }
};

/**
 * What only reading the pages through can answer.
 *
 * Kept apart from @ref PageFacts because it is two orders of magnitude dearer.
 * Measured on that same magazine: parsing one page's content stream costs
 * 47 to 66 ms, because ps::PageObjects opens the file afresh for each call; laying
 * out one page's text costs 5 to 23 ms; and the font inventory is 130 ms for the
 * whole document however few pages are wanted from it. End to end that is about
 * a tenth of a second a page: 1.5 seconds for sixteen pages and 18 for all 180.
 *
 * A panel that did any of that while somebody arrowed through pages would be a
 * panel that stutters, so it is fetched off the GUI thread, and only unasked
 * for a selection small enough to be worth it, see
 * DocumentProperties::automaticLimit().
 */
struct DeepFacts {
    /** How many pages these totals are over; 0 while nothing has been counted. */
    int pages = 0;

    int words = 0;

    /** Not counting spaces or line breaks, which is what people mean by it. */
    int characters = 0;

    /** What the pages draw, as ps::PageObjects reads them. */
    int textRuns = 0;
    int pictures = 0;
    int shapes = 0;

    /** Families rather than font objects: three subsets of Minion are one font. */
    int fontFamilies = 0;

    /** The families whose glyphs do not travel with the document, by name. */
    QStringList notEmbedded;

    /** True once an answer has arrived, so "no words" can be told from "not yet". */
    bool done = false;

    /** Set when a page could not be read; the panel says so rather than lying. */
    QString trouble;
};

/**
 * One page to read through, named the way the file that holds it names it.
 *
 * Never a view row. What the dear readers take is a path and a page number
 * inside that file, and a document merged from two files has a different answer
 * for each of them at the same row.
 */
struct SurveyPage {
    int sourceId = -1;
    int pageInFile = -1;
    QString file;
};

/**
 * The properties panel's answer when nothing *on* the page is selected.
 *
 * The five overlays fill the panel when the user has clicked a comment, a form
 * field, a picture or a run of text. Between those clicks (which is the whole
 * of Read mode and most of Arrange the Pages) the panel used to be empty, and
 * an empty properties panel teaches people to close it.
 *
 * So this answers the two questions that can always be asked. With pages picked
 * out in the organiser it describes those pages; with none picked out it
 * describes the document, and lets what the document says about itself be edited
 * in place, through the same undo command the Document Properties dialog pushes,
 * so that a change made here and a change made there are one thing to walk back
 * rather than two. There is deliberately no third answer: a panel that also
 * offered commands would stop being something a person can glance at.
 *
 * It puts itself into @p panel rather than being wired up from outside, because
 * the panel is its only consumer and the alternative is three lines in the window
 * that have to be kept in step with one another.
 */
class DocumentProperties : public QObject, public InspectionSource
{
    Q_OBJECT

public:
    /**
     * @p pages is the view whose selection decides which of the two answers is
     * given. Its selection counts only while it is on screen: rows left behind
     * in the organiser must not go on describing themselves once the reader has
     * gone back to reading, which is the case the complaint was about.
     *
     * @p panel may be null, which is what a test driving this directly passes.
     */
    DocumentProperties(Document *document, QAbstractItemView *pages, InspectorDock *panel, QObject *parent = nullptr);
    ~DocumentProperties() override;

    // ── InspectionSource ──────────────────────────────────────────────────

    Inspectable *inspected() override;

    // ── What it is describing ─────────────────────────────────────────────

    /** The rows being described; empty means the document itself. */
    QVector<int> chosenRows() const { return m_rows; }

    /** The cheap facts of those rows, in row order. */
    const QVector<PageFacts> &pageFacts() const { return m_facts; }

    /** The dear ones, once they have arrived. */
    const DeepFacts &deepFacts() const { return m_deep; }

    /** Reads the pages through, off the GUI thread. Harmless if already running. */
    void startCounting();

    /** True while a count is in flight. */
    bool isCounting() const;

    /**
     * How many pages are read through without the user asking.
     *
     * Sixteen, from the measurement quoted on @ref DeepFacts: a second and a
     * half of work on a worker thread for a magazine's pages, which is worth
     * spending on spec. All 180 of them is eighteen seconds, which is not, so
     * past this the panel offers a button and lets the user decide.
     */
    static int automaticLimit();

Q_SIGNALS:
    /** The counted numbers have changed: they either arrived or were dropped. */
    void countingChanged();

protected:
    /** Watches @p pages going on and off screen; see the constructor's note. */
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    class AboutDocument;
    class AboutPages;

    /** Re-reads the selection and the cheap facts, then tells the panel. */
    void refresh();

    /** Coalesces a burst of selection changes into one refresh. */
    void schedule();

    /** Points the selection-change connection at whatever model the view has now. */
    void watchSelection();

    /** The pages a count would be over: the chosen ones, or the whole document. */
    QVector<SurveyPage> wantedPages() const;

    /** Those pages as bytes, so that "the same pages as last time" can be spotted. */
    QByteArray countKey() const;

    /** Pushes edited metadata as the one undoable step the dialog also pushes. */
    void apply(const Metadata::Fields &fields);

    Document *m_document;
    QAbstractItemView *m_pages;
    InspectorDock *m_panel;

    /** The one place a view row becomes a page in a file. */
    PageRows m_pageRows;

    /** Held rather than remade, so that typing in the panel does not rebuild it. */
    std::unique_ptr<AboutDocument> m_aboutDocument;
    std::unique_ptr<AboutPages> m_aboutPages;

    /** The rows last described. Empty means the document itself. */
    QVector<int> m_rows;

    QVector<PageFacts> m_facts;
    DeepFacts m_deep;

    /**
     * Rises with every change of what is described, so that an answer worked out
     * for a selection since replaced is dropped rather than shown against the
     * wrong pages. A count already running cannot be called off (QtConcurrent
     * has no way to interrupt one), so it is finished and thrown away.
     */
    int m_generation = 0;

    /** Which generation the count in flight belongs to, or -1 when idle. */
    int m_countingFor = -1;

    /** True when the current generation wants a count, asked for or automatic. */
    bool m_countWanted = false;

    /** Which pages the numbers on hand were counted over; see countKey(). */
    QByteArray m_countKey;

    QFutureWatcher<DeepFacts> m_counting;
    QTimer *m_settle;
    QMetaObject::Connection m_selectionWatch;
};

} // namespace ps
