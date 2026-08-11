/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#include "TextEdit.h"

#include "Encodings.h"

#include "Core14Widths.h"
#include "FontEmbedder.h"
#include "GlyphNames.h"
#include "PdfFile.h"
#include "PdfGeometry.h"

#include <KLocalizedString>

#include <QHash>
#include <QRegularExpression>
#include <QTransform>

#include <qpdf/Pl_Buffer.hh>
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>
#include <qpdf/QPDFTokenizer.hh>
#include <qpdf/QPDFWriter.hh>

#include <algorithm>
#include <cmath>
#include <cstring>

using namespace Qt::Literals::StringLiterals;

namespace ps {

namespace {

using PdfGeometry::number;

/**
 * What a font can show, and how wide each character is.
 *
 * The two questions a replacement has to answer before it writes anything: can
 * this font produce a "ü" at all, and how much room will the new text take.
 */
struct FontInfo {
    bool twoByte = false;
    double defaultWidth = 0.5;

    /** Character code to advance width, in text space units. */
    QHash<int, double> widths;

    /** Character code to the character it draws. */
    QHash<int, QChar> toUnicode;

    /** The reverse, which is what encoding a replacement needs. */
    QHash<QChar, int> fromUnicode;

    /** True when the code space is known well enough to write into. */
    bool writable = false;

    QString limitation;

    /** What the face is, as far as anything meaning to draw it has to know. */
    QString family;
    int weight = 400;
    bool italic = false;
    bool serif = false;
    bool fixedPitch = false;
    bool embedded = false;

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

/** A hex run from a CMap, "004F0055" say, as the characters it stands for. */
QString charactersFromHex(const QString &hex)
{
    QString text;
    for (qsizetype i = 0; i + 3 < hex.size(); i += 4) {
        bool ok = false;
        const ushort code = hex.mid(i, 4).toUShort(&ok, 16);
        if (!ok) {
            return text;
        }
        text.append(QChar(code));
    }
    return text;
}

/**
 * Reads a `/ToUnicode` CMap.
 *
 * This is the one thing in a PDF that says what a glyph actually *means*, and
 * most producers write it because without it nobody can copy text out. Parsing
 * it gives both directions at once: what the page says, and which codes to write
 * to make it say something else.
 *
 * Matched with a pattern rather than split on the angle brackets. Splitting
 * looks simpler and is wrong: the newline before the first bracket becomes a
 * token of its own, every pair after it is off by one, and the whole page comes
 * back as replacement characters, which is exactly what happened.
 */
void parseToUnicode(QPDFObjectHandle stream, FontInfo &info)
{
    if (!stream.isStream()) {
        return;
    }

    std::shared_ptr<Buffer> buffer;
    try {
        buffer = stream.getStreamData();
    } catch (const std::exception &) {
        return;
    }
    const QString text
        = QString::fromLatin1(reinterpret_cast<const char *>(buffer->getBuffer()), qsizetype(buffer->getSize()));

    const auto remember = [&info](int code, const QString &characters) {
        if (code < 0 || characters.size() != 1) {
            // A code standing for a ligature ("ffi" as one glyph) can be read
            // but not written, because there is no telling which of several
            // codes to use going back.
            return;
        }
        info.toUnicode.insert(code, characters.at(0));
        if (!info.fromUnicode.contains(characters.at(0))) {
            info.fromUnicode.insert(characters.at(0), code);
        }
    };

    static const QRegularExpression hexPair(u"<([0-9A-Fa-f]+)>\\s*<([0-9A-Fa-f]*)>"_s);
    static const QRegularExpression hexTriple(u"<([0-9A-Fa-f]+)>\\s*<([0-9A-Fa-f]+)>\\s*<([0-9A-Fa-f]*)>"_s);
    static const QRegularExpression rangeToArray(u"<([0-9A-Fa-f]+)>\\s*<([0-9A-Fa-f]+)>\\s*\\[([^\\]]*)\\]"_s);
    static const QRegularExpression hexItem(u"<([0-9A-Fa-f]*)>"_s);

    // The single-mapping form: <code> <character>.
    qsizetype at = 0;
    while ((at = text.indexOf(u"beginbfchar"_s, at)) >= 0) {
        const qsizetype end = text.indexOf(u"endbfchar"_s, at);
        if (end < 0) {
            break;
        }
        const QString block = text.mid(at + 11, end - at - 11);
        auto matches = hexPair.globalMatch(block);
        while (matches.hasNext()) {
            const QRegularExpressionMatch match = matches.next();
            bool ok = false;
            const int code = match.captured(1).toInt(&ok, 16);
            if (ok) {
                remember(code, charactersFromHex(match.captured(2)));
            }
        }
        at = end;
    }

    // The range form, in both its spellings: a run of consecutive characters,
    // or an explicit list of them.
    at = 0;
    while ((at = text.indexOf(u"beginbfrange"_s, at)) >= 0) {
        const qsizetype end = text.indexOf(u"endbfrange"_s, at);
        if (end < 0) {
            break;
        }
        const QString block = text.mid(at + 12, end - at - 12);

        auto arrays = rangeToArray.globalMatch(block);
        QString withoutArrays = block;
        while (arrays.hasNext()) {
            const QRegularExpressionMatch match = arrays.next();
            bool ok = false;
            const int from = match.captured(1).toInt(&ok, 16);
            if (!ok) {
                continue;
            }
            int code = from;
            auto items = hexItem.globalMatch(match.captured(3));
            while (items.hasNext()) {
                remember(code, charactersFromHex(items.next().captured(1)));
                ++code;
            }
            // Taken out so the plain form below does not read it again as a
            // three-value range.
            withoutArrays.replace(match.captured(0), QString());
        }

        auto triples = hexTriple.globalMatch(withoutArrays);
        while (triples.hasNext()) {
            const QRegularExpressionMatch match = triples.next();
            bool okFrom = false;
            bool okTo = false;
            const int from = match.captured(1).toInt(&okFrom, 16);
            const int to = match.captured(2).toInt(&okTo, 16);
            const QString start = charactersFromHex(match.captured(3));
            if (!okFrom || !okTo || to < from || start.size() != 1) {
                continue;
            }
            // A range of millions would be a malformed file, not a font.
            const int last = std::min(to, from + 65535);
            for (int code = from; code <= last; ++code) {
                remember(code, QString(QChar(ushort(start.at(0).unicode() + (code - from)))));
            }
        }
        at = end;
    }
}

/**
 * A glyph name as the character it draws.
 *
 * Three forms turn up. The explicit ones (`uni00E9`, `u00E9`) say the answer
 * outright. Named ones need the table. And a name may carry a variant suffix
 * after a full stop: `one.fitted` is still a one, which is the convention every
 * font tool follows and is why documents full of `.fitted` digits are readable
 * at all. Names like `g42` or `cid17` mean nothing outside their own font, and
 * are refused rather than guessed at.
 */
QChar characterForGlyphName(const QString &glyphName)
{
    QString name = glyphName;
    if (name.startsWith(u'/')) {
        name = name.mid(1);
    }
    // The variant suffix, which says how the glyph looks and not what it is.
    const qsizetype dot = name.indexOf(u'.');
    if (dot > 0) {
        name = name.left(dot);
    }
    if (name.isEmpty()) {
        return {};
    }

    if (name.size() >= 5 && name.startsWith(u"uni"_s)) {
        bool ok = false;
        const ushort code = name.mid(3, 4).toUShort(&ok, 16);
        if (ok) {
            return QChar(code);
        }
    }
    if (name.size() >= 5 && name.at(0) == u'u') {
        bool ok = false;
        const uint code = name.mid(1).toUInt(&ok, 16);
        if (ok && code <= 0xFFFF) {
            return QChar(ushort(code));
        }
    }

    const QByteArray needle = name.toLatin1();
    const auto found = std::lower_bound(std::begin(GlyphNames::table), std::end(GlyphNames::table), needle,
                                        [](const GlyphNames::Entry &entry, const QByteArray &wanted) {
                                            return std::strcmp(entry.name, wanted.constData()) < 0;
                                        });
    if (found != std::end(GlyphNames::table) && std::strcmp(found->name, needle.constData()) == 0) {
        return QChar(found->character);
    }
    return {};
}

/**
 * Works out what each code of a simple font means.
 *
 * The named encodings are the easy half. The other half is an /Encoding
 * dictionary carrying a /Differences array, which is what subset fonts from
 * every serious layout program use, and without reading it, the text of most
 * real documents comes back as a row of question marks.
 */
void applySimpleEncoding(QPDFObjectHandle font, FontInfo &info)
{
    QPDFObjectHandle encoding = font.getKey("/Encoding");
    const std::string baseName = encoding.isName()
        ? encoding.getName()
        : (encoding.isDictionary() && encoding.getKey("/BaseEncoding").isName()
               ? encoding.getKey("/BaseEncoding").getName()
               : std::string());

    // Above 126 the three encodings part company, and reading any of them as
    // Latin-1 loses real text. WinAnsi fills 0x80 to 0x9F, which Latin-1 leaves
    // empty: the en dash, the euro sign, the ellipsis and all four curly quotes
    // live there, so every one of them came back as a replacement character:
    // in extracted text, in search, and in redaction, where a phrase that fails
    // to match is not a cosmetic problem. MacRoman disagrees with Latin-1 across
    // the whole upper half, so a Mac-produced "Grüße" came back as "GrÅ°e".
    const char16_t *table = nullptr;
    if (baseName == "/WinAnsiEncoding") {
        table = Encodings::winAnsi;
    } else if (baseName == "/MacRomanEncoding") {
        table = Encodings::macRoman;
    }

    // An unnamed or standard encoding is left as it was: StandardEncoding is not
    // Latin-1 either, but it agrees across the printable ASCII range, and above
    // that a document that means anything unusual says so with /Differences.
    const bool latinBase = table != nullptr || baseName.empty() || baseName == "/StandardEncoding";
    if (latinBase) {
        for (int code = 32; code <= 255; ++code) {
            if (!info.widths.isEmpty() && !info.widths.contains(code)) {
                continue; // A subset font holds what the document used, no more.
            }
            char16_t unicode = char16_t(code);
            if (code > 126) {
                unicode
                    = table ? table[code] : char16_t(QString::fromLatin1(QByteArray(1, char(code))).at(0).unicode());
            }
            const QChar character(unicode);
            if (unicode != 0 && character.isPrint()) {
                info.toUnicode.insert(code, character);
            }
        }
    }

    QPDFObjectHandle differences
        = encoding.isDictionary() ? encoding.getKey("/Differences") : QPDFObjectHandle::newNull();
    if (differences.isArray()) {
        int code = 0;
        for (int i = 0; i < differences.getArrayNItems(); ++i) {
            QPDFObjectHandle item = differences.getArrayItem(i);
            if (item.isInteger()) {
                code = item.getIntValueAsInt();
                continue;
            }
            if (!item.isName()) {
                continue;
            }
            const QChar character = characterForGlyphName(QString::fromStdString(item.getName()));
            if (character.isNull()) {
                info.toUnicode.remove(code);
            } else {
                info.toUnicode.insert(code, character);
            }
            ++code;
        }
    }

    for (auto it = info.toUnicode.constBegin(); it != info.toUnicode.constEnd(); ++it) {
        if (!info.fromUnicode.contains(it.value())) {
            info.fromUnicode.insert(it.value(), it.key());
        }
    }
}

/** `/BaseFont` as a person reads it: no leading slash, no `ABCDEF+` subset tag. */
QString familyOf(const std::string &baseFont)
{
    QString name = QString::fromStdString(baseFont);
    if (name.startsWith(u'/')) {
        name = name.mid(1);
    }
    // The prefix is exactly six capitals and a plus, which is what every
    // producer writes and what keeps "A+B Grotesk" from losing its first word.
    static const QRegularExpression subsetTag(u"^[A-Z]{6}\\+"_s);
    return name.remove(subsetTag);
}

/**
 * What a font's name says about its weight, on the scale everything else uses.
 *
 * A PDF states weight in three places and often in none of them: `/FontWeight`
 * is optional, `/StemV` is a stem width rather than a weight, and the only thing
 * always present is the name the designer gave the cut. So the name is read
 * first, because "Graphik-Semibold" is a statement and a stem width of 144 is a
 * guess.
 */
int weightFromName(const QString &family)
{
    const QString name = family.toLower().remove(u'-').remove(u' ').remove(u'_');

    // Longest first: "extrabold" contains "bold", and "semibold" contains it too.
    struct Named {
        const char16_t *word;
        int weight;
    };
    static constexpr Named table[] = {
        { u"extralight", 200 }, { u"ultralight", 200 }, { u"semibold", 600 }, { u"demibold", 600 },
        { u"extrabold", 800 },  { u"ultrabold", 800 },  { u"thin", 100 },     { u"light", 300 },
        { u"book", 400 },       { u"regular", 400 },    { u"normal", 400 },   { u"roman", 400 },
        { u"medium", 500 },     { u"demi", 600 },       { u"black", 900 },    { u"heavy", 900 },
        { u"bold", 700 },
    };
    for (const Named &entry : table) {
        if (name.contains(QString::fromUtf16(entry.word))) {
            return entry.weight;
        }
    }
    return 0;
}

/** The descriptor of a font, reaching through a Type 0 font to its descendant. */
QPDFObjectHandle descriptorOf(QPDFObjectHandle font)
{
    QPDFObjectHandle descriptor = font.getKey("/FontDescriptor");
    if (descriptor.isDictionary()) {
        return descriptor;
    }
    QPDFObjectHandle descendants = font.getKey("/DescendantFonts");
    if (descendants.isArray() && descendants.getArrayNItems() > 0) {
        return descendants.getArrayItem(0).getKey("/FontDescriptor");
    }
    return QPDFObjectHandle::newNull();
}

/**
 * Everything about the face that is not about which characters it holds.
 *
 * Read even for a font that cannot be written into: a run nobody may retype is
 * still drawn on the screen, and drawing it in the interface font because it
 * could not be edited would be the same mistake in a quieter place.
 */
void describeFace(QPDFObjectHandle font, const std::string &baseFont, FontInfo &info)
{
    info.family = familyOf(baseFont);

    QPDFObjectHandle descriptor = descriptorOf(font);
    const int flags = descriptor.isDictionary() && descriptor.getKey("/Flags").isInteger()
        ? descriptor.getKey("/Flags").getIntValueAsInt()
        : 0;
    info.fixedPitch = (flags & 1) != 0;
    info.serif = (flags & 2) != 0;
    info.italic = (flags & 64) != 0;

    // The standard fourteen carry no descriptor at all (every reader is
    // required to know them), so the only thing that says Courier is a
    // typewriter face is its name.
    const QString plain = info.family.toLower().remove(u'-').remove(u' ');
    if (!descriptor.isDictionary()) {
        info.fixedPitch = plain.contains(u"courier"_s) || plain.contains(u"mono"_s);
        info.serif = !info.fixedPitch
            && (plain.contains(u"times"_s) || plain.contains(u"serif"_s) || plain.contains(u"roman"_s)
                || plain.contains(u"georgia"_s) || plain.contains(u"garamond"_s) || plain.contains(u"minion"_s));
        info.italic = plain.contains(u"italic"_s) || plain.contains(u"oblique"_s);
    }

    if (descriptor.isDictionary()) {
        // An angle of its own outranks the flag: producers set one and forget
        // the other, and a face leaning eight degrees is leaning whatever any
        // bit says.
        const double angle = PdfGeometry::numericValue(descriptor.getKey("/ItalicAngle"), 0.0);
        if (std::abs(angle) > 0.5) {
            info.italic = true;
        }
        for (const char *key : { "/FontFile", "/FontFile2", "/FontFile3" }) {
            if (descriptor.getKey(key).isStream()) {
                info.embedded = true;
                break;
            }
        }
    }

    info.weight = weightFromName(info.family);
    if (info.weight == 0 && descriptor.isDictionary() && descriptor.getKey("/FontWeight").isNumber()) {
        info.weight = int(std::lround(PdfGeometry::numericValue(descriptor.getKey("/FontWeight"), 400.0)));
    }
    if (info.weight == 0) {
        // The last resort, and the least trustworthy: a stem width depends on
        // the design as much as on the weight, so it is only asked whether this
        // is bold rather than how bold.
        const double stem
            = descriptor.isDictionary() ? PdfGeometry::numericValue(descriptor.getKey("/StemV"), 0.0) : 0.0;
        info.weight = (flags & 0x40000) != 0 || stem > 120.0 ? 700 : 400;
    }
    // A name with no cut in it, from a serif face with no descriptor at all,
    // still has to answer something; regular is what such a font almost is.
    info.weight = std::clamp(info.weight, 100, 900);
}

/** The standard-14 metrics, for a font that gives no /Widths of its own. */
void applyCore14(const std::string &baseFont, FontInfo &info)
{
    QString name = QString::fromStdString(baseFont);
    const qsizetype plus = name.indexOf(u'+');
    if (plus >= 0) {
        name = name.mid(plus + 1);
    }
    const QString normalised = name.toLower().remove(u'-').remove(u' ').remove(u'/');

    const bool bold = normalised.contains(u"bold"_s);
    const bool slanted = normalised.contains(u"italic"_s) || normalised.contains(u"oblique"_s);

    QString wanted = u"Helvetica"_s;
    if (normalised.contains(u"courier"_s) || normalised.contains(u"mono"_s)) {
        wanted = u"Courier"_s;
    } else if (normalised.contains(u"times"_s) || normalised.contains(u"serif"_s)) {
        wanted = u"Times-Roman"_s;
    }
    if (wanted == u"Times-Roman"_s) {
        if (bold && slanted) {
            wanted = u"Times-BoldItalic"_s;
        } else if (bold) {
            wanted = u"Times-Bold"_s;
        } else if (slanted) {
            wanted = u"Times-Italic"_s;
        }
    } else {
        if (bold && slanted) {
            wanted += u"-BoldOblique"_s;
        } else if (bold) {
            wanted += u"-Bold"_s;
        } else if (slanted) {
            wanted += u"-Oblique"_s;
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
        info.limitation = i18n("The page does not say which font this text uses.");
        return info;
    }

    const std::string subtype = nameOf(font, "/Subtype");
    const std::string baseFont = nameOf(font, "/BaseFont");

    describeFace(font, baseFont, info);
    parseToUnicode(font.getKey("/ToUnicode"), info);

    if (subtype == "/Type0") {
        info.twoByte = true;
        info.defaultWidth = 1.0;
        QPDFObjectHandle descendants = font.getKey("/DescendantFonts");
        if (descendants.isArray() && descendants.getArrayNItems() > 0) {
            QPDFObjectHandle cid = descendants.getArrayItem(0);
            QPDFObjectHandle defaultWidth = cid.isDictionary() ? cid.getKey("/DW") : QPDFObjectHandle::newNull();
            if (defaultWidth.isNumber()) {
                info.defaultWidth = PdfGeometry::numericValue(defaultWidth, 1000.0) / 1000.0;
            }
        }
    } else {
        QPDFObjectHandle widths = font.getKey("/Widths");
        QPDFObjectHandle firstChar = font.getKey("/FirstChar");
        if (widths.isArray() && firstChar.isInteger()) {
            const int first = firstChar.getIntValueAsInt();
            for (int i = 0; i < widths.getArrayNItems(); ++i) {
                info.widths.insert(first + i, PdfGeometry::numberAt(widths, i, 0.0) / 1000.0);
            }
        }
        if (info.widths.isEmpty()) {
            applyCore14(baseFont, info);
        }

        // No /ToUnicode: work it out from the encoding instead, which is the
        // case for most documents that were not made for copying text out of.
        if (info.toUnicode.isEmpty()) {
            applySimpleEncoding(font, info);
        }
    }

    info.writable = !info.fromUnicode.isEmpty();
    if (!info.writable) {
        info.limitation = i18n("This font does not say which characters it holds, so replacing its text would "
                               "produce the wrong letters.");
    }
    return info;
}

/** How many numbers a `sc`/`scn` carries, and what they mean. */
enum class Ink {
    Unknown, //!< say nothing rather than guess; the operand count decides
    Gray,
    Rgb,
    Cmyk,
    Tint, //!< a separation or DeviceN: ink coverage, not a colour
};

/**
 * What a colour space named by `cs` turns out to be.
 *
 * Only the family matters here. Turning a Lab or an Indexed value into a screen
 * colour properly means running the space's own conversion, which is a colour
 * engine's work; what this is for is knowing whether the three numbers after
 * `scn` are red, green and blue, and that much the family answers.
 */
Ink inkOf(QPDFObjectHandle resources, const QString &name)
{
    if (name == u"/DeviceGray"_s || name == u"/G"_s || name == u"/CalGray"_s) {
        return Ink::Gray;
    }
    if (name == u"/DeviceRGB"_s || name == u"/RGB"_s || name == u"/CalRGB"_s) {
        return Ink::Rgb;
    }
    if (name == u"/DeviceCMYK"_s || name == u"/CMYK"_s) {
        return Ink::Cmyk;
    }

    QPDFObjectHandle spaces = resources.isDictionary() ? resources.getKey("/ColorSpace") : QPDFObjectHandle::newNull();
    QPDFObjectHandle space = spaces.isDictionary() ? spaces.getKey(name.toStdString()) : QPDFObjectHandle::newNull();
    if (space.isName()) {
        return inkOf(QPDFObjectHandle::newNull(), QString::fromStdString(space.getName()));
    }
    if (!space.isArray() || space.getArrayNItems() < 1 || !space.getArrayItem(0).isName()) {
        return Ink::Unknown;
    }

    const QString family = QString::fromStdString(space.getArrayItem(0).getName());
    if (family == u"/Separation"_s || family == u"/DeviceN"_s) {
        return Ink::Tint;
    }
    if (family == u"/ICCBased"_s && space.getArrayNItems() >= 2 && space.getArrayItem(1).isStream()) {
        switch (space.getArrayItem(1).getDict().getKey("/N").isInteger()
                    ? space.getArrayItem(1).getDict().getKey("/N").getIntValueAsInt()
                    : 0) {
        case 1:
            return Ink::Gray;
        case 3:
            return Ink::Rgb;
        case 4:
            return Ink::Cmyk;
        default:
            return Ink::Unknown;
        }
    }
    return inkOf(QPDFObjectHandle::newNull(), family);
}

/** Graphics and text state, as far as placing a run of text needs it. */
struct State {
    QTransform ctm;
    QString fontName;
    double fontSize = 0.0;
    double charSpacing = 0.0;
    double wordSpacing = 0.0;
    double horizScale = 1.0;
    double leading = 0.0;
    double rise = 0.0;

    /** The non-stroking colour, which is what a filled glyph comes out. */
    QColor fill = QColor(Qt::black);

    /** What `scn`'s numbers mean, as the last `cs` said. */
    Ink ink = Ink::Unknown;

    /** `Tr`: 3 draws nothing at all, which is how a scan hides its text layer. */
    int renderMode = 0;
};

/**
 * Walks a page's text, and on the way through swaps in whatever was asked for.
 *
 * One filter for both jobs on purpose: reading and writing have to agree exactly
 * on which show operator is the third one on the page, and two separate walkers
 * would eventually disagree.
 */
class TextFilter : public QPDFObjectHandle::TokenFilter
{
public:
    TextFilter(QPDFObjectHandle resources, const QTransform &toDisplay, int page)
        : m_resources(std::move(resources))
        , m_toDisplay(toDisplay)
        , m_page(page)
    {
    }

    /**
     * One run set differently, with the new face already in the page's resources.
     *
     * The family is a resource name rather than a family name because by the
     * time the stream is being written the font is in the file: choosing a face
     * and cutting it down is the caller's work, and doing it from inside a token
     * filter would mean changing the document while walking it.
     */
    struct Restyle {
        /** The `/Fx` to set, or empty to keep the run's own. */
        QString fontName;

        /** In text space, as `Tf` takes it. Zero keeps the run's own size. */
        double size = 0.0;

        /** Invalid keeps the colour the page had set. */
        QColor colour;

        bool isEmpty() const { return fontName.isEmpty() && size <= 0.0 && !colour.isValid(); }
    };

    /** Collect runs instead of rewriting. */
    void setReading(bool reading) { m_reading = reading; }

    void setReplacements(const QHash<int, QString> &replacements, const TextEdit::Options &options)
    {
        m_replacements = replacements;
        m_options = options;
    }

    void setRestyles(const QHash<int, Restyle> &restyles) { m_restyles = restyles; }

    void handleToken(QPDFTokenizer::Token const &token) override;

    QVector<TextEdit::Run> runs() const { return m_runs; }
    int replaced() const { return m_replaced; }
    int restyled() const { return m_restyled; }
    QStringList refusals() const { return m_refusals; }
    double widthAdjustment() const { return m_widthAdjustment; }
    QStringList overflowed() const { return m_overflowed; }

private:
    void handleOperator(const QString &op);
    void showText(const QString &op);
    void writeOperands(const QString &op);
    void setFillColour(const QString &op);
    void nextLine();
    const FontInfo &fontNamed(const QString &name);
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

    QPDFObjectHandle m_resources;
    QTransform m_toDisplay;
    int m_page = 0;

    bool m_reading = true;
    QHash<int, QString> m_replacements;
    QHash<int, Restyle> m_restyles;
    TextEdit::Options m_options;

    QVector<QPDFTokenizer::Token> m_operands;
    State m_state;
    QVector<State> m_stack;
    QTransform m_textMatrix;
    QTransform m_lineMatrix;
    QHash<QString, FontInfo> m_fonts;

    int m_showIndex = -1;
    QVector<TextEdit::Run> m_runs;
    int m_replaced = 0;
    int m_restyled = 0;
    QStringList m_refusals;
    double m_widthAdjustment = 0.0;
    QStringList m_overflowed;
};

void TextFilter::handleToken(QPDFTokenizer::Token const &token)
{
    switch (token.getType()) {
    case QPDFTokenizer::tt_space:
    case QPDFTokenizer::tt_comment:
        if (m_operands.isEmpty() && !m_reading) {
            writeToken(token);
        }
        return;
    case QPDFTokenizer::tt_word:
        handleOperator(QString::fromStdString(token.getValue()));
        m_operands.clear();
        return;
    case QPDFTokenizer::tt_inline_image:
        if (!m_reading) {
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

void TextFilter::writeOperands(const QString &op)
{
    if (m_reading) {
        return;
    }
    for (const QPDFTokenizer::Token &token : std::as_const(m_operands)) {
        writeToken(token);
        write(" ");
    }
    write(op.toStdString());
    write("\n");
}

/**
 * The non-stroking colour, from whichever of the six ways the page set it.
 *
 * Only the fill: a glyph is filled unless the page says otherwise with `Tr`, and
 * a page that strokes its type is rare enough that reading the fill and being
 * plainly wrong about the outline beats reading neither.
 */
void TextFilter::setFillColour(const QString &op)
{
    const int count = int(m_operands.size());
    if (op == u"cs"_s) {
        m_state.ink = count >= 1 && m_operands.at(0).getType() == QPDFTokenizer::tt_name
            ? inkOf(m_resources, QString::fromStdString(m_operands.at(0).getValue()))
            : Ink::Unknown;
        // The spec sets black on a change of space, and a page that says `cs`
        // and then draws without `scn` means that black rather than whatever
        // the last space happened to leave behind.
        m_state.fill = QColor(Qt::black);
        return;
    }

    Ink ink = op == u"g"_s ? Ink::Gray : op == u"rg"_s ? Ink::Rgb : op == u"k"_s ? Ink::Cmyk : m_state.ink;
    if (op == u"sc"_s || op == u"scn"_s) {
        // A pattern is named rather than numbered and has no one colour, so the
        // page keeps whatever it had rather than being told it is now black.
        if (count >= 1 && m_operands.constLast().getType() == QPDFTokenizer::tt_name) {
            return;
        }
        if (ink == Ink::Unknown) {
            ink = count == 1 ? Ink::Gray : count == 3 ? Ink::Rgb : count == 4 ? Ink::Cmyk : Ink::Unknown;
        }
    }

    const auto clamped = [this](int index) { return std::clamp(operandNumber(index), 0.0, 1.0); };
    switch (ink) {
    case Ink::Gray:
        if (count >= 1) {
            m_state.fill = QColor::fromRgbF(float(clamped(0)), float(clamped(0)), float(clamped(0)));
        }
        break;
    case Ink::Rgb:
        if (count >= 3) {
            m_state.fill = QColor::fromRgbF(float(clamped(0)), float(clamped(1)), float(clamped(2)));
        }
        break;
    case Ink::Cmyk:
        if (count >= 4) {
            m_state.fill
                = QColor::fromCmykF(float(clamped(0)), float(clamped(1)), float(clamped(2)), float(clamped(3)));
        }
        break;
    case Ink::Tint: {
        // A separation says how much ink, not which colour, and the alternate
        // space's tint transform is the only thing that knows. Full ink is dark
        // and no ink is paper, which is the part of the answer that is certain.
        double most = 0.0;
        for (int i = 0; i < count; ++i) {
            most = std::max(most, clamped(i));
        }
        m_state.fill = QColor::fromRgbF(float(1.0 - most), float(1.0 - most), float(1.0 - most));
        break;
    }
    case Ink::Unknown:
        break;
    }
}

void TextFilter::nextLine()
{
    m_lineMatrix = QTransform(1, 0, 0, 1, 0, -m_state.leading) * m_lineMatrix;
    m_textMatrix = m_lineMatrix;
}

const FontInfo &TextFilter::fontNamed(const QString &name)
{
    const auto cached = m_fonts.constFind(name);
    if (cached != m_fonts.constEnd()) {
        return *cached;
    }
    QPDFObjectHandle fonts = m_resources.isDictionary() ? m_resources.getKey("/Font") : QPDFObjectHandle::newNull();
    QPDFObjectHandle font = fonts.isDictionary() ? fonts.getKey(name.toStdString()) : QPDFObjectHandle::newNull();
    return *m_fonts.insert(name, loadFont(font));
}

const FontInfo &TextFilter::currentFont()
{
    return fontNamed(m_state.fontName);
}

void TextFilter::handleOperator(const QString &op)
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
    } else if (op == u"Tr"_s) {
        m_state.renderMode = int(std::lround(operandNumber(0)));
    } else if (op == u"g"_s || op == u"rg"_s || op == u"k"_s || op == u"cs"_s || op == u"sc"_s || op == u"scn"_s) {
        setFillColour(op);
    } else if (op == u"Tj"_s || op == u"TJ"_s || op == u"'"_s || op == u"\""_s) {
        showText(op);
        return;
    }

    writeOperands(op);
}

void TextFilter::showText(const QString &op)
{
    ++m_showIndex;

    std::string prefix;
    if (op == u"'"_s || op == u"\""_s) {
        if (op == u"\""_s && m_operands.size() >= 3) {
            m_state.wordSpacing = operandNumber(0);
            m_state.charSpacing = operandNumber(1);
            prefix = number(m_state.wordSpacing) + " Tw " + number(m_state.charSpacing) + " Tc ";
        }
        nextLine();
        prefix += "T* ";
    }

    // Gather the shown bytes and the position adjustments between them.
    QVector<QPair<std::string, double>> parts; // bytes, then an adjustment after
    if (op == u"TJ"_s) {
        std::string pending;
        for (const QPDFTokenizer::Token &token : std::as_const(m_operands)) {
            if (token.getType() == QPDFTokenizer::tt_string) {
                pending += token.getValue();
            } else if (token.getType() == QPDFTokenizer::tt_integer || token.getType() == QPDFTokenizer::tt_real) {
                parts.append({ pending, QString::fromStdString(token.getValue()).toDouble() });
                pending.clear();
            }
        }
        if (!pending.empty()) {
            parts.append({ pending, 0.0 });
        }
    } else {
        for (const QPDFTokenizer::Token &token : std::as_const(m_operands)) {
            if (token.getType() == QPDFTokenizer::tt_string) {
                parts.clear();
                parts.append({ token.getValue(), 0.0 });
            }
        }
    }

    const FontInfo &font = currentFont();
    const int step = font.twoByte ? 2 : 1;

    // Decode, and measure, in one pass.
    QString text;
    double advance = 0.0;
    for (const auto &[bytes, adjustment] : parts) {
        for (size_t i = 0; i + size_t(step) <= bytes.size(); i += size_t(step)) {
            const int code = font.twoByte
                ? ((static_cast<unsigned char>(bytes[i]) << 8) | static_cast<unsigned char>(bytes[i + 1]))
                : static_cast<unsigned char>(bytes[i]);
            const auto character = font.toUnicode.constFind(code);
            text.append(character != font.toUnicode.constEnd() ? *character : QChar(u'�'));

            const bool isSpace = !font.twoByte && code == 32;
            advance += (font.widthFor(code) * m_state.fontSize + m_state.charSpacing
                        + (isSpace ? m_state.wordSpacing : 0.0))
                * m_state.horizScale;
        }
        advance -= adjustment / 1000.0 * m_state.fontSize * m_state.horizScale;
    }

    const QTransform toDevice = m_textMatrix * m_state.ctm;
    const QRectF box
        = toDevice.mapRect(QRectF(0.0, m_state.rise - 0.25 * m_state.fontSize, advance, 1.2 * m_state.fontSize));

    if (m_reading) {
        if (!text.trimmed().isEmpty()) {
            // How much the matrices magnify text space. Taken from the area
            // rather than from one cell so that a rotated or mirrored matrix
            // still answers a size rather than a zero.
            const QTransform onPage = toDevice * m_toDisplay;
            const double scale = std::sqrt(std::abs(onPage.determinant()));

            TextEdit::Run run;
            run.page = m_page;
            run.index = m_showIndex;
            run.text = text;
            run.rect = m_toDisplay.mapRect(box);
            run.fontResource = m_state.fontName;
            run.fontSize = m_state.fontSize;
            run.scaledSize = m_state.fontSize * scale;
            run.fontFamily = font.family;
            run.fontWeight = font.weight;
            run.italic = font.italic;
            run.serif = font.serif;
            run.fixedPitch = font.fixedPitch;
            run.embedded = font.embedded;
            run.colour = m_state.fill;
            run.charSpacing = m_state.charSpacing * scale;
            run.wordSpacing = m_state.wordSpacing * scale;
            run.horizontalScale = m_state.horizScale;
            run.renderMode = m_state.renderMode;
            run.editable = font.writable && !text.contains(QChar(u'�'));
            if (!font.writable) {
                run.limitation = font.limitation;
            } else if (!run.editable) {
                run.limitation = i18n("Some of this text uses characters the font does not explain, so it cannot be "
                                      "read back reliably.");
            }
            m_runs.append(run);
        }
        m_textMatrix = QTransform(1, 0, 0, 1, advance, 0) * m_textMatrix;
        return;
    }

    const auto wanted = m_replacements.constFind(m_showIndex);
    if (wanted == m_replacements.constEnd()) {
        writeOperands(op);
        m_textMatrix = QTransform(1, 0, 0, 1, advance, 0) * m_textMatrix;
        return;
    }

    // Which face and which size the new text is set in. Copied rather than
    // referred to: asking for a font not seen before puts it in the same hash
    // the current one lives in, and a reference into a hash that has just grown
    // is a reference to nothing.
    // A page that shows text without ever naming a font is malformed, and there
    // would be nothing to put back after an override, so its type is left alone.
    const Restyle restyle = m_state.fontName.isEmpty() ? Restyle() : m_restyles.value(m_showIndex);
    const bool differentFace = !restyle.fontName.isEmpty() && restyle.fontName != m_state.fontName;
    const FontInfo target = differentFace ? fontNamed(restyle.fontName) : font;

    // The wanted size arrives in display points, because that is the only size
    // anybody can see; `Tf` takes text space, so it is divided back out by
    // whatever the matrices are doing here.
    const double onPage = std::sqrt(std::abs((m_textMatrix * m_state.ctm * m_toDisplay).determinant()));
    const double size = restyle.size > 0.0 && onPage > 0.0 ? restyle.size / onPage : m_state.fontSize;

    // Encode the replacement in the font's own code space. A character the font
    // has never held has no code, and writing one anyway would draw a different
    // letter, which is worse than saying no.
    std::string encoded;
    QStringList missing;
    double newAdvance = 0.0;
    for (const QChar &character : *wanted) {
        const auto code = target.fromUnicode.constFind(character);
        if (code == target.fromUnicode.constEnd()) {
            if (!missing.contains(QString(character))) {
                missing << QString(character);
            }
            continue;
        }
        if (target.twoByte) {
            encoded += char((*code >> 8) & 0xFF);
            encoded += char(*code & 0xFF);
        } else {
            encoded += char(*code & 0xFF);
        }
        const bool isSpace = !target.twoByte && *code == 32;
        newAdvance += (target.widthFor(*code) * size + m_state.charSpacing + (isSpace ? m_state.wordSpacing : 0.0))
            * m_state.horizScale;
    }

    if (!missing.isEmpty()) {
        m_refusals << (differentFace ? i18n("“%1” was left as it was: the face asked for does not contain %2.", text,
                                            missing.join(u", "_s))
                                     : i18n("“%1” was left as it was: this document's font does not contain %2.", text,
                                            missing.join(u", "_s)));
        writeOperands(op);
        m_textMatrix = QTransform(1, 0, 0, 1, advance, 0) * m_textMatrix;
        return;
    }

    // Squeeze only what would be too wide. Growing text is what collides with
    // its neighbours; shrinking text just ends sooner, and stretching it back
    // out to the old width would produce letters nobody would call type.
    double squeeze = 1.0;
    if (m_options.fitWidth && newAdvance > advance + 0.01 && advance > 0.01) {
        squeeze = std::max(m_options.minimumSqueeze, advance / newAdvance);
        m_widthAdjustment = std::max(m_widthAdjustment, (1.0 - squeeze) * 100.0);
        if (newAdvance * squeeze > advance + 0.5) {
            m_overflowed << i18n("“%1” is longer than the space it replaces and reaches past it.", *wanted);
        }
    }

    const bool setsFont = differentFace || !qFuzzyCompare(size, m_state.fontSize);
    const bool differentColour = restyle.colour.isValid() && restyle.colour != m_state.fill;

    write(prefix);
    if (setsFont) {
        write((differentFace ? restyle.fontName : m_state.fontName).toStdString() + " " + number(size) + " Tf\n");
    }
    if (differentColour) {
        write(number(restyle.colour.redF()) + " " + number(restyle.colour.greenF()) + " "
              + number(restyle.colour.blueF()) + " rg\n");
    }
    if (!qFuzzyCompare(squeeze, 1.0)) {
        write(number(m_state.horizScale * squeeze * 100.0) + " Tz\n");
    }
    writeToken(QPDFTokenizer::Token(QPDFTokenizer::tt_string, encoded));
    write(" Tj\n");
    if (!qFuzzyCompare(squeeze, 1.0)) {
        // Put the scaling back, or every later run on the page inherits it.
        write(number(m_state.horizScale * 100.0) + " Tz\n");
    }
    // The page's own state has to be exactly as it was by the time the next
    // operator runs: a `Tf` or an `rg` left behind here would restyle every run
    // after this one, which is how one corrected word turns a column red.
    if (differentColour) {
        write(number(m_state.fill.redF()) + " " + number(m_state.fill.greenF()) + " " + number(m_state.fill.blueF())
              + " rg\n");
    }
    if (setsFont) {
        write(m_state.fontName.toStdString() + " " + number(m_state.fontSize) + " Tf\n");
    }

    ++m_replaced;
    if (setsFont || differentColour) {
        ++m_restyled;
    }
    // The line follows the text, exactly as it would in any editor: a shorter
    // run pulls whatever comes next on the line back with it.
    m_textMatrix = QTransform(1, 0, 0, 1, newAdvance * squeeze, 0) * m_textMatrix;
}

/**
 * Puts every face a batch asks for into the page, and names them.
 *
 * One embedding per face rather than one per run: two lines restyled into the
 * same typeface should be one subset in the file and not two, and the only way
 * to manage that is to know every character wanted before any of it is cut down.
 *
 * A face that cannot be found or cannot be cut is reported and dropped, leaving
 * the size and the colour of that run to be changed anyway: half of what was
 * asked for is better than a page of empty boxes where the glyphs should be.
 */
QHash<int, TextFilter::Restyle> embedWantedFaces(QPDF &pdf, QPDFObjectHandle resources,
                                                 const QHash<int, TextEdit::Format> &formats,
                                                 const QHash<int, QString> &texts, TextEdit::Report *report)
{
    QHash<int, TextFilter::Restyle> restyles;
    if (formats.isEmpty()) {
        return restyles;
    }

    struct Wanted {
        QString family;
        bool bold = false;
        bool italic = false;
        QSet<QChar> characters;
        QVector<int> runs;
    };
    QHash<QString, Wanted> faces;

    for (auto format = formats.constBegin(); format != formats.constEnd(); ++format) {
        TextFilter::Restyle restyle;
        restyle.size = format->size;
        restyle.colour = format->colour;
        restyles.insert(format.key(), restyle);
        if (format->family.isEmpty()) {
            continue;
        }

        const QString cut
            = format->family + (format->bold ? u"·b"_s : QString()) + (format->italic ? u"·i"_s : QString());
        Wanted &face = faces[cut];
        face.family = format->family;
        face.bold = format->bold;
        face.italic = format->italic;
        for (const QChar &character : texts.value(format.key())) {
            face.characters.insert(character);
        }
        face.runs.append(format.key());
    }

    for (auto face = faces.constBegin(); face != faces.constEnd(); ++face) {
        FontEmbedder::Request request;
        request.family = face->family;
        request.bold = face->bold;
        request.italic = face->italic;
        request.characters = face->characters;

        FontEmbedder::Result result;
        QString why;
        if (!FontEmbedder::embed(pdf, resources, request, &result, &why)) {
            if (report) {
                report->refusals << i18nc("@info %1 is a font family, %2 the reason it could not be used",
                                          "The text keeps the face it had: “%1” could not be put into this "
                                          "document. %2",
                                          face->family, why);
            }
            continue;
        }
        if (report) {
            report->notes += result.warnings;
        }
        for (const int run : std::as_const(face->runs)) {
            restyles[run].fontName = result.resourceName;
        }
    }
    return restyles;
}

/** Display points to page space, /Rotate and a shifted media box included. */
QTransform pageTransformFor(QPDFPageObjectHelper &page)
{
    const QRectF media = PdfGeometry::mediaBoxOf(page);
    return PdfGeometry::displayToPageTransform(PdfGeometry::rotationOf(page), media.width(), media.height())
        * QTransform::fromTranslate(media.x(), media.y());
}

} // namespace

QVector<TextEdit::Run> TextEdit::runsOn(const QString &pdf, int page, QString *error)
{
    QVector<Run> runs;
    try {
        QPDF document;
        PdfFile::open(document, pdf);

        QPDFPageDocumentHelper documents(document);
        std::vector<QPDFPageObjectHelper> pages = documents.getAllPages();
        if (page < 0 || page >= int(pages.size())) {
            if (error) {
                *error = i18n("The document has no page %1.", page + 1);
            }
            return runs;
        }

        QPDFPageObjectHelper handle = pages.at(size_t(page));
        bool invertible = false;
        const QTransform toDisplay = pageTransformFor(handle).inverted(&invertible);
        if (!invertible) {
            return runs;
        }

        TextFilter filter(handle.getAttribute("/Resources", false), toDisplay, page);
        filter.setReading(true);
        handle.filterContents(&filter, nullptr);
        runs = filter.runs();
    } catch (const std::exception &e) {
        if (error) {
            *error = QString::fromUtf8(e.what());
        }
        return {};
    }
    return runs;
}

bool TextEdit::apply(const QString &inputPdf, const QString &outputPdf, const QVector<Replacement> &replacements,
                     const Options &options, Report *report, QString *error)
{
    if (replacements.isEmpty()) {
        if (error) {
            *error = i18n("There is no text to change.");
        }
        return false;
    }

    Report collected;
    try {
        QPDF pdf;
        PdfFile::open(pdf, inputPdf);

        QPDFPageDocumentHelper documents(pdf);
        std::vector<QPDFPageObjectHelper> pages = documents.getAllPages();

        QHash<int, QHash<int, QString>> byPage;
        QHash<int, QHash<int, Format>> formats;
        for (const Replacement &replacement : replacements) {
            if (replacement.page < 0 || replacement.page >= int(pages.size()) || replacement.index < 0) {
                continue;
            }
            byPage[replacement.page].insert(replacement.index, replacement.text);
            if (!replacement.format.isEmpty()) {
                formats[replacement.page].insert(replacement.index, replacement.format);
            }
        }
        if (byPage.isEmpty()) {
            if (error) {
                *error = i18n("None of the text to change is on a page of this document.");
            }
            return false;
        }

        QList<int> indexes = byPage.keys();
        std::sort(indexes.begin(), indexes.end());

        for (int index : std::as_const(indexes)) {
            QPDFPageObjectHelper page = pages.at(size_t(index));
            bool invertible = false;
            const QTransform toDisplay = pageTransformFor(page).inverted(&invertible);

            // Copied if shared, because a face asked for on one page must not
            // appear on every page that happens to inherit the same dictionary.
            // Only when there is a face to add: a plain correction should leave
            // the page tree exactly as it found it.
            const QHash<int, Format> wanted = formats.value(index);
            const bool addsFonts = std::any_of(wanted.constBegin(), wanted.constEnd(),
                                               [](const Format &format) { return !format.family.isEmpty(); });
            QPDFObjectHandle resources = page.getAttribute("/Resources", addsFonts);

            QHash<int, TextFilter::Restyle> restyles
                = embedWantedFaces(pdf, resources, wanted, byPage.value(index), &collected);

            Pl_Buffer buffer("edited");
            TextFilter filter(resources, invertible ? toDisplay : QTransform(), index);
            filter.setReading(false);
            filter.setReplacements(byPage.value(index), options);
            filter.setRestyles(restyles);
            page.filterContents(&filter, &buffer);

            const auto data = buffer.getBufferSharedPointer();
            page.getObjectHandle().replaceKey(
                "/Contents",
                QPDFObjectHandle::newStream(
                    &pdf, std::string(reinterpret_cast<const char *>(data->getBuffer()), data->getSize())));

            collected.replaced += filter.replaced();
            collected.restyled += filter.restyled();
            collected.refusals += filter.refusals();
            collected.widthAdjustment = std::max(collected.widthAdjustment, filter.widthAdjustment());
            collected.overflowed += filter.overflowed();
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

QStringList TextEdit::limitations()
{
    return {
        i18n("Text is replaced in the page's own font, so the result matches everything around it."),
        i18n("A replacement that is too wide for its space is squeezed to fit, so that it does not run into what "
             "sits beside it. A shorter one simply makes the line shorter."),
        i18n("Nothing on other lines moves, because a PDF does not record which line follows which."),
        i18n("A font embedded in a document usually carries only the characters that document used. Asking for a "
             "character it does not hold is refused rather than drawn wrongly."),
        i18n("Text drawn inside an embedded drawing, and text that is part of a scanned picture, cannot be "
             "changed this way."),
    };
}

} // namespace ps
