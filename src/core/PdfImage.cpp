/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#include "PdfImage.h"

#include <QBuffer>

#include <qpdf/QPDF.hh>
#include <qpdf/QPDFObjectHandle.hh>

namespace ps::PdfImage {

QPDFObjectHandle embed(QPDF &pdf, const QImage &image)
{
    const QImage rgb = image.convertToFormat(QImage::Format_RGB888);
    if (rgb.isNull()) {
        return QPDFObjectHandle::newNull();
    }

    const int width = rgb.width();
    const int height = rgb.height();

    std::string pixels;
    pixels.reserve(static_cast<size_t>(width) * height * 3);
    for (int y = 0; y < height; ++y) {
        pixels.append(reinterpret_cast<const char *>(rgb.constScanLine(y)), static_cast<size_t>(width) * 3);
    }

    QPDFObjectHandle stream = QPDFObjectHandle::newStream(&pdf, pixels);
    QPDFObjectHandle dict = stream.getDict();
    dict.replaceKey("/Type", QPDFObjectHandle::newName("/XObject"));
    dict.replaceKey("/Subtype", QPDFObjectHandle::newName("/Image"));
    dict.replaceKey("/Width", QPDFObjectHandle::newInteger(width));
    dict.replaceKey("/Height", QPDFObjectHandle::newInteger(height));
    dict.replaceKey("/ColorSpace", QPDFObjectHandle::newName("/DeviceRGB"));
    dict.replaceKey("/BitsPerComponent", QPDFObjectHandle::newInteger(8));

    // A signature scanned on white paper is useless without transparency, so
    // the alpha channel becomes a soft mask rather than being thrown away.
    if (image.hasAlphaChannel()) {
        const QImage source = image.convertToFormat(QImage::Format_ARGB32);
        std::string alpha;
        alpha.reserve(static_cast<size_t>(width) * height);
        for (int y = 0; y < height; ++y) {
            const auto *line = reinterpret_cast<const QRgb *>(source.constScanLine(y));
            for (int x = 0; x < width; ++x) {
                alpha.push_back(static_cast<char>(qAlpha(line[x])));
            }
        }

        QPDFObjectHandle mask = QPDFObjectHandle::newStream(&pdf, alpha);
        QPDFObjectHandle maskDict = mask.getDict();
        maskDict.replaceKey("/Type", QPDFObjectHandle::newName("/XObject"));
        maskDict.replaceKey("/Subtype", QPDFObjectHandle::newName("/Image"));
        maskDict.replaceKey("/Width", QPDFObjectHandle::newInteger(width));
        maskDict.replaceKey("/Height", QPDFObjectHandle::newInteger(height));
        maskDict.replaceKey("/ColorSpace", QPDFObjectHandle::newName("/DeviceGray"));
        maskDict.replaceKey("/BitsPerComponent", QPDFObjectHandle::newInteger(8));
        dict.replaceKey("/SMask", mask);
    }

    return stream;
}

QPDFObjectHandle embedAsJpeg(QPDF &pdf, const QImage &image, int quality)
{
    if (image.isNull()) {
        return QPDFObjectHandle::newNull();
    }
    if (image.hasAlphaChannel()) {
        // JPEG has no alpha, and silently flattening it onto white would
        // surprise anyone stamping a signature.
        return embed(pdf, image);
    }

    QByteArray encoded;
    QBuffer buffer(&encoded);
    buffer.open(QIODevice::WriteOnly);
    if (!image.convertToFormat(QImage::Format_RGB888).save(&buffer, "JPEG", quality)) {
        return embed(pdf, image);
    }
    buffer.close();

    QPDFObjectHandle stream
        = QPDFObjectHandle::newStream(&pdf, std::string(encoded.constData(), static_cast<size_t>(encoded.size())));
    QPDFObjectHandle dict = stream.getDict();
    dict.replaceKey("/Type", QPDFObjectHandle::newName("/XObject"));
    dict.replaceKey("/Subtype", QPDFObjectHandle::newName("/Image"));
    dict.replaceKey("/Width", QPDFObjectHandle::newInteger(image.width()));
    dict.replaceKey("/Height", QPDFObjectHandle::newInteger(image.height()));
    dict.replaceKey("/ColorSpace", QPDFObjectHandle::newName("/DeviceRGB"));
    dict.replaceKey("/BitsPerComponent", QPDFObjectHandle::newInteger(8));
    // Marked as already-compressed so the writer carries the bytes across
    // untouched instead of deflating a JPEG, which only makes it bigger.
    dict.replaceKey("/Filter", QPDFObjectHandle::newName("/DCTDecode"));

    return stream;
}

} // namespace ps::PdfImage
