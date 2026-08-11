/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#include "Redaction.h"
#include "PdfFile.h"

#include "Core14Widths.h"
#include "PdfGeometry.h"
#include "PdfImage.h"

#include <KLocalizedString>
#include <QFileInfo>
#include <QHash>
#include <QPainter>
#include <QTransform>

#include <qpdf/Pl_Buffer.hh>
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>
#include <qpdf/QPDFTokenizer.hh>
#include <qpdf/QPDFWriter.hh>

#include <cmath>
#include <set>

using namespace Qt::Literals::StringLiterals;

namespace ps {

namespace {

/** What a font tells us about how far each character advances. */
struct FontInfo {
    /**
     * Whether character codes are two bytes wide.
     *
     * Composite fonts almost always use Identity-H, and assuming so is far
     * better than treating a two-byte stream as single bytes, which would
     * produce nonsense widths.
     */
    bool twoByte = false;

    /** Advance for codes missing from the table, in text space units. */
    double defaultWidth = 0.6;

    QHash<int, double> widths;

    double widthFor(int code) const
    {
        const auto found = widths.constFind(code);
        return found != widths.constEnd() ? *found : defaultWidth;
    }
};

std::string nameOf(QPDFObjectHandle object, const char *key)
{
    if (!object.isDictionary()) {
        return {};
    }
    QPDFObjectHandle value = object.getKey(key);
    return value.isName() ? value.getName() : std::string();
}

void parseCidWidths(QPDFObjectHandle w, QHash<int, double> &into)
{
    if (!w.isArray()) {
        return;
    }

    // /W is [ code [w w w] code1 code2 w ... ], mixing both forms freely.
    const int count = w.getArrayNItems();
    for (int i = 0; i < count;) {
        QPDFObjectHandle first = w.getArrayItem(i);
        if (!first.isInteger() || i + 1 >= count) {
            break;
        }
        const int start = first.getIntValueAsInt();
        QPDFObjectHandle second = w.getArrayItem(i + 1);

        if (second.isArray()) {
            for (int j = 0; j < second.getArrayNItems(); ++j) {
                QPDFObjectHandle width = second.getArrayItem(j);
                if (width.isNumber()) {
                    into.insert(start + j, PdfGeometry::numericValue(width, 0.0) / 1000.0);
                }
            }
            i += 2;
            continue;
        }

        if (i + 2 >= count) {
            break;
        }
        const int end = second.isInteger() ? second.getIntValueAsInt() : start;
        QPDFObjectHandle width = w.getArrayItem(i + 2);
        // A pathological range would otherwise build a table of millions of
        // identical entries; the default width covers the rest just as well.
        const int limit = std::min(end, start + 65535);
        if (width.isNumber()) {
            for (int code = start; code <= limit; ++code) {
                into.insert(code, PdfGeometry::numericValue(width, 0.0) / 1000.0);
            }
        }
        i += 3;
    }
}

/**
 * Fills @p info from the built-in metrics of whichever standard font @p baseFont
 * names, or the nearest one.
 *
 * Documents are allowed to use the fourteen standard fonts without saying how
 * wide their characters are, and plenty do, including anything generated from
 * a bare "Helvetica". Falling back to an average here would put every glyph
 * after the first in the wrong place, and wrong places redact the wrong words.
 */
void applyCore14(const std::string &baseFont, FontInfo &info)
{
    QString name = QString::fromStdString(baseFont);
    // Subset fonts arrive as "ABCDEF+Helvetica".
    const qsizetype plus = name.indexOf(u'+');
    if (plus >= 0) {
        name = name.mid(plus + 1);
    }
    const QString normalised = name.toLower().remove(u'-').remove(u' ').remove(u',').remove(u'/');

    const bool bold
        = normalised.contains(u"bold"_s) || normalised.contains(u"black"_s) || normalised.contains(u"heavy"_s);
    const bool slanted = normalised.contains(u"italic"_s) || normalised.contains(u"oblique"_s);

    QString wanted;
    if (normalised.contains(u"courier"_s) || normalised.contains(u"mono"_s)) {
        wanted = u"Courier"_s;
        if (bold && slanted) {
            wanted = u"Courier-BoldOblique"_s;
        } else if (bold) {
            wanted = u"Courier-Bold"_s;
        } else if (slanted) {
            wanted = u"Courier-Oblique"_s;
        }
    } else if (normalised.contains(u"zapf"_s) || normalised.contains(u"dingbat"_s)) {
        wanted = u"ZapfDingbats"_s;
    } else if (normalised.contains(u"symbol"_s)) {
        wanted = u"Symbol"_s;
    } else if (normalised.contains(u"times"_s) || normalised.contains(u"georgia"_s)
               || normalised.contains(u"serif"_s)) {
        wanted = u"Times-Roman"_s;
        if (bold && slanted) {
            wanted = u"Times-BoldItalic"_s;
        } else if (bold) {
            wanted = u"Times-Bold"_s;
        } else if (slanted) {
            wanted = u"Times-Italic"_s;
        }
    } else {
        // Helvetica stands in for Arial and for anything unrecognised, which is
        // the usual substitution a reader makes anyway.
        wanted = u"Helvetica"_s;
        if (bold && slanted) {
            wanted = u"Helvetica-BoldOblique"_s;
        } else if (bold) {
            wanted = u"Helvetica-Bold"_s;
        } else if (slanted) {
            wanted = u"Helvetica-Oblique"_s;
        }
    }

    for (const Core14::Entry &entry : Core14::table) {
        if (wanted != QLatin1StringView(entry.name)) {
            continue;
        }
        for (int code = Core14::firstCode; code <= Core14::lastCode; ++code) {
            info.widths.insert(code, entry.widths[code - Core14::firstCode] / 1000.0);
        }
        info.defaultWidth = entry.averageWidth / 1000.0;
        return;
    }
}

FontInfo loadFont(QPDFObjectHandle font)
{
    FontInfo info;
    if (!font.isDictionary()) {
        return info;
    }

    if (nameOf(font, "/Subtype") == "/Type0") {
        info.twoByte = true;
        info.defaultWidth = 1.0;

        QPDFObjectHandle descendants = font.getKey("/DescendantFonts");
        if (descendants.isArray() && descendants.getArrayNItems() > 0) {
            QPDFObjectHandle cid = descendants.getArrayItem(0);
            if (cid.isDictionary()) {
                QPDFObjectHandle defaultWidth = cid.getKey("/DW");
                if (defaultWidth.isNumber()) {
                    info.defaultWidth = PdfGeometry::numericValue(defaultWidth, 1000.0) / 1000.0;
                }
                parseCidWidths(cid.getKey("/W"), info.widths);
            }
        }
        return info;
    }

    QPDFObjectHandle widths = font.getKey("/Widths");
    QPDFObjectHandle firstChar = font.getKey("/FirstChar");
    if (widths.isArray() && firstChar.isInteger()) {
        const int first = firstChar.getIntValueAsInt();
        for (int i = 0; i < widths.getArrayNItems(); ++i) {
            QPDFObjectHandle width = widths.getArrayItem(i);
            if (width.isNumber()) {
                info.widths.insert(first + i, PdfGeometry::numericValue(width, 0.0) / 1000.0);
            }
        }
    }

    if (info.widths.isEmpty()) {
        applyCore14(nameOf(font, "/BaseFont"), info);
    }

    QPDFObjectHandle descriptor = font.getKey("/FontDescriptor");
    if (descriptor.isDictionary() && descriptor.getKey("/MissingWidth").isNumber()) {
        info.defaultWidth = PdfGeometry::numericValue(descriptor.getKey("/MissingWidth"), 600.0) / 1000.0;
    }
    return info;
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

/** How many colour components a colour space has, or 0 when it is not one we can read. */
int componentsOf(QPDFObjectHandle colourSpace)
{
    if (colourSpace.isName()) {
        const std::string name = colourSpace.getName();
        if (name == "/DeviceGray" || name == "/G" || name == "/CalGray") {
            return 1;
        }
        if (name == "/DeviceRGB" || name == "/RGB" || name == "/CalRGB") {
            return 3;
        }
        return 0;
    }

    if (colourSpace.isArray() && colourSpace.getArrayNItems() >= 2) {
        QPDFObjectHandle family = colourSpace.getArrayItem(0);
        const std::string name = family.isName() ? family.getName() : std::string();
        if (name == "/ICCBased") {
            QPDFObjectHandle stream = colourSpace.getArrayItem(1);
            QPDFObjectHandle n = stream.isStream() ? stream.getDict().getKey("/N") : QPDFObjectHandle::newNull();
            const int components = n.isInteger() ? n.getIntValueAsInt() : 0;
            return (components == 1 || components == 3) ? components : 0;
        }
        if (name == "/CalGray") {
            return 1;
        }
        if (name == "/CalRGB") {
            return 3;
        }
    }
    return 0;
}

/** True when decodeImage() has a fair chance, checked without doing the work. */
bool looksDecodable(QPDFObjectHandle image)
{
    QPDFObjectHandle dict = image.getDict();
    const QStringList filters = filtersOf(dict);

    // Qt reads JPEG; nothing here reads fax or JPEG 2000.
    if (filters.contains(u"/JPXDecode"_s) || filters.contains(u"/CCITTFaxDecode"_s)
        || filters.contains(u"/JBIG2Decode"_s)) {
        return false;
    }
    if (filters.contains(u"/DCTDecode"_s)) {
        return true;
    }

    const int components = componentsOf(dict.getKey("/ColorSpace"));
    QPDFObjectHandle bits = dict.getKey("/BitsPerComponent");
    const int bitsPerComponent = bits.isInteger() ? bits.getIntValueAsInt() : 8;
    if (components == 1 && (bitsPerComponent == 1 || bitsPerComponent == 8)) {
        return true;
    }
    return components == 3 && bitsPerComponent == 8;
}

QImage decodeImage(QPDFObjectHandle image, bool *wasJpeg)
{
    *wasJpeg = false;
    QPDFObjectHandle dict = image.getDict();
    const QStringList filters = filtersOf(dict);

    QPDFObjectHandle widthKey = dict.getKey("/Width");
    QPDFObjectHandle heightKey = dict.getKey("/Height");
    const int width = widthKey.isInteger() ? widthKey.getIntValueAsInt() : 0;
    const int height = heightKey.isInteger() ? heightKey.getIntValueAsInt() : 0;
    if (width <= 0 || height <= 0) {
        return {};
    }

    if (filters.contains(u"/DCTDecode"_s)) {
        // Everything before the JPEG is unwrapped; the JPEG itself goes to Qt.
        // Note the six-argument overload: the shorter one reports whether
        // filtering was attempted, not whether it worked, and a JPEG left
        // deliberately untouched would look like a failure.
        Pl_Buffer buffer("jpeg");
        bool attempted = false;
        if (!image.pipeStreamData(&buffer, &attempted, 0, qpdf_dl_specialized, true, false)) {
            return {};
        }
        const auto data = buffer.getBufferSharedPointer();
        const QImage decoded = QImage::fromData(
            QByteArray(reinterpret_cast<const char *>(data->getBuffer()), qsizetype(data->getSize())));
        *wasJpeg = !decoded.isNull();
        return decoded;
    }

    const int components = componentsOf(dict.getKey("/ColorSpace"));
    QPDFObjectHandle bits = dict.getKey("/BitsPerComponent");
    const int bitsPerComponent = bits.isInteger() ? bits.getIntValueAsInt() : 8;

    Pl_Buffer buffer("samples");
    bool attempted = false;
    if (!image.pipeStreamData(&buffer, &attempted, 0, qpdf_dl_all, true, false)) {
        return {};
    }
    if (!attempted && !filters.isEmpty()) {
        // Something is still wrapped around the samples that QPDF would not
        // unwrap, so what came out is not pixels.
        return {};
    }
    const auto data = buffer.getBufferSharedPointer();
    const auto *bytes = reinterpret_cast<const uchar *>(data->getBuffer());
    const qsizetype size = qsizetype(data->getSize());

    if (components == 1 && bitsPerComponent == 1) {
        const qsizetype stride = (qsizetype(width) + 7) / 8;
        if (size < stride * height) {
            return {};
        }
        // /DeviceGray means 0 is black, which is Format_MonoLSB inverted; Qt's
        // Format_Mono has bit 0 as the leftmost pixel and index 0 as colour 0.
        QImage mono(bytes, width, height, stride, QImage::Format_Mono);
        mono.setColorTable({ qRgb(0, 0, 0), qRgb(255, 255, 255) });
        QPDFObjectHandle decode = dict.getKey("/Decode");
        if (decode.isArray() && decode.getArrayNItems() == 2 && decode.getArrayItem(0).isNumber()
            && PdfGeometry::numericValue(decode.getArrayItem(0), 0.0) > 0.5) {
            mono.setColorTable({ qRgb(255, 255, 255), qRgb(0, 0, 0) });
        }
        return mono.convertToFormat(QImage::Format_RGB32);
    }

    if (components == 1 && bitsPerComponent == 8) {
        if (size < qsizetype(width) * height) {
            return {};
        }
        return QImage(bytes, width, height, width, QImage::Format_Grayscale8).convertToFormat(QImage::Format_RGB32);
    }

    if (components == 3 && bitsPerComponent == 8) {
        const qsizetype stride = qsizetype(width) * 3;
        if (size < stride * height) {
            return {};
        }
        return QImage(bytes, width, height, stride, QImage::Format_RGB888).convertToFormat(QImage::Format_RGB32);
    }

    return {};
}

/** Graphics state, as far as placing a glyph is concerned. */
struct TextState {
    QTransform ctm;
    QString fontName;
    double fontSize = 0.0;
    double charSpacing = 0.0;
    double wordSpacing = 0.0;
    double horizScale = 1.0;
    double leading = 0.0;
    double rise = 0.0;
};

class RedactFilter : public QPDFObjectHandle::TokenFilter
{
public:
    RedactFilter(QPDF &pdf, QPDFObjectHandle resources, QVector<QRectF> areas, bool probeOnly)
        : m_pdf(pdf)
        , m_resources(std::move(resources))
        , m_areas(std::move(areas))
        , m_probeOnly(probeOnly)
    {
    }

    void handleToken(QPDFTokenizer::Token const &token) override;

    int glyphsRemoved() const { return m_glyphsRemoved; }
    /** Pictures whose pixels were edited, so the originals can be chased down. */
    QVector<QPDFObjGen> editedOriginals() const { return m_editedOriginals; }
    int imagesEdited() const { return m_imagesEdited; }
    int imagesRemoved() const { return m_imagesRemoved; }
    int formsRemoved() const { return m_formsRemoved; }
    /** An overlapping picture in a format we cannot open: the caller must decide. */
    bool needsRaster() const { return m_needsRaster; }

private:
    void handleOperator(const QString &op);
    void showText(const QString &op);
    void handleDo();
    void writeOperands(const QString &op);
    void nextLine();
    bool intersectsArea(const QRectF &box) const;
    bool blankImage(QPDFObjectHandle image, std::string *newName);
    const FontInfo &currentFont();

    double operandNumber(int index) const
    {
        if (index < 0 || index >= m_operands.size()) {
            return 0.0;
        }
        return QString::fromStdString(m_operands.at(index).getValue()).toDouble();
    }

    QTransform operandMatrix() const
    {
        return QTransform(operandNumber(0), operandNumber(1), operandNumber(2), operandNumber(3), operandNumber(4),
                          operandNumber(5));
    }

    QPDF &m_pdf;
    QPDFObjectHandle m_resources;
    QVector<QRectF> m_areas;
    bool m_probeOnly = false;

    QVector<QPDFTokenizer::Token> m_operands;
    TextState m_state;
    QVector<TextState> m_stack;
    QTransform m_textMatrix;
    QTransform m_lineMatrix;
    QHash<QString, FontInfo> m_fonts;

    QVector<QPDFObjGen> m_editedOriginals;
    int m_glyphsRemoved = 0;
    int m_imagesEdited = 0;
    int m_imagesRemoved = 0;
    int m_formsRemoved = 0;
    bool m_needsRaster = false;
};

void RedactFilter::handleToken(QPDFTokenizer::Token const &token)
{
    switch (token.getType()) {
    case QPDFTokenizer::tt_space:
    case QPDFTokenizer::tt_comment:
        // Between operands, whitespace is written back by writeOperands(); on
        // its own it is worth keeping so the stream stays readable.
        if (m_operands.isEmpty()) {
            writeToken(token);
        }
        return;

    case QPDFTokenizer::tt_word:
        handleOperator(QString::fromStdString(token.getValue()));
        m_operands.clear();
        return;

    case QPDFTokenizer::tt_inline_image:
        if (intersectsArea(m_state.ctm.mapRect(QRectF(0, 0, 1, 1)))) {
            ++m_imagesRemoved;
        } else {
            writeToken(token);
        }
        m_operands.clear();
        return;

    case QPDFTokenizer::tt_eof:
        return;

    default:
        m_operands.append(token);
        return;
    }
}

void RedactFilter::writeOperands(const QString &op)
{
    for (const QPDFTokenizer::Token &token : std::as_const(m_operands)) {
        writeToken(token);
        write(" ");
    }
    write(op.toStdString());
    write("\n");
}

void RedactFilter::nextLine()
{
    m_lineMatrix = QTransform(1, 0, 0, 1, 0, -m_state.leading) * m_lineMatrix;
    m_textMatrix = m_lineMatrix;
}

bool RedactFilter::intersectsArea(const QRectF &box) const
{
    for (const QRectF &area : m_areas) {
        if (area.intersects(box)) {
            return true;
        }
    }
    return false;
}

const FontInfo &RedactFilter::currentFont()
{
    const auto cached = m_fonts.constFind(m_state.fontName);
    if (cached != m_fonts.constEnd()) {
        return *cached;
    }

    QPDFObjectHandle fonts = m_resources.isDictionary() ? m_resources.getKey("/Font") : QPDFObjectHandle::newNull();
    QPDFObjectHandle font
        = fonts.isDictionary() ? fonts.getKey(m_state.fontName.toStdString()) : QPDFObjectHandle::newNull();
    return *m_fonts.insert(m_state.fontName, loadFont(font));
}

void RedactFilter::handleOperator(const QString &op)
{
    if (op == u"q"_s) {
        m_stack.append(m_state);
    } else if (op == u"Q"_s) {
        if (!m_stack.isEmpty()) {
            m_state = m_stack.takeLast();
        }
    } else if (op == u"cm"_s && m_operands.size() >= 6) {
        m_state.ctm = operandMatrix() * m_state.ctm;
    } else if (op == u"BT"_s) {
        m_textMatrix = QTransform();
        m_lineMatrix = QTransform();
    } else if (op == u"Tf"_s && m_operands.size() >= 2) {
        m_state.fontName = QString::fromStdString(m_operands.at(0).getValue());
        m_state.fontSize = operandNumber(1);
    } else if (op == u"Tm"_s && m_operands.size() >= 6) {
        m_lineMatrix = operandMatrix();
        m_textMatrix = m_lineMatrix;
    } else if ((op == u"Td"_s || op == u"TD"_s) && m_operands.size() >= 2) {
        if (op == u"TD"_s) {
            m_state.leading = -operandNumber(1);
        }
        m_lineMatrix = QTransform(1, 0, 0, 1, operandNumber(0), operandNumber(1)) * m_lineMatrix;
        m_textMatrix = m_lineMatrix;
    } else if (op == u"T*"_s) {
        nextLine();
    } else if (op == u"TL"_s) {
        m_state.leading = operandNumber(0);
    } else if (op == u"Tc"_s) {
        m_state.charSpacing = operandNumber(0);
    } else if (op == u"Tw"_s) {
        m_state.wordSpacing = operandNumber(0);
    } else if (op == u"Tz"_s) {
        m_state.horizScale = operandNumber(0) / 100.0;
    } else if (op == u"Ts"_s) {
        m_state.rise = operandNumber(0);
    } else if (op == u"Tj"_s || op == u"TJ"_s || op == u"'"_s || op == u"\""_s) {
        showText(op);
        return;
    } else if (op == u"Do"_s) {
        handleDo();
        return;
    }

    writeOperands(op);
}

void RedactFilter::showText(const QString &op)
{
    // ' and " advance a line before drawing anything, so the state has to move
    // first or every glyph would be measured one line too high.
    std::string prefix;
    if (op == u"'"_s || op == u"\""_s) {
        if (op == u"\""_s && m_operands.size() >= 3) {
            m_state.wordSpacing = operandNumber(0);
            m_state.charSpacing = operandNumber(1);
            prefix
                = PdfGeometry::number(m_state.wordSpacing) + " Tw " + PdfGeometry::number(m_state.charSpacing) + " Tc ";
        }
        nextLine();
        prefix += "T* ";
    }

    QVector<QPDFTokenizer::Token> parts;
    if (op == u"TJ"_s) {
        for (const QPDFTokenizer::Token &token : std::as_const(m_operands)) {
            const auto type = token.getType();
            if (type == QPDFTokenizer::tt_string || type == QPDFTokenizer::tt_integer
                || type == QPDFTokenizer::tt_real) {
                parts.append(token);
            }
        }
    } else {
        for (const QPDFTokenizer::Token &token : std::as_const(m_operands)) {
            if (token.getType() == QPDFTokenizer::tt_string) {
                parts.clear();
                parts.append(token);
            }
        }
    }

    // A zero size draws nothing and would divide by zero below.
    if (qFuzzyIsNull(m_state.fontSize) || parts.isEmpty()) {
        writeOperands(op);
        return;
    }

    struct Element {
        bool isGlyph = false;
        bool dropped = false;
        std::string bytes;
        double adjustment = 0.0; // TJ number, for non-glyphs
        double tjEquivalent = 0.0; // the number that reproduces a dropped glyph's advance
    };

    const FontInfo &font = currentFont();
    const QTransform toDevice = m_textMatrix * m_state.ctm;
    const int step = font.twoByte ? 2 : 1;

    QVector<Element> elements;
    double offset = 0.0;
    bool anyDropped = false;

    for (const QPDFTokenizer::Token &token : std::as_const(parts)) {
        if (token.getType() != QPDFTokenizer::tt_string) {
            const double number = QString::fromStdString(token.getValue()).toDouble();
            offset -= number / 1000.0 * m_state.fontSize * m_state.horizScale;
            elements.append({ false, false, {}, number, 0.0 });
            continue;
        }

        const std::string bytes = token.getValue();
        for (size_t i = 0; i + size_t(step) <= bytes.size(); i += size_t(step)) {
            const int code = font.twoByte
                ? ((static_cast<unsigned char>(bytes[i]) << 8) | static_cast<unsigned char>(bytes[i + 1]))
                : static_cast<unsigned char>(bytes[i]);
            const double glyphWidth = font.widthFor(code);
            const bool isSpace = !font.twoByte && code == 32;
            const double extra = m_state.charSpacing + (isSpace ? m_state.wordSpacing : 0.0);
            const double advance = (glyphWidth * m_state.fontSize + extra) * m_state.horizScale;

            // Roughly ascender to descender: exact metrics would need the font
            // programme, and a box slightly too tall only removes more.
            const QRectF inTextSpace(offset, m_state.rise - 0.25 * m_state.fontSize,
                                     glyphWidth * m_state.fontSize * m_state.horizScale, 1.2 * m_state.fontSize);
            const bool dropped = intersectsArea(toDevice.mapRect(inTextSpace));
            anyDropped = anyDropped || dropped;

            Element element;
            element.isGlyph = true;
            element.dropped = dropped;
            element.bytes = bytes.substr(i, size_t(step));
            element.tjEquivalent = -1000.0 * (glyphWidth + extra / m_state.fontSize);
            elements.append(element);

            offset += advance;
        }
    }

    if (!anyDropped) {
        writeOperands(op);
        m_textMatrix = QTransform(1, 0, 0, 1, offset, 0) * m_textMatrix;
        return;
    }

    if (m_probeOnly) {
        m_glyphsRemoved += int(
            std::count_if(elements.cbegin(), elements.cend(), [](const Element &element) { return element.dropped; }));
        writeOperands(op);
        m_textMatrix = QTransform(1, 0, 0, 1, offset, 0) * m_textMatrix;
        return;
    }

    // Rewriting as one TJ keeps the line matrix untouched, because a Tm here would
    // also reset it and break the next Td.
    write(prefix);
    write("[");

    std::string pending;
    double adjustment = 0.0;
    const auto flushBytes = [&] {
        if (pending.empty()) {
            return;
        }
        writeToken(QPDFTokenizer::Token(QPDFTokenizer::tt_string, pending));
        pending.clear();
    };
    const auto flushAdjustment = [&] {
        if (std::abs(adjustment) < 1e-6) {
            adjustment = 0.0;
            return;
        }
        write(" " + PdfGeometry::number(adjustment) + " ");
        adjustment = 0.0;
    };

    for (const Element &element : std::as_const(elements)) {
        if (element.isGlyph && !element.dropped) {
            flushAdjustment();
            pending += element.bytes;
            continue;
        }
        flushBytes();
        adjustment += element.isGlyph ? element.tjEquivalent : element.adjustment;
        if (element.isGlyph) {
            ++m_glyphsRemoved;
        }
    }
    flushBytes();
    // A trailing number matters: it leaves the text matrix where the original
    // would have left it.
    flushAdjustment();
    write("] TJ\n");

    m_textMatrix = QTransform(1, 0, 0, 1, offset, 0) * m_textMatrix;
}

bool RedactFilter::blankImage(QPDFObjectHandle image, std::string *newName)
{
    bool invertible = false;
    const QTransform pageFromUnit = m_state.ctm.inverted(&invertible);
    if (!invertible) {
        return false;
    }

    bool wasJpeg = false;
    QImage decoded = decodeImage(image, &wasJpeg);
    if (decoded.isNull()) {
        return false;
    }

    const bool hasAlpha = decoded.hasAlphaChannel();
    QImage canvas = decoded.convertToFormat(hasAlpha ? QImage::Format_ARGB32 : QImage::Format_RGB32);
    if (canvas.isNull()) {
        return false;
    }

    QPainter painter(&canvas);
    // Source composition rather than blending, so a transparent pixel under
    // the box becomes opaque black instead of staying see-through.
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.setTransform(pageFromUnit * QTransform(canvas.width(), 0, 0, -canvas.height(), 0, canvas.height()));
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0, 0, 0, 255));
    const QRectF placed = m_state.ctm.mapRect(QRectF(0, 0, 1, 1));
    for (const QRectF &area : std::as_const(m_areas)) {
        if (area.intersects(placed)) {
            painter.drawRect(area);
        }
    }
    painter.end();

    QPDFObjectHandle embedded
        = wasJpeg && !hasAlpha ? PdfImage::embedAsJpeg(m_pdf, canvas, 92) : PdfImage::embed(m_pdf, canvas);
    if (!embedded.isStream()) {
        return false;
    }

    QPDFObjectHandle xobjects = m_resources.getKey("/XObject");
    if (!xobjects.isDictionary()) {
        return false;
    }
    // A fresh name, because the same picture may be placed twice on one page
    // and only this placement is being edited.
    int suffix = 0;
    std::string candidate;
    do {
        candidate = "/PsRedacted" + std::to_string(suffix++);
    } while (xobjects.hasKey(candidate));

    xobjects.replaceKey(candidate, embedded);
    *newName = candidate;
    return true;
}

void RedactFilter::handleDo()
{
    if (m_operands.isEmpty() || m_operands.constLast().getType() != QPDFTokenizer::tt_name) {
        writeOperands(u"Do"_s);
        return;
    }

    QPDFObjectHandle xobjects
        = m_resources.isDictionary() ? m_resources.getKey("/XObject") : QPDFObjectHandle::newNull();
    QPDFObjectHandle xobject
        = xobjects.isDictionary() ? xobjects.getKey(m_operands.constLast().getValue()) : QPDFObjectHandle::newNull();
    if (!xobject.isStream()) {
        writeOperands(u"Do"_s);
        return;
    }

    QPDFObjectHandle dict = xobject.getDict();
    const std::string subtype = nameOf(dict, "/Subtype");

    if (subtype == "/Image") {
        if (!intersectsArea(m_state.ctm.mapRect(QRectF(0, 0, 1, 1)))) {
            writeOperands(u"Do"_s);
            return;
        }
        if (!looksDecodable(xobject)) {
            m_needsRaster = true;
            ++m_imagesRemoved;
            return;
        }
        if (m_probeOnly) {
            ++m_imagesEdited;
            writeOperands(u"Do"_s);
            return;
        }

        std::string replacement;
        if (!blankImage(xobject, &replacement)) {
            m_needsRaster = true;
            ++m_imagesRemoved;
            return;
        }
        ++m_imagesEdited;
        m_editedOriginals.append(xobject.getObjGen());
        write(replacement);
        write(" Do\n");
        return;
    }

    if (subtype == "/Form") {
        QRectF extent(0, 0, 1, 1);
        QPDFObjectHandle bbox = dict.getKey("/BBox");
        if (bbox.isArray() && bbox.getArrayNItems() == 4) {
            const double left = PdfGeometry::boxValue(bbox, 0, 0.0);
            const double bottom = PdfGeometry::boxValue(bbox, 1, 0.0);
            const double right = PdfGeometry::boxValue(bbox, 2, 0.0);
            const double top = PdfGeometry::boxValue(bbox, 3, 0.0);
            extent
                = QRectF(std::min(left, right), std::min(bottom, top), std::abs(right - left), std::abs(top - bottom));
        }
        QPDFObjectHandle matrix = dict.getKey("/Matrix");
        QTransform formMatrix;
        if (matrix.isArray() && matrix.getArrayNItems() == 6) {
            // Six entries, so not a box: boxValue() would hand back the
            // fallbacks and leave the matrix at identity, which would put a
            // scaled or shifted drawing outside the area it really covers,
            // and an area judged clear is an area not redacted.
            formMatrix = QTransform(PdfGeometry::numberAt(matrix, 0, 1.0), PdfGeometry::numberAt(matrix, 1, 0.0),
                                    PdfGeometry::numberAt(matrix, 2, 0.0), PdfGeometry::numberAt(matrix, 3, 1.0),
                                    PdfGeometry::numberAt(matrix, 4, 0.0), PdfGeometry::numberAt(matrix, 5, 0.0));
        }

        if (!intersectsArea((formMatrix * m_state.ctm).mapRect(extent))) {
            writeOperands(u"Do"_s);
            return;
        }
        // Recursing into the form would be better; until then the whole thing
        // goes, which errs the safe way.
        ++m_formsRemoved;
        return;
    }

    writeOperands(u"Do"_s);
}

/** Replaces a page with a picture of itself, for content we cannot edit in place. */
bool rasteriseInstead(QPDF &pdf, QPDFPageObjectHelper &page, const QImage &rendered,
                      const QVector<QRectF> &displayAreas)
{
    if (rendered.isNull()) {
        return false;
    }

    const QRectF media = PdfGeometry::mediaBoxOf(page);
    const int rotate = PdfGeometry::rotationOf(page);
    const bool turned = rotate == 90 || rotate == 270;
    const double displayWidth = turned ? media.height() : media.width();
    const double displayHeight = turned ? media.width() : media.height();

    QImage canvas = rendered.convertToFormat(QImage::Format_RGB32);
    QPainter painter(&canvas);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0, 0, 0, 255));
    // Display points to pixels, with the y axis turned over.
    painter.setTransform(
        QTransform(canvas.width() / displayWidth, 0, 0, -canvas.height() / displayHeight, 0, canvas.height()));
    for (const QRectF &area : displayAreas) {
        painter.drawRect(area);
    }
    painter.end();

    QPDFObjectHandle embedded = PdfImage::embedAsJpeg(pdf, canvas, 90);
    if (!embedded.isStream()) {
        return false;
    }

    QPDFObjectHandle resources = QPDFObjectHandle::newDictionary();
    QPDFObjectHandle xobjects = QPDFObjectHandle::newDictionary();
    xobjects.replaceKey("/PsPage", embedded);
    resources.replaceKey("/XObject", xobjects);

    std::string content = "q\n";
    content += PdfGeometry::displayToPageMatrix(rotate, media.width(), media.height());
    content += PdfGeometry::number(displayWidth) + " 0 0 " + PdfGeometry::number(displayHeight) + " 0 0 cm\n";
    content += "/PsPage Do\nQ\n";

    QPDFObjectHandle object = page.getObjectHandle();
    object.replaceKey("/Resources", resources);
    object.replaceKey("/Contents", QPDFObjectHandle::newStream(&pdf, content));
    object.removeKey("/Annots");
    return true;
}

/**
 * Every XObject any page can still reach, following embedded drawings inwards.
 *
 * Editing a picture leaves a second copy of it behind, and the original is only
 * really gone once nothing points at it any more. Anything still in this set
 * after the work is done is a picture the writer will carry into the output,
 * which for a redaction is worth saying out loud.
 */
void collectReachableXObjects(QPDFObjectHandle resources, std::set<QPDFObjGen> &found, int depth = 0)
{
    if (depth > 8 || !resources.isDictionary()) {
        return;
    }
    QPDFObjectHandle xobjects = resources.getKey("/XObject");
    if (!xobjects.isDictionary()) {
        return;
    }

    for (const auto &[name, object] : xobjects.getDictAsMap()) {
        Q_UNUSED(name)
        if (!object.isStream() || found.count(object.getObjGen()) > 0) {
            continue;
        }
        found.insert(object.getObjGen());
        collectReachableXObjects(object.getDict().getKey("/Resources"), found, depth + 1);
    }
}

void removeAnnotations(QPDFPageObjectHelper &page, const QVector<QRectF> &pageAreas, int *removed)
{
    QPDFObjectHandle annotations = page.getObjectHandle().getKey("/Annots");
    if (!annotations.isArray()) {
        return;
    }

    QPDFObjectHandle kept = QPDFObjectHandle::newArray();
    for (int i = 0; i < annotations.getArrayNItems(); ++i) {
        QPDFObjectHandle annotation = annotations.getArrayItem(i);
        QPDFObjectHandle rect = annotation.isDictionary() ? annotation.getKey("/Rect") : QPDFObjectHandle::newNull();
        bool overlaps = false;
        if (rect.isArray() && rect.getArrayNItems() == 4) {
            const double left = PdfGeometry::boxValue(rect, 0, 0.0);
            const double bottom = PdfGeometry::boxValue(rect, 1, 0.0);
            const double right = PdfGeometry::boxValue(rect, 2, 0.0);
            const double top = PdfGeometry::boxValue(rect, 3, 0.0);
            const QRectF box(std::min(left, right), std::min(bottom, top), std::abs(right - left),
                             std::abs(top - bottom));
            for (const QRectF &area : pageAreas) {
                overlaps = overlaps || area.intersects(box);
            }
        }
        if (overlaps) {
            ++*removed;
        } else {
            kept.appendItem(annotation);
        }
    }

    if (kept.getArrayNItems() == 0) {
        page.getObjectHandle().removeKey("/Annots");
    } else {
        page.getObjectHandle().replaceKey("/Annots", kept);
    }
}

} // namespace

bool Redaction::apply(const QString &inputPdf, const QString &outputPdf, const QVector<Area> &areas,
                      const Options &options, Report *report, QString *error)
{
    if (areas.isEmpty()) {
        if (error) {
            *error = i18n("Nothing was marked for redaction.");
        }
        return false;
    }

    Report collected;
    try {
        QPDF pdf;
        PdfFile::open(pdf, inputPdf);

        QPDFPageDocumentHelper documents(pdf);
        std::vector<QPDFPageObjectHelper> pages = documents.getAllPages();

        QHash<int, QVector<QRectF>> byPage;
        for (const Area &area : areas) {
            if (area.page < 0 || area.page >= int(pages.size()) || area.rect.isEmpty()) {
                continue;
            }
            byPage[area.page].append(area.rect.normalized());
        }
        if (byPage.isEmpty()) {
            if (error) {
                *error = i18n("None of the marked areas fall on a page.");
            }
            return false;
        }

        QList<int> indexes = byPage.keys();
        std::sort(indexes.begin(), indexes.end());
        QVector<QPDFObjGen> editedImages;

        for (int index : std::as_const(indexes)) {
            QPDFPageObjectHelper page = pages.at(size_t(index));
            const QVector<QRectF> displayAreas = byPage.value(index);
            const QRectF media = PdfGeometry::mediaBoxOf(page);
            const QTransform toPage
                = PdfGeometry::displayToPageTransform(PdfGeometry::rotationOf(page), media.width(), media.height());

            QVector<QRectF> pageAreas;
            pageAreas.reserve(displayAreas.size());
            for (const QRectF &area : displayAreas) {
                pageAreas.append(toPage.mapRect(area).translated(media.x(), media.y()));
            }
            collected.areasApplied += pageAreas.size();

            // Resources have to be the page's own before anything is added to
            // them, or the edit would land on every page that inherits them.
            QPDFObjectHandle resources = page.getAttribute("/Resources", true);

            // A cheap look first: if a picture we cannot open sits in the way,
            // the whole page has to be handled differently, and it would be
            // wasteful to have rewritten the text by then.
            RedactFilter probe(pdf, resources, pageAreas, true);
            page.filterContents(&probe, nullptr);

            if (probe.needsRaster()) {
                if (options.pageRasteriser) {
                    const QImage rendered = options.pageRasteriser(index, options.rasterDpi);
                    if (rasteriseInstead(pdf, page, rendered, displayAreas)) {
                        collected.pagesRasterised.append(index);
                        collected.glyphsRemoved += probe.glyphsRemoved();
                        continue;
                    }
                }
                collected.warnings << i18n("Page %1 holds a picture in a format that cannot be edited "
                                           "(fax or JPEG 2000). It was removed rather than left in place.")
                                          .arg(index + 1);
            }

            Pl_Buffer buffer("redacted");
            RedactFilter filter(pdf, resources, pageAreas, false);
            page.filterContents(&filter, &buffer);

            const auto data = buffer.getBufferSharedPointer();
            const std::string content(reinterpret_cast<const char *>(data->getBuffer()), data->getSize());
            page.getObjectHandle().replaceKey("/Contents", QPDFObjectHandle::newStream(&pdf, content));

            // An edited picture is added beside the original rather than over
            // it, because the same picture may be placed twice and only one
            // placement was marked. That leaves the untouched original in the
            // page's resources, still holding every pixel that was meant to
            // go, so the names nothing draws any more have to be dropped.
            page.removeUnreferencedResources();
            editedImages += filter.editedOriginals();

            collected.glyphsRemoved += filter.glyphsRemoved();
            collected.imagesEdited += filter.imagesEdited();
            collected.imagesRemoved += filter.imagesRemoved();
            collected.formsRemoved += filter.formsRemoved();

            removeAnnotations(page, pageAreas, &collected.annotationsRemoved);

            if (options.paintBox) {
                std::string box = "q 0 0 0 rg\n";
                for (const QRectF &rect : std::as_const(pageAreas)) {
                    box += PdfGeometry::number(rect.x()) + " " + PdfGeometry::number(rect.y()) + " "
                        + PdfGeometry::number(rect.width()) + " " + PdfGeometry::number(rect.height()) + " re\n";
                }
                box += "f\nQ\n";
                PdfGeometry::isolateExistingContent(page, page.getObjectHandle());
                page.addPageContents(QPDFObjectHandle::newStream(&pdf, box), false);
            }
        }

        if (!editedImages.isEmpty()) {
            std::set<QPDFObjGen> reachable;
            for (QPDFPageObjectHelper &page : documents.getAllPages()) {
                collectReachableXObjects(page.getAttribute("/Resources", false), reachable);
            }
            for (const QPDFObjGen &original : std::as_const(editedImages)) {
                if (reachable.count(original) > 0) {
                    collected.warnings << i18n(
                        "A picture that was edited is also shown somewhere that was not marked, so the "
                        "original still travels with the file. Mark it everywhere it appears.");
                    break;
                }
            }
        }

        QPDFWriter writer(pdf, outputPdf.toUtf8().constData());
        writer.setObjectStreamMode(qpdf_o_generate);
        writer.setStreamDataMode(qpdf_s_compress);
        writer.write();
    } catch (const std::exception &e) {
        if (error) {
            *error = QString::fromUtf8(e.what());
        }
        return false;
    }

    if (report) {
        *report = collected;
    }
    return true;
}

QStringList Redaction::limitations()
{
    return {
        i18n("Text is removed character by character, so the rest of a line stays intact."),
        i18n("Pictures that overlap an area are edited pixel by pixel and re-saved."),
        i18n("Text drawn inside an embedded drawing is not searched; the whole drawing is "
             "removed instead when it overlaps."),
        i18n("Fax and JPEG 2000 pictures cannot be edited in place. The page is flattened to "
             "an image instead, which loses its selectable text."),
        i18nc("@info a limit of redaction; the quoted name is the “Clean Up Document…” menu entry "
              "and must read the same as that entry does",
              "Attachments, and text hidden in the document's own properties, are not "
              "touched. Use “Clean Up Document” for those."),
    };
}

} // namespace ps
