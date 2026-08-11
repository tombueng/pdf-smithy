/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#include "PageCommands.h"

#include "core/Document.h"

#include <KLocalizedString>

#include <algorithm>

namespace ps {

namespace {

/** Ascending, no duplicates: every command below relies on both properties. */
QVector<int> tidy(QVector<int> indexes)
{
    std::sort(indexes.begin(), indexes.end());
    indexes.erase(std::unique(indexes.begin(), indexes.end()), indexes.end());
    return indexes;
}

/** Restores pages to remembered absolute positions, ascending. */
void reinsertAt(Document *document, const QVector<int> &positions, const QVector<PageRef> &pages)
{
    const int count = std::min(positions.size(), pages.size());
    for (int i = 0; i < count; ++i) {
        document->insertPages(positions.at(i), { pages.at(i) });
    }
}

QVector<int> contiguousBlock(int start, int count)
{
    QVector<int> block;
    block.reserve(count);
    for (int i = 0; i < count; ++i) {
        block.append(start + i);
    }
    return block;
}

} // namespace

// ── Insert ────────────────────────────────────────────────────────────────

InsertPagesCommand::InsertPagesCommand(Document *document, int at, const QVector<PageRef> &pages, const QString &text,
                                       QUndoCommand *parent)
    : QUndoCommand(text, parent)
    , m_document(document)
    , m_at(at)
    , m_pages(pages)
{
}

void InsertPagesCommand::redo()
{
    m_document->insertPages(m_at, m_pages);
}

void InsertPagesCommand::undo()
{
    m_document->removePages(contiguousBlock(m_at, m_pages.size()));
}

// ── Remove ────────────────────────────────────────────────────────────────

RemovePagesCommand::RemovePagesCommand(Document *document, const QVector<int> &indexes, QUndoCommand *parent)
    : QUndoCommand(parent)
    , m_document(document)
    , m_indexes(tidy(indexes))
{
    setText(i18ncp("@action:inmenu undo step", "Delete page", "Delete %1 pages", m_indexes.size()));
}

void RemovePagesCommand::redo()
{
    m_removed = m_document->removePages(m_indexes);
}

void RemovePagesCommand::undo()
{
    reinsertAt(m_document, m_indexes, m_removed);
}

// ── Move ──────────────────────────────────────────────────────────────────

MovePagesCommand::MovePagesCommand(Document *document, const QVector<int> &indexes, int destination,
                                   QUndoCommand *parent)
    : QUndoCommand(parent)
    , m_document(document)
    , m_indexes(tidy(indexes))
    , m_destination(destination)
{
    setText(i18ncp("@action:inmenu undo step", "Move page", "Move %1 pages", m_indexes.size()));
}

void MovePagesCommand::redo()
{
    // Lifting the block out shifts everything behind it, so the drop target
    // slides left by however many selected pages sat in front of it.
    int removedBefore = 0;
    for (const int index : std::as_const(m_indexes)) {
        if (index < m_destination) {
            ++removedBefore;
        }
    }
    m_landedAt = m_destination - removedBefore;

    const QVector<PageRef> taken = m_document->removePages(m_indexes);
    m_document->insertPages(m_landedAt, taken);
}

void MovePagesCommand::undo()
{
    const QVector<PageRef> taken = m_document->removePages(contiguousBlock(m_landedAt, m_indexes.size()));
    reinsertAt(m_document, m_indexes, taken);
}

// ── Rotate ────────────────────────────────────────────────────────────────

RotatePagesCommand::RotatePagesCommand(Document *document, const QVector<int> &indexes, int delta, QUndoCommand *parent)
    : QUndoCommand(parent)
    , m_document(document)
    , m_indexes(tidy(indexes))
    , m_delta(normalizeRotation(delta))
{
    setText(i18ncp("@action:inmenu undo step", "Rotate page", "Rotate %1 pages", m_indexes.size()));
}

void RotatePagesCommand::redo()
{
    m_document->applyRotation(m_indexes, m_delta);
}

void RotatePagesCommand::undo()
{
    m_document->applyRotation(m_indexes, -m_delta);
}

bool RotatePagesCommand::mergeWith(const QUndoCommand *other)
{
    const auto *rotation = static_cast<const RotatePagesCommand *>(other);
    if (rotation->m_indexes != m_indexes) {
        return false;
    }

    m_delta = normalizeRotation(m_delta + rotation->m_delta);
    // Four quarter turns bring the pages home; leaving a do-nothing entry on
    // the stack would make undo look broken.
    if (m_delta == 0) {
        setObsolete(true);
    }
    return true;
}

// ── Replace ───────────────────────────────────────────────────────────────

ReplacePagesCommand::ReplacePagesCommand(Document *document, const QVector<int> &indexes,
                                         const QVector<PageRef> &replacements, const QString &text,
                                         QUndoCommand *parent)
    : QUndoCommand(text, parent)
    , m_document(document)
    , m_indexes(tidy(indexes))
    , m_replacements(replacements)
{
}

void ReplacePagesCommand::redo()
{
    if (m_indexes.size() != m_replacements.size()) {
        return;
    }

    if (m_originals.isEmpty()) {
        m_originals.reserve(m_indexes.size());
        for (const int index : std::as_const(m_indexes)) {
            m_originals.append(m_document->pageAt(index));
        }
    }

    // Swapped in place: page 7 stays page 7, it is merely a better page 7 now.
    for (int i = 0; i < m_indexes.size(); ++i) {
        m_document->replacePage(m_indexes.at(i), m_replacements.at(i));
    }
}

void ReplacePagesCommand::undo()
{
    for (int i = 0; i < m_indexes.size() && i < m_originals.size(); ++i) {
        m_document->replacePage(m_indexes.at(i), m_originals.at(i));
    }
}

// ── Duplicate ─────────────────────────────────────────────────────────────

DuplicatePagesCommand::DuplicatePagesCommand(Document *document, const QVector<int> &indexes, QUndoCommand *parent)
    : QUndoCommand(parent)
    , m_document(document)
    , m_indexes(tidy(indexes))
{
    setText(i18ncp("@action:inmenu undo step", "Duplicate page", "Duplicate %1 pages", m_indexes.size()));
}

void DuplicatePagesCommand::redo()
{
    if (m_indexes.isEmpty()) {
        return;
    }

    if (m_copies.isEmpty()) {
        m_copies.reserve(m_indexes.size());
        for (const int index : std::as_const(m_indexes)) {
            m_copies.append(m_document->pageAt(index));
        }
        // Copies land directly behind the selection, where the user is looking.
        m_insertAt = m_indexes.last() + 1;
    }

    m_document->insertPages(m_insertAt, m_copies);
}

void DuplicatePagesCommand::undo()
{
    m_document->removePages(contiguousBlock(m_insertAt, m_copies.size()));
}

// ── SetOutlineCommand ─────────────────────────────────────────────────────

SetOutlineCommand::SetOutlineCommand(Document *document, const QVector<OutlineItem> &items, const QString &text,
                                     QUndoCommand *parent)
    : QUndoCommand(text, parent)
    , m_document(document)
    , m_items(items)
{
}

void SetOutlineCommand::redo()
{
    m_previous = m_document->outline();
    m_document->setOutline(m_items);
}

void SetOutlineCommand::undo()
{
    m_document->setOutline(m_previous);
}

} // namespace ps
