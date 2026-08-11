/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#include "FontEmbedder.h"

#include "Core14Widths.h"
#include "Encodings.h"
#include "GlyphNames.h"
#include "PdfFile.h"
#include "PdfGeometry.h"

#include <KLocalizedString>

#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QMap>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QVector>

#include <qpdf/QPDF.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>
#include <qpdf/QPDFWriter.hh>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <set>

using namespace Qt::Literals::StringLiterals;

namespace ps {

namespace {

using PdfGeometry::number;

/**
 * A font file is a file, and a file may say anything.
 *
 * Every read goes through these, and every one of them answers zero rather than
 * walking off the end. A subsetter that trusts its input crashes on the first
 * truncated font in somebody's cache, and reading past the end of a table is the
 * easiest mistake to make here by a wide margin.
 */
quint8 byteAt(const QByteArray &data, qsizetype at)
{
    return (at >= 0 && at < data.size()) ? quint8(data.at(at)) : quint8(0);
}

quint16 u16At(const QByteArray &data, qsizetype at)
{
    return quint16((quint16(byteAt(data, at)) << 8) | byteAt(data, at + 1));
}

qint16 s16At(const QByteArray &data, qsizetype at)
{
    return qint16(u16At(data, at));
}

quint32 u32At(const QByteArray &data, qsizetype at)
{
    return (quint32(u16At(data, at)) << 16) | u16At(data, at + 2);
}

void appendU16(QByteArray &data, quint16 value)
{
    data.append(char((value >> 8) & 0xFF));
    data.append(char(value & 0xFF));
}

void appendU32(QByteArray &data, quint32 value)
{
    appendU16(data, quint16((value >> 16) & 0xFFFF));
    appendU16(data, quint16(value & 0xFFFF));
}

void put16(QByteArray &data, qsizetype at, quint16 value)
{
    if (at < 0 || at + 1 >= data.size()) {
        return;
    }
    data[at] = char((value >> 8) & 0xFF);
    data[at + 1] = char(value & 0xFF);
}

void put32(QByteArray &data, qsizetype at, quint32 value)
{
    put16(data, at, quint16((value >> 16) & 0xFFFF));
    put16(data, at + 2, quint16(value & 0xFFFF));
}

/** Composite glyph flags, from the TrueType outline format. */
enum ComponentFlag : quint16 {
    ArgsAreWords = 0x0001,
    HaveScale = 0x0008,
    MoreComponents = 0x0020,
    HaveXAndYScale = 0x0040,
    HaveTwoByTwo = 0x0080,
};

/**
 * A font file, parsed as far as taking a piece out of it requires.
 *
 * Only the tables that a subset has to understand are read; the rest are either
 * copied over untouched or left behind. Everything is kept in the font's own
 * units, and scaled to the PDF's thousandths only at the point of writing, so
 * that rounding happens once.
 */
struct Face {
    QByteArray data;
    QMap<QByteArray, QPair<quint32, quint32>> tables;

    /** True when the outlines are compact-format rather than TrueType. */
    bool compactOutlines = false;

    int numGlyphs = 0;
    int unitsPerEm = 1000;
    QVector<quint32> loca;
    QVector<quint16> advances;
    QVector<qint16> bearings;

    /** Unicode to glyph, from the widest cmap subtable the font offers. */
    QHash<uint, int> glyphForCharacter;

    /**
     * Glyph name to glyph, from a `post` table that names its glyphs.
     *
     * The second way in, and the one that saves the day on real documents: a
     * subsetter is free to throw the `cmap` away, since a PDF says what its own
     * codes mean, and plenty do. A font with no `cmap` and named glyphs can
     * still be cut down; a font with neither cannot, and says so.
     */
    QHash<QByteArray, int> glyphForName;

    qint16 xMin = 0;
    qint16 yMin = 0;
    qint16 xMax = 0;
    qint16 yMax = 0;
    qint16 ascender = 0;
    qint16 descender = 0;
    qint16 capHeight = 0;
    quint16 weightClass = 400;
    quint16 fsType = 0;
    double italicAngle = 0.0;
    bool fixedPitch = false;
    bool serif = false;
};

QByteArray tableBytes(const Face &face, const char *tag)
{
    const auto found = face.tables.constFind(QByteArray(tag));
    if (found == face.tables.constEnd()) {
        return {};
    }
    const qsizetype offset = qsizetype(found->first);
    const qsizetype length = qsizetype(found->second);
    if (offset < 0 || offset >= face.data.size()) {
        return {};
    }
    return face.data.mid(offset, qMin(length, face.data.size() - offset));
}

/** One cmap subtable, decoded into the map. Formats 0, 4, 6 and 12 only. */
void readCmapSubtable(const QByteArray &subtable, bool symbol, QHash<uint, int> &into)
{
    const quint16 format = u16At(subtable, 0);
    if (format == 0) {
        for (uint code = 0; code < 256; ++code) {
            const int glyph = byteAt(subtable, 6 + qsizetype(code));
            if (glyph > 0) {
                into.insert(code, glyph);
            }
        }
        return;
    }
    if (format == 4) {
        const int segments = u16At(subtable, 6) / 2;
        const qsizetype ends = 14;
        const qsizetype starts = ends + qsizetype(segments) * 2 + 2;
        const qsizetype deltas = starts + qsizetype(segments) * 2;
        const qsizetype ranges = deltas + qsizetype(segments) * 2;
        for (int segment = 0; segment < segments; ++segment) {
            const uint last = u16At(subtable, ends + segment * 2);
            const uint first = u16At(subtable, starts + segment * 2);
            const qint16 delta = s16At(subtable, deltas + segment * 2);
            const quint16 rangeOffset = u16At(subtable, ranges + segment * 2);
            if (first > last) {
                continue;
            }
            // 0xFFFF is the sentinel every format 4 table ends with and stands
            // for nothing, but the segment holding it may map real codes below it.
            for (uint code = first; code <= last && code < 0xFFFF; ++code) {
                int glyph = 0;
                if (rangeOffset == 0) {
                    glyph = int((code + uint(quint16(delta))) & 0xFFFF);
                } else {
                    const qsizetype at = ranges + segment * 2 + rangeOffset + qsizetype(code - first) * 2;
                    const quint16 raw = u16At(subtable, at);
                    glyph = raw == 0 ? 0 : int((raw + quint16(delta)) & 0xFFFF);
                }
                if (glyph > 0) {
                    into.insert(symbol ? (code & 0xFF) : code, glyph);
                    if (symbol) {
                        into.insert(code, glyph);
                    }
                }
            }
        }
        return;
    }
    if (format == 6) {
        const uint first = u16At(subtable, 6);
        const int count = u16At(subtable, 8);
        for (int i = 0; i < count; ++i) {
            const int glyph = u16At(subtable, 10 + qsizetype(i) * 2);
            if (glyph > 0) {
                into.insert(first + uint(i), glyph);
            }
        }
        return;
    }
    if (format == 12) {
        const quint32 groups = u32At(subtable, 12);
        // A malformed count would otherwise ask for a map of four billion entries.
        const quint32 sane = qMin(groups, quint32(0x10000));
        for (quint32 group = 0; group < sane; ++group) {
            const qsizetype at = 16 + qsizetype(group) * 12;
            const quint32 first = u32At(subtable, at);
            const quint32 last = u32At(subtable, at + 4);
            const quint32 glyph = u32At(subtable, at + 8);
            if (last < first || last - first > 0x10000) {
                continue;
            }
            for (quint32 code = first; code <= last; ++code) {
                // Only the basic multilingual plane, because a QChar holds no more.
                if (code <= 0xFFFF) {
                    into.insert(code, int(glyph + (code - first)));
                }
            }
        }
    }
}

/**
 * The names a `post` table gives its glyphs, where it gives them at all.
 *
 * Only the custom half of format 2.0 is read: an index below 258 refers to the
 * standard Macintosh ordering, a list this does not carry, and a wrong name
 * there would put a wrong glyph in a subset. Missing one is merely a font that
 * cannot be reduced, which is said out loud.
 */
void readPostNames(Face &face)
{
    const QByteArray post = tableBytes(face, "post");
    if (post.size() < 34 || u32At(post, 0) != 0x00020000u) {
        return;
    }
    const int count = qMin(int(u16At(post, 32)), face.numGlyphs);

    QVector<QByteArray> names;
    qsizetype at = 34 + qsizetype(count) * 2;
    while (at < post.size()) {
        const qsizetype length = qsizetype(byteAt(post, at));
        names.append(post.mid(at + 1, length));
        at += length + 1;
    }

    for (int glyph = 0; glyph < count; ++glyph) {
        const int index = int(u16At(post, 34 + qsizetype(glyph) * 2)) - 258;
        if (index >= 0 && index < names.size() && !names.at(index).isEmpty()) {
            face.glyphForName.insert(names.at(index), glyph);
        }
    }
}

void readCmap(Face &face)
{
    const QByteArray cmap = tableBytes(face, "cmap");
    if (cmap.isEmpty()) {
        return;
    }
    const int count = u16At(cmap, 2);

    // A font that offers several subtables offers the same glyphs through
    // different doors, and only the widest one is worth walking through: a
    // full-Unicode table beats a sixteen-bit one, and either beats the symbol
    // and Mac tables, which say least about what a character means.
    int bestScore = -1;
    qsizetype bestOffset = -1;
    bool bestIsSymbol = false;
    for (int i = 0; i < count; ++i) {
        const qsizetype at = 4 + qsizetype(i) * 8;
        const quint16 platform = u16At(cmap, at);
        const quint16 encoding = u16At(cmap, at + 2);
        const qsizetype offset = qsizetype(u32At(cmap, at + 4));

        int score = -1;
        bool symbol = false;
        if (platform == 3 && encoding == 10) {
            score = 5;
        } else if (platform == 0 && encoding >= 4) {
            score = 5;
        } else if (platform == 3 && encoding == 1) {
            score = 4;
        } else if (platform == 0) {
            score = 4;
        } else if (platform == 3 && encoding == 0) {
            score = 2;
            symbol = true;
        } else if (platform == 1 && encoding == 0) {
            score = 1;
        }
        if (score > bestScore) {
            bestScore = score;
            bestOffset = offset;
            bestIsSymbol = symbol;
        }
    }
    if (bestOffset < 0 || bestOffset >= cmap.size()) {
        return;
    }
    readCmapSubtable(cmap.mid(bestOffset), bestIsSymbol, face.glyphForCharacter);
}

void readMetrics(Face &face)
{
    const QByteArray hmtx = tableBytes(face, "hmtx");
    const QByteArray hhea = tableBytes(face, "hhea");
    const int entries = qMax(1, int(u16At(hhea, 34)));

    face.advances = QVector<quint16>(face.numGlyphs, 0);
    face.bearings = QVector<qint16>(face.numGlyphs, 0);
    quint16 advance = 0;
    for (int glyph = 0; glyph < face.numGlyphs; ++glyph) {
        if (glyph < entries) {
            advance = u16At(hmtx, qsizetype(glyph) * 4);
            face.bearings[glyph] = s16At(hmtx, qsizetype(glyph) * 4 + 2);
        } else {
            // Past the last full entry a monospaced tail is implied: one advance
            // for all of them, and side bearings only.
            face.bearings[glyph] = s16At(hmtx, qsizetype(entries) * 4 + qsizetype(glyph - entries) * 2);
        }
        face.advances[glyph] = advance;
    }
}

bool openFace(const QByteArray &bytes, Face *face, QString *error)
{
    face->data = bytes;

    qsizetype base = 0;
    if (bytes.size() >= 16 && bytes.left(4) == QByteArrayLiteral("ttcf")) {
        // A collection holds several faces sharing tables. Fontconfig names one
        // by index; without that index the first face is the only defensible
        // choice, and it is the one index 0 refers to.
        base = qsizetype(u32At(bytes, 12));
    }

    const quint32 flavour = u32At(bytes, base);
    const bool trueTypeOutlines = flavour == 0x00010000u || flavour == 0x74727565u;
    const bool compactOutlines = flavour == 0x4F54544Fu;
    if (!trueTypeOutlines && !compactOutlines) {
        if (error) {
            *error = i18n("This is not a TrueType or OpenType font file.");
        }
        return false;
    }

    const int count = u16At(bytes, base + 4);
    for (int i = 0; i < count; ++i) {
        const qsizetype at = base + 12 + qsizetype(i) * 16;
        const QByteArray tag = bytes.mid(at, 4);
        if (tag.size() != 4) {
            break;
        }
        face->tables.insert(tag, { u32At(bytes, at + 8), u32At(bytes, at + 12) });
    }
    face->compactOutlines = compactOutlines || face->tables.contains(QByteArrayLiteral("CFF "));

    const QByteArray head = tableBytes(*face, "head");
    const QByteArray maxp = tableBytes(*face, "maxp");
    if (head.size() < 54 || maxp.size() < 6) {
        if (error) {
            *error = i18n("This font file is missing the tables that describe it.");
        }
        return false;
    }
    face->unitsPerEm = qMax(16, int(u16At(head, 18)));
    face->xMin = s16At(head, 36);
    face->yMin = s16At(head, 38);
    face->xMax = s16At(head, 40);
    face->yMax = s16At(head, 42);
    face->numGlyphs = u16At(maxp, 4);
    if (face->numGlyphs <= 0) {
        if (error) {
            *error = i18n("This font file contains no glyphs.");
        }
        return false;
    }

    const QByteArray hhea = tableBytes(*face, "hhea");
    face->ascender = s16At(hhea, 4);
    face->descender = s16At(hhea, 6);

    const QByteArray os2 = tableBytes(*face, "OS/2");
    if (os2.size() >= 10) {
        face->weightClass = u16At(os2, 4);
        face->fsType = u16At(os2, 8);
    }
    if (os2.size() >= 42 && byteAt(os2, 32) == 2) {
        // Panose says what kind of type this is; serif styles are 2 to 10.
        const quint8 serifStyle = byteAt(os2, 33);
        face->serif = serifStyle >= 2 && serifStyle <= 10;
    }
    if (u16At(os2, 0) >= 2 && os2.size() >= 90) {
        face->capHeight = s16At(os2, 88);
    }

    const QByteArray post = tableBytes(*face, "post");
    if (post.size() >= 16) {
        // A 16.16 fixed-point number, read by hand: the fractional half of an
        // italic angle is worth keeping and a plain division is locale-proof.
        face->italicAngle = double(qint32(u32At(post, 4))) / 65536.0;
        face->fixedPitch = u32At(post, 12) != 0;
    }

    if (!face->compactOutlines) {
        const QByteArray loca = tableBytes(*face, "loca");
        if (loca.isEmpty() || !face->tables.contains(QByteArrayLiteral("glyf"))) {
            if (error) {
                *error = i18n("This font file has no outlines that can be copied.");
            }
            return false;
        }
        const bool longFormat = s16At(head, 50) != 0;
        face->loca.reserve(face->numGlyphs + 1);
        for (int glyph = 0; glyph <= face->numGlyphs; ++glyph) {
            face->loca.append(longFormat ? u32At(loca, qsizetype(glyph) * 4)
                                         : quint32(u16At(loca, qsizetype(glyph) * 2)) * 2);
        }
    }

    readMetrics(*face);
    readCmap(*face);
    readPostNames(*face);
    return true;
}

QByteArray glyphOutline(const Face &face, int glyph)
{
    if (glyph < 0 || glyph + 1 >= face.loca.size()) {
        return {};
    }
    const quint32 from = face.loca.at(glyph);
    const quint32 to = face.loca.at(glyph + 1);
    if (to <= from) {
        return {}; // An empty glyph, which is what a space is.
    }
    const auto glyf = face.tables.constFind(QByteArrayLiteral("glyf"));
    if (glyf == face.tables.constEnd()) {
        return {};
    }
    const qsizetype at = qsizetype(glyf->first) + qsizetype(from);
    const qsizetype length = qsizetype(to - from);
    if (at < 0 || at >= face.data.size()) {
        return {};
    }
    return face.data.mid(at, qMin(length, face.data.size() - at));
}

/**
 * Visits every component reference of a composite glyph.
 *
 * @p visit is handed the byte position of each component's glyph index, which is
 * what both callers need: one collects the glyphs referred to, the other
 * renumbers them once their new positions are known. Walking the record twice
 * from two separate parsers is how the two would eventually disagree.
 */
void eachComponent(const QByteArray &outline, const std::function<void(qsizetype)> &visit)
{
    if (outline.size() < 12 || s16At(outline, 0) >= 0) {
        return; // A positive contour count means the glyph has its own outline.
    }
    qsizetype at = 10;
    for (int guard = 0; guard < 512 && at + 4 <= outline.size(); ++guard) {
        const quint16 flags = u16At(outline, at);
        visit(at + 2);
        at += 4;
        at += (flags & ArgsAreWords) ? 4 : 2;
        if (flags & HaveScale) {
            at += 2;
        } else if (flags & HaveXAndYScale) {
            at += 4;
        } else if (flags & HaveTwoByTwo) {
            at += 8;
        }
        if (!(flags & MoreComponents)) {
            return;
        }
    }
}

/**
 * Adds every glyph the wanted ones are built out of.
 *
 * This is the whole difficulty of subsetting in one function. An "ä" is a
 * reference to an "a" and a reference to a diaeresis; copy it without them and
 * the letter comes out blank in a file that validates perfectly. Composites may
 * reference composites, so it iterates to a fixed point rather than descending
 * once.
 */
void closeOverComponents(const Face &face, QSet<int> &glyphs)
{
    QVector<int> pending(glyphs.constBegin(), glyphs.constEnd());
    int guard = 0;
    while (!pending.isEmpty() && ++guard < 100000) {
        const int glyph = pending.takeLast();
        const QByteArray outline = glyphOutline(face, glyph);
        eachComponent(outline, [&](qsizetype at) {
            const int component = int(u16At(outline, at));
            if (component >= 0 && component < face.numGlyphs && !glyphs.contains(component)) {
                glyphs.insert(component);
                pending.append(component);
            }
        });
    }
}

quint32 tableChecksum(const QByteArray &table)
{
    quint32 sum = 0;
    // u32At() reads zeroes past the end, which is exactly the padding the
    // checksum is defined over.
    for (qsizetype at = 0; at < table.size(); at += 4) {
        sum += u32At(table, at);
    }
    return sum;
}

/** Assembles tables into a font file, directory, padding and checksums included. */
QByteArray buildSfnt(const QMap<QByteArray, QByteArray> &tables)
{
    const int count = tables.size();
    if (count == 0) {
        return {};
    }
    int entrySelector = 0;
    while ((1 << (entrySelector + 1)) <= count) {
        ++entrySelector;
    }
    const int searchRange = (1 << entrySelector) * 16;

    QByteArray directory;
    QByteArray body;
    qsizetype headAt = -1;
    const qsizetype bodyStart = 12 + qsizetype(count) * 16;
    for (auto it = tables.constBegin(); it != tables.constEnd(); ++it) {
        if (it.key() == QByteArrayLiteral("head")) {
            headAt = bodyStart + body.size();
        }
        directory.append(it.key());
        appendU32(directory, tableChecksum(it.value()));
        appendU32(directory, quint32(bodyStart + body.size()));
        appendU32(directory, quint32(it.value().size()));
        body.append(it.value());
        while (body.size() % 4 != 0) {
            body.append('\0');
        }
    }

    QByteArray out;
    appendU32(out, 0x00010000u);
    appendU16(out, quint16(count));
    appendU16(out, quint16(searchRange));
    appendU16(out, quint16(entrySelector));
    appendU16(out, quint16(count * 16 - searchRange));
    out.append(directory);
    out.append(body);

    // The checksum of the whole file lives inside the head table, so it can
    // only be worked out once the file exists. Its own field must read zero
    // while the sum is taken, which is why head was written with it cleared.
    if (headAt >= 0) {
        put32(out, headAt + 8, 0xB1B0AFBAu - tableChecksum(out));
    }
    return out;
}

/** A cmap with one Windows Unicode subtable, in the segmented format. */
QByteArray buildCmap(const QMap<uint, int> &glyphForCharacter)
{
    struct Segment {
        quint16 first = 0;
        quint16 last = 0;
        QVector<quint16> glyphs;
    };
    QVector<Segment> segments;
    for (auto it = glyphForCharacter.constBegin(); it != glyphForCharacter.constEnd(); ++it) {
        if (it.key() > 0xFFFF) {
            continue;
        }
        const quint16 code = quint16(it.key());
        if (!segments.isEmpty() && code == segments.last().last + 1) {
            segments.last().last = code;
            segments.last().glyphs.append(quint16(it.value()));
        } else {
            segments.append({ code, code, { quint16(it.value()) } });
        }
    }

    const int count = segments.size() + 1; // Plus the mandatory 0xFFFF segment.
    QByteArray subtable;
    appendU16(subtable, 4);
    appendU16(subtable, 0); // Length, filled in below.
    appendU16(subtable, 0);
    appendU16(subtable, quint16(count * 2));
    int entrySelector = 0;
    while ((1 << (entrySelector + 1)) <= count) {
        ++entrySelector;
    }
    appendU16(subtable, quint16((1 << entrySelector) * 2));
    appendU16(subtable, quint16(entrySelector));
    appendU16(subtable, quint16(count * 2 - (1 << entrySelector) * 2));

    for (const Segment &segment : std::as_const(segments)) {
        appendU16(subtable, segment.last);
    }
    appendU16(subtable, 0xFFFF);
    appendU16(subtable, 0); // The reserved pad, which real parsers do expect.
    for (const Segment &segment : std::as_const(segments)) {
        appendU16(subtable, segment.first);
    }
    appendU16(subtable, 0xFFFF);
    for (int i = 0; i < segments.size(); ++i) {
        appendU16(subtable, 0); // Every real segment goes through the glyph array.
    }
    appendU16(subtable, 1); // 0xFFFF + 1 lands on the missing glyph, as required.

    // The offset is counted from the entry that holds it, so it has to know both
    // how many entries follow and how far into the glyph array its run begins.
    int atGlyph = 0;
    for (int i = 0; i < segments.size(); ++i) {
        appendU16(subtable, quint16((count - i + atGlyph) * 2));
        atGlyph += segments.at(i).glyphs.size();
    }
    appendU16(subtable, 0);
    for (const Segment &segment : std::as_const(segments)) {
        for (quint16 glyph : segment.glyphs) {
            appendU16(subtable, glyph);
        }
    }
    put16(subtable, 2, quint16(subtable.size()));

    QByteArray cmap;
    appendU16(cmap, 0);
    appendU16(cmap, 1);
    appendU16(cmap, 3); // Windows
    appendU16(cmap, 1); // Unicode, sixteen bit
    appendU32(cmap, 12);
    cmap.append(subtable);
    return cmap;
}

/** Glyph names in the file, so a reader can resolve a name without guessing. */
QByteArray buildPost(const QVector<QByteArray> &names, const Face &face)
{
    QByteArray post;
    appendU32(post, 0x00020000u);
    appendU32(post, quint32(qint32(std::lround(face.italicAngle * 65536.0))));
    appendU16(post, quint16(-100)); // Underline position, a plausible default.
    appendU16(post, 50);            // Underline thickness, likewise.
    appendU32(post, face.fixedPitch ? 1 : 0);
    for (int i = 0; i < 4; ++i) {
        appendU32(post, 0); // The memory hints, which nothing has needed since Type 42.
    }
    appendU16(post, quint16(names.size()));
    for (int i = 0; i < names.size(); ++i) {
        // Every name is written out rather than referred to the standard
        // Macintosh order, which is shorter but only for glyphs that happen to
        // be in it.
        appendU16(post, quint16(258 + i));
    }
    for (const QByteArray &name : names) {
        const QByteArray trimmed = name.left(63);
        post.append(char(trimmed.size()));
        post.append(trimmed);
    }
    return post;
}

struct Subset {
    QByteArray program;

    /** New glyph id to the one it had in the original. */
    QVector<int> glyphs;
};

/**
 * Rewrites @p face as a font holding only the glyphs asked for.
 *
 * The glyphs are renumbered rather than left in place: keeping the original ids
 * would mean carrying an index and a metrics entry for every glyph the font ever
 * had, which for a font like DejaVu Sans is forty kilobytes of nothing in a file
 * that is supposed to be small. Renumbering costs one pass over the composite
 * references, which have to be visited anyway.
 */
bool buildSubset(const Face &face, const QMap<uint, int> &glyphForCharacter,
                 const QHash<int, QByteArray> &nameForGlyph, Subset *out, QString *error)
{
    QByteArray head = tableBytes(face, "head");
    QByteArray hhea = tableBytes(face, "hhea");
    QByteArray maxp = tableBytes(face, "maxp");
    if (head.size() < 54 || hhea.size() < 36 || maxp.size() < 6) {
        if (error) {
            *error = i18n("This font file is missing the tables that describe it.");
        }
        return false;
    }

    QSet<int> kept;
    kept.insert(0); // .notdef, which a font must have and a viewer may ask for.
    for (auto it = glyphForCharacter.constBegin(); it != glyphForCharacter.constEnd(); ++it) {
        kept.insert(it.value());
    }
    closeOverComponents(face, kept);

    QVector<int> glyphs(kept.constBegin(), kept.constEnd());
    std::sort(glyphs.begin(), glyphs.end());
    QHash<int, int> renumbered;
    for (int fresh = 0; fresh < glyphs.size(); ++fresh) {
        renumbered.insert(glyphs.at(fresh), fresh);
    }

    QByteArray glyf;
    QVector<quint32> offsets;
    QByteArray hmtx;
    QVector<QByteArray> names;
    for (int old : std::as_const(glyphs)) {
        offsets.append(quint32(glyf.size()));
        QByteArray outline = glyphOutline(face, old);
        eachComponent(outline, [&outline, &renumbered](qsizetype at) {
            put16(outline, at, quint16(renumbered.value(int(u16At(outline, at)), 0)));
        });
        glyf.append(outline);
        while (glyf.size() % 4 != 0) {
            glyf.append('\0');
        }

        appendU16(hmtx, face.advances.value(old, 0));
        appendU16(hmtx, quint16(face.bearings.value(old, 0)));

        names.append(old == 0 ? QByteArrayLiteral(".notdef")
                              : nameForGlyph.value(old, QByteArrayLiteral("g") + QByteArray::number(old)));
    }
    offsets.append(quint32(glyf.size()));

    QByteArray loca;
    for (quint32 offset : std::as_const(offsets)) {
        appendU32(loca, offset);
    }

    head.truncate(54);
    put32(head, 8, 0);   // The file checksum, which buildSfnt() fills in.
    put16(head, 50, 1);  // Long offsets, so a large glyf never needs a second format.

    hhea.truncate(36);
    put16(hhea, 34, quint16(glyphs.size()));

    put16(maxp, 4, quint16(glyphs.size()));

    QMap<QByteArray, QByteArray> tables;
    tables.insert(QByteArrayLiteral("head"), head);
    tables.insert(QByteArrayLiteral("hhea"), hhea);
    tables.insert(QByteArrayLiteral("maxp"), maxp);
    tables.insert(QByteArrayLiteral("hmtx"), hmtx);
    tables.insert(QByteArrayLiteral("loca"), loca);
    tables.insert(QByteArrayLiteral("glyf"), glyf);
    tables.insert(QByteArrayLiteral("cmap"), buildCmap([&] {
                      QMap<uint, int> mapped;
                      for (auto it = glyphForCharacter.constBegin(); it != glyphForCharacter.constEnd(); ++it) {
                          mapped.insert(it.key(), renumbered.value(it.value(), 0));
                      }
                      return mapped;
                  }()));
    tables.insert(QByteArrayLiteral("post"), buildPost(names, face));

    // The hinting programmes say nothing about which glyphs exist, so they come
    // over untouched and small text keeps its shape.
    for (const char *tag : { "cvt ", "fpgm", "prep", "gasp", "OS/2" }) {
        const QByteArray copied = tableBytes(face, tag);
        if (!copied.isEmpty()) {
            tables.insert(QByteArray(tag), copied);
        }
    }

    out->program = buildSfnt(tables);
    out->glyphs = glyphs;
    return !out->program.isEmpty();
}

// ── Finding a font on this system ─────────────────────────────────────────

struct Located {
    QString path;
    QString family;
    QString style;
};

QString normalisedFamily(const QString &family)
{
    return family.toLower().remove(u' ').remove(u'-').remove(u'_');
}

/**
 * Whether the font Fontconfig offered is the one that was asked for.
 *
 * fc-match always answers. Ask it for a family nobody has installed and it hands
 * back whatever it would substitute, so without this check every request would
 * appear to succeed and quietly embed the wrong typeface.
 */
bool familyMatches(const QString &wanted, const QString &offered)
{
    static const QStringList aliases = { u"sans"_s,      u"sansserif"_s, u"serif"_s,  u"monospace"_s,
                                         u"mono"_s,      u"cursive"_s,   u"fantasy"_s, u"system"_s,
                                         u"systemui"_s };
    const QString target = normalisedFamily(wanted);
    if (target.isEmpty() || aliases.contains(target)) {
        return true; // These name a role rather than a font; anything is right.
    }
    const QStringList names = offered.split(u',', Qt::SkipEmptyParts);
    for (const QString &name : names) {
        if (normalisedFamily(name) == target) {
            return true;
        }
    }
    return false;
}

bool styleIsBold(const QString &style)
{
    return style.contains(u"bold"_s, Qt::CaseInsensitive) || style.contains(u"heavy"_s, Qt::CaseInsensitive)
        || style.contains(u"black"_s, Qt::CaseInsensitive);
}

bool styleIsItalic(const QString &style)
{
    return style.contains(u"italic"_s, Qt::CaseInsensitive) || style.contains(u"oblique"_s, Qt::CaseInsensitive);
}

Located locateWithFontconfig(const FontEmbedder::Request &request)
{
    QString pattern;
    for (const QChar &character : request.family) {
        // Fontconfig reads these as pattern syntax rather than as part of a name.
        if (character == u'-' || character == u':' || character == u',' || character == u'=' || character == u'\\') {
            pattern.append(u'\\');
        }
        pattern.append(character);
    }
    if (request.bold) {
        pattern += u":bold"_s;
    }
    if (request.italic) {
        pattern += u":italic"_s;
    }

    QProcess process;
    process.start(u"fc-match"_s, { u"--format=%{file}\t%{family}\t%{style}"_s, pattern });
    if (!process.waitForFinished(5000) || process.exitStatus() != QProcess::NormalExit
        || process.exitCode() != 0) {
        return {};
    }
    const QStringList fields = QString::fromUtf8(process.readAllStandardOutput()).split(u'\t');
    if (fields.size() < 3) {
        return {};
    }
    Located found { fields.at(0).trimmed(), fields.at(1).trimmed(), fields.at(2).trimmed() };
    if (found.path.isEmpty() || !familyMatches(request.family, found.family)) {
        return {};
    }
    return found;
}

/** A name-table string, from the Windows or the Macintosh half of it. */
QString nameTableEntry(const QByteArray &name, int wanted)
{
    const int count = u16At(name, 2);
    const qsizetype strings = u16At(name, 4);
    QString mac;
    for (int i = 0; i < count; ++i) {
        const qsizetype at = 6 + qsizetype(i) * 12;
        if (int(u16At(name, at + 6)) != wanted) {
            continue;
        }
        const quint16 platform = u16At(name, at);
        const qsizetype length = u16At(name, at + 8);
        const qsizetype offset = strings + u16At(name, at + 10);
        const QByteArray raw = name.mid(offset, length);
        if (platform == 3) {
            QString text;
            for (qsizetype at16 = 0; at16 + 1 < raw.size(); at16 += 2) {
                text.append(QChar(u16At(raw, at16)));
            }
            if (!text.isEmpty()) {
                return text;
            }
        } else if (platform == 1 && mac.isEmpty()) {
            mac = QString::fromLatin1(raw);
        }
    }
    return mac;
}

/** Reads one table out of a font file without reading the whole font. */
QByteArray readOneTable(QFile &file, const char *tag)
{
    if (!file.seek(0)) {
        return {};
    }
    const QByteArray header = file.read(16);
    qsizetype base = 0;
    if (header.left(4) == QByteArrayLiteral("ttcf")) {
        base = qsizetype(u32At(header, 12));
    }
    if (!file.seek(base)) {
        return {};
    }
    const QByteArray offsets = file.read(12);
    const int count = u16At(offsets, 4);
    const QByteArray directory = file.read(qsizetype(count) * 16);
    for (int i = 0; i < count; ++i) {
        const qsizetype at = qsizetype(i) * 16;
        if (directory.mid(at, 4) != QByteArray(tag)) {
            continue;
        }
        if (!file.seek(u32At(directory, at + 8))) {
            return {};
        }
        return file.read(qsizetype(u32At(directory, at + 12)));
    }
    return {};
}

/**
 * The same search without Fontconfig, for a system that has no fc-match.
 *
 * Deliberately a fallback and not the first choice: fontconfig knows about the
 * user's own configuration, about aliases and about which cut of a family is
 * meant by "bold", and a directory walk knows none of that. But a directory walk
 * always works, and a missing helper programme is a poor reason to refuse to
 * embed a font.
 */
Located locateByScanning(const FontEmbedder::Request &request)
{
    QStringList directories = QStandardPaths::standardLocations(QStandardPaths::FontsLocation);
    for (const QString &fixed : { u"/usr/share/fonts"_s, u"/usr/local/share/fonts"_s }) {
        if (!directories.contains(fixed)) {
            directories.append(fixed);
        }
    }

    Located best;
    for (const QString &directory : std::as_const(directories)) {
        QDirIterator walk(directory, { u"*.ttf"_s, u"*.otf"_s, u"*.ttc"_s }, QDir::Files,
                          QDirIterator::Subdirectories);
        while (walk.hasNext()) {
            const QString path = walk.next();
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly)) {
                continue;
            }
            const QByteArray name = readOneTable(file, "name");
            if (name.isEmpty()) {
                continue;
            }
            const QString family = nameTableEntry(name, 16).isEmpty() ? nameTableEntry(name, 1)
                                                                      : nameTableEntry(name, 16);
            if (!familyMatches(request.family, family)) {
                continue;
            }
            const QString style = nameTableEntry(name, 17).isEmpty() ? nameTableEntry(name, 2)
                                                                     : nameTableEntry(name, 17);
            const Located found { path, family, style };
            if (styleIsBold(style) == request.bold && styleIsItalic(style) == request.italic) {
                return found;
            }
            if (best.path.isEmpty()) {
                best = found; // The family is right even if the cut is not.
            }
        }
    }
    return best;
}

Located locateFont(const FontEmbedder::Request &request)
{
    const Located found = locateWithFontconfig(request);
    return found.path.isEmpty() ? locateByScanning(request) : found;
}

// ── Turning that into a PDF font ──────────────────────────────────────────

/**
 * WinAnsiEncoding, which is Latin-1 with the gap between 0x80 and 0x9F filled.
 *
 * Used in preference to codes handed out in order, so that the bytes a page ends
 * up containing are the ones anything else reading the file would expect, and a
 * content stream stays readable to a human with a hex editor.
 */
int winAnsiCodeFor(QChar character)
{
    static const struct {
        int code;
        char16_t character;
    } filled[] = {
        { 0x80, 0x20AC }, { 0x82, 0x201A }, { 0x83, 0x0192 }, { 0x84, 0x201E }, { 0x85, 0x2026 },
        { 0x86, 0x2020 }, { 0x87, 0x2021 }, { 0x88, 0x02C6 }, { 0x89, 0x2030 }, { 0x8A, 0x0160 },
        { 0x8B, 0x2039 }, { 0x8C, 0x0152 }, { 0x8E, 0x017D }, { 0x91, 0x2018 }, { 0x92, 0x2019 },
        { 0x93, 0x201C }, { 0x94, 0x201D }, { 0x95, 0x2022 }, { 0x96, 0x2013 }, { 0x97, 0x2014 },
        { 0x98, 0x02DC }, { 0x99, 0x2122 }, { 0x9A, 0x0161 }, { 0x9B, 0x203A }, { 0x9C, 0x0153 },
        { 0x9E, 0x017E }, { 0x9F, 0x0178 },
    };
    const char16_t code = character.unicode();
    if (code >= 0x20 && code <= 0x7E) {
        return int(code);
    }
    if (code >= 0xA0 && code <= 0xFF) {
        return int(code);
    }
    for (const auto &entry : filled) {
        if (entry.character == code) {
            return entry.code;
        }
    }
    return 0;
}

/** The PostScript name of a character, from the same table TextEdit reads. */
QByteArray glyphNameFor(QChar character)
{
    static const QHash<char16_t, QByteArray> names = [] {
        QHash<char16_t, QByteArray> map;
        for (const GlyphNames::Entry &entry : GlyphNames::table) {
            // The table is sorted by name, so where two names mean the same
            // character the earlier one wins: "hyphen" over "minus", which is
            // the more usual spelling of the two.
            if (!map.contains(entry.character)) {
                map.insert(entry.character, QByteArray(entry.name));
            }
        }
        return map;
    }();

    const auto found = names.constFind(character.unicode());
    if (found != names.constEnd()) {
        return *found;
    }
    return QByteArrayLiteral("uni")
        + QByteArray::number(character.unicode(), 16).toUpper().rightJustified(4, '0');
}

/** A glyph name as the text of a PDF name object. */
std::string nameObjectFor(const QByteArray &glyphName)
{
    const QByteArray name = QByteArrayLiteral("/") + glyphName;
    return std::string(name.constData(), size_t(name.size()));
}

/**
 * Six capitals in front of the base name, as every producer marks a subset.
 *
 * Worked out from what went in rather than drawn at random, so that embedding
 * the same characters of the same font twice gives the same bytes, which is
 * what lets a document be built reproducibly and a test compare two runs.
 */
QString subsetTag(const QString &family, const QMap<int, QChar> &characterForCode)
{
    quint32 hash = 2166136261u;
    const auto mix = [&hash](quint32 value) {
        hash = (hash ^ value) * 16777619u;
    };
    for (const QChar &character : family) {
        mix(character.unicode());
    }
    for (auto it = characterForCode.constBegin(); it != characterForCode.constEnd(); ++it) {
        mix(quint32(it.key()));
        mix(it.value().unicode());
    }

    QString tag;
    for (int i = 0; i < 6; ++i) {
        tag.append(QChar(char16_t(u'A' + hash % 26)));
        hash /= 26;
    }
    return tag;
}

/** A name a PDF can hold: no spaces, no brackets, nothing that needs escaping. */
QString postScriptName(const QString &family, bool bold, bool italic)
{
    QString name;
    for (const QChar &character : family) {
        if (character.isLetterOrNumber()) {
            name.append(character);
        }
    }
    if (name.isEmpty()) {
        name = u"EmbeddedFont"_s;
    }
    if (bold && italic) {
        name += u"-BoldItalic"_s;
    } else if (bold) {
        name += u"-Bold"_s;
    } else if (italic) {
        name += u"-Italic"_s;
    }
    return name;
}

/** What the font's own licence bits say about being embedded, or nothing. */
QString licenceNote(quint16 fsType)
{
    // The low four bits are exclusive of one another, and zero means the font
    // may be installed anywhere, let alone embedded.
    const quint16 permission = fsType & 0x000F;
    QStringList notes;
    if (permission == 0x0002) {
        notes << i18n("This font's licence forbids embedding it in a document.");
    } else if (permission == 0x0004) {
        notes << i18n("This font's licence allows embedding for viewing and printing only, not for editing.");
    }
    if (fsType & 0x0100) {
        notes << i18n("This font's licence forbids embedding part of it, so the whole font would have to travel "
                      "with the document.");
    }
    if (fsType & 0x0200) {
        notes << i18n("This font's licence allows only its bitmaps to be embedded, not its outlines.");
    }
    return notes.join(u' ');
}

int scaledToThousandths(double value, int unitsPerEm)
{
    return int(std::lround(value * 1000.0 / unitsPerEm));
}

/** A `/ToUnicode` CMap, which is what keeps the finished text copyable. */
QByteArray toUnicodeCMap(const QMap<int, QChar> &characterForCode)
{
    QByteArray cmap = QByteArrayLiteral(
        "/CIDInit /ProcSet findresource begin\n"
        "12 dict begin\n"
        "begincmap\n"
        "/CIDSystemInfo << /Registry (Adobe) /Ordering (UCS) /Supplement 0 >> def\n"
        "/CMapName /Adobe-Identity-UCS def\n"
        "/CMapType 2 def\n"
        "1 begincodespacerange\n"
        "<00> <FF>\n"
        "endcodespacerange\n");

    // A hundred mappings per block is the limit the specification sets.
    QVector<QPair<int, QChar>> entries;
    for (auto it = characterForCode.constBegin(); it != characterForCode.constEnd(); ++it) {
        entries.append({ it.key(), it.value() });
    }
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

QPDFObjectHandle realNumber(double value)
{
    // QPDF's own newReal(double) formats through snprintf, which follows the
    // locale and writes "11,5" on a German desktop.
    return QPDFObjectHandle::newReal(number(value));
}

QString freeResourceName(QPDFObjectHandle fonts)
{
    for (int i = 1; i < 100000; ++i) {
        const QString name = u"/PsF"_s + QString::number(i);
        if (!fonts.hasKey(name.toStdString())) {
            return name;
        }
    }
    return u"/PsF"_s;
}

// ── Reading what the document already says about a font ───────────────────

/**
 * StandardEncoding, which is what a simple font with no `/Encoding` means.
 *
 * Not in Encodings.h beside WinAnsi and MacRoman because it is not a character
 * set anybody stores text in: it is the built-in encoding of the Adobe base
 * fonts, and the only place it ever surfaces is exactly this one: a `/Type1`
 * font dictionary with a `/BaseFont` and nothing else, which is what a word
 * processor writes when it means Helvetica.
 *
 * The two low-code differences from ASCII are the ones that catch people out:
 * code 39 is a closing quote here, not an apostrophe, and code 96 is an opening
 * quote, not a backtick.
 */
const char16_t *standardEncoding()
{
    static const std::array<char16_t, 256> table = [] {
        std::array<char16_t, 256> codes {};
        for (int code = 32; code <= 126; ++code) {
            codes[size_t(code)] = char16_t(code);
        }
        codes[39] = 0x2019;
        codes[96] = 0x2018;
        const struct {
            int code;
            char16_t character;
        } upper[] = {
            { 161, 0x00A1 }, { 162, 0x00A2 }, { 163, 0x00A3 }, { 164, 0x2044 }, { 165, 0x00A5 }, { 166, 0x0192 },
            { 167, 0x00A7 }, { 168, 0x00A4 }, { 169, 0x0027 }, { 170, 0x201C }, { 171, 0x00AB }, { 172, 0x2039 },
            { 173, 0x203A }, { 174, 0xFB01 }, { 175, 0xFB02 }, { 177, 0x2013 }, { 178, 0x2020 }, { 179, 0x2021 },
            { 180, 0x00B7 }, { 182, 0x00B6 }, { 183, 0x2022 }, { 184, 0x201A }, { 185, 0x201E }, { 186, 0x201D },
            { 187, 0x00BB }, { 188, 0x2026 }, { 189, 0x2030 }, { 191, 0x00BF }, { 193, 0x0060 }, { 194, 0x00B4 },
            { 195, 0x02C6 }, { 196, 0x02DC }, { 197, 0x00AF }, { 198, 0x02D8 }, { 199, 0x02D9 }, { 200, 0x00A8 },
            { 202, 0x02DA }, { 203, 0x00B8 }, { 205, 0x02DD }, { 206, 0x02DB }, { 207, 0x02C7 }, { 208, 0x2014 },
            { 225, 0x00C6 }, { 227, 0x00AA }, { 232, 0x0141 }, { 233, 0x00D8 }, { 234, 0x0152 }, { 235, 0x00BA },
            { 241, 0x00E6 }, { 245, 0x0131 }, { 248, 0x0142 }, { 249, 0x00F8 }, { 250, 0x0153 }, { 251, 0x00DF },
        };
        for (const auto &entry : upper) {
            codes[size_t(entry.code)] = entry.character;
        }
        return codes;
    }();
    return table.data();
}

std::string nameOf(QPDFObjectHandle object, const char *key)
{
    if (!object.isDictionary()) {
        return {};
    }
    QPDFObjectHandle value = object.getKey(key);
    return value.isName() ? value.getName() : std::string();
}

QString baseFontOf(QPDFObjectHandle font)
{
    QString name = QString::fromStdString(nameOf(font, "/BaseFont"));
    if (name.startsWith(u'/')) {
        name.remove(0, 1);
    }
    return name;
}

/** A glyph name as the character it draws; the mirror of glyphNameFor(). */
QChar characterForGlyphName(const QString &glyphName)
{
    QString name = glyphName;
    if (name.startsWith(u'/')) {
        name = name.mid(1);
    }
    // A variant suffix leaves the character alone: "one.fitted" is still a one.
    const qsizetype dot = name.indexOf(u'.');
    if (dot > 0) {
        name = name.left(dot);
    }
    if (name.isEmpty()) {
        return {};
    }
    if (name.size() >= 7 && name.startsWith(u"uni"_s)) {
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

/** What a simple font's codes mean, as the document itself states it. */
struct DocumentEncoding {
    QMap<int, QChar> characterForCode;

    /** True when the font carries an `/Encoding` rather than leaving it implied. */
    bool stated = false;

    /**
     * `/Differences` names that say nothing about what they draw.
     *
     * "g42" and "cid17" are what a subsetter writes once it has stopped caring,
     * and a code named that way cannot be given to a substitute honestly. It is
     * dropped rather than left pointing at whatever the base encoding happened
     * to say, and the name is kept here so that a caller can name out loud the
     * part of the page it could not carry over. Dropping them quietly is how a
     * substitution comes to lose two letters in a paragraph nobody rereads.
     */
    QStringList unresolved;
};

DocumentEncoding readEncoding(QPDFObjectHandle font)
{
    DocumentEncoding read;

    QPDFObjectHandle encoding = font.getKey("/Encoding");
    std::string base;
    if (encoding.isName()) {
        read.stated = true;
        base = encoding.getName();
    } else if (encoding.isDictionary()) {
        read.stated = true;
        base = nameOf(encoding, "/BaseEncoding");
    }
    const char16_t *table = base == "/WinAnsiEncoding" ? Encodings::winAnsi
        : base == "/MacRomanEncoding"                  ? Encodings::macRoman
                                                       : standardEncoding();

    // Which codes the document can address at all: a subset carries widths for
    // what its document used and for nothing else, and carrying a glyph for a
    // code the file cannot name would only make the subset bigger.
    QSet<int> known;
    QPDFObjectHandle widths = font.getKey("/Widths");
    QPDFObjectHandle firstChar = font.getKey("/FirstChar");
    if (widths.isArray() && firstChar.isInteger()) {
        const int first = firstChar.getIntValueAsInt();
        for (int i = 0; i < widths.getArrayNItems(); ++i) {
            known.insert(first + i);
        }
    }

    for (int code = 0; code <= 255; ++code) {
        const char16_t character = table[code];
        if (character < 0x20 || (!known.isEmpty() && !known.contains(code))) {
            continue;
        }
        read.characterForCode.insert(code, QChar(character));
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
            // A name nothing can resolve ("g42", "cid17") means the code can
            // no longer be given to a substitute honestly, so it is dropped
            // rather than left pointing at whatever the base encoding said.
            if (character.isNull()) {
                read.characterForCode.remove(code);
                read.unresolved << QString::fromStdString(item.getName());
            } else if (code >= 0 && code <= 255) {
                read.characterForCode.insert(code, character);
            }
            ++code;
        }
    }
    return read;
}

// ── Matching a name to a face ─────────────────────────────────────────────

/** A font name reduced to what two spellings of the same face have in common. */
QString squashedName(const QString &baseFont)
{
    static const QRegularExpression tag(u"^[A-Z]{6}\\+"_s);
    static const QRegularExpression punctuation(u"[^a-z0-9]"_s);
    QString name = baseFont;
    name.remove(tag);
    return name.toLower().remove(punctuation);
}

bool nameSaysBold(const QString &squashed)
{
    return squashed.contains(u"bold"_s) || squashed.contains(u"black"_s) || squashed.contains(u"heavy"_s);
}

bool nameSaysItalic(const QString &squashed)
{
    return squashed.contains(u"italic"_s) || squashed.contains(u"oblique"_s);
}

/** True when @p baseFont is the font @p wanted names, tag, spaces and case aside. */
bool namesTheSameFont(const QString &baseFont, const QString &wanted)
{
    return !wanted.trimmed().isEmpty() && squashedName(baseFont) == squashedName(wanted);
}

/**
 * The standard-fourteen entry a `/BaseFont` names, or nothing.
 *
 * Generous about the spelling on purpose: what a document actually contains is
 * `ArialMT` or `TimesNewRomanPS-BoldMT` far more often than `Helvetica`, and all
 * of them are the same metrics under a different foundry's name. Getting this
 * wrong in the cautious direction merely means the widths cannot be worked out
 * and the call says so.
 */
const Core14::Entry *core14For(const QString &baseFont)
{
    QString squashed = squashedName(baseFont);
    for (const QString &suffix : { u"psmt"_s, u"ps"_s, u"mt"_s }) {
        if (squashed.endsWith(suffix)) {
            squashed.chop(suffix.size());
            break;
        }
    }
    const bool bold = nameSaysBold(squashed);
    const bool italic = nameSaysItalic(squashed);

    QString name;
    if (squashed.contains(u"courier"_s) || squashed.contains(u"mono"_s)) {
        name = bold && italic ? u"Courier-BoldOblique"_s
            : bold            ? u"Courier-Bold"_s
            : italic          ? u"Courier-Oblique"_s
                              : u"Courier"_s;
    } else if (squashed.contains(u"helvetica"_s) || squashed.contains(u"arial"_s) || squashed.contains(u"sans"_s)) {
        name = bold && italic ? u"Helvetica-BoldOblique"_s
            : bold            ? u"Helvetica-Bold"_s
            : italic          ? u"Helvetica-Oblique"_s
                              : u"Helvetica"_s;
    } else if (squashed.contains(u"times"_s) || squashed.contains(u"serif"_s) || squashed.contains(u"roman"_s)) {
        name = bold && italic ? u"Times-BoldItalic"_s
            : bold            ? u"Times-Bold"_s
            : italic          ? u"Times-Italic"_s
                              : u"Times-Roman"_s;
    } else if (squashed.contains(u"zapf"_s) || squashed.contains(u"dingbat"_s)) {
        name = u"ZapfDingbats"_s;
    } else if (squashed.contains(u"symbol"_s)) {
        name = u"Symbol"_s;
    } else {
        return nullptr;
    }

    for (const Core14::Entry &entry : Core14::table) {
        if (QString::fromLatin1(entry.name) == name) {
            return &entry;
        }
    }
    return nullptr;
}

/**
 * A standard font's advances by character rather than by code, where it has one.
 *
 * The table is indexed the way its generator built it: codes 32 to 126 come
 * from the metrics file directly, which means StandardEncoding, and from 128 up
 * they were resolved through cp1252. Reading it back the same way is what keeps
 * an apostrophe from being given a closing quote's width.
 *
 * Above ASCII the generator could only resolve the accented letters, by
 * stripping the accent off a character its plain form already answers for. For
 * everything else (the dashes, the ellipsis, the guillemets, Æ, ß) it wrote
 * the family's average width, which is a serviceable answer to "does this glyph
 * fall inside a redaction rectangle" and a wrong one to "how far does the page
 * advance after it". Helvetica's ellipsis is a full em and the average is 514,
 * so writing the average into a document as though it were the metric a reader
 * had been laying the page out with moves that glyph by half its own width.
 * Those entries are left out, and what to do about a width nobody records is
 * then the caller's decision to make and to report.
 */
QHash<char16_t, int> standardWidths(const Core14::Entry &entry)
{
    QHash<char16_t, int> byCharacter;
    for (int code = Core14::firstCode; code <= Core14::lastCode; ++code) {
        const char16_t character = code <= 126 ? standardEncoding()[code]
            : code >= 128                      ? Encodings::winAnsi[code]
                                               : char16_t(0);
        if (character < 0x20 || byCharacter.contains(character)) {
            continue;
        }
        const int width = int(entry.widths[code - Core14::firstCode]);
        if (code > 126 && width == entry.averageWidth) {
            continue;
        }
        byCharacter.insert(character, width);
    }
    return byCharacter;
}

/** "TimesNewRomanPS-BoldMT" as a person would type it: "Times New Roman". */
QString readableFamily(const QString &baseFont)
{
    static const QRegularExpression tag(u"^[A-Z]{6}\\+"_s);
    QString name = baseFont;
    name.remove(tag);
    const qsizetype dash = name.indexOf(u'-');
    if (dash > 0) {
        name.truncate(dash);
    }
    for (const QString &suffix : { u"PSMT"_s, u"MT"_s, u"PS"_s }) {
        if (name.endsWith(suffix)) {
            name.chop(suffix.size());
            break;
        }
    }

    QString spaced;
    for (qsizetype at = 0; at < name.size(); ++at) {
        const QChar character = name.at(at);
        if (at > 0 && character.isUpper() && name.at(at - 1).isLower()) {
            spaced.append(u' ');
        }
        spaced.append(character);
    }
    return spaced.trimmed();
}

/**
 * Families to try for a font the document does not carry, best first.
 *
 * The document's own family name leads, because a machine that has Garamond
 * installed should get Garamond. After that come the metrically compatible
 * stand-ins (Liberation Sans is Helvetica's widths under another name, and
 * Liberation Serif and Mono are Times' and Courier's), which is the difference
 * between a substitution nobody notices and one that leaves every letter
 * fractionally adrift in its own space.
 */
QStringList substitutesFor(const QString &baseFont, QPDFObjectHandle descriptor)
{
    const QString squashed = squashedName(baseFont);
    QPDFObjectHandle flagsValue = descriptor.isDictionary() ? descriptor.getKey("/Flags") : QPDFObjectHandle::newNull();
    const int flags = flagsValue.isInteger() ? flagsValue.getIntValueAsInt() : 0;

    const bool mono = (flags & 1) || squashed.contains(u"courier"_s) || squashed.contains(u"mono"_s);
    const bool sans = squashed.contains(u"sans"_s) || squashed.contains(u"helvetica"_s)
        || squashed.contains(u"arial"_s) || squashed.contains(u"verdana"_s) || squashed.contains(u"tahoma"_s);
    const bool serif = !mono && !sans
        && ((flags & 2) || squashed.contains(u"times"_s) || squashed.contains(u"serif"_s)
            || squashed.contains(u"roman"_s) || squashed.contains(u"georgia"_s) || squashed.contains(u"garamond"_s)
            || squashed.contains(u"book"_s) || squashed.contains(u"minion"_s));

    QStringList candidates;
    const QString stated = readableFamily(baseFont);
    if (!stated.isEmpty()) {
        candidates << stated;
    }
    if (mono) {
        candidates << u"Liberation Mono"_s << u"DejaVu Sans Mono"_s << u"Nimbus Mono PS"_s << u"monospace"_s;
    } else if (serif) {
        candidates << u"Liberation Serif"_s << u"DejaVu Serif"_s << u"Nimbus Roman"_s << u"serif"_s;
    } else {
        candidates << u"Liberation Sans"_s << u"DejaVu Sans"_s << u"Nimbus Sans"_s << u"sans-serif"_s;
    }
    candidates.removeDuplicates();
    return candidates;
}

// ── Putting a system face behind a font the document already refers to ────

struct SystemFace {
    Located found;
    Face face;
};

bool loadFace(const QString &family, bool bold, bool italic, SystemFace *loaded, QString *error)
{
    FontEmbedder::Request request;
    request.family = family;
    request.bold = bold;
    request.italic = italic;

    const Located found = locateFont(request);
    if (found.path.isEmpty()) {
        if (error) {
            *error = i18n("No font called “%1” is installed on this system.", family);
        }
        return false;
    }
    QFile file(found.path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) {
            *error = i18n("The font file %1 cannot be read.", found.path);
        }
        return false;
    }
    const QByteArray bytes = file.readAll();
    file.close();

    *loaded = SystemFace {};
    loaded->found = found;
    return openFace(bytes, &loaded->face, error);
}

/** A glyph for a character, through the cmap or failing that the glyph names. */
int glyphFor(const Face &face, QChar character)
{
    const int fromCmap = face.glyphForCharacter.value(character.unicode(), 0);
    if (fromCmap > 0 && fromCmap < face.numGlyphs) {
        return fromCmap;
    }
    const int fromName = face.glyphForName.value(glyphNameFor(character), 0);
    return (fromName > 0 && fromName < face.numGlyphs) ? fromName : 0;
}

/** True when this font's glyphs already travel with the document. */
bool carriesGlyphs(QPDFObjectHandle font)
{
    const std::string subtype = nameOf(font, "/Subtype");
    if (subtype == "/Type3") {
        return true; // Its glyphs are drawings in the document; nothing is missing.
    }
    QPDFObjectHandle descriptor = font.getKey("/FontDescriptor");
    if (subtype == "/Type0") {
        QPDFObjectHandle descendants = font.getKey("/DescendantFonts");
        descriptor = descendants.isArray() && descendants.getArrayNItems() > 0
            ? descendants.getArrayItem(0).getKey("/FontDescriptor")
            : QPDFObjectHandle::newNull();
    }
    if (!descriptor.isDictionary()) {
        return false;
    }
    for (const char *key : { "/FontFile", "/FontFile2", "/FontFile3" }) {
        if (descriptor.getKey(key).isStream()) {
            return true;
        }
    }
    return false;
}

/**
 * Points @p font at a face taken off this system without moving a single glyph.
 *
 * The whole difficulty is in what is *not* rewritten. The `/Encoding` stays, so
 * code 233 goes on meaning é. The `/Widths` stay, so every glyph keeps the
 * position the page gave it: a simple font's advances come from the document,
 * not from the font programme, which is the fact that makes an honest
 * substitution possible at all. What changes is the descriptor, the glyph
 * programme behind it, and the name, because the document genuinely carries a
 * different face now and pretending otherwise would be a lie in the file.
 */
bool substituteInPlace(QPDF &pdf, QPDFObjectHandle font, const QString &requestedFamily,
                       FontEmbedder::Substitution *report)
{
    const QString baseFont = report->baseFont;
    const std::string subtype = nameOf(font, "/Subtype");
    if (subtype == "/Type0" || subtype == "/CIDFontType0" || subtype == "/CIDFontType2") {
        report->warnings << i18n("“%1” is a composite font, whose codes are glyph numbers with no meaning outside "
                                 "the font programme itself. Putting another face behind those numbers would need "
                                 "every code on every page rewritten, so it was left alone.",
                                 baseFont);
        return false;
    }
    if (subtype == "/Type3") {
        report->warnings << i18n("“%1” draws its glyphs with the document's own operators, so there is no font "
                                 "programme to supply.",
                                 baseFont);
        return false;
    }

    const DocumentEncoding stated = readEncoding(font);
    if (stated.characterForCode.isEmpty()) {
        report->warnings << i18n("Nothing in this document says what “%1”'s character codes mean, so no substitute "
                                 "could be given the same meaning. It was left alone.",
                                 baseFont);
        return false;
    }

    QPDFObjectHandle descriptor = font.getKey("/FontDescriptor");
    QPDFObjectHandle flagsValue = descriptor.isDictionary() ? descriptor.getKey("/Flags") : QPDFObjectHandle::newNull();
    const int oldFlags = flagsValue.isInteger() ? flagsValue.getIntValueAsInt() : 0;
    const QString squashed = squashedName(baseFont);
    const bool bold = nameSaysBold(squashed);
    const bool italic = nameSaysItalic(squashed) || (oldFlags & 64) != 0;

    const QStringList candidates
        = requestedFamily.trimmed().isEmpty() ? substitutesFor(baseFont, descriptor) : QStringList { requestedFamily };
    SystemFace loaded;
    QString whyNot;
    bool have = false;
    for (const QString &candidate : candidates) {
        QString reason;
        if (loadFace(candidate, bold, italic, &loaded, &reason)) {
            have = true;
            break;
        }
        if (whyNot.isEmpty()) {
            whyNot = reason;
        }
    }
    if (!have) {
        report->warnings << i18n("Nothing on this system could stand in for “%1”. %2", baseFont, whyNot);
        return false;
    }

    // ── The widths, which are what keeps the page from reflowing ──
    QPDFObjectHandle widths = font.getKey("/Widths");
    QPDFObjectHandle firstChar = font.getKey("/FirstChar");
    QMap<int, int> statedWidth;
    const bool widthsInFile = widths.isArray() && widths.getArrayNItems() > 0 && firstChar.isInteger();
    if (widthsInFile) {
        const int first = firstChar.getIntValueAsInt();
        for (int i = 0; i < widths.getArrayNItems(); ++i) {
            statedWidth.insert(first + i,
                               int(std::lround(PdfGeometry::numericValue(widths.getArrayItem(i), 0.0))));
        }
    }
    const Core14::Entry *standard = widthsInFile ? nullptr : core14For(baseFont);
    const QHash<char16_t, int> byCharacter = standard ? standardWidths(*standard) : QHash<char16_t, int>();

    QMap<uint, int> glyphForCharacter;
    QHash<int, QByteArray> nameForGlyph;
    QMap<int, QChar> carried;
    QMap<int, int> widthToWrite;
    QStringList undrawable;
    int worst = 0;

    /** Codes the document states no width for and no table records one for. */
    int unrecorded = 0;

    for (auto it = stated.characterForCode.constBegin(); it != stated.characterForCode.constEnd(); ++it) {
        const QChar character = it.value();
        const int glyph = glyphFor(loaded.face, character);
        if (glyph <= 0) {
            undrawable << QString(character);
            continue;
        }
        glyphForCharacter.insert(character.unicode(), glyph);
        nameForGlyph.insert(glyph, glyphNameFor(character));
        carried.insert(it.key(), character);

        const int own = scaledToThousandths(loaded.face.advances.value(glyph, 0), loaded.face.unitsPerEm);
        int assumed = -1;
        if (widthsInFile) {
            const auto found = statedWidth.constFind(it.key());
            assumed = found == statedWidth.constEnd() ? -1 : *found;
        } else {
            const auto found = byCharacter.constFind(character.unicode());
            assumed = found == byCharacter.constEnd() ? -1 : *found;
            widthToWrite.insert(it.key(), assumed < 0 ? own : assumed);
        }
        if (assumed < 0) {
            unrecorded += widthsInFile ? 0 : 1;
        } else {
            // Only where something states a width is there a gap to measure. A
            // code whose width was taken from the substitute in the first place
            // agrees with it by construction, and counting that as agreement
            // would flatter every substitution equally.
            worst = qMax(worst, qAbs(assumed - own));
        }
    }

    if (carried.isEmpty()) {
        report->warnings << i18n("“%1” holds none of the characters “%2” is used for, so substituting it would have "
                                 "emptied the page. It was left alone.",
                                 loaded.found.family, baseFont);
        return false;
    }

    // ── The glyph programme ──
    QByteArray programme;
    int glyphCount = 0;
    if (loaded.face.compactOutlines) {
        programme = tableBytes(loaded.face, "CFF ");
        if (programme.isEmpty()) {
            report->warnings << i18n("“%1”'s outlines are in a form this cannot read.", loaded.found.family);
            return false;
        }
        glyphCount = loaded.face.numGlyphs;
        report->warnings << i18n("“%1” stores its outlines in compact form, which cannot be cut down, so the whole "
                                 "font was embedded and the document grew by %2 kB.",
                                 loaded.found.family, QString::number(programme.size() / 1024));
    } else {
        Subset subset;
        QString reason;
        if (!buildSubset(loaded.face, glyphForCharacter, nameForGlyph, &subset, &reason)) {
            report->warnings << reason;
            return false;
        }
        programme = subset.program;
        glyphCount = int(subset.glyphs.size());
    }

    // ── The dictionary, rewritten only where it must be ──
    QPDFObjectHandle programmeStream = QPDFObjectHandle::newStream(
        &pdf, std::string(programme.constData(), size_t(programme.size())));
    if (loaded.face.compactOutlines) {
        programmeStream.getDict().replaceKey("/Subtype", QPDFObjectHandle::newName("/Type1C"));
    } else {
        programmeStream.getDict().replaceKey("/Length1", QPDFObjectHandle::newInteger(programme.size()));
    }

    const QString newBase = u"/"_s + subsetTag(loaded.found.family, carried) + u"+"_s
        + postScriptName(loaded.found.family, styleIsBold(loaded.found.style), styleIsItalic(loaded.found.style));

    int capHeight = loaded.face.capHeight;
    if (capHeight == 0) {
        const QByteArray outline = glyphOutline(loaded.face, loaded.face.glyphForCharacter.value(u'H', 0));
        capHeight = outline.isEmpty() ? qint16(double(loaded.face.ascender) * 0.7) : s16At(outline, 8);
    }

    QPDFObjectHandle bbox = QPDFObjectHandle::newArray();
    for (qint16 edge : { loaded.face.xMin, loaded.face.yMin, loaded.face.xMax, loaded.face.yMax }) {
        bbox.appendItem(QPDFObjectHandle::newInteger(scaledToThousandths(edge, loaded.face.unitsPerEm)));
    }

    // Non-symbolic and nothing else carried over: the old flags described a face
    // that is no longer in the file, and a stale symbolic bit would send a
    // reader looking for an encoding the new programme does not have.
    int flags = 32;
    if (loaded.face.fixedPitch) {
        flags |= 1;
    }
    if (loaded.face.serif) {
        flags |= 2;
    }
    if (styleIsItalic(loaded.found.style)) {
        flags |= 64;
    }

    QPDFObjectHandle fresh = QPDFObjectHandle::newDictionary();
    fresh.replaceKey("/Type", QPDFObjectHandle::newName("/FontDescriptor"));
    fresh.replaceKey("/FontName", QPDFObjectHandle::newName(newBase.toStdString()));
    fresh.replaceKey("/Flags", QPDFObjectHandle::newInteger(flags));
    fresh.replaceKey("/FontBBox", bbox);
    fresh.replaceKey("/ItalicAngle", realNumber(loaded.face.italicAngle));
    fresh.replaceKey("/Ascent",
                     QPDFObjectHandle::newInteger(scaledToThousandths(loaded.face.ascender, loaded.face.unitsPerEm)));
    fresh.replaceKey("/Descent",
                     QPDFObjectHandle::newInteger(scaledToThousandths(loaded.face.descender, loaded.face.unitsPerEm)));
    fresh.replaceKey("/CapHeight", QPDFObjectHandle::newInteger(scaledToThousandths(capHeight, loaded.face.unitsPerEm)));
    fresh.replaceKey("/StemV",
                     QPDFObjectHandle::newInteger(50 + int(std::lround(std::pow(loaded.face.weightClass / 100.0, 2.0)))));
    fresh.replaceKey(loaded.face.compactOutlines ? "/FontFile3" : "/FontFile2", programmeStream);

    font.replaceKey("/Subtype", QPDFObjectHandle::newName(loaded.face.compactOutlines ? "/Type1" : "/TrueType"));
    font.replaceKey("/BaseFont", QPDFObjectHandle::newName(newBase.toStdString()));
    font.replaceKey("/FontDescriptor", pdf.makeIndirectObject(fresh));

    if (!stated.stated) {
        // The font had no /Encoding, so what its codes meant lived in an
        // agreement between the reader and a face that is not in the file. It is
        // written out now, code by code, so the agreement is in the document.
        QPDFObjectHandle differences = QPDFObjectHandle::newArray();
        int expected = -1;
        for (auto it = carried.constBegin(); it != carried.constEnd(); ++it) {
            if (it.key() != expected) {
                differences.appendItem(QPDFObjectHandle::newInteger(it.key()));
            }
            differences.appendItem(QPDFObjectHandle::newName(nameObjectFor(glyphNameFor(it.value()))));
            expected = it.key() + 1;
        }
        QPDFObjectHandle encoding = QPDFObjectHandle::newDictionary();
        encoding.replaceKey("/Type", QPDFObjectHandle::newName("/Encoding"));
        encoding.replaceKey("/Differences", differences);
        font.replaceKey("/Encoding", encoding);
    }

    if (!widthsInFile && !widthToWrite.isEmpty()) {
        const int first = widthToWrite.firstKey();
        const int last = widthToWrite.lastKey();
        QPDFObjectHandle array = QPDFObjectHandle::newArray();
        for (int code = first; code <= last; ++code) {
            array.appendItem(QPDFObjectHandle::newInteger(widthToWrite.value(code, 0)));
        }
        font.replaceKey("/FirstChar", QPDFObjectHandle::newInteger(first));
        font.replaceKey("/LastChar", QPDFObjectHandle::newInteger(last));
        font.replaceKey("/Widths", array);
    }

    if (!font.getKey("/ToUnicode").isStream()) {
        const QByteArray cmap = toUnicodeCMap(carried);
        font.replaceKey("/ToUnicode",
                        QPDFObjectHandle::newStream(&pdf, std::string(cmap.constData(), size_t(cmap.size()))));
    }

    // ── What the person doing this needs to be told ──
    const QString licence = licenceNote(loaded.face.fsType);
    if (!licence.isEmpty()) {
        report->warnings << licence;
    }
    if (!undrawable.isEmpty()) {
        report->warnings << i18n("“%1” cannot draw %2, so those codes now show nothing. The text is still there and "
                                 "still copyable; it is the shapes that are missing.",
                                 loaded.found.family, undrawable.join(u", "_s));
    }
    if (!stated.unresolved.isEmpty()) {
        QStringList named = stated.unresolved;
        named.removeDuplicates();
        report->warnings << i18np("“%2” names one of its codes %3, which says nothing about what that code draws. "
                                  "It was left out rather than given a letter on a guess, so that code now shows "
                                  "nothing.",
                                  "“%2” names %1 of its codes with glyph names that say nothing about what they "
                                  "draw (%3). They were left out rather than given letters on a guess, so those "
                                  "codes now show nothing.",
                                  stated.unresolved.size(), baseFont, named.mid(0, 6).join(u", "_s));
    }
    if (!widthsInFile && standard != nullptr && unrecorded == 0) {
        report->warnings << i18n("“%1” stated no widths of its own, as one of the standard fourteen may. The "
                                 "standard metrics were written into the document so that the spacing survives "
                                 "being made explicit.",
                                 baseFont);
    }
    if (!widthsInFile && standard != nullptr && unrecorded > 0) {
        report->warnings << i18np("“%2” stated no widths of its own, as one of the standard fourteen may, so the "
                                  "standard metrics were written into the document. One code has no width in "
                                  "those tables and was given the substitute's own advance instead.",
                                  "“%2” stated no widths of its own, as one of the standard fourteen may, so the "
                                  "standard metrics were written into the document. %1 codes have no width in "
                                  "those tables (the dashes, the ellipsis, the letters with no plain form) and "
                                  "were given the substitute's own advances instead.",
                                  unrecorded, baseFont);
    }
    if (!widthsInFile && standard == nullptr) {
        report->warnings << i18n("“%1” states no widths and is not one of the standard fourteen, so how wide its "
                                 "characters were meant to be is not recorded anywhere. The substitute's own widths "
                                 "were written instead, and lines that depended on the old spacing will move.",
                                 baseFont);
    }
    if (worst > 20) {
        report->warnings << i18n("“%1” is not metrically the same font as “%2”: the widest character differs by %3 "
                                 "thousandths of the em. Nothing moves, because the document's own widths still "
                                 "decide that, but the letters no longer quite fill the space kept for them.",
                                 loaded.found.family, baseFont, worst);
    }

    report->family = loaded.found.family;
    report->codes = int(carried.size());
    report->glyphCount = glyphCount;
    report->embeddedBytes = programme.size();
    report->widthGap = worst;
    return true;
}

// ── Finding the fonts a document's pages actually use ─────────────────────

/** How deep to follow form XObjects, which carry resources of their own. */
constexpr int MaxResourceDepth = 8;

using FontVisitor = std::function<void(const std::string &, QPDFObjectHandle)>;

void eachFontIn(QPDFObjectHandle resources, std::set<QPDFObjGen> &visited, int depth, const FontVisitor &visit)
{
    if (depth > MaxResourceDepth || !resources.isDictionary()) {
        return;
    }
    QPDFObjectHandle fonts = resources.getKey("/Font");
    if (fonts.isDictionary()) {
        for (const auto &[key, font] : fonts.getDictAsMap()) {
            if (font.isDictionary()) {
                visit(key, font);
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
        eachFontIn(object.getDict().getKey("/Resources"), visited, depth + 1, visit);
    }
}

/** The fonts an annotation's appearance uses, which no page's resources mention. */
void eachAnnotationFontIn(QPDFObjectHandle page, std::set<QPDFObjGen> &visited, const FontVisitor &visit)
{
    QPDFObjectHandle annotations = page.getKey("/Annots");
    if (!annotations.isArray()) {
        return;
    }
    for (int i = 0; i < annotations.getArrayNItems(); ++i) {
        QPDFObjectHandle annotation = annotations.getArrayItem(i);
        QPDFObjectHandle appearance
            = annotation.isDictionary() ? annotation.getKey("/AP") : QPDFObjectHandle::newNull();
        if (!appearance.isDictionary()) {
            continue;
        }
        QPDFObjectHandle normal = appearance.getKey("/N");
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
            eachFontIn(stream.getDict().getKey("/Resources"), visited, 1, visit);
        }
    }
}

/** One font object, wherever the pages reach it from. */
struct FontSite {
    QPDFObjectHandle font;
    QStringList resourceNames;
    QVector<int> pages;
};

QVector<FontSite> collectFontSites(QPDF &pdf)
{
    QVector<FontSite> sites;
    std::map<QPDFObjGen, int> known;

    QPDFPageDocumentHelper documents(pdf);
    std::vector<QPDFPageObjectHelper> pages = documents.getAllPages();
    for (int index = 0; index < int(pages.size()); ++index) {
        std::set<QPDFObjGen> visited;
        const FontVisitor note = [&sites, &known, index](const std::string &key, QPDFObjectHandle font) {
            int at = -1;
            if (font.isIndirect()) {
                const auto seen = known.find(font.getObjGen());
                if (seen != known.end()) {
                    at = seen->second;
                }
            }
            if (at < 0) {
                at = int(sites.size());
                sites.append(FontSite { font, {}, {} });
                if (font.isIndirect()) {
                    known.insert({ font.getObjGen(), at });
                }
            }
            const QString name = QString::fromStdString(key);
            if (!sites[at].resourceNames.contains(name)) {
                sites[at].resourceNames.append(name);
            }
            if (!sites[at].pages.contains(index)) {
                sites[at].pages.append(index);
            }
        };
        eachFontIn(pages.at(size_t(index)).getAttribute("/Resources", false), visited, 0, note);
        eachAnnotationFontIn(pages.at(size_t(index)).getObjectHandle(), visited, note);
    }
    return sites;
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

bool FontEmbedder::isAvailable(const Request &request)
{
    const Located found = locateFont(request);
    if (found.path.isEmpty()) {
        return false;
    }
    QFile file(found.path);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    const QByteArray flavour = file.read(4);
    return flavour == QByteArray("\x00\x01\x00\x00", 4) || flavour == QByteArrayLiteral("true")
        || flavour == QByteArrayLiteral("OTTO") || flavour == QByteArrayLiteral("ttcf");
}

QString FontEmbedder::locate(const Request &request)
{
    return locateFont(request).path;
}

bool FontEmbedder::embed(QPDF &pdf, QPDFObjectHandle resources, const Request &request, Result *result,
                         QString *error)
{
    if (request.characters.isEmpty()) {
        if (error) {
            *error = i18n("No characters were asked for, so there is nothing to embed.");
        }
        return false;
    }
    if (!resources.isDictionary()) {
        if (error) {
            *error = i18n("This page has no resources for a font to be added to.");
        }
        return false;
    }

    const Located found = locateFont(request);
    if (found.path.isEmpty()) {
        if (error) {
            *error = i18n("No font called “%1” is installed on this system.", request.family);
        }
        return false;
    }

    QFile file(found.path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) {
            *error = i18n("The font file %1 cannot be read.", found.path);
        }
        return false;
    }
    const QByteArray bytes = file.readAll();
    file.close();

    Face face;
    if (!openFace(bytes, &face, error)) {
        return false;
    }

    Result collected;
    const QString licence = licenceNote(face.fsType);
    if (!licence.isEmpty()) {
        collected.warnings << licence;
    }
    const bool bold = styleIsBold(found.style);
    const bool italic = styleIsItalic(found.style);
    if (request.bold && !bold) {
        collected.warnings << i18n("No bold cut of “%1” is installed, so the ordinary one was embedded.",
                                   request.family);
    }
    if (request.italic && !italic) {
        collected.warnings << i18n("No italic cut of “%1” is installed, so the upright one was embedded.",
                                   request.family);
    }

    // Sorted, because a QSet has no order and two runs of the same request have
    // to produce the same file.
    QVector<QChar> characters(request.characters.constBegin(), request.characters.constEnd());
    std::sort(characters.begin(), characters.end(), [](QChar left, QChar right) {
        return left.unicode() < right.unicode();
    });

    QMap<uint, int> glyphForCharacter;
    QStringList missing;
    for (const QChar &character : std::as_const(characters)) {
        // Through glyphFor() rather than the cmap alone, so that a face whose
        // cmap was thrown away but whose glyphs are still named can be embedded
        // here as well as cut down by subsetProgramme(). Two ways in that
        // disagree about which characters a font holds is worse than either.
        const int glyph = glyphFor(face, character);
        if (glyph <= 0) {
            missing << QString(character);
            continue;
        }
        glyphForCharacter.insert(character.unicode(), glyph);
    }
    if (glyphForCharacter.isEmpty()) {
        if (error) {
            *error = i18n("“%1” holds none of the characters asked for.", found.family);
        }
        return false;
    }
    if (!missing.isEmpty()) {
        collected.warnings << i18n("“%1” does not hold %2, so those were left out.", found.family,
                                   missing.join(u", "_s));
    }

    // Give every character a code. WinAnsi where it has one, a spare code
    // otherwise; either way the encoding written into the file is what says
    // which is which, so nothing has to agree with this function later.
    QMap<int, QChar> characterForCode;
    QVector<QChar> deferred;
    for (auto it = glyphForCharacter.constBegin(); it != glyphForCharacter.constEnd(); ++it) {
        const QChar character = QChar(char16_t(it.key()));
        const int code = winAnsiCodeFor(character);
        if (code > 0 && !characterForCode.contains(code)) {
            characterForCode.insert(code, character);
        } else {
            deferred.append(character);
        }
    }
    for (const QChar &character : std::as_const(deferred)) {
        int code = 0;
        for (int candidate = 33; candidate <= 255 && code == 0; ++candidate) {
            if (!characterForCode.contains(candidate)) {
                code = candidate;
            }
        }
        for (int candidate = 1; candidate <= 32 && code == 0; ++candidate) {
            if (!characterForCode.contains(candidate)) {
                code = candidate;
            }
        }
        if (code == 0) {
            if (error) {
                *error = i18n("A font of this kind can show at most 255 different characters, and more than that "
                              "were asked for.");
            }
            return false;
        }
        characterForCode.insert(code, character);
    }

    QHash<int, QByteArray> nameForGlyph;
    for (auto it = characterForCode.constBegin(); it != characterForCode.constEnd(); ++it) {
        nameForGlyph.insert(glyphForCharacter.value(it.value().unicode()), glyphNameFor(it.value()));
    }

    QByteArray programme;
    int glyphCount = 0;
    if (face.compactOutlines) {
        // Told plainly rather than done badly: cutting a compact-format outline
        // table down means rewriting a second, quite different font format, and
        // an honestly larger file beats a subtly broken one.
        programme = tableBytes(face, "CFF ");
        if (programme.isEmpty()) {
            if (error) {
                *error = i18n("This font's outlines are in a form this cannot read.");
            }
            return false;
        }
        glyphCount = face.numGlyphs;
        collected.warnings << i18n("“%1” stores its outlines in compact form, which cannot be cut down, so the "
                                   "whole font was embedded and the document grew by %2 kB.",
                                   found.family, QString::number(programme.size() / 1024));
        collected.warnings << i18n("A font embedded whole in this form is addressed by glyph name, so a character "
                                   "with no standard name may not appear.");
    } else {
        Subset subset;
        if (!buildSubset(face, glyphForCharacter, nameForGlyph, &subset, error)) {
            return false;
        }
        programme = subset.program;
        glyphCount = int(subset.glyphs.size());
    }

    // ── The font's own numbers, in the thousandths a PDF measures in ──
    const int first = characterForCode.firstKey();
    const int last = characterForCode.lastKey();

    QPDFObjectHandle widths = QPDFObjectHandle::newArray();
    for (int code = first; code <= last; ++code) {
        const auto character = characterForCode.constFind(code);
        const int glyph = character == characterForCode.constEnd()
            ? 0
            : glyphForCharacter.value(character->unicode(), 0);
        widths.appendItem(QPDFObjectHandle::newInteger(
            scaledToThousandths(face.advances.value(glyph, 0), face.unitsPerEm)));
    }

    QPDFObjectHandle differences = QPDFObjectHandle::newArray();
    int expected = -1;
    for (auto it = characterForCode.constBegin(); it != characterForCode.constEnd(); ++it) {
        if (it.key() != expected) {
            differences.appendItem(QPDFObjectHandle::newInteger(it.key()));
        }
        differences.appendItem(QPDFObjectHandle::newName(nameObjectFor(glyphNameFor(it.value()))));
        expected = it.key() + 1;
    }

    QPDFObjectHandle encoding = QPDFObjectHandle::newDictionary();
    encoding.replaceKey("/Type", QPDFObjectHandle::newName("/Encoding"));
    encoding.replaceKey("/BaseEncoding", QPDFObjectHandle::newName("/WinAnsiEncoding"));
    encoding.replaceKey("/Differences", differences);

    int capHeight = face.capHeight;
    if (capHeight == 0) {
        // Measured off a capital rather than guessed at, when the font does not
        // say: the bounding box of an H is exactly what cap height means.
        const QByteArray outline = glyphOutline(face, face.glyphForCharacter.value(u'H', 0));
        capHeight = outline.isEmpty() ? qint16(double(face.ascender) * 0.7) : s16At(outline, 8);
    }

    QPDFObjectHandle bbox = QPDFObjectHandle::newArray();
    for (qint16 edge : { face.xMin, face.yMin, face.xMax, face.yMax }) {
        bbox.appendItem(QPDFObjectHandle::newInteger(scaledToThousandths(edge, face.unitsPerEm)));
    }

    int flags = 32; // Non-symbolic, because every code is named in the encoding.
    if (face.fixedPitch) {
        flags |= 1;
    }
    if (face.serif) {
        flags |= 2;
    }
    if (italic) {
        flags |= 64;
    }

    QPDFObjectHandle programmeStream = QPDFObjectHandle::newStream(
        &pdf, std::string(programme.constData(), size_t(programme.size())));
    if (face.compactOutlines) {
        programmeStream.getDict().replaceKey("/Subtype", QPDFObjectHandle::newName("/Type1C"));
    } else {
        programmeStream.getDict().replaceKey("/Length1", QPDFObjectHandle::newInteger(programme.size()));
    }

    const QString baseName = u"/"_s + subsetTag(found.family, characterForCode) + u"+"_s
        + postScriptName(found.family, bold, italic);

    QPDFObjectHandle descriptor = QPDFObjectHandle::newDictionary();
    descriptor.replaceKey("/Type", QPDFObjectHandle::newName("/FontDescriptor"));
    descriptor.replaceKey("/FontName", QPDFObjectHandle::newName(baseName.toStdString()));
    descriptor.replaceKey("/Flags", QPDFObjectHandle::newInteger(flags));
    descriptor.replaceKey("/FontBBox", bbox);
    descriptor.replaceKey("/ItalicAngle", realNumber(face.italicAngle));
    descriptor.replaceKey("/Ascent",
                          QPDFObjectHandle::newInteger(scaledToThousandths(face.ascender, face.unitsPerEm)));
    descriptor.replaceKey("/Descent",
                          QPDFObjectHandle::newInteger(scaledToThousandths(face.descender, face.unitsPerEm)));
    descriptor.replaceKey("/CapHeight",
                          QPDFObjectHandle::newInteger(scaledToThousandths(capHeight, face.unitsPerEm)));
    // Stem width is an estimate from the font's weight and always was: it only
    // guides a viewer that has to draw a substitute, and this font is here.
    descriptor.replaceKey("/StemV",
                          QPDFObjectHandle::newInteger(50 + int(std::lround(std::pow(face.weightClass / 100.0, 2.0)))));
    descriptor.replaceKey(face.compactOutlines ? "/FontFile3" : "/FontFile2", programmeStream);

    QPDFObjectHandle toUnicode = QPDFObjectHandle::newStream(&pdf, [&] {
        const QByteArray cmap = toUnicodeCMap(characterForCode);
        return std::string(cmap.constData(), size_t(cmap.size()));
    }());

    QPDFObjectHandle font = QPDFObjectHandle::newDictionary();
    font.replaceKey("/Type", QPDFObjectHandle::newName("/Font"));
    font.replaceKey("/Subtype", QPDFObjectHandle::newName(face.compactOutlines ? "/Type1" : "/TrueType"));
    font.replaceKey("/BaseFont", QPDFObjectHandle::newName(baseName.toStdString()));
    font.replaceKey("/FirstChar", QPDFObjectHandle::newInteger(first));
    font.replaceKey("/LastChar", QPDFObjectHandle::newInteger(last));
    font.replaceKey("/Widths", widths);
    font.replaceKey("/Encoding", encoding);
    font.replaceKey("/FontDescriptor", pdf.makeIndirectObject(descriptor));
    font.replaceKey("/ToUnicode", toUnicode);

    QPDFObjectHandle fonts = resources.getKey("/Font");
    if (!fonts.isDictionary()) {
        fonts = QPDFObjectHandle::newDictionary();
        resources.replaceKey("/Font", fonts);
    }
    const QString name = freeResourceName(fonts);
    fonts.replaceKey(name.toStdString(), pdf.makeIndirectObject(font));

    collected.resourceName = name;
    collected.glyphCount = glyphCount;
    collected.embeddedBytes = programme.size();
    if (result) {
        *result = collected;
    }
    return true;
}

bool FontEmbedder::embed(const QString &in, const QString &out, const Request &request, const QVector<int> &pages,
                         QVector<Result> *results, QString *error)
{
    QVector<Result> collected;
    try {
        QPDF pdf;
        PdfFile::open(pdf, in);

        QPDFPageDocumentHelper documents(pdf);
        std::vector<QPDFPageObjectHelper> all = documents.getAllPages();
        if (all.empty()) {
            if (error) {
                *error = i18n("This document has no pages for a font to be added to.");
            }
            return false;
        }

        QVector<int> wanted = pages;
        if (wanted.isEmpty()) {
            for (int page = 0; page < int(all.size()); ++page) {
                wanted.append(page);
            }
        }
        for (const int page : std::as_const(wanted)) {
            if (page < 0 || page >= int(all.size())) {
                if (error) {
                    *error = i18n("This document has no page %1.", page + 1);
                }
                return false;
            }
            // Copied if shared: pages very often inherit one resource dictionary
            // from the page tree, and adding a font to that would quietly add it
            // to every page in the document.
            QPDFObjectHandle resources = all.at(size_t(page)).getAttribute("/Resources", true);
            Result result;
            if (!embed(pdf, resources, request, &result, error)) {
                return false;
            }
            collected.append(result);
        }

        writeOut(pdf, out);
    } catch (const std::exception &e) {
        if (error) {
            *error = QString::fromUtf8(e.what());
        }
        return false;
    }

    if (results) {
        *results = collected;
    }
    return true;
}

bool FontEmbedder::embedMissing(const QString &in, const QString &out, const QString &family,
                                QVector<Substitution> *substitutions, QString *error)
{
    QVector<Substitution> report;
    int done = 0;
    try {
        QPDF pdf;
        PdfFile::open(pdf, in);

        QVector<FontSite> sites = collectFontSites(pdf);
        int considered = 0;
        for (FontSite &site : sites) {
            if (carriesGlyphs(site.font)) {
                continue;
            }
            ++considered;
            Substitution one;
            one.baseFont = baseFontOf(site.font);
            one.resourceNames = site.resourceNames;
            one.pages = site.pages;
            if (substituteInPlace(pdf, site.font, family, &one)) {
                ++done;
            }
            report.append(one);
        }

        if (done == 0) {
            if (error) {
                *error = considered == 0
                    ? i18n("Every font this document uses already travels with it, so there is nothing to embed.")
                    : i18n("None of the fonts this document is missing could be given a face from this system.");
            }
            if (substitutions) {
                *substitutions = report;
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

    if (substitutions) {
        *substitutions = report;
    }
    return true;
}

bool FontEmbedder::replaceFont(const QString &in, const QString &out, const QString &oldBaseFont,
                               const QString &newFamily, QVector<Substitution> *substitutions, QString *error)
{
    if (oldBaseFont.trimmed().isEmpty() || newFamily.trimmed().isEmpty()) {
        if (error) {
            *error = i18n("Replacing a font needs both the font to replace and the family to put in its place.");
        }
        return false;
    }

    QVector<Substitution> report;
    int done = 0;
    try {
        QPDF pdf;
        PdfFile::open(pdf, in);

        QVector<FontSite> sites = collectFontSites(pdf);
        int matched = 0;
        for (FontSite &site : sites) {
            const QString baseFont = baseFontOf(site.font);
            if (!namesTheSameFont(baseFont, oldBaseFont)) {
                continue;
            }
            ++matched;
            Substitution one;
            one.baseFont = baseFont;
            one.resourceNames = site.resourceNames;
            one.pages = site.pages;
            if (substituteInPlace(pdf, site.font, newFamily, &one)) {
                ++done;
            }
            report.append(one);
        }

        if (done == 0) {
            if (error) {
                *error = matched == 0
                    ? i18n("No font in this document is called “%1”. “fonts list” says what it does use.",
                           oldBaseFont)
                    : i18n("“%1” was found but could not be replaced with “%2”.", oldBaseFont, newFamily);
            }
            if (substitutions) {
                *substitutions = report;
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

    if (substitutions) {
        *substitutions = report;
    }
    return true;
}

QByteArray FontEmbedder::subsetProgramme(const QByteArray &programme, const QSet<QChar> &characters, int *glyphsBefore,
                                         int *glyphsAfter, QString *error)
{
    Face face;
    if (!openFace(programme, &face, error)) {
        return {};
    }
    if (glyphsBefore) {
        *glyphsBefore = face.numGlyphs;
    }
    if (face.compactOutlines) {
        if (error) {
            *error = i18n("This font's outlines are in compact form, which cannot be cut down.");
        }
        return {};
    }

    // Sorted, so that two runs over the same set produce the same bytes.
    QVector<QChar> sorted(characters.constBegin(), characters.constEnd());
    std::sort(sorted.begin(), sorted.end(), [](QChar left, QChar right) {
        return left.unicode() < right.unicode();
    });

    QMap<uint, int> glyphForCharacter;
    QHash<int, QByteArray> nameForGlyph;
    for (const QChar &character : std::as_const(sorted)) {
        const int glyph = glyphFor(face, character);
        if (glyph <= 0) {
            continue;
        }
        glyphForCharacter.insert(character.unicode(), glyph);
        nameForGlyph.insert(glyph, glyphNameFor(character));
    }
    if (glyphForCharacter.isEmpty()) {
        if (error) {
            *error = i18n("This font programme says nothing about which glyph draws which character, so what it "
                          "holds cannot be worked out from the outside.");
        }
        return {};
    }

    Subset subset;
    if (!buildSubset(face, glyphForCharacter, nameForGlyph, &subset, error)) {
        return {};
    }
    if (glyphsAfter) {
        *glyphsAfter = int(subset.glyphs.size());
    }
    return subset.program;
}

QStringList FontEmbedder::limitations()
{
    return {
        i18n("Only the characters asked for go into the document, which is what keeps the file small. Text added "
             "later in the same font needs its characters embedded too."),
        i18n("A font added this way can hold 255 different characters at most. A document needing more than that "
             "needs one font per 255, which this does not yet arrange."),
        i18n("Characters outside the basic multilingual plane (most emoji, and the rarer Chinese) cannot be "
             "embedded."),
        i18n("A font whose outlines are in compact form is embedded whole rather than cut down, because cutting "
             "that format down properly is a second piece of work. The file is larger and the reason is reported."),
        i18n("Kerning is not carried over: the characters are spaced by their own widths, which is what a PDF "
             "records anyway."),
        i18n("What a font's licence permits is read from the font and reported, never enforced. Whether a licence "
             "covers what is being done with it is not something a program can know."),
        i18n("Substituting a face for a font the document is missing keeps every character code and every stated "
             "width, so nothing on the page moves. What it cannot keep is the shapes: a substitute is a different "
             "typeface, and only the metrically compatible ones fill the same space."),
        i18n("A composite font (the kind used for Chinese, and by most modern producers for everything) cannot "
             "be substituted or replaced, because its codes are glyph numbers meaningful only inside the font "
             "programme that is missing. Those are named rather than guessed at."),
        i18n("A font the document refers to without saying what its codes mean, and which is not one of the "
             "standard fourteen, is left alone: there is no honest way to give a substitute the same meaning."),
        i18n("Only fonts a page can reach are dealt with. A font used solely by a document's interactive form "
             "resources is not."),
    };
}

} // namespace ps
