/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#include "PdfGeometry.h"

#include <QByteArray>

#include <qpdf/QPDF.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDFPageObjectHelper.hh>

#include <cmath>

namespace ps::PdfGeometry {

std::string number(double value)
{
    // QByteArray::number always uses the C locale; snprintf and std::to_string
    // do not. See the header for why that matters so much here.
    return QByteArray::number(value, 'f', 4).toStdString();
}

double numericValue(QPDFObjectHandle item, double fallback)
{
    if (item.isInteger()) {
        return double(item.getIntValue());
    }
    if (item.isReal()) {
        // The real's own text, parsed the one way that ignores LC_NUMERIC.
        return QByteArray::fromStdString(item.getRealValue()).toDouble();
    }
    return fallback;
}

std::string displayToPageMatrix(int rotate, double pageWidth, double pageHeight)
{
    switch (((rotate % 360) + 360) % 360) {
    case 90:
        return "0 1 -1 0 " + number(pageWidth) + " 0 cm\n";
    case 180:
        return "-1 0 0 -1 " + number(pageWidth) + " " + number(pageHeight) + " cm\n";
    case 270:
        return "0 -1 1 0 0 " + number(pageHeight) + " cm\n";
    default:
        return "1 0 0 1 0 0 cm\n";
    }
}

QTransform displayToPageTransform(int rotate, double pageWidth, double pageHeight)
{
    // QTransform(m11, m12, m21, m22, dx, dy) and a PDF matrix [a b c d e f] are
    // the same six numbers in the same order: both map row vectors.
    switch (((rotate % 360) + 360) % 360) {
    case 90:
        return QTransform(0, 1, -1, 0, pageWidth, 0);
    case 180:
        return QTransform(-1, 0, 0, -1, pageWidth, pageHeight);
    case 270:
        return QTransform(0, -1, 1, 0, 0, pageHeight);
    default:
        return QTransform();
    }
}

std::string rotateAboutCentre(double degrees, double width, double height)
{
    const double radians = degrees * M_PI / 180.0;
    const double c = std::cos(radians);
    const double s = std::sin(radians);
    const double cx = width / 2.0;
    const double cy = height / 2.0;

    return number(c) + " " + number(s) + " " + number(-s) + " " + number(c) + " " + number(cx - cx * c + cy * s) + " "
        + number(cy - cx * s - cy * c) + " cm\n";
}

std::string placementMatrix(const QRectF &target, double rotationDegrees)
{
    const double radians = rotationDegrees * M_PI / 180.0;
    const double c = std::cos(radians);
    const double s = std::sin(radians);
    const double w = target.width();
    const double h = target.height();
    const double cx = target.center().x();
    const double cy = target.center().y();

    // Unit square → target, turned about the target's own centre.
    return number(w * c) + " " + number(w * s) + " " + number(-h * s) + " " + number(h * c) + " "
        + number(cx - 0.5 * w * c + 0.5 * h * s) + " " + number(cy - 0.5 * w * s - 0.5 * h * c) + " cm\n";
}

void isolateExistingContent(QPDFPageObjectHelper &page, QPDFObjectHandle document, const std::string &extraPrefix)
{
    QPDF *pdf = document.getOwningQPDF();
    if (!pdf) {
        return;
    }
    page.addPageContents(QPDFObjectHandle::newStream(pdf, "q\n" + extraPrefix), true);
    page.addPageContents(QPDFObjectHandle::newStream(pdf, std::string("Q\n")), false);
}

double boxValue(QPDFObjectHandle box, int index, double fallback)
{
    if (!box.isArray() || box.getArrayNItems() != 4) {
        return fallback;
    }
    return numericValue(box.getArrayItem(index), fallback);
}

double numberAt(QPDFObjectHandle array, int index, double fallback)
{
    if (!array.isArray() || index < 0 || index >= array.getArrayNItems()) {
        return fallback;
    }
    return numericValue(array.getArrayItem(index), fallback);
}

QRectF mediaBoxOf(QPDFPageObjectHelper &page)
{
    QPDFObjectHandle box = page.getMediaBox(true);
    const double left = boxValue(box, 0, 0.0);
    const double bottom = boxValue(box, 1, 0.0);
    const double right = boxValue(box, 2, 612.0);
    const double top = boxValue(box, 3, 792.0);
    return QRectF(left, bottom, right - left, top - bottom);
}

int rotationOf(QPDFPageObjectHelper &page)
{
    QPDFObjectHandle rotate = page.getAttribute("/Rotate", false);
    const int value = rotate.isInteger() ? rotate.getIntValueAsInt() : 0;
    return ((value % 360) + 360) % 360;
}

std::string winAnsi(const QString &text)
{
    // The characters WinAnsi puts in the range Latin-1 leaves empty. There is no
    // converter for this in Qt (QStringConverter deals in Unicode encodings),
    // so the table is written out.
    struct Extra {
        char16_t unicode;
        unsigned char byte;
    };
    static const Extra extras[] = {
        { u'€', 0x80 }, { u'‚', 0x82 }, { u'ƒ', 0x83 }, { u'„', 0x84 }, { u'…', 0x85 }, { u'†', 0x86 }, { u'‡', 0x87 },
        { u'ˆ', 0x88 }, { u'‰', 0x89 }, { u'Š', 0x8A }, { u'‹', 0x8B }, { u'Œ', 0x8C }, { u'Ž', 0x8E }, { u'‘', 0x91 },
        { u'’', 0x92 }, { u'“', 0x93 }, { u'”', 0x94 }, { u'•', 0x95 }, { u'–', 0x96 }, { u'—', 0x97 }, { u'˜', 0x98 },
        { u'™', 0x99 }, { u'š', 0x9A }, { u'›', 0x9B }, { u'œ', 0x9C }, { u'ž', 0x9E }, { u'Ÿ', 0x9F },
    };

    std::string out;
    out.reserve(size_t(text.size()) + 8);
    for (const QChar character : text) {
        const char16_t code = character.unicode();

        unsigned char byte = '?';
        if ((code >= 0x20 && code < 0x7F) || (code >= 0xA0 && code < 0x100)) {
            // Latin-1 and WinAnsi agree here. The gap in the middle is left out
            // on purpose: U+0080 to U+009F are the C1 controls, and writing them
            // through as bytes would make them print as the euro sign and the
            // curly quotes that WinAnsi keeps at those positions instead.
            byte = static_cast<unsigned char>(code);
        } else {
            for (const Extra &extra : extras) {
                if (code == extra.unicode) {
                    byte = extra.byte;
                    break;
                }
            }
        }

        if (byte == '(' || byte == ')' || byte == '\\') {
            out += '\\';
        }
        out += static_cast<char>(byte);
    }
    return out;
}

std::string winAnsiLiteral(const QString &text)
{
    return "(" + winAnsi(text) + ")";
}

} // namespace ps::PdfGeometry
