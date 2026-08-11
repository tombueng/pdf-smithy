/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QSizeF>

namespace ps {

class Document;
class RenderCache;

/**
 * Adapts a Document's page list to Qt's item views.
 *
 * The model is a thin translator, not a second source of truth: it owns no
 * page state and forwards every structural signal from the Document into the
 * matching begin/end calls. Edits go the other way through undo commands, so
 * dragging pages around in the grid lands on the same stack as the menu items.
 */
class PageModel : public QAbstractListModel
{
    Q_OBJECT

public:
    enum Role {
        ThumbnailRole = Qt::UserRole + 1, //!< QImage, already rotated
        RotationRole, //!< int, degrees
        SourceIdRole,
        SourcePageRole,
        SourceNameRole, //!< file the page came from, for merged documents
        AspectRatioRole, //!< qreal, width / height including rotation
        PageNumberRole, //!< 1-based position in this document
    };
    Q_ENUM(Role)

    /** The drag payload for reordering inside the grid. */
    static QString pageMimeType();

    explicit PageModel(QObject *parent = nullptr);
    ~PageModel() override;

    void setDocument(Document *document);
    Document *document() const { return m_document; }

    void setRenderCache(RenderCache *cache);

    /**
     * The width the view using this model draws thumbnails at.
     *
     * Asked for by name rather than left to the cache's one shared size: the
     * strip draws at 180 pixels and the grid at whatever its zoom says, and a
     * single size for both meant the strip was rendering every page six times
     * wider than it drew it: five times the work and thirty-six times the
     * memory, thrown away at the moment of drawing. Zero means "whatever the
     * cache is set to", which is what a model with no view of its own gets.
     */
    void setThumbnailWidth(int pixels);

    // ── QAbstractListModel ────────────────────────────────────────────────

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;

    Qt::DropActions supportedDropActions() const override;
    Qt::DropActions supportedDragActions() const override;
    QStringList mimeTypes() const override;
    QMimeData *mimeData(const QModelIndexList &indexes) const override;
    bool canDropMimeData(const QMimeData *data, Qt::DropAction action, int row, int column,
                         const QModelIndex &parent) const override;
    bool dropMimeData(const QMimeData *data, Qt::DropAction action, int row, int column,
                      const QModelIndex &parent) override;

private:
    void connectDocument();
    void onThumbnailReady(int sourceId, int sourcePage);
    qreal aspectRatioFor(int row) const;

    Document *m_document = nullptr;
    int m_thumbnailWidth = 0;
    RenderCache *m_cache = nullptr;

    /** Page geometry is asked for on every paint; the backend lock is not. */
    mutable QHash<quint64, QSizeF> m_sizeCache;
};

} // namespace ps
