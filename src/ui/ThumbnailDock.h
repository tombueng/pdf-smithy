/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#pragma once

#include <QDockWidget>
#include <QElapsedTimer>

class QAbstractItemModel;
class QListView;
class QScrollBar;
class QSlider;

namespace ps {

class PageDelegate;

/**
 * One narrow column of page thumbnails, beside the document.
 *
 * Every reader has this and the window did not, which left "where am I in this
 * file" answerable only by the page number in the status bar. A number tells
 * you nothing about a two-hundred-page report; a strip of pictures tells you
 * that the chapter you want is the one with the table in it, four thumbnails
 * down.
 *
 * It shows the same pages, through the same PageModel and PageDelegate, as the
 * grid in the middle of the window. Sharing the model is not tidiness: the
 * model holds the render cache, so a page drawn once for the grid is drawn once
 * for the strip as well, and page sizes measured for one are already measured
 * for the other.
 *
 * ## Which way it runs
 *
 * A panel down the side of the window is read by running an eye down it, and
 * one along the top by running an eye across it, so where it has been put
 * decides which way the cells flow, which scrollbar carries the travel, and
 * which way round every piece of arithmetic below is done. A floating panel is
 * in no dock area at all and there is nothing to ask, so it goes by the shape
 * the user dragged it into and is asked again each time that shape changes.
 *
 * The cells wrap either way round. With the thumbnails small and the panel
 * wide, several pages sit abreast rather than one to a line; how many is
 * whatever the cell size and the room divide into each other, so it follows the
 * zoom slider and the panel's edge without either of them naming a number.
 *
 * ## What keeps it cheap on a four-hundred-page document
 *
 * Nothing here walks the document. The list draws only the cells inside its own
 * viewport, and a cell asks the model for its thumbnail while painting, so a
 * page that has never scrolled into view is never rendered, and RenderCache
 * never hears of it. Uniform item sizes mean the layout is arithmetic on one
 * measured cell rather than a size hint per row, so scrolling to page 380 does
 * not touch pages 1 to 379; wrapping costs nothing there, because the view
 * keeps using the one cached measurement whether it is filling lines or a
 * column. The one thing that would have walked the whole document is working
 * out what shape the cells should be, because that asks the render backend how
 * big each page is and takes its lock to do so; a sample of the first few pages
 * settles that instead.
 *
 * ## Staying in step with the document view
 *
 * Two things have to stay in step, and they are not the same thing. *Which*
 * page is being read is a whole number that changes rarely: followPage()
 * carries it, and it is what the mark is drawn from. *Where the reader stands*
 * is a continuous quantity that changes on every pixel of scroll:
 * followPosition() carries that, and it is what the strip's own scrollbar is
 * driven from. Driving the travel from the page number is what made the strip
 * look broken: a reader going slowly down a tall page at reading zoom turns no
 * page for many seconds, so the strip stood perfectly still while the document
 * moved, and then lurched by a screenful when the page finally did turn.
 *
 * Neither slot is a setter with a signal on it. The window calls them because
 * the document view has scrolled, and a pageRequested() coming back out would
 * scroll that view again from under the reader, a loop that shows up as the
 * page jumping while it is being dragged. m_following stops that direction; the
 * guard around the user's own scrolling of the strip stops the other one.
 */
class ThumbnailDock : public QDockWidget
{
    Q_OBJECT

public:
    /**
     * The range the thumbnail size is clamped to.
     *
     * The floor is where a page stops being a picture of itself and becomes a
     * grey rectangle, which is no use for navigating by; the ceiling is where a
     * sidebar has stopped being a sidebar.
     */
    static constexpr int MinimumThumbnailWidth = 60;
    static constexpr int MaximumThumbnailWidth = 320;

    explicit ThumbnailDock(QWidget *parent = nullptr);
    ~ThumbnailDock() override;

    /** The window's page model; not owned, and shared with the grid. */
    void setModel(QAbstractItemModel *model);

    int thumbnailWidth() const;

    /**
     * Puts the panel away for a mode that shows every page anyway, or brings it
     * back exactly as the user had it.
     *
     * Not the same as hide() and show(). What is remembered is the user's own
     * answer to "do I want this panel", so a strip they had closed themselves
     * stays closed on the way out; otherwise every visit to that mode would
     * hand them back a panel they had already dismissed.
     */
    void setTemporarilyHidden(bool hidden);

public Q_SLOTS:
    /** Widens or narrows the column, and the dock along with it. */
    void setThumbnailWidth(int pixels);

    /** Marks @p row, without asking to go there and without shoving the strip. */
    void followPage(int row);

    /**
     * Travels with the document view, which reports @p fraction on every scroll.
     *
     * 0 is the top of the document and 1 the bottom, the same numbers the
     * document view's own scrollbar runs between.
     */
    void followPosition(double fraction);

Q_SIGNALS:
    /** The user picked a page out of the strip. */
    void pageRequested(int row);

    /** So that whatever feeds the strip can render at the size it draws at. */
    void thumbnailWidthChanged(int pixels);

protected:
    void resizeEvent(QResizeEvent *event) override;
    void showEvent(QShowEvent *event) override;

private:
    /**
     * Which way the strip should run, given where it has been put.
     *
     * The dock areas answer for themselves: down the side means read downwards,
     * along the top or the bottom means read across. A floating panel belongs
     * to no area (dockWidgetArea() says NoDockWidgetArea for one and there is
     * nothing else to ask), so it goes by its own shape instead.
     */
    Qt::Orientation orientationForPlace() const;

    /** Turns the strip if where it now is says it should run the other way. */
    void updateOrientation();

    /** Points the flow, the travel and the mark along the current orientation. */
    void applyOrientation();

    /** Sizes the cells without disturbing the dock the strip sits in. */
    void applyWidth(int pixels);

    /** Asks the main window for the room the new cell size needs. */
    void resizeDockToFit();

    /** Keeps the strip from being squeezed thinner across than one thumbnail. */
    void updateStripFloor();

    /** Shapes the cells for the pages, measuring only a few of them. */
    void refreshCellRatio();

    /** Takes the mark off a page the document no longer has, and reshapes. */
    void onRowsChanged();

    /** Forgets everything about a document that is no longer the one shown. */
    void onModelReset();

    /** Keeps the mark on the same page when rows appear above it, or return. */
    void onRowsInserted(int first, int last);

    /** Remembers which page the mark is on while it is out of the model. */
    void onRowsAboutToBeRemoved(int first, int last);

    /** Keeps the mark on the same page when rows vanish above it. */
    void onRowsRemoved(int first, int last);

    /** Moves the "you are here" mark, takes the current item with it, repaints. */
    void markRow(int row);

    /** Puts the strip where the last reported position says it should be. */
    void trackPosition();

    /** Brings the marked page back on screen if something put it off it. */
    void keepMarkVisible();

    /** Travels @p offset from the start of the strip, never past the mark. */
    void scrollStripTo(int offset);

    /** @p offset, moved as little as needed to leave the marked cell on screen. */
    int clampToMark(int offset) const;

    /** The scrollbar the strip travels on: the one it is laid out along. */
    QScrollBar *travelBar() const;

    /** How much of the strip is on screen, measured the way the strip runs. */
    int stripExtent() const;

    /** True while the strip belongs to the user rather than to the document. */
    bool userIsSteering() const;

    /** Hands the strip to the user for a moment, because they just moved it. */
    void noteUserSteering();

    QListView *m_list;

    /** A PageDelegate that also draws the mark: MarkingDelegate, in the .cpp. */
    PageDelegate *m_delegate;

    QSlider *m_size;

    /** Which way the strip runs; orientationForPlace() says what decides it. */
    Qt::Orientation m_orientation = Qt::Vertical;

    /** Where the window last said the panel sits; none of them while it floats. */
    Qt::DockWidgetArea m_area = Qt::NoDockWidgetArea;

    /** Set while a mode that shows every page anyway has the panel put away. */
    bool m_temporarilyHidden = false;

    /** Whether the user had the panel open at the moment a mode took it away. */
    bool m_wanted = true;

    /** Set while the panel is hiding or showing itself rather than being asked to. */
    bool m_changingVisibility = false;

    /** The page the document view is showing, or -1 before it has said. */
    int m_currentRow = -1;

    /** Set while following the document view, so that doing so asks for nothing. */
    bool m_following = false;

    /** How far through the document the view last said it stood, from 0 to 1. */
    double m_position = 0.0;

    /** Since the user last moved the strip themselves; invalid if they never have. */
    QElapsedTimer m_steered;

    /**
     * Which page the mark was on while that page is out of the model.
     *
     * A page move reaches the model as a removal and then an insertion of the
     * very same pages, so a mark that only knew a row number would come out of
     * it pointing at whatever slid into that row. -1 when nothing is in flight.
     */
    int m_orphanSource = -1;
    int m_orphanPage = -1;
};

} // namespace ps
