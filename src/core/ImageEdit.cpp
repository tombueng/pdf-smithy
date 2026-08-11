/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#include "ImageEdit.h"
#include "PdfFile.h"

#include "PdfGeometry.h"

#include <KLocalizedString>
#include <QBuffer>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QHash>

#include <qpdf/Pl_Buffer.hh>
#include <qpdf/Pl_Flate.hh>
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFObjGen.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>
#include <qpdf/QPDFTokenizer.hh>
#include <qpdf/QPDFWriter.hh>

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <utility>

using namespace Qt::Literals::StringLiterals;

namespace ps {

namespace {

// ── Reading the dictionary ────────────────────────────────────────────────

std::string nameOf(QPDFObjectHandle object, const char *key)
{
    if (!object.isDictionary()) {
        return {};
    }
    QPDFObjectHandle value = object.getKey(key);
    return value.isName() ? value.getName() : std::string();
}

QStringList filtersOf(QPDFObjectHandle dict)
{
    QStringList names;
    QPDFObjectHandle filter = dict.isDictionary() ? dict.getKey("/Filter") : QPDFObjectHandle::newNull();
    if (filter.isName()) {
        names << QString::fromStdString(filter.getName());
    } else if (filter.isArray()) {
        for (int i = 0; i < filter.getArrayNItems(); ++i) {
            QPDFObjectHandle item = filter.getArrayItem(i);
            if (item.isName()) {
                names << QString::fromStdString(item.getName());
            }
        }
    }
    return names;
}

/**
 * The filter that actually made the bytes, without its slash.
 *
 * Filters decode in the order they are listed, so the codec is the last one:
 * `[/ASCII85Decode /DCTDecode]` is a JPEG wrapped in text, not an ASCII85 image.
 */
QString encodingOf(QPDFObjectHandle dict)
{
    const QStringList filters = filtersOf(dict);
    if (filters.isEmpty()) {
        return u"none"_s;
    }
    return filters.constLast().mid(1);
}

/** How the pixels of a colour space are laid out, and whether we can read them. */
struct Colour {
    QString label;
    /** Samples per stored pixel; zero when this is a space we decline to read. */
    int components = 0;
    bool indexed = false;
    QVector<QRgb> palette;
};

QRgb fromComponents(const uchar *samples, int components)
{
    if (components == 1) {
        return qRgb(samples[0], samples[0], samples[0]);
    }
    if (components == 3) {
        return qRgb(samples[0], samples[1], samples[2]);
    }
    // The multiplicative conversion rather than the subtractive one: 100% cyan
    // over 100% black should stay black, and c + k clipped turns it grey.
    const int c = samples[0];
    const int m = samples[1];
    const int y = samples[2];
    const int k = samples[3];
    return qRgb((255 - c) * (255 - k) / 255, (255 - m) * (255 - k) / 255, (255 - y) * (255 - k) / 255);
}

Colour readColourSpace(QPDFObjectHandle space);

/** The bytes of an /Indexed lookup, which the file may store either way round. */
QByteArray lookupBytes(QPDFObjectHandle lookup)
{
    if (lookup.isString()) {
        return QByteArray::fromStdString(lookup.getStringValue());
    }
    if (!lookup.isStream()) {
        return {};
    }
    Pl_Buffer buffer("palette");
    bool attempted = false;
    if (!lookup.pipeStreamData(&buffer, &attempted, 0, qpdf_dl_all, true, false)) {
        return {};
    }
    const auto data = buffer.getBufferSharedPointer();
    return QByteArray(reinterpret_cast<const char *>(data->getBuffer()), qsizetype(data->getSize()));
}

Colour readColourSpace(QPDFObjectHandle space)
{
    Colour colour;
    if (space.isName()) {
        const std::string name = space.getName();
        if (name == "/DeviceGray" || name == "/G" || name == "/CalGray") {
            return { u"DeviceGray"_s, 1, false, {} };
        }
        if (name == "/DeviceRGB" || name == "/RGB" || name == "/CalRGB") {
            return { u"DeviceRGB"_s, 3, false, {} };
        }
        if (name == "/DeviceCMYK" || name == "/CMYK") {
            return { u"DeviceCMYK"_s, 4, false, {} };
        }
        colour.label = QString::fromStdString(name).mid(1);
        return colour;
    }

    if (!space.isArray() || space.getArrayNItems() < 2) {
        return colour;
    }

    QPDFObjectHandle family = space.getArrayItem(0);
    const std::string name = family.isName() ? family.getName() : std::string();

    if (name == "/ICCBased") {
        QPDFObjectHandle stream = space.getArrayItem(1);
        QPDFObjectHandle n = stream.isStream() ? stream.getDict().getKey("/N") : QPDFObjectHandle::newNull();
        const int components = n.isInteger() ? n.getIntValueAsInt() : 0;
        colour.label = i18nc("an ICC colour profile with a number of components", "ICCBased(%1)", components);
        // The profile itself is not applied; an ICC space with three components
        // is read as if it were sRGB, which is what every viewer falls back to
        // when the profile is broken and is close enough to be recognisable.
        colour.components = (components == 1 || components == 3 || components == 4) ? components : 0;
        return colour;
    }
    if (name == "/CalGray") {
        return { u"CalGray"_s, 1, false, {} };
    }
    if (name == "/CalRGB") {
        return { u"CalRGB"_s, 3, false, {} };
    }
    if (name == "/Indexed" || name == "/I") {
        colour.label = u"Indexed"_s;
        colour.indexed = true;
        colour.components = 1;
        if (space.getArrayNItems() < 4) {
            colour.components = 0;
            return colour;
        }
        const Colour base = readColourSpace(space.getArrayItem(1));
        QPDFObjectHandle hivalKey = space.getArrayItem(2);
        const int hival = hivalKey.isInteger() ? hivalKey.getIntValueAsInt() : 0;
        const QByteArray table = lookupBytes(space.getArrayItem(3));
        if (base.components <= 0 || base.indexed || hival < 0
            || table.size() < qsizetype(hival + 1) * base.components) {
            colour.components = 0;
            return colour;
        }
        colour.palette.reserve(hival + 1);
        for (int index = 0; index <= hival; ++index) {
            colour.palette.append(fromComponents(
                reinterpret_cast<const uchar *>(table.constData()) + index * base.components, base.components));
        }
        return colour;
    }
    if (name == "/DeviceN") {
        QPDFObjectHandle names = space.getArrayItem(1);
        colour.label = i18nc("a DeviceN colour space with a number of colourants", "DeviceN(%1)",
                             names.isArray() ? names.getArrayNItems() : 0);
        return colour;
    }

    // /Separation and /Lab both need a transform to mean anything, and reading
    // their samples as grey or as RGB would produce a confidently wrong picture.
    colour.label = QString::fromStdString(name).mid(1);
    return colour;
}

/** True when the pixels of @p image can be opened, decided without opening them. */
bool looksDecodable(QPDFObjectHandle image)
{
    QPDFObjectHandle dict = image.getDict();
    if (dict.getKey("/ImageMask").isBool() && dict.getKey("/ImageMask").getBoolValue()) {
        return false;
    }

    const QStringList filters = filtersOf(dict);
    if (filters.contains(u"/JPXDecode"_s) || filters.contains(u"/CCITTFaxDecode"_s)
        || filters.contains(u"/JBIG2Decode"_s)) {
        return false;
    }

    const Colour colour = readColourSpace(dict.getKey("/ColorSpace"));
    QPDFObjectHandle bits = dict.getKey("/BitsPerComponent");
    const int bitsPerComponent = bits.isInteger() ? bits.getIntValueAsInt() : 8;

    if (filters.contains(u"/DCTDecode"_s)) {
        // Qt reads grey and colour JPEGs. A four-component Adobe JPEG it may or
        // may not read depending on the platform's libjpeg, and a promise that
        // only sometimes holds is worse than a refusal.
        return colour.components == 1 || colour.components == 3;
    }

    if (colour.indexed) {
        return bitsPerComponent == 1 || bitsPerComponent == 2 || bitsPerComponent == 4 || bitsPerComponent == 8;
    }
    if (colour.components == 1) {
        return bitsPerComponent == 1 || bitsPerComponent == 2 || bitsPerComponent == 4 || bitsPerComponent == 8;
    }
    return (colour.components == 3 || colour.components == 4) && bitsPerComponent == 8;
}

bool isImageMask(QPDFObjectHandle dict)
{
    QPDFObjectHandle mask = dict.isDictionary() ? dict.getKey("/ImageMask") : QPDFObjectHandle::newNull();
    return mask.isBool() && mask.getBoolValue();
}

qint64 storedBytesOf(QPDFObjectHandle stream)
{
    QPDFObjectHandle length = stream.getDict().getKey("/Length");
    if (length.isInteger()) {
        return qint64(length.getIntValue());
    }
    return 0;
}

// ── Getting at the pixels ─────────────────────────────────────────────────

/** One sample out of a packed row, for the bit depths a PDF is allowed to use. */
int sampleAt(const uchar *row, qsizetype index, int bits)
{
    switch (bits) {
    case 8:
        return row[index];
    case 4:
        return (row[index / 2] >> (index % 2 == 0 ? 4 : 0)) & 0x0f;
    case 2:
        return (row[index / 4] >> (6 - 2 * (index % 4))) & 0x03;
    case 1:
        return (row[index / 8] >> (7 - index % 8)) & 0x01;
    default:
        return 0;
    }
}

/** True when /Decode turns the samples round, as scanners and Adobe both do. */
bool decodeIsInverted(QPDFObjectHandle dict)
{
    QPDFObjectHandle decode = dict.getKey("/Decode");
    if (!decode.isArray() || decode.getArrayNItems() < 2) {
        return false;
    }
    return PdfGeometry::numberAt(decode, 0, 0.0) > PdfGeometry::numberAt(decode, 1, 1.0);
}

/** The stream with every filter QPDF understands taken off, or empty on failure. */
QByteArray unfilteredBytes(QPDFObjectHandle stream, qpdf_stream_decode_level_e level)
{
    Pl_Buffer buffer("samples");
    bool attempted = false;
    // The six-argument overload deliberately: the short one reports whether
    // filtering was attempted, not whether it worked, so a stream left
    // untouched on purpose looks exactly like a failure.
    if (!stream.pipeStreamData(&buffer, &attempted, 0, level, true, false)) {
        return {};
    }
    if (!attempted && !filtersOf(stream.getDict()).isEmpty() && level == qpdf_dl_all) {
        // Something is still wrapped round the samples, so what came out is not
        // pixels.
        return {};
    }
    const auto data = buffer.getBufferSharedPointer();
    return QByteArray(reinterpret_cast<const char *>(data->getBuffer()), qsizetype(data->getSize()));
}

/** What decoding found out, so that re-encoding can match it. */
struct Decoded {
    QImage image;
    /** The bytes were a JPEG, so writing a JPEG back costs one generation, not two. */
    bool wasJpeg = false;
    /** One stored sample per pixel, so grey should stay grey. */
    bool wasGrey = false;
};

Decoded decodeImage(QPDFObjectHandle image)
{
    Decoded result;
    QPDFObjectHandle dict = image.getDict();

    QPDFObjectHandle widthKey = dict.getKey("/Width");
    QPDFObjectHandle heightKey = dict.getKey("/Height");
    const int width = widthKey.isInteger() ? widthKey.getIntValueAsInt() : 0;
    const int height = heightKey.isInteger() ? heightKey.getIntValueAsInt() : 0;
    if (width <= 0 || height <= 0) {
        return result;
    }

    const Colour colour = readColourSpace(dict.getKey("/ColorSpace"));
    result.wasGrey = colour.components == 1 && !colour.indexed;

    if (filtersOf(dict).contains(u"/DCTDecode"_s)) {
        // Everything before the JPEG comes off; the JPEG itself goes to Qt.
        const QByteArray jpeg = unfilteredBytes(image, qpdf_dl_specialized);
        result.image = QImage::fromData(jpeg);
        result.wasJpeg = !result.image.isNull();
        result.wasGrey = result.image.format() == QImage::Format_Grayscale8;
        return result;
    }

    QPDFObjectHandle bits = dict.getKey("/BitsPerComponent");
    const int bitsPerComponent = bits.isInteger() ? bits.getIntValueAsInt() : 8;
    const int components = colour.indexed ? 1 : colour.components;
    if (components <= 0 || bitsPerComponent <= 0) {
        return result;
    }

    const QByteArray samples = unfilteredBytes(image, qpdf_dl_all);
    const qsizetype stride = (qsizetype(width) * components * bitsPerComponent + 7) / 8;
    if (samples.size() < stride * height) {
        return result;
    }

    const bool inverted = decodeIsInverted(dict);
    const int maximum = (1 << bitsPerComponent) - 1;
    const auto *bytes = reinterpret_cast<const uchar *>(samples.constData());

    if (colour.indexed) {
        QImage canvas(width, height, QImage::Format_RGB32);
        if (canvas.isNull()) {
            return result;
        }
        for (int y = 0; y < height; ++y) {
            const uchar *row = bytes + stride * y;
            auto *out = reinterpret_cast<QRgb *>(canvas.scanLine(y));
            for (int x = 0; x < width; ++x) {
                const int index = sampleAt(row, x, bitsPerComponent);
                out[x] = index < colour.palette.size() ? colour.palette.at(index) : qRgb(0, 0, 0);
            }
        }
        result.image = canvas;
        return result;
    }

    if (components == 1) {
        QImage canvas(width, height, QImage::Format_Grayscale8);
        if (canvas.isNull()) {
            return result;
        }
        for (int y = 0; y < height; ++y) {
            const uchar *row = bytes + stride * y;
            uchar *out = canvas.scanLine(y);
            for (int x = 0; x < width; ++x) {
                const int value = sampleAt(row, x, bitsPerComponent) * 255 / maximum;
                out[x] = uchar(inverted ? 255 - value : value);
            }
        }
        result.image = canvas;
        return result;
    }

    QImage canvas(width, height, QImage::Format_RGB32);
    if (canvas.isNull()) {
        return result;
    }
    for (int y = 0; y < height; ++y) {
        const uchar *row = bytes + stride * y;
        auto *out = reinterpret_cast<QRgb *>(canvas.scanLine(y));
        for (int x = 0; x < width; ++x) {
            std::array<uchar, 4> pixel {};
            for (int component = 0; component < components; ++component) {
                const int value = row[x * components + component];
                pixel[size_t(component)] = uchar(inverted ? 255 - value : value);
            }
            out[x] = fromComponents(pixel.data(), components);
        }
    }
    result.image = canvas;
    return result;
}

/**
 * The soft mask of @p image as an 8-bit picture.
 *
 * @p present says whether there was one at all, which is what separates "opaque"
 * from "transparent in a way we cannot read".
 */
QImage decodeSoftMask(QPDFObjectHandle image, bool *present)
{
    QPDFObjectHandle mask = image.getDict().getKey("/SMask");
    *present = mask.isStream();
    if (!*present) {
        return {};
    }
    if (!looksDecodable(mask)) {
        return {};
    }
    const Decoded decoded = decodeImage(mask);
    if (decoded.image.isNull()) {
        return {};
    }
    return decoded.image.convertToFormat(QImage::Format_Grayscale8);
}

// ── Putting the pixels back ───────────────────────────────────────────────

std::string deflated(const QByteArray &raw)
{
    Pl_Buffer sink("deflated");
    Pl_Flate flate("deflate", &sink, Pl_Flate::a_deflate);
    flate.write(reinterpret_cast<const unsigned char *>(raw.constData()), size_t(raw.size()));
    flate.finish();
    const auto buffer = sink.getBufferSharedPointer();
    return std::string(reinterpret_cast<const char *>(buffer->getBuffer()), buffer->getSize());
}

/** Compressed bytes together with the dictionary entries that describe them. */
struct Encoded {
    std::string data;
    QString filter;
    int width = 0;
    int height = 0;
    int bits = 8;
    QString colourSpace;
    /** Deflated 8-bit alpha, empty when the picture is opaque. */
    std::string softMask;

    bool isValid() const { return !data.empty() && width > 0 && height > 0; }
};

/**
 * How many components a JPEG really has, read from its own markers.
 *
 * Qt decides for itself whether a grey image becomes a one-component JPEG, and a
 * dictionary that disagrees with the codestream is a picture no reader can show.
 * Asking the bytes is the only answer that cannot be wrong.
 */
int jpegComponents(const QByteArray &jpeg)
{
    qsizetype at = 2;
    while (at + 9 < jpeg.size()) {
        if (uchar(jpeg.at(at)) != 0xff) {
            ++at;
            continue;
        }
        const uchar marker = uchar(jpeg.at(at + 1));
        if (marker == 0xd8 || marker == 0x01 || (marker >= 0xd0 && marker <= 0xd7)) {
            at += 2;
            continue;
        }
        const bool isStartOfFrame
            = marker >= 0xc0 && marker <= 0xcf && marker != 0xc4 && marker != 0xc8 && marker != 0xcc;
        if (isStartOfFrame) {
            return uchar(jpeg.at(at + 9));
        }
        at += 2 + ((uchar(jpeg.at(at + 2)) << 8) | uchar(jpeg.at(at + 3)));
    }
    return 0;
}

Encoded encodeJpeg(const QImage &image, int quality)
{
    Encoded encoded;
    QImage source = image;
    if (source.format() != QImage::Format_Grayscale8) {
        source = source.convertToFormat(QImage::Format_RGB888);
    }
    if (source.isNull()) {
        return encoded;
    }

    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    if (!source.save(&buffer, "JPEG", quality)) {
        return encoded;
    }
    buffer.close();

    const int components = jpegComponents(bytes);
    if (components != 1 && components != 3) {
        return encoded;
    }

    encoded.data = std::string(bytes.constData(), size_t(bytes.size()));
    encoded.filter = u"/DCTDecode"_s;
    encoded.width = source.width();
    encoded.height = source.height();
    encoded.bits = 8;
    encoded.colourSpace = components == 1 ? u"/DeviceGray"_s : u"/DeviceRGB"_s;
    return encoded;
}

Encoded encodeFlate(const QImage &image, bool grey)
{
    Encoded encoded;
    const QImage source = image.convertToFormat(grey ? QImage::Format_Grayscale8 : QImage::Format_RGB888);
    if (source.isNull()) {
        return encoded;
    }

    const qsizetype rowBytes = qsizetype(source.width()) * (grey ? 1 : 3);
    QByteArray raw;
    raw.reserve(rowBytes * source.height());
    for (int y = 0; y < source.height(); ++y) {
        raw.append(reinterpret_cast<const char *>(source.constScanLine(y)), rowBytes);
    }

    encoded.data = deflated(raw);
    encoded.filter = u"/FlateDecode"_s;
    encoded.width = source.width();
    encoded.height = source.height();
    encoded.bits = 8;
    encoded.colourSpace = grey ? u"/DeviceGray"_s : u"/DeviceRGB"_s;
    return encoded;
}

/**
 * One bit per pixel, packed by hand rather than through Qt's Format_Mono.
 *
 * Qt's mono conversion carries a colour table that may put black at either
 * index, whereas /DeviceGray fixes zero as black. Writing the bits directly
 * removes the question.
 */
Encoded encodeMono(const QImage &image, int threshold)
{
    Encoded encoded;
    const QImage grey = image.convertToFormat(QImage::Format_Grayscale8);
    if (grey.isNull()) {
        return encoded;
    }

    const qsizetype stride = (qsizetype(grey.width()) + 7) / 8;
    QByteArray raw(stride * grey.height(), '\0');
    for (int y = 0; y < grey.height(); ++y) {
        const uchar *in = grey.constScanLine(y);
        auto *out = reinterpret_cast<uchar *>(raw.data()) + stride * y;
        for (int x = 0; x < grey.width(); ++x) {
            if (in[x] >= threshold) {
                out[x / 8] |= uchar(0x80 >> (x % 8));
            }
        }
    }

    encoded.data = deflated(raw);
    encoded.filter = u"/FlateDecode"_s;
    encoded.width = grey.width();
    encoded.height = grey.height();
    encoded.bits = 1;
    encoded.colourSpace = u"/DeviceGray"_s;
    return encoded;
}

/** Deflated 8-bit alpha for a picture that has any, ready to become an /SMask. */
std::string encodeAlpha(const QImage &image)
{
    if (!image.hasAlphaChannel()) {
        return {};
    }
    const QImage source = image.convertToFormat(QImage::Format_ARGB32);
    if (source.isNull()) {
        return {};
    }
    QByteArray raw;
    raw.reserve(qsizetype(source.width()) * source.height());
    for (int y = 0; y < source.height(); ++y) {
        const auto *line = reinterpret_cast<const QRgb *>(source.constScanLine(y));
        for (int x = 0; x < source.width(); ++x) {
            raw.append(char(qAlpha(line[x])));
        }
    }
    return deflated(raw);
}

/** A soft-mask stream from an already-decoded 8-bit mask. */
QPDFObjectHandle makeSoftMask(QPDF &pdf, const QImage &mask)
{
    const Encoded encoded = encodeFlate(mask, true);
    if (!encoded.isValid()) {
        return QPDFObjectHandle::newNull();
    }
    QPDFObjectHandle stream = QPDFObjectHandle::newStream(&pdf);
    stream.replaceStreamData(encoded.data, QPDFObjectHandle::newName(encoded.filter.toStdString()),
                             QPDFObjectHandle::newNull());
    QPDFObjectHandle dict = stream.getDict();
    dict.replaceKey("/Type", QPDFObjectHandle::newName("/XObject"));
    dict.replaceKey("/Subtype", QPDFObjectHandle::newName("/Image"));
    dict.replaceKey("/Width", QPDFObjectHandle::newInteger(encoded.width));
    dict.replaceKey("/Height", QPDFObjectHandle::newInteger(encoded.height));
    dict.replaceKey("/ColorSpace", QPDFObjectHandle::newName("/DeviceGray"));
    dict.replaceKey("/BitsPerComponent", QPDFObjectHandle::newInteger(8));
    return stream;
}

/** Writes @p encoded into @p stream, which may be a fresh object or an old one. */
void fillImageStream(QPDF &pdf, QPDFObjectHandle stream, const Encoded &encoded)
{
    stream.replaceStreamData(encoded.data, QPDFObjectHandle::newName(encoded.filter.toStdString()),
                             QPDFObjectHandle::newNull());
    QPDFObjectHandle dict = stream.getDict();
    dict.replaceKey("/Type", QPDFObjectHandle::newName("/XObject"));
    dict.replaceKey("/Subtype", QPDFObjectHandle::newName("/Image"));
    dict.replaceKey("/Width", QPDFObjectHandle::newInteger(encoded.width));
    dict.replaceKey("/Height", QPDFObjectHandle::newInteger(encoded.height));
    dict.replaceKey("/ColorSpace", QPDFObjectHandle::newName(encoded.colourSpace.toStdString()));
    dict.replaceKey("/BitsPerComponent", QPDFObjectHandle::newInteger(encoded.bits));
    // A /Decode written for the old samples, or /DecodeParms describing the old
    // filter, would be read against the new ones.
    dict.removeKey("/Decode");
    dict.removeKey("/ImageMask");

    if (encoded.softMask.empty()) {
        return;
    }
    QPDFObjectHandle mask = QPDFObjectHandle::newStream(&pdf);
    mask.replaceStreamData(encoded.softMask, QPDFObjectHandle::newName("/FlateDecode"), QPDFObjectHandle::newNull());
    QPDFObjectHandle maskDict = mask.getDict();
    maskDict.replaceKey("/Type", QPDFObjectHandle::newName("/XObject"));
    maskDict.replaceKey("/Subtype", QPDFObjectHandle::newName("/Image"));
    maskDict.replaceKey("/Width", QPDFObjectHandle::newInteger(encoded.width));
    maskDict.replaceKey("/Height", QPDFObjectHandle::newInteger(encoded.height));
    maskDict.replaceKey("/ColorSpace", QPDFObjectHandle::newName("/DeviceGray"));
    maskDict.replaceKey("/BitsPerComponent", QPDFObjectHandle::newInteger(8));
    dict.replaceKey("/SMask", mask);
}

QPDFObjectHandle makeImageStream(QPDF &pdf, const Encoded &encoded)
{
    QPDFObjectHandle stream = QPDFObjectHandle::newStream(&pdf);
    fillImageStream(pdf, stream, encoded);
    return stream;
}

/** Carries the entries that describe how a picture is shown, not what it holds. */
void copyPresentation(QPDFObjectHandle from, QPDFObjectHandle to)
{
    for (const char *key : { "/Intent", "/Interpolate", "/Mask", "/SMask" }) {
        QPDFObjectHandle value = from.getDict().getKey(key);
        if (!value.isNull()) {
            to.getDict().replaceKey(key, value);
        }
    }
}

// ── Pixel work ───────────────────────────────────────────────────────────

void applyCurve(QImage &image, const std::array<uchar, 256> &table)
{
    if (image.format() == QImage::Format_Grayscale8) {
        for (int y = 0; y < image.height(); ++y) {
            uchar *line = image.scanLine(y);
            for (int x = 0; x < image.width(); ++x) {
                line[x] = table[line[x]];
            }
        }
        return;
    }
    for (int y = 0; y < image.height(); ++y) {
        auto *line = reinterpret_cast<QRgb *>(image.scanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            const QRgb pixel = line[x];
            line[x] = qRgba(table[qRed(pixel)], table[qGreen(pixel)], table[qBlue(pixel)], qAlpha(pixel));
        }
    }
}

/** A 3×3 median, which is what removes scanner dust without softening edges. */
QImage medianFiltered(const QImage &image)
{
    QImage out = image;
    const int width = image.width();
    const int height = image.height();
    for (int y = 0; y < height; ++y) {
        auto *outLine = reinterpret_cast<QRgb *>(out.scanLine(y));
        for (int x = 0; x < width; ++x) {
            std::array<int, 9> red {};
            std::array<int, 9> green {};
            std::array<int, 9> blue {};
            int count = 0;
            for (int dy = -1; dy <= 1; ++dy) {
                const int sy = std::clamp(y + dy, 0, height - 1);
                const auto *line = reinterpret_cast<const QRgb *>(image.constScanLine(sy));
                for (int dx = -1; dx <= 1; ++dx) {
                    const QRgb pixel = line[std::clamp(x + dx, 0, width - 1)];
                    red[size_t(count)] = qRed(pixel);
                    green[size_t(count)] = qGreen(pixel);
                    blue[size_t(count)] = qBlue(pixel);
                    ++count;
                }
            }
            std::nth_element(red.begin(), red.begin() + 4, red.end());
            std::nth_element(green.begin(), green.begin() + 4, green.end());
            std::nth_element(blue.begin(), blue.begin() + 4, blue.end());
            outLine[x] = qRgb(red[4], green[4], blue[4]);
        }
    }
    return out;
}

QImage sharpened(const QImage &image)
{
    QImage out = image;
    const int width = image.width();
    const int height = image.height();
    for (int y = 0; y < height; ++y) {
        auto *outLine = reinterpret_cast<QRgb *>(out.scanLine(y));
        const auto *middle = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        const auto *above = reinterpret_cast<const QRgb *>(image.constScanLine(std::max(0, y - 1)));
        const auto *below = reinterpret_cast<const QRgb *>(image.constScanLine(std::min(height - 1, y + 1)));
        for (int x = 0; x < width; ++x) {
            const int left = std::max(0, x - 1);
            const int right = std::min(width - 1, x + 1);
            const auto channel = [&](int (*take)(QRgb)) {
                const int value
                    = 5 * take(middle[x]) - take(above[x]) - take(below[x]) - take(middle[left]) - take(middle[right]);
                return std::clamp(value, 0, 255);
            };
            outLine[x] = qRgb(channel(qRed), channel(qGreen), channel(qBlue));
        }
    }
    return out;
}

/** Stretches the tones so the darkest half-percent is black and the lightest white. */
void autoLevel(QImage &image)
{
    std::array<int, 256> histogram {};
    const bool grey = image.format() == QImage::Format_Grayscale8;
    for (int y = 0; y < image.height(); ++y) {
        if (grey) {
            const uchar *line = image.constScanLine(y);
            for (int x = 0; x < image.width(); ++x) {
                ++histogram[line[x]];
            }
            continue;
        }
        const auto *line = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            ++histogram[size_t(qGray(line[x]))];
        }
    }

    const qint64 total = qint64(image.width()) * image.height();
    // Clipping rather than taking the extremes: a single dark speck or a white
    // border pixel would otherwise decide the whole curve and change nothing.
    const qint64 clip = std::max<qint64>(1, total / 200);
    int low = 0;
    int high = 255;
    qint64 seen = 0;
    for (int value = 0; value < 256; ++value) {
        seen += histogram[size_t(value)];
        if (seen >= clip) {
            low = value;
            break;
        }
    }
    seen = 0;
    for (int value = 255; value >= 0; --value) {
        seen += histogram[size_t(value)];
        if (seen >= clip) {
            high = value;
            break;
        }
    }
    if (high <= low) {
        return;
    }

    std::array<uchar, 256> table {};
    for (int value = 0; value < 256; ++value) {
        table[size_t(value)] = uchar(std::clamp((value - low) * 255 / (high - low), 0, 255));
    }
    applyCurve(image, table);
}

void applyAdjust(QImage &image, const ImageEdit::Adjust &options)
{
    if (options.autoLevels) {
        autoLevel(image);
    }

    const bool changesTone
        = !qFuzzyIsNull(options.brightness) || !qFuzzyIsNull(options.contrast) || !qFuzzyCompare(options.gamma, 1.0);
    if (changesTone) {
        // Contrast pivots on mid-grey and saturates rather than wrapping, so a
        // heavy setting flattens the picture instead of turning it inside out.
        const double contrast = std::clamp(options.contrast, -0.99, 0.99);
        const double slope = (1.0 + contrast) / (1.0 - contrast);
        const double gamma = options.gamma > 0.0 ? options.gamma : 1.0;
        std::array<uchar, 256> table {};
        for (int value = 0; value < 256; ++value) {
            double level = value / 255.0;
            level = std::pow(level, 1.0 / gamma);
            level = (level - 0.5) * slope + 0.5 + options.brightness;
            table[size_t(value)] = uchar(std::clamp(int(std::lround(level * 255.0)), 0, 255));
        }
        applyCurve(image, table);
    }

    if (options.despeckle || options.sharpen) {
        // Both kernels read neighbours as RGB, so a grey picture is widened for
        // the duration and narrowed again by the encoder afterwards.
        QImage colour = image.convertToFormat(QImage::Format_RGB32);
        if (options.despeckle) {
            colour = medianFiltered(colour);
        }
        if (options.sharpen) {
            colour = sharpened(colour);
        }
        image = colour;
    }
}

// ── Finding where pictures are drawn ─────────────────────────────────────

/** One `Do` that draws an image, with the matrix that put it there. */
struct Placement {
    int page = 0;
    QString name;
    QPDFObjGen object;
    /** Unit square to unrotated page space. */
    QTransform ctm;
};

class PlacementScanner : public QPDFObjectHandle::TokenFilter
{
public:
    PlacementScanner(QPDFObjectHandle xobjects, int page, QVector<Placement> *into)
        : m_xobjects(std::move(xobjects))
        , m_page(page)
        , m_into(into)
    {
    }

    void handleToken(QPDFTokenizer::Token const &token) override;

private:
    double operandNumber(int index) const
    {
        if (index < 0 || index >= m_operands.size()) {
            return 0.0;
        }
        // QString::toDouble is C-locale whatever the user's is, unlike strtod.
        return QString::fromStdString(m_operands.at(index).getValue()).toDouble();
    }

    QPDFObjectHandle m_xobjects;
    int m_page = 0;
    QVector<Placement> *m_into = nullptr;
    QVector<QPDFTokenizer::Token> m_operands;
    QTransform m_ctm;
    QVector<QTransform> m_stack;
};

void PlacementScanner::handleToken(QPDFTokenizer::Token const &token)
{
    if (token.getType() == QPDFTokenizer::tt_space || token.getType() == QPDFTokenizer::tt_comment
        || token.getType() == QPDFTokenizer::tt_eof) {
        return;
    }
    if (token.getType() != QPDFTokenizer::tt_word) {
        m_operands.append(token);
        return;
    }

    const QString op = QString::fromStdString(token.getValue());
    if (op == u"q"_s) {
        m_stack.append(m_ctm);
    } else if (op == u"Q"_s) {
        if (!m_stack.isEmpty()) {
            m_ctm = m_stack.takeLast();
        }
    } else if (op == u"cm"_s && m_operands.size() >= 6) {
        m_ctm = QTransform(operandNumber(0), operandNumber(1), operandNumber(2), operandNumber(3), operandNumber(4),
                           operandNumber(5))
            * m_ctm;
    } else if (op == u"Do"_s && !m_operands.isEmpty() && m_operands.constLast().getType() == QPDFTokenizer::tt_name) {
        const std::string name = m_operands.constLast().getValue();
        QPDFObjectHandle xobject = m_xobjects.isDictionary() ? m_xobjects.getKey(name) : QPDFObjectHandle::newNull();
        if (xobject.isStream() && nameOf(xobject.getDict(), "/Subtype") == "/Image") {
            m_into->append({ m_page, QString::fromStdString(name), xobject.getObjGen(), m_ctm });
        }
    }
    m_operands.clear();
}

/** What a placement measures out to, once the page's own rotation is allowed for. */
struct Geometry {
    QRectF display;
    /** The picture's own width and height where it landed, in points. */
    double pointsX = 0.0;
    double pointsY = 0.0;
};

Geometry measure(const Placement &placement, const QRectF &media, int rotate)
{
    Geometry geometry;
    const QTransform &ctm = placement.ctm;
    // The lengths of the images of (1,0) and (0,1): a turned or mirrored
    // placement covers the same number of points along the picture's own axes,
    // and it is those that the pixels have to cover.
    geometry.pointsX = std::hypot(ctm.m11(), ctm.m12());
    geometry.pointsY = std::hypot(ctm.m21(), ctm.m22());

    const QTransform toDisplay = PdfGeometry::displayToPageTransform(rotate, media.width(), media.height()).inverted();
    geometry.display = toDisplay.mapRect(ctm.mapRect(QRectF(0, 0, 1, 1)).translated(-media.x(), -media.y()));
    return geometry;
}

/** Everything every page draws, with the counts that say what is shared. */
QVector<Placement> scanDocument(std::vector<QPDFPageObjectHelper> &pages)
{
    QVector<Placement> placements;
    for (size_t index = 0; index < pages.size(); ++index) {
        QPDFPageObjectHelper page = pages.at(index);
        QPDFObjectHandle resources = page.getAttribute("/Resources", false);
        QPDFObjectHandle xobjects
            = resources.isDictionary() ? resources.getKey("/XObject") : QPDFObjectHandle::newNull();
        if (!xobjects.isDictionary()) {
            continue;
        }
        PlacementScanner scanner(xobjects, int(index), &placements);
        page.filterContents(&scanner, nullptr);
    }
    return placements;
}

// ── Changing what a page draws ───────────────────────────────────────────

/** A matrix as the six numbers and the operator of a `cm`, in the C locale whatever the desktop's is. */
std::string cmOperator(const QTransform &matrix)
{
    return PdfGeometry::number(matrix.m11()) + " " + PdfGeometry::number(matrix.m12()) + " "
        + PdfGeometry::number(matrix.m21()) + " " + PdfGeometry::number(matrix.m22()) + " "
        + PdfGeometry::number(matrix.dx()) + " " + PdfGeometry::number(matrix.dy()) + " cm\n";
}

/** What should happen to the `Do` that draws one particular name. */
class DrawRewriter : public QPDFObjectHandle::TokenFilter
{
public:
    DrawRewriter(QString target, QString replacement, bool wrap, QTransform prefix)
        : m_target(std::move(target))
        , m_replacement(std::move(replacement))
        , m_wrap(wrap)
        , m_prefix(prefix)
    {
    }

    void handleToken(QPDFTokenizer::Token const &token) override;

    int rewritten() const { return m_rewritten; }

private:
    void flush(const QString &op);

    QString m_target;
    QString m_replacement;
    bool m_wrap = false;
    QTransform m_prefix;
    QVector<QPDFTokenizer::Token> m_operands;
    int m_rewritten = 0;
};

void DrawRewriter::flush(const QString &op)
{
    for (const QPDFTokenizer::Token &token : std::as_const(m_operands)) {
        writeToken(token);
        write(" ");
    }
    write(op.toStdString());
    write("\n");
}

void DrawRewriter::handleToken(QPDFTokenizer::Token const &token)
{
    switch (token.getType()) {
    case QPDFTokenizer::tt_space:
    case QPDFTokenizer::tt_comment:
        // Between operands the whitespace is put back by flush(); on its own it
        // is worth keeping so the stream stays readable.
        if (m_operands.isEmpty()) {
            writeToken(token);
        }
        return;

    case QPDFTokenizer::tt_inline_image:
        writeToken(token);
        m_operands.clear();
        return;

    case QPDFTokenizer::tt_eof:
        return;

    case QPDFTokenizer::tt_word:
        break;

    default:
        m_operands.append(token);
        return;
    }

    const QString op = QString::fromStdString(token.getValue());
    const bool isTarget = op == u"Do"_s && !m_operands.isEmpty()
        && m_operands.constLast().getType() == QPDFTokenizer::tt_name
        && QString::fromStdString(m_operands.constLast().getValue()) == m_target;
    if (!isTarget) {
        flush(op);
        m_operands.clear();
        return;
    }

    ++m_rewritten;
    if (!m_replacement.isEmpty()) {
        if (m_wrap) {
            // The extra matrix goes inside its own q/Q, because `cm`
            // concatenates and anything drawn afterwards would inherit it.
            write("q ");
            write(PdfGeometry::number(m_prefix.m11()) + " " + PdfGeometry::number(m_prefix.m12()) + " "
                  + PdfGeometry::number(m_prefix.m21()) + " " + PdfGeometry::number(m_prefix.m22()) + " "
                  + PdfGeometry::number(m_prefix.dx()) + " " + PdfGeometry::number(m_prefix.dy()) + " cm\n");
        }
        writeToken(QPDFTokenizer::Token(QPDFTokenizer::tt_name, m_replacement.toStdString()));
        write(" Do\n");
        if (m_wrap) {
            write("Q\n");
        }
    }
    m_operands.clear();
}

/**
 * Turns every placement of one name, by changing the matrix rather than the pixels.
 *
 * A `cm` in the stream acts *before* the one already in force, so a movement
 * described in page space ("turn this a quarter turn about where it sits")
 * cannot be written down directly. It has to be conjugated by the matrix in
 * force at that point: `CTM × T × CTM⁻¹`. Applying T on its own instead turns
 * the picture about the page's origin and scales the movement by whatever the
 * placement's own scaling happens to be, which looks like a mystery rather than
 * a bug, and only looks right for a picture that happens to sit at the corner
 * of the page at unit scale.
 *
 * Each placement needs its own answer, which is why this tracks the matrix
 * itself instead of taking a prefix the way DrawRewriter does.
 */
class TurnRewriter : public QPDFObjectHandle::TokenFilter
{
public:
    TurnRewriter(QString target, double degrees)
        : m_target(std::move(target))
        , m_degrees(degrees)
    {
    }

    void handleToken(QPDFTokenizer::Token const &token) override;

    int turned() const { return m_turned; }
    int refused() const { return m_refused; }
    QVector<QRectF> results() const { return m_results; }

private:
    double operandNumber(int index) const
    {
        if (index < 0 || index >= m_operands.size()) {
            return 0.0;
        }
        // QString::toDouble is C-locale whatever the desktop's is, unlike strtod.
        return QString::fromStdString(m_operands.at(index).getValue()).toDouble();
    }

    void flush(const QString &op);

    QString m_target;
    double m_degrees = 0.0;
    QVector<QPDFTokenizer::Token> m_operands;
    QTransform m_ctm;
    QVector<QTransform> m_stack;
    int m_turned = 0;
    int m_refused = 0;
    QVector<QRectF> m_results;
};

void TurnRewriter::flush(const QString &op)
{
    for (const QPDFTokenizer::Token &token : std::as_const(m_operands)) {
        writeToken(token);
        write(" ");
    }
    write(op.toStdString());
    write("\n");
}

void TurnRewriter::handleToken(QPDFTokenizer::Token const &token)
{
    switch (token.getType()) {
    case QPDFTokenizer::tt_space:
    case QPDFTokenizer::tt_comment:
        if (m_operands.isEmpty()) {
            writeToken(token);
        }
        return;

    case QPDFTokenizer::tt_inline_image:
        writeToken(token);
        m_operands.clear();
        return;

    case QPDFTokenizer::tt_eof:
        return;

    case QPDFTokenizer::tt_word:
        break;

    default:
        m_operands.append(token);
        return;
    }

    const QString op = QString::fromStdString(token.getValue());

    if (op == u"q"_s) {
        m_stack.append(m_ctm);
    } else if (op == u"Q"_s) {
        if (!m_stack.isEmpty()) {
            m_ctm = m_stack.takeLast();
        }
    } else if (op == u"cm"_s && m_operands.size() >= 6) {
        m_ctm = QTransform(operandNumber(0), operandNumber(1), operandNumber(2), operandNumber(3), operandNumber(4),
                           operandNumber(5))
            * m_ctm;
    }

    const bool isTarget = op == u"Do"_s && !m_operands.isEmpty()
        && m_operands.constLast().getType() == QPDFTokenizer::tt_name
        && QString::fromStdString(m_operands.constLast().getValue()) == m_target;
    if (!isTarget) {
        flush(op);
        m_operands.clear();
        return;
    }

    bool reversible = false;
    const QTransform back = m_ctm.inverted(&reversible);
    if (!reversible) {
        // A placement squashed to a line has no inverse, so no conjugation can
        // be worked out. Rasterising it instead would be a different operation
        // than the one that was asked for, done silently.
        ++m_refused;
        flush(op);
        m_operands.clear();
        return;
    }

    // Where it sits now, so the turn can be about its own middle rather than
    // about the corner of the page.
    const QPointF centre = m_ctm.map(QPointF(0.5, 0.5));

    // Clockwise as a reader sees it. Page space has y pointing up, and
    // QTransform::rotate turns the other way there, so the sign is flipped once
    // and only once: here.
    QTransform turn;
    turn.translate(centre.x(), centre.y());
    turn.rotate(-m_degrees);
    turn.translate(-centre.x(), -centre.y());

    const QTransform conjugated = m_ctm * turn * back;

    write("q ");
    write(cmOperator(conjugated));
    for (const QPDFTokenizer::Token &operand : std::as_const(m_operands)) {
        writeToken(operand);
        write(" ");
    }
    write("Do\nQ\n");

    ++m_turned;
    m_results.append((m_ctm * turn).mapRect(QRectF(0, 0, 1, 1)));
    m_operands.clear();
}

/**
 * The page's own picture resources, copied so an edit cannot leak to its neighbours.
 *
 * getAttribute() copies the resource dictionary when it is shared or inherited,
 * but the /XObject dictionary underneath it is still the very one every other
 * page sharing those resources is reading, so a name added here would appear on
 * all of them.
 */
QPDFObjectHandle ownXObjects(QPDFPageObjectHelper &page)
{
    QPDFObjectHandle resources = page.getAttribute("/Resources", true);
    if (!resources.isDictionary()) {
        return QPDFObjectHandle::newNull();
    }
    QPDFObjectHandle xobjects = resources.getKey("/XObject");
    if (!xobjects.isDictionary()) {
        return QPDFObjectHandle::newNull();
    }
    QPDFObjectHandle own = xobjects.shallowCopy();
    resources.replaceKey("/XObject", own);
    return own;
}

std::string freshName(QPDFObjectHandle xobjects, const char *stem)
{
    int suffix = 0;
    std::string candidate;
    do {
        candidate = std::string("/") + stem + std::to_string(suffix++);
    } while (xobjects.hasKey(candidate));
    return candidate;
}

QString normalisedName(const QString &name)
{
    return name.startsWith(u'/') ? name : u'/' + name;
}

/** Swaps in a rewritten content stream, and drops the names nothing draws now. */
void applyRewrite(QPDF &pdf, QPDFPageObjectHelper &page, QPDFObjectHandle::TokenFilter &rewriter)
{
    Pl_Buffer buffer("content");
    page.filterContents(&rewriter, &buffer);
    const auto data = buffer.getBufferSharedPointer();
    const std::string content(reinterpret_cast<const char *>(data->getBuffer()), data->getSize());
    page.getObjectHandle().replaceKey("/Contents", QPDFObjectHandle::newStream(&pdf, content));
    page.removeUnreferencedResources();
}

bool writeOut(QPDF &pdf, const QString &out)
{
    QPDFWriter writer(pdf, out.toUtf8().constData());
    writer.setObjectStreamMode(qpdf_o_generate);
    // Preserve rather than compress: an image nobody asked about must come out
    // of the far end with the bytes it went in with.
    writer.setStreamDataMode(qpdf_s_preserve);
    writer.write();
    return true;
}

/** The one picture an operation was asked about, or a sentence saying why not. */
struct Located {
    QPDFObjectHandle xobjects;
    QPDFObjectHandle image;
    Geometry geometry;
    int drawnTimes = 1;
};

bool locate(std::vector<QPDFPageObjectHelper> &pages, int page, const QString &name, bool needPixels, Located *located,
            QString *error)
{
    if (page < 0 || page >= int(pages.size())) {
        if (error) {
            *error = i18n("There is no page %1 in this document.", page + 1);
        }
        return false;
    }

    const QVector<Placement> placements = scanDocument(pages);
    QPDFPageObjectHelper subject = pages.at(size_t(page));
    located->xobjects = ownXObjects(subject);
    QPDFObjectHandle image
        = located->xobjects.isDictionary() ? located->xobjects.getKey(name.toStdString()) : QPDFObjectHandle::newNull();
    if (!image.isStream() || nameOf(image.getDict(), "/Subtype") != "/Image") {
        if (error) {
            *error = i18n("There is no picture called %1 on page %2.", name, page + 1);
        }
        return false;
    }
    located->image = image;

    const QRectF media = PdfGeometry::mediaBoxOf(subject);
    const int rotate = PdfGeometry::rotationOf(subject);
    double largest = -1.0;
    for (const Placement &placement : placements) {
        if (placement.object == image.getObjGen()) {
            ++located->drawnTimes;
        }
        if (placement.page != page || placement.name != name) {
            continue;
        }
        const Geometry geometry = measure(placement, media, rotate);
        if (geometry.pointsX * geometry.pointsY > largest) {
            largest = geometry.pointsX * geometry.pointsY;
            located->geometry = geometry;
        }
    }
    --located->drawnTimes;
    if (largest < 0.0) {
        if (error) {
            *error = i18n("Page %1 lists a picture called %2 but never draws it.", page + 1, name);
        }
        return false;
    }

    if (needPixels) {
        if (isImageMask(image.getDict())) {
            if (error) {
                *error = i18n("%1 on page %2 is a stencil, painted in whatever colour the page is using. "
                              "Its pixels carry no colour of their own, so they cannot be edited here.",
                              name, page + 1);
            }
            return false;
        }
        if (!looksDecodable(image)) {
            if (error) {
                *error = i18n("%1 on page %2 is stored as %3, which this tool cannot open. The picture is "
                              "left exactly as it is rather than being replaced by something wrong.",
                              name, page + 1, encodingOf(image.getDict()));
            }
            return false;
        }
    }
    return true;
}

} // namespace

// ── read ─────────────────────────────────────────────────────────────────

QVector<ImageEdit::ImageUse> ImageEdit::read(const QString &pdf, QString *error)
{
    QVector<ImageUse> uses;
    try {
        QPDF document;
        PdfFile::open(document, pdf);

        QPDFPageDocumentHelper documents(document);
        std::vector<QPDFPageObjectHelper> pages = documents.getAllPages();
        const QVector<Placement> placements = scanDocument(pages);

        std::map<QPDFObjGen, int> drawCounts;
        for (const Placement &placement : placements) {
            ++drawCounts[placement.object];
        }

        // One entry per name per page, describing the largest placement of it:
        // that is the one deciding how many pixels are really needed, and
        // reporting the small one would advise throwing away pixels the big one
        // still uses.
        QHash<QString, qsizetype> seen;
        QVector<double> areas;
        for (const Placement &placement : placements) {
            QPDFPageObjectHelper page = pages.at(size_t(placement.page));
            const Geometry geometry = measure(placement, PdfGeometry::mediaBoxOf(page), PdfGeometry::rotationOf(page));
            const double area = geometry.pointsX * geometry.pointsY;

            const QString key = QString::number(placement.page) + placement.name;
            const auto found = seen.constFind(key);
            if (found != seen.constEnd() && area <= areas.at(*found)) {
                continue;
            }

            QPDFObjectHandle resources = page.getAttribute("/Resources", false);
            QPDFObjectHandle xobjects
                = resources.isDictionary() ? resources.getKey("/XObject") : QPDFObjectHandle::newNull();
            QPDFObjectHandle image
                = xobjects.isDictionary() ? xobjects.getKey(placement.name.toStdString()) : QPDFObjectHandle::newNull();
            if (!image.isStream()) {
                continue;
            }
            QPDFObjectHandle dict = image.getDict();

            ImageUse use;
            use.page = placement.page;
            use.resourceName = placement.name;
            QPDFObjectHandle widthKey = dict.getKey("/Width");
            QPDFObjectHandle heightKey = dict.getKey("/Height");
            use.pixelSize = QSize(widthKey.isInteger() ? widthKey.getIntValueAsInt() : 0,
                                  heightKey.isInteger() ? heightKey.getIntValueAsInt() : 0);
            use.placement = geometry.display;
            if (geometry.pointsX > 0.0) {
                use.effectiveDpiX = use.pixelSize.width() * 72.0 / geometry.pointsX;
            }
            if (geometry.pointsY > 0.0) {
                use.effectiveDpiY = use.pixelSize.height() * 72.0 / geometry.pointsY;
            }
            use.encoding = encodingOf(dict);
            use.colourSpace = readColourSpace(dict.getKey("/ColorSpace")).label;
            QPDFObjectHandle bits = dict.getKey("/BitsPerComponent");
            use.bitsPerComponent = bits.isInteger() ? bits.getIntValueAsInt() : 8;
            use.hasAlpha = dict.getKey("/SMask").isStream() || !dict.getKey("/Mask").isNull();
            use.isMask = isImageMask(dict);
            use.storedBytes = storedBytesOf(image);
            use.decodable = looksDecodable(image);
            use.drawnTimes = drawCounts[placement.object];

            if (found != seen.constEnd()) {
                uses[*found] = use;
                areas[*found] = area;
            } else {
                seen.insert(key, uses.size());
                uses.append(use);
                areas.append(area);
            }
        }
    } catch (const std::exception &e) {
        if (error) {
            *error = QString::fromUtf8(e.what());
        }
        return {};
    }
    return uses;
}

// ── recompress ───────────────────────────────────────────────────────────

bool ImageEdit::recompress(const QString &in, const QString &out, const QVector<int> &pages,
                           const QStringList &resourceNames, const Recompress &options, qint64 *savedBytes,
                           QString *error)
{
    if (pages.size() != resourceNames.size()) {
        if (error) {
            *error = i18n("Each page number needs a picture name to go with it.");
        }
        return false;
    }

    qint64 saved = 0;
    try {
        QPDF document;
        PdfFile::open(document, in);

        QPDFPageDocumentHelper documents(document);
        std::vector<QPDFPageObjectHelper> pageHelpers = documents.getAllPages();
        const QVector<Placement> placements = scanDocument(pageHelpers);

        /** The biggest this object is ever drawn, which is what it must still serve. */
        struct Demand {
            double pointsX = 0.0;
            double pointsY = 0.0;
        };
        std::map<QPDFObjGen, Demand> demands;
        for (const Placement &placement : placements) {
            QPDFPageObjectHelper page = pageHelpers.at(size_t(placement.page));
            const Geometry geometry = measure(placement, PdfGeometry::mediaBoxOf(page), PdfGeometry::rotationOf(page));
            Demand &demand = demands[placement.object];
            demand.pointsX = std::max(demand.pointsX, geometry.pointsX);
            demand.pointsY = std::max(demand.pointsY, geometry.pointsY);
        }

        const bool everything = pages.isEmpty();
        std::map<QPDFObjGen, QPDFObjectHandle> chosen;
        std::map<QPDFObjGen, QString> namedBy;

        const auto pick = [&](int pageIndex, const QString &name) {
            if (pageIndex < 0 || pageIndex >= int(pageHelpers.size())) {
                return false;
            }
            QPDFPageObjectHelper page = pageHelpers.at(size_t(pageIndex));
            QPDFObjectHandle resources = page.getAttribute("/Resources", false);
            QPDFObjectHandle xobjects
                = resources.isDictionary() ? resources.getKey("/XObject") : QPDFObjectHandle::newNull();
            QPDFObjectHandle image
                = xobjects.isDictionary() ? xobjects.getKey(name.toStdString()) : QPDFObjectHandle::newNull();
            if (!image.isStream() || nameOf(image.getDict(), "/Subtype") != "/Image") {
                return false;
            }
            chosen.insert({ image.getObjGen(), image });
            namedBy.insert({ image.getObjGen(), name });
            return true;
        };

        if (everything) {
            for (const Placement &placement : placements) {
                pick(placement.page, placement.name);
            }
        } else {
            for (qsizetype i = 0; i < pages.size(); ++i) {
                const QString name = normalisedName(resourceNames.at(i));
                if (!pick(pages.at(i), name)) {
                    if (error) {
                        *error = i18n("There is no picture called %1 on page %2.", name, pages.at(i) + 1);
                    }
                    return false;
                }
            }
        }

        for (auto &[objgen, image] : chosen) {
            QPDFObjectHandle dict = image.getDict();
            QPDFObjectHandle widthKey = dict.getKey("/Width");
            QPDFObjectHandle heightKey = dict.getKey("/Height");
            const int width = widthKey.isInteger() ? widthKey.getIntValueAsInt() : 0;
            const int height = heightKey.isInteger() ? heightKey.getIntValueAsInt() : 0;
            if (width <= 0 || height <= 0) {
                continue;
            }

            const Demand demand = demands[objgen];
            int wantWidth = width;
            int wantHeight = height;
            if (options.targetDpi > 0.0 && demand.pointsX > 0.0 && demand.pointsY > 0.0) {
                wantWidth = std::min(width, std::max(1, int(std::ceil(demand.pointsX / 72.0 * options.targetDpi))));
                wantHeight = std::min(height, std::max(1, int(std::ceil(demand.pointsY / 72.0 * options.targetDpi))));
            }

            const Colour colour = readColourSpace(dict.getKey("/ColorSpace"));
            const bool alreadyGrey = colour.components == 1 && !colour.indexed;
            // A one-in-twenty shave is not worth a generation of JPEG loss, so
            // the resize only counts as needed when it genuinely saves pixels.
            const double areaRatio = double(wantWidth) * wantHeight / (double(width) * height);
            const bool needsResize = areaRatio < 0.9;
            const bool needsGrey = options.toGrayscale && !alreadyGrey;
            // Without forceReencode this is the promise that asking for a
            // resolution a file already has costs it nothing. With it, the
            // quality is applied whatever the resolution says, which is the
            // only way to answer "smaller, but keep the resolution", and the
            // wrong answer to anything else, so it is never the default.
            if (!needsResize && !needsGrey && !options.forceReencode) {
                continue;
            }

            if (isImageMask(dict) || !looksDecodable(image)) {
                if (everything) {
                    continue;
                }
                if (error) {
                    *error = i18n("%1 is stored as %2, which this tool cannot open, so it was left alone.",
                                  namedBy[objgen], encodingOf(dict));
                }
                return false;
            }

            const Decoded decoded = decodeImage(image);
            if (decoded.image.isNull()) {
                if (everything) {
                    continue;
                }
                if (error) {
                    *error = i18n("The pixels of %1 could not be read.", namedBy[objgen]);
                }
                return false;
            }

            const bool grey = decoded.wasGrey || options.toGrayscale;
            QImage pixels = decoded.image;
            if (needsResize) {
                // Through 32-bit and back: Qt's smooth scaler works there, and a
                // nearest-neighbour downsample of a scan looks like a fax.
                pixels = pixels.convertToFormat(QImage::Format_RGB32)
                             .scaled(wantWidth, wantHeight, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
            }
            if (grey) {
                pixels = pixels.convertToFormat(QImage::Format_Grayscale8);
            }

            Encoded encoded = options.lossless ? encodeFlate(pixels, grey)
                                               : encodeJpeg(pixels, std::clamp(options.jpegQuality, 1, 100));
            if (!encoded.isValid()) {
                if (everything) {
                    continue;
                }
                if (error) {
                    *error = i18n("%1 could not be re-encoded.", namedBy[objgen]);
                }
                return false;
            }

            const qint64 before = storedBytesOf(image);
            const qint64 after = qint64(encoded.data.size());
            if (!needsGrey && after >= before && before > 0) {
                // Re-encoding made it bigger. Keeping the original is the only
                // outcome that serves the request.
                continue;
            }

            bool hadMask = false;
            const QImage mask = decodeSoftMask(image, &hadMask);
            fillImageStream(document, image, encoded);
            if (hadMask && !mask.isNull() && needsResize) {
                // The mask is scaled to the image by the reader anyway, but a
                // full-size mask beside a downsampled picture keeps exactly the
                // bytes this operation was asked to save.
                const qint64 maskBefore = storedBytesOf(dict.getKey("/SMask"));
                QPDFObjectHandle resized = makeSoftMask(
                    document,
                    mask.scaled(encoded.width, encoded.height, Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
                if (resized.isStream()) {
                    dict.replaceKey("/SMask", resized);
                    saved += maskBefore - storedBytesOf(resized);
                }
            }
            saved += before - after;
        }

        writeOut(document, out);
    } catch (const std::exception &e) {
        if (error) {
            *error = QString::fromUtf8(e.what());
        }
        return false;
    }

    if (savedBytes) {
        *savedBytes = saved;
    }
    return true;
}

// ── adjust ───────────────────────────────────────────────────────────────

bool ImageEdit::adjust(const QString &in, const QString &out, int page, const QString &resourceName,
                       const Adjust &options, QString *error)
{
    const QString name = normalisedName(resourceName);
    try {
        QPDF document;
        PdfFile::open(document, in);

        QPDFPageDocumentHelper documents(document);
        std::vector<QPDFPageObjectHelper> pages = documents.getAllPages();

        Located located;
        if (!locate(pages, page, name, true, &located, error)) {
            return false;
        }

        const Decoded decoded = decodeImage(located.image);
        if (decoded.image.isNull()) {
            if (error) {
                *error = i18n("The pixels of %1 could not be read.", name);
            }
            return false;
        }

        QImage pixels = decoded.image;
        applyAdjust(pixels, options);

        const bool grey = options.toGrayscale || options.toBlackWhite || decoded.wasGrey;
        Encoded encoded;
        if (options.toBlackWhite) {
            // One bit per pixel, which for a thresholded scan is both what was
            // asked for and a fraction of the size of a grey JPEG of the same.
            encoded = encodeMono(pixels, std::clamp(options.threshold, 0, 255));
        } else if (decoded.wasJpeg) {
            encoded = encodeJpeg(grey ? pixels.convertToFormat(QImage::Format_Grayscale8) : pixels, 92);
        } else {
            encoded = encodeFlate(pixels, grey);
        }
        if (!encoded.isValid()) {
            if (error) {
                *error = i18n("The adjusted pixels of %1 could not be stored.", name);
            }
            return false;
        }

        QPDFObjectHandle replacement = makeImageStream(document, encoded);
        copyPresentation(located.image, replacement);

        // A new object under a new name, never an overwrite: the same picture
        // may well be drawn on another page that was not asked about, and that
        // page has to keep the pixels it had.
        const std::string fresh = freshName(located.xobjects, "PsImage");
        located.xobjects.replaceKey(fresh, replacement);

        QPDFPageObjectHelper subject = pages.at(size_t(page));
        DrawRewriter rewriter(name, QString::fromStdString(fresh), false, {});
        applyRewrite(document, subject, rewriter);
        if (rewriter.rewritten() == 0) {
            if (error) {
                *error = i18n("Page %1 never draws %2, so there was nothing to change.", page + 1, name);
            }
            return false;
        }

        writeOut(document, out);
    } catch (const std::exception &e) {
        if (error) {
            *error = QString::fromUtf8(e.what());
        }
        return false;
    }
    return true;
}

// ── crop ─────────────────────────────────────────────────────────────────

bool ImageEdit::crop(const QString &in, const QString &out, int page, const QString &resourceName,
                     const QRectF &keepFractional, QString *error)
{
    const QString name = normalisedName(resourceName);
    const QRectF keep = keepFractional.normalized().intersected(QRectF(0, 0, 1, 1));
    if (keep.width() <= 0.0 || keep.height() <= 0.0) {
        if (error) {
            *error = i18n("The part to keep must lie inside the picture and cannot be empty.");
        }
        return false;
    }

    try {
        QPDF document;
        PdfFile::open(document, in);

        QPDFPageDocumentHelper documents(document);
        std::vector<QPDFPageObjectHelper> pages = documents.getAllPages();

        Located located;
        if (!locate(pages, page, name, true, &located, error)) {
            return false;
        }

        const Decoded decoded = decodeImage(located.image);
        if (decoded.image.isNull()) {
            if (error) {
                *error = i18n("The pixels of %1 could not be read.", name);
            }
            return false;
        }

        bool hadMask = false;
        const QImage mask = decodeSoftMask(located.image, &hadMask);
        // A transparency layer that cannot be cropped alongside the pixels would
        // leave the see-through parts over the wrong bits of the picture, which
        // looks like corruption rather than like a crop.
        if ((hadMask && mask.isNull()) || located.image.getDict().getKey("/Mask").isStream()) {
            if (error) {
                *error = i18n("The transparency of %1 is stored in a form this tool cannot open, so "
                              "cropping its pixels would leave the see-through parts in the wrong place.",
                              name);
            }
            return false;
        }

        const QRect pixelRect(int(std::lround(keep.x() * decoded.image.width())),
                              int(std::lround(keep.y() * decoded.image.height())),
                              std::max(1, int(std::lround(keep.width() * decoded.image.width()))),
                              std::max(1, int(std::lround(keep.height() * decoded.image.height()))));
        const QImage cropped = decoded.image.copy(pixelRect.intersected(decoded.image.rect()));
        if (cropped.isNull()) {
            if (error) {
                *error = i18n("Cropping %1 would leave nothing behind.", name);
            }
            return false;
        }

        Encoded encoded = decoded.wasJpeg ? encodeJpeg(cropped, 92) : encodeFlate(cropped, decoded.wasGrey);
        if (!encoded.isValid()) {
            if (error) {
                *error = i18n("The cropped pixels of %1 could not be stored.", name);
            }
            return false;
        }

        QPDFObjectHandle replacement = makeImageStream(document, encoded);
        copyPresentation(located.image, replacement);
        if (hadMask) {
            const QRect maskRect(int(std::lround(keep.x() * mask.width())), int(std::lround(keep.y() * mask.height())),
                                 std::max(1, int(std::lround(keep.width() * mask.width()))),
                                 std::max(1, int(std::lround(keep.height() * mask.height()))));
            QPDFObjectHandle croppedMask = makeSoftMask(document, mask.copy(maskRect.intersected(mask.rect())));
            if (!croppedMask.isStream()) {
                if (error) {
                    *error = i18n("The cropped transparency of %1 could not be stored.", name);
                }
                return false;
            }
            replacement.getDict().replaceKey("/SMask", croppedMask);
        }

        const std::string fresh = freshName(located.xobjects, "PsCropped");
        located.xobjects.replaceKey(fresh, replacement);

        // The kept part must not move, so the placement is narrowed to the same
        // fraction of the unit square the pixels came from. Image space has its
        // origin at the bottom left, hence the flipped y.
        const QTransform prefix(keep.width(), 0, 0, keep.height(), keep.x(), 1.0 - keep.y() - keep.height());

        QPDFPageObjectHelper subject = pages.at(size_t(page));
        DrawRewriter rewriter(name, QString::fromStdString(fresh), true, prefix);
        applyRewrite(document, subject, rewriter);
        if (rewriter.rewritten() == 0) {
            if (error) {
                *error = i18n("Page %1 never draws %2, so there was nothing to crop.", page + 1, name);
            }
            return false;
        }

        writeOut(document, out);
    } catch (const std::exception &e) {
        if (error) {
            *error = QString::fromUtf8(e.what());
        }
        return false;
    }
    return true;
}

// ── replace ──────────────────────────────────────────────────────────────

bool ImageEdit::replace(const QString &in, const QString &out, int page, const QString &resourceName,
                        const QImage &replacement, bool keepAspect, QString *error)
{
    const QString name = normalisedName(resourceName);
    if (replacement.isNull()) {
        if (error) {
            *error = i18n("There is no replacement picture to put in.");
        }
        return false;
    }

    try {
        QPDF document;
        PdfFile::open(document, in);

        QPDFPageDocumentHelper documents(document);
        std::vector<QPDFPageObjectHelper> pages = documents.getAllPages();

        Located located;
        if (!locate(pages, page, name, false, &located, error)) {
            return false;
        }

        const bool transparent = replacement.hasAlphaChannel();
        Encoded encoded = transparent ? encodeFlate(replacement, false) : encodeJpeg(replacement, 90);
        if (!encoded.isValid()) {
            encoded = encodeFlate(replacement, false);
        }
        if (!encoded.isValid()) {
            if (error) {
                *error = i18n("The replacement picture could not be stored in the document.");
            }
            return false;
        }
        if (transparent) {
            encoded.softMask = encodeAlpha(replacement);
        }

        QPDFObjectHandle fresh = makeImageStream(document, encoded);
        const std::string freshKey = freshName(located.xobjects, "PsReplaced");
        located.xobjects.replaceKey(freshKey, fresh);

        // Fitting means shrinking one axis inside the old area and centring it,
        // never growing past it: a swapped photograph that overlaps the caption
        // beside it is not a swap anyone wanted.
        QTransform prefix;
        bool wrap = false;
        if (keepAspect && located.geometry.pointsX > 0.0 && located.geometry.pointsY > 0.0) {
            const double placed = located.geometry.pointsX / located.geometry.pointsY;
            const double wanted = double(replacement.width()) / replacement.height();
            double scaleX = 1.0;
            double scaleY = 1.0;
            if (wanted > placed) {
                scaleY = placed / wanted;
            } else if (wanted < placed) {
                scaleX = wanted / placed;
            }
            if (!qFuzzyCompare(scaleX, 1.0) || !qFuzzyCompare(scaleY, 1.0)) {
                prefix = QTransform(scaleX, 0, 0, scaleY, (1.0 - scaleX) / 2.0, (1.0 - scaleY) / 2.0);
                wrap = true;
            }
        }

        QPDFPageObjectHelper subject = pages.at(size_t(page));
        DrawRewriter rewriter(name, QString::fromStdString(freshKey), wrap, prefix);
        applyRewrite(document, subject, rewriter);
        if (rewriter.rewritten() == 0) {
            if (error) {
                *error = i18n("Page %1 never draws %2, so there was nothing to replace.", page + 1, name);
            }
            return false;
        }

        writeOut(document, out);
    } catch (const std::exception &e) {
        if (error) {
            *error = QString::fromUtf8(e.what());
        }
        return false;
    }
    return true;
}

// ── extract ──────────────────────────────────────────────────────────────

bool ImageEdit::extract(const QString &pdf, int page, const QString &resourceName, const QString &toFile,
                        QString *error)
{
    const QString name = normalisedName(resourceName);
    try {
        QPDF document;
        PdfFile::open(document, pdf);

        QPDFPageDocumentHelper documents(document);
        std::vector<QPDFPageObjectHelper> pages = documents.getAllPages();
        if (page < 0 || page >= int(pages.size())) {
            if (error) {
                *error = i18n("There is no page %1 in this document.", page + 1);
            }
            return false;
        }

        QPDFPageObjectHelper subject = pages.at(size_t(page));
        QPDFObjectHandle resources = subject.getAttribute("/Resources", false);
        QPDFObjectHandle xobjects
            = resources.isDictionary() ? resources.getKey("/XObject") : QPDFObjectHandle::newNull();
        QPDFObjectHandle image
            = xobjects.isDictionary() ? xobjects.getKey(name.toStdString()) : QPDFObjectHandle::newNull();
        if (!image.isStream() || nameOf(image.getDict(), "/Subtype") != "/Image") {
            if (error) {
                *error = i18n("There is no picture called %1 on page %2.", name, page + 1);
            }
            return false;
        }

        const QString suffix = QFileInfo(toFile).suffix().toLower();
        const bool wantsJpeg = suffix == u"jpg"_s || suffix == u"jpeg"_s;
        if (wantsJpeg && filtersOf(image.getDict()).contains(u"/DCTDecode"_s)) {
            // The one extraction that loses nothing: the stored JPEG is already
            // a JPEG, so decoding and re-encoding it would only cost quality.
            const QByteArray jpeg = unfilteredBytes(image, qpdf_dl_specialized);
            if (!jpeg.isEmpty()) {
                QFile file(toFile);
                if (!file.open(QIODevice::WriteOnly)) {
                    if (error) {
                        *error = i18n("%1 could not be opened for writing.", toFile);
                    }
                    return false;
                }
                const bool written = file.write(jpeg) == jpeg.size();
                file.close();
                if (!written && error) {
                    *error = i18n("%1 could not be written.", toFile);
                }
                return written;
            }
        }

        if (isImageMask(image.getDict()) || !looksDecodable(image)) {
            if (error) {
                *error = i18n("%1 on page %2 is stored as %3, which this tool cannot open.", name, page + 1,
                              encodingOf(image.getDict()));
            }
            return false;
        }

        const Decoded decoded = decodeImage(image);
        if (decoded.image.isNull()) {
            if (error) {
                *error = i18n("The pixels of %1 could not be read.", name);
            }
            return false;
        }

        QImage result = decoded.image;
        bool hadMask = false;
        const QImage mask = decodeSoftMask(image, &hadMask);
        if (hadMask && !mask.isNull() && suffix != u"jpg"_s && suffix != u"jpeg"_s) {
            // Folding the soft mask back into alpha, because a logo extracted
            // on an opaque white square is not the logo that was in the file.
            const QImage scaled = mask.size() == result.size()
                ? mask
                : mask.scaled(result.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
            QImage merged = result.convertToFormat(QImage::Format_ARGB32);
            for (int y = 0; y < merged.height(); ++y) {
                auto *line = reinterpret_cast<QRgb *>(merged.scanLine(y));
                const uchar *alpha = scaled.constScanLine(y);
                for (int x = 0; x < merged.width(); ++x) {
                    line[x] = qRgba(qRed(line[x]), qGreen(line[x]), qBlue(line[x]), alpha[x]);
                }
            }
            result = merged;
        }

        if (!result.save(toFile)) {
            if (error) {
                *error = i18n("%1 could not be written.", toFile);
            }
            return false;
        }
    } catch (const std::exception &e) {
        if (error) {
            *error = QString::fromUtf8(e.what());
        }
        return false;
    }
    return true;
}

// ── remove ───────────────────────────────────────────────────────────────

bool ImageEdit::remove(const QString &in, const QString &out, int page, const QString &resourceName, QString *error)
{
    const QString name = normalisedName(resourceName);
    try {
        QPDF document;
        PdfFile::open(document, in);

        QPDFPageDocumentHelper documents(document);
        std::vector<QPDFPageObjectHelper> pages = documents.getAllPages();
        if (page < 0 || page >= int(pages.size())) {
            if (error) {
                *error = i18n("There is no page %1 in this document.", page + 1);
            }
            return false;
        }

        QPDFPageObjectHelper subject = pages.at(size_t(page));
        QPDFObjectHandle xobjects = ownXObjects(subject);
        if (!xobjects.isDictionary() || !xobjects.hasKey(name.toStdString())) {
            if (error) {
                *error = i18n("There is no picture called %1 on page %2.", name, page + 1);
            }
            return false;
        }

        // The name goes with the drawing; the bytes only go when no other page
        // still points at them, which the writer works out for itself.
        xobjects.removeKey(name.toStdString());

        DrawRewriter rewriter(name, QString(), false, {});
        applyRewrite(document, subject, rewriter);
        if (rewriter.rewritten() == 0) {
            if (error) {
                *error = i18n("Page %1 never draws %2, so there was nothing to remove.", page + 1, name);
            }
            return false;
        }

        writeOut(document, out);
    } catch (const std::exception &e) {
        if (error) {
            *error = QString::fromUtf8(e.what());
        }
        return false;
    }
    return true;
}

bool ImageEdit::rotate(const QString &in, const QString &out, int page, const QString &resourceName, double degrees,
                       RotateReport *report, QString *error)
{
    const QString name = normalisedName(resourceName);

    // A whole number of turns is not an error, it is just nothing to do; saying
    // so beats writing a copy of the file and reporting success.
    const double normalised = std::fmod(std::fmod(degrees, 360.0) + 360.0, 360.0);
    if (qFuzzyIsNull(normalised)) {
        if (error) {
            *error = i18n("A turn of %1 degrees leaves the picture where it was.", degrees);
        }
        return false;
    }

    try {
        QPDF document;
        PdfFile::open(document, in);

        QPDFPageDocumentHelper documents(document);
        std::vector<QPDFPageObjectHelper> pages = documents.getAllPages();
        if (page < 0 || page >= int(pages.size())) {
            if (error) {
                *error = i18n("There is no page %1 in this document.", page + 1);
            }
            return false;
        }

        QPDFPageObjectHelper subject = pages.at(size_t(page));
        QPDFObjectHandle xobjects = ownXObjects(subject);
        if (!xobjects.isDictionary() || !xobjects.hasKey(name.toStdString())) {
            if (error) {
                *error = i18n("There is no picture called %1 on page %2.", name, page + 1);
            }
            return false;
        }

        TurnRewriter rewriter(name, normalised);
        applyRewrite(document, subject, rewriter);
        if (rewriter.turned() == 0 && rewriter.refused() == 0) {
            if (error) {
                *error = i18n("Page %1 never draws %2, so there was nothing to turn.", page + 1, name);
            }
            return false;
        }

        if (report) {
            report->placementsTurned = rewriter.turned();
            report->placementsRefused = rewriter.refused();
            // Nothing above decodes anything, so the stored stream is untouched
            // by construction. It stops being the whole truth only when some
            // placement had to be left where it was.
            report->pixelsUntouched = rewriter.refused() == 0;
            report->quarterTurn = qFuzzyCompare(normalised, 90.0) || qFuzzyCompare(normalised, 180.0)
                || qFuzzyCompare(normalised, 270.0);

            const QVector<QRectF> where = rewriter.results();
            if (!where.isEmpty()) {
                report->placement = where.constFirst();
            }

            if (rewriter.refused() > 0) {
                report->notes += i18np("One placement has a flattened matrix that cannot be reversed, so it was "
                                       "left where it was.",
                                       "%1 placements have a flattened matrix that cannot be reversed, so they "
                                       "were left where they were.",
                                       rewriter.refused());
            }
            if (!report->quarterTurn) {
                report->notes += i18n("A turn that is not a quarter, a half or three quarters leaves the picture "
                                      "at an angle to the page, covering a different area than before.");
            }

            const QRectF media = PdfGeometry::mediaBoxOf(subject);
            for (const QRectF &box : where) {
                if (!media.contains(box)) {
                    report->notes += i18n("Turned, the picture reaches past the edge of the page.");
                    break;
                }
            }
        }

        writeOut(document, out);
    } catch (const std::exception &e) {
        if (error) {
            *error = QString::fromUtf8(e.what());
        }
        return false;
    }
    return true;
}

// ── deduplicate ──────────────────────────────────────────────────────────

bool ImageEdit::deduplicate(const QString &in, const QString &out, int *merged, qint64 *saved, QString *error)
{
    int mergedCount = 0;
    qint64 savedBytes = 0;
    try {
        QPDF document;
        PdfFile::open(document, in);

        QPDFPageDocumentHelper documents(document);
        std::vector<QPDFPageObjectHelper> pages = documents.getAllPages();

        /** Where a picture is referred to from, so a reference can be repointed. */
        struct Reference {
            QPDFObjectHandle holder;
            std::string name;
        };

        std::map<QPDFObjGen, QVector<Reference>> references;
        std::map<QPDFObjGen, QPDFObjectHandle> objects;
        for (QPDFPageObjectHelper &page : pages) {
            QPDFObjectHandle resources = page.getAttribute("/Resources", false);
            QPDFObjectHandle xobjects
                = resources.isDictionary() ? resources.getKey("/XObject") : QPDFObjectHandle::newNull();
            if (!xobjects.isDictionary()) {
                continue;
            }
            for (const auto &[name, object] : xobjects.getDictAsMap()) {
                if (!object.isStream() || nameOf(object.getDict(), "/Subtype") != "/Image") {
                    continue;
                }
                references[object.getObjGen()].append({ xobjects, name });
                objects.insert({ object.getObjGen(), object });
            }
        }

        // The comparison is over the *stored* bytes plus the dictionary entries
        // that say how to read them, not over decoded pixels. Two streams that
        // decode to the same picture but were compressed differently are not
        // interchangeable (merging them would silently change one of them),
        // and decoding every image in a two-hundred-page magazine to find that
        // out would cost far more than the merge saves. The case that actually
        // occurs is the same logo embedded once per page by a layout program,
        // and that produces byte-identical streams.
        QHash<QByteArray, QVector<QPDFObjGen>> byFingerprint;
        for (auto &[objgen, object] : objects) {
            QCryptographicHash hash(QCryptographicHash::Sha256);
            for (const char *key : { "/Width", "/Height", "/BitsPerComponent", "/ColorSpace", "/Filter", "/DecodeParms",
                                     "/Decode", "/ImageMask", "/SMask", "/Mask" }) {
                QPDFObjectHandle value = object.getDict().getKey(key);
                hash.addData(QByteArray::fromStdString(value.unparse()));
                hash.addData(QByteArrayLiteral("\x1f"));
            }
            const auto raw = object.getRawStreamData();
            hash.addData(QByteArrayView(reinterpret_cast<const char *>(raw->getBuffer()), qsizetype(raw->getSize())));
            byFingerprint[hash.result()].append(objgen);
        }

        for (auto it = byFingerprint.cbegin(); it != byFingerprint.cend(); ++it) {
            const QVector<QPDFObjGen> &group = it.value();
            if (group.size() < 2) {
                continue;
            }
            // The lowest object number wins, so the same input always produces
            // the same output.
            QPDFObjGen keeper = group.constFirst();
            for (const QPDFObjGen &candidate : group) {
                if (candidate < keeper) {
                    keeper = candidate;
                }
            }
            for (const QPDFObjGen &duplicate : group) {
                if (duplicate == keeper) {
                    continue;
                }
                for (Reference &reference : references[duplicate]) {
                    reference.holder.replaceKey(reference.name, objects[keeper]);
                }
                savedBytes += storedBytesOf(objects[duplicate]);
                ++mergedCount;
            }
        }

        writeOut(document, out);
    } catch (const std::exception &e) {
        if (error) {
            *error = QString::fromUtf8(e.what());
        }
        return false;
    }

    if (merged) {
        *merged = mergedCount;
    }
    if (saved) {
        *saved = savedBytes;
    }
    return true;
}

QStringList ImageEdit::limitations()
{
    return {
        i18n("Effective resolution comes from the matrix that placed a picture, so a picture drawn "
             "several times on one page is reported at its largest placement."),
        i18n("Fax (CCITT, JBIG2) and JPEG 2000 pictures cannot be opened. They are listed and can be "
             "removed or deduplicated, but not recompressed, adjusted or cropped."),
        i18n("Stencil masks, which take their colour from the page rather than carrying their own, are "
             "left alone by everything that edits pixels."),
        i18n("Pictures drawn inside an embedded drawing rather than straight onto a page are not listed, "
             "and neither are inline images."),
        i18n("Recompressing a picture that several pages share changes it for all of them, sized for the "
             "largest place it appears."),
        i18n("An operation naming one picture applies to every place that page draws it."),
        i18n("Adjusting pixels re-encodes them at high quality, which can leave a sharpened scan larger "
             "than it was. Recompress afterwards if size matters more than the last of the detail."),
        i18n("Colour is converted arithmetically: an ICC profile or a spot colour is read as its nearest "
             "device equivalent rather than through a colour engine."),
        i18n("Deduplication compares the stored bytes, so two copies of the same photograph saved at "
             "different quality settings are correctly left as two pictures."),
    };
}

} // namespace ps
