/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#include "TestPdf.h"

#include "core/FontEmbedder.h"
#include "core/FontInventory.h"
#include "core/PdfFile.h"

#include <KLocalizedString>

#include <QFileInfo>
#include <QMap>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QTest>
#include <QtEnvironmentVariables>

#include <cmath>

#include <qpdf/Buffer.hh>
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>
#include <qpdf/QPDFWriter.hh>

using namespace ps;
using namespace Qt::Literals::StringLiterals;

namespace {

QSet<QChar> charactersOf(const QString &text)
{
    QSet<QChar> characters;
    for (const QChar &character : text) {
        characters.insert(character);
    }
    return characters;
}

QByteArray dataOf(QPDFObjectHandle stream)
{
    if (!stream.isStream()) {
        return {};
    }
    const std::shared_ptr<Buffer> buffer = stream.getStreamData();
    return QByteArray(reinterpret_cast<const char *>(buffer->getBuffer()), qsizetype(buffer->getSize()));
}

/**
 * A `/ToUnicode` CMap read back out of a finished file.
 *
 * Written from scratch rather than by calling the parser that produced it, so
 * that a mistake in one is not confirmed by the other. Only the one-byte form
 * this project writes is understood, which is all that is being checked.
 */
QMap<int, QChar> toUnicodeOf(QPDFObjectHandle font)
{
    QMap<int, QChar> mapping;
    const QByteArray raw = dataOf(font.getKey("/ToUnicode"));
    const QString text = QString::fromLatin1(raw);
    if (!text.contains(u"beginbfchar"_s) || !text.contains(u"endcmap"_s)) {
        return mapping;
    }
    static const QRegularExpression pair(u"<([0-9A-Fa-f]{2})>\\s*<([0-9A-Fa-f]{4})>"_s);
    auto matches = pair.globalMatch(text);
    while (matches.hasNext()) {
        const QRegularExpressionMatch match = matches.next();
        mapping.insert(match.captured(1).toInt(nullptr, 16), QChar(match.captured(2).toUShort(nullptr, 16)));
    }
    return mapping;
}

// ══ A second, independent reader for the font files produced ═══════════════
//
// The point of a subset test is that the bytes are right, so the test parses
// them itself instead of asking the subsetter what it thinks it wrote. Enough of
// the sfnt format for that and no more.

quint16 u16Of(const QByteArray &data, qsizetype at)
{
    if (at < 0 || at + 1 >= data.size()) {
        return 0;
    }
    return quint16((quint16(quint8(data.at(at))) << 8) | quint8(data.at(at + 1)));
}

qint16 s16Of(const QByteArray &data, qsizetype at)
{
    return qint16(u16Of(data, at));
}

quint32 u32Of(const QByteArray &data, qsizetype at)
{
    return (quint32(u16Of(data, at)) << 16) | u16Of(data, at + 2);
}

class SfntReader
{
public:
    bool open(const QByteArray &bytes)
    {
        m_data = bytes;
        const quint32 flavour = u32Of(bytes, 0);
        if (flavour != 0x00010000u && flavour != 0x74727565u && flavour != 0x4F54544Fu) {
            return false;
        }
        const int count = u16Of(bytes, 4);
        for (int i = 0; i < count; ++i) {
            const qsizetype at = 12 + qsizetype(i) * 16;
            const QByteArray tag = bytes.mid(at, 4);
            if (tag.size() != 4) {
                return false;
            }
            const qsizetype offset = qsizetype(u32Of(bytes, at + 8));
            const qsizetype length = qsizetype(u32Of(bytes, at + 12));
            if (offset + length > bytes.size()) {
                return false;
            }
            m_tables.insert(tag, bytes.mid(offset, length));
        }
        return m_tables.contains(QByteArrayLiteral("head")) && m_tables.contains(QByteArrayLiteral("maxp"));
    }

    QByteArray table(const char *tag) const { return m_tables.value(QByteArray(tag)); }

    int numGlyphs() const { return u16Of(table("maxp"), 4); }

    int unitsPerEm() const { return u16Of(table("head"), 18); }

    /** The glyph a character maps to, through the Windows Unicode subtable. */
    int glyphFor(char16_t character) const
    {
        const QByteArray cmap = table("cmap");
        const int count = u16Of(cmap, 2);
        for (int i = 0; i < count; ++i) {
            const qsizetype record = 4 + qsizetype(i) * 8;
            if (u16Of(cmap, record) != 3 || u16Of(cmap, record + 2) != 1) {
                continue;
            }
            const qsizetype at = qsizetype(u32Of(cmap, record + 4));
            if (u16Of(cmap, at) != 4) {
                continue;
            }
            const int segments = u16Of(cmap, at + 6) / 2;
            const qsizetype ends = at + 14;
            const qsizetype starts = ends + qsizetype(segments) * 2 + 2;
            const qsizetype deltas = starts + qsizetype(segments) * 2;
            const qsizetype ranges = deltas + qsizetype(segments) * 2;
            for (int segment = 0; segment < segments; ++segment) {
                const quint16 last = u16Of(cmap, ends + segment * 2);
                const quint16 first = u16Of(cmap, starts + segment * 2);
                if (character < first || character > last) {
                    continue;
                }
                const quint16 rangeOffset = u16Of(cmap, ranges + segment * 2);
                const qint16 delta = s16Of(cmap, deltas + segment * 2);
                if (rangeOffset == 0) {
                    return int((character + quint16(delta)) & 0xFFFF);
                }
                const quint16 glyph
                    = u16Of(cmap, ranges + segment * 2 + rangeOffset + qsizetype(character - first) * 2);
                return glyph == 0 ? 0 : int((glyph + quint16(delta)) & 0xFFFF);
            }
        }
        return 0;
    }

    QByteArray outlineOf(int glyph) const
    {
        const QByteArray loca = table("loca");
        const QByteArray glyf = table("glyf");
        const bool longFormat = s16Of(table("head"), 50) != 0;
        const quint32 from = longFormat ? u32Of(loca, qsizetype(glyph) * 4) : quint32(u16Of(loca, glyph * 2)) * 2;
        const quint32 to
            = longFormat ? u32Of(loca, qsizetype(glyph) * 4 + 4) : quint32(u16Of(loca, glyph * 2 + 2)) * 2;
        if (to <= from || qsizetype(to) > glyf.size()) {
            return {};
        }
        return glyf.mid(qsizetype(from), qsizetype(to - from));
    }

    /** The glyphs a composite is assembled from. */
    QVector<int> componentsOf(const QByteArray &outline) const
    {
        QVector<int> components;
        if (s16Of(outline, 0) >= 0) {
            return components;
        }
        qsizetype at = 10;
        while (at + 4 <= outline.size()) {
            const quint16 flags = u16Of(outline, at);
            components.append(int(u16Of(outline, at + 2)));
            at += 4;
            at += (flags & 0x0001) ? 4 : 2;
            if (flags & 0x0008) {
                at += 2;
            } else if (flags & 0x0040) {
                at += 4;
            } else if (flags & 0x0080) {
                at += 8;
            }
            if (!(flags & 0x0020)) {
                break;
            }
        }
        return components;
    }

    /** The advance width of a glyph, in the font's own units. */
    int advanceOf(int glyph) const
    {
        const int entries = qMax(1, int(u16Of(table("hhea"), 34)));
        const int at = qMin(glyph, entries - 1);
        return u16Of(table("hmtx"), qsizetype(at) * 4);
    }

    /**
     * Whether every table's checksum in the directory matches its bytes.
     *
     * The head table is the awkward one: its checksum is defined to be taken
     * with its own checkSumAdjustment field zeroed, because that field holds a
     * sum over the whole file and cannot be known while the table is being
     * written. So it is zeroed here too, rather than skipped.
     */
    QString mismatch() const { return m_mismatch; }

    bool checksumsAgree() const
    {
        const int count = u16Of(m_data, 4);
        for (int i = 0; i < count; ++i) {
            const qsizetype at = 12 + qsizetype(i) * 16;
            const QByteArray tag = m_data.mid(at, 4);
            QByteArray bytes = m_tables.value(tag);
            if (tag == QByteArrayLiteral("head") && bytes.size() >= 12) {
                bytes.replace(8, 4, QByteArray(4, '\0'));
            }
            // The sum runs over the table padded with zeros to a multiple of
            // four, which the file itself does not store. Without the padding
            // the last partial word is read as nothing and the total comes out
            // short by whatever those one to three bytes hold.
            while (bytes.size() % 4 != 0) {
                bytes.append('\0');
            }
            quint32 sum = 0;
            for (qsizetype byte = 0; byte < bytes.size(); byte += 4) {
                sum += u32Of(bytes, byte);
            }
            // A directory entry is tag, checkSum, offset, length, in that
            // order. Reading at + 8 gets the offset, which for the first table
            // happens to be a plausible-looking number, so the mistake shows up
            // as "the checksum is wrong" rather than as an obvious misread.
            if (sum != u32Of(m_data, at + 4)) {
                m_mismatch = QStringLiteral("%1: computed %2, stored %3, %4 bytes")
                                 .arg(QString::fromLatin1(tag))
                                 .arg(sum)
                                 .arg(u32Of(m_data, at + 4))
                                 .arg(bytes.size());
                return false;
            }
        }
        return true;
    }

    /** Whether the sum over the whole file is the one head claims it is. */
    bool fileChecksumAgrees() const
    {
        const int count = u16Of(m_data, 4);
        for (int i = 0; i < count; ++i) {
            const qsizetype at = 12 + qsizetype(i) * 16;
            if (m_data.mid(at, 4) != QByteArrayLiteral("head")) {
                continue;
            }
            // Offset is the third field of the entry, at + 8. Reading at + 12
            // gets the length, which for a small table is a number small enough
            // to look like a plausible offset, so the error surfaces as a wrong
            // checksum rather than as an obvious misread.
            const qsizetype head = qsizetype(u32Of(m_data, at + 8));
            const quint32 claimed = u32Of(m_data, head + 8);
            QByteArray whole = m_data;
            whole.replace(head + 8, 4, QByteArray(4, '\0'));
            while (whole.size() % 4 != 0) {
                whole.append('\0');
            }
            quint32 sum = 0;
            for (qsizetype byte = 0; byte < whole.size(); byte += 4) {
                sum += u32Of(whole, byte);
            }
            return claimed == 0xB1B0AFBAu - sum;
        }
        return false;
    }

private:
    QByteArray m_data;
    QMap<QByteArray, QByteArray> m_tables;
    mutable QString m_mismatch;
};

} // namespace

/**
 * Embedding a font, and reading a document's fonts back out.
 *
 * Every assertion here goes through a file on disc: the subset is parsed out of
 * the PDF that was written, not out of whatever the subsetter still had in
 * memory. That matters most for the composite-glyph case, where a wrong answer
 * produces a file that validates, opens, and draws nothing where the letter
 * should be.
 */
class TestFonts : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;

    QString path(const QString &name) const { return m_dir.filePath(name); }

    /** True when a family this system could embed is installed under that name. */
    static bool haveFamily(const QString &family)
    {
        FontEmbedder::Request request;
        request.family = family;
        request.characters = charactersOf(u"a"_s);
        return FontEmbedder::isAvailable(request);
    }

    /** True when DejaVu Sans is installed, which every test below needs. */
    static bool haveTestFont() { return haveFamily(u"DejaVu Sans"_s); }

    /**
     * Puts a subset holding @p text on the first page of a fresh document, and
     * optionally draws @p text with it.
     *
     * The codes to draw with are taken from the font's own `/ToUnicode` map, i.e.
     * from the file rather than from anything the embedder returned, which is
     * how a caller with no special knowledge would have to do it.
     */
    bool embedInto(const QString &output, const QString &text, bool draw, FontEmbedder::Result *result,
                   QString *error, int pageCount = 1)
    {
        const QString source = path(u"source.pdf"_s);
        if (!ps::test::writeSamplePdf(source, pageCount)) {
            return false;
        }
        try {
            QPDF pdf;
            PdfFile::open(pdf, source);
            QPDFPageDocumentHelper documents(pdf);
            std::vector<QPDFPageObjectHelper> pages = documents.getAllPages();

            FontEmbedder::Request request;
            request.characters = charactersOf(text);
            for (int page = 0; page < int(pages.size()); ++page) {
                QPDFObjectHandle resources = pages.at(size_t(page)).getAttribute("/Resources", false);
                if (!FontEmbedder::embed(pdf, resources, request, result, error)) {
                    return false;
                }
                if (!draw) {
                    continue;
                }

                QMap<QChar, int> codes;
                const QMap<int, QChar> mapping
                    = toUnicodeOf(resources.getKey("/Font").getKey(result->resourceName.toStdString()));
                for (auto it = mapping.constBegin(); it != mapping.constEnd(); ++it) {
                    codes.insert(it.value(), it.key());
                }
                QByteArray hex;
                for (const QChar &character : text) {
                    hex += QByteArray::number(codes.value(character, 0), 16).rightJustified(2, '0');
                }
                const std::string content = "BT " + result->resourceName.toStdString() + " 24 Tf 72 600 Td <"
                    + hex.toStdString() + "> Tj ET\n";
                pages.at(size_t(page)).addPageContents(QPDFObjectHandle::newStream(&pdf, content), false);
            }

            QPDFWriter writer(pdf);
            writer.setOutputFilename(QFile::encodeName(output).constData());
            writer.setObjectStreamMode(qpdf_o_generate);
            writer.setStreamDataMode(qpdf_s_compress);
            writer.write();
        } catch (const std::exception &e) {
            if (error) {
                *error = QString::fromUtf8(e.what());
            }
            return false;
        }
        return true;
    }

    /** The font this project added, found by the name it was added under. */
    static QPDFObjectHandle embeddedFontOf(QPDF &pdf, const QString &resourceName, int page = 0)
    {
        QPDFPageDocumentHelper documents(pdf);
        std::vector<QPDFPageObjectHelper> pages = documents.getAllPages();
        QPDFObjectHandle resources = pages.at(size_t(page)).getAttribute("/Resources", false);
        return resources.getKey("/Font").getKey(resourceName.toStdString());
    }

    /**
     * A one-page document whose only font is @p fontDictionary, written verbatim.
     *
     * Hand-built rather than produced by this project, because everything below
     * is about repairing what somebody else's producer wrote: a font the page
     * refers to and no glyphs travel with, a composite font, a font whose
     * `/Differences` name their glyphs by number. Nothing here can write one of
     * those, which is exactly why the tests need them.
     */
    bool writeDocumentWithFont(const QString &output, const char *fontDictionary, const char *content)
    {
        try {
            QPDF pdf;
            pdf.emptyPDF();
            QPDFPageDocumentHelper documents(pdf);

            QPDFObjectHandle font = pdf.makeIndirectObject(QPDFObjectHandle::parse(fontDictionary));
            QPDFObjectHandle fonts = QPDFObjectHandle::newDictionary();
            fonts.replaceKey("/F1", font);
            QPDFObjectHandle resources = QPDFObjectHandle::newDictionary();
            resources.replaceKey("/Font", fonts);

            QPDFObjectHandle page = QPDFObjectHandle::newDictionary();
            page.replaceKey("/Type", QPDFObjectHandle::newName("/Page"));
            page.replaceKey("/MediaBox", QPDFObjectHandle::parse("[0 0 595 842]"));
            page.replaceKey("/Resources", resources);
            page.replaceKey("/Contents", QPDFObjectHandle::newStream(&pdf, std::string(content)));
            documents.addPage(QPDFPageObjectHelper(pdf.makeIndirectObject(page)), false);

            QPDFWriter writer(pdf);
            writer.setOutputFilename(QFile::encodeName(output).constData());
            writer.setDeterministicID(true);
            writer.write();
        } catch (const std::exception &) {
            return false;
        }
        return true;
    }

    /** A document whose font describes its characters but does not say what they mean. */
    bool writeFontWithoutToUnicode(const QString &output)
    {
        return writeDocumentWithFont(
            output,
            "<< /Type /Font /Subtype /TrueType /BaseFont /ABCDEF+Fixture /FirstChar 65 /LastChar 67"
            " /Widths [ 600 600 600 ]"
            " /Encoding << /Type /Encoding /Differences [ 65 /eacute /germandbls /adieresis ] >> >>",
            "BT /F1 24 Tf 72 700 Td <414243> Tj ET\n");
    }

    /** The glyph programme a font in a finished document carries, decompressed. */
    static QByteArray programmeOf(QPDFObjectHandle font)
    {
        QPDFObjectHandle descriptor = font.getKey("/FontDescriptor");
        return descriptor.isDictionary() ? dataOf(descriptor.getKey("/FontFile2")) : QByteArray();
    }

    /** Every warning of every substitution, glued together for one contains() check. */
    static QString allWarnings(const QVector<FontEmbedder::Substitution> &substitutions)
    {
        QStringList collected;
        for (const FontEmbedder::Substitution &one : substitutions) {
            collected += one.warnings;
        }
        return collected.join(u" | "_s);
    }

private Q_SLOTS:
    void initTestCase()
    {
        KLocalizedString::setApplicationDomain("pdf-smithy");
        QVERIFY(m_dir.isValid());
    }

    void aFamilyNobodyHasInstalledIsReportedAsUnavailable()
    {
        // fc-match answers every question, substituting when it must, so this is
        // the check that a request for a font nobody has does not quietly embed
        // whatever happened to be nearby.
        FontEmbedder::Request request;
        request.family = u"Erfundene Schrift Ohne Datei 12345"_s;
        request.characters = charactersOf(u"abc"_s);
        QVERIFY(!FontEmbedder::isAvailable(request));
        QVERIFY(FontEmbedder::locate(request).isEmpty());

        QString error;
        QPDF pdf;
        pdf.emptyPDF();
        QVERIFY(!FontEmbedder::embed(pdf, QPDFObjectHandle::newDictionary(), request, nullptr, &error));
        QVERIFY(!error.isEmpty());
    }

    void anInstalledFamilyIsFoundAsAFileThatCanBeRead()
    {
        if (!haveTestFont()) {
            QSKIP("DejaVu Sans is not installed on this machine");
        }
        FontEmbedder::Request request;
        request.characters = charactersOf(u"a"_s);
        const QString file = FontEmbedder::locate(request);
        QVERIFY(!file.isEmpty());
        QVERIFY(QFileInfo::exists(file));

        // The bold cut is a different file from the ordinary one; a locator that
        // ignored the style would return the same path for both.
        request.bold = true;
        const QString bold = FontEmbedder::locate(request);
        QVERIFY(!bold.isEmpty());
        QVERIFY(bold != file);
    }

    void embeddingWritesASubsetWithWidthsAndAMapBackToTheCharacters()
    {
        if (!haveTestFont()) {
            QSKIP("DejaVu Sans is not installed on this machine");
        }
        const QString wanted = u"Grüße Tom"_s;
        const QString output = path(u"greeting.pdf"_s);
        FontEmbedder::Result result;
        QString error;
        QVERIFY2(embedInto(output, wanted, false, &result, &error), qPrintable(error));

        QCOMPARE(result.resourceName, u"/PsF1"_s);
        QVERIFY(result.embeddedBytes > 1000);
        // A subset and not the whole face, which is three quarters of a megabyte.
        QVERIFY(result.embeddedBytes < 60000);
        QVERIFY(result.glyphCount >= charactersOf(wanted).size());

        QPDF pdf;
        PdfFile::open(pdf, output);
        QPDFObjectHandle font = embeddedFontOf(pdf, result.resourceName);
        QVERIFY(font.isDictionary());
        QCOMPARE(QString::fromStdString(font.getKey("/Subtype").getName()), u"/TrueType"_s);

        // Six capitals and a plus, which is how every producer marks a subset and
        // how the inventory recognises one.
        const QString baseFont = QString::fromStdString(font.getKey("/BaseFont").getName());
        QVERIFY2(QRegularExpression(u"^/[A-Z]{6}\\+DejaVuSans$"_s).match(baseFont).hasMatch(),
                 qPrintable(baseFont));

        QPDFObjectHandle descriptor = font.getKey("/FontDescriptor");
        QVERIFY(descriptor.isDictionary());
        QPDFObjectHandle programme = descriptor.getKey("/FontFile2");
        QVERIFY(programme.isStream());
        const QByteArray bytes = dataOf(programme);
        QCOMPARE(programme.getDict().getKey("/Length1").getIntValue(), qint64(bytes.size()));
        QCOMPARE(bytes.size(), qsizetype(result.embeddedBytes));

        // Non-symbolic, because every code is named in the encoding.
        QVERIFY(descriptor.getKey("/Flags").getIntValueAsInt() & 32);
        QVERIFY(descriptor.getKey("/Ascent").getIntValueAsInt() > 0);
        QVERIFY(descriptor.getKey("/Descent").getIntValueAsInt() < 0);
        QVERIFY(descriptor.getKey("/CapHeight").getIntValueAsInt() > 0);
        QCOMPARE(descriptor.getKey("/FontBBox").getArrayNItems(), 4);

        // The map back to characters holds exactly what was asked for.
        const QMap<int, QChar> mapping = toUnicodeOf(font);
        QSet<QChar> mapped;
        for (const QChar &character : mapping) {
            mapped.insert(character);
        }
        QCOMPARE(mapped, charactersOf(wanted));

        // And every code it uses has a width in the table.
        const int first = font.getKey("/FirstChar").getIntValueAsInt();
        const int last = font.getKey("/LastChar").getIntValueAsInt();
        QPDFObjectHandle widths = font.getKey("/Widths");
        QCOMPARE(widths.getArrayNItems(), last - first + 1);
        for (auto it = mapping.constBegin(); it != mapping.constEnd(); ++it) {
            QVERIFY(it.key() >= first && it.key() <= last);
            const int width = widths.getArrayItem(it.key() - first).getIntValueAsInt();
            QVERIFY2(width > 0, qPrintable(u"no width for %1"_s.arg(it.value())));
        }

        // The umlaut kept its own name rather than becoming a number.
        QVERIFY(QString::fromStdString(font.getKey("/Encoding").unparseResolved())
                    .contains(u"/udieresis"_s));
    }

    void theSubsetOfAnUmlautKeepsTheGlyphsItIsBuiltFrom()
    {
        if (!haveTestFont()) {
            QSKIP("DejaVu Sans is not installed on this machine");
        }
        // The bug this test exists for: an "ä" is a reference to an "a" and a
        // reference to a diaeresis. A subset holding only the "ä" is a valid file
        // in which the letter draws as nothing at all.
        const QString output = path(u"umlaut.pdf"_s);
        FontEmbedder::Result result;
        QString error;
        QVERIFY2(embedInto(output, u"ä"_s, false, &result, &error), qPrintable(error));

        QPDF pdf;
        PdfFile::open(pdf, output);
        QPDFObjectHandle font = embeddedFontOf(pdf, result.resourceName);
        const QByteArray programme = dataOf(font.getKey("/FontDescriptor").getKey("/FontFile2"));

        SfntReader subset;
        QVERIFY(subset.open(programme));
        QVERIFY2(subset.checksumsAgree(), qPrintable(subset.mismatch()));
        QVERIFY2(subset.fileChecksumAgrees(), "the whole-file checksum in head is wrong");
        QVERIFY(subset.unitsPerEm() > 0);

        const int glyph = subset.glyphFor(u'ä');
        QVERIFY2(glyph > 0, "the subset does not map the character it was made for");
        QVERIFY(glyph < subset.numGlyphs());

        const QByteArray outline = subset.outlineOf(glyph);
        QVERIFY2(!outline.isEmpty(), "the glyph has no outline at all");

        const QVector<int> components = subset.componentsOf(outline);
        if (components.isEmpty()) {
            // A font that draws the letter in one piece is equally correct.
            QVERIFY(s16Of(outline, 0) > 0);
        } else {
            QVERIFY2(components.size() >= 2, "a diaeresis on an a needs two parts");
            QVERIFY2(subset.numGlyphs() > 2, "the parts cannot be there if the font holds only two glyphs");
            for (int component : components) {
                // Renumbered into the subset, not left pointing at the position
                // it held in the 6000-glyph original.
                QVERIFY2(component > 0 && component < subset.numGlyphs(), "a component points outside the subset");
                const QByteArray part = subset.outlineOf(component);
                QVERIFY2(!part.isEmpty(), "a component of the composite was left behind");
                QVERIFY2(s16Of(part, 0) > 0, "a component was kept but has no contours");
            }
        }

        // The advance width survived, or the letter would sit on top of the next.
        QVERIFY(subset.advanceOf(glyph) > 0);
        QVERIFY(programme.size() < 8000);
    }

    void aPageWrittenWithTheEmbeddedFontReadsBackAsWhatWasWritten()
    {
        if (!haveTestFont()) {
            QSKIP("DejaVu Sans is not installed on this machine");
        }
        const QString wanted = u"Größe"_s;
        const QString output = path(u"written.pdf"_s);
        FontEmbedder::Result result;
        QString error;
        QVERIFY2(embedInto(output, wanted, true, &result, &error), qPrintable(error));

        const QVector<FontUse> uses = FontInventory::read(output, &error);
        QVERIFY2(!uses.isEmpty(), qPrintable(error));

        int found = 0;
        for (const FontUse &use : uses) {
            if (!use.family.contains(u"DejaVu"_s)) {
                continue;
            }
            ++found;
            QVERIFY(use.embedded);
            QVERIFY(use.subset);
            QVERIFY(use.hasToUnicode);
            QCOMPARE(use.resourceName, result.resourceName);
            QCOMPARE(use.pages, QVector<int>({ 0 }));
            QCOMPARE(use.fileSize, result.embeddedBytes);
            QVERIFY(!use.licence.isEmpty());
            for (const QChar &character : wanted) {
                QVERIFY2(use.coverage.contains(character),
                         qPrintable(u"coverage is missing %1"_s.arg(character)));
            }
        }
        QCOMPARE(found, 1);

        // And the bytes actually on the page mean what was asked for. Decoded
        // through the map in the file, the way any other reader would.
        const QString content = ps::test::contentOf(output, 0);
        QVERIFY(content.contains(result.resourceName));
        static const QRegularExpression shown(u"<([0-9A-Fa-f]+)>\\s*Tj"_s);
        const QRegularExpressionMatch match = shown.match(content);
        QVERIFY(match.hasMatch());

        QPDF pdf;
        PdfFile::open(pdf, output);
        const QMap<int, QChar> mapping = toUnicodeOf(embeddedFontOf(pdf, result.resourceName));
        const QString hex = match.captured(1);
        QString read;
        for (qsizetype at = 0; at + 1 < hex.size(); at += 2) {
            read.append(mapping.value(hex.mid(at, 2).toInt(nullptr, 16), QChar(u'?')));
        }
        QCOMPARE(read, wanted);
    }

    void tooManyCharactersForOneFontIsRefusedRatherThanTruncated()
    {
        if (!haveTestFont()) {
            QSKIP("DejaVu Sans is not installed on this machine");
        }
        FontEmbedder::Request request;
        for (char16_t character = 0x20; character < 0x0180; ++character) {
            request.characters.insert(QChar(character));
        }
        QVERIFY(request.characters.size() > 255);

        QPDF pdf;
        pdf.emptyPDF();
        QString error;
        QPDFObjectHandle resources = QPDFObjectHandle::newDictionary();
        QVERIFY(!FontEmbedder::embed(pdf, resources, request, nullptr, &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(!resources.hasKey("/Font"));
    }

    void removingAnEmbeddedFontMakesTheFileSmallerAndTheFontUnembedded()
    {
        if (!haveTestFont()) {
            QSKIP("DejaVu Sans is not installed on this machine");
        }
        const QString embedded = path(u"embedded.pdf"_s);
        FontEmbedder::Result result;
        QString error;
        QVERIFY2(embedInto(embedded, u"Grüße Tom"_s, true, &result, &error), qPrintable(error));

        const QString stripped = path(u"stripped.pdf"_s);
        int removed = 0;
        QVERIFY2(FontInventory::removeEmbedding(embedded, stripped, { u"DejaVu Sans"_s }, &removed, &error),
                 qPrintable(error));
        QCOMPARE(removed, 1);
        QVERIFY(QFileInfo(stripped).size() < QFileInfo(embedded).size());

        const QVector<FontUse> uses = FontInventory::read(stripped, &error);
        int found = 0;
        for (const FontUse &use : uses) {
            if (!use.family.contains(u"DejaVu"_s)) {
                continue;
            }
            ++found;
            QVERIFY(!use.embedded);
            QCOMPARE(use.fileSize, 0);
            // The text is still addressable; only its appearance is now the
            // reader's business.
            QVERIFY(use.hasToUnicode);
            QVERIFY(use.coverage.contains(QChar(u'ü')));
        }
        QCOMPARE(found, 1);

        // A name that is not in the document is refused rather than silently
        // doing nothing.
        QVERIFY(!FontInventory::removeEmbedding(embedded, path(u"none.pdf"_s), { u"Garamond"_s }, &removed, &error));
        QVERIFY(!error.isEmpty());
    }

    void twoIdenticalSubsetsOfTheSameFamilyBecomeOneObject()
    {
        if (!haveTestFont()) {
            QSKIP("DejaVu Sans is not installed on this machine");
        }
        // What merging documents produces: the same subset of the same font,
        // embedded once per chapter. Two pages here, each with its own copy.
        const QString doubled = path(u"doubled.pdf"_s);
        FontEmbedder::Result result;
        QString error;
        QVERIFY2(embedInto(doubled, u"Grüße Tom"_s, true, &result, &error, 2), qPrintable(error));

        QVector<FontUse> before = FontInventory::read(doubled, &error);
        int copies = 0;
        for (const FontUse &use : before) {
            if (use.family.contains(u"DejaVu"_s)) {
                ++copies;
            }
        }
        QCOMPARE(copies, 2);

        const QString merged = path(u"merged.pdf"_s);
        int count = 0;
        QVERIFY2(FontInventory::mergeSubsets(doubled, merged, &count, &error), qPrintable(error));
        QCOMPARE(count, 1);
        QVERIFY(QFileInfo(merged).size() < QFileInfo(doubled).size());

        const QVector<FontUse> after = FontInventory::read(merged, &error);
        int survivors = 0;
        for (const FontUse &use : after) {
            if (!use.family.contains(u"DejaVu"_s)) {
                continue;
            }
            ++survivors;
            // One object, now used by both pages, and still complete.
            QCOMPARE(use.pages, QVector<int>({ 0, 1 }));
            QVERIFY(use.embedded);
            QCOMPARE(use.fileSize, result.embeddedBytes);
        }
        QCOMPARE(survivors, 1);

        // Both pages still say what they said, in the same bytes.
        QCOMPARE(ps::test::contentOf(merged, 0), ps::test::contentOf(doubled, 0));
        QCOMPARE(ps::test::contentOf(merged, 1), ps::test::contentOf(doubled, 1));
    }

    void aFontWithNoMapOfItsOwnGetsOneWrittenFromItsEncoding()
    {
        const QString source = path(u"unreadable.pdf"_s);
        QVERIFY(writeFontWithoutToUnicode(source));

        QString error;
        QVector<FontUse> before = FontInventory::read(source, &error);
        QCOMPARE(before.size(), 1);
        QVERIFY(!before.at(0).hasToUnicode);
        // The encoding alone already says what the codes mean, which is how the
        // map can be written at all.
        QVERIFY(before.at(0).coverage.contains(QChar(u'é')));
        QCOMPARE(before.at(0).encoding, u"/Differences"_s);

        const QString repaired = path(u"repaired.pdf"_s);
        int added = 0;
        QVERIFY2(FontInventory::addToUnicode(source, repaired, &added, &error), qPrintable(error));
        QCOMPARE(added, 1);

        QPDF pdf;
        PdfFile::open(pdf, repaired);
        QPDFObjectHandle font = embeddedFontOf(pdf, u"/F1"_s);
        QVERIFY(font.getKey("/ToUnicode").isStream());

        const QMap<int, QChar> mapping = toUnicodeOf(font);
        QCOMPARE(mapping.value(65), QChar(u'é'));
        QCOMPARE(mapping.value(66), QChar(u'ß'));
        QCOMPARE(mapping.value(67), QChar(u'ä'));
        QCOMPARE(mapping.size(), 3);

        const QVector<FontUse> after = FontInventory::read(repaired, &error);
        QCOMPARE(after.size(), 1);
        QVERIFY(after.at(0).hasToUnicode);
        QCOMPARE(after.at(0).coverage, QSet<QChar>({ QChar(u'é'), QChar(u'ß'), QChar(u'ä') }));

        // Asking twice changes nothing the second time.
        QVERIFY(!FontInventory::addToUnicode(repaired, path(u"again.pdf"_s), &added, &error));
    }

    // ══ Repairing what a document already refers to ════════════════════════

    void aMissingStandardFontGainsAFaceWithoutMovingAnything()
    {
        if (!haveFamily(u"Liberation Sans"_s)) {
            QSKIP("Liberation Sans is not installed on this machine");
        }
        // The complaint every prepress desk makes: the page says /F1 12 Tf, the
        // document names Helvetica, and no glyphs travel with the file.
        const QString source = path(u"missing-helvetica.pdf"_s);
        QVERIFY(writeDocumentWithFont(source, "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
                                      "BT /F1 24 Tf 72 700 Td (Grosse Freude) Tj ET\n"));

        const QString output = path(u"helvetica-embedded.pdf"_s);
        QVector<FontEmbedder::Substitution> substitutions;
        QString error;
        QVERIFY2(FontEmbedder::embedMissing(source, output, u"Liberation Sans"_s, &substitutions, &error),
                 qPrintable(error));
        QCOMPARE(substitutions.size(), 1);

        const FontEmbedder::Substitution &done = substitutions.constFirst();
        QCOMPARE(done.baseFont, u"Helvetica"_s);
        QCOMPARE(done.family, u"Liberation Sans"_s);
        QCOMPARE(done.resourceNames, QStringList { u"/F1"_s });
        QCOMPARE(done.pages, QVector<int>({ 0 }));
        QVERIFY(done.codes > 90);
        QVERIFY(done.embeddedBytes > 1000);

        // Liberation Sans is Helvetica's metrics under another name, so an
        // honest substitution has no gap to report at all. What this number
        // caught was a width invented from the average of a family and written
        // into the document as though it were a metric: half an em out on the
        // ellipsis, and reported as a substitution that did not fit.
        QVERIFY2(done.widthGap <= 10, qPrintable(QString::number(done.widthGap)));

        // The page was never rewritten. Same /Fx name, same codes, same bytes.
        QCOMPARE(ps::test::contentOf(output, 0), ps::test::contentOf(source, 0));

        QPDF pdf;
        PdfFile::open(pdf, output);
        QPDFObjectHandle font = embeddedFontOf(pdf, u"/F1"_s);
        QVERIFY(font.isDictionary());
        QCOMPARE(QString::fromStdString(font.getKey("/Subtype").getName()), u"/TrueType"_s);
        const QString baseFont = QString::fromStdString(font.getKey("/BaseFont").getName());
        QVERIFY2(QRegularExpression(u"^/[A-Z]{6}\\+LiberationSans$"_s).match(baseFont).hasMatch(),
                 qPrintable(baseFont));

        const QByteArray programme = programmeOf(font);
        QVERIFY(!programme.isEmpty());
        SfntReader face;
        QVERIFY(face.open(programme));
        QVERIFY2(face.checksumsAgree(), qPrintable(face.mismatch()));

        // Helvetica may state no widths, because every reader knows them, so
        // they had to be written out. Each one is the advance the embedded face
        // actually has, which is what "nothing moves" means when the
        // substitute is a metric twin, and is not what a family average gives.
        const QMap<int, QChar> mapping = toUnicodeOf(font);
        QVERIFY(mapping.size() > 90);
        const int first = font.getKey("/FirstChar").getIntValueAsInt();
        QPDFObjectHandle widths = font.getKey("/Widths");
        QCOMPARE(widths.getArrayNItems(), font.getKey("/LastChar").getIntValueAsInt() - first + 1);
        for (auto it = mapping.constBegin(); it != mapping.constEnd(); ++it) {
            const int glyph = face.glyphFor(it.value().unicode());
            QVERIFY2(glyph > 0, qPrintable(u"the subset cannot draw %1"_s.arg(it.value())));
            const int own = int(std::lround(face.advanceOf(glyph) * 1000.0 / face.unitsPerEm()));
            const int stated = widths.getArrayItem(it.key() - first).getIntValueAsInt();
            QVERIFY2(qAbs(stated - own) <= 10,
                     qPrintable(u"%1: the document now says %2, the face draws %3"_s.arg(it.value())
                                    .arg(stated)
                                    .arg(own)));
        }

        // The one that used to be wrong by name: an ellipsis is a full em wide
        // in this family, and the average of the family is half that.
        const int ellipsis = mapping.key(QChar(char16_t(0x2026)), -1);
        QVERIFY(ellipsis > 0);
        QVERIFY2(widths.getArrayItem(ellipsis - first).getIntValueAsInt() > 900,
                 "the ellipsis was given an average width instead of its own");

        // Asking again says so rather than writing the file over again.
        QVERIFY(!FontEmbedder::embedMissing(output, path(u"again-embedded.pdf"_s), QString(), nullptr, &error));
        QVERIFY(!error.isEmpty());
    }

    void aCompositeFontIsNamedAndLeftAloneRatherThanGuessedAt()
    {
        // Its codes are glyph numbers meaningful only inside the font programme
        // that is missing, so there is nothing to put another face behind.
        const QString source = path(u"composite.pdf"_s);
        QVERIFY(writeDocumentWithFont(
            source,
            "<< /Type /Font /Subtype /Type0 /BaseFont /MSMincho /Encoding /Identity-H"
            " /DescendantFonts [ << /Type /Font /Subtype /CIDFontType2 /BaseFont /MSMincho"
            " /CIDSystemInfo << /Registry (Adobe) /Ordering (Identity) /Supplement 0 >> /DW 1000 >> ] >>",
            "BT /F1 24 Tf 72 700 Td <00250026> Tj ET\n"));

        const QString output = path(u"composite-embedded.pdf"_s);
        QVector<FontEmbedder::Substitution> substitutions;
        QString error;
        QVERIFY(!FontEmbedder::embedMissing(source, output, QString(), &substitutions, &error));
        QVERIFY(!error.isEmpty());

        // Reported rather than passed over. A call that returned only its
        // successes would let a font it cannot touch pass for one it fixed.
        QCOMPARE(substitutions.size(), 1);
        QCOMPARE(substitutions.constFirst().baseFont, u"MSMincho"_s);
        QVERIFY(substitutions.constFirst().family.isEmpty());
        QCOMPARE(substitutions.constFirst().codes, 0);
        QCOMPARE(substitutions.constFirst().pages, QVector<int>({ 0 }));
        QVERIFY(!substitutions.constFirst().warnings.isEmpty());
        QVERIFY(allWarnings(substitutions).contains(u"MSMincho"_s));

        // And nothing was written, because a document that opens and shows the
        // wrong letters is worse than no document.
        QVERIFY(!QFileInfo::exists(output));
    }

    void replacingAFontKeepsEveryCodeAndEveryWidthTheDocumentStated()
    {
        if (!haveTestFont()) {
            QSKIP("DejaVu Sans is not installed on this machine");
        }
        const QString source = path(u"to-replace.pdf"_s);
        QVERIFY(writeDocumentWithFont(
            source,
            "<< /Type /Font /Subtype /TrueType /BaseFont /ABCDEF+LiberationSerif /FirstChar 65 /LastChar 67"
            " /Widths [ 611 722 500 ]"
            " /Encoding << /Type /Encoding /BaseEncoding /WinAnsiEncoding"
            " /Differences [ 65 /eacute /germandbls /adieresis ] >> >>",
            "BT /F1 24 Tf 72 700 Td <414243> Tj ET\n"));

        const QString output = path(u"replaced.pdf"_s);
        QVector<FontEmbedder::Substitution> substitutions;
        QString error;
        // Named with the spaces a person types and without the subset tag the
        // document carries, both of which have to reach the same font.
        QVERIFY2(FontEmbedder::replaceFont(source, output, u"Liberation Serif"_s, u"DejaVu Sans"_s, &substitutions,
                                           &error),
                 qPrintable(error));
        QCOMPARE(substitutions.size(), 1);
        QCOMPARE(substitutions.constFirst().baseFont, u"ABCDEF+LiberationSerif"_s);
        QCOMPARE(substitutions.constFirst().family, u"DejaVu Sans"_s);
        QCOMPARE(substitutions.constFirst().codes, 3);
        QVERIFY(substitutions.constFirst().embeddedBytes > 0);

        QCOMPARE(ps::test::contentOf(output, 0), ps::test::contentOf(source, 0));

        QPDF pdf;
        PdfFile::open(pdf, output);
        QPDFObjectHandle font = embeddedFontOf(pdf, u"/F1"_s);
        QVERIFY(QString::fromStdString(font.getKey("/BaseFont").getName()).contains(u"DejaVuSans"_s));
        QVERIFY(!programmeOf(font).isEmpty());

        // The three things that decide where a glyph sits, untouched.
        QCOMPARE(font.getKey("/FirstChar").getIntValueAsInt(), 65);
        QCOMPARE(font.getKey("/LastChar").getIntValueAsInt(), 67);
        QCOMPARE(font.getKey("/Widths").getArrayNItems(), 3);
        QCOMPARE(font.getKey("/Widths").getArrayItem(0).getIntValueAsInt(), 611);
        QCOMPARE(font.getKey("/Widths").getArrayItem(1).getIntValueAsInt(), 722);
        QCOMPARE(font.getKey("/Widths").getArrayItem(2).getIntValueAsInt(), 500);
        const QString encoding = QString::fromStdString(font.getKey("/Encoding").unparseResolved());
        QVERIFY2(encoding.contains(u"/eacute"_s), qPrintable(encoding));
        QVERIFY2(encoding.contains(u"/germandbls"_s), qPrintable(encoding));
        QVERIFY2(encoding.contains(u"/adieresis"_s), qPrintable(encoding));

        // And the codes still mean what they meant, now said out loud.
        const QMap<int, QChar> mapping = toUnicodeOf(font);
        QCOMPARE(mapping.value(65), QChar(u'é'));
        QCOMPARE(mapping.value(66), QChar(u'ß'));
        QCOMPARE(mapping.value(67), QChar(u'ä'));

        // A font this document has not got, refused with the name it does use.
        substitutions.clear();
        QVERIFY(!FontEmbedder::replaceFont(source, path(u"unmatched.pdf"_s), u"Garamond"_s, u"DejaVu Sans"_s,
                                           &substitutions, &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(substitutions.isEmpty());
        QVERIFY(!QFileInfo::exists(path(u"unmatched.pdf"_s)));

        // A family this system has not got: found, and still refused.
        substitutions.clear();
        QVERIFY(!FontEmbedder::replaceFont(source, path(u"uninstalled.pdf"_s), u"Liberation Serif"_s,
                                           u"Erfundene Schrift Ohne Datei 12345"_s, &substitutions, &error));
        QCOMPARE(substitutions.size(), 1);
        QVERIFY(substitutions.constFirst().family.isEmpty());
        QVERIFY(!substitutions.constFirst().warnings.isEmpty());
        QVERIFY(!QFileInfo::exists(path(u"uninstalled.pdf"_s)));

        QVERIFY(!FontEmbedder::replaceFont(source, path(u"nameless.pdf"_s), QString(), u"DejaVu Sans"_s, nullptr,
                                           &error));
        QVERIFY(!error.isEmpty());
    }

    void codesNamedByNumberAreReportedRatherThanQuietlyDropped()
    {
        if (!haveTestFont()) {
            QSKIP("DejaVu Sans is not installed on this machine");
        }
        // What a subsetter writes once it has stopped caring what its glyphs
        // draw. Three of these five codes can be carried across; two name a
        // glyph nobody can resolve, and dropping those without saying so is how
        // a substitution loses two letters in a paragraph nobody rereads.
        const QString source = path(u"named-by-number.pdf"_s);
        QVERIFY(writeDocumentWithFont(
            source,
            "<< /Type /Font /Subtype /TrueType /BaseFont /Fixture /FirstChar 65 /LastChar 69"
            " /Widths [ 600 600 600 600 600 ]"
            " /Encoding << /Type /Encoding /BaseEncoding /WinAnsiEncoding"
            " /Differences [ 65 /A /g3 /cid17 /D /E ] >> >>",
            "BT /F1 24 Tf 72 700 Td <4142434445> Tj ET\n"));

        QVector<FontEmbedder::Substitution> substitutions;
        QString error;
        QVERIFY2(FontEmbedder::embedMissing(source, path(u"named-embedded.pdf"_s), u"DejaVu Sans"_s, &substitutions,
                                            &error),
                 qPrintable(error));
        QCOMPARE(substitutions.size(), 1);
        QCOMPARE(substitutions.constFirst().codes, 3);

        // The names go into the sentence verbatim, so this holds whichever
        // language the sentence around them is written in.
        const QString said = allWarnings(substitutions);
        QVERIFY2(said.contains(u"/g3"_s), qPrintable(said));
        QVERIFY2(said.contains(u"/cid17"_s), qPrintable(said));

        // Where every code is named that way there is nothing left for a
        // substitute to mean, and the answer is a refusal rather than a page of
        // plausible wrong letters.
        const QString hopeless = path(u"all-by-number.pdf"_s);
        QVERIFY(writeDocumentWithFont(hopeless,
                                      "<< /Type /Font /Subtype /TrueType /BaseFont /Fixture /FirstChar 1 /LastChar 3"
                                      " /Widths [ 600 600 600 ]"
                                      " /Encoding << /Type /Encoding /Differences [ 1 /g3 /g5 /g7 ] >> >>",
                                      "BT /F1 24 Tf 72 700 Td <010203> Tj ET\n"));
        substitutions.clear();
        const QString refused = path(u"all-by-number-embedded.pdf"_s);
        QVERIFY(!FontEmbedder::embedMissing(hopeless, refused, u"DejaVu Sans"_s, &substitutions, &error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(substitutions.size(), 1);
        QVERIFY(substitutions.constFirst().family.isEmpty());
        QVERIFY(!substitutions.constFirst().warnings.isEmpty());
        QVERIFY(!QFileInfo::exists(refused));
    }

    void aProgrammeCanBeCutDownWithNoDocumentAroundIt()
    {
        if (!haveTestFont()) {
            QSKIP("DejaVu Sans is not installed on this machine");
        }
        FontEmbedder::Request request;
        request.characters = charactersOf(u"a"_s);
        QFile file(FontEmbedder::locate(request));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray whole = file.readAll();
        QVERIFY(whole.size() > 100000);

        int before = 0;
        int after = 0;
        QString error;
        const QByteArray cut = FontEmbedder::subsetProgramme(whole, charactersOf(u"ä"_s), &before, &after, &error);
        QVERIFY2(!cut.isEmpty(), qPrintable(error));
        QVERIFY(before > 1000);
        QVERIFY(after > 1 && after < 20);
        QVERIFY(cut.size() < whole.size() / 20);

        SfntReader face;
        QVERIFY(face.open(cut));
        QVERIFY2(face.checksumsAgree(), qPrintable(face.mismatch()));
        QVERIFY2(face.fileChecksumAgrees(), "the whole-file checksum in head is wrong");
        QCOMPARE(face.numGlyphs(), after);

        const int glyph = face.glyphFor(u'ä');
        QVERIFY(glyph > 0 && glyph < face.numGlyphs());
        const QByteArray outline = face.outlineOf(glyph);
        QVERIFY2(!outline.isEmpty(), "the glyph has no outline at all");
        for (int component : face.componentsOf(outline)) {
            // The same trap as in a document, one level down: an "ä" built out
            // of an "a" and a diaeresis draws nothing at all when its parts were
            // left behind, in a font file that parses perfectly.
            QVERIFY2(component > 0 && component < face.numGlyphs(), "a component points outside the subset");
            QVERIFY2(!face.outlineOf(component).isEmpty(), "a component of the composite was left behind");
        }

        // Refused rather than answered with an empty font: characters this
        // programme says nothing about, and bytes that are not a font at all.
        QString whyNot;
        QVERIFY(FontEmbedder::subsetProgramme(cut, { QChar(char16_t(0x4E00)) }, nullptr, nullptr, &whyNot).isEmpty());
        QVERIFY(!whyNot.isEmpty());

        whyNot.clear();
        QVERIFY(FontEmbedder::subsetProgramme(QByteArrayLiteral("not a font at all"), charactersOf(u"a"_s), nullptr,
                                              nullptr, &whyNot)
                    .isEmpty());
        QVERIFY(!whyNot.isEmpty());
    }

    void embeddingFromFileToFileGivesEveryPageItsOwnCopy()
    {
        if (!haveTestFont()) {
            QSKIP("DejaVu Sans is not installed on this machine");
        }
        const QString source = path(u"two-pages.pdf"_s);
        QVERIFY(ps::test::writeSamplePdf(source, 2));

        FontEmbedder::Request request;
        request.characters = charactersOf(u"Müller"_s);
        const QString output = path(u"two-pages-embedded.pdf"_s);
        QVector<FontEmbedder::Result> results;
        QString error;
        QVERIFY2(FontEmbedder::embed(source, output, request, {}, &results, &error), qPrintable(error));

        // One result per page and in page order, because a page's resources are
        // its own: adding the font once to a dictionary two pages share would
        // put it on a page nobody asked about.
        QCOMPARE(results.size(), 2);
        QCOMPARE(results.at(0).resourceName, results.at(1).resourceName);
        QCOMPARE(results.at(0).embeddedBytes, results.at(1).embeddedBytes);
        QVERIFY(results.at(0).glyphCount >= charactersOf(u"Müller"_s).size());

        const QVector<FontUse> uses = FontInventory::read(output, &error);
        int found = 0;
        for (const FontUse &use : uses) {
            if (!use.family.contains(u"DejaVu"_s)) {
                continue;
            }
            ++found;
            QVERIFY(use.embedded);
            QVERIFY(use.subset);
            QCOMPARE(use.pages.size(), 1);
            QVERIFY(use.coverage.contains(QChar(u'ü')));
        }
        QCOMPARE(found, 2);

        // A page this document has not got is refused before anything is
        // written, rather than after most of it is.
        QVERIFY(!FontEmbedder::embed(source, path(u"no-such-page.pdf"_s), request, { 7 }, nullptr, &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(!QFileInfo::exists(path(u"no-such-page.pdf"_s)));

        QVERIFY(!FontEmbedder::embed(source, path(u"no-characters.pdf"_s), FontEmbedder::Request {}, {}, nullptr,
                                     &error));
        QVERIFY(!error.isEmpty());
    }

    void readingARealMagazineFindsItsSubsetFonts()
    {
        const QString magazine = qEnvironmentVariable("PS_STRESS_PDF");
        if (!QFileInfo::exists(magazine)) {
            QSKIP("set PS_STRESS_PDF to a large real document to run this");
        }

        QString error;
        const QVector<FontUse> uses = FontInventory::read(magazine, &error);
        QVERIFY2(!uses.isEmpty(), qPrintable(error));
        QVERIFY2(uses.size() >= 5, qPrintable(QString::number(uses.size())));

        int subsets = 0;
        int copyable = 0;
        int embedded = 0;
        for (const FontUse &use : uses) {
            QVERIFY(!use.baseFont.isEmpty());
            QVERIFY(!use.subtype.isEmpty());
            QVERIFY(!use.pages.isEmpty());
            QVERIFY(use.pages.first() >= 0);
            if (use.subset) {
                ++subsets;
                // A subset nobody carries with them is a document that cannot be
                // printed anywhere else; producers do not do that.
                QVERIFY2(use.embedded, qPrintable(use.baseFont));
                QVERIFY(use.baseFont.mid(6, 1) == u"+"_s);
                QVERIFY(use.family != use.baseFont);
            }
            if (use.embedded) {
                ++embedded;
                QVERIFY2(use.fileSize > 0, qPrintable(use.baseFont));
            }
            if (use.hasToUnicode) {
                ++copyable;
                QVERIFY(!use.coverage.isEmpty());
            }
        }
        QVERIFY(subsets >= 3);
        QVERIFY(embedded >= subsets);
        QVERIFY(copyable >= 1);
    }

    void bothClassesSayWhatTheyCannotDo()
    {
        // Not the wording, which is translated, but that the promise exists.
        QVERIFY(FontEmbedder::limitations().size() >= 4);
        QVERIFY(FontInventory::limitations().size() >= 4);
        for (const QString &note : FontEmbedder::limitations() + FontInventory::limitations()) {
            QVERIFY(note.size() > 20);
        }
    }
};

QTEST_MAIN(TestFonts)

#include "tst_fonts.moc"
