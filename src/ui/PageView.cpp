/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "PageView.h"

#include "PageRows.h"
#include "core/Document.h"
#include "core/ImageEdit.h"
#include "core/Source.h"

#include <KLocalizedString>

#include <QApplication>
#include <QClipboard>
#include <QCoreApplication>
#include <QCursor>
#include <QDir>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QReadWriteLock>
#include <QRunnable>
#include <QScrollBar>
#include <QStyle>
#include <QTemporaryFile>
#include <QThread>
#include <QTimerEvent>
#include <QTransform>
#include <QWheelEvent>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>

using namespace Qt::Literals::StringLiterals;

namespace ps {

namespace {

/** Air between pages, and around the whole run of them. */
constexpr int Gap = 14;

constexpr double MinimumZoom = 0.1;
constexpr double MaximumZoom = 8.0;

/**
 * How many bytes of rendered page are held at once.
 *
 * A count of pages is not a bound at all, which is what the twelve this used to
 * keep were: one A4 sheet fitted to the width of a 4K screen is about eighty
 * megabytes of pixels, so twelve of them is a gigabyte, while twelve pages at
 * a tenth of that zoom is nothing worth caching. Two hundred and fifty-six
 * megabytes holds the pages on screen and the handful queued ahead of the
 * reader at any zoom a person actually reads at, and at zooms beyond that the
 * rule below never drops a page that is about to be drawn: running slightly
 * over the budget is better than a blank sheet where the document should be.
 */
constexpr qint64 RenderBudget = 256LL * 1024 * 1024;

/**
 * How far ahead of the reader pages are fetched, and how far behind kept. Both
 * are ceilings, because the budget above can and does cut the reach shorter.
 *
 * Deeper than it once was, because the renders now run several at a time: with
 * one page drawing at a time a long queue only delayed the page being waited
 * for, and with eight the queue is what keeps the cores fed.
 */
constexpr int PagesAhead = 6;
constexpr int PagesBehind = 2;

/**
 * How wide the kept stand-in render of a page is, in device pixels.
 *
 * Small enough that a page costs about a fiftieth of a millisecond per pixel
 * row and a couple of hundred kilobytes (a whole book's worth fits in the
 * budget below) and large enough that stretched over a screen it reads as the
 * page it is rather than as a coloured smear.
 */
constexpr int ProxyWidth = 200;

/** What all the kept stand-ins together may cost. About four hundred A4 pages. */
constexpr qint64 ProxyBudget = 48LL * 1024 * 1024;

/**
 * The smallest page, in pixels, that is ever rendered in bands rather than
 * whole. See bandOf(), which also weighs it against the size of the window.
 *
 * Six megapixels is a little more than a fitted A4 sheet on an ordinary screen,
 * so reading zooms still render the whole page. That is worth having, because
 * a whole page never has to be rendered again when the reader scrolls.
 */
constexpr qint64 WholePageLimit = 6LL * 1000 * 1000;

/**
 * Bands are snapped out to a multiple of this many pixels.
 *
 * Without it every scrolled pixel would ask for a band one pixel further down
 * and nothing would ever be reused; with it a band covers a good deal more than
 * the window and is asked for again only when the reader has left it.
 */
constexpr int BandGrid = 256;

/** A second zoom within this of the last one says a run of notches is under way. */
constexpr int ZoomRun = 250;

/** How long after the last notch the render is started. */
constexpr int ZoomSettle = 120;

/** A frame at sixty hertz. */
constexpr int GlideInterval = 16;

/**
 * How much of the distance still to go the view takes each frame.
 *
 * A little over a quarter settles a wheel notch in about a tenth of a second,
 * which reads as motion rather than as a jump and is still short enough that
 * the page has arrived by the time the eye looks for it.
 */
constexpr double GlideShare = 0.28;

/** A drag shorter than this is a click that wobbled, not a box. */
constexpr int BoxThreshold = 4;

QImage rotated(const QImage &image, int degrees)
{
    if (degrees == 0 || image.isNull()) {
        return image;
    }
    // Fast rather than smooth: at a quarter turn every source pixel lands
    // exactly on a destination pixel, so the two give the same answer and only
    // one of them walks the image through a filter to arrive at it.
    return image.transformed(QTransform().rotate(degrees), Qt::FastTransformation);
}

/**
 * Draws @p image into @p box, touching only what falls inside @p clip.
 *
 * @p image holds @p tile of a page @p width device pixels across, or the whole
 * page when @p tile is empty. The clipping is not an optimisation to be taken
 * or left: at 800% a page is a hundred megapixels, and asking QPainter to
 * stretch a stand-in over all of it takes the better part of a second even
 * though the window can show a fiftieth of the result.
 */
void drawSheet(QPainter &painter, const QRect &box, const QRect &clip, const QImage &image, int width,
               const QRect &tile)
{
    if (image.isNull() || box.isEmpty()) {
        return;
    }

    QRectF target(box);
    if (!tile.isEmpty() && width > 0) {
        const double scale = double(box.width()) / double(width);
        target = QRectF(box.left() + tile.x() * scale, box.top() + tile.y() * scale, tile.width() * scale,
                        tile.height() * scale);
    }

    const QRectF visible = target.intersected(QRectF(clip));
    if (visible.isEmpty() || target.width() <= 0.0 || target.height() <= 0.0) {
        return;
    }

    const QRectF source((visible.left() - target.left()) / target.width() * image.width(),
                        (visible.top() - target.top()) / target.height() * image.height(),
                        visible.width() / target.width() * image.width(),
                        visible.height() / target.height() * image.height());
    painter.drawImage(visible, image, source);
}

quint64 pictureKey(int sourceId, int page)
{
    return (static_cast<quint64>(static_cast<quint32>(sourceId)) << 32) | static_cast<quint32>(page);
}

/**
 * Where inside a word's box character @p index falls.
 *
 * The renderer reports one box per word and no more, so the characters are
 * spread evenly across it. That is exact for a monospaced face and off by a
 * fraction of a letter for everything else, which is the difference between a
 * selection that ends where the pointer is and one that swallows whole words.
 * It is the only place, together with characterAt() below, that would have to
 * change if RenderBackend ever reported the boxes it already knows.
 */
double xOfCharacter(const RenderBackend::Word &word, int index)
{
    const int length = int(word.text.size());
    if (length <= 0) {
        return word.rect.left();
    }
    const double share = double(std::clamp(index, 0, length)) / length;
    return word.rect.left() + word.rect.width() * share;
}

/** Which character boundary of @p word the position @p x is nearest. */
int characterAt(const RenderBackend::Word &word, double x)
{
    const int length = int(word.text.size());
    if (length <= 0 || word.rect.width() <= 0.0) {
        return 0;
    }
    if (x <= word.rect.left()) {
        return 0;
    }
    if (x >= word.rect.right()) {
        return length;
    }
    // Rounded rather than truncated: a letter joins the selection once the
    // pointer has passed its middle, which is where the eye puts the caret.
    const double share = (x - word.rect.left()) / word.rect.width() * length;
    return std::clamp(int(std::lround(share)), 0, length);
}

/** True when two boxes sit on the same line of type. */
bool sameLine(const QRectF &a, const QRectF &b)
{
    const double centre = b.center().y();
    return centre >= a.top() && centre <= a.bottom();
}

} // namespace

/**
 * What a worker thread is allowed to touch.
 *
 * ## Why the lock is shared and what that guarantees
 *
 * A render holds @ref lock for **reading** for exactly as long as it is inside
 * the rasteriser, and stopRendering() takes it for **writing** to set
 * @ref backend to null. Two facts follow, and together they are the whole
 * safety argument:
 *
 * - `lockForWrite()` does not return until every reader has let go, so by the
 *   time the backend pointer is cleared no thread is inside the rasteriser.
 * - The pointer is cleared while the write lock is still held, so any render
 *   that takes the read lock afterwards finds null and calls nothing.
 *
 * There is therefore no instant at which a render is inside a rasteriser its
 * owner has begun to destroy, which is the crash this program already had once,
 * on roughly one exit in three under load. The old arrangement (one plain
 * mutex) gave the same guarantee and charged the whole program for it: every
 * page of every document rendered one at a time. A shared lock keeps the
 * guarantee and lets the renders run side by side, which is the entire point.
 *
 * The gate is held by shared_ptr by every task, so the lock itself outlives any
 * worker even if the view is already gone.
 *
 * The window and the generation are atomic instead of living under the lock,
 * and that is the point of splitting them: the GUI thread writes them on every
 * scroll, and a scroll that had to wait for a page render to finish would be
 * exactly the stutter all of this exists to remove.
 */
struct PageView::RenderGate {
    QReadWriteLock lock;
    RenderBackend *backend = nullptr;
    std::atomic<quint64> generation { 1 };

    /**
     * Bumped whenever the pages change size.
     *
     * A queued render for a size the reader has already zoomed past is work
     * nobody will ever see, and a run of wheel notches queues one per notch.
     * Checked by the task as it starts rather than cancelled from the outside,
     * so that the cheap stand-in renders, which no zoom makes wrong, are not
     * thrown out along with them.
     */
    std::atomic<quint64> sizing { 1 };

    std::atomic<int> wantFirst { 0 };
    std::atomic<int> wantLast { -1 };
};

/**
 * One page rendered off the GUI thread, with its words while the file is open.
 *
 * The task re-reads the gate as it starts rather than trusting the order it was
 * queued in: a reader who has scrolled past a page while it waited its turn
 * wants the page they are looking at now, and dropping the stale work here
 * costs nothing where cancelling a queue would cost a good deal.
 */
class PageRenderTask : public QRunnable
{
public:
    PageRenderTask(PageView *view, std::shared_ptr<PageView::RenderGate> gate, quint64 generation, quint64 sizing,
                   int row, const PageRef &ref, const RenderBackend::Request &request, bool wantImage, bool wantWords,
                   bool proxy)
        : m_view(view)
        , m_gate(std::move(gate))
        , m_generation(generation)
        , m_sizing(sizing)
        , m_row(row)
        , m_ref(ref)
        , m_request(request)
        , m_wantImage(wantImage)
        , m_wantWords(wantWords)
        , m_proxy(proxy)
    {
        setAutoDelete(true);
    }

    void run() override
    {
        PageView::Rendered result;
        result.row = m_row;
        result.generation = m_generation;
        result.width = m_request.widthPx;
        result.tile = m_request.tile;
        result.proxy = m_proxy;

        // A proxy is wanted whatever the reader has done since: it is kept for
        // the whole session and is the only thing standing between a page and
        // white paper. The accurate render is not: a reader who has scrolled
        // past a page while it waited its turn wants the page they are looking
        // at now, and dropping the stale work here costs nothing where
        // cancelling a queue would cost a good deal.
        const bool current = m_gate->generation.load() == m_generation
            && (m_proxy
                || (m_gate->sizing.load() == m_sizing && m_row >= m_gate->wantFirst.load()
                    && m_row <= m_gate->wantLast.load()));
        if (current) {
            // Shared, so every other render on the machine runs beside this
            // one; see RenderGate for why that is still safe on the way out.
            QReadLocker locker(&m_gate->lock);
            if (m_gate->backend && m_gate->generation.load() == m_generation) {
                if (m_wantWords) {
                    result.words = m_gate->backend->words(m_ref.sourceId, m_ref.sourcePage);
                    result.gotWords = true;
                }
                if (m_wantImage) {
                    result.tried = true;
                    result.image
                        = rotated(m_gate->backend->render(m_ref.sourceId, m_ref.sourcePage, m_request), m_ref.rotation);
                }
            }
        }

        // Answered even when there was nothing to say: the view is holding the
        // row open for this reply, and a page that never answers is a page that
        // is never asked for again.
        PageView *view = m_view;
        QMetaObject::invokeMethod(view, [view, result] { view->applyRender(result); }, Qt::QueuedConnection);
    }

private:
    PageView *m_view;
    std::shared_ptr<PageView::RenderGate> m_gate;
    quint64 m_generation;
    quint64 m_sizing;
    int m_row;
    PageRef m_ref;
    RenderBackend::Request m_request;
    bool m_wantImage;
    bool m_wantWords;
    bool m_proxy;
};

/**
 * Where every picture in one file sits, read off the GUI thread.
 *
 * Boxes only: what is where, so that the pointer can promise a picture before
 * anybody asks for its pixels. Nothing here goes near the render backend, so it
 * needs no gate: it opens the file itself and hands back plain values.
 */
class PictureScanTask : public QRunnable
{
public:
    PictureScanTask(PageView *view, int sourceId, const QString &path)
        : m_view(view)
        , m_sourceId(sourceId)
        , m_path(path)
    {
        setAutoDelete(true);
    }

    void run() override
    {
        QString error;
        const QVector<ImageEdit::ImageUse> uses = ImageEdit::read(m_path, &error);

        QVector<PageView::Picture> pictures;
        pictures.reserve(uses.size());
        for (const ImageEdit::ImageUse &use : uses) {
            pictures.append({ use.page, use.resourceName, use.placement, use.decodable });
        }

        PageView *view = m_view;
        const int sourceId = m_sourceId;
        QMetaObject::invokeMethod(
            view, [view, sourceId, pictures] { view->applyPictures(sourceId, pictures); }, Qt::QueuedConnection);
    }

private:
    PageView *m_view;
    int m_sourceId;
    QString m_path;
};

PageView::PageView(QWidget *parent)
    : QAbstractScrollArea(parent)
    , m_gate(std::make_shared<RenderGate>())
{
    setFrameShape(QFrame::NoFrame);
    viewport()->setBackgroundRole(QPalette::Dark);
    viewport()->setAutoFillBackground(true);
    viewport()->setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);

    // Pixels, not steps. The bars measure the run of pages in pixels already, so
    // a single step is a line of body text and an arrow key moves by one. The
    // forty-pixel step this used to have was a jump with nothing behind it.
    verticalScrollBar()->setSingleStep(24);
    horizontalScrollBar()->setSingleStep(24);

    // As wide as the machine goes, less one so that the thread painting the
    // window always has somewhere to run: renders are the one part of this
    // program that parallelises perfectly, and the backend now holds enough
    // handles on each file to let them. Reading where the pictures are gets a
    // thread of its own, because it takes seconds on a large document and a
    // page the reader is waiting for must never queue behind it.
    m_pool.setMaxThreadCount(std::clamp(QThread::idealThreadCount() - 1, 2, 8));
    m_scanPool.setMaxThreadCount(1);

    // A hand on the scroll bar overrules whatever the wheel last asked for, and
    // the bar is the one place the view is moved from without going through
    // this class at all.
    connect(verticalScrollBar(), &QAbstractSlider::actionTriggered, this, [this] { stopGlide(); });
    connect(verticalScrollBar(), &QAbstractSlider::sliderPressed, this, [this] { stopGlide(); });

    // Before the rasteriser goes. This view is a child of the window and the
    // window owns the backend, so by the time the view is destroyed the backend
    // is already gone; the application says it is leaving while everything is
    // still standing, which is the moment to stop drawing.
    if (QCoreApplication *application = QCoreApplication::instance()) {
        connect(application, &QCoreApplication::aboutToQuit, this, &PageView::stopRendering);
    }
}

PageView::~PageView()
{
    stopRendering();
}

// ── What it is showing ────────────────────────────────────────────────────

void PageView::setDocument(Document *document)
{
    if (m_document == document) {
        return;
    }
    if (m_document) {
        m_document->disconnect(this);
    }
    m_document = document;

    if (m_document) {
        // Any of these can change how many pages there are or what they look
        // like, and a stale render is worse than a slow one.
        const auto forget = [this] { refresh(); };
        connect(m_document, &Document::pagesInserted, this, forget);
        connect(m_document, &Document::pagesRemoved, this, forget);
        connect(m_document, &Document::pagesChanged, this, forget);
        connect(m_document, &Document::wasReset, this, forget);
    }
    refresh();
}

void PageView::setRenderBackend(RenderBackend *backend)
{
    // Whatever is being drawn is drawn into the old backend, so it is waited for
    // before the new one is handed to the workers.
    stopRendering();
    m_backend = backend;
    {
        QWriteLocker locker(&m_gate->lock);
        m_gate->backend = backend;
    }
    refresh();
}

void PageView::stopRendering()
{
    // Cleared first so that nothing queued starts after the wait begins, and
    // only then waited on: the other order lets a task slip in between.
    m_pool.clear();
    m_scanPool.clear();
    {
        // Exclusive, so this does not return until every render that is inside
        // the rasteriser has come out of it, and nulls the pointer before it
        // lets go, so none can go back in. After this line there is no way for
        // a worker to reach a backend the caller is about to destroy.
        QWriteLocker locker(&m_gate->lock);
        m_gate->backend = nullptr;
    }
    m_pool.waitForDone();
    m_scanPool.waitForDone();
    m_pending.clear();
    m_pendingProxy.clear();

    // Let go of here as well, not only in the gate: the words on a page are
    // still read straight from the backend when a selection needs them, and
    // after this call there is no backend left to read them from.
    m_backend = nullptr;
}

void PageView::addOverlay(Overlay *overlay)
{
    if (!overlay) {
        return;
    }
    // Moved to the back rather than left where it was. An overlay that put
    // itself in the list from its own constructor would otherwise fix the order
    // by construction order, and the window's later, deliberate sequence (a
    // click on a line of text must reach the layer that types into it before
    // the one that picks the block up) would be silently ignored.
    m_overlays.removeAll(overlay);
    m_overlays.append(overlay);
    viewport()->update();
    Q_EMIT overlaysChanged();
}

void PageView::removeOverlay(Overlay *overlay)
{
    if (m_grabbed == overlay) {
        m_grabbed = nullptr;
    }
    m_overlays.removeAll(overlay);
    viewport()->update();
    Q_EMIT overlaysChanged();
}

void PageView::refresh(int row)
{
    if (row < 0) {
        invalidateRenders();
        m_words.clear();
        m_pictures.clear();
        m_scanAsked.clear();
        m_scanDone.clear();
        clearSelection();
    } else {
        m_sheets.remove(row);
        m_proxies.remove(row);
        m_words.remove(row);
        m_barren.remove(row);
        // The rows now mean something else than the work in flight was asked
        // for, so that work is disowned rather than allowed to land on them.
        m_pending.clear();
        m_pendingProxy.clear();
        m_gate->generation.fetch_add(1);
    }
    relayout();
    // The pages this is a fit *of* may be a different shape now: another
    // document, or the same one with a page turned on its side.
    applyFit();
    viewport()->update();
}

void PageView::setOmittedFromRenders(RenderBackend::Request::Omit omit)
{
    if (m_omit == omit) {
        return;
    }
    m_omit = omit;

    // Every picture in hand (the proxies included, which nothing else throws
    // away) is a picture of a page drawn the other way, so it goes. The words
    // are left alone: what is on the page does not change because part of it was
    // not drawn, and re-reading them would cost a pass over the file for nothing.
    invalidateRenders();
    ensureRenders();
    viewport()->update();
}

bool PageView::isRendering() const
{
    return !m_pending.isEmpty() || !m_pendingProxy.isEmpty();
}

bool PageView::isSharp(int row) const
{
    return sheetSatisfies(row);
}

// ── Geometry ──────────────────────────────────────────────────────────────

QSizeF PageView::pageSizeOf(int row) const
{
    if (!m_document || row < 0 || row >= m_document->pageCount()) {
        return { 595.0, 842.0 }; // A4, so an empty view still looks like paper
    }
    const QSizeF size = m_document->pageSizePoints(row);
    return size.isEmpty() ? QSizeF(595.0, 842.0) : size;
}

void PageView::relayout()
{
    m_places.clear();
    if (!m_document) {
        m_contents = QSize(0, 0);
        verticalScrollBar()->setRange(0, 0);
        horizontalScrollBar()->setRange(0, 0);
        return;
    }

    const int count = m_document->pageCount();
    m_places.resize(count);

    const int perRow = m_layout == Layout::Facing ? 2 : 1;
    int y = Gap;
    int widest = 0;

    for (int row = 0; row < count; row += perRow) {
        // Every page in a row is bottom-aligned on one line and the row is as
        // tall as its tallest page, so a landscape page among portrait ones
        // does not shift its neighbour up.
        int rowHeight = 0;
        int rowWidth = 0;
        for (int i = 0; i < perRow && row + i < count; ++i) {
            const QSizeF size = pageSizeOf(row + i);
            const int w = std::max(1, int(std::lround(size.width() * m_zoom)));
            const int h = std::max(1, int(std::lround(size.height() * m_zoom)));
            rowHeight = std::max(rowHeight, h);
            rowWidth += w + (i > 0 ? Gap : 0);
        }
        widest = std::max(widest, rowWidth);

        int x = 0;
        for (int i = 0; i < perRow && row + i < count; ++i) {
            const QSizeF size = pageSizeOf(row + i);
            const int w = std::max(1, int(std::lround(size.width() * m_zoom)));
            const int h = std::max(1, int(std::lround(size.height() * m_zoom)));
            m_places[row + i].box = QRect(x, y + (rowHeight - h), w, h);
            x += w + Gap;
        }
        y += rowHeight + Gap;
    }

    // Centred when the pages are narrower than the window, which is what makes
    // a single page at reading size look like a sheet of paper rather than
    // like something that failed to fill the screen.
    const int available = viewport()->width();
    if (widest < available) {
        const int shift = (available - widest) / 2;
        for (Placement &place : m_places) {
            place.box.moveLeft(place.box.left() + shift);
        }
        widest = available;
    } else {
        for (Placement &place : m_places) {
            place.box.moveLeft(place.box.left() + Gap);
        }
        widest += 2 * Gap;
    }

    m_contents = QSize(widest, y);
    verticalScrollBar()->setRange(0, std::max(0, m_contents.height() - viewport()->height()));
    verticalScrollBar()->setPageStep(viewport()->height());
    horizontalScrollBar()->setRange(0, std::max(0, m_contents.width() - viewport()->width()));
    horizontalScrollBar()->setPageStep(viewport()->width());

    // Said out loud, because the pages under the reader may be different pages
    // than they were: after a deletion or a reorder the row the view calls
    // current is whatever number it was before, and the page number in the
    // window, the strip and anything else keyed to it would sit there wrong
    // until the reader happened to scroll. Safe to do here: the scroll bars
    // are already set, and neither of these calls lays out anything.
    updateCurrentPage();
    emitPosition();
}

QSizeF PageView::pageSizePoints(int row) const
{
    return pageSizeOf(row);
}

QRect PageView::pageRect(int row) const
{
    if (row < 0 || row >= m_places.size()) {
        return {};
    }
    return m_places.at(row).box.translated(-horizontalScrollBar()->value(), -verticalScrollBar()->value());
}

QPointF PageView::fromPoints(int row, const QPointF &points) const
{
    const QRect box = pageRect(row);
    const QSizeF size = pageSizeOf(row);
    if (box.isEmpty() || size.isEmpty()) {
        return {};
    }
    // Points run up the page and pixels run down it, which is the one
    // conversion in this file worth stating rather than inlining.
    return QPointF(box.left() + points.x() / size.width() * box.width(),
                   box.top() + (1.0 - points.y() / size.height()) * box.height());
}

QRectF PageView::fromPoints(int row, const QRectF &points) const
{
    const QPointF topLeft = fromPoints(row, QPointF(points.left(), points.top() + points.height()));
    const QPointF bottomRight = fromPoints(row, QPointF(points.right(), points.top()));
    return QRectF(topLeft, bottomRight).normalized();
}

QPointF PageView::toPoints(int row, const QPointF &pixels) const
{
    const QRect box = pageRect(row);
    const QSizeF size = pageSizeOf(row);
    if (box.isEmpty() || size.isEmpty()) {
        return {};
    }
    return QPointF((pixels.x() - box.left()) / box.width() * size.width(),
                   (1.0 - (pixels.y() - box.top()) / box.height()) * size.height());
}

int PageView::pageAt(const QPoint &viewportPoint) const
{
    for (int row = 0; row < m_places.size(); ++row) {
        if (pageRect(row).contains(viewportPoint)) {
            return row;
        }
    }
    return -1;
}

// ── Mode, layout, tool and zoom ───────────────────────────────────────────

void PageView::setMode(Mode mode)
{
    if (m_mode == mode) {
        return;
    }
    m_mode = mode;

    // A selection made for reading means nothing once the page is editable,
    // and leaving it highlighted would suggest the next command applies to it.
    clearSelection();
    m_grabbed = nullptr;
    viewport()->update();
    Q_EMIT modeChanged(m_mode);
}

void PageView::setLayout(Layout layout)
{
    if (m_layout == layout) {
        return;
    }
    m_layout = layout;
    relayout();
    goToPage(m_current);
    // A spread is twice as wide as a page, so every fit means a different
    // number here than it did a line ago.
    applyFit();
    viewport()->update();
    Q_EMIT layoutChanged(m_layout);
}

void PageView::setTool(Tool tool)
{
    if (m_tool == tool) {
        return;
    }
    m_tool = tool;
    m_selecting = false;
    m_draggingBox = false;
    updateCursor(viewport()->mapFromGlobal(QCursor::pos()));
    Q_EMIT toolChanged(m_tool);
}

void PageView::setZoom(double factor)
{
    // A size the reader set by hand, so whatever was being fitted to is over:
    // putting it back on the next resize would be the window arguing with them.
    setFit(Fit::Free);
    applyZoom(factor);
}

void PageView::applyZoom(double factor)
{
    factor = std::clamp(factor, MinimumZoom, MaximumZoom);
    if (std::abs(factor - m_zoom) < 0.0001) {
        return;
    }

    // How far down the page being read the top of the window stands. Held as a
    // fraction of that page because that is the one measure a scale does not
    // change, and put back afterwards: a zoom that jumped to the top of the
    // page instead cost the reader their place every time, and a mode change
    // that works a fit out again would do it for no reason they could see.
    const int keep = m_current;
    const QRect was = keep >= 0 && keep < m_places.size() ? m_places.at(keep).box : QRect();
    const double held
        = was.height() > 0 ? double(verticalScrollBar()->value() - was.top()) / double(was.height()) : 0.0;
    m_zoom = factor;

    // Nothing is thrown away. Every render is now the wrong size, and the wrong
    // size drawn scaled says far more than white paper does for the moment it
    // takes the right one to arrive. Where the reader zooms back out again,
    // which they do constantly, the render they left behind is not merely a
    // stand-in but exactly right.
    //
    // Whatever is queued was asked for at a size that no longer exists. The
    // tasks drop themselves when they see this, rather than being cleared out
    // of the pool, which would take the stand-in renders with them.
    m_gate->sizing.fetch_add(1);
    m_pending.clear();

    // A second notch on the heels of the first says a run is under way, and the
    // renders then wait for the hand to stop: ten notches used to mean ten full
    // renders of which nine were thrown away before anyone saw them. The first
    // notch of a run is never delayed, because most zooms are one notch.
    if (m_zoomAge.isValid() && m_zoomAge.elapsed() < ZoomRun) {
        m_zooming = true;
        m_zoomSettle.start(ZoomSettle, this);
    }
    m_zoomAge.restart();

    relayout();

    if (keep >= 0 && keep < m_places.size()) {
        // Not glided, for the same reason goToPage() is not: this is an answer
        // rather than a movement, and the rest of the window is already showing
        // the new size while the paper would still be on its way there.
        stopGlide();
        const QRect now = m_places.at(keep).box;
        verticalScrollBar()->setValue(int(std::lround(now.top() + held * now.height())));
    }

    viewport()->update();
    Q_EMIT zoomChanged(m_zoom);
}

void PageView::zoomAround(double factor, const QPoint &anchor)
{
    const int row = pageAt(anchor);
    if (row < 0) {
        setZoom(factor); // over the gap between pages; there is nothing to hold
        return;
    }

    // What the pointer is on, in the page's own measure, which does not move
    // when the zoom does. Everything else about the layout, the gaps between
    // pages and the centring of a narrow page, does.
    const QPointF held = toPoints(row, QPointF(anchor));
    setZoom(factor);

    const QPointF now = fromPoints(row, held);
    if (now.isNull()) {
        return;
    }
    horizontalScrollBar()->setValue(horizontalScrollBar()->value() + int(std::lround(now.x() - anchor.x())));
    verticalScrollBar()->setValue(verticalScrollBar()->value() + int(std::lround(now.y() - anchor.y())));
}

void PageView::zoomIn()
{
    setZoom(m_zoom * 1.25);
}

void PageView::zoomOut()
{
    setZoom(m_zoom / 1.25);
}

void PageView::setFit(Fit fit)
{
    if (m_fit != fit) {
        m_fit = fit;
        Q_EMIT fitChanged(m_fit);
    }
    applyFit();
}

void PageView::fitWidth()
{
    setFit(Fit::Width);
}

void PageView::fitHeight()
{
    setFit(Fit::Height);
}

void PageView::fitPage()
{
    setFit(Fit::Page);
}

void PageView::applyFit()
{
    if (m_fit == Fit::Free || m_fitting || !m_document || m_document->pageCount() == 0) {
        return;
    }
    const QSizeF size = pageSizeOf(m_current);
    if (size.isEmpty()) {
        return;
    }

    // Changing the scale relays out the pages, which can bring a scrollbar in,
    // which resizes the viewport, which asks for the fit again. One pass is
    // enough; the second would only chase the first.
    m_fitting = true;

    const int perRow = m_layout == Layout::Facing ? 2 : 1;

    // The vertical scrollbar counts even where it is not up yet: a page fitted
    // to the exact width of the window makes the run of pages taller than the
    // window, the bar arrives, the viewport narrows and the page no longer
    // fits. Subtracted up front rather than discovered afterwards, and not
    // subtracted twice, because a bar already up is outside viewport()->width().
    const int bar
        = verticalScrollBar()->isVisible() ? 0 : style()->pixelMetric(QStyle::PM_ScrollBarExtent, nullptr, this);

    const double byWidth = (viewport()->width() - bar - (perRow + 1) * Gap) / (size.width() * perRow);
    const double byHeight = (viewport()->height() - 2 * Gap) / size.height();

    // Fitting the height is the one fit that leaves the page free to be wider
    // than the window, so it is the one that has to pay for the bar along the
    // bottom. The others cannot need it and it would only shrink the page.
    const int sideways
        = horizontalScrollBar()->isVisible() ? 0 : style()->pixelMetric(QStyle::PM_ScrollBarExtent, nullptr, this);
    const double byHeightAlone = (viewport()->height() - sideways - 2 * Gap) / size.height();

    switch (m_fit) {
    case Fit::Width:
        applyZoom(byWidth);
        break;
    case Fit::Height:
        applyZoom(byHeightAlone);
        break;
    case Fit::Page:
        applyZoom(std::min(byWidth, byHeight));
        break;
    case Fit::Free:
        break;
    }

    m_fitting = false;
}

void PageView::goToPage(int row)
{
    if (row < 0 || row >= m_places.size()) {
        return;
    }

    // Not glided, deliberately. Scrolling is a movement and is smoothed like
    // one; "go to page five" is an answer, and an answer that arrives a tenth
    // of a second late is one the rest of the window is already out of step
    // with: the page number, the grid and whatever asked all say five while
    // the paper is still on its way there.
    stopGlide();
    verticalScrollBar()->setValue(std::max(0, m_places.at(row).box.top() - Gap));
    m_current = row;
    Q_EMIT currentPageChanged(m_current);
}

void PageView::emitPosition()
{
    const QScrollBar *bar = verticalScrollBar();
    const int span = bar->maximum() - bar->minimum();
    // A document shorter than the window has no travel at all; saying "at the
    // top" there is more honest than dividing by zero and more useful than
    // saying nothing, because the strip still has to mark where the reader is.
    const double fraction = span > 0 ? double(bar->value() - bar->minimum()) / double(span) : 0.0;
    Q_EMIT viewPositionChanged(std::clamp(fraction, 0.0, 1.0));
}

void PageView::updateCurrentPage()
{
    // The page that owns the middle of the window, which is the one a reader
    // would say they are on, not the topmost one, which on a tall page is the
    // one they finished with two screens ago.
    const int middle = verticalScrollBar()->value() + viewport()->height() / 2;
    int best = m_current;
    for (int row = 0; row < m_places.size(); ++row) {
        const QRect box = m_places.at(row).box;
        if (box.top() <= middle && middle <= box.bottom()) {
            best = row;
            break;
        }
        if (box.top() > middle) {
            best = row;
            break;
        }
    }
    if (best != m_current) {
        m_current = best;
        Q_EMIT currentPageChanged(m_current);
    }
}

// ── Rendering ahead ───────────────────────────────────────────────────────

int PageView::renderWidthOf(int row) const
{
    if (!m_document || row < 0 || row >= m_document->pageCount()) {
        return 0;
    }
    const QRect box = m_places.value(row).box;
    if (box.isEmpty()) {
        return 0;
    }

    // Rendered at the width it is drawn at, not at a fixed thumbnail size:
    // this is the view people read in, and a scaled-up thumbnail is the exact
    // thing that makes a PDF program feel cheap. At the screen's own ratio and
    // no more: a second scaling on top of the rasteriser's is both slower and
    // softer than asking it for the pixels the screen actually has.
    const int wanted = m_document->pageAt(row).rotation % 180 == 0 ? box.width() : box.height();
    return std::max(1, int(std::lround(wanted * devicePixelRatioF())));
}

int PageView::renderHeightOf(int row) const
{
    if (!m_document || row < 0 || row >= m_document->pageCount()) {
        return 0;
    }
    const QRect box = m_places.value(row).box;
    if (box.isEmpty()) {
        return 0;
    }
    const int wanted = m_document->pageAt(row).rotation % 180 == 0 ? box.height() : box.width();
    return std::max(1, int(std::lround(wanted * devicePixelRatioF())));
}

QRect PageView::bandOf(int row, double margin) const
{
    if (!m_document || row < 0 || row >= m_document->pageCount()) {
        return {};
    }
    // A turned page would need the band mapped back through the rotation, and
    // turned pages are rare enough that the whole sheet is the better answer.
    if (m_document->pageAt(row).rotation != 0) {
        return {};
    }

    const QRect box = m_places.value(row).box;
    const int width = renderWidthOf(row);
    const int height = renderHeightOf(row);
    if (box.isEmpty() || width <= 0 || height <= 0) {
        return {};
    }
    // Whole pages until one is several windowfuls, whichever of the two rules
    // bites first. The absolute ceiling keeps a small window on a small screen
    // from banding an ordinary page; the relative one keeps a large screen from
    // banding a page it very nearly shows all of, where the bands would cost
    // more in re-renders on every scroll than the whole sheet costs once.
    const qint64 windowful = qint64(std::lround(viewport()->width() * devicePixelRatioF()))
        * qint64(std::lround(viewport()->height() * devicePixelRatioF()));
    if (qint64(width) * qint64(height) <= std::max(WholePageLimit, 3 * windowful)) {
        return {};
    }

    // The window in the same coordinates the page is placed in, opened out by
    // the margin, and cut down to the page.
    const QRect window(horizontalScrollBar()->value(), verticalScrollBar()->value(), viewport()->width(),
                       viewport()->height());
    // Less to the sides than above and below, because reading moves down a
    // page and only rarely across one, and every pixel of margin is a pixel
    // the rasteriser has to draw before the reader sees anything.
    const int growX = int(std::lround(window.width() * margin * 0.5));
    const int growY = int(std::lround(window.height() * margin));
    const QRect wanted = window.adjusted(-growX, -growY, growX, growY).intersected(box).translated(-box.topLeft());
    if (wanted.isEmpty()) {
        return {};
    }

    const double scale = double(width) / double(box.width());
    const auto down = [](double value) { return int(std::floor(value / BandGrid)) * BandGrid; };
    const auto up = [](double value) { return int(std::ceil(value / BandGrid)) * BandGrid; };
    const QRect band(down(wanted.left() * scale), down(wanted.top() * scale),
                     up(wanted.right() * scale) - down(wanted.left() * scale),
                     up(wanted.bottom() * scale) - down(wanted.top() * scale));
    return band.intersected(QRect(0, 0, width, height));
}

bool PageView::sheetSatisfies(int row) const
{
    const auto held = m_sheets.constFind(row);
    if (held == m_sheets.constEnd() || held->image.isNull()) {
        return false;
    }

    const int wanted = renderWidthOf(row);
    // A picture made for a larger zoom drawn at a smaller one is not a
    // compromise: every screen pixel is an average of several rendered ones,
    // which is what supersampling is, and it arrives in no time at all. So a
    // reader zooming back out, which they do constantly, pays nothing. Twice
    // over is the limit, because past that the image being held costs four
    // times what a fresh one would and the reader can see none of it.
    if (held->width < wanted || held->width > 2 * wanted) {
        return false;
    }
    if (held->tile.isEmpty()) {
        return true; // the whole page covers anything asked of it
    }

    // A band, though, only covers what it covers, and its pixels are measured
    // at the size it was made, so the part that has to be inside it is worked
    // out at that size too.
    QRect needed = bandOf(row, 0.0);
    if (needed.isEmpty()) {
        return false;
    }
    if (held->width != wanted) {
        const double scale = double(held->width) / double(wanted);
        needed = QRect(int(std::floor(needed.left() * scale)), int(std::floor(needed.top() * scale)),
                       int(std::ceil(needed.width() * scale)) + 1, int(std::ceil(needed.height() * scale)) + 1);
    }
    return held->tile.contains(needed);
}

void PageView::ensureRenders()
{
    if (!m_document || !m_backend || m_places.isEmpty()) {
        m_wantFirst = 0;
        m_wantLast = -1;
        m_gate->wantFirst.store(0);
        m_gate->wantLast.store(-1);
        return;
    }

    const int top = verticalScrollBar()->value();
    const int bottom = top + viewport()->height();

    int first = -1;
    int last = -1;
    for (int row = 0; row < m_places.size(); ++row) {
        const QRect box = m_places.at(row).box;
        if (box.bottom() >= top && box.top() <= bottom) {
            first = first < 0 ? row : first;
            last = row;
        } else if (first >= 0) {
            break; // the pages are in order, so the run of visible ones has ended
        }
    }
    if (first < 0) {
        first = last = std::clamp(m_current, 0, int(m_places.size()) - 1);
    }

    // What a page will cost once it is drawn, which is what decides how many of
    // them are worth having: three A4 pages at reading size are a few megabytes
    // and three at fit-width on a large screen are a quarter of a gigabyte, so
    // the number of pages fetched ahead is an answer, never a constant.
    const double scale = devicePixelRatioF();
    const auto weightOf = [this, scale](int row) {
        const QRect box = m_places.at(row).box;
        return qint64(box.width() * scale) * qint64(box.height() * scale) * 4;
    };

    m_wantFirst = first;
    m_wantLast = last;
    qint64 held = 0;
    for (int row = first; row <= last; ++row) {
        held += weightOf(row);
    }

    // Ahead means ahead of the reader, not below them: someone paging backwards
    // through a document needs the page above the window, and fetching the ones
    // they have just left would leave them looking at white paper every time.
    const bool downwards = m_travel >= 0;
    int &aheadEdge = downwards ? m_wantLast : m_wantFirst;
    int &behindEdge = downwards ? m_wantFirst : m_wantLast;
    const int aheadStep = downwards ? 1 : -1;

    int aheadLeft = PagesAhead;
    int behindLeft = PagesBehind;
    const auto reach = [&](int &edge, int step, int &left) {
        const int row = edge + step;
        if (left <= 0 || row < 0 || row >= m_places.size() || held + weightOf(row) > RenderBudget) {
            left = 0;
            return false;
        }
        held += weightOf(row);
        edge = row;
        --left;
        return true;
    };

    while (aheadLeft > 0 || behindLeft > 0) {
        const bool forward = reach(aheadEdge, aheadStep, aheadLeft);
        const bool backward = reach(behindEdge, -aheadStep, behindLeft);
        if (!forward && !backward) {
            break;
        }
    }

    m_gate->wantFirst.store(m_wantFirst);
    m_gate->wantLast.store(m_wantLast);

    // Nearest to the middle of the window first, so that the order things are
    // asked for in is the order the reader meets them. It matters less than it
    // did now that several render at once, but a queue deeper than the pool is
    // still a queue.
    QVector<int> order;
    order.reserve(m_wantLast - m_wantFirst + 1);
    for (int row = m_wantFirst; row <= m_wantLast; ++row) {
        order.append(row);
    }
    const int centre = (first + last) / 2;
    std::sort(order.begin(), order.end(),
              [centre](int a, int b) { return std::abs(a - centre) < std::abs(b - centre); });

    // The cheap stand-in for everything first, and only then the accurate
    // renders. A proxy is a few milliseconds against a few hundred, so a whole
    // screenful of pages is *shown* in the time one of them takes to be drawn
    // properly. That is the difference the reader actually feels.
    for (int row : std::as_const(order)) {
        requestProxy(row);
    }
    if (!m_zooming) {
        for (int row : std::as_const(order)) {
            requestRender(row, row >= first && row <= last);
        }
    }

    trimRenders();
}

void PageView::requestProxy(int row)
{
    if (!m_document || !m_backend || row < 0 || row >= m_document->pageCount()) {
        return;
    }
    if (m_proxies.contains(row) || m_pendingProxy.contains(row) || m_barren.contains(row)) {
        return;
    }

    RenderBackend::Request request;
    request.widthPx = ProxyWidth;
    // The stand-in has to stand in for the same page the accurate render will
    // show, or a field would appear in its old place for as long as the proxy is
    // stretched over the sheet and vanish when the real picture arrived.
    request.omit = m_omit;
    // Aliased on purpose. At two hundred pixels the difference is invisible
    // once it is stretched over a screen, and this is the render whose whole
    // reason for existing is that it arrives before the accurate one.
    request.draft = true;

    m_pendingProxy.insert(row);
    m_pool.start(new PageRenderTask(this, m_gate, m_gate->generation.load(), m_gate->sizing.load(), row,
                                    m_document->pageAt(row), request, true, false, true),
                 // Ahead of every accurate render: a screenful of stand-ins is
                 // worth more than one perfect page.
                 2);
}

void PageView::requestRender(int row, bool visible)
{
    if (!m_document || !m_backend || row < 0 || row >= m_document->pageCount() || m_pending.contains(row)) {
        return;
    }
    const bool wantImage = !sheetSatisfies(row) && !m_barren.contains(row);
    const bool wantWords = !m_words.contains(row);
    if (!wantImage && !wantWords) {
        return;
    }

    RenderBackend::Request request;
    request.widthPx = renderWidthOf(row);
    request.tile = bandOf(row, 0.5);
    request.omit = m_omit;
    if (request.widthPx <= 0) {
        return;
    }

    m_pending.insert(row);
    m_pool.start(new PageRenderTask(this, m_gate, m_gate->generation.load(), m_gate->sizing.load(), row,
                                    m_document->pageAt(row), request, wantImage, wantWords, false),
                 visible ? 1 : 0);
}

void PageView::applyRender(const Rendered &result)
{
    if (result.generation != m_gate->generation.load()) {
        return; // the document moved on while this was being drawn
    }

    if (result.proxy) {
        m_pendingProxy.remove(result.row);
        if (!result.image.isNull()) {
            // Sixteen bits a pixel: these are stand-ins stretched over a screen,
            // where the banding cannot be seen, and halving them is what lets a
            // whole book's worth stay in hand for the session.
            m_proxies.insert(result.row, result.image.convertToFormat(QImage::Format_RGB16));
            trimProxies();
            viewport()->update();
        } else if (result.tried) {
            m_barren.insert(result.row);
        }
        return;
    }

    m_pending.remove(result.row);

    bool changed = false;
    if (result.gotWords) {
        m_words.insert(result.row, result.words);
        changed = true;
    }

    if (!result.image.isNull()) {
        const Sheet fresh { result.image, result.width, result.tile };
        const Sheet held = m_sheets.value(result.row);
        const bool exact = result.width == renderWidthOf(result.row)
            && (result.tile.isEmpty() || result.tile.contains(bandOf(result.row, 0.0)));

        // Kept when it is what the view wants, and otherwise only when it is a
        // better stand-in than what is already there. A render that arrives
        // after the reader has zoomed past it is still a picture of that page,
        // and a larger picture scales down better than a smaller one scales up.
        if (exact || held.image.isNull() || (!sheetSatisfies(result.row) && fresh.width > held.width)) {
            m_sheets.insert(result.row, fresh);
            trimRenders();
            changed = true;
        }
    } else if (result.tried) {
        m_barren.insert(result.row);
    }

    if (changed) {
        viewport()->update();
    }
}

void PageView::trimRenders()
{
    const auto weigh = [](const QHash<int, Sheet> &pages) {
        qint64 total = 0;
        for (const Sheet &sheet : pages) {
            total += sheet.image.sizeInBytes();
        }
        return total;
    };

    qint64 held = weigh(m_sheets);
    if (held <= RenderBudget) {
        return;
    }

    QVector<int> rows = m_sheets.keys();
    std::sort(rows.begin(), rows.end(),
              [this](int a, int b) { return std::abs(a - m_current) > std::abs(b - m_current); });
    for (int row : std::as_const(rows)) {
        if (held <= RenderBudget) {
            break;
        }
        // Never the page the reader is looking at, whatever it costs: running
        // over the budget is a number, an empty page is a broken program. The
        // proxy survives regardless, so a page dropped here still shows.
        if (row >= m_wantFirst && row <= m_wantLast) {
            continue;
        }
        held -= m_sheets.take(row).image.sizeInBytes();
    }
}

void PageView::trimProxies()
{
    qint64 held = 0;
    for (const QImage &image : std::as_const(m_proxies)) {
        held += image.sizeInBytes();
    }
    if (held <= ProxyBudget) {
        return;
    }

    QVector<int> rows = m_proxies.keys();
    std::sort(rows.begin(), rows.end(),
              [this](int a, int b) { return std::abs(a - m_current) > std::abs(b - m_current); });
    for (int row : std::as_const(rows)) {
        if (held <= ProxyBudget) {
            break;
        }
        if (row >= m_wantFirst && row <= m_wantLast) {
            continue;
        }
        held -= m_proxies.take(row).sizeInBytes();
    }
}

void PageView::invalidateRenders()
{
    m_sheets.clear();
    m_proxies.clear();
    m_pending.clear();
    m_pendingProxy.clear();
    m_barren.clear();

    // Everything in flight was asked for the pages that have just changed, so
    // it is disowned here rather than checked for on arrival.
    m_gate->generation.fetch_add(1);
}

// ── Content ───────────────────────────────────────────────────────────────

const QVector<RenderBackend::Word> *PageView::cachedWords(int row) const
{
    const auto found = m_words.constFind(row);
    return found == m_words.constEnd() ? nullptr : &found.value();
}

const QVector<RenderBackend::Word> &PageView::wordsOf(int row)
{
    const auto cached = m_words.constFind(row);
    if (cached != m_words.constEnd()) {
        return cached.value();
    }

    QVector<RenderBackend::Word> found;
    if (m_document && m_backend && row >= 0 && row < m_document->pageCount()) {
        const PageRef ref = m_document->pageAt(row);
        found = m_backend->words(ref.sourceId, ref.sourcePage);

        // The words come back in the source page's own space; the view shows
        // the page as the organiser has turned it.
        const QTransform turn = pageTurn(ref.rotation, m_document->pageSizePoints(row));
        if (!turn.isIdentity()) {
            for (RenderBackend::Word &word : found) {
                word.rect = turn.mapRect(word.rect.normalized());
            }
        }
    }
    return *m_words.insert(row, found);
}

void PageView::requestPictures(int sourceId)
{
    if (!m_document || sourceId < 0 || m_scanAsked.contains(sourceId)) {
        return;
    }
    const Source *source = m_document->source(sourceId);
    if (!source || source->path().isEmpty()) {
        return;
    }
    m_scanAsked.insert(sourceId);
    m_scanPool.start(new PictureScanTask(this, sourceId, source->path()));
}

void PageView::applyPictures(int sourceId, const QVector<Picture> &pictures)
{
    if (m_scanDone.contains(sourceId)) {
        return; // a scan the view had already grown tired of waiting for
    }
    m_scanDone.insert(sourceId);
    for (const Picture &picture : pictures) {
        m_pictures[pictureKey(sourceId, picture.page)].append(picture);
    }
    viewport()->update();
}

QVector<PageView::Picture> PageView::picturesOf(int row)
{
    if (!m_document || row < 0 || row >= m_document->pageCount()) {
        return {};
    }
    const PageRef ref = m_document->pageAt(row);
    if (!m_scanDone.contains(ref.sourceId)) {
        // Asked for the first time somebody wants to know: a file nobody points
        // at is a file whose content streams never have to be walked.
        requestPictures(ref.sourceId);
        return {};
    }

    QVector<Picture> found = m_pictures.value(pictureKey(ref.sourceId, ref.sourcePage));

    // The size is asked for only when there is a turn to apply, because asking
    // is a question for the rasteriser and this runs on every repaint of a page
    // that has any picture on it at all.
    const QTransform turn = ref.rotation == 0 ? QTransform() : pageTurn(ref.rotation, m_document->pageSizePoints(row));
    if (!turn.isIdentity()) {
        for (Picture &picture : found) {
            picture.placement = turn.mapRect(picture.placement.normalized());
        }
    }
    return found;
}

QVector<PageView::Picture> PageView::picturesOfNow(int row)
{
    if (!m_document || row < 0 || row >= m_document->pageCount()) {
        return {};
    }
    const PageRef ref = m_document->pageAt(row);
    if (!m_scanDone.contains(ref.sourceId)) {
        const Source *source = m_document->source(ref.sourceId);
        if (!source || source->path().isEmpty()) {
            return {};
        }
        // Somebody has asked for a picture by dragging a box round it, so the
        // file is read here and now rather than answered with "not yet".
        QGuiApplication::setOverrideCursor(Qt::BusyCursor);
        QString error;
        const QVector<ImageEdit::ImageUse> uses = ImageEdit::read(source->path(), &error);
        QGuiApplication::restoreOverrideCursor();

        QVector<Picture> pictures;
        pictures.reserve(uses.size());
        for (const ImageEdit::ImageUse &use : uses) {
            pictures.append({ use.page, use.resourceName, use.placement, use.decodable });
        }
        m_scanAsked.insert(ref.sourceId);
        applyPictures(ref.sourceId, pictures);
    }
    return picturesOf(row);
}

// ── Painting ──────────────────────────────────────────────────────────────

void PageView::paintEvent(QPaintEvent *event)
{
    QPainter painter(viewport());
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.fillRect(event->rect(), palette().color(QPalette::Dark));

    if (!m_document || m_document->pageCount() == 0) {
        return;
    }

    // Asked for here as well as on every scroll, so that a window shown, resized
    // or uncovered has its pages on the way before it needs them.
    ensureRenders();

    const Caret from = std::min(m_anchor, m_focus);
    const Caret to = std::max(m_anchor, m_focus);
    const bool text = hasTextSelection();

    for (int row = 0; row < m_places.size(); ++row) {
        const QRect box = pageRect(row);
        if (!box.intersects(event->rect())) {
            continue;
        }

        // Only the damaged part of the sheet, not the whole of it: at a high
        // zoom a page is a hundred megapixels and filling all of them white on
        // every scrolled line is time spent on paper nobody is looking at.
        painter.fillRect(box.intersected(event->rect()), Qt::white);

        // The kept stand-in underneath and whatever is sharp on top of it. Two
        // draws rather than one, because a band covers only part of the page
        // and the rest of the sheet still has to say what is on it, and because
        // a page with no current render at all then shows softened rather than
        // blank, which is what makes a viewer feel quick even when it is not.
        const auto proxy = m_proxies.constFind(row);
        if (proxy != m_proxies.constEnd()) {
            drawSheet(painter, box, event->rect(), proxy.value(), 0, QRect());
        }
        const auto ready = m_sheets.constFind(row);
        if (ready != m_sheets.constEnd()) {
            drawSheet(painter, box, event->rect(), ready->image, ready->width, ready->tile);
        }

        painter.setPen(QPen(QColor(0, 0, 0, 60), 1));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(box.adjusted(0, 0, -1, -1));

        // ── the selection ──────────────────────────────────────────────────
        if (text && row >= from.page && row <= to.page) {
            // Never fetched from here: a page whose words are still being read
            // will ask for another paint the moment they arrive, and blocking
            // the window on the renderer is what this class stopped doing.
            if (const QVector<RenderBackend::Word> *words = cachedWords(row)) {
                const int begin = row == from.page ? from.word : 0;
                const int end = row == to.page ? to.word : int(words->size()) - 1;

                QColor wash = palette().color(QPalette::Highlight);
                wash.setAlpha(90);
                painter.setPen(Qt::NoPen);
                painter.setBrush(wash);

                for (int i = std::max(0, begin); i <= std::min<int>(end, int(words->size()) - 1); ++i) {
                    const RenderBackend::Word &word = words->at(i);
                    const int firstChar = row == from.page && i == from.word ? from.character : 0;
                    const int lastChar = row == to.page && i == to.word ? to.character : int(word.text.size());
                    if (lastChar <= firstChar) {
                        continue;
                    }
                    const QRectF part(QPointF(xOfCharacter(word, firstChar), word.rect.top()),
                                      QPointF(xOfCharacter(word, lastChar), word.rect.bottom()));
                    painter.drawRect(fromPoints(row, part));
                }
            }
        }

        // ── the box, and what it holds ─────────────────────────────────────
        if (m_boxPage == row && m_boxPoints.isValid()) {
            const QRectF drawn = fromPoints(row, m_boxPoints);
            QColor wash = palette().color(QPalette::Highlight);
            wash.setAlpha(40);
            painter.setPen(Qt::NoPen);
            painter.setBrush(wash);
            painter.drawRect(drawn);
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(palette().color(QPalette::Highlight), 1, Qt::DashLine));
            painter.drawRect(drawn);
        }

        // ── the overlays ───────────────────────────────────────────────────
        for (Overlay *overlay : std::as_const(m_overlays)) {
            if (!overlay->appliesTo(m_mode)) {
                continue;
            }
            painter.save();
            painter.setClipRect(box);
            overlay->paint(painter, row, box);
            painter.restore();
        }
    }
}

void PageView::resizeEvent(QResizeEvent *event)
{
    QAbstractScrollArea::resizeEvent(event);
    relayout();

    // A fit is a statement about the window, so it is answered again whenever
    // the window changes. This is also what carries a reading size across a
    // trip through another mode: the view is not laid out at all while it is
    // off screen, so the window it comes back to may be a different one from
    // the window it left, and without this the page would hang off the edge
    // with the menu still claiming it fits.
    applyFit();
}

void PageView::scrollContentsBy(int dx, int dy)
{
    QAbstractScrollArea::scrollContentsBy(dx, dy);

    // Which way the reader is going decides which pages are worth fetching, and
    // the scroll is the only place that knows it. A sideways-only scroll leaves
    // the answer as it was rather than pretending the reader turned round.
    if (dy != 0) {
        m_travel = dy < 0 ? 1 : -1; // the contents move up when the reader goes down
    }
    updateCurrentPage();
    emitPosition();
    viewport()->update();
}

// ── The motion ────────────────────────────────────────────────────────────

void PageView::glideTo(double target)
{
    QScrollBar *bar = verticalScrollBar();
    m_glideTarget = std::clamp(target, double(bar->minimum()), double(bar->maximum()));
    if (std::abs(m_glideTarget - bar->value()) < 1.0) {
        stopGlide();
        return;
    }
    if (!m_gliding) {
        m_gliding = true;
        // Precise rather than coarse: a frame timer allowed to drift by a
        // twentieth shows up as a stutter in something the eye is following.
        m_glide.start(GlideInterval, Qt::PreciseTimer, this);
    }
}

void PageView::glideBy(double pixels)
{
    // Measured from where the view is *going*, not from where it is: a second
    // notch while the first is still travelling has to add to the journey, or
    // the wheel feels like it is being ignored.
    const double from = m_gliding ? m_glideTarget : verticalScrollBar()->value();
    glideTo(from + pixels);
}

void PageView::stopGlide()
{
    m_glide.stop();
    m_gliding = false;
}

void PageView::timerEvent(QTimerEvent *event)
{
    if (event->timerId() == m_zoomSettle.timerId()) {
        m_zoomSettle.stop();
        m_zooming = false;
        ensureRenders();
        return;
    }
    if (event->timerId() != m_glide.timerId()) {
        QAbstractScrollArea::timerEvent(event);
        return;
    }

    QScrollBar *bar = verticalScrollBar();
    const double remaining = m_glideTarget - bar->value();
    if (std::abs(remaining) < 1.0) {
        bar->setValue(int(std::lround(m_glideTarget)));
        stopGlide();
        return;
    }

    const double step = remaining * GlideShare;
    const int was = bar->value();
    bar->setValue(was + int(remaining > 0 ? std::ceil(step) : std::floor(step)));
    if (bar->value() == was) {
        stopGlide(); // the end of the document; there is nowhere left to go
    }
}

void PageView::panBy(const QPoint &delta)
{
    stopGlide();
    verticalScrollBar()->setValue(verticalScrollBar()->value() - delta.y());
    horizontalScrollBar()->setValue(horizontalScrollBar()->value() - delta.x());
}

void PageView::wheelEvent(QWheelEvent *event)
{
    if (event->modifiers() & Qt::ControlModifier) {
        // Gathered rather than divided away: a high-resolution wheel reports
        // fractions of a notch, and a view that keeps only whole ones ignores
        // the hand entirely until it has turned far enough.
        m_zoomNotches += event->angleDelta().y();
        const int steps = m_zoomNotches / 120;
        m_zoomNotches -= steps * 120;
        if (steps != 0) {
            zoomAround(m_zoom * std::pow(1.25, steps), event->position().toPoint());
        }
        event->accept();
        return;
    }

    // A touchpad reports the distance the fingers actually travelled, which is
    // already as smooth as the hand that made it; gliding it as well would only
    // put the page behind the fingers.
    const QPoint pixels = event->pixelDelta();
    if (!pixels.isNull()) {
        stopGlide();
        panBy(pixels);
        event->accept();
        return;
    }

    const QPoint angle = event->angleDelta();
    if (angle.isNull()) {
        QAbstractScrollArea::wheelEvent(event);
        return;
    }

    // As many lines to a notch as the desktop says a wheel should carry, and
    // never more than half a window: on a tall page a notch that jumped a
    // screen would lose the reader's place.
    const double lines = std::max(1, QApplication::wheelScrollLines());
    const double step = std::min(lines * 40.0, viewport()->height() / 2.0);

    if (std::abs(angle.x()) > std::abs(angle.y())) {
        // Sideways is not glided: it is a nudge across a page too wide for the
        // window, not a journey through the document.
        horizontalScrollBar()->setValue(horizontalScrollBar()->value() - int(std::lround(angle.x() / 120.0 * step)));
        event->accept();
        return;
    }

    glideBy(-(angle.y() / 120.0) * step);
    event->accept();
}

// ── Selecting text ────────────────────────────────────────────────────────

bool PageView::hasTextSelection() const
{
    return m_anchor.isValid() && m_focus.isValid() && m_anchor != m_focus;
}

bool PageView::hasSelection() const
{
    return hasTextSelection() || (m_boxPage >= 0 && m_boxPoints.isValid());
}

int PageView::wordAt(int row, const QPointF &pagePoint) const
{
    const QVector<RenderBackend::Word> *words = cachedWords(row);
    if (!words) {
        return -1;
    }

    // Inside a word first, and only then the nearest one: dragging through
    // the space between two lines should carry the selection along rather
    // than stopping dead.
    const int inside = wordUnder(row, pagePoint);
    if (inside >= 0) {
        return inside;
    }

    int best = -1;
    double nearest = std::numeric_limits<double>::max();
    for (int i = 0; i < words->size(); ++i) {
        const QRectF r = words->at(i).rect;
        const double dx = std::max({ r.left() - pagePoint.x(), 0.0, pagePoint.x() - r.right() });
        const double dy = std::max({ r.top() - pagePoint.y(), 0.0, pagePoint.y() - r.bottom() });
        const double distance = dx * dx + dy * dy;
        if (distance < nearest) {
            nearest = distance;
            best = i;
        }
    }
    return best;
}

int PageView::wordUnder(int row, const QPointF &pagePoint) const
{
    const QVector<RenderBackend::Word> *words = cachedWords(row);
    if (!words) {
        return -1;
    }
    for (int i = 0; i < words->size(); ++i) {
        if (words->at(i).rect.contains(pagePoint)) {
            return i;
        }
    }
    return -1;
}

PageView::Caret PageView::caretAt(int row, const QPointF &pagePoint) const
{
    const QVector<RenderBackend::Word> *words = cachedWords(row);
    if (!words || words->isEmpty()) {
        return {};
    }
    const int index = wordAt(row, pagePoint);
    if (index < 0) {
        return {};
    }
    return { row, index, characterAt(words->at(index), pagePoint.x()) };
}

void PageView::extendSelectionTo(int row, const QPointF &pagePoint)
{
    wordsOf(row);
    const Caret caret = caretAt(row, pagePoint);
    if (!caret.isValid() || caret == m_focus) {
        return;
    }
    m_focus = caret;
    viewport()->update();
    Q_EMIT selectionChanged();
}

void PageView::selectLineAt(int row, const QPointF &pagePoint)
{
    const QVector<RenderBackend::Word> &words = wordsOf(row);
    const int index = wordAt(row, pagePoint);
    if (index < 0) {
        return;
    }

    // A line is the run of words either side of this one that share its band of
    // the page. Reading order is what makes that a run rather than a search:
    // the word before this one on the page is the word before it in the list,
    // and the line ends where the band does.
    const QRectF band = words.at(index).rect;
    int first = index;
    while (first > 0 && sameLine(band, words.at(first - 1).rect)) {
        --first;
    }
    int last = index;
    while (last + 1 < words.size() && sameLine(band, words.at(last + 1).rect)) {
        ++last;
    }

    m_boxPage = -1;
    m_boxPoints = QRectF();
    m_anchor = { row, first, 0 };
    m_focus = { row, last, int(words.at(last).text.size()) };
    m_selecting = false;
    viewport()->update();
    Q_EMIT selectionChanged();
    publishSelection();
}

void PageView::clearSelection()
{
    if (!m_anchor.isValid() && !m_focus.isValid() && m_boxPage < 0) {
        return;
    }
    m_anchor = {};
    m_focus = {};
    m_boxPage = -1;
    m_boxPoints = QRectF();
    m_draggingBox = false;
    viewport()->update();
    Q_EMIT selectionChanged();
}

void PageView::selectAll()
{
    if (!m_document || m_document->pageCount() == 0) {
        return;
    }
    const int last = m_document->pageCount() - 1;
    wordsOf(0);
    const QVector<RenderBackend::Word> &lastWords = wordsOf(last);

    m_boxPage = -1;
    m_boxPoints = QRectF();
    m_anchor = { 0, 0, 0 };
    m_focus
        = { last, std::max(0, int(lastWords.size()) - 1), lastWords.isEmpty() ? 0 : int(lastWords.last().text.size()) };
    viewport()->update();
    Q_EMIT selectionChanged();
}

QString PageView::selectedText() const
{
    if (m_boxPage >= 0 && m_boxPoints.isValid()) {
        return textInside(m_boxPage, m_boxPoints);
    }
    if (!hasTextSelection()) {
        return {};
    }

    const Caret from = std::min(m_anchor, m_focus);
    const Caret to = std::max(m_anchor, m_focus);

    QString text;

    // The word most recently taken, and which row it came off. Carried across
    // the rows rather than reset at each one, because the break between two
    // pages is a line break like any other and counting it separately is what
    // used to make it the only one in the whole selection: a paragraph dragged
    // over with the mouse pasted as a single unbroken run of words, while the
    // very same paragraph taken with a rectangle came out as a paragraph.
    const RenderBackend::Word *previous = nullptr;
    int previousRow = -1;

    for (int row = from.page; row <= to.page; ++row) {
        const QVector<RenderBackend::Word> *words = cachedWords(row);
        if (!words) {
            continue;
        }
        const int begin = row == from.page ? from.word : 0;
        const int end = row == to.page ? to.word : int(words->size()) - 1;

        for (int i = std::max(0, begin); i <= std::min<int>(end, int(words->size()) - 1); ++i) {
            const RenderBackend::Word &word = words->at(i);
            const int firstChar = row == from.page && i == from.word ? from.character : 0;
            const int lastChar = row == to.page && i == to.word ? to.character : int(word.text.size());
            if (lastChar <= firstChar) {
                continue;
            }

            if (previous) {
                // What separates this word from the one before it. A different
                // sheet is a new line whatever the boxes say; on one sheet the
                // boxes settle it, and a column break counts as a line ending
                // because the words arrive in reading order and the next column
                // starts back up at the top.
                //
                // The line break wins over what the renderer says about a space.
                // A word broken across a line reports no space after it. That
                // is true, and is why "Nord-" and "see" must not be pasted with
                // one between them, but they are still on two lines, and the
                // hyphen is left where the page printed it. It cannot be told
                // from the one in "E-Mail" without a dictionary, and swallowing
                // it would silently damage every word that really carries one.
                if (row != previousRow || !sameLine(previous->rect, word.rect)) {
                    text += u'\n';
                } else if (!previous->joinedToNext) {
                    // The renderer knows whether a space followed; guessing from
                    // the boxes puts spaces inside hyphenated words and none
                    // between words set tight.
                    text += u' ';
                }
            }

            text += word.text.mid(firstChar, lastChar - firstChar);
            previous = &word;
            previousRow = row;
        }
    }
    return text;
}

QString PageView::textInside(int row, const QRectF &pageRect) const
{
    const QVector<RenderBackend::Word> *words = cachedWords(row);
    if (!words) {
        return {};
    }

    QString text;
    int previous = -1;
    for (int i = 0; i < words->size(); ++i) {
        const RenderBackend::Word &word = words->at(i);
        // A word belongs to the box when its middle does. Any other rule either
        // takes words the box only grazes or drops words it plainly covers, and
        // the middle is the one a person can predict while dragging.
        if (!pageRect.contains(word.rect.center())) {
            continue;
        }
        if (previous >= 0) {
            // A box drawn round a paragraph should paste as that paragraph, so
            // the line breaks the page has are kept and the spacing inside a
            // line is the renderer's answer rather than a guess from the boxes.
            const RenderBackend::Word &before = words->at(previous);
            if (!sameLine(before.rect, word.rect)) {
                text += u'\n';
            } else if (!before.joinedToNext) {
                text += u' ';
            }
        }
        text += word.text;
        previous = i;
    }
    return text;
}

void PageView::publishSelection()
{
    // The selection clipboard is what middle-click pastes, and on this desktop
    // it is filled by selecting rather than by copying. A page that leaves it
    // alone is the one window on the screen where that gesture does nothing.
    QClipboard *clipboard = QApplication::clipboard();
    if (!clipboard->supportsSelection()) {
        return;
    }
    const QString text = selectedText();
    if (!text.isEmpty()) {
        clipboard->setText(text, QClipboard::Selection);
    }
}

void PageView::copySelection()
{
    if (m_boxPage >= 0 && m_boxPoints.isValid()) {
        copyBox();
        return;
    }
    const QString text = selectedText();
    if (text.isEmpty()) {
        return;
    }
    QApplication::clipboard()->setText(text);
    publishSelection();
}

// ── Taking a picture off the page ─────────────────────────────────────────

int PageView::pictureAt(int row, const QPointF &pagePoint)
{
    const QVector<Picture> pictures = picturesOf(row);
    int best = -1;
    double smallest = std::numeric_limits<double>::max();
    for (int i = 0; i < pictures.size(); ++i) {
        const QRectF placement = pictures.at(i).placement;
        if (!placement.contains(pagePoint)) {
            continue;
        }
        // The smallest picture covering the point, because a picture laid over a
        // background is the one the reader is pointing at.
        const double area = placement.width() * placement.height();
        if (area < smallest) {
            smallest = area;
            best = i;
        }
    }
    return best;
}

QImage PageView::pictureInside(int row, const QRectF &pageRect, QString *why)
{
    const auto refuse = [why](const QString &reason) {
        if (why) {
            *why = reason;
        }
        return QImage();
    };

    if (!m_document || row < 0 || row >= m_document->pageCount()) {
        return QImage();
    }
    const PageRef ref = m_document->pageAt(row);
    const Source *source = m_document->source(ref.sourceId);
    if (!source || source->path().isEmpty()) {
        return refuse(i18nc("@info", "The file behind this page is not available, so its pictures cannot be read."));
    }

    const QVector<Picture> pictures = picturesOfNow(row);
    const double boxArea = pageRect.width() * pageRect.height();

    int chosen = -1;
    double most = 0.0;
    bool refusedOne = false;
    for (int i = 0; i < pictures.size(); ++i) {
        const QRectF shared = pictures.at(i).placement.intersected(pageRect);
        if (shared.isEmpty()) {
            continue;
        }
        const double area = shared.width() * shared.height();
        const double own = pictures.at(i).placement.width() * pictures.at(i).placement.height();
        // Over a picture, not merely touching one: either the box is mostly on
        // it or it is mostly in the box. A corner clipped by a box drawn round a
        // paragraph is not a request for that picture.
        if (area < 0.25 * std::min(boxArea, own)) {
            continue;
        }
        if (area > most) {
            if (!pictures.at(i).decodable) {
                refusedOne = true;
                continue;
            }
            most = area;
            chosen = i;
        }
    }

    if (chosen < 0) {
        if (refusedOne) {
            return refuse(i18nc("@info",
                                "The picture in that box is stored in a form this program cannot open: a "
                                "fax or a JPEG 2000 image, whose pixels nothing here can read."));
        }
        return QImage();
    }

    // Taken out of the file rather than off the screen: what the page draws is
    // the stored picture, and a copy cut out of the rendered view would carry
    // whatever zoom it happened to be at and whatever was drawn behind it.
    QTemporaryFile file(QDir::tempPath() + u"/pdf-smithy-copy-XXXXXX.png"_s);
    if (!file.open()) {
        return refuse(i18nc("@info there is nowhere to write a temporary file",
                            "A picture could not be copied: this machine has nowhere to put it while it is being "
                            "read out of the file."));
    }
    const QString path = file.fileName();
    file.close();

    QString error;
    QGuiApplication::setOverrideCursor(Qt::BusyCursor);
    const bool taken = ImageEdit::extract(source->path(), ref.sourcePage, pictures.at(chosen).name, path, &error);
    QGuiApplication::restoreOverrideCursor();
    if (!taken) {
        return refuse(i18nc("@info %1 names a picture, %2 says what went wrong", "%1 could not be copied: %2",
                            pictures.at(chosen).name, error));
    }

    const QImage image(path);
    if (image.isNull()) {
        return refuse(i18nc("@info %1 names a picture",
                            "%1 came out of the file in a form this program cannot read back.",
                            pictures.at(chosen).name));
    }
    // Handed over as the reader sees it, not as the file stores it: the page
    // they took it from is the turned one.
    return rotated(image, ref.rotation);
}

bool PageView::copyBox()
{
    if (m_boxPage < 0 || !m_boxPoints.isValid()) {
        return false;
    }

    const QString text = textInside(m_boxPage, m_boxPoints);
    QString why;
    const QImage picture = pictureInside(m_boxPage, m_boxPoints, &why);

    if (text.isEmpty() && picture.isNull()) {
        Q_EMIT refused(why.isEmpty() ? i18nc("@info",
                                             "There is nothing inside that box that can be copied. A box "
                                             "takes the words it covers, and a picture it is drawn over.")
                                     : why);
        return false;
    }

    auto *data = new QMimeData;
    if (!text.isEmpty()) {
        data->setText(text);
    }
    if (!picture.isNull()) {
        data->setImageData(picture);
    }
    QApplication::clipboard()->setMimeData(data);

    // Said out loud even though something was copied: the reader drew a box
    // round a picture and got only the words, and a silent half-result is the
    // kind of thing people discover after they have pasted it.
    if (picture.isNull() && !why.isEmpty()) {
        Q_EMIT refused(why);
    }
    return true;
}

// ── Cut, copy and paste ───────────────────────────────────────────────────

void PageView::copy()
{
    if (m_mode == Mode::View) {
        if (!hasSelection()) {
            Q_EMIT refused(i18nc("@info",
                                 "Nothing is selected. Drag across the words you want to copy, or hold Ctrl "
                                 "and drag a box round what you want to take."));
            return;
        }
        copySelection();
        return;
    }

    for (Overlay *overlay : std::as_const(m_overlays)) {
        if (overlay->appliesTo(m_mode) && overlay->copy()) {
            return;
        }
    }
    Q_EMIT refused(i18nc("@info", "Nothing is chosen to copy. Click something on the page first."));
}

void PageView::cut()
{
    if (m_mode == Mode::View) {
        // Refused by name rather than ignored: a command that does nothing at
        // all is indistinguishable from one that is broken.
        Q_EMIT refused(i18nc("@info",
                             "This document is open for reading, so nothing can be cut out of it. Switch "
                             "to Edit to change the page."));
        return;
    }

    for (Overlay *overlay : std::as_const(m_overlays)) {
        if (overlay->appliesTo(m_mode) && overlay->cut()) {
            return;
        }
    }
    Q_EMIT refused(i18nc("@info", "Nothing is chosen to cut. Click something on the page first."));
}

void PageView::paste()
{
    if (m_mode == Mode::View) {
        Q_EMIT refused(i18nc("@info",
                             "This document is open for reading, so nothing can be put on the page. Switch "
                             "to Edit to add something to it."));
        return;
    }

    const QMimeData *data = QApplication::clipboard()->mimeData();
    if (!data) {
        Q_EMIT refused(i18nc("@info", "There is nothing on the clipboard."));
        return;
    }
    for (Overlay *overlay : std::as_const(m_overlays)) {
        if (overlay->appliesTo(m_mode) && overlay->paste(data, m_current)) {
            return;
        }
    }
    Q_EMIT refused(i18nc("@info",
                         "What is on the clipboard is not something that can be put on a page. Text and "
                         "pictures can."));
}

// ── The pointer ───────────────────────────────────────────────────────────

void PageView::updateCursor(const QPoint &where)
{
    // The hand comes before everything, overlays included: someone holding
    // space or dragging the middle button has said they are moving the page
    // about, and no layer under the pointer changes that.
    if (m_panning) {
        viewport()->setCursor(Qt::ClosedHandCursor);
        return;
    }
    if (m_spaceHeld || m_tool == Tool::Hand) {
        viewport()->setCursor(Qt::OpenHandCursor);
        return;
    }

    const int row = pageAt(where);
    if (row < 0) {
        viewport()->unsetCursor();
        return;
    }
    const QPointF point = toPoints(row, QPointF(where));

    // Where an overlay will take the press, what the view would have done with
    // it is not what is about to happen, so the overlay answers for the place.
    for (Overlay *overlay : std::as_const(m_overlays)) {
        if (!overlay->appliesTo(m_mode)) {
            continue;
        }
        const Qt::CursorShape shape = overlay->cursor(row, point);
        if (shape != Qt::ArrowCursor) {
            viewport()->setCursor(shape);
            return;
        }
    }

    if (m_tool == Tool::Rectangle || (QGuiApplication::keyboardModifiers() & Qt::ControlModifier)) {
        viewport()->setCursor(Qt::CrossCursor);
        return;
    }

    if (m_mode == Mode::View) {
        // The I-beam is the promise that text can be dragged over, so it only
        // appears where there actually is text under the pointer.
        if (wordUnder(row, point) >= 0) {
            viewport()->setCursor(Qt::IBeamCursor);
            return;
        }
        // And over a picture the pointer says a copy can be taken, because that
        // is what a click there now does.
        if (pictureAt(row, point) >= 0) {
            viewport()->setCursor(Qt::DragCopyCursor);
            return;
        }
    }
    viewport()->unsetCursor();
}

// ── The mouse ─────────────────────────────────────────────────────────────

void PageView::mousePressEvent(QMouseEvent *event)
{
    // A hand on the mouse ends whatever the wheel started; carrying on gliding
    // under a press is the view arguing with the user.
    stopGlide();

    const bool wantsHand = event->button() == Qt::MiddleButton
        || (event->button() == Qt::LeftButton && (m_spaceHeld || m_tool == Tool::Hand));
    if (wantsHand) {
        m_panning = true;
        m_panFrom = event->pos();
        viewport()->setCursor(Qt::ClosedHandCursor);
        event->accept();
        return;
    }

    const int row = pageAt(event->pos());
    if (row < 0) {
        clearSelection();
        return;
    }
    const QPointF point = toPoints(row, event->position());

    if (event->button() == Qt::RightButton) {
        Q_EMIT contextMenuWanted(row, point, event->globalPosition().toPoint());
        return;
    }
    if (event->button() != Qt::LeftButton) {
        return;
    }

    // Qt counts to two and stops. The third click of a triple arrives here as
    // an ordinary press, so the counting is done here or not at all.
    const bool sameSpot = m_clickAge.isValid() && m_clickAge.elapsed() < QApplication::doubleClickInterval()
        && (event->pos() - m_clickWhere).manhattanLength() < 5;
    m_clicks = sameSpot ? m_clicks + 1 : 1;
    m_clickAge.restart();
    m_clickWhere = event->pos();

    for (Overlay *overlay : std::as_const(m_overlays)) {
        if (overlay->appliesTo(m_mode) && overlay->press(row, point, event->buttons())) {
            m_grabbed = overlay;
            m_grabbedRow = row;
            return;
        }
    }

    // After the overlays, not before: three clicks into a form field are three
    // clicks into a form field, whatever they would have meant on the page.
    if (m_clicks >= 3 && m_mode == Mode::View) {
        selectLineAt(row, point);
        return;
    }

    if (m_mode == Mode::View) {
        if (m_tool == Tool::Rectangle || (event->modifiers() & Qt::ControlModifier)) {
            // Fetched now rather than at the copy: the box is about to be told
            // which words it covers, and it can only answer for words it has.
            wordsOf(row);
            m_anchor = {};
            m_focus = {};
            m_boxPage = row;
            m_boxAnchor = point;
            m_boxPoints = QRectF(point, point);
            m_draggingBox = true;
            viewport()->update();
            Q_EMIT selectionChanged();
            return;
        }

        wordsOf(row);
        const Caret caret = caretAt(row, point);
        if (wordUnder(row, point) >= 0) {
            m_boxPage = -1;
            m_boxPoints = QRectF();
            m_anchor = m_focus = caret; // a click is where a selection starts, not one in itself
            m_selecting = true;
            viewport()->update();
            Q_EMIT selectionChanged();
            return;
        }

        // No text under the pointer, but a picture: the click takes the picture,
        // which is what the pointer has been promising over it.
        const int picture = pictureAt(row, point);
        if (picture >= 0) {
            m_anchor = {};
            m_focus = {};
            m_boxPage = row;
            m_boxPoints = picturesOf(row).at(picture).placement;
            m_draggingBox = false;
            viewport()->update();
            Q_EMIT selectionChanged();
            return;
        }

        // Off the text but still on the page: the drag starts from the nearest
        // word, the way a drag begun in a margin does in any reader.
        if (caret.isValid()) {
            m_boxPage = -1;
            m_boxPoints = QRectF();
            m_anchor = m_focus = caret;
            m_selecting = true;
            viewport()->update();
            Q_EMIT selectionChanged();
            return;
        }
    }
    clearSelection();
}

void PageView::mouseMoveEvent(QMouseEvent *event)
{
    if (m_panning) {
        const QPoint delta = event->pos() - m_panFrom;
        m_panFrom = event->pos();
        panBy(delta);
        return;
    }

    if (m_grabbed) {
        m_grabbed->move(m_grabbedRow, toPoints(m_grabbedRow, event->position()), event->buttons());
        viewport()->update();
        return;
    }

    if (m_draggingBox && m_boxPage >= 0) {
        m_boxPoints = QRectF(m_boxAnchor, toPoints(m_boxPage, event->position())).normalized();
        viewport()->update();
        return;
    }

    const int row = pageAt(event->pos());
    if (m_selecting && row >= 0) {
        extendSelectionTo(row, toPoints(row, event->position()));
        return;
    }

    updateCursor(event->pos());
}

void PageView::mouseReleaseEvent(QMouseEvent *event)
{
    if (m_panning) {
        m_panning = false;
        updateCursor(event->pos());
        return;
    }

    if (m_grabbed) {
        m_grabbed->release(m_grabbedRow, toPoints(m_grabbedRow, event->position()));
        m_grabbed = nullptr;
        m_grabbedRow = -1;
        viewport()->update();
        return;
    }

    if (m_draggingBox) {
        m_draggingBox = false;
        const QRectF drawn = fromPoints(m_boxPage, m_boxPoints);
        if (drawn.width() < BoxThreshold && drawn.height() < BoxThreshold) {
            // A box that never opened is a click: whatever is under it, if
            // anything, rather than an empty selection nobody can see.
            const QPointF point = m_boxAnchor;
            const int picture = pictureAt(m_boxPage, point);
            m_boxPoints = picture >= 0 ? picturesOf(m_boxPage).at(picture).placement : QRectF();
            if (picture < 0) {
                m_boxPage = -1;
            }
        }
        viewport()->update();
        Q_EMIT selectionChanged();
        publishSelection();
        return;
    }

    if (m_selecting) {
        m_selecting = false;
        publishSelection();
    }
}

void PageView::mouseDoubleClickEvent(QMouseEvent *event)
{
    const int row = pageAt(event->pos());
    if (row < 0 || m_mode != Mode::View) {
        return;
    }

    // The second click of a triple, counted here because Qt sends it as a
    // double click rather than as a press and the count would otherwise stall.
    m_clicks = 2;
    m_clickAge.restart();
    m_clickWhere = event->pos();

    // A double click picks the word under it, which is what it does everywhere
    // else and what people try first. Under it and not merely near it: a double
    // click in a margin means nothing, and answering it with whichever word
    // happened to be closest is the kind of guess that reads as a bug.
    const QVector<RenderBackend::Word> &words = wordsOf(row);
    const int word = wordUnder(row, toPoints(row, event->position()));
    if (word >= 0) {
        m_boxPage = -1;
        m_boxPoints = QRectF();
        m_anchor = { row, word, 0 };
        m_focus = { row, word, int(words.at(word).text.size()) };
        viewport()->update();
        Q_EMIT selectionChanged();
        publishSelection();
    }
}

bool PageView::event(QEvent *event)
{
    // The window binds Delete, and may well bind the clipboard keys, to actions
    // of its own about whole pages. A shortcut is matched before the key ever
    // reaches this widget, so the ones the open page means something by are
    // claimed back here; accepting the override sends the key on as a key.
    if (event->type() == QEvent::ShortcutOverride) {
        auto *key = static_cast<QKeyEvent *>(event);
        if (key->matches(QKeySequence::Cut) || key->matches(QKeySequence::Copy) || key->matches(QKeySequence::Paste)) {
            key->accept();
            return true;
        }
    }

    // A window that loses the keyboard never sees the key come back up, and a
    // view left believing space is still down would pan on the next click.
    if (event->type() == QEvent::FocusOut || event->type() == QEvent::WindowDeactivate) {
        if (m_spaceHeld) {
            m_spaceHeld = false;
            updateCursor(viewport()->mapFromGlobal(QCursor::pos()));
        }
    }
    return QAbstractScrollArea::event(event);
}

void PageView::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Space && !event->isAutoRepeat() && !(event->modifiers() & Qt::ControlModifier)) {
        m_spaceHeld = true;
        updateCursor(viewport()->mapFromGlobal(QCursor::pos()));
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Control) {
        updateCursor(viewport()->mapFromGlobal(QCursor::pos()));
    }

    if (event->matches(QKeySequence::Copy)) {
        copy();
        event->accept();
        return;
    }
    if (event->matches(QKeySequence::Cut)) {
        cut();
        event->accept();
        return;
    }
    if (event->matches(QKeySequence::Paste)) {
        paste();
        event->accept();
        return;
    }
    if (event->matches(QKeySequence::SelectAll)) {
        selectAll();
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Escape) {
        stopGlide();
        clearSelection();
        event->accept();
        return;
    }

    // The keys that move through the document go the same way the wheel does.
    // A page key that jumps leaves the reader hunting for where they were, and
    // it is the same complaint whichever hand made the movement.
    if (event->modifiers() == Qt::NoModifier) {
        const double screen = viewport()->height();
        switch (event->key()) {
        case Qt::Key_PageDown:
            // A little less than a windowful, so a line or two of what was just
            // read stays on screen and the eye has somewhere to land.
            glideBy(screen * 0.9);
            event->accept();
            return;
        case Qt::Key_PageUp:
            glideBy(-screen * 0.9);
            event->accept();
            return;
        case Qt::Key_Down:
            glideBy(verticalScrollBar()->singleStep());
            event->accept();
            return;
        case Qt::Key_Up:
            glideBy(-verticalScrollBar()->singleStep());
            event->accept();
            return;
        default:
            break;
        }
    }
    QAbstractScrollArea::keyPressEvent(event);
}

void PageView::keyReleaseEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Space && !event->isAutoRepeat()) {
        m_spaceHeld = false;
        updateCursor(viewport()->mapFromGlobal(QCursor::pos()));
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Control) {
        updateCursor(viewport()->mapFromGlobal(QCursor::pos()));
    }
    QAbstractScrollArea::keyReleaseEvent(event);
}

} // namespace ps
