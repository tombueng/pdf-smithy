/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#pragma once

#include "Inspector.h"
#include "core/Outline.h"

#include <QDockWidget>
#include <QVector>

#include <memory>

class QLabel;
class QTreeWidget;
class QTreeWidgetItem;

namespace ps {

class BookmarkInspectable;
class Document;

/**
 * The document's own table of contents, alongside the pages.
 *
 * A three-hundred-page manual is unnavigable in a grid of thumbnails, and its
 * author already wrote the navigation; it just was not being shown. Entries
 * point at pages, so clicking one selects that page in the organiser and the
 * two halves of the window stay in step.
 *
 * Editing happens here too: rename in place with a double click, add an entry
 * for the page in view, drag entries about to change the order and the nesting,
 * delete. Every change goes onto the same undo stack as the page operations,
 * because "undo" meaning different things in different parts of one window is
 * its own kind of bug.
 *
 * A bookmark's attributes (what it says, where it points, whether it opens
 * showing its sub-entries, and the bold, italic and colour the reader will draw
 * it in) have no gesture on the page, so they live in the properties panel
 * rather than behind a dialog. That is what InspectionSource is for here.
 */
class OutlineDock : public QDockWidget, public InspectionSource
{
    Q_OBJECT

public:
    explicit OutlineDock(Document *document, QWidget *parent = nullptr);
    ~OutlineDock() override;

    /** Rebuilds the tree from the document. */
    void refresh();

    /** Highlights the entry covering @p row, without emitting a jump. */
    void followPage(int row);

    // ── InspectionSource ──────────────────────────────────────────────────

    Inspectable *inspected() override;

    // ── What the properties panel reads and writes ────────────────────────

    /** The selected entry's own attributes, its sub-entries left out. */
    OutlineItem selectedAttributes() const;

    /** True when the selected entry has anything under it. */
    bool selectedHasChildren() const;

    /** Puts title, destination, open state and style back, as one undo step. */
    void applyToSelected(const OutlineItem &attributes);

    /** How many pages there are to point at. */
    int pageCount() const;

Q_SIGNALS:
    /** The user asked to go to a page. */
    void pageRequested(int row);

    /** A different entry is selected, so the properties panel is stale. */
    void inspectionChanged();

private:
    void addForCurrentPage();

    /**
     * Offers a whole table of contents inferred from how the document is set.
     *
     * The one thing the panel could not do before: a document with no bookmarks
     * could only be given them one at a time, which nobody does for a manual.
     * What comes out is a guess (see Convert::findHeadings()), so it is said to
     * be one, and it arrives on the undo stack like every other edit.
     */
    void buildFromHeadings();

    void renameSelected();
    void removeSelected();
    void nestSelected();
    void unnestSelected();
    void moveSelected(int delta);
    void showMenu(const QPoint &where);

    /** Called once a drag has finished moving @p moved somewhere else. */
    void finishDrop(QTreeWidgetItem *moved);

    void commit(const QString &undoText);

    /** Reads the tree widget back into an outline. */
    QVector<OutlineItem> harvest() const;

    void fill(QTreeWidgetItem *parent, const QVector<OutlineItem> &items);

    /** Writes one entry onto @p node, its look included. */
    void decorate(QTreeWidgetItem *node, const OutlineItem &item);

    /** Reads it back off @p node, without walking into its children. */
    static OutlineItem attributesOf(const QTreeWidgetItem *node);

    /** Opens @p node and everything under it as far as each entry says to. */
    void restoreExpansion(QTreeWidgetItem *node);

    /** Shows the hint instead of an empty tree, or takes it away again. */
    void updateHint();

    Document *m_document;
    QTreeWidget *m_tree;

    /** What a document with no table of contents shows instead of nothing. */
    QWidget *m_empty;

    /** Set while rebuilding, so that filling the tree is not read as an edit. */
    bool m_loading = false;

    /**
     * Set while pushing an edit of our own, so that the document's answer does
     * not rebuild the tree under the item the user is still working in.
     */
    bool m_committing = false;

    /** The row the organiser is showing, for "add a bookmark here". */
    int m_currentRow = 0;

    std::unique_ptr<BookmarkInspectable> m_inspectable;
};

/**
 * The selected bookmark's attributes, for the properties panel.
 *
 * Everything here is editable, unlike a form field's: a bookmark has no
 * appearance on the page to click, so the panel is the only place its
 * destination and its styling can be reached at all.
 */
class BookmarkInspectable : public Inspectable
{
public:
    explicit BookmarkInspectable(OutlineDock *dock);

    QString kindName() const override;
    QString description() const override;
    QWidget *buildEditor(QWidget *parent) override;

private:
    /** Owns this, and destroys it before it goes itself. */
    OutlineDock *m_dock = nullptr;
};

} // namespace ps
