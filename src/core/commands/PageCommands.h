/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#pragma once

#include "core/Outline.h"
#include "core/PageRef.h"

#include <QUndoCommand>
#include <QVector>

namespace ps {

class Document;

/**
 * Every structural edit the user can make, as undoable commands.
 *
 * Undo is implemented by inverse operations rather than by snapshotting the
 * page list, so the view keeps receiving precise insert/remove signals and can
 * animate and preserve selection instead of resetting itself on every step.
 */

class InsertPagesCommand : public QUndoCommand
{
public:
    InsertPagesCommand(Document *document, int at, const QVector<PageRef> &pages, const QString &text,
                       QUndoCommand *parent = nullptr);

    void redo() override;
    void undo() override;

private:
    Document *m_document;
    int m_at;
    QVector<PageRef> m_pages;
};

class RemovePagesCommand : public QUndoCommand
{
public:
    RemovePagesCommand(Document *document, const QVector<int> &indexes, QUndoCommand *parent = nullptr);

    void redo() override;
    void undo() override;

private:
    Document *m_document;
    QVector<int> m_indexes; //!< ascending, deduplicated
    QVector<PageRef> m_removed;
};

class MovePagesCommand : public QUndoCommand
{
public:
    MovePagesCommand(Document *document, const QVector<int> &indexes, int destination, QUndoCommand *parent = nullptr);

    void redo() override;
    void undo() override;

    /** Where the block ended up; the view uses this to keep it selected. */
    int landedAt() const { return m_landedAt; }

private:
    Document *m_document;
    QVector<int> m_indexes; //!< ascending, deduplicated, original positions
    int m_destination;
    int m_landedAt = 0;
};

class RotatePagesCommand : public QUndoCommand
{
public:
    RotatePagesCommand(Document *document, const QVector<int> &indexes, int delta, QUndoCommand *parent = nullptr);

    void redo() override;
    void undo() override;

    int id() const override
    {
        return 0x50533031; // "PS01"
    }

    /** Successive turns of the same pages collapse into one undo step. */
    bool mergeWith(const QUndoCommand *other) override;

private:
    Document *m_document;
    QVector<int> m_indexes;
    int m_delta;
};

/**
 * Swaps a set of pages for processed versions of themselves.
 *
 * This is how OCR and compression apply to part of a document: the chosen
 * pages are written out, worked on, and the results dropped back into exactly
 * the same slots. The originals stay in their source file untouched, so undo
 * costs nothing and the operation can be applied to a selection rather than
 * being all-or-nothing.
 */
class ReplacePagesCommand : public QUndoCommand
{
public:
    ReplacePagesCommand(Document *document, const QVector<int> &indexes, const QVector<PageRef> &replacements,
                        const QString &text, QUndoCommand *parent = nullptr);

    void redo() override;
    void undo() override;

private:
    Document *m_document;
    QVector<int> m_indexes;
    QVector<PageRef> m_replacements;
    QVector<PageRef> m_originals;
};

/**
 * Replaces the table of contents.
 *
 * On the same stack as the page operations on purpose: a window where Ctrl+Z
 * undoes page moves but not bookmark edits teaches people not to trust it.
 */
class SetOutlineCommand : public QUndoCommand
{
public:
    SetOutlineCommand(Document *document, const QVector<OutlineItem> &items, const QString &text,
                      QUndoCommand *parent = nullptr);

    void redo() override;
    void undo() override;

private:
    Document *m_document;
    QVector<OutlineItem> m_items;
    QVector<OutlineItem> m_previous;
};

class DuplicatePagesCommand : public QUndoCommand
{
public:
    DuplicatePagesCommand(Document *document, const QVector<int> &indexes, QUndoCommand *parent = nullptr);

    void redo() override;
    void undo() override;

private:
    Document *m_document;
    QVector<int> m_indexes;
    int m_insertAt = 0;
    QVector<PageRef> m_copies;
};

} // namespace ps
