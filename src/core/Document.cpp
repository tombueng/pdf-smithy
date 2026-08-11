/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#include "Document.h"

#include "RenderBackend.h"
#include "Source.h"

#include <QFileInfo>
#include <QUndoStack>

#include <KLocalizedString>

#include <algorithm>

namespace ps {

namespace {

/** Rewrites page numbers throughout a tree, dropping what cannot be found. */
void resolveRows(QVector<OutlineItem> &items, const QHash<QPair<int, int>, int> &rowOf)
{
    for (OutlineItem &item : items) {
        item.page = rowOf.value({ item.sourceId, item.sourcePage }, -1);
        resolveRows(item.children, rowOf);
    }
}

/** The reverse: turns row numbers into anchors that survive reordering. */
void anchorRows(QVector<OutlineItem> &items, const QVector<PageRef> &pages)
{
    for (OutlineItem &item : items) {
        if (item.page >= 0 && item.page < pages.size()) {
            item.sourceId = pages.at(item.page).sourceId;
            item.sourcePage = pages.at(item.page).sourcePage;
        }
        anchorRows(item.children, pages);
    }
}

/** Stamps every entry of a freshly read tree with the file it came from. */
void stampSource(QVector<OutlineItem> &items, int sourceId)
{
    for (OutlineItem &item : items) {
        item.sourceId = sourceId;
        item.sourcePage = item.page;
        stampSource(item.children, sourceId);
    }
}

} // namespace

Document::Document(QObject *parent)
    : QObject(parent)
    , m_undo(new QUndoStack(this))
{
    // The undo stack's clean state *is* the modified state: there is no
    // second bookkeeping flag to drift out of sync with it.
    connect(m_undo, &QUndoStack::cleanChanged, this, [this](bool clean) { Q_EMIT modifiedChanged(!clean); });
}

Document::~Document() = default;

void Document::setRenderBackend(RenderBackend *backend)
{
    m_backend = backend;
}

// ── Loading ───────────────────────────────────────────────────────────────

bool Document::open(const QString &path, QString *error, const QString &password, bool *needsPassword)
{
    clear();

    const int id = addSource(path, error, password, needsPassword);
    if (id < 0) {
        return false;
    }

    Source *src = source(id);
    QVector<PageRef> refs;
    refs.reserve(src->pageCount());
    for (int i = 0; i < src->pageCount(); ++i) {
        refs.append(PageRef { id, i, 0 });
    }

    Q_EMIT aboutToReset();
    m_pages = refs;
    Q_EMIT wasReset();

    // Carried across so that opening and saving does not silently strip the
    // title and author out of the document.
    Metadata::read(path, &m_metadata, nullptr);
    Q_EMIT metadataChanged();

    adoptOutlineOf(id);
    setFilePath(path);
    m_undo->clear();
    m_undo->setClean();
    return true;
}

int Document::addSource(const QString &path, QString *error, const QString &password, bool *needsPassword)
{
    if (needsPassword) {
        *needsPassword = false;
    }

    Source::Trouble trouble = Source::Trouble::None;
    auto src = Source::open(path, error, password, &trouble);
    if (!src) {
        if (needsPassword) {
            *needsPassword = trouble == Source::Trouble::NeedsPassword;
        }
        return -1;
    }

    const int id = static_cast<int>(m_sources.size());

    if (m_backend && !m_backend->addDocument(id, path, error)) {
        // Rendering failed but the structure parsed. That is worth surviving:
        // the user can still reorder and save, just without previews.
        qWarning("Render backend rejected %s", qPrintable(path));
    }

    m_sources.push_back(std::move(src));
    Q_EMIT sourceAdded(id);
    return id;
}

bool Document::importFile(const QString &path, int at, QString *error)
{
    const int id = addSource(path, error);
    if (id < 0) {
        return false;
    }

    Source *src = source(id);
    QVector<PageRef> refs;
    refs.reserve(src->pageCount());
    for (int i = 0; i < src->pageCount(); ++i) {
        refs.append(PageRef { id, i, 0 });
    }

    insertPages(at < 0 ? pageCount() : at, refs);
    adoptOutlineOf(id);
    return true;
}

void Document::adoptOutlineOf(int sourceId)
{
    Source *src = source(sourceId);
    if (!src) {
        return;
    }

    // Whatever table of contents the file brought with it. Merging appends the
    // second document's chapters after the first's, which is what someone
    // gluing two reports together expects to find.
    QVector<OutlineItem> incoming = Outline::readFrom(src->qpdf());
    if (incoming.isEmpty()) {
        return;
    }
    stampSource(incoming, sourceId);
    m_outline += incoming;
    Q_EMIT outlineChanged();
}

void Document::clear()
{
    Q_EMIT aboutToReset();
    m_pages.clear();
    m_outline.clear();
    if (m_backend) {
        for (size_t i = 0; i < m_sources.size(); ++i) {
            m_backend->removeDocument(static_cast<int>(i));
        }
    }
    m_sources.clear();
    m_metadata = Metadata::Fields {};
    Q_EMIT metadataChanged();
    Q_EMIT wasReset();

    m_undo->clear();
    setFilePath(QString());
}

Source *Document::source(int id) const
{
    if (id < 0 || id >= static_cast<int>(m_sources.size())) {
        return nullptr;
    }
    return m_sources[static_cast<size_t>(id)].get();
}

// ── Pages ─────────────────────────────────────────────────────────────────

PageRef Document::pageAt(int index) const
{
    if (index < 0 || index >= m_pages.size()) {
        return PageRef {};
    }
    return m_pages.at(index);
}

QSizeF Document::pageSizePoints(int index) const
{
    if (!m_backend || index < 0 || index >= m_pages.size()) {
        return QSizeF();
    }
    const PageRef ref = m_pages.at(index);
    QSizeF size = m_backend->pageSizePoints(ref.sourceId, ref.sourcePage);
    if (ref.rotation == 90 || ref.rotation == 270) {
        size.transpose();
    }
    return size;
}

// ── Session state ─────────────────────────────────────────────────────────

void Document::setFilePath(const QString &path)
{
    if (m_filePath == path) {
        return;
    }
    m_filePath = path;
    Q_EMIT filePathChanged(path);
}

QString Document::displayName() const
{
    if (m_filePath.isEmpty()) {
        return i18n("Untitled");
    }
    return QFileInfo(m_filePath).fileName();
}

bool Document::isModified() const
{
    return !m_undo->isClean();
}

// ── Mutations ─────────────────────────────────────────────────────────────

void Document::insertPages(int at, const QVector<PageRef> &refs)
{
    if (refs.isEmpty()) {
        return;
    }
    at = std::clamp(at, 0, static_cast<int>(m_pages.size()));

    Q_EMIT pagesAboutToBeInserted(at, refs.size());
    for (int i = 0; i < refs.size(); ++i) {
        m_pages.insert(at + i, refs.at(i));
    }
    Q_EMIT pagesInserted(at, refs.size());
}

QVector<PageRef> Document::removePages(QVector<int> indexes)
{
    std::sort(indexes.begin(), indexes.end());
    indexes.erase(std::unique(indexes.begin(), indexes.end()), indexes.end());

    QVector<PageRef> taken;
    taken.reserve(indexes.size());
    for (const int i : std::as_const(indexes)) {
        if (i >= 0 && i < m_pages.size()) {
            taken.append(m_pages.at(i));
        }
    }

    // Remove contiguous runs from the back so earlier indexes stay valid, and
    // so the view gets one signal per run instead of one per page.
    int i = indexes.size() - 1;
    while (i >= 0) {
        const int end = indexes.at(i);
        int start = end;
        while (i > 0 && indexes.at(i - 1) == start - 1) {
            --i;
            start = indexes.at(i);
        }
        --i;

        if (start < 0 || end >= m_pages.size()) {
            continue;
        }
        const int count = end - start + 1;
        Q_EMIT pagesAboutToBeRemoved(start, count);
        m_pages.remove(start, count);
        Q_EMIT pagesRemoved(start, count);
    }

    return taken;
}

void Document::movePages(QVector<int> indexes, int destination)
{
    std::sort(indexes.begin(), indexes.end());

    // Removing the block shifts everything after it; the drop target has to
    // move left by however many selected pages sat in front of it.
    int removedBefore = 0;
    for (const int i : std::as_const(indexes)) {
        if (i < destination) {
            ++removedBefore;
        }
    }

    const QVector<PageRef> taken = removePages(indexes);
    insertPages(destination - removedBefore, taken);
}

void Document::applyRotation(const QVector<int> &indexes, int delta)
{
    if (indexes.isEmpty() || delta % 360 == 0) {
        return;
    }

    int first = m_pages.size();
    int last = -1;
    for (const int i : indexes) {
        if (i < 0 || i >= m_pages.size()) {
            continue;
        }
        m_pages[i].rotation = normalizeRotation(m_pages.at(i).rotation + delta);
        first = std::min(first, i);
        last = std::max(last, i);
    }

    if (last >= 0) {
        Q_EMIT pagesChanged(first, last);
    }
}

void Document::setRotation(const QVector<int> &indexes, int absolute)
{
    const int value = normalizeRotation(absolute);

    int first = m_pages.size();
    int last = -1;
    for (const int i : indexes) {
        if (i < 0 || i >= m_pages.size()) {
            continue;
        }
        m_pages[i].rotation = value;
        first = std::min(first, i);
        last = std::max(last, i);
    }

    if (last >= 0) {
        Q_EMIT pagesChanged(first, last);
    }
}

QVector<OutlineItem> Document::outline() const
{
    QHash<QPair<int, int>, int> rowOf;
    // First occurrence wins: a page duplicated by the user should not move the
    // bookmark to the copy.
    for (int row = m_pages.size() - 1; row >= 0; --row) {
        rowOf.insert({ m_pages.at(row).sourceId, m_pages.at(row).sourcePage }, row);
    }

    QVector<OutlineItem> resolved = m_outline;
    resolveRows(resolved, rowOf);
    return resolved;
}

void Document::setOutline(const QVector<OutlineItem> &items)
{
    m_outline = items;
    anchorRows(m_outline, m_pages);
    Q_EMIT outlineChanged();
}

void Document::setMetadata(const Metadata::Fields &fields)
{
    m_metadata = fields;
    Q_EMIT metadataChanged();
}

void Document::replacePage(int index, const PageRef &ref)
{
    if (index < 0 || index >= m_pages.size()) {
        return;
    }
    m_pages[index] = ref;
    Q_EMIT pagesChanged(index, index);
}

void Document::setPages(const QVector<PageRef> &refs)
{
    Q_EMIT aboutToReset();
    m_pages = refs;
    Q_EMIT wasReset();
}

} // namespace ps
