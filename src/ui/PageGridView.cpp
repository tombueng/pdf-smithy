/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "PageGridView.h"

#include "PageDelegate.h"

#include <QApplication>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QPainter>
#include <QResizeEvent>
#include <QScrollBar>
#include <QStyle>
#include <QWheelEvent>

#include "PageModel.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace ps {

namespace {

constexpr int ZoomStep = 20;

} // namespace

PageGridView::PageGridView(QWidget *parent)
    : QListView(parent)
    , m_delegate(new PageDelegate(this))
{
    setItemDelegate(m_delegate);

    setViewMode(QListView::IconMode);
    // Static movement keeps Qt computing drop positions between items instead
    // of letting icons be dropped at free coordinates.
    setMovement(QListView::Static);
    setFlow(QListView::LeftToRight);
    setWrapping(true);
    setResizeMode(QListView::Adjust);
    setLayoutMode(QListView::Batched);
    setBatchSize(64);
    setUniformItemSizes(true);
    setSpacing(6);

    setSelectionMode(QAbstractItemView::ExtendedSelection);
    setSelectionBehavior(QAbstractItemView::SelectItems);

    setDragEnabled(true);
    setAcceptDrops(true);
    // Explicitly on the viewport, not just on the view. Drag events are
    // delivered to whichever widget is registered as a drop site, and that is
    // the viewport: setting the flag on the view alone left the grid silently
    // refusing every drag, with no error anywhere to show for it.
    viewport()->setAcceptDrops(true);
    setDropIndicatorShown(true);
    // The mode still matters for cursor feedback, but every drag event is
    // handled below rather than by QListView; see the header for why.
    setDragDropMode(QAbstractItemView::DragDrop);
    setDefaultDropAction(Qt::MoveAction);

    setMouseTracking(true);
    setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setFrameShape(QFrame::NoFrame);

    connect(this, &QAbstractItemView::doubleClicked, this, [this](const QModelIndex &index) {
        if (index.isValid()) {
            Q_EMIT pageActivated(index.row());
        }
    });

    applyGrid();
}

int PageGridView::thumbnailWidth() const
{
    return m_delegate->thumbnailWidth();
}

void PageGridView::setThumbnailWidth(int pixels)
{
    // A width the user asked for by hand, so whatever fit was in force is over.
    if (m_fit != Fit::Free) {
        m_fit = Fit::Free;
        Q_EMIT fitChanged(m_fit);
    }
    applyWidth(pixels);
}

void PageGridView::applyWidth(int pixels)
{
    pixels = std::clamp(pixels, MinimumWidth, MaximumWidth);
    if (pixels == m_delegate->thumbnailWidth()) {
        return;
    }

    m_delegate->setThumbnailWidth(pixels);
    applyGrid();
    Q_EMIT thumbnailWidthChanged(m_delegate->thumbnailWidth());
}

void PageGridView::applyGrid()
{
    const QSize cell = m_delegate->gridSize();
    setGridSize(cell);

    // Normally off, because a wrapping grid has nothing to scroll to sideways.
    // Once one page is wider than the window (which "fit height" on a tall
    // page does on purpose), the rest of that page has to be reachable, and
    // without this it is simply cut off.
    setHorizontalScrollBarPolicy(cell.width() > viewport()->width() ? Qt::ScrollBarAsNeeded : Qt::ScrollBarAlwaysOff);

    // Batched layout caches geometry; without this the grid keeps the old cell
    // size until something else forces a relayout.
    scheduleDelayedItemsLayout();
    viewport()->update();
}

void PageGridView::zoomIn()
{
    setThumbnailWidth(thumbnailWidth() + ZoomStep);
}

void PageGridView::zoomOut()
{
    setThumbnailWidth(thumbnailWidth() - ZoomStep);
}

// ── Fitting ───────────────────────────────────────────────────────────────

void PageGridView::setFit(Fit mode)
{
    if (m_fit != mode) {
        m_fit = mode;
        Q_EMIT fitChanged(m_fit);
    }
    refit();
}

int PageGridView::widthForColumns(int columns) const
{
    // The vertical scrollbar counts even when it is not there yet. A fitted
    // page that is exactly the viewport wide makes the content taller than the
    // window, the bar appears, the viewport narrows, and the page no longer
    // fits, so it is subtracted up front rather than discovered afterwards.
    int usableWidth = viewport()->width() - 2 * spacing();
    if (!verticalScrollBar()->isVisible()) {
        usableWidth -= style()->pixelMetric(QStyle::PM_ScrollBarExtent, nullptr, this);
    }
    return m_delegate->widthForCellWidth(usableWidth / std::max(1, columns));
}

void PageGridView::showOverview()
{
    // Not a fit of its own: what this answers is "show me a lot of pages", and
    // the moment the user says anything about the size themselves that answer
    // has been overruled, which is exactly what Fit::Free already means.
    if (m_fit != Fit::Free) {
        m_fit = Fit::Free;
        Q_EMIT fitChanged(m_fit);
    }
    // Clamped by applyWidth, so on a narrow window this quietly becomes as many
    // columns as fit rather than eight unrecognisable ones.
    applyWidth(widthForColumns(OverviewColumns));
}

int PageGridView::widthForFit(Fit mode) const
{
    if (mode == Fit::Free) {
        return thumbnailWidth();
    }

    const QSize area = viewport()->size();
    const int perRow = mode == Fit::TwoPages ? 2 : 1;
    const int byWidth = widthForColumns(perRow);
    const int byHeight = m_delegate->widthForCellHeight(area.height() - 2 * spacing());

    switch (mode) {
    case Fit::Width:
        return byWidth;
    case Fit::Height:
        return byHeight;
    case Fit::Page:
    case Fit::TwoPages:
        return std::min(byWidth, byHeight);
    case Fit::Free:
        break;
    }
    return thumbnailWidth();
}

void PageGridView::refit()
{
    if (m_fit == Fit::Free || m_fitting || !viewport()->isVisible()) {
        return;
    }

    // Changing the width relays out the grid, which can show or hide the
    // scrollbar, which resizes the viewport, which asks for another fit. One
    // pass is enough; the second would only chase the first.
    m_fitting = true;
    applyWidth(widthForFit(m_fit));
    m_fitting = false;
}

void PageGridView::refreshCellRatio()
{
    // Tallest rather than average or first: the cell has to hold every page, so
    // the one page in landscape among two hundred decides nothing, and the one
    // page in portrait among two hundred landscape ones decides everything.
    qreal tallest = 0.0;
    if (model()) {
        const int rows = model()->rowCount();
        for (int row = 0; row < rows; ++row) {
            const qreal aspect = model()->index(row, 0).data(PageModel::AspectRatioRole).toReal();
            if (aspect > 0.0) {
                tallest = std::max(tallest, 1.0 / aspect);
            }
        }
    }
    if (tallest <= 0.0) {
        tallest = 297.0 / 210.0; // an empty window still has to look like paper
    }

    if (std::abs(tallest - m_delegate->cellRatio()) < 0.001) {
        return;
    }
    m_delegate->setCellRatio(tallest);
    applyGrid();
    refit();
}

void PageGridView::setModel(QAbstractItemModel *model)
{
    if (QAbstractItemModel *old = QListView::model()) {
        old->disconnect(this);
    }
    QListView::setModel(model);

    if (model) {
        // Every one of these can bring in a page of a shape the grid has no
        // room for, and a cell that is too short crops nothing; it just draws
        // the page smaller than it needs to be, which is the sort of wrong that
        // never gets reported and always gets noticed.
        connect(model, &QAbstractItemModel::modelReset, this, &PageGridView::refreshCellRatio);
        connect(model, &QAbstractItemModel::rowsInserted, this, &PageGridView::refreshCellRatio);
        connect(model, &QAbstractItemModel::rowsRemoved, this, &PageGridView::refreshCellRatio);
        connect(model, &QAbstractItemModel::layoutChanged, this, &PageGridView::refreshCellRatio);
    }
    refreshCellRatio();
}

void PageGridView::resizeEvent(QResizeEvent *event)
{
    QListView::resizeEvent(event);
    refit();
}

void PageGridView::wheelEvent(QWheelEvent *event)
{
    if (event->modifiers() & Qt::ControlModifier) {
        const int steps = event->angleDelta().y() / 120;
        if (steps != 0) {
            // Keep whatever the pointer is over roughly in place, so zooming
            // does not throw the user to a different part of the document.
            const QModelIndex anchor = indexAt(event->position().toPoint());
            setThumbnailWidth(thumbnailWidth() + steps * ZoomStep);
            if (anchor.isValid()) {
                scrollTo(anchor, QAbstractItemView::PositionAtCenter);
            }
        }
        event->accept();
        return;
    }

    QListView::wheelEvent(event);
}

// ── Drag and drop ─────────────────────────────────────────────────────────

bool PageGridView::acceptsDrag(const QMimeData *mime) const
{
    return mime && (mime->hasFormat(PageModel::pageMimeType()) || mime->hasUrls());
}

int PageGridView::insertionIndexAt(const QPoint &pos) const
{
    if (!model()) {
        return 0;
    }
    const int rows = model()->rowCount();
    if (rows == 0) {
        return 0;
    }

    // Straight over a page: the near half means "before it", the far half
    // "after it". That is the gesture people already know from every list.
    const QModelIndex under = indexAt(pos);
    if (under.isValid()) {
        const QRect rect = visualRect(under);
        return pos.x() < rect.center().x() ? under.row() : under.row() + 1;
    }

    // In the gaps, or past the last page: fall back to whichever page edge is
    // nearest, so dropping into empty space below the grid appends rather than
    // doing nothing.
    int best = rows;
    int bestDistance = std::numeric_limits<int>::max();
    for (int row = 0; row < rows; ++row) {
        const QRect rect = visualRect(model()->index(row, 0));
        if (!rect.isValid()) {
            continue;
        }
        const int centreY = rect.center().y();
        const int toLeft = std::abs(pos.x() - rect.left()) + std::abs(pos.y() - centreY);
        const int toRight = std::abs(pos.x() - rect.right()) + std::abs(pos.y() - centreY);
        if (toLeft < bestDistance) {
            bestDistance = toLeft;
            best = row;
        }
        if (toRight < bestDistance) {
            bestDistance = toRight;
            best = row + 1;
        }
    }
    return best;
}

void PageGridView::startDrag(Qt::DropActions supportedActions)
{
    Q_UNUSED(supportedActions)

    const QModelIndexList indexes = selectionModel() ? selectionModel()->selectedIndexes() : QModelIndexList();
    if (indexes.isEmpty() || !model()) {
        return;
    }

    QMimeData *mime = model()->mimeData(indexes);
    if (!mime) {
        return;
    }

    // Drag the page itself, not an abstract rectangle: the user is moving a
    // specific sheet and should be able to see which one.
    const QRect first = visualRect(indexes.constFirst());
    QPixmap pixmap = viewport()->grab(first);
    if (indexes.size() > 1) {
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QString badge = QString::number(indexes.size());
        QFont font = painter.font();
        font.setBold(true);
        painter.setFont(font);
        const QRect chip(6, 6, std::max(24, painter.fontMetrics().horizontalAdvance(badge) + 16), 24);
        painter.setPen(Qt::NoPen);
        painter.setBrush(palette().color(QPalette::Highlight));
        painter.drawRoundedRect(chip, 12, 12);
        painter.setPen(palette().color(QPalette::HighlightedText));
        painter.drawText(chip, Qt::AlignCenter, badge);
    }

    auto *drag = new QDrag(this);
    drag->setMimeData(mime);
    drag->setPixmap(pixmap);
    drag->setHotSpot(QPoint(pixmap.width() / 2, pixmap.height() / 2));
    drag->exec(Qt::MoveAction, Qt::MoveAction);
}

void PageGridView::dragEnterEvent(QDragEnterEvent *event)
{
    if (!acceptsDrag(event->mimeData())) {
        event->ignore();
        return;
    }
    event->setDropAction(Qt::MoveAction);
    event->accept();
}

void PageGridView::dragMoveEvent(QDragMoveEvent *event)
{
    if (!acceptsDrag(event->mimeData())) {
        event->ignore();
        return;
    }

    const int row = insertionIndexAt(event->position().toPoint());
    if (row != m_dropRow) {
        m_dropRow = row;
        viewport()->update();
    }

    event->setDropAction(Qt::MoveAction);
    event->accept();
}

void PageGridView::dragLeaveEvent(QDragLeaveEvent *event)
{
    m_dropRow = -1;
    viewport()->update();
    QListView::dragLeaveEvent(event);
}

void PageGridView::dropEvent(QDropEvent *event)
{
    const int row = m_dropRow >= 0 ? m_dropRow : insertionIndexAt(event->position().toPoint());
    m_dropRow = -1;
    viewport()->update();

    if (!acceptsDrag(event->mimeData()) || !model()) {
        event->ignore();
        return;
    }

    if (model()->dropMimeData(event->mimeData(), Qt::MoveAction, row, 0, QModelIndex())) {
        event->setDropAction(Qt::MoveAction);
        event->accept();
    } else {
        event->ignore();
    }
}

void PageGridView::paintEvent(QPaintEvent *event)
{
    QListView::paintEvent(event);

    if (m_dropRow < 0 || !model() || model()->rowCount() == 0) {
        return;
    }

    // A vertical bar in the gap the pages will open up, rather than a box
    // around a page: the pages move apart, they are not replaced.
    QRect reference;
    int x = 0;
    if (m_dropRow < model()->rowCount()) {
        reference = visualRect(model()->index(m_dropRow, 0));
        x = reference.left() - spacing();
    } else {
        reference = visualRect(model()->index(model()->rowCount() - 1, 0));
        x = reference.right() + spacing();
    }
    if (!reference.isValid()) {
        return;
    }

    QPainter painter(viewport());
    painter.setRenderHint(QPainter::Antialiasing, true);
    QColor colour = palette().color(QPalette::Highlight);
    painter.setPen(QPen(colour, 3, Qt::SolidLine, Qt::RoundCap));
    painter.drawLine(x, reference.top() + 2, x, reference.bottom() - 2);
    painter.setBrush(colour);
    painter.setPen(Qt::NoPen);
    painter.drawEllipse(QPoint(x, reference.top() + 2), 4, 4);
    painter.drawEllipse(QPoint(x, reference.bottom() - 2), 4, 4);
}

QVector<int> PageGridView::selectedRows() const
{
    QVector<int> rows;
    const QModelIndexList indexes = selectionModel() ? selectionModel()->selectedIndexes() : QModelIndexList();
    rows.reserve(indexes.size());
    for (const QModelIndex &index : indexes) {
        if (index.isValid()) {
            rows.append(index.row());
        }
    }
    std::sort(rows.begin(), rows.end());
    rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
    return rows;
}

void PageGridView::selectRows(const QVector<int> &rows)
{
    if (!selectionModel() || !model()) {
        return;
    }

    QItemSelection selection;
    for (const int row : rows) {
        if (row >= 0 && row < model()->rowCount()) {
            const QModelIndex index = model()->index(row, 0);
            selection.select(index, index);
        }
    }

    selectionModel()->select(selection, QItemSelectionModel::ClearAndSelect);
    if (!rows.isEmpty()) {
        scrollTo(model()->index(rows.first(), 0), QAbstractItemView::EnsureVisible);
    }
}

} // namespace ps
