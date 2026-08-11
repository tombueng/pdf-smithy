/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#pragma once

#include "Metadata.h"
#include "Outline.h"
#include "PageRef.h"

#include <QObject>
#include <QString>
#include <QVector>

#include <memory>
#include <vector>

class QUndoStack;

namespace ps {

class Source;
class RenderBackend;

/**
 * The document the user is assembling.
 *
 * Holds any number of opened Sources plus one ordered list of PageRefs that
 * describes the result. Merging is therefore not a special operation but the
 * normal state of affairs: pages from three files sit in one list, each
 * remembering where it came from.
 *
 * The mutating methods are deliberately primitive and do *not* touch the undo
 * stack: they are the vocabulary that PageCommands speaks. UI code should go
 * through the commands so that everything stays undoable.
 */
class Document : public QObject
{
    Q_OBJECT

public:
    explicit Document(QObject *parent = nullptr);
    ~Document() override;

    /** Not owned; must outlive the document. */
    void setRenderBackend(RenderBackend *backend);
    RenderBackend *renderBackend() const { return m_backend; }

    // ── Loading ───────────────────────────────────────────────────────────

    /**
     * Discards everything, then loads @p path as the only source.
     *
     * @p needsPassword comes back true when the file is locked and @p password
     * does not open it: a question rather than a failure, and the only thing
     * standing between a user and a document they own. Reported as a bool
     * rather than as Source::Trouble so that this header stays free of QPDF,
     * which everything that touches a document would otherwise have to read.
     */
    bool open(const QString &path, QString *error, const QString &password = QString(),
              bool *needsPassword = nullptr);

    /** Loads @p path as an extra source. Returns its id, or -1 on failure. */
    int addSource(const QString &path, QString *error, const QString &password = QString(),
                  bool *needsPassword = nullptr);

    /**
     * Loads @p path and inserts all of its pages at @p at (-1 appends).
     * This is what dropping a file onto the window does.
     */
    bool importFile(const QString &path, int at, QString *error);

    void clear();

    Source *source(int id) const;
    int sourceCount() const { return static_cast<int>(m_sources.size()); }

    // ── Pages ─────────────────────────────────────────────────────────────

    const QVector<PageRef> &pages() const { return m_pages; }

    int pageCount() const { return static_cast<int>(m_pages.size()); }

    PageRef pageAt(int index) const;

    /** Page size in points with the user's rotation taken into account. */
    QSizeF pageSizePoints(int index) const;

    // ── Session state ─────────────────────────────────────────────────────

    QUndoStack *undoStack() const { return m_undo; }

    QString filePath() const { return m_filePath; }
    void setFilePath(const QString &path);

    /** File name for the title bar, or a translated placeholder when unsaved. */
    QString displayName() const;

    bool isModified() const;

    /**
     * The document's own metadata.
     *
     * Held here rather than read from a source on demand: output is assembled
     * from scratch, so without somewhere to keep it the title and author would
     * quietly disappear on every save.
     */
    const Metadata::Fields &metadata() const { return m_metadata; }
    void setMetadata(const Metadata::Fields &fields);

    /**
     * The table of contents, with @ref OutlineItem::page resolved to rows.
     *
     * Entries are held anchored to the page they point at rather than to its
     * number, so moving a chapter takes its bookmark along and deleting a page
     * takes its bookmark away. Entries whose page is gone come back with a page
     * of -1 and are dropped on save.
     */
    QVector<OutlineItem> outline() const;

    /** Replaces the table of contents; @p items use row numbers. */
    void setOutline(const QVector<OutlineItem> &items);

    bool hasOutline() const { return !m_outline.isEmpty(); }

    /** True once anything at all has been loaded. */
    bool isEmpty() const { return m_pages.isEmpty() && m_sources.empty(); }

    // ── Mutations, for PageCommands, not for UI code ───────────────────────

    void insertPages(int at, const QVector<PageRef> &refs);

    /** Removes @p indexes (any order) and returns them in ascending order. */
    QVector<PageRef> removePages(QVector<int> indexes);

    /** Moves @p indexes so the block lands before original index @p destination. */
    void movePages(QVector<int> indexes, int destination);

    /** Adds @p delta degrees to each listed page. */
    void applyRotation(const QVector<int> &indexes, int delta);

    /** Sets absolute rotation on each listed page. */
    void setRotation(const QVector<int> &indexes, int absolute);

    /** Points one slot at a different page, keeping its position. */
    void replacePage(int index, const PageRef &ref);

    /** Wholesale replacement, used by undo of complex operations. */
    void setPages(const QVector<PageRef> &refs);

Q_SIGNALS:
    void pagesAboutToBeInserted(int at, int count);
    void pagesInserted(int at, int count);
    void pagesAboutToBeRemoved(int at, int count);
    void pagesRemoved(int at, int count);
    void pagesChanged(int first, int last);
    void aboutToReset();
    void wasReset();
    void modifiedChanged(bool modified);
    void filePathChanged(const QString &path);
    void sourceAdded(int sourceId);
    void metadataChanged();
    void outlineChanged();

private:
    /** Reads the table of contents one source brought and appends it. */
    void adoptOutlineOf(int sourceId);

    QVector<PageRef> m_pages;
    std::vector<std::unique_ptr<Source>> m_sources;
    QUndoStack *m_undo = nullptr;
    RenderBackend *m_backend = nullptr;
    QString m_filePath;
    Metadata::Fields m_metadata;

    /** Anchored to (sourceId, sourcePage); rows are worked out on demand. */
    QVector<OutlineItem> m_outline;
};

} // namespace ps
