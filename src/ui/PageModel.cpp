/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "PageModel.h"

#include "core/Document.h"
#include "core/RenderBackend.h"
#include "core/RenderCache.h"
#include "core/Source.h"
#include "core/commands/PageCommands.h"

#include <QDataStream>
#include <QFileInfo>
#include <QMimeData>
#include <QTransform>
#include <QUndoStack>
#include <QUrl>

#include <KLocalizedString>

namespace ps {

namespace {

quint64 sizeKey(int sourceId, int sourcePage)
{
    return (static_cast<quint64>(static_cast<quint32>(sourceId)) << 32) | static_cast<quint32>(sourcePage);
}

} // namespace

QString PageModel::pageMimeType()
{
    return QStringLiteral("application/x-pdf-smithy-pages");
}

PageModel::PageModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

PageModel::~PageModel() = default;

void PageModel::setDocument(Document *document)
{
    if (m_document == document) {
        return;
    }

    beginResetModel();
    if (m_document) {
        m_document->disconnect(this);
    }
    m_document = document;
    m_sizeCache.clear();
    connectDocument();
    endResetModel();
}

void PageModel::connectDocument()
{
    if (!m_document) {
        return;
    }

    connect(m_document, &Document::pagesAboutToBeInserted, this,
            [this](int at, int count) { beginInsertRows(QModelIndex(), at, at + count - 1); });
    connect(m_document, &Document::pagesInserted, this, [this] { endInsertRows(); });

    connect(m_document, &Document::pagesAboutToBeRemoved, this,
            [this](int at, int count) { beginRemoveRows(QModelIndex(), at, at + count - 1); });
    connect(m_document, &Document::pagesRemoved, this, [this] { endRemoveRows(); });

    connect(m_document, &Document::pagesChanged, this,
            [this](int first, int last) { Q_EMIT dataChanged(index(first), index(last)); });

    connect(m_document, &Document::aboutToReset, this, [this] { beginResetModel(); });
    connect(m_document, &Document::wasReset, this, [this] {
        m_sizeCache.clear();
        endResetModel();
    });
}

void PageModel::setThumbnailWidth(int pixels)
{
    if (m_thumbnailWidth == pixels) {
        return;
    }
    m_thumbnailWidth = pixels;
    if (m_document && rowCount() > 0) {
        Q_EMIT dataChanged(index(0, 0), index(rowCount() - 1, 0), { ThumbnailRole });
    }
}

void PageModel::setRenderCache(RenderCache *cache)
{
    if (m_cache == cache) {
        return;
    }
    if (m_cache) {
        disconnect(m_cache, nullptr, this, nullptr);
    }
    m_cache = cache;
    if (m_cache) {
        connect(m_cache, &RenderCache::thumbnailReady, this, &PageModel::onThumbnailReady);
    }
}

void PageModel::onThumbnailReady(int sourceId, int sourcePage)
{
    if (!m_document) {
        return;
    }

    // One render can satisfy several rows at once, because the same source page may
    // appear many times in a document after duplication.
    const int count = m_document->pageCount();
    for (int row = 0; row < count; ++row) {
        const PageRef ref = m_document->pageAt(row);
        if (ref.sourceId == sourceId && ref.sourcePage == sourcePage) {
            Q_EMIT dataChanged(index(row), index(row), { ThumbnailRole });
        }
    }
}

int PageModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid() || !m_document) {
        return 0;
    }
    return m_document->pageCount();
}

qreal PageModel::aspectRatioFor(int row) const
{
    const PageRef ref = m_document->pageAt(row);
    const quint64 key = sizeKey(ref.sourceId, ref.sourcePage);

    auto it = m_sizeCache.constFind(key);
    if (it == m_sizeCache.constEnd()) {
        QSizeF size;
        if (RenderBackend *backend = m_document->renderBackend()) {
            size = backend->pageSizePoints(ref.sourceId, ref.sourcePage);
        }
        if (size.isEmpty()) {
            size = QSizeF(210, 297); // A4, a sane fallback
        }
        it = m_sizeCache.insert(key, size);
    }

    QSizeF size = *it;
    if (ref.rotation == 90 || ref.rotation == 270) {
        size.transpose();
    }
    return size.height() > 0 ? size.width() / size.height() : 1.0;
}

QVariant PageModel::data(const QModelIndex &index, int role) const
{
    if (!m_document || !index.isValid() || index.row() >= m_document->pageCount()) {
        return {};
    }

    const int row = index.row();
    const PageRef ref = m_document->pageAt(row);

    switch (role) {
    case Qt::DisplayRole:
    case PageNumberRole:
        return row + 1;

    case ThumbnailRole: {
        if (!m_cache) {
            return {};
        }
        const QImage image = m_thumbnailWidth > 0 ? m_cache->thumbnail(ref.sourceId, ref.sourcePage, m_thumbnailWidth)
                                                  : m_cache->thumbnail(ref.sourceId, ref.sourcePage);
        if (image.isNull() || ref.rotation == 0) {
            return image;
        }
        // Rotating here rather than at render time is what lets the cache
        // survive a turn: the pixels are the same, only the presentation moves.
        return image.transformed(QTransform().rotate(ref.rotation), Qt::SmoothTransformation);
    }

    case RotationRole:
        return ref.rotation;
    case SourceIdRole:
        return ref.sourceId;
    case SourcePageRole:
        return ref.sourcePage;
    case AspectRatioRole:
        return aspectRatioFor(row);

    case SourceNameRole: {
        Source *source = m_document->source(ref.sourceId);
        return source ? QFileInfo(source->path()).fileName() : QString();
    }

    case Qt::ToolTipRole: {
        Source *source = m_document->source(ref.sourceId);
        const QString origin = source ? QFileInfo(source->path()).fileName() : i18n("unknown file");
        return i18nc("@info:tooltip page %1 of document, from file %2, original page %3", "Page %1\nFrom %2, page %3",
                     row + 1, origin, ref.sourcePage + 1);
    }

    default:
        return {};
    }
}

Qt::ItemFlags PageModel::flags(const QModelIndex &index) const
{
    Qt::ItemFlags flags = QAbstractListModel::flags(index);
    if (index.isValid()) {
        flags |= Qt::ItemIsDragEnabled;
    }
    // Dropping onto empty space appends, so the root has to accept drops too.
    return flags | Qt::ItemIsDropEnabled;
}

// ── Drag and drop ─────────────────────────────────────────────────────────

Qt::DropActions PageModel::supportedDropActions() const
{
    return Qt::MoveAction | Qt::CopyAction;
}

Qt::DropActions PageModel::supportedDragActions() const
{
    return Qt::MoveAction | Qt::CopyAction;
}

QStringList PageModel::mimeTypes() const
{
    // URLs come second but matter just as much: dropping a PDF from Dolphin
    // onto the grid is how merging is meant to feel.
    return { pageMimeType(), QStringLiteral("text/uri-list") };
}

QMimeData *PageModel::mimeData(const QModelIndexList &indexes) const
{
    QVector<int> rows;
    rows.reserve(indexes.size());
    for (const QModelIndex &index : indexes) {
        if (index.isValid()) {
            rows.append(index.row());
        }
    }
    if (rows.isEmpty()) {
        return nullptr;
    }

    std::sort(rows.begin(), rows.end());

    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);
    stream << rows;

    auto *mime = new QMimeData;
    mime->setData(pageMimeType(), payload);
    return mime;
}

bool PageModel::canDropMimeData(const QMimeData *data, Qt::DropAction action, int row, int column,
                                const QModelIndex &parent) const
{
    Q_UNUSED(action)
    Q_UNUSED(row)
    Q_UNUSED(column)
    Q_UNUSED(parent)

    if (!m_document) {
        return false;
    }
    if (data->hasFormat(pageMimeType())) {
        return true;
    }
    if (data->hasUrls()) {
        const QList<QUrl> urls = data->urls();
        return std::any_of(urls.cbegin(), urls.cend(), [](const QUrl &url) { return url.isLocalFile(); });
    }
    return false;
}

bool PageModel::dropMimeData(const QMimeData *data, Qt::DropAction action, int row, int column,
                             const QModelIndex &parent)
{
    Q_UNUSED(column)

    if (!m_document || action == Qt::IgnoreAction) {
        return false;
    }

    // Qt reports the target three different ways depending on whether the
    // pointer was between items, on an item, or past the end.
    int destination = row;
    if (destination < 0) {
        destination = parent.isValid() ? parent.row() : m_document->pageCount();
    }

    if (data->hasFormat(pageMimeType())) {
        QByteArray payload = data->data(pageMimeType());
        QDataStream stream(&payload, QIODevice::ReadOnly);
        QVector<int> rows;
        stream >> rows;
        if (rows.isEmpty()) {
            return false;
        }

        m_document->undoStack()->push(new MovePagesCommand(m_document, rows, destination));
        return true;
    }

    if (data->hasUrls()) {
        bool any = false;
        auto *macro = new QUndoCommand(i18nc("@action:inmenu undo step", "Insert pages from file"));

        for (const QUrl &url : data->urls()) {
            if (!url.isLocalFile()) {
                continue;
            }
            QString error;
            const int sourceId = m_document->addSource(url.toLocalFile(), &error);
            if (sourceId < 0) {
                continue;
            }

            Source *source = m_document->source(sourceId);
            QVector<PageRef> refs;
            refs.reserve(source->pageCount());
            for (int i = 0; i < source->pageCount(); ++i) {
                refs.append(PageRef { sourceId, i, 0 });
            }

            new InsertPagesCommand(m_document, destination, refs, QString(), macro);
            destination += refs.size();
            any = true;
        }

        if (!any) {
            delete macro;
            return false;
        }
        m_document->undoStack()->push(macro);
        return true;
    }

    return false;
}

} // namespace ps
