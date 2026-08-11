/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later

    The pictures inside a document, rather than pictures made out of it.

    “export-images” renders whole pages to files; everything here works on the
    image XObjects themselves and addresses them the way a content stream does,
    by page number and resource name. The number every verb is built around is
    the effective resolution, because that is what decides whether a file will
    print, whether it is worth re-encoding, and how much there is to gain.
*/
#include "Commands.h"

#include "core/Document.h"
#include "core/ImageEdit.h"
#include "core/PageRange.h"
#include "render/PopplerBackend.h"

#include <KLocalizedString>

#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QImageWriter>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QTemporaryDir>
#include <QTextStream>
#include <QTransform>

#include <algorithm>
#include <cmath>

using namespace Qt::Literals::StringLiterals;

namespace ps::cli {
namespace {

/** Above this a picture costs bytes that no printer and no screen ever shows. */
constexpr int lavishDpi = 300;

// ── the arguments these verbs share ──────────────────────────────────────

/** The slash a content stream writes, whether or not the user typed it. */
QString slashed(const QString &name)
{
    return name.startsWith(u'/') ? name : u'/' + name;
}

/** --page, one-based on the command line and zero-based everywhere else. */
bool onePage(const QCommandLineParser &parser, int *page, int *code)
{
    // --page carries a default of 1 for the text commands, so an unset option
    // is indistinguishable from “page 1” by value alone. Silently editing the
    // first page of a two-hundred-page book is not a mistake worth allowing.
    if (!parser.isSet(u"page"_s)) {
        *code = fail(i18n("Which page? Give --page, counting from 1."), UsageError);
        return false;
    }
    bool ok = false;
    const int number = parser.value(u"page"_s).toInt(&ok);
    if (!ok || number < 1) {
        *code = fail(i18n("“%1” is not a page number.", parser.value(u"page"_s)), UsageError);
        return false;
    }
    *page = number - 1;
    return true;
}

/** --resource, the /Im0 that “images list” prints. */
bool oneName(const QCommandLineParser &parser, QString *name, int *code)
{
    const QString given = parser.value(u"resource"_s).trimmed();
    if (given.isEmpty()) {
        *code = fail(i18n("Which picture? Give --resource, as “images list” shows it."), UsageError);
        return false;
    }
    *name = slashed(given);
    return true;
}

/** Where to write, never over the file being read. */
bool destination(const QCommandLineParser &parser, const QString &input, const QString &suffix, QString *output,
                 int *code)
{
    const QString target = outputOr(parser.value(u"output"_s), input, suffix);
    if (QFileInfo(target).absoluteFilePath() == QFileInfo(input).absoluteFilePath()) {
        *code = fail(i18n("That would write over the original. Give -o another name."), UsageError);
        return false;
    }
    *output = target;
    return true;
}

/** Reads a numeric option, saying which one when it is not a number. */
bool number(const QCommandLineParser &parser, const QString &option, double *value, int *code)
{
    bool ok = false;
    const double parsed = parser.value(option).toDouble(&ok);
    if (!ok) {
        *code = fail(i18n("“%1” is not a number for --%2.", parser.value(option), option), UsageError);
        return false;
    }
    *value = parsed;
    return true;
}

struct PageChoice {
    bool all = true;
    QSet<int> chosen;

    bool wants(int page) const { return all || chosen.contains(page); }
};

/** --pages against this document; unset means every page. */
bool readPageChoice(const QString &input, const QCommandLineParser &parser, PageChoice *choice, int *code)
{
    const QString range = parser.value(u"pages"_s).trimmed();
    if (range.isEmpty()) {
        return true;
    }

    ps::PopplerBackend backend;
    ps::Document document;
    document.setRenderBackend(&backend);
    QString error;
    if (!document.open(input, &error)) {
        *code = fail(error);
        return false;
    }
    const QVector<int> pages = ps::PageRange::parse(range, document.pageCount(), &error);
    if (pages.isEmpty()) {
        *code = fail(error, UsageError);
        return false;
    }
    choice->all = false;
    choice->chosen = QSet<int>(pages.cbegin(), pages.cend());
    return true;
}

/** The pages and names recompress() takes, read in step. */
struct Chosen {
    QVector<int> pages;
    QStringList names;
    int skipped = 0; //!< pictures on those pages whose pixels cannot be opened
};

/**
 * Turns a page range into the pair of lists recompress() wants.
 *
 * Both lists empty is its whole-document mode, where a picture it cannot open
 * is skipped instead of failing the run. A range is the same kind of request
 * (“these pages”, not “this picture”), so anything unopenable is dropped here
 * for the same reason, and counted so the report can say so.
 */
bool picturesIn(const QString &input, const PageChoice &choice, Chosen *chosen, int *code)
{
    if (choice.all) {
        return true;
    }

    QString error;
    const QVector<ps::ImageEdit::ImageUse> uses = ps::ImageEdit::read(input, &error);
    if (!error.isEmpty()) {
        *code = fail(error);
        return false;
    }
    for (const ps::ImageEdit::ImageUse &use : uses) {
        if (!choice.wants(use.page)) {
            continue;
        }
        if (use.isMask || !use.decodable) {
            ++chosen->skipped;
            continue;
        }
        chosen->pages.append(use.page);
        chosen->names.append(use.resourceName);
    }
    if (chosen->pages.isEmpty()) {
        *code = fail(i18n("Those pages hold no picture whose pixels this tool can open."));
        return false;
    }
    return true;
}

/** Before and after, because the size is what the request was about. */
void reportSizes(const QString &input, const QString &output)
{
    const qint64 before = QFileInfo(input).size();
    const qint64 after = QFileInfo(output).size();
    if (before > 0 && after > 0 && after < before) {
        out() << i18n("%1 → %2  (%3% smaller)", humanSize(before), humanSize(after),
                      QString::number((before - after) * 100 / before))
              << Qt::endl;
    } else if (before > 0 && after > 0) {
        out() << i18n("%1 → %2", humanSize(before), humanSize(after)) << Qt::endl;
    }
    out() << i18nc("@info where the result was written", "Wrote %1.", output) << Qt::endl;
}

// ── list ─────────────────────────────────────────────────────────────────

/** The resolution as placed, as one number when both axes agree. */
QString dpiText(const ps::ImageEdit::ImageUse &use)
{
    const int across = int(std::lround(use.effectiveDpiX));
    const int down = int(std::lround(use.effectiveDpiY));
    if (across <= 0 || down <= 0) {
        return i18nc("@item effective resolution that could not be worked out", "unknown");
    }
    // Only worth two numbers when the placement genuinely stretched the pixels.
    if (std::abs(across - down) * 20 > std::max(across, down)) {
        return u"%1×%2"_s.arg(across).arg(down);
    }
    return QString::number(across);
}

/** What is unusual about this picture, in a word each. */
QStringList notesFor(const ps::ImageEdit::ImageUse &use)
{
    QStringList notes;
    if (use.hasAlpha) {
        notes += i18nc("@item this picture carries a soft mask, so parts of it are transparent", "soft mask");
    }
    if (use.isMask) {
        notes += i18nc("@item a stencil that takes its colour from the page", "stencil");
    }
    if (!use.decodable) {
        notes += i18nc("@item the pixels are stored in a form this tool cannot open", "cannot open");
    }
    if (use.drawnTimes > 1) {
        notes += i18ncp("@item how often the same picture is drawn in the document", "drawn %1 time", "drawn %1 times",
                        use.drawnTimes);
    }
    return notes;
}

int listPictures(const QString &input, const QCommandLineParser &parser)
{
    PageChoice choice;
    int code = Success;
    if (!readPageChoice(input, parser, &choice, &code)) {
        return code;
    }

    QString error;
    QVector<ps::ImageEdit::ImageUse> uses = ps::ImageEdit::read(input, &error);
    if (!error.isEmpty()) {
        return fail(error);
    }
    uses.erase(std::remove_if(uses.begin(), uses.end(),
                              [&choice](const ps::ImageEdit::ImageUse &use) { return !choice.wants(use.page); }),
               uses.end());

    if (parser.isSet(u"json"_s)) {
        QJsonArray array;
        for (const ps::ImageEdit::ImageUse &use : uses) {
            array.append(QJsonObject {
                { u"page"_s, use.page + 1 },
                { u"name"_s, use.resourceName },
                { u"widthPx"_s, use.pixelSize.width() },
                { u"heightPx"_s, use.pixelSize.height() },
                // The two numbers the rest of the object exists to explain.
                { u"dpiX"_s, std::round(use.effectiveDpiX * 10.0) / 10.0 },
                { u"dpiY"_s, std::round(use.effectiveDpiY * 10.0) / 10.0 },
                { u"xPt"_s, use.placement.x() },
                { u"yPt"_s, use.placement.y() },
                { u"widthPt"_s, use.placement.width() },
                { u"heightPt"_s, use.placement.height() },
                { u"encoding"_s, use.encoding },
                { u"colourSpace"_s, use.colourSpace },
                { u"bitsPerComponent"_s, use.bitsPerComponent },
                { u"hasAlpha"_s, use.hasAlpha },
                { u"isMask"_s, use.isMask },
                { u"storedBytes"_s, use.storedBytes },
                { u"decodable"_s, use.decodable },
                { u"drawnTimes"_s, use.drawnTimes },
            });
        }
        out() << QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Indented));
        return Success;
    }

    if (uses.isEmpty()) {
        out() << i18n("No page of this document draws a picture.") << Qt::endl;
        return Success;
    }

    // dpi sits next to the name rather than after the pixel count: it is the
    // answer, and the columns behind it are the working.
    const QStringList headers { i18nc("@title:column page number", "Page"),
                                i18nc("@title:column the /Im0 name a content stream uses", "Name"),
                                i18nc("@title:column effective resolution, pixels per inch as placed", "dpi"),
                                i18nc("@title:column the stored pixel size", "Pixels"),
                                i18nc("@title:column how large it is drawn", "On page"),
                                i18nc("@title:column bytes it occupies in the file", "Stored"),
                                i18nc("@title:column how the pixels are stored", "Format") };
    static constexpr bool rightAligned[] = { true, false, true, false, false, true, false };

    QVector<QStringList> rows;
    qint64 total = 0;
    qint64 lavishBytes = 0;
    int lavishCount = 0;
    for (const ps::ImageEdit::ImageUse &use : uses) {
        total += use.storedBytes;
        if (std::max(use.effectiveDpiX, use.effectiveDpiY) > double(lavishDpi)) {
            lavishBytes += use.storedBytes;
            ++lavishCount;
        }
        rows.append({ QString::number(use.page + 1), use.resourceName, dpiText(use),
                      u"%1×%2"_s.arg(use.pixelSize.width()).arg(use.pixelSize.height()),
                      i18nc("@item size on the page, in millimetres", "%1×%2 mm",
                            QString::number(use.placement.width() * 25.4 / 72.0, 'f', 0),
                            QString::number(use.placement.height() * 25.4 / 72.0, 'f', 0)),
                      humanSize(use.storedBytes),
                      u"%1 %2 %3"_s.arg(use.encoding, use.colourSpace, QString::number(use.bitsPerComponent)) });
    }

    QVector<int> widths;
    for (qsizetype column = 0; column < headers.size(); ++column) {
        int width = int(headers.at(column).size());
        for (const QStringList &row : rows) {
            width = std::max(width, int(row.at(column).size()));
        }
        widths.append(width);
    }

    const auto printRow = [&widths](const QStringList &cells, const QString &trailing) {
        QStringList padded;
        for (qsizetype column = 0; column < cells.size(); ++column) {
            padded += rightAligned[column] ? cells.at(column).rightJustified(widths.at(column))
                                           : cells.at(column).leftJustified(widths.at(column));
        }
        // Only the padding at the end goes: trimming the front as well would
        // pull a short page number out from under its own column.
        QString rendered = padded.join(u"  "_s);
        while (rendered.endsWith(u' ')) {
            rendered.chop(1);
        }
        out() << rendered << trailing << Qt::endl;
    };

    printRow(headers, QString());
    for (qsizetype i = 0; i < rows.size(); ++i) {
        const QStringList notes = notesFor(uses.at(i));
        printRow(rows.at(i), notes.isEmpty() ? QString() : u"  "_s + notes.join(u", "_s));
    }

    out() << Qt::endl;
    out() << i18np("%1 picture, %2 in all.", "%1 pictures, %2 in all.", int(uses.size()), humanSize(total)) << Qt::endl;
    if (lavishCount > 0) {
        out() << i18np("%1 of them sits above %2 dpi and holds %3, more than any printer will show.",
                       "%1 of them sit above %2 dpi and hold %3, more than any printer will show.", lavishCount,
                       lavishDpi, humanSize(lavishBytes))
              << Qt::endl;
        out() << i18n("“images resample %1 --max-dpi 150 -o smaller.pdf” brings them down.",
                      QFileInfo(input).fileName())
              << Qt::endl;
    }
    return Success;
}

// ── compress and resample ────────────────────────────────────────────────

/** --format, as far as the engine can honour it. */
bool readEncoding(const QCommandLineParser &parser, bool *lossless, int *code)
{
    if (!parser.isSet(u"format"_s)) {
        return true; // JPEG, which is what re-encoding a photograph means
    }
    const QString wanted = parser.value(u"format"_s).trimmed().toLower();
    if (wanted == u"jpeg"_s || wanted == u"jpg"_s) {
        *lossless = false;
        return true;
    }
    if (wanted == u"keep"_s || wanted == u"lossless"_s || wanted == u"png"_s) {
        *lossless = true;
        return true;
    }
    if (wanted == u"jpeg2000"_s || wanted == u"jp2"_s) {
        // Reading JPEG 2000 is already beyond this engine; writing it would
        // leave a file half of which it could no longer open.
        *code = fail(i18n("JPEG 2000 cannot be written. Use --format jpeg, or --format keep to re-pack the pixels "
                          "without any loss at all."),
                     UsageError);
        return false;
    }
    *code = fail(i18n("“%1” is not a picture format here. Use jpeg or keep.", parser.value(u"format"_s)), UsageError);
    return false;
}

/** The shared half of compress and resample: both are recompress() underneath. */
int recompressPictures(const QString &input, const QString &output, const Chosen &chosen,
                       const ps::ImageEdit::Recompress &options, const QString &hint)
{
    qint64 saved = 0;
    QString error;
    if (!ps::ImageEdit::recompress(input, output, chosen.pages, chosen.names, options, &saved, &error)) {
        return fail(error, OutputError);
    }

    if (chosen.skipped > 0) {
        out() << i18np("Left %1 picture alone: its pixels are stored in a form this cannot open.",
                       "Left %1 pictures alone: their pixels are stored in a form this cannot open.", chosen.skipped)
              << Qt::endl;
    }

    if (saved <= 0) {
        // Nothing was re-encoded, and saying so beats a "0 % smaller" that
        // reads like a failure. The output is still written: it is the same
        // document, packed by the writer.
        out() << i18n("No picture needed re-encoding, so %1 is a copy of the original.", output) << Qt::endl;
        if (!hint.isEmpty()) {
            out() << hint << Qt::endl;
        }
        return Success;
    }

    out() << i18n("%1 left the picture streams.", humanSize(saved)) << Qt::endl;
    reportSizes(input, output);
    return Success;
}

int compressPictures(const QString &input, const QCommandLineParser &parser)
{
    int code = Success;
    QString output;
    if (!destination(parser, input, u"-smaller.pdf"_s, &output, &code)) {
        return code;
    }

    ps::ImageEdit::Recompress options;
    if (!readEncoding(parser, &options.lossless, &code)) {
        return code;
    }
    // --dpi carries 150 of its own, which is also the right default here: a
    // quality-only re-encode is refused by the engine on purpose, so a
    // resolution is what gives “compress” something to do.
    double dpi = 150.0;
    if (!number(parser, u"dpi"_s, &dpi, &code)) {
        return code;
    }
    double quality = 85.0;
    if (!number(parser, u"quality"_s, &quality, &code)) {
        return code;
    }
    options.targetDpi = std::max(0.0, dpi);
    options.jpegQuality = std::clamp(int(std::lround(quality)), 1, 100);
    options.toGrayscale = parser.isSet(u"grayscale"_s);

    PageChoice choice;
    if (!readPageChoice(input, parser, &choice, &code)) {
        return code;
    }
    Chosen chosen;
    if (!picturesIn(input, choice, &chosen, &code)) {
        return code;
    }

    if (options.lossless) {
        out() << i18n("Re-packing at up to %1 dpi, with no loss of quality at all.",
                      QString::number(options.targetDpi, 'f', 0))
              << Qt::endl;
    } else {
        out() << i18n("Re-encoding at up to %1 dpi, quality %2.", QString::number(options.targetDpi, 'f', 0),
                      options.jpegQuality)
              << Qt::endl;
    }
    return recompressPictures(input, output, chosen, options,
                              i18n("A lower --dpi or --quality would give it something to work with."));
}

int resamplePictures(const QString &input, const QCommandLineParser &parser)
{
    int code = Success;
    if (!parser.isSet(u"max-dpi"_s)) {
        return fail(i18n("How fine should the pictures still be? Give --max-dpi, for example 150 for reading on "
                         "screen or 300 for printing."),
                    UsageError);
    }
    double maxDpi = 0.0;
    if (!number(parser, u"max-dpi"_s, &maxDpi, &code)) {
        return code;
    }
    if (maxDpi <= 0.0) {
        return fail(i18n("--max-dpi has to be more than zero."), UsageError);
    }

    QString output;
    if (!destination(parser, input, u"-resampled.pdf"_s, &output, &code)) {
        return code;
    }

    ps::ImageEdit::Recompress options;
    options.targetDpi = maxDpi;
    if (!readEncoding(parser, &options.lossless, &code)) {
        return code;
    }
    double quality = 85.0;
    if (!number(parser, u"quality"_s, &quality, &code)) {
        return code;
    }
    options.jpegQuality = std::clamp(int(std::lround(quality)), 1, 100);
    options.toGrayscale = parser.isSet(u"grayscale"_s);

    PageChoice choice;
    if (!readPageChoice(input, parser, &choice, &code)) {
        return code;
    }
    Chosen chosen;
    if (!picturesIn(input, choice, &chosen, &code)) {
        return code;
    }

    return recompressPictures(input, output, chosen, options,
                              i18n("Every picture is already at or below %1 dpi.", QString::number(maxDpi, 'f', 0)));
}

// ── replace, rotate, remove ──────────────────────────────────────────────

int replacePicture(const QString &input, const QCommandLineParser &parser)
{
    int code = Success;
    int page = 0;
    QString name;
    if (!onePage(parser, &page, &code) || !oneName(parser, &name, &code)) {
        return code;
    }
    const QString source = parser.value(u"with"_s);
    if (source.isEmpty()) {
        return fail(i18n("Which picture should go in? Give --with a file."), UsageError);
    }
    const QImage replacement(source);
    if (replacement.isNull()) {
        return fail(i18n("“%1” could not be read as a picture.", source));
    }

    QString output;
    if (!destination(parser, input, u"-edited.pdf"_s, &output, &code)) {
        return code;
    }

    QString error;
    if (!ps::ImageEdit::replace(input, output, page, name, replacement, !parser.isSet(u"stretch"_s), &error)) {
        return fail(error, OutputError);
    }

    out() << i18n("Page %1 now draws %2, in the place the old picture had.", page + 1, name) << Qt::endl;
    if (parser.isSet(u"stretch"_s)) {
        out() << i18n("It was stretched to fill that place exactly.") << Qt::endl;
    }
    reportSizes(input, output);
    return Success;
}

int rotatePicture(const QString &input, const QCommandLineParser &parser)
{
    int code = Success;
    int page = 0;
    QString name;
    if (!onePage(parser, &page, &code) || !oneName(parser, &name, &code)) {
        return code;
    }
    if (!parser.isSet(u"angle"_s)) {
        return fail(i18n("By how much? Give --angle 90, or any number of degrees."), UsageError);
    }
    bool ok = false;
    const double given = parser.value(u"angle"_s).toDouble(&ok);
    if (!ok) {
        return fail(i18n("“%1” is not an angle in degrees.", parser.value(u"angle"_s)), UsageError);
    }

    QString output;
    if (!destination(parser, input, u"-edited.pdf"_s, &output, &code)) {
        return code;
    }

    // A PDF image is put on the page by a matrix, so a turn belongs in that
    // matrix and costs nothing: the stored stream comes out with the bytes it
    // went in with, whatever it is, and any angle works, not only quarter
    // turns, and not only pictures this tool can decode.
    ps::ImageEdit::RotateReport report;
    QString error;
    if (!ps::ImageEdit::rotate(input, output, page, name, given, &report, &error)) {
        return fail(error, OutputError);
    }

    out() << i18np("Turned %2 on page %3 by %4°, in one place.", "Turned %2 on page %3 by %4°, in %1 places.",
                   report.placementsTurned, name, page + 1, given)
          << Qt::endl;
    if (report.pixelsUntouched) {
        out() << i18n("Not one stored byte of the picture was rewritten.") << Qt::endl;
    }
    for (const QString &note : std::as_const(report.notes)) {
        err() << i18nc("@info a caveat on the command line", "Note: ") << note << Qt::endl;
    }
    reportSizes(input, output);
    return Success;
}

int removePicture(const QString &input, const QCommandLineParser &parser)
{
    int code = Success;
    int page = 0;
    QString name;
    if (!onePage(parser, &page, &code) || !oneName(parser, &name, &code)) {
        return code;
    }
    QString output;
    if (!destination(parser, input, u"-edited.pdf"_s, &output, &code)) {
        return code;
    }

    QString error;
    if (!ps::ImageEdit::remove(input, output, page, name, &error)) {
        return fail(error, OutputError);
    }

    out() << i18n("Page %1 no longer draws %2.", page + 1, name) << Qt::endl;
    reportSizes(input, output);
    return Success;
}

// ── adjust and crop ──────────────────────────────────────────────────────

int adjustPicture(const QString &input, const QCommandLineParser &parser)
{
    int code = Success;
    int page = 0;
    QString name;
    if (!onePage(parser, &page, &code) || !oneName(parser, &name, &code)) {
        return code;
    }

    ps::ImageEdit::Adjust options;
    if (parser.isSet(u"brightness"_s) && !number(parser, u"brightness"_s, &options.brightness, &code)) {
        return code;
    }
    if (parser.isSet(u"contrast"_s) && !number(parser, u"contrast"_s, &options.contrast, &code)) {
        return code;
    }
    if (parser.isSet(u"gamma"_s) && !number(parser, u"gamma"_s, &options.gamma, &code)) {
        return code;
    }
    if (options.brightness < -1.0 || options.brightness > 1.0 || options.contrast < -1.0 || options.contrast > 1.0) {
        return fail(i18n("--brightness and --contrast run from -1 to 1."), UsageError);
    }
    if (options.gamma <= 0.0) {
        return fail(i18n("--gamma has to be more than zero. Above 1 lifts the midtones, below 1 lowers them."),
                    UsageError);
    }
    options.sharpen = parser.isSet(u"sharpen"_s);
    options.despeckle = parser.isSet(u"despeckle"_s);
    options.autoLevels = parser.isSet(u"auto-levels"_s);
    options.toGrayscale = parser.isSet(u"grayscale"_s);
    options.toBlackWhite = parser.isSet(u"black-white"_s);
    if (parser.isSet(u"bw-threshold"_s)) {
        double threshold = 128.0;
        if (!number(parser, u"bw-threshold"_s, &threshold, &code)) {
            return code;
        }
        options.threshold = std::clamp(int(std::lround(threshold)), 0, 255);
    }

    const bool somethingToDo = options.sharpen || options.despeckle || options.autoLevels || options.toGrayscale
        || options.toBlackWhite || !qFuzzyIsNull(options.brightness) || !qFuzzyIsNull(options.contrast)
        || !qFuzzyCompare(options.gamma, 1.0);
    if (!somethingToDo) {
        return fail(i18n("Nothing to adjust. Try --brightness, --contrast, --gamma, --sharpen, --despeckle, "
                         "--auto-levels, --grayscale or --black-white."),
                    UsageError);
    }

    QString output;
    if (!destination(parser, input, u"-edited.pdf"_s, &output, &code)) {
        return code;
    }

    QString error;
    if (!ps::ImageEdit::adjust(input, output, page, name, options, &error)) {
        return fail(error, OutputError);
    }

    out() << i18n("Worked over the pixels of %1 on page %2; it keeps its place and its size.", name, page + 1)
          << Qt::endl;
    // Sharpening a scan can leave it larger than it was, so the two sizes are
    // worth printing even when this is not a shrinking operation.
    reportSizes(input, output);
    return Success;
}

int cropPicture(const QString &input, const QCommandLineParser &parser)
{
    int code = Success;
    int page = 0;
    QString name;
    if (!onePage(parser, &page, &code) || !oneName(parser, &name, &code)) {
        return code;
    }

    const QString box = parser.value(u"keep-box"_s).trimmed();
    if (box.isEmpty()) {
        return fail(i18n("Which part should stay? Give --keep-box X,Y,WIDTH,HEIGHT as fractions of the picture, "
                         "measured from its top-left corner, for example 0,0,1,0.5 for the upper half."),
                    UsageError);
    }
    const QStringList parts = box.split(u',', Qt::SkipEmptyParts);
    double edges[4] = { 0.0, 0.0, 0.0, 0.0 };
    bool ok = parts.size() == 4;
    for (qsizetype i = 0; ok && i < parts.size(); ++i) {
        edges[i] = parts.at(i).trimmed().toDouble(&ok);
    }
    if (!ok) {
        return fail(i18n("“%1” is not an area. The form is X,Y,WIDTH,HEIGHT as fractions, e.g. 0.1,0,0.8,1.", box),
                    UsageError);
    }
    // A hair over one is what a hand-typed 0.333,0,0.334,1 comes to; a long
    // way over it means someone gave points or pixels instead of fractions.
    if (edges[2] <= 0.0 || edges[3] <= 0.0 || edges[0] < 0.0 || edges[1] < 0.0 || edges[0] + edges[2] > 1.001
        || edges[1] + edges[3] > 1.001) {
        return fail(i18n("The area has to lie inside the picture, as fractions between 0 and 1."), UsageError);
    }

    QString output;
    if (!destination(parser, input, u"-edited.pdf"_s, &output, &code)) {
        return code;
    }

    QString error;
    if (!ps::ImageEdit::crop(input, output, page, name, QRectF(edges[0], edges[1], edges[2], edges[3]), &error)) {
        return fail(error, OutputError);
    }

    out() << i18n("Cropped %1 on page %2. The part cut away has left the file.", name, page + 1) << Qt::endl;
    reportSizes(input, output);
    return Success;
}

// ── extract ──────────────────────────────────────────────────────────────

/** A resource name a file system will accept, e.g. /Im0 as Im0. */
QString fileStem(const QString &resourceName)
{
    QString stem;
    for (const QChar letter : resourceName) {
        stem += letter.isLetterOrNumber() || letter == u'-' || letter == u'_' ? letter : u'_';
    }
    while (stem.startsWith(u'_')) {
        stem.remove(0, 1);
    }
    return stem.isEmpty() ? u"image"_s : stem;
}

int extractPictures(const QString &input, const QCommandLineParser &parser)
{
    int code = Success;
    const QString outDir = parser.value(u"out-dir"_s);
    const QString single = parser.value(u"output"_s);
    const bool named = !parser.value(u"resource"_s).trimmed().isEmpty();
    if (outDir.isEmpty() && !(named && !single.isEmpty())) {
        return fail(i18n("Where should the pictures go? Give --out-dir a folder."), UsageError);
    }
    if (!outDir.isEmpty() && !QDir(outDir).exists() && !QDir().mkpath(outDir)) {
        return fail(i18n("“%1” could not be created.", outDir), OutputError);
    }

    // --format keeps its own default of png, which is right for anything that
    // has to be decoded. Left unset, a stored JPEG is asked for as .jpg
    // instead, because that extraction is a byte-for-byte copy and loses
    // nothing at all.
    QString suffix;
    if (parser.isSet(u"format"_s)) {
        suffix = parser.value(u"format"_s).trimmed().toLower();
        if (!QImageWriter::supportedImageFormats().contains(suffix.toLatin1())) {
            return fail(i18n("“%1” is not a picture format this can write. Try png, jpg, tif or webp.", suffix),
                        UsageError);
        }
    }

    QString error;
    const QVector<ps::ImageEdit::ImageUse> uses = ps::ImageEdit::read(input, &error);
    if (!error.isEmpty()) {
        return fail(error);
    }

    const QString base = QFileInfo(input).completeBaseName();
    const auto suffixFor = [&suffix](const ps::ImageEdit::ImageUse &use) {
        return !suffix.isEmpty() ? suffix : (use.encoding == u"DCTDecode"_s ? u"jpg"_s : u"png"_s);
    };

    if (named) {
        int page = 0;
        QString name;
        if (!onePage(parser, &page, &code) || !oneName(parser, &name, &code)) {
            return code;
        }
        QString target = single;
        if (target.isEmpty()) {
            const auto found = std::find_if(uses.cbegin(), uses.cend(), [page, &name](const auto &use) {
                return use.page == page && use.resourceName == name;
            });
            const QString ending = found != uses.cend() ? suffixFor(*found) : (suffix.isEmpty() ? u"png"_s : suffix);
            target
                = QDir(outDir).filePath(u"%1-p%2-%3.%4"_s.arg(base, QString::number(page + 1), fileStem(name), ending));
        }
        if (!ps::ImageEdit::extract(input, page, name, target, &error)) {
            return fail(error, OutputError);
        }
        out() << target << Qt::endl;
        out() << i18nc("@info", "Wrote one picture, %1.", humanSize(QFileInfo(target).size())) << Qt::endl;
        return Success;
    }

    PageChoice choice;
    if (!readPageChoice(input, parser, &choice, &code)) {
        return code;
    }

    int written = 0;
    int skipped = 0;
    qint64 bytes = 0;
    for (const ps::ImageEdit::ImageUse &use : uses) {
        if (!choice.wants(use.page)) {
            continue;
        }
        if (use.isMask || !use.decodable) {
            // Named on its own it would earn a refusal by name; in a sweep it
            // is a note at the end, so one fax does not stop the other twelve.
            ++skipped;
            continue;
        }
        const QString target = QDir(outDir).filePath(
            u"%1-p%2-%3.%4"_s.arg(base, QString::number(use.page + 1), fileStem(use.resourceName), suffixFor(use)));
        if (!ps::ImageEdit::extract(input, use.page, use.resourceName, target, &error)) {
            err() << i18n("Note: %1", error) << Qt::endl;
            ++skipped;
            continue;
        }
        bytes += QFileInfo(target).size();
        ++written;
        out() << target << Qt::endl;
    }

    if (written == 0 && skipped == 0) {
        out() << i18n("No page of this document draws a picture.") << Qt::endl;
        return Success;
    }
    out() << i18ncp("@info", "Wrote %1 picture, %2 in all.", "Wrote %1 pictures, %2 in all.", written, humanSize(bytes))
          << Qt::endl;
    if (skipped > 0) {
        out() << i18np("Left %1 picture in the document: its pixels are stored in a form this cannot open.",
                       "Left %1 pictures in the document: their pixels are stored in a form this cannot open.", skipped)
              << Qt::endl;
    }
    return Success;
}

// ── deduplicate ──────────────────────────────────────────────────────────

int dedupePictures(const QString &input, const QCommandLineParser &parser)
{
    int code = Success;
    QString output;
    if (!destination(parser, input, u"-deduped.pdf"_s, &output, &code)) {
        return code;
    }

    int merged = 0;
    qint64 saved = 0;
    QString error;
    if (!ps::ImageEdit::deduplicate(input, output, &merged, &saved, &error)) {
        return fail(error, OutputError);
    }

    if (merged == 0) {
        out() << i18n("No picture is stored more than once, so %1 is a copy of the original.", output) << Qt::endl;
        return Success;
    }
    out() << i18np("Merged %1 duplicate into the picture it repeated, freeing %2.",
                   "Merged %1 duplicates into the pictures they repeated, freeing %2.", merged, humanSize(saved))
          << Qt::endl;
    reportSizes(input, output);
    return Success;
}

// ── the group ────────────────────────────────────────────────────────────

void imageOptions(QCommandLineParser &parser)
{
    const QCommandLineOption nameOption(u"resource"_s, i18n("Which picture, by the resource name “images list” shows."),
                                        u"name"_s);
    const QCommandLineOption withOption(u"with"_s, i18n("The picture file to put in instead."), u"file"_s);
    const QCommandLineOption maxDpiOption(u"max-dpi"_s, i18n("Bring anything finer than this down to it, e.g. 150."),
                                          u"number"_s);
    const QCommandLineOption stretchOption(
        u"stretch"_s, i18n("Fill the old place exactly instead of fitting the picture inside it."));
    const QCommandLineOption keepBoxOption(
        u"keep-box"_s,
        i18n("The part of a picture to keep, as X,Y,WIDTH,HEIGHT in fractions from its top-left corner."), u"box"_s);
    const QCommandLineOption brightnessOption(u"brightness"_s, i18n("Brightness, -1 to 1."), u"amount"_s, u"0"_s);
    const QCommandLineOption contrastOption(u"contrast"_s, i18n("Contrast, -1 to 1."), u"amount"_s, u"0"_s);
    const QCommandLineOption gammaOption(u"gamma"_s, i18n("Gamma; above 1 lifts the midtones, below 1 lowers them."),
                                         u"amount"_s, u"1"_s);
    const QCommandLineOption sharpenOption(u"sharpen"_s, i18n("Sharpen the picture."));
    const QCommandLineOption despeckleOption(u"despeckle"_s, i18n("Take the specks out of a scan."));
    const QCommandLineOption autoLevelsOption(u"auto-levels"_s, i18n("Spread the tones over the full range."));
    const QCommandLineOption blackWhiteOption(u"black-white"_s, i18n("Reduce the picture to black and white."));
    const QCommandLineOption thresholdOption(u"bw-threshold"_s, i18n("Where black turns into white, 0 to 255."),
                                             u"level"_s, u"128"_s);

    parser.addOptions({ nameOption, withOption, maxDpiOption, stretchOption, keepBoxOption, brightnessOption,
                        contrastOption, gammaOption, sharpenOption, despeckleOption, autoLevelsOption, blackWhiteOption,
                        thresholdOption });
}

std::optional<int> runImages(const QString &verb, const QStringList &arguments, const QCommandLineParser &parser)
{
    if (verb != u"images"_s) {
        return std::nullopt;
    }

    QStringList rest = arguments;
    const QString action = rest.isEmpty() ? QString() : rest.takeFirst().toLower();
    const QString actions = u"list, compress, resample, replace, rotate, remove, extract, adjust, crop, dedupe, "
                            "limitations"_s;
    if (action.isEmpty()) {
        return fail(i18n("images needs to know what to do: %1.", actions), UsageError);
    }

    // Nothing to open: this one answers out of the engine itself.
    if (action == u"limitations"_s || action == u"limits"_s) {
        for (const QString &note : ps::ImageEdit::limitations()) {
            out() << u"• "_s << note << Qt::endl;
        }
        return Success;
    }

    QString input;
    int code = Success;
    if (!oneInputFile(rest, u"images %1"_s.arg(action), &input, &code)) {
        return code;
    }

    if (action == u"list"_s) {
        return listPictures(input, parser);
    }
    if (action == u"compress"_s) {
        return compressPictures(input, parser);
    }
    if (action == u"resample"_s) {
        return resamplePictures(input, parser);
    }
    if (action == u"replace"_s) {
        return replacePicture(input, parser);
    }
    if (action == u"rotate"_s) {
        return rotatePicture(input, parser);
    }
    if (action == u"remove"_s) {
        return removePicture(input, parser);
    }
    if (action == u"extract"_s) {
        return extractPictures(input, parser);
    }
    if (action == u"adjust"_s) {
        return adjustPicture(input, parser);
    }
    if (action == u"crop"_s) {
        return cropPicture(input, parser);
    }
    if (action == u"dedupe"_s || action == u"deduplicate"_s) {
        return dedupePictures(input, parser);
    }

    return fail(i18n("“images %1” is not something this can do. Try %2.", action, actions), UsageError);
}

QStringList imageHelp()
{
    const auto line
        = [](const QString &usage, const QString &what) { return u"  "_s + usage.leftJustified(36) + what; };
    return {
        line(u"images list FILE"_s, i18n("Every picture, with the resolution it really has")),
        line(u"images compress FILE [-o OUT]"_s, i18n("Re-encode the pictures, smaller")),
        line(u"images resample FILE --max-dpi N"_s, i18n("Bring anything finer than that down to it")),
        line(u"images replace FILE --with PIC"_s, i18n("Put another picture in the same place")),
        line(u"images rotate FILE --angle 90"_s, i18n("Turn one picture, without re-encoding it")),
        line(u"images remove FILE --resource RES"_s, i18n("Stop a page drawing one")),
        line(u"images extract FILE --out-dir DIR"_s, i18n("Write the stored pictures out as they are")),
        line(u"images adjust FILE --resource RES"_s, i18n("Brightness, contrast, gamma, sharpen, despeckle")),
        line(u"images crop FILE --keep-box BOX"_s, i18n("Cut pixels off one picture for good")),
        line(u"images dedupe FILE [-o OUT]"_s, i18n("Merge pictures the file stores twice")),
        line(u"images limitations"_s, i18n("What working on pictures cannot promise")),
    };
}

} // namespace

Group imageGroup()
{
    return { imageOptions, runImages, imageHelp };
}

} // namespace ps::cli
