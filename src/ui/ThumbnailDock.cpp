/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "ThumbnailDock.h"

#include "PageDelegate.h"
#include "PageModel.h"

#include <KConfigGroup>
#include <KLocalizedString>
#include <KSharedConfig>

#include <QAbstractItemModel>
#include <QHBoxLayout>
#include <QItemSelectionModel>
#include <QListView>
#include <QMainWindow>
#include <QPainter>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSlider>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <functional>

namespace ps {

namespace {

/** How much one notch of Ctrl+wheel moves the thumbnail size. */
constexpr int ZoomStep = 20;

/** The size the strip starts at when the last session left nothing behind. */
constexpr int DefaultWidth = 140;

/**
 * How many pages are measured to decide what shape the cells are.
 *
 * Measuring a page takes the render backend's lock, so a document of four
 * hundred would pay four hundred of those before the strip drew anything. The
 * cells only need a shape to reserve room in, pages after the first few are
 * almost always the same shape as those, and a page that turns out to be taller
 * than the cell is drawn smaller inside it rather than cropped.
 */
constexpr int RatioSample = 8;

/** How wide the bar along the edge of the current page is. */
constexpr int MarkWidth = 4;

/**
 * How lopsided a floating panel has to be before its shape decides how it reads.
 *
 * A panel dragged to very nearly square would otherwise flip between running
 * down and running across as the pointer moved, because turning the strip
 * changes which way its own floor points and a floor can shove the window back
 * over a boundary it has only just crossed. Below this it keeps what it has.
 */
constexpr double ShapeMargin = 1.25;

/**
 * How much of the strip is kept clear at each end for the reading point.
 *
 * A quarter at the top and a quarter at the bottom, which leaves the reading
 * point free to wander the middle half of the strip before anything moves. See
 * trackPosition() for why there is a band here at all rather than a fixed line.
 */
constexpr double BandMargin = 0.25;

/**
 * How long the strip is left to the user after they last moved it themselves.
 *
 * Long enough that reaching for the scrollbar, dragging, letting go and looking
 * is one gesture rather than a fight; short enough that going back to the
 * document and reading on visibly puts the strip back in charge of itself.
 */
constexpr int HandBackDelay = 1200;

/**
 * The page delegate, plus which page the document view is showing.
 *
 * Selection alone will not do here. The strip is glanced at rather than read,
 * and a selection is what the *strip* was last clicked on: after scrolling the
 * document with the wheel those are two different pages, and the one worth
 * showing is the one being read. So the mark is drawn as a bar down the edge of
 * the cell, which survives the page being scrolled half out of sight.
 */
class MarkingDelegate : public PageDelegate
{
public:
    using PageDelegate::PageDelegate;

    void setCurrentRow(int row) { m_currentRow = row; }

    /** Which way the strip runs, which is which edge of a cell the mark sits on. */
    void setOrientation(Qt::Orientation orientation) { m_orientation = orientation; }

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        PageDelegate::paint(painter, option, index);
        if (index.row() != m_currentRow) {
            return;
        }

        // Along one whole side of the cell, and which side follows the way the
        // strip is read: down the leading edge in a strip read downwards, so
        // that a column of marks lines up whichever way round the interface
        // runs, and across the top in one read sideways, where a bar down the
        // side would look like a rule drawn between two pages rather than a
        // mark on one of them.
        const QRect cell = option.rect;
        QRect bar;
        if (m_orientation == Qt::Vertical) {
            const int left = option.direction == Qt::RightToLeft ? cell.right() - MarkWidth + 1 : cell.left();
            bar = QRect(left, cell.top() + 2, MarkWidth, cell.height() - 4);
        } else {
            bar = QRect(cell.left() + 2, cell.top(), cell.width() - 4, MarkWidth);
        }

        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        painter->setPen(Qt::NoPen);
        painter->setBrush(option.palette.color(QPalette::Highlight));
        painter->drawRoundedRect(bar, MarkWidth / 2.0, MarkWidth / 2.0);
        painter->restore();
    }

private:
    int m_currentRow = -1;
    Qt::Orientation m_orientation = Qt::Vertical;
};

/**
 * A list that zooms on Ctrl+wheel.
 *
 * The same gesture as the grid in the middle of the window, because a user who
 * has learnt it there will try it here, and a strip that scrolled instead would
 * read as the gesture being ignored.
 */
class StripView : public QListView
{
public:
    using QListView::QListView;

    std::function<void(int)> zoomed;

    /** Called when the wheel is about to scroll the strip rather than zoom it. */
    std::function<void()> scrolledByHand;

    /** True while the strip runs across the window rather than down it. */
    bool sideways = false;

protected:
    void wheelEvent(QWheelEvent *event) override
    {
        const int steps = event->angleDelta().y() / 120;
        if ((event->modifiers() & Qt::ControlModifier) && zoomed && steps != 0) {
            zoomed(steps);
            event->accept();
            return;
        }
        // Told from here rather than worked out from the scrollbar afterwards.
        // The bar cannot tell a value the wheel put there from one a layout
        // change clamped into range, and mistaking the second for the first
        // would hand the strip to a user who never touched it.
        if (scrolledByHand) {
            scrolledByHand();
        }

        if (sideways && event->angleDelta().x() == 0 && event->pixelDelta().x() == 0) {
            // A mouse wheel turns one way only, and a scroll area hands what it
            // reports to the scrollbar of that same axis, which in a strip laid
            // out across the window is the bar with nothing behind it, so the
            // wheel would simply do nothing. Turned a quarter, so that the
            // gesture which walks a strip down the side of the window walks
            // this one along the top of it.
            QWheelEvent turned(event->position(), event->globalPosition(),
                               QPoint(event->pixelDelta().y(), event->pixelDelta().x()),
                               QPoint(event->angleDelta().y(), event->angleDelta().x()), event->buttons(),
                               event->modifiers(), event->phase(), event->inverted());
            QListView::wheelEvent(&turned);
            event->setAccepted(turned.isAccepted());
            return;
        }

        QListView::wheelEvent(event);
    }
};

} // namespace

ThumbnailDock::ThumbnailDock(QWidget *parent)
    : QDockWidget(i18nc("@title:window", "Pages"), parent)
    , m_list(new StripView(this))
    , m_delegate(new MarkingDelegate(this))
    , m_size(new QSlider(Qt::Horizontal, this))
{
    setObjectName(QStringLiteral("thumbnail_dock"));
    setAllowedAreas(Qt::AllDockWidgetAreas);
    setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable | QDockWidget::DockWidgetClosable);

    m_list->setItemDelegate(m_delegate);

    m_list->setViewMode(QListView::ListMode);
    // Wrapping is on whichever way the strip runs, and the flow is what says
    // which way that is; applyOrientation() sets it. The number of lines then
    // falls out of the cell size and the room there is, rather than being fixed
    // at one: a panel dragged wide with the thumbnails small shows several pages
    // abreast, which is the only way a strip can be an overview as well as a
    // place to click.
    m_list->setWrapping(true);
    m_list->setResizeMode(QListView::Adjust);
    // With uniform sizes the view measures one cell and multiplies, so laying
    // out four hundred rows is arithmetic rather than four hundred size hints,
    // and the delegate is asked to paint only the dozen cells on screen, which
    // is what keeps the render cache from ever hearing about the rest. Wrapping
    // does not cost this: measured over four hundred rows, a wrapped layout
    // asks the delegate for one size hint just as an unwrapped one does,
    // against four hundred and one apiece with uniform sizes off.
    m_list->setUniformItemSizes(true);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_list->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_list->setFrameShape(QFrame::NoFrame);
    // Reordering pages is the grid's job. A drag started here would be a page
    // move nobody asked for, in a panel whose whole purpose is to be clicked.
    m_list->setDragDropMode(QAbstractItemView::NoDragDrop);

    m_size->setRange(MinimumThumbnailWidth, MaximumThumbnailWidth);
    m_size->setPageStep(ZoomStep);
    m_size->setToolTip(i18nc("@info:tooltip", "How large the page thumbnails are drawn"));
    m_size->setAccessibleName(i18nc("@label", "Thumbnail size"));

    auto *smaller = new QToolButton(this);
    smaller->setAutoRaise(true);
    smaller->setIcon(QIcon::fromTheme(QStringLiteral("zoom-out")));
    smaller->setToolTip(i18nc("@info:tooltip", "Narrower thumbnails"));
    connect(smaller, &QToolButton::clicked, this, [this] { setThumbnailWidth(thumbnailWidth() - ZoomStep); });

    auto *bigger = new QToolButton(this);
    bigger->setAutoRaise(true);
    bigger->setIcon(QIcon::fromTheme(QStringLiteral("zoom-in")));
    bigger->setToolTip(i18nc("@info:tooltip", "Wider thumbnails"));
    connect(bigger, &QToolButton::clicked, this, [this] { setThumbnailWidth(thumbnailWidth() + ZoomStep); });

    auto *footer = new QHBoxLayout;
    footer->setContentsMargins(0, 0, 0, 0);
    footer->addWidget(smaller);
    footer->addWidget(m_size, 1);
    footer->addWidget(bigger);

    auto *body = new QWidget(this);
    auto *layout = new QVBoxLayout(body);
    layout->setContentsMargins(2, 2, 2, 2);
    layout->setSpacing(2);
    layout->addWidget(m_list, 1);
    layout->addLayout(footer);
    setWidget(body);

    connect(m_size, &QSlider::valueChanged, this, &ThumbnailDock::setThumbnailWidth);
    auto *strip = static_cast<StripView *>(m_list);
    strip->zoomed = [this](int steps) { setThumbnailWidth(thumbnailWidth() + steps * ZoomStep); };
    strip->scrolledByHand = [this] { noteUserSteering(); };

    // The mirror image of m_following. That guard stops the strip from steering
    // the document while the document is steering the strip; these stop the
    // document from steering the strip while the *user* is, which is the same
    // fight seen from the other side and just as visible: a scrollbar that
    // slides back under the thumb is a scrollbar that appears not to work. Only
    // the ways a person can move it are listened for: a trough click or an
    // arrow button (actionTriggered), a drag of the handle (sliderMoved), and
    // the wheel, which the strip reports for itself. Both bars, because which
    // of the two carries the travel changes with where the panel is put.
    for (QScrollBar *bar : { m_list->verticalScrollBar(), m_list->horizontalScrollBar() }) {
        connect(bar, &QAbstractSlider::actionTriggered, this, [this] { noteUserSteering(); });
        connect(bar, &QAbstractSlider::sliderMoved, this, [this] { noteUserSteering(); });
    }

    connect(m_list, &QAbstractItemView::clicked, this, [this](const QModelIndex &index) {
        // Also when the page clicked is the one already marked. Asking to go
        // where you already are costs nothing, and refusing would make the
        // panel feel dead exactly when a reader taps it to get back on track
        // after scrolling somewhere else.
        if (index.isValid()) {
            // The document is about to jump, and its answer must not slide the
            // strip out from under the pointer that just picked a page off it.
            noteUserSteering();
            markRow(index.row());
            Q_EMIT pageRequested(index.row());
        }
    });

    // Where the window puts the panel, and where the user drags it afterwards.
    // A dock leaving for a window of its own reports no area at all, so the two
    // are listened for together and the answer is worked out in one place.
    connect(this, &QDockWidget::dockLocationChanged, this, [this](Qt::DockWidgetArea area) {
        m_area = area;
        updateOrientation();
    });
    connect(this, &QDockWidget::topLevelChanged, this, [this](bool) { updateOrientation(); });

    // The remembered size is put on before anything is shown, so the strip never
    // appears at one width and jumps to another.
    const KConfigGroup group(KSharedConfig::openConfig(), QStringLiteral("ThumbnailDock"));
    applyWidth(group.readEntry("ThumbnailWidth", DefaultWidth));
    applyOrientation();
}

ThumbnailDock::~ThumbnailDock()
{
    // Kept in the panel's own group rather than handed to the window to save.
    // The dock is the only thing that knows this number, and putting it here
    // means adding the panel to a window costs that window nothing.
    KConfigGroup group(KSharedConfig::openConfig(), QStringLiteral("ThumbnailDock"));
    group.writeEntry("ThumbnailWidth", thumbnailWidth());
    group.sync();
}

// ── Which way it runs ─────────────────────────────────────────────────────

Qt::Orientation ThumbnailDock::orientationForPlace() const
{
    if (!isFloating()) {
        switch (m_area) {
        case Qt::TopDockWidgetArea:
        case Qt::BottomDockWidgetArea:
            return Qt::Horizontal;
        case Qt::LeftDockWidgetArea:
        case Qt::RightDockWidgetArea:
            return Qt::Vertical;
        default:
            break;
        }
    }

    // Floating, or not yet placed. A floating dock belongs to no dock area
    // (dockWidgetArea() answers NoDockWidgetArea for one), so there is nothing
    // to ask about where it is, and the only thing left that says how it will
    // be read is the shape the user dragged it into. Wide and shallow is read
    // across; tall and narrow is read down. See ShapeMargin for why nearly
    // square keeps whatever it had.
    if (width() > height() * ShapeMargin) {
        return Qt::Horizontal;
    }
    if (height() > width() * ShapeMargin) {
        return Qt::Vertical;
    }
    return m_orientation;
}

void ThumbnailDock::updateOrientation()
{
    const Qt::Orientation wanted = orientationForPlace();
    if (wanted == m_orientation) {
        return;
    }
    m_orientation = wanted;
    applyOrientation();
}

void ThumbnailDock::applyOrientation()
{
    const bool down = m_orientation == Qt::Vertical;

    // Cells that run across and wrap onto the next line make a panel that
    // scrolls downwards; cells that run down and wrap into the next column make
    // one that scrolls sideways. That single choice is what turns the strip.
    m_list->setFlow(down ? QListView::LeftToRight : QListView::TopToBottom);

    // The cross-axis bar is switched off rather than merely left unused. The
    // strip wraps, so it never has anything to show that way, and a live bar
    // there would let one flick of a trackpad slide the pages out of sight in a
    // direction they cannot be brought back from.
    m_list->setVerticalScrollBarPolicy(down ? Qt::ScrollBarAsNeeded : Qt::ScrollBarAlwaysOff);
    m_list->setHorizontalScrollBarPolicy(down ? Qt::ScrollBarAlwaysOff : Qt::ScrollBarAsNeeded);

    static_cast<StripView *>(m_list)->sideways = !down;
    static_cast<MarkingDelegate *>(m_delegate)->setOrientation(m_orientation);

    updateStripFloor();
    m_list->doItemsLayout();
    m_list->viewport()->update();

    // Queued, because the panel has only just been told where it is: the list
    // still has the shape and the scrollbar range it had in the old orientation,
    // and where the strip ought to sit cannot be worked out until the window has
    // laid it out again.
    QTimer::singleShot(0, this, [this] {
        trackPosition();
        keepMarkVisible();
    });
}

void ThumbnailDock::resizeEvent(QResizeEvent *event)
{
    QDockWidget::resizeEvent(event);

    // Only while floating. A docked panel is told which way it runs by the area
    // it sits in, and reading its shape as well would turn a strip down the
    // side of the window sideways the moment the user dragged it wider than it
    // is tall, which is a shape a short window makes on its own.
    if (isFloating()) {
        updateOrientation();
    }
}

// ── Being put away by a mode ──────────────────────────────────────────────

void ThumbnailDock::setTemporarilyHidden(bool hidden)
{
    if (hidden == m_temporarilyHidden) {
        return;
    }
    m_temporarilyHidden = hidden;

    m_changingVisibility = true;
    if (hidden) {
        // isHidden() rather than isVisible(): a panel in a window that has not
        // been shown yet is not visible either, and reading that as "the user
        // closed it" would take the strip away for good.
        m_wanted = !isHidden();
        hide();
    } else if (m_wanted) {
        show();
    }
    m_changingVisibility = false;
}

void ThumbnailDock::showEvent(QShowEvent *event)
{
    QDockWidget::showEvent(event);

    // Somebody other than the mode has put the panel back: the user, through
    // the menu entry or the key for it. A wish outranks a preference, so the
    // mode stops owning the panel's visibility until it is next entered;
    // without this, leaving the mode would hide again what the user had just
    // asked for.
    if (m_temporarilyHidden && !m_changingVisibility) {
        m_temporarilyHidden = false;
        m_wanted = true;
    }
}

// ── The pages it shows ────────────────────────────────────────────────────

void ThumbnailDock::setModel(QAbstractItemModel *model)
{
    if (QAbstractItemModel *old = m_list->model()) {
        old->disconnect(this);
    }

    // Before the view is given the model, and the order is the whole point.
    // Handing a model to a view wires the view and its selection model to that
    // same signal, and both of them answer a removal by moving the current item
    // off the doomed row and announcing it, so by the time a handler connected
    // afterwards ran, the row whose page the mark belongs to would already be
    // forgotten. Slots are called in the order they were connected, so this one
    // gets there first, reads the page while it still exists, and holds the
    // door shut on the announcement that follows.
    if (model) {
        connect(model, &QAbstractItemModel::rowsAboutToBeRemoved, this,
                [this](const QModelIndex &, int first, int last) { onRowsAboutToBeRemoved(first, last); });
    }

    m_list->setModel(model);

    if (model) {
        // These go the other way about, after the view, because they act on
        // where the cells have ended up and the view has not moved them yet.
        //
        // Each can bring in a page of a shape the cells have no room for, and
        // each moves the mark: the mark is a row number, and a row number means
        // a different page the moment anything is inserted above it or taken out
        // from above it. Without the arguments the mark would simply stay where
        // it was and start pointing at a neighbour.
        connect(model, &QAbstractItemModel::modelReset, this, &ThumbnailDock::onModelReset);
        connect(model, &QAbstractItemModel::rowsInserted, this,
                [this](const QModelIndex &, int first, int last) { onRowsInserted(first, last); });
        connect(model, &QAbstractItemModel::rowsRemoved, this,
                [this](const QModelIndex &, int first, int last) { onRowsRemoved(first, last); });
    }

    // The view builds a fresh selection model for every model it is given, so
    // this has to be wired here rather than once in the constructor.
    if (QItemSelectionModel *selection = m_list->selectionModel()) {
        connect(selection, &QItemSelectionModel::currentChanged, this, [this](const QModelIndex &current) {
            // Arrow keys and Home and End land here rather than in clicked(),
            // and someone walking down the strip with the keyboard means to be
            // turning pages. m_following is what keeps the window's own answer
            // from coming straight back at it. Deliberately not treated as the
            // user taking the strip over, even though a key press is as much a
            // person as a wheel notch is. The view hands the current item out
            // for reasons of its own (taking focus for the first time is one)
            // and the strip going deaf for a second because a panel got focus
            // would be a bug nobody could see the cause of. What follows a key
            // press is a page request, and the answer to that comes back
            // through the ordinary channels. The focus test is what tells a
            // person from the view. A list gives itself a current row the first
            // time it is shown or its model is reset, with nobody having
            // touched it, and that arrived here as a request to turn to page
            // one, which is a document jumping to its start for no reason the
            // reader can see. Arrow keys, Home and End all require the focus
            // this asks for, so nothing a person does is lost by it.
            if (!m_following && current.isValid() && m_list->hasFocus()) {
                markRow(current.row());
                Q_EMIT pageRequested(current.row());
            }
        });
    }

    onModelReset();
}

void ThumbnailDock::onRowsChanged()
{
    QAbstractItemModel *model = m_list->model();
    const int rows = model ? model->rowCount() : 0;

    // A row that is no longer there must not go on being marked: the delegate
    // compares row numbers, so after pages are deleted the mark would simply
    // reappear on whichever page slid into that position.
    if (m_currentRow >= rows) {
        markRow(rows > 0 ? rows - 1 : -1);
    }

    refreshCellRatio();
}

void ThumbnailDock::onModelReset()
{
    // A different document, or the same one rebuilt. Nothing about where the
    // reader stood in the old one means anything in the new one, and a strip
    // left parked halfway down a file it no longer shows is worse than useless.
    markRow(-1);
    m_orphanSource = -1;
    m_orphanPage = -1;
    m_position = 0.0;
    m_steered.invalidate();
    // Both, rather than the one that carries the travel: the panel may be moved
    // while a document is open, and a stale value on the other bar would come
    // back the moment it did.
    m_list->verticalScrollBar()->setValue(0);
    m_list->horizontalScrollBar()->setValue(0);

    onRowsChanged();
}

void ThumbnailDock::onRowsInserted(int first, int last)
{
    const int count = last - first + 1;

    if (m_orphanSource >= 0) {
        // The second half of a page move. The mark belongs to a page, not to a
        // position, so it goes looking for its page among the ones that just
        // arrived, which is a handful of rows, not a walk of the document.
        QAbstractItemModel *model = m_list->model();
        for (int row = first; model && row <= last; ++row) {
            const QModelIndex index = model->index(row, 0);
            if (index.data(PageModel::SourceIdRole).toInt() == m_orphanSource
                && index.data(PageModel::SourcePageRole).toInt() == m_orphanPage) {
                m_orphanSource = -1;
                m_orphanPage = -1;
                markRow(row);
                break;
            }
        }
    } else if (m_currentRow >= first) {
        markRow(m_currentRow + count);
    }

    onRowsChanged();
    keepMarkVisible();
}

void ThumbnailDock::onRowsAboutToBeRemoved(int first, int last)
{
    // Held until onRowsRemoved(). In between, the view and its selection model
    // shunt the current item off the row that is going and announce it, and
    // that announcement is indistinguishable from an arrow key, so without this
    // the strip would answer every page deletion by sending the document off to
    // a page nobody asked for, in the middle of an edit.
    m_following = true;

    QAbstractItemModel *model = m_list->model();
    if (!model || m_currentRow < first || m_currentRow > last) {
        return;
    }

    // Read now, because in a moment the row will not be there to read. Which
    // file the page came from and which page of it: the one thing about a page
    // that survives being moved, which is the case this is here for.
    const QModelIndex index = model->index(m_currentRow, 0);
    m_orphanSource = index.data(PageModel::SourceIdRole).toInt();
    m_orphanPage = index.data(PageModel::SourcePageRole).toInt();

    // Only the insertion that completes a move counts, and a move puts the
    // pages back in the same turn of the event loop that took them out. Held
    // any longer, this would let some later, unrelated paste steal the mark.
    QTimer::singleShot(0, this, [this] {
        m_orphanSource = -1;
        m_orphanPage = -1;
    });
}

void ThumbnailDock::onRowsRemoved(int first, int last)
{
    m_following = false;

    const int count = last - first + 1;
    if (m_currentRow > last) {
        markRow(m_currentRow - count);
    } else if (m_currentRow >= first) {
        // The marked page has gone. The page that took its place is the nearest
        // thing left to where the reader was; if a move is under way, the
        // insertion that follows will claim the mark back.
        markRow(first);
    }

    onRowsChanged();
    keepMarkVisible();
}

void ThumbnailDock::refreshCellRatio()
{
    QAbstractItemModel *model = m_list->model();
    const int rows = model ? model->rowCount() : 0;

    qreal tallest = 0.0;
    for (int row = 0; row < std::min(rows, RatioSample); ++row) {
        const qreal aspect = model->index(row, 0).data(PageModel::AspectRatioRole).toReal();
        if (aspect > 0.0) {
            tallest = std::max(tallest, 1.0 / aspect);
        }
    }
    if (tallest <= 0.0) {
        tallest = 297.0 / 210.0; // an empty panel still has to look like paper
    }

    if (std::abs(tallest - m_delegate->cellRatio()) < 0.001) {
        return;
    }
    m_delegate->setCellRatio(tallest);
    m_list->doItemsLayout();
    updateStripFloor();
}

// ── Size ──────────────────────────────────────────────────────────────────

int ThumbnailDock::thumbnailWidth() const
{
    return m_delegate->thumbnailWidth();
}

void ThumbnailDock::setThumbnailWidth(int pixels)
{
    applyWidth(pixels);
    resizeDockToFit();
    Q_EMIT thumbnailWidthChanged(thumbnailWidth());
}

void ThumbnailDock::applyWidth(int pixels)
{
    pixels = std::clamp(pixels, MinimumThumbnailWidth, MaximumThumbnailWidth);
    // Whether anything moved is worked out before the slider and the floor are
    // put right, not instead of it: the delegate's own starting width need not
    // be the one remembered from last time, and an early return here would leave
    // the slider sitting at its minimum with thumbnails that are not.
    const bool changed = pixels != m_delegate->thumbnailWidth();
    m_delegate->setThumbnailWidth(pixels);

    if (m_size->value() != pixels) {
        const QSignalBlocker blocker(m_size);
        m_size->setValue(pixels);
    }
    updateStripFloor();

    if (changed) {
        // Uniform item sizes mean the measured cell is cached, so the view keeps
        // the old one until it is told in so many words to measure again.
        m_list->doItemsLayout();
        m_list->viewport()->update();
        // Every cell just changed height, so the scroll position the strip is
        // sitting at now points at a different page than it did a moment ago.
        // Resizing thumbnails is not meant to be a way of losing your place.
        trackPosition();
    }
}

void ThumbnailDock::updateStripFloor()
{
    // A panel squeezed thinner across than one thumbnail would crop the paper
    // down its edge, so the floor moves with the size instead, and it is a
    // floor on whichever measurement runs across the strip, which changes with
    // where the panel is put. The other measurement is deliberately left free:
    // that is the one wrapping fills, and a floor on it would stop the window
    // being made small at all.
    const int bar = style()->pixelMetric(QStyle::PM_ScrollBarExtent, nullptr, this);
    const QSize cell = m_delegate->gridSize();
    if (m_orientation == Qt::Vertical) {
        m_list->setMinimumHeight(0);
        m_list->setMinimumWidth(cell.width() + bar);
    } else {
        m_list->setMinimumWidth(0);
        m_list->setMinimumHeight(cell.height() + bar);
    }
}

void ThumbnailDock::resizeDockToFit()
{
    // Widening the thumbnails and then leaving the user to drag the dock edge
    // to see the result would make the slider look broken. Only for a docked
    // panel: a floating one is a window the user has already sized by hand.
    auto *window = qobject_cast<QMainWindow *>(parentWidget());
    if (!window || isFloating() || !isVisible()) {
        return;
    }

    const bool down = m_orientation == Qt::Vertical;
    // The list's floor plus whatever the dock's own frame and title take, which
    // is the difference between the two as they stand.
    const int chrome = down ? std::max(0, width() - m_list->width()) : std::max(0, height() - m_list->height());
    const int needed = (down ? m_list->minimumWidth() : m_list->minimumHeight()) + chrome;
    const int have = down ? width() : height();

    // Only ever grown, never shrunk. Room the user made for the panel is room
    // they meant it to have, and since the strip wraps, making the thumbnails
    // smaller in that room is a way of asking for more pages at once: pulling
    // the panel back to one cell across would answer by taking the pages away.
    if (needed > have) {
        window->resizeDocks({ this }, { needed }, down ? Qt::Horizontal : Qt::Vertical);
    }
}

// ── Where the reader is ───────────────────────────────────────────────────

void ThumbnailDock::markRow(int row)
{
    if (m_currentRow == row) {
        return;
    }
    m_currentRow = row;
    static_cast<MarkingDelegate *>(m_delegate)->setCurrentRow(row);

    QAbstractItemModel *model = m_list->model();
    QItemSelectionModel *selection = m_list->selectionModel();
    if (model && selection && row >= 0 && row < model->rowCount()) {
        // The current item goes with the mark so that arrow keys carry on from
        // the page being read rather than from whatever the strip was last
        // clicked on, which after any scrolling at all are two different pages.
        //
        // Auto-scroll is off for the duration because the view's answer to a
        // new current item is scrollTo(EnsureVisible), a shove by the smallest
        // amount that makes the row visible, which arrives a whole page late
        // and a whole screenful at a time. Where the strip sits is
        // trackPosition()'s business. m_following covers the other direction:
        // without it the selection change below would come back out as
        // pageRequested() and scroll the document view that asked for this in
        // the first place.
        const bool autoScroll = m_list->hasAutoScroll();
        m_list->setAutoScroll(false);
        m_following = true;
        selection->setCurrentIndex(model->index(row, 0), QItemSelectionModel::ClearAndSelect);
        m_following = false;
        m_list->setAutoScroll(autoScroll);
    }

    // The whole viewport rather than the two cells: only what is on screen is
    // painted anyway, and the cell the mark left may have scrolled away since.
    m_list->viewport()->update();
}

void ThumbnailDock::followPage(int row)
{
    QAbstractItemModel *model = m_list->model();
    if (!model || row < 0 || row >= model->rowCount()) {
        return;
    }

    markRow(row);

    // Travelling is followPosition()'s job and this deliberately does none of
    // it. But a jump the reader asked for (Ctrl+G, an outline entry, a search
    // hit) arrives as one enormous step, and the two signals need not agree
    // about it to the pixel; a mark nobody can see is no mark at all, so this
    // is the floor under the agreement rather than a second way of scrolling.
    keepMarkVisible();
}

void ThumbnailDock::followPosition(double fraction)
{
    m_position = std::clamp(fraction, 0.0, 1.0);
    trackPosition();
}

QScrollBar *ThumbnailDock::travelBar() const
{
    return m_orientation == Qt::Vertical ? m_list->verticalScrollBar() : m_list->horizontalScrollBar();
}

int ThumbnailDock::stripExtent() const
{
    const QSize size = m_list->viewport()->size();
    return m_orientation == Qt::Vertical ? size.height() : size.width();
}

void ThumbnailDock::trackPosition()
{
    if (!m_list->model() || userIsSteering()) {
        return;
    }

    const QScrollBar *bar = travelBar();
    const int travel = bar->maximum() - bar->minimum();
    if (travel <= 0) {
        // Every page already fits on screen: a short document, or a strip made
        // large enough to hold it all. There is no travel to distribute, so
        // there is nothing to do; the mark alone says where the reader is.
        return;
    }

    // Both scrollbars answer the same question (how much of the document lies
    // behind the reader, as a share of everything that could), so the strip's
    // ideal position is simply the document's, read off its own range. That
    // needs no page geometry, and it comes out exactly right at both ends: at
    // the start of the document the strip is at the start of itself, at the end
    // it is at the end. Nought means the beginning on either bar and in either
    // direction, which is Qt's doing rather than luck: a list laid out
    // right-to-left still counts its travel from the end it starts at.
    const int extent = stripExtent();
    const double ideal = bar->minimum() + m_position * travel;
    const double here = bar->value();

    // ── A band, not a line ────────────────────────────────────────────────
    //
    // The obvious thing is to put the strip exactly where `ideal` says on every
    // report, which pins the reading point to one fixed line down the strip.
    // Try it and the strip creeps continuously under the eye: every pixel of
    // document scroll slides every thumbnail, so the panel is never still long
    // enough to be looked at, and picking a page out of it means chasing it.
    //
    // So the strip is allowed to lag. The reading point may wander the middle
    // half of the strip and nothing moves at all; only when it reaches the edge
    // of that band does the strip travel, and then it travels *with* the
    // document, pixel for pixel, because `ideal` is continuous. That is the
    // whole difference from the old behaviour: it moves smoothly when it moves,
    // rather than sitting still for a page and then jumping a screenful.
    //
    // Past a whole screenful of disagreement the reader did not scroll there,
    // they jumped: an outline entry, a search hit, a page typed in. Creeping to
    // the near edge of the band would land the page they asked for against the
    // rim of the strip, so that one case goes straight to `ideal`, which puts
    // the reading point in the middle. It is the recovery a person makes when
    // they have lost their place, and it is rare enough not to be a creep.
    double target = ideal;
    if (std::abs(ideal - here) <= extent) {
        const double slack = std::max(0.0, (0.5 - BandMargin) * extent);
        target = std::clamp(here, ideal - slack, ideal + slack);
    }

    scrollStripTo(int(std::lround(target)));
}

void ThumbnailDock::keepMarkVisible()
{
    if (!m_list->model() || userIsSteering()) {
        return;
    }
    scrollStripTo(travelBar()->value());
}

void ThumbnailDock::scrollStripTo(int offset)
{
    QScrollBar *bar = travelBar();
    offset = std::clamp(clampToMark(offset), bar->minimum(), bar->maximum());
    if (offset != bar->value()) {
        bar->setValue(offset);
    }
}

int ThumbnailDock::clampToMark(int offset) const
{
    QAbstractItemModel *model = m_list->model();
    if (!model || m_currentRow < 0 || m_currentRow >= model->rowCount()) {
        return offset;
    }

    // Mapping one scrollbar onto the other assumes the strip and the document
    // run through their pages at the same rate, and they do not: the strip
    // gives every page a cell of one size, while the document gives each one
    // however tall it really is at this zoom, two to a row in facing layout. On
    // an ordinary file the two agree closely enough that this changes nothing;
    // on a file with one huge sheet among small ones it is what keeps the mark
    // and the strip from telling the reader two different stories.
    const QRect cell = m_list->visualRect(model->index(m_currentRow, 0));
    if (cell.isEmpty()) {
        return offset; // nothing laid out yet, so nothing to be off the edge of
    }

    const bool down = m_orientation == Qt::Vertical;
    const int extent = stripExtent();
    const int span = down ? cell.height() : cell.width();
    const int here = travelBar()->value();

    // How far the marked cell stands from the beginning of the whole strip,
    // which is a property of the page rather than of where the strip has been
    // scrolled to, and so is what the arithmetic below can be done in. A strip
    // laid out across the window in a right-to-left interface begins at its
    // right-hand end and fills leftwards, so for that one case the distance is
    // measured from the other side of the viewport.
    int lead = down ? cell.top() + here : cell.left() + here;
    if (!down && m_list->isRightToLeft()) {
        lead = extent - (cell.left() + span) + here;
    }

    // Kept small on purpose. This is a backstop, not the thing that decides
    // where the strip sits: a generous margin here would be narrower than the
    // band and would take the steering away from it, so that the strip shifted
    // on every page turn after all, the very twitch the band is there to stop.
    // All that is wanted is that the marked page is not flush against the rim.
    const int margin = std::min(span / 4, extent / 16);
    const int lowest = lead + span - extent + margin;
    const int highest = lead - margin;
    if (lowest > highest) {
        // The cell is larger than the strip has room for with margins, which is
        // one enormous thumbnail in a shallow dock. Centred is the best on offer.
        return lead + span / 2 - extent / 2;
    }
    return std::clamp(offset, lowest, highest);
}

bool ThumbnailDock::userIsSteering() const
{
    // A held handle counts even while it is not moving, or letting go to think
    // for a second would let the document snatch the strip back mid-drag.
    return travelBar()->isSliderDown() || (m_steered.isValid() && m_steered.elapsed() < HandBackDelay);
}

void ThumbnailDock::noteUserSteering()
{
    m_steered.start();
}

} // namespace ps
