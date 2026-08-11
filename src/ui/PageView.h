/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#pragma once

#include "core/RenderBackend.h"

#include <QAbstractScrollArea>
#include <QBasicTimer>
#include <QElapsedTimer>
#include <QHash>
#include <QImage>
#include <QPoint>
#include <QRectF>
#include <QSet>
#include <QString>
#include <QThreadPool>
#include <QVector>

#include <compare>
#include <memory>

class QMimeData;

namespace ps {

class Document;

/**
 * The document itself, at reading size, to be worked in rather than looked at.
 *
 * The page grid answers "which pages, in what order"; this answers "what does
 * this page say and how do I change it". They are different questions and this
 * is the one every other PDF editor is judged on, so it is the central view
 * whenever a document is open rather than a mode of the grid.
 *
 * ## Two modes, and why there are two
 *
 * In **View** the document behaves the way a reader does: text can be dragged
 * over, selected and copied, and form fields can be filled in. Nothing about
 * the document can be altered by accident, which is what makes it safe to hand
 * someone a program that can alter everything.
 *
 * In **Edit** the same page becomes editable in place: click into a line and
 * type, drag a picture, pull a handle. The mode is explicit rather than modal
 * by tool, because "am I about to change this document" is a question a user
 * should be able to answer by looking at the window, not by remembering which
 * button they last pressed.
 *
 * ## What lives here and what does not
 *
 * This class owns the geometry (where each page sits, at what scale, and what
 * is under the pointer) plus text selection, which is inseparable from it.
 * Everything else that draws on a page (form fields, editable text runs,
 * pictures with handles, comments) is an overlay that plugs in, so that adding
 * one is a new file rather than another thousand lines here.
 *
 * ## Reading has to move like reading
 *
 * The pages sit under one another in one continuous run, and the motion over
 * them is in pixels rather than in steps: a turn of the wheel raises a target
 * the view then glides towards, so the paragraph crossing the top of the window
 * can still be read while it travels. The target is raised by the next notch
 * the instant it arrives, which is the whole difference between smoothing the
 * motion and lagging behind the hand. An animation that has to finish before
 * it will listen again is worse than no animation at all. A touchpad, which
 * already reports the distance the fingers travelled, is passed straight
 * through for the same reason.
 *
 * ## Pages are rendered before anyone looks at them
 *
 * A render inside paintEvent() is a page turn that stutters, so renders are
 * asked for a few pages ahead of the reader and run on worker threads; the
 * paint draws what is already in hand and nothing else. Two things make that
 * safe rather than merely fast: the work is bounded in **bytes** rather than in
 * pages, because one A4 sheet at reading size is tens of megabytes; and a
 * render can never be inside a rasteriser that is being let go (see
 * @ref stopRendering(), which exists for a crash this program has already had).
 *
 * ## Why it is fast, in four moves
 *
 * A page render is about a sixth of a second on a dense A4 sheet at reading
 * size, and there is nothing to be done about that number: it is what the
 * rasteriser costs. Everything here is about not paying it more often than
 * necessary and never making the reader wait for it.
 *
 * **Several pages at once.** Renders run as wide as the machine allows. The
 * rasteriser cannot be shared between threads, so the backend keeps several
 * handles on each open file instead; eight pages that took 1.2 seconds one
 * after another take a third of a second side by side.
 *
 * **A proxy of every page, kept.** The first thing asked for is a tiny render
 * (two hundred pixels across, a few milliseconds) and it is never thrown away.
 * Any page without a current render draws its proxy stretched, so the document
 * is always *there*, softened at worst. Blank paper is what makes a viewer feel
 * slow even when it is fast.
 *
 * **What is held is reused.** Renders are kept with the size and the part of
 * the page they were made at, and a zoom does not throw them away: a picture
 * made for 400% scaled down to 200% is instant and looks right, which beats
 * rendering it again. A run of wheel notches moves the picture at once and
 * starts one render when the hand stops, not ten.
 *
 * **Only what is on the screen.** Past the point where a page is much larger
 * than the window, the visible band and a margin round it are rendered rather
 * than the whole sheet, and the paint touches only the pixels inside the
 * damaged rectangle. A page at 800% is a hundred megapixels, and both drawing
 * and rendering all of it is time spent on what nobody can see.
 *
 * ## Taking things off the page
 *
 * Text is selected the way text is selected everywhere: dragging runs from one
 * character to another, a double click takes the word, a triple click the line.
 * A box dragged with Ctrl held, or with @ref Tool::Rectangle chosen, takes
 * whatever falls inside it instead, which on a page is both the words and, if
 * the box lies over one, the picture, put on the clipboard as a picture and
 * read out of the file rather than scraped off the screen.
 *
 * ## The pointer says what will happen
 *
 * Everything above is worth nothing if the user cannot tell what a press is
 * about to do, so the shape under the pointer is answered in one place and in
 * the order the press itself is answered: the hand first, because someone
 * holding space has already said what they are doing; then any overlay that
 * will take the press, because it and not the view is what will happen; and
 * only then the view's own answer, which is a box, an I-beam over text, a
 * picture that can be taken, or the plain arrow over paper that does nothing.
 *
 * ## Cut, copy and paste
 *
 * The view holds the keys and the mode, and the overlays hold the things worth
 * copying, so cut(), copy() and paste() go the same way a mouse press does: the
 * overlays are asked in order and the first one that claims the command keeps
 * it. In View mode nothing is asked, because View mode does not change the
 * document. Cutting and pasting are refused by name through @ref refused()
 * rather than doing nothing, which is indistinguishable from being broken.
 */
class PageView : public QAbstractScrollArea
{
    Q_OBJECT

public:
    enum class Mode {
        View, //!< read, select, copy, fill in
        Edit, //!< change anything
    };
    Q_ENUM(Mode)

    /** How the pages are laid out under one another. */
    enum class Layout {
        Single, //!< one column
        Facing, //!< two columns, as a spread
    };
    Q_ENUM(Layout)

    /**
     * What the reading size is worked out from, rather than a number.
     *
     * The size a reader wants is almost never a number: it is "as wide as the
     * window" or "the whole page". Those are statements about the window, so
     * they are kept as one and answered again whenever the window changes:
     * a fit chosen before a panel opened is otherwise a fit in the menu and
     * nowhere on screen. Zooming by hand ends it, because a size the reader
     * has just set is not something to argue with.
     */
    enum class Fit {
        Free, //!< the number the reader last set
        Width, //!< the page, or the spread, as wide as the window
        Height, //!< the whole height of one page
        Page, //!< one whole page, or one whole spread when facing
    };
    Q_ENUM(Fit)

    /**
     * What dragging the left button over the page does, where no overlay claims
     * it and no modifier says otherwise.
     */
    enum class Tool {
        Text, //!< select words to copy them; what a reader expects
        Rectangle, //!< drag a box and take the text and the picture inside it
        Hand, //!< drag the page itself about
    };
    Q_ENUM(Tool)

    /**
     * A layer drawn over the pages, which may also take the mouse.
     *
     * Overlays are asked in order and the first one that claims an event keeps
     * it, so a form field on top of a text run wins. That is the order a reader
     * would expect, since the field is the thing you can see.
     */
    class Overlay
    {
    public:
        virtual ~Overlay() = default;

        /** Only asked to draw and to take events in these modes. */
        virtual bool appliesTo(Mode mode) const = 0;

        /**
         * Draws over @p row's page, clipped to @p pageRect.
         *
         * @p painter is in the viewport's own coordinates, which is what
         * PageView::fromPoints() answers in, so anything measured in points goes
         * through that and nothing here has to find the page's corner itself.
         */
        virtual void paint(QPainter &painter, int row, const QRect &pageRect) = 0;

        /** True when the overlay took the press; the view then leaves it alone. */
        virtual bool press(int row, const QPointF &pagePoint, Qt::MouseButtons buttons)
        {
            Q_UNUSED(row)
            Q_UNUSED(pagePoint)
            Q_UNUSED(buttons)
            return false;
        }

        virtual bool move(int row, const QPointF &pagePoint, Qt::MouseButtons buttons)
        {
            Q_UNUSED(row)
            Q_UNUSED(pagePoint)
            Q_UNUSED(buttons)
            return false;
        }

        virtual bool release(int row, const QPointF &pagePoint)
        {
            Q_UNUSED(row)
            Q_UNUSED(pagePoint)
            return false;
        }

        /** The pointer shape over @p pagePoint, or Qt::ArrowCursor to decline. */
        virtual Qt::CursorShape cursor(int row, const QPointF &pagePoint)
        {
            Q_UNUSED(row)
            Q_UNUSED(pagePoint)
            return Qt::ArrowCursor;
        }

        /** Puts whatever it has chosen on the clipboard. True when it did. */
        virtual bool copy() { return false; }

        /** Copies, then queues the removal of what it copied. True when it did. */
        virtual bool cut() { return false; }

        /**
         * Takes @p data onto @p row, which is the page the reader is looking at.
         *
         * The overlay decides what of @p data it understands; declining lets the
         * next one try, and a paste nobody claims is refused out loud.
         */
        virtual bool paste(const QMimeData *data, int row)
        {
            Q_UNUSED(data)
            Q_UNUSED(row)
            return false;
        }
    };

    explicit PageView(QWidget *parent = nullptr);
    ~PageView() override;

    void setDocument(Document *document);

    /**
     * Not owned, and it must outlive the view or be taken back with
     * @ref stopRendering() first, because pages are drawn on worker threads.
     */
    void setRenderBackend(RenderBackend *backend);

    /** Not owned. Added overlays are asked in the order they were added. */
    void addOverlay(Overlay *overlay);
    void removeOverlay(Overlay *overlay);

    /**
     * Whether @p overlay is one of the layers this view is currently asking.
     *
     * A layer that has been taken off draws nothing and answers nothing, and a
     * layer which has arranged for part of the page to be left out of the
     * renders (see @ref setOmittedFromRenders()) has to put that back the
     * moment it is no longer the one drawing it. Otherwise a mode that edits the
     * text of a form shows a form with no fields on it at all.
     */
    bool hasOverlay(const Overlay *overlay) const { return m_overlays.contains(const_cast<Overlay *>(overlay)); }

    Mode mode() const { return m_mode; }

    Layout layout() const { return m_layout; }

    Tool tool() const { return m_tool; }

    /** 1.0 draws one point as one pixel; the screen's own scaling is on top. */
    double zoom() const { return m_zoom; }

    Fit fit() const { return m_fit; }

    int currentPage() const { return m_current; }

    /** What is selected, joined the way the document spaces it. */
    QString selectedText() const;

    bool hasSelection() const;

    /** Where @p row's page is drawn, in the viewport's coordinates. */
    QRect pageRect(int row) const;

    /** How big @p row's page is in points, as the view shows it. */
    QSizeF pageSizePoints(int row) const;

    /** Points on the page (origin bottom left) to viewport pixels. */
    QPointF fromPoints(int row, const QPointF &points) const;
    QRectF fromPoints(int row, const QRectF &points) const;

    /** Viewport pixels to points on the page (origin bottom left). */
    QPointF toPoints(int row, const QPointF &pixels) const;

    /** The page under @p viewportPoint, or -1. */
    int pageAt(const QPoint &viewportPoint) const;

    /** Throws away cached renders and text for @p row, or for all of them. */
    void refresh(int row = -1);

    /**
     * Leaves part of the page off every render, for a layer that draws it itself.
     *
     * A form field is painted into the page bitmap, so an overlay that lets
     * someone drag one can only ever draw a second copy beside the first: the
     * original stays standing where the file still has it. Asking for a page
     * that never carried the fields is what makes the one being dragged the only
     * one there is.
     *
     * One setting for the whole view rather than one per row, because the change
     * throws every render away: doing it when the first field is grabbed would
     * blank the page in the middle of the gesture. Set it when the mode that
     * draws the fields is entered, and put it back when it is left.
     */
    void setOmittedFromRenders(RenderBackend::Request::Omit omit);

    RenderBackend::Request::Omit omittedFromRenders() const { return m_omit; }

    /**
     * True while any page is queued for drawing or being drawn.
     *
     * Here so that a window can say the document is still arriving, and so that
     * a test or a benchmark can wait for the view to settle instead of guessing
     * at a delay.
     */
    bool isRendering() const;

    /**
     * True when @p row is drawn from a render made at the size it is shown at.
     *
     * A page covered by its proxy, or by a render from a different zoom, is
     * *shown*: it is simply not yet sharp, which is exactly the distinction
     * worth measuring.
     */
    bool isSharp(int row) const;

public Q_SLOTS:
    void setMode(Mode mode);
    void setLayout(Layout layout);
    void setTool(Tool tool);
    void setZoom(double factor);
    void zoomIn();
    void zoomOut();

    /** Takes the reading size from the window from now on; see @ref Fit. */
    void setFit(Fit fit);

    void fitWidth();
    void fitHeight();
    void fitPage();
    void goToPage(int row);
    void selectAll();
    void copySelection();
    void clearSelection();

    /**
     * Waits for every render in flight and takes no more.
     *
     * The same contract as RenderCache::stop(), and it is here for the same
     * accident: this view is a child of the window, and a QObject child is
     * destroyed by its parent *after* the parent's own members, so a page still
     * being drawn outlives the rasteriser the window owns by exactly the time a
     * worker needs to call into it. The view asks the application to tell it
     * when it is leaving, so the ordinary way out is already covered; whoever
     * hands the backend over by other means calls this first.
     *
     * Rendering resumes only when a backend is given again.
     */
    void stopRendering();

    /** What Ctrl+X, Ctrl+C and Ctrl+V do, and what the window's menu should call. */
    void cut();
    void copy();
    void paste();

Q_SIGNALS:
    void modeChanged(Mode mode);
    void layoutChanged(Layout layout);
    void toolChanged(Tool tool);
    void zoomChanged(double factor);
    void fitChanged(Fit fit);
    void currentPageChanged(int row);

    /**
     * How far through the document the view stands, from 0 to 1.
     *
     * Emitted on every scroll rather than on every page change, which is the
     * difference between a thumbnail strip that travels with the reader and one
     * that jumps a page at a time and sits still in between. A reader going
     * slowly down a tall page gets no page change at all for several seconds,
     * and a panel that does not move in that time reads as broken.
     */
    void viewPositionChanged(double fraction);

    void selectionChanged();

    /**
     * A layer was added or taken off.
     *
     * Which layers are up is what a window changes when it changes what it is
     * for, and it changes several at once; a layer that has to know whether it
     * is still the one drawing something follows this rather than being told
     * separately by whoever rearranged them.
     */
    void overlaysChanged();

    /** Right-click, so the window can put its context menu up. */
    void contextMenuWanted(int row, const QPointF &pagePoint, const QPoint &globalPos);

    /**
     * What was asked for and could not be done, in words a person can act on.
     *
     * The window shows it. Same shape as ObjectOverlay::refused() so that both
     * can go to the same place.
     */
    void refused(const QString &reason);

protected:
    bool event(QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;
    void timerEvent(QTimerEvent *event) override;
    void scrollContentsBy(int dx, int dy) override;

private:
    friend class PageRenderTask;
    friend class PictureScanTask;

    /** Where one page sits in the whole scrolled document, in pixels. */
    struct Placement {
        QRect box; //!< in document coordinates, not viewport ones
    };

    /**
     * One end of a text selection: a word, and how far into that word.
     *
     * Comparing two of these compares page, then word, then character, which is
     * the order the document reads in, so which end of a drag comes first is a
     * comparison rather than four lines of special cases.
     */
    struct Caret {
        int page = -1;
        int word = -1;
        int character = 0;

        auto operator<=>(const Caret &) const = default;

        bool isValid() const { return page >= 0 && word >= 0; }
    };

    /** A picture as one page places it, with no pixels attached. */
    struct Picture {
        int page = 0; //!< in the source file, not in the assembled document
        QString name; //!< the /Im0 resource name
        QRectF placement; //!< display points, origin bottom left
        bool decodable = false;
    };

    /** What the worker threads may touch. Defined and explained in the .cpp. */
    struct RenderGate;

    /**
     * One page as it is held: the pixels, and what they are pixels of.
     *
     * Keeping the size and the part alongside the image is what lets a render
     * outlive the zoom it was made for. A picture is either exactly right, in
     * which case it is blitted, or it is a picture of the same page at another
     * size, in which case it is stretched. Either beats white paper.
     */
    struct Sheet {
        QImage image;

        /** Device pixels across the whole page these pixels were made at. */
        int width = 0;

        /** The part of that page, in its own pixels. Empty means all of it. */
        QRect tile;
    };

    /** What one worker came back with, whether or not it found anything. */
    struct Rendered {
        int row = -1;
        quint64 generation = 0;
        int width = 0;
        QRect tile;
        QImage image;
        QVector<RenderBackend::Word> words;
        bool gotWords = false;

        /** True for the small stand-in render rather than the accurate one. */
        bool proxy = false;

        /** True when the page really was drawn, so an empty image means empty. */
        bool tried = false;
    };

    void relayout();
    void updateCurrentPage();
    void emitPosition();

    /**
     * Sets the scale without ending the fit, keeping the reader's place.
     *
     * What @ref setZoom does once it has recorded that the number came from the
     * reader rather than from the window.
     */
    void applyZoom(double factor);

    /** Works @ref m_fit out against the window as it is now; a no-op in Free. */
    void applyFit();

    /** Asks for the pages about to be needed, nearest first, and drops the rest. */
    void ensureRenders();
    void requestRender(int row, bool visible);
    void requestProxy(int row);

    /** Device pixels the rasteriser must produce for @p row as it is drawn now. */
    int renderWidthOf(int row) const;

    /** How tall @p row's render is, in the same pixels @ref renderWidthOf counts. */
    int renderHeightOf(int row) const;

    /**
     * The part of @p row worth rendering, or an empty rectangle for the lot.
     *
     * @p margin is how much of a windowful is added round the visible band, as
     * a fraction: 0 asks for exactly what can be seen, which is what a held
     * render has to cover, and something larger is what is actually asked for
     * so that a small scroll does not start a new one.
     *
     * Always the whole page below @ref WholePageLimit pixels, and always the
     * whole page for a page the organiser has turned, because a tile would have
     * to be mapped through the rotation, and a turned page is rare enough that the
     * simpler answer is the better one.
     */
    QRect bandOf(int row, double margin) const;

    /** True when what is held for @p row is the picture the view wants now. */
    bool sheetSatisfies(int row) const;

    /** Called on the GUI thread with whatever a worker came back with. */
    void applyRender(const Rendered &result);
    void applyPictures(int sourceId, const QVector<Picture> &pictures);

    /** Keeps what is held under @ref RenderBudget, never dropping a visible page. */
    void trimRenders();

    /** Keeps the proxies under their own, much smaller, ceiling. */
    void trimProxies();

    /** Forgets every render and every proxy. */
    void invalidateRenders();

    const QVector<RenderBackend::Word> &wordsOf(int row);

    /** The words already in hand, or null. For the paths that must not block. */
    const QVector<RenderBackend::Word> *cachedWords(int row) const;

    /** The pictures @p row places, turned as the view shows the page. */
    QVector<Picture> picturesOf(int row);

    /** The same, reading the file here and now if that is what it takes. */
    QVector<Picture> picturesOfNow(int row);
    void requestPictures(int sourceId);

    /** The word nearest @p pagePoint on @p row, or -1. */
    int wordAt(int row, const QPointF &pagePoint) const;

    /** The word @p pagePoint is actually inside, or -1. No nearest-word guess. */
    int wordUnder(int row, const QPointF &pagePoint) const;

    /** The picture @p pagePoint lies on, as an index into picturesOf(), or -1. */
    int pictureAt(int row, const QPointF &pagePoint);

    /** Where in the document @p pagePoint points, to the character. */
    Caret caretAt(int row, const QPointF &pagePoint) const;

    /** Marks everything between the anchor and the point now under the mouse. */
    void extendSelectionTo(int row, const QPointF &pagePoint);

    /** Takes the whole line the point sits on, which is what a third click means. */
    void selectLineAt(int row, const QPointF &pagePoint);

    bool hasTextSelection() const;

    /** The words inside @p pageRect, laid out as they read. */
    QString textInside(int row, const QRectF &pageRect) const;

    /** The picture the box lies over, read out of the file. Null when there is none. */
    QImage pictureInside(int row, const QRectF &pageRect, QString *why);

    /** Puts the box's text and picture on the clipboard. True when it had either. */
    bool copyBox();

    /** Offers the selected text to whatever middle-click will paste next. */
    void publishSelection();

    /** The pointer shape for where the mouse is now, asking the overlays first. */
    void updateCursor(const QPoint &viewportPoint);

    // ── The motion ────────────────────────────────────────────────────────

    void glideBy(double pixels);
    void glideTo(double target);
    void stopGlide();
    void panBy(const QPoint &delta);

    /**
     * Zooms so that whatever is under @p anchor stays under it.
     *
     * What the wheel does, and what every zoom made with a pointer should do:
     * a zoom that jumps to the top of the page instead loses the paragraph the
     * reader was looking at, ten times over in a run of notches.
     */
    void zoomAround(double factor, const QPoint &anchor);

    QSizeF pageSizeOf(int row) const;

    Document *m_document = nullptr;
    RenderBackend *m_backend = nullptr;
    QVector<Overlay *> m_overlays;

    Mode m_mode = Mode::View;

    /** What the pages are drawn without; see @ref setOmittedFromRenders(). */
    RenderBackend::Request::Omit m_omit = RenderBackend::Request::Omit::Nothing;

    Layout m_layout = Layout::Single;
    Tool m_tool = Tool::Text;
    double m_zoom = 1.0;
    Fit m_fit = Fit::Free;

    /** Guards against a fit that resizes the window the fit was worked out from. */
    bool m_fitting = false;

    int m_current = 0;

    QVector<Placement> m_places;
    QSize m_contents;

    // ── Rendering ahead ───────────────────────────────────────────────────

    QThreadPool m_pool;

    /**
     * Reading where the pictures on a page sit, kept off the render pool.
     *
     * One file's worth of content streams takes seconds on a large document,
     * and a page the reader is waiting for must never queue behind it.
     */
    QThreadPool m_scanPool;

    std::shared_ptr<RenderGate> m_gate;

    /** Rendered pages, keyed by row, each remembering what size it is. */
    QHash<int, Sheet> m_sheets;

    /**
     * A small render of every page that has been looked at, kept.
     *
     * These are what stands between the reader and white paper: a few hundred
     * kilobytes a page against tens of megabytes for the real thing, so they
     * are worth holding for a document's whole session and are dropped only
     * when @ref ProxyBudget is reached, farthest from the reader first.
     */
    QHash<int, QImage> m_proxies;

    QHash<int, QVector<RenderBackend::Word>> m_words;

    /** Rows asked for and not yet answered, so nothing is asked for twice. */
    QSet<int> m_pending;
    QSet<int> m_pendingProxy;

    /**
     * Rows the rasteriser had nothing to give.
     *
     * Without this a page it cannot draw is asked for again by the very repaint
     * its empty answer triggered, which is a loop that spins a core for as long
     * as the page is on screen. Forgotten whenever anything about the document
     * or the zoom changes, so a page is never written off for good.
     */
    QSet<int> m_barren;

    /** The run of pages worth having in hand, visible ones included. */
    int m_wantFirst = 0;
    int m_wantLast = -1;

    /** Which way the reader is going, so the pages ahead are the ones fetched. */
    int m_travel = 1;

    /** Picture placements, keyed by the source file and the page inside it. */
    QHash<quint64, QVector<Picture>> m_pictures;
    QSet<int> m_scanAsked;
    QSet<int> m_scanDone;

    // ── What is chosen ────────────────────────────────────────────────────

    Caret m_anchor;
    Caret m_focus;
    bool m_selecting = false;

    /** A box dragged over one page, in that page's points. Empty page means none. */
    int m_boxPage = -1;
    QRectF m_boxPoints;
    QPointF m_boxAnchor;
    bool m_draggingBox = false;

    /** Two clicks take a word and three take a line, which Qt does not count for us. */
    QElapsedTimer m_clickAge;
    QPoint m_clickWhere;
    int m_clicks = 0;

    // ── The motion and the hand ───────────────────────────────────────────

    QBasicTimer m_glide;
    double m_glideTarget = 0.0;
    bool m_gliding = false;

    /**
     * A run of zoom notches is one zoom.
     *
     * The picture moves on every notch (scaled from what is already in hand, so
     * it costs nothing) and the render is put off until the hand stops.
     * Ten notches used to mean ten full renders of which nine were thrown away
     * before anyone saw them. The first notch of a run is not delayed: only a
     * second one arriving on the heels of the first says a run is under way.
     */
    QBasicTimer m_zoomSettle;
    QElapsedTimer m_zoomAge;
    bool m_zooming = false;

    /** Wheel travel not yet worth a notch; see wheelEvent(). */
    int m_zoomNotches = 0;

    bool m_panning = false;
    bool m_spaceHeld = false;
    QPoint m_panFrom;

    /** The overlay that took the current press, if any. */
    Overlay *m_grabbed = nullptr;
    int m_grabbedRow = -1;
};

} // namespace ps
