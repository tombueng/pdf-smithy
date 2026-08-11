/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#include "ImageIO.h"

#include "PdfGeometry.h"
#include "PdfImage.h"
#include "RenderBackend.h"

#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QImageWriter>
#include <QTemporaryFile>
#include <QTransform>

#include <KLocalizedString>

#include <qpdf/QPDF.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>
#include <qpdf/QPDFWriter.hh>

#include <cmath>
#include <cstdio>

namespace ps {

namespace {

using PdfGeometry::number;

int digitsFor(int count)
{
    return count < 10 ? 1 : static_cast<int>(std::floor(std::log10(count))) + 1;
}

} // namespace

QStringList ImageIO::readableImageFilters()
{
    QStringList patterns;
    const QList<QByteArray> formats = QImageReader::supportedImageFormats();
    patterns.reserve(formats.size());
    for (const QByteArray &format : formats) {
        patterns.append(QStringLiteral("*.") + QString::fromLatin1(format));
    }
    patterns.sort();
    patterns.removeDuplicates();
    return patterns;
}

bool ImageIO::imagesToPdf(const QStringList &imagePaths, const QString &outputPdf, const ImportOptions &options,
                          QString *error)
{
    if (imagePaths.isEmpty()) {
        if (error) {
            *error = i18n("No images were given.");
        }
        return false;
    }

    QTemporaryFile temp(QFileInfo(outputPdf).absolutePath() + QLatin1String("/.pdf-smithy-import-XXXXXX.pdf"));
    temp.setAutoRemove(false);
    if (!temp.open()) {
        if (error) {
            *error = i18n("Cannot write to “%1”.", QFileInfo(outputPdf).absolutePath());
        }
        return false;
    }
    const QString tempPath = temp.fileName();
    temp.close();

    try {
        QPDF pdf;
        pdf.emptyPDF();
        QPDFPageDocumentHelper pages(pdf);

        int added = 0;
        for (const QString &path : imagePaths) {
            QImage image(path);
            if (image.isNull()) {
                continue;
            }

            // A page the size of the picture, unless a fixed size was asked
            // for. Importing a stack of 300 dpi scans should produce A4-ish
            // pages, not pages measured in pixels.
            const double dpi = image.dotsPerMeterX() > 0 ? image.dotsPerMeterX() * 0.0254 : options.assumedDpi;
            const double scale = 72.0 / (dpi > 1.0 ? dpi : options.assumedDpi);

            QSizeF pageSize = options.pageSizePoints;
            QRectF target;
            if (pageSize.isEmpty()) {
                pageSize = QSizeF(image.width() * scale, image.height() * scale);
                target = QRectF(QPointF(0, 0), pageSize);
            } else {
                const QRectF usable(options.marginPoints, options.marginPoints,
                                    pageSize.width() - 2 * options.marginPoints,
                                    pageSize.height() - 2 * options.marginPoints);
                const double aspect = static_cast<double>(image.height()) / image.width();
                double w = usable.width();
                double h = w * aspect;
                if (h > usable.height()) {
                    h = usable.height();
                    w = h / aspect;
                }
                target = QRectF(usable.left() + (usable.width() - w) / 2.0, usable.top() + (usable.height() - h) / 2.0,
                                w, h);
            }

            QPDFObjectHandle embedded = options.jpegQuality > 0 ? PdfImage::embedAsJpeg(pdf, image, options.jpegQuality)
                                                                : PdfImage::embed(pdf, image);
            if (embedded.isNull()) {
                continue;
            }

            QPDFObjectHandle xobjects = QPDFObjectHandle::newDictionary();
            xobjects.replaceKey("/Im0", embedded);
            QPDFObjectHandle resources = QPDFObjectHandle::newDictionary();
            resources.replaceKey("/XObject", xobjects);

            const std::string content = "q\n" + PdfGeometry::placementMatrix(target, 0.0) + "/Im0 Do\nQ\n";

            QPDFObjectHandle page = QPDFObjectHandle::newDictionary();
            page.replaceKey("/Type", QPDFObjectHandle::newName("/Page"));
            page.replaceKey(
                "/MediaBox",
                QPDFObjectHandle::parse("[0 0 " + number(pageSize.width()) + " " + number(pageSize.height()) + "]"));
            page.replaceKey("/Resources", resources);
            page.replaceKey("/Contents", QPDFObjectHandle::newStream(&pdf, content));

            pages.addPage(QPDFPageObjectHelper(pdf.makeIndirectObject(page)), false);
            ++added;
        }

        if (added == 0) {
            QFile::remove(tempPath);
            if (error) {
                *error = i18n("None of the files could be read as an image.");
            }
            return false;
        }

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
    if (::rename(QFile::encodeName(tempPath).constData(), QFile::encodeName(outputPdf).constData()) != 0) {
        QFile::remove(tempPath);
        if (error) {
            *error = i18n("Could not write “%1”.", outputPdf);
        }
        return false;
    }
    return true;
}

bool ImageIO::pagesToImages(RenderBackend *backend, int sourceId, const QVector<int> &pages,
                            const QVector<int> &rotations, const QString &outputTemplate, const ExportOptions &options,
                            QStringList *written, QString *error)
{
    QVector<PageRef> refs;
    refs.reserve(pages.size());
    for (int i = 0; i < pages.size(); ++i) {
        refs.append(PageRef { sourceId, pages.at(i), i < rotations.size() ? rotations.at(i) : 0 });
    }
    return pagesToImages(backend, refs, outputTemplate, options, written, nullptr, error);
}

bool ImageIO::pagesToImages(RenderBackend *backend, const QVector<PageRef> &pages, const QString &outputTemplate,
                            const ExportOptions &options, QStringList *written, QVector<int> *skipped, QString *error)
{
    if (!backend || pages.isEmpty()) {
        if (error) {
            *error = i18n("There are no pages to export.");
        }
        return false;
    }
    if (outputTemplate.isEmpty()) {
        if (error) {
            *error = i18n("No output file name was given.");
        }
        return false;
    }

    const int width = digitsFor(pages.size());
    const QByteArray format = options.format.toLatin1();

    for (int i = 0; i < pages.size(); ++i) {
        const PageRef &ref = pages.at(i);
        const QSizeF sizePoints = backend->pageSizePoints(ref.sourceId, ref.sourcePage);
        if (sizePoints.isEmpty()) {
            if (skipped) {
                skipped->append(i + 1);
            }
            continue;
        }

        const int pixels = std::max(1, static_cast<int>(std::lround(sizePoints.width() / 72.0 * options.dpi)));
        QImage image = backend->renderPage(ref.sourceId, ref.sourcePage, pixels);
        if (image.isNull()) {
            if (skipped) {
                skipped->append(i + 1);
            }
            continue;
        }

        const int rotation = ref.rotation;
        if (rotation != 0) {
            image = image.transformed(QTransform().rotate(rotation), Qt::SmoothTransformation);
        }

        // Recorded in the file so that opening the export in an editor shows
        // it at the size it was on paper rather than at screen size.
        const int dotsPerMeter = static_cast<int>(std::lround(options.dpi / 0.0254));
        image.setDotsPerMeterX(dotsPerMeter);
        image.setDotsPerMeterY(dotsPerMeter);

        QString path = outputTemplate;
        const QString numberText = QStringLiteral("%1").arg(i + 1, width, 10, QLatin1Char('0'));
        if (path.contains(QLatin1String("%1"))) {
            path.replace(QLatin1String("%1"), numberText);
        } else {
            const QFileInfo info(path);
            path = info.absolutePath() + QLatin1Char('/') + info.completeBaseName() + QLatin1Char('-') + numberText
                + QLatin1Char('.') + info.suffix();
        }

        QImageWriter writer(path, format);
        writer.setQuality(options.quality);
        if (!writer.write(image)) {
            if (error) {
                *error = i18n("“%1” could not be written: %2", path, writer.errorString());
            }
            return false;
        }
        if (written) {
            written->append(path);
        }
    }

    if (written && written->isEmpty()) {
        if (error) {
            *error = i18n("No pages could be rendered.");
        }
        return false;
    }
    return true;
}

} // namespace ps
