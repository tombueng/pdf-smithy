/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "PageDelegate.h"

#include "PageModel.h"

#include <QApplication>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>

namespace ps {

namespace {

/** Breathing room around the sheet inside its cell. */
constexpr int CellMargin = 10;

/** Height of the page-number strip below the sheet. */
constexpr int LabelHeight = 22;

constexpr int CornerRadius = 4;

} // namespace

PageDelegate::PageDelegate(QObject *parent)
    : QStyledItemDelegate(parent)
{
}

void PageDelegate::setThumbnailWidth(int pixels)
{
    // The ceiling is what a page filling a 4K screen needs. It is not free
    // (one A4 sheet at this width is some fifty megabytes in the render cache,
    // so only a handful are kept), but a "fit width" that stopped short of the
    // window would be a fit in name only.
    m_thumbnailWidth = std::clamp(pixels, 40, 3200);
}

void PageDelegate::setCellRatio(qreal heightOverWidth)
{
    // Guarded rather than trusted: a page with a nonsense MediaBox would
    // otherwise turn every cell in the grid into a strip a mile high.
    m_cellRatio = std::clamp(heightOverWidth, 0.2, 6.0);
}

QSize PageDelegate::gridSize() const
{
    const int sheetHeight = static_cast<int>(std::lround(m_thumbnailWidth * m_cellRatio));
    return QSize(m_thumbnailWidth + 2 * CellMargin, sheetHeight + 2 * CellMargin + LabelHeight);
}

int PageDelegate::widthForCellWidth(int pixels) const
{
    return pixels - 2 * CellMargin;
}

int PageDelegate::widthForCellHeight(int pixels) const
{
    const qreal sheetHeight = pixels - 2 * CellMargin - LabelHeight;
    return static_cast<int>(std::floor(sheetHeight / m_cellRatio));
}

QRect PageDelegate::sheetRect(const QRect &cell, qreal aspectRatio) const
{
    // The area the tallest page in the document fills; everything else is
    // fitted inside it, so that one odd page among five hundred does not shove
    // the whole grid around.
    const QRect box(cell.left() + CellMargin, cell.top() + CellMargin, cell.width() - 2 * CellMargin,
                    cell.height() - 2 * CellMargin - LabelHeight);

    if (aspectRatio <= 0.0 || box.width() <= 0 || box.height() <= 0) {
        return box;
    }

    int width = box.width();
    int height = static_cast<int>(std::lround(width / aspectRatio));
    if (height > box.height()) {
        height = box.height();
        width = static_cast<int>(std::lround(height * aspectRatio));
    }

    // Bottom-aligned: pages of different heights then stand on one line, the
    // way a stack of paper actually looks.
    return QRect(box.left() + (box.width() - width) / 2, box.bottom() - height, width, height);
}

QSize PageDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    Q_UNUSED(option)
    Q_UNUSED(index)
    return gridSize();
}

void PageDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);

    const QPalette &palette = option.palette;
    const bool selected = option.state & QStyle::State_Selected;
    const bool hovered = option.state & QStyle::State_MouseOver;

    // Selection is drawn as a soft plate behind the sheet rather than a tint on
    // top of it, so the page content keeps its true colours.
    if (selected || hovered) {
        QColor plate = palette.color(QPalette::Highlight);
        plate.setAlpha(selected ? 90 : 35);
        painter->setPen(Qt::NoPen);
        painter->setBrush(plate);
        painter->drawRoundedRect(option.rect.adjusted(2, 2, -2, -2), CornerRadius + 2, CornerRadius + 2);
    }

    const qreal aspect = index.data(PageModel::AspectRatioRole).toReal();
    const QRect sheet = sheetRect(option.rect, aspect > 0 ? aspect : 1.0 / m_cellRatio);

    // A hint of a shadow to lift the paper off the background. Cheap, three
    // strokes, no blur; anything more starts to cost frames while scrolling.
    for (int i = 3; i >= 1; --i) {
        QColor shadow(0, 0, 0, 12);
        painter->setPen(Qt::NoPen);
        painter->setBrush(shadow);
        painter->drawRoundedRect(sheet.adjusted(-i + 1, i, i - 1, i), CornerRadius, CornerRadius);
    }

    const QImage thumbnail = index.data(PageModel::ThumbnailRole).value<QImage>();

    QPainterPath clip;
    clip.addRoundedRect(sheet, CornerRadius, CornerRadius);
    painter->setClipPath(clip);

    if (thumbnail.isNull()) {
        paintPlaceholder(painter, sheet, option);
    } else {
        painter->fillRect(sheet, Qt::white);
        painter->drawImage(sheet, thumbnail);
    }
    painter->setClipping(false);

    // Outline last, so it sits cleanly on top of the content edge.
    QColor border = palette.color(QPalette::WindowText);
    border.setAlpha(selected ? 140 : 60);
    painter->setPen(QPen(border, selected ? 2 : 1));
    painter->setBrush(Qt::NoBrush);
    painter->drawRoundedRect(sheet, CornerRadius, CornerRadius);

    paintBadge(painter, option.rect, index, option);

    painter->restore();
}

void PageDelegate::paintPlaceholder(QPainter *painter, const QRect &sheet, const QStyleOptionViewItem &option) const
{
    // Until the render arrives: a blank sheet with faint rules, which reads as
    // "a page is coming" rather than as an error.
    painter->fillRect(sheet, option.palette.color(QPalette::Base));

    QColor rule = option.palette.color(QPalette::WindowText);
    rule.setAlpha(22);
    painter->setPen(QPen(rule, 1));

    const int inset = sheet.width() / 8;
    const int step = std::max(6, sheet.height() / 12);
    for (int y = sheet.top() + step; y < sheet.bottom() - step / 2; y += step) {
        painter->drawLine(sheet.left() + inset, y, sheet.right() - inset, y);
    }
}

void PageDelegate::paintBadge(QPainter *painter, const QRect &cell, const QModelIndex &index,
                              const QStyleOptionViewItem &option) const
{
    const QString number = QString::number(index.data(PageModel::PageNumberRole).toInt());
    const bool selected = option.state & QStyle::State_Selected;

    QFont font = option.font;
    font.setPointSizeF(std::max(7.5, font.pointSizeF() - 0.5));
    painter->setFont(font);

    const QFontMetrics metrics(font);
    const int textWidth = metrics.horizontalAdvance(number);
    const int pillWidth = std::max(22, textWidth + 14);
    const int pillHeight = std::min(LabelHeight - 4, metrics.height() + 4);

    const QRect pill((cell.left() + cell.right() - pillWidth) / 2,
                     cell.bottom() - LabelHeight + (LabelHeight - pillHeight) / 2, pillWidth, pillHeight);

    painter->setPen(Qt::NoPen);
    painter->setBrush(selected ? option.palette.color(QPalette::Highlight)
                               : option.palette.color(QPalette::Window).darker(108));
    painter->drawRoundedRect(pill, pillHeight / 2.0, pillHeight / 2.0);

    painter->setPen(selected ? option.palette.color(QPalette::HighlightedText)
                             : option.palette.color(QPalette::WindowText));
    painter->drawText(pill, Qt::AlignCenter, number);
}

} // namespace ps
