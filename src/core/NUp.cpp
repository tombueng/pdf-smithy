/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#include "NUp.h"
#include "PdfFile.h"

#include "PdfGeometry.h"

#include <KLocalizedString>
#include <QRectF>
#include <QVector>

#include <qpdf/QPDF.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>
#include <qpdf/QPDFWriter.hh>

#include <cmath>

namespace ps {

QSize NUp::gridFor(int pagesPerSheet, bool sheetIsLandscape)
{
    // The near-square split, which is what makes the slots as large as they can
    // be. Ten up as 5×2 wastes far less than 10×1.
    int columns = int(std::floor(std::sqrt(double(qMax(1, pagesPerSheet)))));
    while (columns > 1 && pagesPerSheet % columns != 0) {
        --columns;
    }
    int rows = qMax(1, pagesPerSheet / qMax(1, columns));

    // The long side of the grid follows the long side of the sheet.
    if (sheetIsLandscape != (columns > rows)) {
        std::swap(columns, rows);
    }
    return { qMax(1, columns), qMax(1, rows) };
}

bool NUp::apply(const QString &inputPdf, const QString &outputPdf, const Options &options, QString *error)
{
    const int columns = qMax(1, options.columns);
    const int rows = qMax(1, options.rows);
    if (columns * rows < 2) {
        if (error) {
            *error = i18n("A sheet has to hold at least two pages.");
        }
        return false;
    }

    try {
        QPDF pdf;
        PdfFile::open(pdf, inputPdf);

        QPDFPageDocumentHelper documents(pdf);
        std::vector<QPDFPageObjectHelper> sources = documents.getAllPages();
        if (sources.empty()) {
            if (error) {
                *error = i18n("The document has no pages.");
            }
            return false;
        }

        // Sheet size first, because the slots are measured against it.
        QSizeF sheet = options.sheetSizePoints;
        if (sheet.isEmpty()) {
            const QRectF media = PdfGeometry::mediaBoxOf(sources.front());
            const int rotate = PdfGeometry::rotationOf(sources.front());
            QSizeF shown = (rotate == 90 || rotate == 270) ? QSizeF(media.height(), media.width()) : media.size();
            // A wide grid on a tall sheet leaves most of the paper empty.
            if ((columns > rows) != (shown.width() > shown.height())) {
                shown.transpose();
            }
            sheet = shown;
        }

        const double usableWidth = sheet.width() - 2 * options.marginPoints - (columns - 1) * options.gutterPoints;
        const double usableHeight = sheet.height() - 2 * options.marginPoints - (rows - 1) * options.gutterPoints;
        if (usableWidth <= 1.0 || usableHeight <= 1.0) {
            if (error) {
                *error = i18n("The margins and gaps leave no room on the sheet. Reduce them or use a larger sheet.");
            }
            return false;
        }
        const double cellWidth = usableWidth / columns;
        const double cellHeight = usableHeight / rows;

        // Every source page is turned into a placeable object before any of them
        // leaves the page tree, so that removing the originals cannot pull the
        // ground out from under a placement.
        QVector<QPDFObjectHandle> placeable;
        placeable.reserve(int(sources.size()));
        for (QPDFPageObjectHelper &page : sources) {
            placeable.append(page.getFormXObjectForPage());
        }
        for (QPDFPageObjectHelper &page : sources) {
            documents.removePage(page);
        }

        const int perSheet = columns * rows;
        const int sheetCount = (placeable.size() + perSheet - 1) / perSheet;

        for (int sheetIndex = 0; sheetIndex < sheetCount; ++sheetIndex) {
            QPDFObjectHandle page = QPDFObjectHandle::parse("<< /Type /Page >>");
            page.replaceKey("/MediaBox",
                            QPDFObjectHandle::parse("[0 0 " + PdfGeometry::number(sheet.width()) + " "
                                                    + PdfGeometry::number(sheet.height()) + "]"));
            page.replaceKey("/Resources", QPDFObjectHandle::parse("<< /XObject << >> >>"));

            QPDFPageObjectHelper sheetPage(pdf.makeIndirectObject(page));
            QPDFObjectHandle resources = sheetPage.getObjectHandle().getKey("/Resources");
            int suffix = 1;

            std::string content;
            for (int slot = 0; slot < perSheet; ++slot) {
                const int sourceIndex = sheetIndex * perSheet + slot;
                if (sourceIndex >= placeable.size()) {
                    break;
                }

                const int column = options.columnsFirst ? slot / rows : slot % columns;
                const int row = options.columnsFirst ? slot % rows : slot / columns;

                const double left = options.marginPoints + column * (cellWidth + options.gutterPoints);
                const double top = sheet.height() - options.marginPoints - row * (cellHeight + options.gutterPoints);
                const QPDFObjectHandle::Rectangle cell(left, top - cellHeight, left + cellWidth, top);

                const std::string name = resources.getUniqueResourceName("/Pg", suffix);
                // Shrinking and expanding are both allowed, so a slot is filled
                // whatever size the source page happens to be, so a document of
                // mixed page sizes still produces an even sheet.
                content += sheetPage.placeFormXObject(placeable.at(sourceIndex), name, cell, false, true, true);
                resources.getKey("/XObject").replaceKey(name, placeable.at(sourceIndex));

                if (options.drawBorders) {
                    content += "q 0.5 w 0.6 0.6 0.6 RG\n" + PdfGeometry::number(cell.llx) + " "
                        + PdfGeometry::number(cell.lly) + " " + PdfGeometry::number(cell.urx - cell.llx) + " "
                        + PdfGeometry::number(cell.ury - cell.lly) + " re S\nQ\n";
                }
            }

            sheetPage.getObjectHandle().replaceKey("/Contents", QPDFObjectHandle::newStream(&pdf, content));
            documents.addPage(sheetPage, false);
        }

        QPDFWriter writer(pdf, outputPdf.toUtf8().constData());
        writer.setObjectStreamMode(qpdf_o_generate);
        // The form XObjects arrive uncompressed, and preserving would write them
        // that way. Compressing only re-deflates what is already lossless;
        // QPDF never touches a JPEG.
        writer.setStreamDataMode(qpdf_s_compress);
        writer.write();
    } catch (const std::exception &e) {
        if (error) {
            *error = QString::fromUtf8(e.what());
        }
        return false;
    }

    return true;
}

} // namespace ps
