/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "Commands.h"

#include "core/FontEmbedder.h"
#include "core/FontInventory.h"
#include "core/PageRange.h"
#include "core/PdfFile.h"

#include <KLocalizedString>

#include <QCommandLineParser>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>
#include <QVector>

#include <qpdf/QPDF.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>
#include <qpdf/QPDFWriter.hh>

#include <algorithm>

using namespace Qt::Literals::StringLiterals;

namespace ps::cli {

namespace {

// ── Small shared pieces ───────────────────────────────────────────────────

QString clip(const QString &text, int width)
{
    return text.size() <= width ? text : text.left(width - 1) + u'…';
}

/**
 * One row of the listing.
 *
 * Header and data both come through here, so a column cannot be widened in one
 * place and left alone in the other. A negative width means the cell is a
 * number and is set to the right; a zero width means the last column, which
 * runs to the end of the line.
 */
QString row(const QStringList &cells)
{
    static const int widths[] = { 7, 29, 9, 4, 4, 5, 17, -5, -8, 0 };
    static const int columns = int(sizeof(widths) / sizeof(widths[0]));

    QString line;
    for (int i = 0; i < cells.size() && i < columns; ++i) {
        const int width = widths[i];
        if (width == 0) {
            line += cells.at(i);
        } else if (width < 0) {
            line += cells.at(i).rightJustified(-width) + u"  "_s;
        } else {
            line += clip(cells.at(i), width - 1).leftJustified(width);
        }
    }
    return line;
}

QString tick(bool yes)
{
    return yes ? u"✓"_s : u"✗"_s;
}

/** "/TrueType" and "/WinAnsiEncoding" read better in a column without the slash. */
QString bare(const QString &name)
{
    return name.startsWith(u'/') ? name.mid(1) : name;
}

/** Zero-based page indexes as a person writes them: "1-3, 7". */
QString pageList(const QVector<int> &pages)
{
    QVector<int> sorted = pages;
    std::sort(sorted.begin(), sorted.end());

    QStringList parts;
    for (int at = 0; at < sorted.size();) {
        int end = at;
        while (end + 1 < sorted.size() && sorted.at(end + 1) == sorted.at(end) + 1) {
            ++end;
        }
        parts << (end == at ? QString::number(sorted.at(at) + 1)
                            : u"%1-%2"_s.arg(sorted.at(at) + 1).arg(sorted.at(end) + 1));
        at = end + 1;
    }
    return parts.join(u", "_s);
}

/** The coverage set in a fixed order, so two runs of --json agree. */
QString sortedCoverage(const QSet<QChar> &coverage)
{
    QVector<QChar> characters(coverage.constBegin(), coverage.constEnd());
    std::sort(characters.begin(), characters.end(),
              [](QChar left, QChar right) { return left.unicode() < right.unicode(); });
    QString text;
    text.reserve(characters.size());
    for (const QChar &character : std::as_const(characters)) {
        text.append(character);
    }
    return text;
}

/** "bold", "italic", "bold-italic", and the German words, as the other commands take. */
bool applyStyle(const QString &text, ps::FontEmbedder::Request *request)
{
    if (text.trimmed().isEmpty()) {
        return true;
    }
    QString normalised = text.toLower();
    normalised.replace(u' ', u'-');

    const QStringList words = normalised.split(u'-', Qt::SkipEmptyParts);
    for (const QString &word : words) {
        if (word == u"regular"_s || word == u"normal"_s) {
            continue;
        }
        if (word == u"bold"_s || word == u"fett"_s) {
            request->bold = true;
        } else if (word == u"italic"_s || word == u"oblique"_s || word == u"kursiv"_s) {
            request->italic = true;
        } else {
            return false;
        }
    }
    return true;
}

QJsonObject asJson(const ps::FontUse &use)
{
    QJsonArray pages;
    for (const int page : use.pages) {
        pages.append(page + 1);
    }
    return QJsonObject { { u"resource"_s, use.resourceName },
                         { u"baseFont"_s, use.baseFont },
                         { u"family"_s, use.family },
                         { u"type"_s, use.subtype },
                         { u"embedded"_s, use.embedded },
                         { u"subset"_s, use.subset },
                         { u"toUnicode"_s, use.hasToUnicode },
                         { u"encoding"_s, use.encoding },
                         { u"bytes"_s, use.fileSize },
                         { u"licence"_s, use.licence },
                         { u"characterCount"_s, use.coverage.size() },
                         { u"coverage"_s, sortedCoverage(use.coverage) },
                         { u"pages"_s, pages } };
}

/** How a problem names the font it is about. */
QString describe(const ps::FontUse &use)
{
    return i18ncp("@item a font, the name a page calls it by, and where it is used", "%2 (%3, page %4)",
                  "%2 (%3, pages %4)", use.pages.size(),
                  use.baseFont.isEmpty() ? i18nc("@item a font with no /BaseFont", "unnamed") : use.baseFont,
                  use.resourceName, pageList(use.pages));
}

// ── The verbs ─────────────────────────────────────────────────────────────

int listFonts(const QStringList &arguments, const QCommandLineParser &parser)
{
    QString input;
    int code = 0;
    if (!oneInputFile(arguments, u"fonts list"_s, &input, &code)) {
        return code;
    }

    QString error;
    const QVector<ps::FontUse> uses = ps::FontInventory::read(input, &error);
    if (!error.isEmpty()) {
        return fail(error);
    }

    if (parser.isSet(u"json"_s)) {
        QJsonArray array;
        for (const ps::FontUse &use : uses) {
            array.append(asJson(use));
        }
        out() << QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Indented));
        return Success;
    }

    if (uses.isEmpty()) {
        out() << i18n("No page in this document uses a font. A scan is all picture, and has none until its text "
                      "has been recognised.")
              << Qt::endl;
        return Success;
    }

    out() << row({ i18nc("@title:column the /Fx name a page calls a font by", "Name"), i18nc("@title:column", "Font"),
                   i18nc("@title:column what kind of font", "Type"), i18nc("@title:column short for embedded", "Emb"),
                   i18nc("@title:column short for subset", "Sub"),
                   i18nc("@title:column short for “has a /ToUnicode map”", "Uni"), i18nc("@title:column", "Encoding"),
                   i18nc("@title:column how many characters can be addressed", "Chars"),
                   i18nc("@title:column size of the embedded font programme", "Size"),
                   i18nc("@title:column", "Pages") })
          << Qt::endl;

    int embedded = 0;
    int noUnicode = 0;
    qint64 total = 0;
    for (const ps::FontUse &use : uses) {
        embedded += use.embedded ? 1 : 0;
        noUnicode += use.hasToUnicode ? 0 : 1;
        total += use.fileSize;
        out() << row(
            { use.resourceName, use.baseFont, bare(use.subtype), tick(use.embedded), tick(use.subset),
              tick(use.hasToUnicode),
              use.encoding.isEmpty() ? i18nc("@item the font names no encoding of its own", "none")
                                     : bare(use.encoding),
              QString::number(use.coverage.size()),
              use.fileSize > 0 ? humanSize(use.fileSize) : i18nc("@item no font file, so no size to give", "none"),
              pageList(use.pages) })
              << Qt::endl;
    }

    out() << Qt::endl;
    out() << i18nc("@info legend for the three tick columns",
                   "Emb = the glyphs travel with the document · Sub = only part of the font is here · "
                   "Uni = its text can be copied")
          << Qt::endl;

    QStringList licences;
    for (const ps::FontUse &use : uses) {
        if (!use.licence.isEmpty()) {
            licences << i18nc("@item a font and what its licence bits say", "%1: %2", use.family, use.licence);
        }
    }
    licences.removeDuplicates();
    if (!licences.isEmpty()) {
        out() << Qt::endl << i18nc("@title heading before a list", "Licences:") << Qt::endl;
        for (const QString &note : std::as_const(licences)) {
            out() << u"  · "_s << note << Qt::endl;
        }
    }

    out() << Qt::endl;
    out() << i18np("%1 font.", "%1 fonts.", uses.size()) << Qt::endl;
    if (embedded < uses.size()) {
        out() << i18np("%1 of them is not embedded, so it depends on the reader having that face.",
                       "%1 of them are not embedded, so they depend on the reader having those faces.",
                       uses.size() - embedded)
              << Qt::endl;
    }
    if (noUnicode > 0) {
        out() << i18np("%1 has no /ToUnicode map, so its text cannot be copied out or searched.",
                       "%1 have no /ToUnicode map, so their text cannot be copied out or searched.", noUnicode)
              << Qt::endl;
    }
    if (total > 0) {
        out() << i18n("The embedded font programmes come to %1.", humanSize(total)) << Qt::endl;
    }
    return Success;
}

int checkFonts(const QStringList &arguments, const QCommandLineParser &parser)
{
    QString input;
    int code = 0;
    if (!oneInputFile(arguments, u"fonts check"_s, &input, &code)) {
        return code;
    }

    QString error;
    const QVector<ps::FontUse> uses = ps::FontInventory::read(input, &error);
    if (!error.isEmpty()) {
        return fail(error);
    }

    // Every problem twice over: a sentence for a person, and beside it a token
    // that does not follow the interface language, for the script that gates on
    // the exit code and then wants to know which of the two things it was.
    QStringList sentences;
    QJsonArray problems;
    for (const ps::FontUse &use : uses) {
        if (!use.embedded) {
            sentences << i18nc("@info",
                               "%1 is not embedded, so the reader will substitute another face and the "
                               "lines will shift.",
                               describe(use));
            problems.append(QJsonObject {
                { u"font"_s, use.baseFont }, { u"resource"_s, use.resourceName }, { u"problem"_s, u"notEmbedded"_s } });
        }
        if (!use.hasToUnicode) {
            sentences << i18nc("@info",
                               "%1 has no /ToUnicode map, so its text cannot be copied, searched or read "
                               "aloud.",
                               describe(use));
            problems.append(QJsonObject {
                { u"font"_s, use.baseFont }, { u"resource"_s, use.resourceName }, { u"problem"_s, u"noToUnicode"_s } });
        }
        if (use.coverage.isEmpty()) {
            sentences << i18nc("@info", "%1 says nothing anywhere about what its character codes mean.", describe(use));
            problems.append(QJsonObject {
                { u"font"_s, use.baseFont }, { u"resource"_s, use.resourceName }, { u"problem"_s, u"noCoverage"_s } });
        }
    }

    if (parser.isSet(u"json"_s)) {
        const QJsonObject root { { u"ok"_s, problems.isEmpty() },
                                 { u"fontCount"_s, uses.size() },
                                 { u"problems"_s, problems } };
        out() << QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Indented));
        return problems.isEmpty() ? Success : DifferencesFound;
    }

    for (const QString &sentence : std::as_const(sentences)) {
        out() << u"  · "_s << sentence << Qt::endl;
    }
    out() << (problems.isEmpty() ? i18n("Nothing wrong with the fonts in this document.")
                                 : i18np("%1 thing to put right.", "%1 things to put right.", problems.size()))
          << Qt::endl;

    // Printed, not optional: the exit code below would otherwise read as a
    // verdict from a font validator, which this is not.
    out() << Qt::endl;
    const QStringList limits = ps::FontInventory::limitations();
    for (const QString &limit : limits) {
        out() << u"  "_s << limit << Qt::endl;
    }

    return problems.isEmpty() ? Success : DifferencesFound;
}

/**
 * Adds a system font to a document.
 *
 * The only verb here that does its own QPDF work. FontEmbedder::embed takes a
 * page's resource dictionary rather than a file name, and deliberately so: the
 * graphical side adds a font in the middle of an edit it is already holding
 * open, so opening the file and writing it out again has to happen somewhere,
 * and on the command line that is here.
 */
int embedFont(const QStringList &arguments, const QCommandLineParser &parser)
{
    QString input;
    int code = 0;
    if (!oneInputFile(arguments, u"fonts embed"_s, &input, &code)) {
        return code;
    }

    ps::FontEmbedder::Request request;
    request.family = parser.value(u"font"_s);
    if (request.family.isEmpty()) {
        return fail(i18n("fonts embed needs a font to embed, as --font “DejaVu Sans”."), UsageError);
    }
    if (!applyStyle(parser.value(u"font-style"_s), &request)) {
        return fail(i18n("“%1” is not a cut. Use bold, italic, bold-italic or regular.", parser.value(u"font-style"_s)),
                    UsageError);
    }

    const QString characters = parser.value(u"text"_s);
    if (characters.isEmpty()) {
        return fail(i18n("fonts embed needs the characters to put in, as --text “Müller”. Only those go in, which "
                         "is what keeps the document from growing by a whole font."),
                    UsageError);
    }
    for (const QChar &character : characters) {
        request.characters.insert(character);
    }

    if (!ps::FontEmbedder::isAvailable(request)) {
        return fail(i18n("No font called “%1” that could be embedded is installed on this system. “fonts find” "
                         "says what is there.",
                         request.family));
    }

    const QString output = outputOr(parser.value(u"output"_s), input, u"-embedded.pdf"_s);

    QString error;
    try {
        QPDF pdf;
        ps::PdfFile::open(pdf, input);

        QPDFPageDocumentHelper documents(pdf);
        std::vector<QPDFPageObjectHelper> pages = documents.getAllPages();

        const QVector<int> wanted = ps::PageRange::parse(parser.value(u"pages"_s), int(pages.size()), &error);
        if (wanted.isEmpty()) {
            return fail(error, UsageError);
        }

        for (const int page : wanted) {
            // Copied if shared: pages very often inherit one resource dictionary
            // from the page tree, and adding a font to that would quietly add it
            // to every page in the document.
            QPDFObjectHandle resources = pages.at(size_t(page)).getAttribute("/Resources", true);

            ps::FontEmbedder::Result result;
            if (!ps::FontEmbedder::embed(pdf, resources, request, &result, &error)) {
                return fail(error);
            }
            out() << i18nc("@info page, font, the /Fx name it got, glyph count and size",
                           "Page %1: %2 added as %3, %4 glyphs, %5", page + 1, request.family, result.resourceName,
                           result.glyphCount, humanSize(result.embeddedBytes))
                  << Qt::endl;
            for (const QString &warning : std::as_const(result.warnings)) {
                err() << i18n("Note: %1", warning) << Qt::endl;
            }
        }

        QPDFWriter writer(pdf);
        writer.setOutputFilename(QFile::encodeName(output).constData());
        writer.setObjectStreamMode(qpdf_o_generate);
        writer.setStreamDataMode(qpdf_s_compress);
        writer.write();
    } catch (const std::exception &e) {
        return fail(QString::fromUtf8(e.what()), OutputError);
    }

    out() << i18n("Wrote %1.", output) << Qt::endl;
    out() << i18n("The text already on those pages still uses the fonts it always did. The new one is a resource "
                  "to write with, not a replacement for what is there.")
          << Qt::endl;
    return Success;
}

int stripFonts(const QStringList &arguments, const QCommandLineParser &parser)
{
    QString input;
    int code = 0;
    if (!oneInputFile(arguments, u"fonts strip"_s, &input, &code)) {
        return code;
    }

    const QStringList names = parser.values(u"font-name"_s);
    if (names.isEmpty() && !parser.isSet(u"all"_s)) {
        return fail(i18n("fonts strip needs to know which fonts, as --font-name Helvetica, or --all for every "
                         "embedded one."),
                    UsageError);
    }

    const QString output = outputOr(parser.value(u"output"_s), input, u"-unembedded.pdf"_s);
    int removed = 0;
    QString error;
    if (!ps::FontInventory::removeEmbedding(input, output, names, &removed, &error)) {
        return fail(error, OutputError);
    }

    out() << i18np("Threw the glyphs of %1 font away.", "Threw the glyphs of %1 fonts away.", removed) << Qt::endl;
    out() << i18n("%1 → %2", humanSize(QFileInfo(input).size()), humanSize(QFileInfo(output).size())) << Qt::endl;
    out() << i18n("The text is still there and still selectable. What it looks like is now up to whatever face the "
                  "reader puts in its place.")
          << Qt::endl;
    return Success;
}

int mergeFonts(const QStringList &arguments, const QCommandLineParser &parser)
{
    QString input;
    int code = 0;
    if (!oneInputFile(arguments, u"fonts merge"_s, &input, &code)) {
        return code;
    }

    const QString output = outputOr(parser.value(u"output"_s), input, u"-fonts-merged.pdf"_s);
    int merged = 0;
    QString error;
    if (!ps::FontInventory::mergeSubsets(input, output, &merged, &error)) {
        return fail(error, OutputError);
    }

    out() << i18np("Reduced %1 duplicate font to the one the pages now share.",
                   "Reduced %1 duplicate fonts to the ones the pages now share.", merged)
          << Qt::endl;
    out() << i18n("%1 → %2", humanSize(QFileInfo(input).size()), humanSize(QFileInfo(output).size())) << Qt::endl;
    return Success;
}

int unicodeFonts(const QStringList &arguments, const QCommandLineParser &parser)
{
    QString input;
    int code = 0;
    if (!oneInputFile(arguments, u"fonts unicode"_s, &input, &code)) {
        return code;
    }

    const QString output = outputOr(parser.value(u"output"_s), input, u"-copyable.pdf"_s);
    int added = 0;
    QString error;
    if (!ps::FontInventory::addToUnicode(input, output, &added, &error)) {
        return fail(error, OutputError);
    }

    out() << i18np("Wrote a /ToUnicode map for %1 font.", "Wrote a /ToUnicode map for %1 fonts.", added) << Qt::endl;
    out() << i18n("Their text can be copied and searched now, by this program and by any other.") << Qt::endl;
    return Success;
}

int findFont(const QStringList &arguments, const QCommandLineParser &parser)
{
    if (arguments.size() != 1) {
        return fail(i18n("fonts find needs one family name, as: fonts find “DejaVu Sans”."), UsageError);
    }

    ps::FontEmbedder::Request request;
    request.family = arguments.constFirst();
    if (!applyStyle(parser.value(u"font-style"_s), &request)) {
        return fail(i18n("“%1” is not a cut. Use bold, italic, bold-italic or regular.", parser.value(u"font-style"_s)),
                    UsageError);
    }

    const QString path = ps::FontEmbedder::locate(request);
    if (path.isEmpty()) {
        return fail(i18n("No font called “%1” is installed on this system.", request.family));
    }

    // The path alone on standard output, so a script can hand it straight on.
    out() << path << Qt::endl;

    if (!ps::FontEmbedder::isAvailable(request)) {
        return fail(i18n("That file was found, but it is not a TrueType or OpenType font, and only those can be "
                         "cut down and embedded."));
    }
    return Success;
}

int fontLimits()
{
    out() << i18nc("@title heading before a list", "Reading and repairing what a document carries:") << Qt::endl;
    const QStringList inventory = ps::FontInventory::limitations();
    for (const QString &limit : inventory) {
        out() << u"  · "_s << limit << Qt::endl;
    }

    out() << Qt::endl << i18nc("@title heading before a list", "Embedding a font from this system:") << Qt::endl;
    const QStringList embedder = ps::FontEmbedder::limitations();
    for (const QString &limit : embedder) {
        out() << u"  · "_s << limit << Qt::endl;
    }
    return Success;
}

// ── Wiring ────────────────────────────────────────────────────────────────

void fontOptions(QCommandLineParser &parser)
{
    parser.addOptions({
        { u"font"_s, i18n("Which font to embed, by family name, e.g. “DejaVu Sans”."), u"family"_s },
        { u"font-name"_s, i18n("Which font in the document to act on, by its /BaseFont name. Repeat for more."),
          u"name"_s },
        { u"font-style"_s, i18n("Which cut of the family: bold, italic, bold-italic or regular."), u"cut"_s },
        { u"all"_s, i18n("Act on every embedded font, not a named one.") },
    });
}

QStringList fontHelp()
{
    const auto line = [](const QString &verb, const QString &takes, const QString &what) {
        return u"  "_s + verb.leftJustified(15) + takes.leftJustified(21) + what;
    };
    return {
        line(u"fonts list"_s, u"FILE"_s, i18n("Every font, where it is used and what is wrong with it")),
        line(u"fonts check"_s, u"FILE"_s, i18n("Exit non-zero when a font is missing or unreadable")),
        line(u"fonts embed"_s, u"FILE -o OUT"_s, i18n("Add a system font, cut down to the characters asked for")),
        line(u"fonts strip"_s, u"FILE -o OUT"_s, i18n("Throw embedded glyphs away and keep the text")),
        line(u"fonts merge"_s, u"FILE -o OUT"_s, i18n("Join duplicate subsets of the same font into one")),
        line(u"fonts unicode"_s, u"FILE -o OUT"_s, i18n("Write /ToUnicode maps so the text can be copied")),
        line(u"fonts find"_s, u"FAMILY"_s, i18n("Where this system keeps a font, if it has it")),
        line(u"fonts limits"_s, QString(), i18n("What the font tools cannot promise")),
    };
}

int unknownAction(const QString &action)
{
    // Named on purpose rather than swept into "unknown": both are the first
    // thing people reach for, and "no such command" would leave them looking
    // for a spelling mistake instead of reading the reason.
    if (action == u"subset"_s || action == u"replace"_s) {
        return fail(i18n("Cutting an embedded font down to the glyphs a page uses, and swapping one font for "
                         "another, both mean rewriting every character code on every page that refers to it, and "
                         "a wrong answer there is a page of wrong letters. Neither is done here. “fonts merge” "
                         "joins duplicate subsets, and “fonts strip” drops the glyphs and keeps the text."),
                    UsageError);
    }
    return fail(i18n("“%1” is not something fonts can do. Try list, check, embed, strip, merge, unicode, find or "
                     "limits.",
                     action),
                UsageError);
}

std::optional<int> fontRun(const QString &verb, const QStringList &arguments, const QCommandLineParser &parser)
{
    if (verb != u"fonts"_s) {
        return std::nullopt;
    }

    QStringList rest = arguments;
    const QString action = rest.isEmpty() ? QString() : rest.takeFirst().toLower();

    if (action.isEmpty()) {
        err() << i18n("fonts needs to be told what to do:") << Qt::endl;
        const QStringList help = fontHelp();
        for (const QString &entry : help) {
            err() << entry << Qt::endl;
        }
        return int(UsageError);
    }
    if (action == u"list"_s) {
        return listFonts(rest, parser);
    }
    if (action == u"check"_s) {
        return checkFonts(rest, parser);
    }
    if (action == u"embed"_s) {
        return embedFont(rest, parser);
    }
    if (action == u"strip"_s) {
        return stripFonts(rest, parser);
    }
    if (action == u"merge"_s) {
        return mergeFonts(rest, parser);
    }
    if (action == u"unicode"_s) {
        return unicodeFonts(rest, parser);
    }
    if (action == u"find"_s) {
        return findFont(rest, parser);
    }
    if (action == u"limits"_s || action == u"limitations"_s) {
        return fontLimits();
    }
    return unknownAction(action);
}

} // namespace

Group fontGroup()
{
    return { fontOptions, fontRun, fontHelp };
}

} // namespace ps::cli
