/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#include "Overlay.h"
#include "PdfFile.h"

#include "PdfGeometry.h"
#include "PdfImage.h"

#include <QFile>
#include <QFileInfo>
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

/** Fetches or creates a sub-dictionary of the page's /Resources. */
QPDFObjectHandle resourceCategory(QPDFPageObjectHelper &page, const char *key)
{
    QPDFObjectHandle resources = page.getAttribute("/Resources", true);
    if (!resources.isDictionary()) {
        resources = QPDFObjectHandle::newDictionary();
        page.getObjectHandle().replaceKey("/Resources", resources);
    }
    QPDFObjectHandle category = resources.getKey(key);
    if (!category.isDictionary()) {
        category = QPDFObjectHandle::newDictionary();
        resources.replaceKey(key, category);
    }
    return category;
}

/** An /ExtGState carrying a fill and stroke alpha, registered on the page. */
std::string registerOpacity(QPDF &pdf, QPDFPageObjectHelper &page, double opacity, int serial)
{
    const double clamped = std::clamp(opacity, 0.0, 1.0);
    if (clamped >= 1.0) {
        return {}; // fully opaque needs no state at all
    }

    QPDFObjectHandle state = QPDFObjectHandle::newDictionary();
    state.replaceKey("/Type", QPDFObjectHandle::newName("/ExtGState"));
    state.replaceKey("/ca", QPDFObjectHandle::newReal(clamped, 3));
    state.replaceKey("/CA", QPDFObjectHandle::newReal(clamped, 3));

    const std::string name = "/PsAlpha" + std::to_string(serial);
    resourceCategory(page, "/ExtGState").replaceKey(name, pdf.makeIndirectObject(state));
    return name + " gs\n";
}

/**
 * Escapes a string for a PDF literal.
 *
 * Was a toLatin1() call, which is wrong in a way that only shows up in real
 * text: WinAnsi carries the en dash, the euro sign and the curly quotes in the
 * 0x80 to 0x9F range that Latin-1 leaves empty, so a German watermark came out
 * with a question mark where its dash should have been. The encoder lives in
 * PdfGeometry now, because printer's marks needed the same thing and had
 * written their own.
 */
std::string escapeText(const QString &text)
{
    return PdfGeometry::winAnsi(text);
}

/** Opens @p path, hands the pages to @p work, writes the result atomically. */
bool transform(const QString &inputPdf, const QString &outputPdf,
               const std::function<bool(QPDF &, std::vector<QPDFPageObjectHelper> &, QString *)> &work, QString *error)
{
    QTemporaryFile temp(QFileInfo(outputPdf).absolutePath() + QLatin1String("/.pdf-smithy-stamp-XXXXXX.pdf"));
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
        PdfFile::open(pdf, inputPdf);
        std::vector<QPDFPageObjectHelper> pages = QPDFPageDocumentHelper(pdf).getAllPages();

        if (!work(pdf, pages, error)) {
            QFile::remove(tempPath);
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

/** Display-space size of a page, i.e. what the reader sees. */
QSizeF displaySize(QPDFPageObjectHelper &page)
{
    const QRectF box = PdfGeometry::mediaBoxOf(page);
    const int rotate = PdfGeometry::rotationOf(page);
    return (rotate == 90 || rotate == 270) ? QSizeF(box.height(), box.width()) : box.size();
}

} // namespace

double Overlay::estimateTextWidth(const QString &text, double fontSize)
{
    // Helvetica-Bold averages a little over half an em across mixed text. Good
    // enough for centring a watermark; nothing here needs typesetting accuracy.
    return 0.55 * fontSize * text.size();
}

QRectF Overlay::anchoredRect(Anchor anchor, const QSizeF &pageSize, const QSizeF &itemSize, double marginPoints)
{
    const double w = itemSize.width();
    const double h = itemSize.height();
    const double m = marginPoints;

    double x = 0.0;
    double y = 0.0;

    switch (anchor) {
    case Anchor::TopLeft:
    case Anchor::CentreLeft:
    case Anchor::BottomLeft:
        x = m;
        break;
    case Anchor::TopCentre:
    case Anchor::Centre:
    case Anchor::BottomCentre:
        x = (pageSize.width() - w) / 2.0;
        break;
    case Anchor::TopRight:
    case Anchor::CentreRight:
    case Anchor::BottomRight:
        x = pageSize.width() - w - m;
        break;
    }

    switch (anchor) {
    case Anchor::TopLeft:
    case Anchor::TopCentre:
    case Anchor::TopRight:
        y = pageSize.height() - h - m;
        break;
    case Anchor::CentreLeft:
    case Anchor::Centre:
    case Anchor::CentreRight:
        y = (pageSize.height() - h) / 2.0;
        break;
    case Anchor::BottomLeft:
    case Anchor::BottomCentre:
    case Anchor::BottomRight:
        y = m;
        break;
    }

    return QRectF(x, y, w, h);
}

bool Overlay::stampImages(const QString &inputPdf, const QString &outputPdf,
                          const QHash<int, QVector<ImageStamp>> &stamps, QString *error)
{
    if (stamps.isEmpty()) {
        if (error) {
            *error = i18n("There is nothing to stamp.");
        }
        return false;
    }

    return transform(
        inputPdf, outputPdf,
        [&stamps](QPDF &pdf, std::vector<QPDFPageObjectHelper> &pages, QString *innerError) {
            for (auto it = stamps.constBegin(); it != stamps.constEnd(); ++it) {
                const int index = it.key();
                if (index < 0 || index >= static_cast<int>(pages.size()) || it.value().isEmpty()) {
                    continue;
                }

                QPDFPageObjectHelper &page = pages[static_cast<size_t>(index)];
                const QRectF box = PdfGeometry::mediaBoxOf(page);
                const int rotate = PdfGeometry::rotationOf(page);

                // Existing content routinely leaves the graphics state dirty,
                // so it is boxed in before anything is drawn over it.
                PdfGeometry::isolateExistingContent(page, page.getObjectHandle());

                std::string content;
                int serial = 0;
                for (const ImageStamp &stamp : it.value()) {
                    QPDFObjectHandle image = PdfImage::embed(pdf, stamp.image);
                    if (image.isNull()) {
                        continue;
                    }

                    const std::string name = "/PsStamp" + std::to_string(serial);
                    resourceCategory(page, "/XObject").replaceKey(name, image);

                    content += "q\n";
                    content += registerOpacity(pdf, page, stamp.opacity, serial);
                    content += PdfGeometry::displayToPageMatrix(rotate, box.width(), box.height());
                    // Emitted after the rotation matrix so that it applies
                    // first: the last `cm` is the one closest to the drawing.
                    content += PdfGeometry::placementMatrix(stamp.rect, stamp.rotation);
                    content += name + " Do\nQ\n";
                    ++serial;
                }

                if (!content.empty()) {
                    page.addPageContents(QPDFObjectHandle::newStream(&pdf, content), false);
                }
            }
            Q_UNUSED(innerError)
            return true;
        },
        error);
}

bool Overlay::stampImageOnPages(const QString &inputPdf, const QString &outputPdf, const QVector<int> &pages,
                                const QImage &image, Anchor anchor, double relativeWidth, double rotation,
                                double opacity, double marginPoints, QString *error)
{
    if (image.isNull()) {
        if (error) {
            *error = i18n("The stamp image could not be read.");
        }
        return false;
    }

    return transform(
        inputPdf, outputPdf,
        [&](QPDF &pdf, std::vector<QPDFPageObjectHelper> &all, QString *) {
            QPDFObjectHandle shared = PdfImage::embed(pdf, image);
            if (shared.isNull()) {
                return false;
            }

            const double aspect = image.height() > 0 ? static_cast<double>(image.height()) / image.width() : 1.0;

            for (const int index : pages) {
                if (index < 0 || index >= static_cast<int>(all.size())) {
                    continue;
                }
                QPDFPageObjectHelper &page = all[static_cast<size_t>(index)];
                const QRectF box = PdfGeometry::mediaBoxOf(page);
                const int rotate = PdfGeometry::rotationOf(page);
                const QSizeF visible = displaySize(page);

                const double width = visible.width() * std::clamp(relativeWidth, 0.01, 1.0);
                const QRectF target = anchoredRect(anchor, visible, QSizeF(width, width * aspect), marginPoints);

                PdfGeometry::isolateExistingContent(page, page.getObjectHandle());

                // One image object, referenced from every page it appears on:
                // a hundred-page watermark costs one copy of the picture.
                resourceCategory(page, "/XObject").replaceKey("/PsStamp0", shared);

                std::string content = "q\n";
                content += registerOpacity(pdf, page, opacity, 0);
                content += PdfGeometry::displayToPageMatrix(rotate, box.width(), box.height());
                content += PdfGeometry::placementMatrix(target, rotation);
                content += "/PsStamp0 Do\nQ\n";
                page.addPageContents(QPDFObjectHandle::newStream(&pdf, content), false);
            }
            return true;
        },
        error);
}

namespace {

/** Draws one text stamp onto one page. Shared by both text entry points. */
void drawText(QPDF &pdf, QPDFPageObjectHelper &page, const Overlay::TextStamp &stamp, QPDFObjectHandle font)
{
    const QRectF box = PdfGeometry::mediaBoxOf(page);
    const int rotate = PdfGeometry::rotationOf(page);
    const QSizeF visible = displaySize(page);

    const double textWidth = Overlay::estimateTextWidth(stamp.text, stamp.fontSize);
    const double radians = stamp.rotation * M_PI / 180.0;
    const double c = std::cos(radians);
    const double s = std::sin(radians);

    const QRectF anchorRect
        = Overlay::anchoredRect(stamp.anchor, visible, QSizeF(textWidth, stamp.fontSize), stamp.marginPoints);
    const double cx = anchorRect.center().x();
    const double cy = anchorRect.center().y();

    // Half the cap height, so the line sits on the anchor rather than hanging
    // below it.
    const double vertical = 0.35 * stamp.fontSize;
    const double tx = cx - (textWidth / 2.0) * c + vertical * s;
    const double ty = cy - (textWidth / 2.0) * s - vertical * c;

    PdfGeometry::isolateExistingContent(page, page.getObjectHandle());
    resourceCategory(page, "/Font").replaceKey("/PsWatermark", font);

    const double r = stamp.colour.redF();
    const double g = stamp.colour.greenF();
    const double b = stamp.colour.blueF();

    std::string content = "q\n";
    content += registerOpacity(pdf, page, stamp.opacity, 0);
    content += PdfGeometry::displayToPageMatrix(rotate, box.width(), box.height());
    content += "BT\n";
    content += "/PsWatermark " + number(stamp.fontSize) + " Tf\n";
    if (stamp.outlineOnly) {
        // Render mode 1 draws the outline only, which keeps the text under a
        // big watermark readable.
        content += number(r) + " " + number(g) + " " + number(b) + " RG\n";
        content += number(std::max(0.5, stamp.fontSize / 40.0)) + " w\n1 Tr\n";
    } else {
        content += number(r) + " " + number(g) + " " + number(b) + " rg\n0 Tr\n";
    }
    content += number(c) + " " + number(s) + " " + number(-s) + " " + number(c) + " " + number(tx) + " " + number(ty)
        + " Tm\n";
    content += "(" + escapeText(stamp.text) + ") Tj\nET\nQ\n";

    page.addPageContents(QPDFObjectHandle::newStream(&pdf, content), false);
}

QPDFObjectHandle makeHelvetica(QPDF &pdf)
{
    return pdf.makeIndirectObject(QPDFObjectHandle::parse(
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica-Bold /Encoding /WinAnsiEncoding >>"));
}

} // namespace

bool Overlay::stampTexts(const QString &inputPdf, const QString &outputPdf, const QHash<int, TextStamp> &stamps,
                         QString *error)
{
    if (stamps.isEmpty()) {
        if (error) {
            *error = i18n("There is nothing to stamp.");
        }
        return false;
    }

    return transform(
        inputPdf, outputPdf,
        [&stamps](QPDF &pdf, std::vector<QPDFPageObjectHelper> &all, QString *) {
            QPDFObjectHandle font = makeHelvetica(pdf);
            for (auto it = stamps.constBegin(); it != stamps.constEnd(); ++it) {
                if (it.key() < 0 || it.key() >= static_cast<int>(all.size()) || it.value().text.isEmpty()) {
                    continue;
                }
                drawText(pdf, all[static_cast<size_t>(it.key())], it.value(), font);
            }
            return true;
        },
        error);
}

bool Overlay::stampText(const QString &inputPdf, const QString &outputPdf, const QVector<int> &pages,
                        const TextStamp &stamp, QString *error)
{
    if (stamp.text.trimmed().isEmpty()) {
        if (error) {
            *error = i18n("The watermark text is empty.");
        }
        return false;
    }

    return transform(
        inputPdf, outputPdf,
        [&](QPDF &pdf, std::vector<QPDFPageObjectHelper> &all, QString *) {
            QPDFObjectHandle font = makeHelvetica(pdf);
            for (const int index : pages) {
                if (index < 0 || index >= static_cast<int>(all.size())) {
                    continue;
                }
                drawText(pdf, all[static_cast<size_t>(index)], stamp, font);
            }
            return true;
        },
        error);
}

} // namespace ps
