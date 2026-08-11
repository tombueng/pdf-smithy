/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#include "FontInventory.h"

#include "FontEmbedder.h"
#include "GlyphNames.h"
#include "PdfFile.h"

#include <KLocalizedString>

#include <QCryptographicHash>
#include <QFile>
#include <QHash>
#include <QRegularExpression>

#include <qpdf/QPDF.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>
#include <qpdf/QPDFTokenizer.hh>
#include <qpdf/QPDFWriter.hh>

#include <algorithm>
#include <cstring>
#include <functional>
#include <map>
#include <set>

using namespace Qt::Literals::StringLiterals;

namespace ps {

namespace {

/** How deep to follow form XObjects when looking for fonts. */
constexpr int MaxResourceDepth = 8;

/** A subset's tag: six capitals and a plus, which is the convention everywhere. */
const QRegularExpression &subsetTag()
{
    static const QRegularExpression pattern(u"^[A-Z]{6}\\+"_s);
    return pattern;
}

std::string nameOf(QPDFObjectHandle object, const char *key)
{
    if (!object.isDictionary()) {
        return {};
    }
    QPDFObjectHandle value = object.getKey(key);
    return value.isName() ? value.getName() : std::string();
}

// ══ Encoding, read exactly the way TextEdit reads it ═══════════════════════
//
// The four functions below are a deliberate copy of their counterparts in
// TextEdit.cpp, not a move. Both files need precisely this reading of a PDF's
// encoding (it is the only thing in a file that says what a glyph means), and
// both were being written at the same time, so factoring it out would have meant
// editing a file another pair of hands was already in. The copy is small and
// finished; when TextEdit settles, one of the two becomes the shared one.

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
 * Matched with a pattern rather than split on the angle brackets. Splitting
 * looks simpler and is wrong: the newline before the first bracket becomes a
 * token of its own, every pair after it is off by one, and the whole page comes
 * back as replacement characters.
 */
QHash<int, QChar> parseToUnicode(QPDFObjectHandle stream)
{
    QHash<int, QChar> mapping;
    if (!stream.isStream()) {
        return mapping;
    }

    std::shared_ptr<Buffer> buffer;
    try {
        buffer = stream.getStreamData();
    } catch (const std::exception &) {
        return mapping;
    }
    const QString text
        = QString::fromLatin1(reinterpret_cast<const char *>(buffer->getBuffer()), qsizetype(buffer->getSize()));

    const auto remember = [&mapping](int code, const QString &characters) {
        // A code standing for a ligature ("ffi" as one glyph) is readable but
        // says nothing about a single character, so it is left out.
        if (code >= 0 && characters.size() == 1) {
            mapping.insert(code, characters.at(0));
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

    // The range form, in both its spellings: a run of consecutive characters, or
    // an explicit list of them.
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
    return mapping;
}

/**
 * A glyph name as the character it draws.
 *
 * Three forms turn up. The explicit ones (`uni00E9`, `u00E9`) say the answer
 * outright. Named ones need the table. And a name may carry a variant suffix
 * after a full stop: `one.fitted` is still a one, which is the convention every
 * font tool follows. Names like `g42` or `cid17` mean nothing outside their own
 * font, and are refused rather than guessed at.
 */
QChar characterForGlyphName(const QString &glyphName)
{
    QString name = glyphName;
    if (name.startsWith(u'/')) {
        name = name.mid(1);
    }
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
QHash<int, QChar> simpleEncoding(QPDFObjectHandle font)
{
    QHash<int, QChar> mapping;
    if (!font.isDictionary()) {
        return mapping;
    }

    // Which codes the font has widths for, because a subset holds what the
    // document used and nothing else.
    QSet<int> known;
    QPDFObjectHandle widths = font.getKey("/Widths");
    QPDFObjectHandle firstChar = font.getKey("/FirstChar");
    if (widths.isArray() && firstChar.isInteger()) {
        const int first = firstChar.getIntValueAsInt();
        for (int i = 0; i < widths.getArrayNItems(); ++i) {
            known.insert(first + i);
        }
    }

    QPDFObjectHandle encoding = font.getKey("/Encoding");
    const std::string baseName = encoding.isName()
        ? encoding.getName()
        : (encoding.isDictionary() && encoding.getKey("/BaseEncoding").isName()
               ? encoding.getKey("/BaseEncoding").getName()
               : std::string());

    // WinAnsi, MacRoman and the standard encoding all agree with ASCII across
    // the printable range, which is what the base map is used for here.
    const bool latinBase = baseName.empty() || baseName == "/WinAnsiEncoding" || baseName == "/StandardEncoding"
        || baseName == "/MacRomanEncoding";
    if (latinBase) {
        for (int code = 32; code <= 255; ++code) {
            if (!known.isEmpty() && !known.contains(code)) {
                continue;
            }
            const QChar character
                = code <= 126 ? QChar(ushort(code)) : QString::fromLatin1(QByteArray(1, char(code))).at(0);
            if (character.isPrint()) {
                mapping.insert(code, character);
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
                mapping.remove(code);
            } else {
                mapping.insert(code, character);
            }
            ++code;
        }
    }
    return mapping;
}

// ══ The font objects themselves ════════════════════════════════════════════

quint16 u16At(const QByteArray &data, qsizetype at)
{
    if (at < 0 || at + 1 >= data.size()) {
        return 0;
    }
    return quint16((quint16(quint8(data.at(at))) << 8) | quint8(data.at(at + 1)));
}

quint32 u32At(const QByteArray &data, qsizetype at)
{
    return (quint32(u16At(data, at)) << 16) | u16At(data, at + 2);
}

/**
 * The descriptor that carries the glyphs, wherever this kind of font keeps it.
 *
 * A composite font is a wrapper: the descriptor belongs to the CIDFont
 * underneath it, and asking the wrapper gives nothing.
 */
QPDFObjectHandle descriptorOf(QPDFObjectHandle font)
{
    if (!font.isDictionary()) {
        return QPDFObjectHandle::newNull();
    }
    if (nameOf(font, "/Subtype") == "/Type0") {
        QPDFObjectHandle descendants = font.getKey("/DescendantFonts");
        if (descendants.isArray() && descendants.getArrayNItems() > 0) {
            return descendants.getArrayItem(0).getKey("/FontDescriptor");
        }
        return QPDFObjectHandle::newNull();
    }
    return font.getKey("/FontDescriptor");
}

QPDFObjectHandle programmeOf(QPDFObjectHandle font)
{
    QPDFObjectHandle descriptor = descriptorOf(font);
    if (!descriptor.isDictionary()) {
        return QPDFObjectHandle::newNull();
    }
    // Type 1, TrueType and the compact/OpenType forms respectively.
    for (const char *key : { "/FontFile2", "/FontFile3", "/FontFile" }) {
        QPDFObjectHandle programme = descriptor.getKey(key);
        if (programme.isStream()) {
            return programme;
        }
    }
    return QPDFObjectHandle::newNull();
}

QByteArray streamBytes(QPDFObjectHandle stream)
{
    if (!stream.isStream()) {
        return {};
    }
    try {
        std::shared_ptr<Buffer> buffer = stream.getStreamData();
        return QByteArray(reinterpret_cast<const char *>(buffer->getBuffer()), qsizetype(buffer->getSize()));
    } catch (const std::exception &) {
        // A font compressed with a filter this build cannot undo is still a
        // font that is present, which is the part that matters.
        return {};
    }
}

/**
 * What an embedded TrueType says about being embedded onward.
 *
 * The two bits at the bottom of the OS/2 table's fsType field are a licence in
 * sixteen bits: whether the font may travel in a document at all, and whether it
 * may do so in a document somebody can edit. Reported, never enforced; see the
 * class documentation for why.
 */
QString licenceOf(const QByteArray &programme)
{
    if (programme.size() < 12) {
        return {};
    }
    const quint32 flavour = u32At(programme, 0);
    if (flavour != 0x00010000u && flavour != 0x74727565u && flavour != 0x4F54544Fu) {
        return {}; // A Type 1 or bare compact programme has no OS/2 table to ask.
    }
    const int count = u16At(programme, 4);
    qsizetype os2 = -1;
    for (int i = 0; i < count; ++i) {
        const qsizetype at = 12 + qsizetype(i) * 16;
        if (programme.mid(at, 4) == QByteArrayLiteral("OS/2")) {
            os2 = qsizetype(u32At(programme, at + 8));
            break;
        }
    }
    if (os2 < 0 || os2 + 10 > programme.size()) {
        return {};
    }

    const quint16 fsType = u16At(programme, os2 + 8);
    const quint16 permission = fsType & 0x000F;
    QStringList notes;
    if (permission == 0x0002) {
        notes << i18n("The licence forbids embedding this font in a document.");
    } else if (permission == 0x0004) {
        notes << i18n("The licence allows embedding for viewing and printing only, not for editing.");
    } else if (permission == 0x0008) {
        notes << i18n("The licence allows embedding, including in a document that can be edited.");
    } else {
        notes << i18n("The licence puts no restriction on embedding this font.");
    }
    if (fsType & 0x0100) {
        notes << i18n("It may not be embedded in part, only whole.");
    }
    if (fsType & 0x0200) {
        notes << i18n("Only its bitmaps may be embedded, not its outlines.");
    }
    return notes.join(u' ');
}

/** How the file spells the font's encoding, in its own words rather than ours. */
QString encodingOf(QPDFObjectHandle font)
{
    QPDFObjectHandle encoding = font.getKey("/Encoding");
    if (encoding.isName()) {
        return QString::fromStdString(encoding.getName());
    }
    if (encoding.isStream()) {
        // An embedded CMap, which names itself.
        return QString::fromStdString(nameOf(encoding.getDict(), "/CMapName"));
    }
    if (!encoding.isDictionary()) {
        return {}; // The font's own built-in encoding.
    }
    QString described = QString::fromStdString(nameOf(encoding, "/BaseEncoding"));
    if (encoding.getKey("/Differences").isArray()) {
        described += described.isEmpty() ? u"/Differences"_s : u"+/Differences"_s;
    }
    return described;
}

FontUse describeFont(QPDFObjectHandle font, const QString &resourceName)
{
    FontUse use;
    use.resourceName = resourceName;
    use.subtype = QString::fromStdString(nameOf(font, "/Subtype"));
    use.baseFont = QString::fromStdString(nameOf(font, "/BaseFont"));
    if (use.baseFont.startsWith(u'/')) {
        use.baseFont.remove(0, 1);
    }
    use.subset = subsetTag().match(use.baseFont).hasMatch();
    use.family = use.baseFont;
    use.family.remove(subsetTag());
    use.encoding = encodingOf(font);

    QPDFObjectHandle programme = programmeOf(font);
    if (programme.isStream()) {
        use.embedded = true;
        const QByteArray bytes = streamBytes(programme);
        use.licence = licenceOf(bytes);
        if (!bytes.isEmpty()) {
            use.fileSize = bytes.size();
        } else {
            QPDFObjectHandle length = programme.getDict().getKey("/Length1");
            if (!length.isInteger()) {
                length = programme.getDict().getKey("/Length");
            }
            use.fileSize = length.isInteger() ? length.getIntValue() : 0;
        }
    } else if (use.subtype == u"/Type3"_s) {
        // A Type 3 font's glyphs are drawings inside the document already, so
        // there is nothing to embed and nothing that could go missing.
        use.embedded = true;
    }

    const QHash<int, QChar> fromCMap = parseToUnicode(font.getKey("/ToUnicode"));
    use.hasToUnicode = !fromCMap.isEmpty();
    const QHash<int, QChar> mapping
        = !fromCMap.isEmpty() || use.subtype == u"/Type0"_s ? fromCMap : simpleEncoding(font);
    for (auto it = mapping.constBegin(); it != mapping.constEnd(); ++it) {
        use.coverage.insert(it.value());
    }
    return use;
}

using FontVisitor = std::function<void(QPDFObjectHandle fonts, const std::string &key, QPDFObjectHandle font)>;

/**
 * Hands every font dictionary reachable from @p resources to @p visit.
 *
 * Form XObjects carry resources of their own, and a font used only inside one is
 * still a font the page uses: a watermark, a letterhead, a stamp. Following
 * them needs the loop guard: a form whose resources reach itself is rare, is
 * legal enough that QPDF will hand it over, and would otherwise recurse until
 * the stack ran out.
 */
void eachFont(QPDFObjectHandle resources, std::set<QPDFObjGen> &visited, int depth, const FontVisitor &visit)
{
    if (depth > MaxResourceDepth || !resources.isDictionary()) {
        return;
    }

    QPDFObjectHandle fonts = resources.getKey("/Font");
    if (fonts.isDictionary()) {
        // A copy of the map, so that a visitor may replace what it is handed.
        for (const auto &[key, font] : fonts.getDictAsMap()) {
            if (font.isDictionary()) {
                visit(fonts, key, font);
            }
        }
    }

    QPDFObjectHandle xobjects = resources.getKey("/XObject");
    if (!xobjects.isDictionary()) {
        return;
    }
    for (const auto &[key, object] : xobjects.getDictAsMap()) {
        Q_UNUSED(key)
        if (!object.isStream()) {
            continue;
        }
        if (object.isIndirect()) {
            if (visited.count(object.getObjGen()) > 0) {
                continue;
            }
            visited.insert(object.getObjGen());
        }
        eachFont(object.getDict().getKey("/Resources"), visited, depth + 1, visit);
    }
}

/** The appearance streams of a page's annotations, which have fonts of their own. */
void eachAnnotationFont(QPDFObjectHandle page, std::set<QPDFObjGen> &visited, const FontVisitor &visit)
{
    QPDFObjectHandle annotations = page.getKey("/Annots");
    if (!annotations.isArray()) {
        return;
    }
    for (int i = 0; i < annotations.getArrayNItems(); ++i) {
        QPDFObjectHandle appearance = annotations.getArrayItem(i).isDictionary()
            ? annotations.getArrayItem(i).getKey("/AP")
            : QPDFObjectHandle::newNull();
        if (!appearance.isDictionary()) {
            continue;
        }
        QPDFObjectHandle normal = appearance.getKey("/N");
        // /N is a stream, or a dictionary of one stream per state: a tick box
        // has one appearance for on and another for off.
        QVector<QPDFObjectHandle> streams;
        if (normal.isStream()) {
            streams.append(normal);
        } else if (normal.isDictionary()) {
            for (const auto &[key, state] : normal.getDictAsMap()) {
                Q_UNUSED(key)
                if (state.isStream()) {
                    streams.append(state);
                }
            }
        }
        for (QPDFObjectHandle stream : std::as_const(streams)) {
            eachFont(stream.getDict().getKey("/Resources"), visited, 1, visit);
        }
    }
}

/** Every font dictionary in the file, whether or not a page can reach it. */
void eachFontInDocument(QPDF &pdf, const FontVisitor &visit)
{
    std::set<QPDFObjGen> done;
    for (QPDFObjectHandle object : pdf.getAllObjects()) {
        if (!object.isDictionary()) {
            continue;
        }
        if (nameOf(object, "/Type") != "/Font" && !object.hasKey("/BaseFont")) {
            continue;
        }
        if (object.isIndirect()) {
            if (done.count(object.getObjGen()) > 0) {
                continue;
            }
            done.insert(object.getObjGen());
        }
        visit(QPDFObjectHandle::newNull(), std::string(), object);
    }

    // A font dictionary written straight into a page's resources is not an
    // object of its own, so getAllObjects() never sees it. Rare in files from
    // large programs, common in files from small ones.
    QPDFPageDocumentHelper documents(pdf);
    for (QPDFPageObjectHelper &page : documents.getAllPages()) {
        std::set<QPDFObjGen> visited;
        const FontVisitor direct = [&visit](QPDFObjectHandle fonts, const std::string &key, QPDFObjectHandle font) {
            if (!font.isIndirect()) {
                visit(fonts, key, font);
            }
        };
        eachFont(page.getAttribute("/Resources", false), visited, 0, direct);
        eachAnnotationFont(page.getObjectHandle(), visited, direct);
    }
}

QString withoutSubsetTag(const QString &baseFont)
{
    QString name = baseFont;
    if (name.startsWith(u'/')) {
        name.remove(0, 1);
    }
    return name.remove(subsetTag());
}

/**
 * Whether a font is one of the ones asked for.
 *
 * Named with or without the subset tag, and case is not insisted on: nobody
 * types "ABCDEF+LiberationSerif" from memory, and the tag differs per subset
 * anyway, so a request for "Liberation Serif" has to reach all four of them.
 */
bool matchesAny(const QString &baseFont, const QStringList &wanted)
{
    if (wanted.isEmpty()) {
        return true; // Every embedded font, which is what an empty list asks for.
    }
    const QString bare = withoutSubsetTag(baseFont);
    const QString squashed = QString(bare).remove(u' ');
    for (const QString &name : wanted) {
        const QString target = withoutSubsetTag(name);
        if (baseFont.compare(name, Qt::CaseInsensitive) == 0) {
            return true;
        }
        if (bare.compare(target, Qt::CaseInsensitive) == 0) {
            return true;
        }
        // "DejaVuSans" for "DejaVu Sans": a PostScript name has no spaces and
        // the person typing the request does not know that.
        if (squashed.compare(QString(target).remove(u' '), Qt::CaseInsensitive) == 0) {
            return true;
        }
    }
    return false;
}

/**
 * What makes two font objects interchangeable.
 *
 * Everything a page's bytes depend on: the kind of font, the family, the codes
 * and their widths, the encoding, and the glyph programme itself. Two fonts with
 * the same fingerprint draw the same letters for the same bytes, so one can
 * stand in for the other and nothing on any page needs rewriting. Anything less
 * than all of that and a merge would silently change what a page says.
 */
QByteArray fingerprintOf(QPDFObjectHandle font)
{
    QPDFObjectHandle programme = programmeOf(font);
    const QByteArray bytes = streamBytes(programme);
    if (bytes.isEmpty()) {
        return {}; // Not embedded, or unreadable: nothing to save by merging.
    }

    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(QByteArray::fromStdString(nameOf(font, "/Subtype")));
    hash.addData(withoutSubsetTag(QString::fromStdString(nameOf(font, "/BaseFont"))).toUtf8());

    // The numbers go in as the file's own text, which is exact and, unlike
    // anything that parses them, cannot be led astray by the locale.
    for (const char *key : { "/FirstChar", "/LastChar", "/Widths", "/Encoding", "/W", "/DW", "/CIDToGIDMap" }) {
        QPDFObjectHandle value = font.getKey(key);
        if (!value.isNull()) {
            hash.addData(QByteArray::fromStdString(value.unparseResolved()));
        }
    }
    QPDFObjectHandle descriptor = descriptorOf(font);
    if (descriptor.isDictionary()) {
        for (const char *key : { "/Flags", "/ItalicAngle", "/MissingWidth" }) {
            QPDFObjectHandle value = descriptor.getKey(key);
            if (!value.isNull()) {
                hash.addData(QByteArray::fromStdString(value.unparse()));
            }
        }
    }
    hash.addData(bytes);
    return hash.result();
}

/** A `/ToUnicode` CMap for a one-byte font, which is what makes text copyable. */
QByteArray toUnicodeCMap(const QMap<int, QChar> &characterForCode)
{
    // Deliberately the same shape as the one FontEmbedder writes; the two are
    // small enough that a shared helper would only tie two files together.
    QByteArray cmap = QByteArrayLiteral("/CIDInit /ProcSet findresource begin\n"
                                        "12 dict begin\n"
                                        "begincmap\n"
                                        "/CIDSystemInfo << /Registry (Adobe) /Ordering (UCS) /Supplement 0 >> def\n"
                                        "/CMapName /Adobe-Identity-UCS def\n"
                                        "/CMapType 2 def\n"
                                        "1 begincodespacerange\n"
                                        "<00> <FF>\n"
                                        "endcodespacerange\n");

    QVector<QPair<int, QChar>> entries;
    for (auto it = characterForCode.constBegin(); it != characterForCode.constEnd(); ++it) {
        entries.append({ it.key(), it.value() });
    }
    // A hundred mappings per block is the limit the specification sets.
    for (qsizetype from = 0; from < entries.size(); from += 100) {
        const qsizetype until = qMin(from + 100, entries.size());
        cmap += QByteArray::number(until - from) + QByteArrayLiteral(" beginbfchar\n");
        for (qsizetype at = from; at < until; ++at) {
            cmap += '<' + QByteArray::number(entries.at(at).first, 16).toUpper().rightJustified(2, '0')
                + QByteArrayLiteral("> <")
                + QByteArray::number(entries.at(at).second.unicode(), 16).toUpper().rightJustified(4, '0')
                + QByteArrayLiteral(">\n");
        }
        cmap += QByteArrayLiteral("endbfchar\n");
    }

    cmap += QByteArrayLiteral("endcmap\n"
                              "CMapName currentdict /CMap defineresource pop\n"
                              "end\n"
                              "end\n");
    return cmap;
}

void writeOut(QPDF &pdf, const QString &path)
{
    QPDFWriter writer(pdf);
    writer.setOutputFilename(QFile::encodeName(path).constData());
    writer.setObjectStreamMode(qpdf_o_generate);
    writer.setStreamDataMode(qpdf_s_compress);
    writer.write();
}

} // namespace

QVector<FontUse> FontInventory::read(const QString &pdf, QString *error)
{
    QVector<FontUse> uses;
    try {
        QPDF document;
        PdfFile::open(document, pdf);

        QPDFPageDocumentHelper documents(document);
        std::vector<QPDFPageObjectHelper> pages = documents.getAllPages();

        // One entry per font object, however many pages reach it. Only indirect
        // objects can be shared; a dictionary written into a page's resources
        // has no identity to compare, so each occurrence is its own entry.
        std::map<QPDFObjGen, int> known;
        for (int index = 0; index < int(pages.size()); ++index) {
            std::set<QPDFObjGen> visited;
            const FontVisitor note
                = [&uses, &known, index](QPDFObjectHandle fonts, const std::string &key, QPDFObjectHandle font) {
                      Q_UNUSED(fonts)
                      if (font.isIndirect()) {
                          const auto seen = known.find(font.getObjGen());
                          if (seen != known.end()) {
                              if (!uses.at(seen->second).pages.contains(index)) {
                                  uses[seen->second].pages.append(index);
                              }
                              return;
                          }
                          known.insert({ font.getObjGen(), int(uses.size()) });
                      }
                      FontUse use = describeFont(font, QString::fromStdString(key));
                      use.pages.append(index);
                      uses.append(use);
                  };
            eachFont(pages.at(size_t(index)).getAttribute("/Resources", false), visited, 0, note);
            eachAnnotationFont(pages.at(size_t(index)).getObjectHandle(), visited, note);
        }
    } catch (const std::exception &e) {
        if (error) {
            *error = QString::fromUtf8(e.what());
        }
        return {};
    }
    return uses;
}

QHash<QString, QByteArray> FontInventory::embeddedProgrammes(const QString &pdf, int page, QString *error)
{
    QHash<QString, QByteArray> programmes;
    try {
        QPDF document;
        PdfFile::open(document, pdf);

        QPDFPageDocumentHelper documents(document);
        std::vector<QPDFPageObjectHelper> pages = documents.getAllPages();
        if (page < 0 || page >= int(pages.size())) {
            if (error) {
                *error = i18n("The document has no page %1.", page + 1);
            }
            return programmes;
        }

        // Through the form XObjects as well, because a page's own `/Fx` names
        // are not the only ones its content stream can use.
        std::set<QPDFObjGen> visited;
        eachFont(pages.at(size_t(page)).getAttribute("/Resources", false), visited, 0,
                 [&programmes](QPDFObjectHandle fonts, const std::string &key, QPDFObjectHandle font) {
                     Q_UNUSED(fonts)
                     const QString name = QString::fromStdString(key);
                     if (programmes.contains(name)) {
                         return;
                     }
                     const QByteArray bytes = streamBytes(programmeOf(font));
                     if (!bytes.isEmpty()) {
                         programmes.insert(name, bytes);
                     }
                 });
    } catch (const std::exception &e) {
        if (error) {
            *error = QString::fromUtf8(e.what());
        }
        return {};
    }
    return programmes;
}

bool FontInventory::removeEmbedding(const QString &in, const QString &out, const QStringList &baseFonts, int *removed,
                                    QString *error)
{
    int count = 0;
    try {
        QPDF pdf;
        PdfFile::open(pdf, in);

        // A descriptor can be shared between the wrapper and the CIDFont beneath
        // it, and between several font dictionaries, so it is stripped once and
        // counted once.
        std::set<QPDFObjGen> stripped;
        eachFontInDocument(pdf, [&](QPDFObjectHandle fonts, const std::string &key, QPDFObjectHandle font) {
            Q_UNUSED(fonts)
            Q_UNUSED(key)
            if (!matchesAny(QString::fromStdString(nameOf(font, "/BaseFont")), baseFonts)) {
                return;
            }
            QPDFObjectHandle descriptor = descriptorOf(font);
            if (!descriptor.isDictionary()) {
                return;
            }
            if (descriptor.isIndirect()) {
                if (stripped.count(descriptor.getObjGen()) > 0) {
                    return;
                }
                stripped.insert(descriptor.getObjGen());
            }
            bool any = false;
            for (const char *file : { "/FontFile", "/FontFile2", "/FontFile3" }) {
                if (descriptor.hasKey(file)) {
                    descriptor.removeKey(file);
                    any = true;
                }
            }
            if (any) {
                ++count;
            }
        });

        if (count == 0) {
            if (error) {
                *error = i18n("No embedded font in this document matches what was asked for.");
            }
            return false;
        }

        // The programme streams are now referred to by nothing, and QPDF writes
        // only what can be reached, which is what actually shrinks the file.
        writeOut(pdf, out);
    } catch (const std::exception &e) {
        if (error) {
            *error = QString::fromUtf8(e.what());
        }
        return false;
    }

    if (removed) {
        *removed = count;
    }
    return true;
}

bool FontInventory::mergeSubsets(const QString &in, const QString &out, int *merged, QString *error)
{
    int count = 0;
    try {
        QPDF pdf;
        PdfFile::open(pdf, in);

        // Where every font is referred to from, so that a reference can be moved
        // once a duplicate is found. Collected before anything is changed.
        struct Reference {
            QPDFObjectHandle fonts;
            std::string key;
            QPDFObjGen font;
        };
        QVector<Reference> references;
        std::map<QPDFObjGen, QPDFObjectHandle> objects;

        QPDFPageDocumentHelper documents(pdf);
        for (QPDFPageObjectHelper &page : documents.getAllPages()) {
            std::set<QPDFObjGen> visited;
            const FontVisitor note
                = [&references, &objects](QPDFObjectHandle fonts, const std::string &key, QPDFObjectHandle font) {
                      if (!font.isIndirect()) {
                          return; // Nothing to share: it lives inside its own resources.
                      }
                      references.append({ fonts, key, font.getObjGen() });
                      objects.insert({ font.getObjGen(), font });
                  };
            eachFont(page.getAttribute("/Resources", false), visited, 0, note);
            eachAnnotationFont(page.getObjectHandle(), visited, note);
        }

        std::map<QByteArray, QPDFObjGen> survivors;
        std::map<QPDFObjGen, QPDFObjGen> replacements;
        for (const auto &[objGen, font] : objects) {
            const QByteArray fingerprint = fingerprintOf(font);
            if (fingerprint.isEmpty()) {
                continue;
            }
            const auto survivor = survivors.find(fingerprint);
            if (survivor == survivors.end()) {
                survivors.insert({ fingerprint, objGen });
            } else if (!(survivor->second == objGen)) {
                replacements.insert({ objGen, survivor->second });
            }
        }

        for (const Reference &reference : std::as_const(references)) {
            const auto replacement = replacements.find(reference.font);
            QPDFObjectHandle fonts = reference.fonts;
            if (replacement == replacements.end() || !fonts.isDictionary()) {
                continue;
            }
            fonts.replaceKey(reference.key, objects.at(replacement->second));
        }
        count = int(replacements.size());

        if (count == 0) {
            if (error) {
                *error = i18n("No two fonts in this document are the same font, so there is nothing to merge.");
            }
            return false;
        }

        writeOut(pdf, out);
    } catch (const std::exception &e) {
        if (error) {
            *error = QString::fromUtf8(e.what());
        }
        return false;
    }

    if (merged) {
        *merged = count;
    }
    return true;
}

bool FontInventory::addToUnicode(const QString &in, const QString &out, int *added, QString *error)
{
    int count = 0;
    try {
        QPDF pdf;
        PdfFile::open(pdf, in);

        eachFontInDocument(pdf, [&](QPDFObjectHandle fonts, const std::string &key, QPDFObjectHandle font) {
            Q_UNUSED(fonts)
            Q_UNUSED(key)
            const std::string subtype = nameOf(font, "/Subtype");
            if (subtype == "/Type0" || subtype == "/CIDFontType0" || subtype == "/CIDFontType2") {
                // A composite font's codes are not characters but CIDs, and what
                // a CID means is a question for the font programme rather than
                // for the encoding. Guessing there would put wrong letters in
                // the clipboard, which is worse than an empty one.
                return;
            }
            if (!parseToUnicode(font.getKey("/ToUnicode")).isEmpty()) {
                return;
            }
            const QHash<int, QChar> mapping = simpleEncoding(font);
            if (mapping.isEmpty()) {
                return;
            }

            QMap<int, QChar> ordered;
            for (auto it = mapping.constBegin(); it != mapping.constEnd(); ++it) {
                if (it.key() >= 0 && it.key() <= 255) {
                    ordered.insert(it.key(), it.value());
                }
            }
            if (ordered.isEmpty()) {
                return;
            }
            const QByteArray cmap = toUnicodeCMap(ordered);
            font.replaceKey("/ToUnicode",
                            QPDFObjectHandle::newStream(&pdf, std::string(cmap.constData(), size_t(cmap.size()))));
            ++count;
        });

        if (count == 0) {
            if (error) {
                *error = i18n("Every font in this document already says what its characters mean, or none of them "
                              "says enough to work it out.");
            }
            return false;
        }

        writeOut(pdf, out);
    } catch (const std::exception &e) {
        if (error) {
            *error = QString::fromUtf8(e.what());
        }
        return false;
    }

    if (added) {
        *added = count;
    }
    return true;
}

// ══ Cutting fonts down to what is used ═════════════════════════════════════

namespace {

/**
 * Which character codes each font resource is asked to draw.
 *
 * Reading the coverage the font *declares* would be easier and is not the same
 * question: a document may address two hundred codes through its encoding and
 * draw eleven of them. The declared coverage is what has to survive an edit;
 * what is drawn is what has to survive a subset. Only the second one makes a
 * two-page letter stop carrying a whole typeface.
 *
 * The graphics-state stack is tracked because `Q` restores the font along with
 * everything else, and a document that sets a font inside `q … Q` and then
 * shows text after it would otherwise have those characters credited to the
 * wrong face, which is the one mistake here that produces a *missing glyph*
 * rather than merely a larger file.
 */
class ShownCharacters : public QPDFObjectHandle::TokenFilter
{
public:
    void handleToken(QPDFTokenizer::Token const &token) override
    {
        const auto type = token.getType();
        if (type == QPDFTokenizer::tt_space || type == QPDFTokenizer::tt_comment) {
            return;
        }

        if (type == QPDFTokenizer::tt_word) {
            handleOperator(QString::fromStdString(token.getValue()));
            m_name.clear();
            m_strings.clear();
            return;
        }
        if (type == QPDFTokenizer::tt_name) {
            m_name = QString::fromStdString(token.getValue());
        } else if (type == QPDFTokenizer::tt_string) {
            const std::string &value = token.getValue();
            m_strings.append(QByteArray(value.data(), qsizetype(value.size())));
        }
    }

    /** Raw bytes shown, per font resource name. */
    const QHash<QString, QByteArray> &shown() const { return m_shown; }

private:
    void handleOperator(const QString &op)
    {
        if (op == u"Tf"_s) {
            if (!m_name.isEmpty()) {
                m_font = m_name;
            }
            return;
        }
        if (op == u"q"_s) {
            m_stack.append(m_font);
            return;
        }
        if (op == u"Q"_s) {
            if (!m_stack.isEmpty()) {
                m_font = m_stack.takeLast();
            }
            return;
        }
        if (op == u"Tj"_s || op == u"TJ"_s || op == u"'"_s || op == u"\""_s) {
            if (m_font.isEmpty()) {
                return;
            }
            for (const QByteArray &text : std::as_const(m_strings)) {
                m_shown[m_font].append(text);
            }
        }
    }

    QString m_name;
    QString m_font;
    QVector<QString> m_stack;
    QVector<QByteArray> m_strings;
    QHash<QString, QByteArray> m_shown;
};

/** Code to character for one simple font, preferring what it says itself. */
QHash<int, QChar> codeMeaningOf(QPDFObjectHandle font)
{
    QPDFObjectHandle toUnicode = font.getKey("/ToUnicode");
    if (toUnicode.isStream()) {
        const QHash<int, QChar> stated = parseToUnicode(toUnicode);
        if (!stated.isEmpty()) {
            return stated;
        }
    }
    return simpleEncoding(font);
}

/**
 * Credits the bytes shown to the embedded programmes that have to draw them.
 *
 * Keyed by the programme rather than by the font dictionary because that is
 * what gets rewritten, and because two font dictionaries sharing one descriptor
 * (which producers do) must contribute their characters to the same subset or
 * the second one loses its glyphs.
 */
void creditShownCharacters(const QHash<QString, QByteArray> &shown, QPDFObjectHandle resources,
                           std::map<QPDFObjGen, QSet<QChar>> &wanted, std::map<QPDFObjGen, QPDFObjectHandle> &fontFor,
                           std::map<QPDFObjGen, QStringList> &namesFor)
{
    if (!resources.isDictionary()) {
        return;
    }
    QPDFObjectHandle fonts = resources.getKey("/Font");
    if (!fonts.isDictionary()) {
        return;
    }

    for (auto it = shown.constBegin(); it != shown.constEnd(); ++it) {
        const std::string key = it.key().toStdString();
        if (!fonts.hasKey(key)) {
            continue;
        }
        QPDFObjectHandle font = fonts.getKey(key);
        QPDFObjectHandle programme = programmeOf(font);
        if (!programme.isStream() || !programme.isIndirect()) {
            continue;
        }

        const QPDFObjGen id = programme.getObjGen();
        fontFor.insert({ id, font });
        QStringList &names = namesFor[id];
        if (!names.contains(it.key())) {
            names.append(it.key());
        }

        // A composite font's bytes are two-byte glyph selectors, not character
        // codes, so decoding them as characters would be nonsense. They are
        // refused further down; nothing is credited here.
        if (nameOf(font, "/Subtype") == "/Type0") {
            wanted.insert({ id, QSet<QChar>() });
            continue;
        }

        const QHash<int, QChar> meaning = codeMeaningOf(font);
        QSet<QChar> &characters = wanted[id];
        for (const char byte : it.value()) {
            const auto found = meaning.constFind(int(quint8(byte)));
            if (found != meaning.constEnd()) {
                characters.insert(found.value());
            }
        }
    }
}

/** Every form XObject reachable from @p resources, however deeply nested. */
void eachFormXObject(QPDFObjectHandle resources, std::set<QPDFObjGen> &visited, int depth,
                     const std::function<void(QPDFObjectHandle)> &visit)
{
    if (!resources.isDictionary() || depth > MaxResourceDepth) {
        return;
    }
    QPDFObjectHandle xobjects = resources.getKey("/XObject");
    if (!xobjects.isDictionary()) {
        return;
    }

    for (const auto &entry : xobjects.getDictAsMap()) {
        QPDFObjectHandle form = entry.second;
        if (!form.isStream() || nameOf(form.getDict(), "/Subtype") != "/Form") {
            continue;
        }
        if (form.isIndirect()) {
            if (visited.count(form.getObjGen()) > 0) {
                continue;
            }
            visited.insert(form.getObjGen());
        }
        visit(form);
        eachFormXObject(form.getDict().getKey("/Resources"), visited, depth + 1, visit);
    }
}

} // namespace

bool FontInventory::subsetEmbedded(const QString &in, const QString &out, QVector<SubsetReduction> *reduced,
                                   QStringList *notes, QString *error)
{
    QVector<SubsetReduction> reductions;
    QStringList said;

    try {
        QPDF pdf;
        PdfFile::open(pdf, in);

        std::map<QPDFObjGen, QSet<QChar>> wanted;
        std::map<QPDFObjGen, QPDFObjectHandle> fontFor;
        std::map<QPDFObjGen, QStringList> namesFor;

        QPDFPageDocumentHelper documents(pdf);
        std::vector<QPDFPageObjectHelper> pages = documents.getAllPages();
        for (QPDFPageObjectHelper &page : pages) {
            QPDFObjectHandle resources = page.getAttribute("/Resources", false);

            ShownCharacters filter;
            page.filterContents(&filter, nullptr);
            creditShownCharacters(filter.shown(), resources, wanted, fontFor, namesFor);

            // A form XObject and an annotation's appearance draw text with
            // their own resources, and a signature block or a stamp is very
            // often the only place a particular face is used at all.
            std::set<QPDFObjGen> visited;
            eachFormXObject(resources, visited, 0, [&](QPDFObjectHandle form) {
                ShownCharacters inner;
                form.filterAsContents(&inner, nullptr);
                QPDFObjectHandle own = form.getDict().getKey("/Resources");
                creditShownCharacters(inner.shown(), own.isDictionary() ? own : resources, wanted, fontFor, namesFor);
            });

            QPDFObjectHandle annotations = page.getObjectHandle().getKey("/Annots");
            if (!annotations.isArray()) {
                continue;
            }
            for (int i = 0; i < annotations.getArrayNItems(); ++i) {
                QPDFObjectHandle appearance = annotations.getArrayItem(i).getKey("/AP");
                if (!appearance.isDictionary()) {
                    continue;
                }
                QPDFObjectHandle normal = appearance.getKey("/N");
                QVector<QPDFObjectHandle> streams;
                if (normal.isStream()) {
                    streams.append(normal);
                } else if (normal.isDictionary()) {
                    // A tick box keeps one appearance per state.
                    for (const auto &state : normal.getDictAsMap()) {
                        if (state.second.isStream()) {
                            streams.append(state.second);
                        }
                    }
                }
                for (QPDFObjectHandle stream : std::as_const(streams)) {
                    ShownCharacters inner;
                    stream.filterAsContents(&inner, nullptr);
                    QPDFObjectHandle own = stream.getDict().getKey("/Resources");
                    creditShownCharacters(inner.shown(), own.isDictionary() ? own : resources, wanted, fontFor,
                                          namesFor);
                }
            }
        }

        // Every embedded programme the document has, so that one nobody draws
        // with can be said out loud rather than silently passed over.
        std::set<QPDFObjGen> embedded;
        eachFontInDocument(pdf, [&](QPDFObjectHandle fonts, const std::string &key, QPDFObjectHandle font) {
            Q_UNUSED(fonts)
            QPDFObjectHandle programme = programmeOf(font);
            if (programme.isStream() && programme.isIndirect()) {
                embedded.insert(programme.getObjGen());
                if (fontFor.find(programme.getObjGen()) == fontFor.end()) {
                    fontFor.insert({ programme.getObjGen(), font });
                    namesFor[programme.getObjGen()].append(QString::fromStdString(key));
                }
            }
        });

        for (const QPDFObjGen &id : embedded) {
            QPDFObjectHandle font = fontFor.at(id);
            const QString baseFont = QString::fromStdString(nameOf(font, "/BaseFont"));
            const QString shown = baseFont.isEmpty() ? i18nc("@item a font with no name of its own", "an unnamed font")
                                                     : withoutSubsetTag(baseFont);

            QPDFObjectHandle descriptor = descriptorOf(font);
            QPDFObjectHandle programme
                = descriptor.isDictionary() ? descriptor.getKey("/FontFile2") : QPDFObjectHandle::newNull();
            if (!programme.isStream()) {
                said += i18n("“%1” is not an sfnt programme: a Type 1 or compact outline cannot be cut down from "
                             "the outside, so it was left exactly as it was.",
                             shown);
                continue;
            }

            if (nameOf(font, "/Subtype") == "/Type0") {
                said += i18n("“%1” is a composite font, whose codes are glyph numbers meaningful only inside the "
                             "font programme itself. Working out which of them the pages use would mean reading "
                             "the programme this is supposed to be cutting down, so it was left alone.",
                             shown);
                continue;
            }

            const auto found = wanted.find(id);
            if (found == wanted.end()) {
                said += i18n("Nothing on any page draws with “%1”, so there was no set of glyphs to keep and it was "
                             "left alone. Removing it outright is a different decision, and not this one.",
                             shown);
                continue;
            }
            if (found->second.isEmpty()) {
                said += i18n("Nothing in this document says what “%1”'s character codes mean, so which glyphs the "
                             "pages need could not be worked out. It was left alone.",
                             shown);
                continue;
            }

            const QByteArray before = streamBytes(programme);
            if (before.isEmpty()) {
                said += i18n("“%1”'s programme is stored with a filter this cannot undo, so it could not be read, "
                             "let alone rewritten.",
                             shown);
                continue;
            }

            int glyphsBefore = 0;
            int glyphsAfter = 0;
            QString why;
            const QByteArray after
                = FontEmbedder::subsetProgramme(before, found->second, &glyphsBefore, &glyphsAfter, &why);
            if (after.isEmpty()) {
                said += i18nc("@info a font that could not be cut down, and the reason", "“%1” was left alone: %2",
                              shown, why.isEmpty() ? i18n("its glyphs could not be worked out") : why);
                continue;
            }
            if (after.size() >= before.size()) {
                said += i18n("“%1” is already no larger than the glyphs the pages use, so nothing was gained by "
                             "rewriting it and it was left as it was.",
                             shown);
                continue;
            }

            programme.replaceStreamData(std::string(after.constData(), size_t(after.size())),
                                        QPDFObjectHandle::newNull(), QPDFObjectHandle::newNull());
            programme.replaceKey("/Length1", QPDFObjectHandle::newInteger(after.size()));

            SubsetReduction reduction;
            reduction.baseFont = baseFont;
            reduction.resourceNames = namesFor[id];
            reduction.glyphsBefore = glyphsBefore;
            reduction.glyphsAfter = glyphsAfter;
            reduction.bytesBefore = before.size();
            reduction.bytesAfter = after.size();
            reductions.append(reduction);
        }

        writeOut(pdf, out);
    } catch (const std::exception &e) {
        if (error) {
            *error = QString::fromUtf8(e.what());
        }
        return false;
    }

    if (reduced) {
        *reduced = reductions;
    }
    if (notes) {
        *notes = said;
    }
    return true;
}

QStringList FontInventory::limitations()
{
    return {
        i18n("What a font can show is read from what the document says about it, which for a subset is only the "
             "characters that document used. The font on disc it came from may well hold more."),
        i18n("A font whose /ToUnicode map is present but unreadable is reported as having none, because that is "
             "what it is worth to anyone trying to copy the text."),
        i18n("Removing an embedded font leaves the text in place but not its appearance: the reader substitutes "
             "whatever it has, and lines shift where the substitute is not the same width."),
        i18n("Only subsets that are the same font programme with the same encoding are merged. Subsets holding "
             "different glyphs are left alone, because combining those means rewriting every page that uses them."),
        i18n("A /ToUnicode map can be written for an ordinary font but not for a composite one, whose codes are "
             "glyph numbers with no stated meaning."),
        i18n("Fonts used only by a document's interactive form, and not by any page, are not listed."),
    };
}

} // namespace ps
