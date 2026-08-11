/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#pragma once

#include <QStyledItemDelegate>

namespace ps {

/**
 * Draws one page as a sheet of paper on the grid.
 *
 * All geometry derives from a single thumbnail width so the zoom slider has
 * exactly one number to drive, and cells stay uniform regardless of whether the
 * page inside is portrait, landscape or square.
 *
 * How much room a cell reserves is the *document's* business, not A4's: a deck
 * of landscape slides in cells shaped for portrait paper would waste half its
 * height and could never be made to fill the window. So the view hands down the
 * tallest ratio it can see, and every cell is shaped for that.
 */
class PageDelegate : public QStyledItemDelegate
{
    Q_OBJECT

public:
    explicit PageDelegate(QObject *parent = nullptr);

    int thumbnailWidth() const { return m_thumbnailWidth; }
    void setThumbnailWidth(int pixels);

    /** Height over width of the tallest page, which every cell is shaped for. */
    qreal cellRatio() const { return m_cellRatio; }
    void setCellRatio(qreal heightOverWidth);

    /** Cell size the view should use for its grid. */
    QSize gridSize() const;

    /**
     * The thumbnail width whose cell is @p pixels wide, and likewise tall.
     *
     * The inverse of gridSize(), and here rather than in the view because the
     * margins and the label strip are this class's private arithmetic. A fit
     * that guessed at them would be a few pixels out, which is exactly enough
     * for the page to wrap to the next row and the fit to look broken.
     */
    int widthForCellWidth(int pixels) const;
    int widthForCellHeight(int pixels) const;

    /** Where the paper sits inside a cell, used for drop indicators. */
    QRect sheetRect(const QRect &cell, qreal aspectRatio) const;

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;

private:
    void paintPlaceholder(QPainter *painter, const QRect &sheet, const QStyleOptionViewItem &option) const;
    void paintBadge(QPainter *painter, const QRect &cell, const QModelIndex &index,
                    const QStyleOptionViewItem &option) const;

    int m_thumbnailWidth = 180;
    qreal m_cellRatio = 297.0 / 210.0;
};

} // namespace ps
