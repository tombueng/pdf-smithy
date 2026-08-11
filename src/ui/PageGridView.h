/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#pragma once

#include <QListView>
#include <QVector>

namespace ps {

class PageDelegate;

/**
 * The page grid: the heart of the window.
 *
 * One continuous zoom takes it from a sixty-page overview down to a single
 * readable page, which is why there is no separate "thumbnail view" and "page
 * view" to switch between. Ctrl+wheel and the status-bar slider drive the same
 * number.
 *
 * ## Fitting
 *
 * A free zoom alone is not enough, because the number a user actually wants is
 * almost never a number: it is "as wide as the window", "the whole page", "two
 * pages side by side". Those are the same continuous zoom with the width worked
 * out from the viewport instead of from the slider, which is why a fit is a
 * *mode* and not a preset. It survives resizing the window, opening a dock and
 * turning a page to landscape, all of which change the answer.
 *
 * Touching the slider or Ctrl+wheel drops back to Fit::Free, because a fit the
 * user has just zoomed away from is no longer a fit, and silently re-imposing
 * it on the next resize would feel like the window fighting back.
 */
class PageGridView : public QListView
{
    Q_OBJECT

public:
    /** What the zoom is being worked out from. */
    enum class Fit {
        Free, //!< whatever the slider or the wheel last said
        Width, //!< one page per row, as wide as the window
        Height, //!< one page as tall as the window, scrolling sideways if it must
        Page, //!< one whole page, both ways
        TwoPages, //!< two whole pages side by side
    };
    Q_ENUM(Fit)

    /** The range the zoom is clamped to; the status bar borrows it. */
    static constexpr int MinimumWidth = 60;
    static constexpr int MaximumWidth = 3200;

    /**
     * Pages across the window in the overview; see @ref showOverview().
     *
     * Eight because arranging pages is done by eye and by hand: the question
     * being answered is "which of these belongs where", and it cannot be
     * answered at all without several pages and their neighbours on screen at
     * once. Fewer than that and the grid is a slideshow.
     */
    static constexpr int OverviewColumns = 8;

    explicit PageGridView(QWidget *parent = nullptr);

    int thumbnailWidth() const;

    Fit fit() const { return m_fit; }

    /** Rows currently selected, ascending. */
    QVector<int> selectedRows() const;

    void selectRows(const QVector<int> &rows);

    /** Also shapes the cells for whatever the new model's pages look like. */
    void setModel(QAbstractItemModel *model) override;

public Q_SLOTS:
    void setThumbnailWidth(int pixels);
    void setFit(Fit mode);
    void zoomIn();
    void zoomOut();

    /**
     * Sizes the pages so that a windowful of them can be seen at once.
     *
     * What arranging pages is for, and what the grid is asked to open on: a
     * page at reading size is the document view's job, and coming here to
     * reorder a document only to find one page filling the window is arriving
     * at the wrong tool. @ref OverviewColumns across, or as many as fit once a
     * page has become too small to recognise.
     */
    void showOverview();

Q_SIGNALS:
    void thumbnailWidthChanged(int pixels);
    void fitChanged(Fit mode);

    /** Emitted on double click; the caller decides what "open" means. */
    void pageActivated(int row);

protected:
    void wheelEvent(QWheelEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

    // Drag and drop is handled here rather than left to QListView. In icon mode
    // Qt intercepts a drop that came from the view itself, shuffles the icon to
    // free coordinates and never tells the model, so the document never learns
    // that anything moved. Its drop-position logic is wrong for a grid anyway:
    // it only looks at a two-pixel margin at the top and bottom of an item,
    // while pages are arranged left to right.
    void startDrag(Qt::DropActions supportedActions) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dragLeaveEvent(QDragLeaveEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void paintEvent(QPaintEvent *event) override;

private:
    void applyGrid();

    /** Sets the width without disturbing the fit mode. */
    void applyWidth(int pixels);

    /** The width @p mode works out to for the viewport as it is now. */
    int widthForFit(Fit mode) const;

    /** The thumbnail width that puts @p columns pages across the viewport. */
    int widthForColumns(int columns) const;

    /** Re-imposes the fit; a no-op in Fit::Free. */
    void refit();

    /** Shapes the cells for the tallest page the document has. */
    void refreshCellRatio();

    /** Where a drop at @p pos would insert, as a row index. */
    int insertionIndexAt(const QPoint &viewportPos) const;

    bool acceptsDrag(const QMimeData *mime) const;

    PageDelegate *m_delegate;

    Fit m_fit = Fit::Free;

    /** Guards against a fit that resizes the view that triggered the fit. */
    bool m_fitting = false;

    /** Row the insertion marker is drawn before; -1 while nothing is dragged. */
    int m_dropRow = -1;
};

} // namespace ps
