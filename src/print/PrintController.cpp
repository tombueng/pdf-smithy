/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "PrintController.h"

#include "core/Document.h"
#include "core/PageRange.h"
#include "core/RenderBackend.h"

#include <QPainter>
#include <QPrinter>
#include <QTransform>

#include <KLocalizedString>

#include <algorithm>
#include <cmath>

namespace ps {

namespace {

/** Columns and rows for an N-up layout. */
QSize gridFor(PrintController::Layout layout)
{
    switch (layout) {
    case PrintController::Layout::Normal:
        return { 1, 1 };
    case PrintController::Layout::TwoUp:
    case PrintController::Layout::Booklet:
        return { 2, 1 };
    case PrintController::Layout::FourUp:
        return { 2, 2 };
    case PrintController::Layout::SixUp:
        return { 3, 2 };
    case PrintController::Layout::NineUp:
        return { 3, 3 };
    case PrintController::Layout::SixteenUp:
        return { 4, 4 };
    }
    return { 1, 1 };
}

} // namespace

PrintController::PrintController(QObject *parent)
    : QObject(parent)
{
}

void PrintController::setDocument(Document *document)
{
    m_document = document;
}

void PrintController::setRenderBackend(RenderBackend *backend)
{
    m_backend = backend;
}

int PrintController::pagesPerSheet(Layout layout)
{
    const QSize grid = gridFor(layout);
    return grid.width() * grid.height();
}

QString PrintController::layoutName(Layout layout)
{
    switch (layout) {
    case Layout::Normal:
        return i18nc("@item print layout", "One page per sheet");
    case Layout::TwoUp:
        return i18nc("@item print layout", "2 pages per sheet");
    case Layout::FourUp:
        return i18nc("@item print layout", "4 pages per sheet");
    case Layout::SixUp:
        return i18nc("@item print layout", "6 pages per sheet");
    case Layout::NineUp:
        return i18nc("@item print layout", "9 pages per sheet");
    case Layout::SixteenUp:
        return i18nc("@item print layout", "16 pages per sheet");
    case Layout::Booklet:
        return i18nc("@item print layout", "Booklet (fold in the middle)");
    }
    return {};
}

QVector<int> PrintController::impose(const Options &options, QString *error) const
{
    if (!m_document || m_document->pageCount() == 0) {
        if (error) {
            *error = i18n("There is nothing to print.");
        }
        return {};
    }

    QVector<int> pages = PageRange::parse(options.range, m_document->pageCount(), error);
    if (pages.isEmpty()) {
        return {};
    }

    if (options.subset != Subset::All) {
        QVector<int> filtered;
        for (int i = 0; i < pages.size(); ++i) {
            // Odd and even refer to the position in the job, not to the page
            // number in the file, which is what makes manual duplex line up
            // after the stack is turned over.
            const bool odd = (i % 2) == 0;
            if ((options.subset == Subset::OddOnly) == odd) {
                filtered.append(pages.at(i));
            }
        }
        pages = filtered;
    }

    if (options.reverseOrder) {
        std::reverse(pages.begin(), pages.end());
    }

    if (options.layout != Layout::Booklet) {
        // Pad the last sheet so the grid stays regular.
        const int perSheet = pagesPerSheet(options.layout);
        while (perSheet > 1 && pages.size() % perSheet != 0) {
            pages.append(-1);
        }
        return pages;
    }

    // A booklet is folded once, so every sheet side carries two pages and the
    // total has to be a multiple of four. The classic saddle-stitch order puts
    // the last page next to the first.
    while (pages.size() % 4 != 0) {
        pages.append(-1);
    }

    QVector<int> booklet;
    booklet.reserve(pages.size());
    int front = 0;
    int back = pages.size() - 1;
    while (front < back) {
        booklet.append(pages.at(back)); // left half of the front side
        booklet.append(pages.at(front)); // right half of the front side
        ++front;
        --back;
        booklet.append(pages.at(front)); // left half of the back side
        booklet.append(pages.at(back)); // right half of the back side
        ++front;
        --back;
    }
    return booklet;
}

int PrintController::sheetCount(const Options &options) const
{
    const QVector<int> imposed = impose(options, nullptr);
    if (imposed.isEmpty()) {
        return 0;
    }
    const int perSheet = pagesPerSheet(options.layout);
    return (imposed.size() + perSheet - 1) / perSheet;
}

bool PrintController::paintSheet(QPainter *painter, const QVector<int> &imposed, int sheetIndex, const Options &options,
                                 const QRectF &sheetRect, int resolutionDpi)
{
    if (!m_document || !m_backend) {
        return false;
    }

    const QSize grid = gridFor(options.layout);
    const int perSheet = grid.width() * grid.height();

    const qreal marginPx = options.marginMm / 25.4 * resolutionDpi;
    const QRectF usable = sheetRect.adjusted(marginPx, marginPx, -marginPx, -marginPx);
    if (usable.width() <= 0 || usable.height() <= 0) {
        return false;
    }

    const qreal cellWidth = usable.width() / grid.width();
    const qreal cellHeight = usable.height() / grid.height();

    for (int slot = 0; slot < perSheet; ++slot) {
        const int index = sheetIndex * perSheet + slot;
        if (index >= imposed.size()) {
            break;
        }
        const int pageIndex = imposed.at(index);
        if (pageIndex < 0) {
            continue; // deliberately blank
        }

        const int column = slot % grid.width();
        const int row = slot / grid.width();
        const QRectF cell(usable.left() + column * cellWidth, usable.top() + row * cellHeight, cellWidth, cellHeight);

        const PageRef ref = m_document->pageAt(pageIndex);
        QSizeF pageSize = m_backend->pageSizePoints(ref.sourceId, ref.sourcePage);
        if (pageSize.isEmpty()) {
            continue;
        }
        if (ref.rotation == 90 || ref.rotation == 270) {
            pageSize.transpose();
        }

        // Work out the drawn size first, then render at exactly that many
        // pixels. Rendering at a fixed DPI and scaling afterwards is what makes
        // printed output look soft.
        qreal scale = 1.0;
        const qreal pageWidthPx = pageSize.width() / 72.0 * resolutionDpi;
        const qreal pageHeightPx = pageSize.height() / 72.0 * resolutionDpi;

        switch (options.scaling) {
        case Scaling::ActualSize:
            scale = 1.0;
            break;
        case Scaling::Custom:
            scale = std::clamp(options.customScalePercent, 1, 1000) / 100.0;
            break;
        case Scaling::FitToPage:
            scale = std::min(cell.width() / pageWidthPx, cell.height() / pageHeightPx);
            break;
        case Scaling::ShrinkToFit:
            scale = std::min({ 1.0, cell.width() / pageWidthPx, cell.height() / pageHeightPx });
            break;
        }

        const int targetWidth = std::max(1, static_cast<int>(std::lround(pageWidthPx * scale)));
        QImage image = m_backend->renderPage(ref.sourceId, ref.sourcePage, targetWidth);
        if (image.isNull()) {
            continue;
        }
        if (ref.rotation != 0) {
            image = image.transformed(QTransform().rotate(ref.rotation), Qt::SmoothTransformation);
        }
        if (options.grayscale) {
            image = image.convertToFormat(QImage::Format_Grayscale8);
        }

        const QRectF target(cell.left() + (cell.width() - image.width()) / 2.0,
                            cell.top() + (cell.height() - image.height()) / 2.0, image.width(), image.height());
        painter->drawImage(target, image);

        if (options.pageBorders) {
            painter->save();
            painter->setPen(QPen(Qt::gray, 1));
            painter->setBrush(Qt::NoBrush);
            painter->drawRect(target);
            painter->restore();
        }
    }

    return true;
}

bool PrintController::print(QPrinter *printer, const Options &options, QString *error)
{
    if (!printer) {
        if (error) {
            *error = i18n("No printer was selected.");
        }
        return false;
    }

    const QVector<int> imposed = impose(options, error);
    if (imposed.isEmpty()) {
        return false;
    }

    const int perSheet = pagesPerSheet(options.layout);
    const int sheets = (imposed.size() + perSheet - 1) / perSheet;

    // A booklet is printed on landscape sheets whatever the pages look like,
    // because it is folded across the short edge.
    if (options.layout == Layout::Booklet || options.layout == Layout::TwoUp) {
        printer->setPageOrientation(QPageLayout::Landscape);
    }

    QPainter painter;
    if (!painter.begin(printer)) {
        if (error) {
            *error = i18n("The printer could not be opened.");
        }
        return false;
    }
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

    const int dpi = printer->resolution();
    const QRectF sheetRect = printer->pageLayout().paintRectPixels(dpi);

    for (int sheet = 0; sheet < sheets; ++sheet) {
        if (sheet > 0 && !printer->newPage()) {
            painter.end();
            if (error) {
                *error = i18n("The printer stopped accepting pages.");
            }
            return false;
        }
        paintSheet(&painter, imposed, sheet, options, sheetRect, dpi);
        Q_EMIT progress(sheet + 1, sheets);
    }

    painter.end();
    return true;
}

} // namespace ps
