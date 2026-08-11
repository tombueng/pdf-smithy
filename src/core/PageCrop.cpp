/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#include "PageCrop.h"
#include "PdfFile.h"

#include "PdfGeometry.h"
#include "RenderBackend.h"

#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QTemporaryFile>

#include <KLocalizedString>

#include <qpdf/QPDF.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>
#include <qpdf/QPDFWriter.hh>

#include <algorithm>
#include <cstdio>
#include <functional>

namespace ps {

namespace {

using PdfGeometry::number;

/**
 * Turns display-space margins into page-space ones.
 *
 * The user trims what they can see; /CropBox is written in the page's own
 * coordinates. On a sideways page those are not the same edges, and getting it
 * wrong crops the wrong side of the paper.
 */
PageCrop::Margins toPageSpace(const PageCrop::Margins &display, int rotate)
{
    PageCrop::Margins page;
    switch (((rotate % 360) + 360) % 360) {
    case 90:
        page.bottom = display.left;
        page.right = display.bottom;
        page.top = display.right;
        page.left = display.top;
        break;
    case 180:
        page.right = display.left;
        page.top = display.bottom;
        page.left = display.right;
        page.bottom = display.top;
        break;
    case 270:
        page.top = display.left;
        page.left = display.bottom;
        page.bottom = display.right;
        page.right = display.top;
        break;
    default:
        page = display;
        break;
    }
    return page;
}

bool rewrite(const QString &input, const QString &output,
             const std::function<void(QPDF &, std::vector<QPDFPageObjectHelper> &)> &work, QString *error)
{
    QTemporaryFile temp(QFileInfo(output).absolutePath() + QLatin1String("/.pdf-smithy-crop-XXXXXX.pdf"));
    temp.setAutoRemove(false);
    if (!temp.open()) {
        if (error) {
            *error = i18n("Cannot write to “%1”.", QFileInfo(output).absolutePath());
        }
        return false;
    }
    const QString tempPath = temp.fileName();
    temp.close();

    try {
        QPDF pdf;
        PdfFile::open(pdf, input);
        std::vector<QPDFPageObjectHelper> pages = QPDFPageDocumentHelper(pdf).getAllPages();
        work(pdf, pages);

        QPDFWriter writer(pdf);
        writer.setOutputFilename(QFile::encodeName(tempPath).constData());
        writer.setObjectStreamMode(qpdf_o_generate);
        writer.setCompressStreams(true);
        writer.setDeterministicID(true);
        writer.write();
    } catch (const std::exception &e) {
        QFile::remove(tempPath);
        if (error) {
            *error = QString::fromUtf8(e.what());
        }
        return false;
    }

    QFile::setPermissions(tempPath, QFile::ReadOwner | QFile::WriteOwner | QFile::ReadGroup | QFile::ReadOther);
    if (::rename(QFile::encodeName(tempPath).constData(), QFile::encodeName(output).constData()) != 0) {
        QFile::remove(tempPath);
        if (error) {
            *error = i18n("Could not write “%1”.", output);
        }
        return false;
    }
    return true;
}

} // namespace

bool PageCrop::crop(const QString &inputPdf, const QString &outputPdf, const QVector<int> &pages,
                    const Margins &margins, QString *error)
{
    if (pages.isEmpty()) {
        if (error) {
            *error = i18n("There are no pages to crop.");
        }
        return false;
    }
    if (margins.isEmpty()) {
        if (error) {
            *error = i18n("Nothing would be trimmed.");
        }
        return false;
    }

    return rewrite(
        inputPdf, outputPdf,
        [&pages, &margins](QPDF &, std::vector<QPDFPageObjectHelper> &all) {
            for (const int index : pages) {
                if (index < 0 || index >= static_cast<int>(all.size())) {
                    continue;
                }
                QPDFPageObjectHelper &page = all[static_cast<size_t>(index)];
                const QRectF box = PdfGeometry::mediaBoxOf(page);
                const Margins inset = toPageSpace(margins, PdfGeometry::rotationOf(page));

                double left = box.left() + std::max(0.0, inset.left);
                double bottom = box.top() + std::max(0.0, inset.bottom);
                double right = box.left() + box.width() - std::max(0.0, inset.right);
                double top = box.top() + box.height() - std::max(0.0, inset.top);

                // A crop that leaves nothing is a mistake, not an instruction.
                if (right - left < 36.0 || top - bottom < 36.0) {
                    continue;
                }

                page.getObjectHandle().replaceKey("/CropBox",
                                                  QPDFObjectHandle::parse("[" + number(left) + " " + number(bottom)
                                                                          + " " + number(right) + " " + number(top)
                                                                          + "]"));
            }
        },
        error);
}

bool PageCrop::reset(const QString &inputPdf, const QString &outputPdf, const QVector<int> &pages, QString *error)
{
    return rewrite(
        inputPdf, outputPdf,
        [&pages](QPDF &, std::vector<QPDFPageObjectHelper> &all) {
            for (const int index : pages) {
                if (index >= 0 && index < static_cast<int>(all.size())) {
                    all[static_cast<size_t>(index)].getObjectHandle().removeKey("/CropBox");
                }
            }
        },
        error);
}

PageCrop::Margins PageCrop::detect(RenderBackend *backend, int sourceId, int page, int threshold, double keepPoints)
{
    Margins margins;
    if (!backend) {
        return margins;
    }

    const QSizeF sizePoints = backend->pageSizePoints(sourceId, page);
    if (sizePoints.isEmpty()) {
        return margins;
    }

    // Low resolution on purpose: this is looking for where the ink starts, and
    // a hundred-odd pixels across is plenty for that while staying quick
    // enough to run over a whole document.
    const QImage image = backend->renderPage(sourceId, page, 220);
    if (image.isNull()) {
        return margins;
    }

    const QImage grey = image.convertToFormat(QImage::Format_Grayscale8);
    int left = grey.width();
    int right = -1;
    int top = grey.height();
    int bottom = -1;

    for (int y = 0; y < grey.height(); ++y) {
        const uchar *line = grey.constScanLine(y);
        for (int x = 0; x < grey.width(); ++x) {
            if (line[x] < threshold) {
                left = std::min(left, x);
                right = std::max(right, x);
                top = std::min(top, y);
                bottom = std::max(bottom, y);
            }
        }
    }

    if (right < left || bottom < top) {
        return margins; // a blank page; leave it alone
    }

    const double scaleX = sizePoints.width() / grey.width();
    const double scaleY = sizePoints.height() / grey.height();

    margins.left = std::max(0.0, left * scaleX - keepPoints);
    margins.right = std::max(0.0, (grey.width() - 1 - right) * scaleX - keepPoints);
    margins.top = std::max(0.0, top * scaleY - keepPoints);
    margins.bottom = std::max(0.0, (grey.height() - 1 - bottom) * scaleY - keepPoints);
    return margins;
}

PageCrop::Margins PageCrop::detectCommon(RenderBackend *backend, int sourceId, const QVector<int> &pages, int threshold,
                                         double keepPoints)
{
    Margins common;
    bool first = true;

    for (const int page : pages) {
        const Margins found = detect(backend, sourceId, page, threshold, keepPoints);
        if (found.isEmpty()) {
            continue;
        }
        if (first) {
            common = found;
            first = false;
            continue;
        }
        // The smallest trim wins on every side, so no page loses content.
        common.left = std::min(common.left, found.left);
        common.right = std::min(common.right, found.right);
        common.top = std::min(common.top, found.top);
        common.bottom = std::min(common.bottom, found.bottom);
    }

    return common;
}

} // namespace ps
