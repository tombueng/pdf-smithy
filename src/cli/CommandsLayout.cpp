/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later

    Page geometry on the command line: the five boxes, paper size, printer's
    marks, imposition and the numbers a reader shows in its page box.

    One verb, "layout", with the job as the next word. The area has fifteen
    operations and the top level already has thirty-odd verbs; putting them all
    there would have made "pdf-smithy-cli --help" unreadable for the sake of
    saving one word.
*/
#include "Commands.h"

#include "core/PageLayout.h"
#include "core/PageRange.h"

#include <QCommandLineParser>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QTextStream>

#include <KLocalizedString>

namespace ps::cli {
namespace {

constexpr double kPointsPerMillimetre = 72.0 / 25.4;
constexpr double kPointsPerInch = 72.0;

/*
 * Numbers on a command line are C-locale numbers, in both directions.
 *
 * The process puts LC_NUMERIC into C so that QPDF's strtod reads the document
 * right; QString::toDouble() and QString::number() ignore the system locale
 * anyway. So "210x297mm" means the same in Bern as in Boston, and a figure this
 * prints can be pasted straight into the next command. Nothing here may go
 * through QLocale; commandCrop parses and prints the same way, deliberately.
 */

/** A length with an optional unit: "5", "5mm", "0.5cm", "0.25in", "18pt". */
bool parseLength(const QString &text, double bareUnitPoints, double *points)
{
    QString number = text.trimmed().toLower();
    double unit = bareUnitPoints;
    if (number.endsWith(QLatin1String("mm"))) {
        unit = kPointsPerMillimetre;
        number.chop(2);
    } else if (number.endsWith(QLatin1String("cm"))) {
        unit = 10.0 * kPointsPerMillimetre;
        number.chop(2);
    } else if (number.endsWith(QLatin1String("in"))) {
        unit = kPointsPerInch;
        number.chop(2);
    } else if (number.endsWith(QLatin1String("pt"))) {
        unit = 1.0;
        number.chop(2);
    }

    bool ok = false;
    const double value = number.trimmed().toDouble(&ok);
    if (!ok) {
        return false;
    }
    *points = value * unit;
    return true;
}

/** A named paper size, or WIDTHxHEIGHT with one unit for the pair. */
bool parsePaperSize(const QString &text, QSizeF *size)
{
    struct Paper {
        const char *name;
        double widthMm;
        double heightMm;
    };
    // Held in millimetres and converted, so A4 comes out 595.276 × 841.89 rather
    // than the rounded 595 × 842 that leaves a hairline of the old page showing.
    static constexpr Paper papers[] = {
        { "a0", 841.0, 1189.0 },        { "a1", 594.0, 841.0 },    { "a2", 420.0, 594.0 },
        { "a3", 297.0, 420.0 },         { "a4", 210.0, 297.0 },    { "a5", 148.0, 210.0 },
        { "a6", 105.0, 148.0 },         { "b4", 250.0, 353.0 },    { "b5", 176.0, 250.0 },
        { "letter", 215.9, 279.4 },     { "legal", 215.9, 355.6 }, { "tabloid", 279.4, 431.8 },
        { "executive", 184.15, 266.7 },
    };

    QString body = text.trimmed().toLower();
    for (const Paper &paper : papers) {
        if (body == QLatin1String(paper.name)) {
            *size = QSizeF(paper.widthMm * kPointsPerMillimetre, paper.heightMm * kPointsPerMillimetre);
            return true;
        }
    }

    // The unit is written once, after the pair: "210x297mm", "8.5x11in". It is
    // required, and no longer guessed. A bare pair used to be points here (the
    // unit the document's own boxes are in) and millimetres to "typeset", so
    // "--size 210x297" asked for A4 in one command and for a page a third that
    // size in the other, with nothing on screen to say which had happened.
    double unit = 0.0;
    if (body.endsWith(QLatin1String("mm"))) {
        unit = kPointsPerMillimetre;
        body.chop(2);
    } else if (body.endsWith(QLatin1String("cm"))) {
        unit = 10.0 * kPointsPerMillimetre;
        body.chop(2);
    } else if (body.endsWith(QLatin1String("in"))) {
        unit = kPointsPerInch;
        body.chop(2);
    } else if (body.endsWith(QLatin1String("pt"))) {
        unit = 1.0;
        body.chop(2);
    }
    if (unit <= 0.0) {
        return false;
    }

    const qsizetype cross = body.indexOf(QLatin1Char('x'));
    if (cross <= 0) {
        return false;
    }
    bool okWidth = false;
    bool okHeight = false;
    const double width = body.left(cross).trimmed().toDouble(&okWidth);
    const double height = body.mid(cross + 1).trimmed().toDouble(&okHeight);
    if (!okWidth || !okHeight || width <= 0.0 || height <= 0.0) {
        return false;
    }
    *size = QSizeF(width * unit, height * unit);
    return true;
}

bool parseFit(const QString &text, PageLayout::Fit *fit)
{
    const QString wanted = text.trimmed().toLower();
    if (wanted == QLatin1String("scale")) {
        *fit = PageLayout::Fit::Scale;
        return true;
    }
    if (wanted == QLatin1String("centre") || wanted == QLatin1String("center")) {
        *fit = PageLayout::Fit::Centre;
        return true;
    }
    if (wanted == QLatin1String("shrink")) {
        *fit = PageLayout::Fit::ScaleAndCentre;
        return true;
    }
    return false;
}

/** "X,Y,WIDTH,HEIGHT" in points from the bottom-left corner, or "none" for no box at all. */
bool parseBox(const QString &text, QRectF *rect)
{
    const QString wanted = text.trimmed();
    if (wanted.compare(QLatin1String("none"), Qt::CaseInsensitive) == 0) {
        *rect = QRectF(); // invalid, which is how PageLayout spells "take it away"
        return true;
    }

    const QStringList numbers = wanted.split(QLatin1Char(','));
    if (numbers.size() != 4) {
        return false;
    }
    double values[4] = { 0.0, 0.0, 0.0, 0.0 };
    for (int i = 0; i < 4; ++i) {
        bool ok = false;
        values[i] = numbers.at(i).trimmed().toDouble(&ok);
        if (!ok) {
            return false;
        }
    }
    if (values[2] <= 0.0 || values[3] <= 0.0) {
        return false;
    }
    *rect = QRectF(values[0], values[1], values[2], values[3]);
    return true;
}

bool parseMarks(const QString &text, PageLayout::Marks *marks)
{
    const QStringList wanted = text.split(QLatin1Char(','), Qt::SkipEmptyParts);
    if (wanted.isEmpty()) {
        return false;
    }
    for (const QString &raw : wanted) {
        const QString one = raw.trimmed().toLower();
        if (one == QLatin1String("all")) {
            marks->cropMarks = true;
            marks->registrationMarks = true;
            marks->colourBars = true;
            marks->pageInformation = true;
        } else if (one == QLatin1String("crop")) {
            marks->cropMarks = true;
        } else if (one == QLatin1String("registration")) {
            marks->registrationMarks = true;
        } else if (one == QLatin1String("colour-bar") || one == QLatin1String("color-bar")) {
            marks->colourBars = true;
        } else if (one == QLatin1String("page-info")) {
            marks->pageInformation = true;
        } else {
            return false;
        }
    }
    return true;
}

bool parseLabelStyle(const QString &text, PageLayout::Labels::Style *style)
{
    const QString wanted = text.trimmed().toLower();
    if (wanted == QLatin1String("decimal")) {
        *style = PageLayout::Labels::Style::Decimal;
    } else if (wanted == QLatin1String("roman-upper")) {
        *style = PageLayout::Labels::Style::RomanUpper;
    } else if (wanted == QLatin1String("roman-lower")) {
        *style = PageLayout::Labels::Style::RomanLower;
    } else if (wanted == QLatin1String("letter-upper")) {
        *style = PageLayout::Labels::Style::LetterUpper;
    } else if (wanted == QLatin1String("letter-lower")) {
        *style = PageLayout::Labels::Style::LetterLower;
    } else if (wanted == QLatin1String("none")) {
        *style = PageLayout::Labels::Style::None;
    } else {
        return false;
    }
    return true;
}

/**
 * How many pages @p input has, or 0 with @p error set.
 *
 * Counted through boxesOf(), which reads the file with QPDF alone. This group
 * never renders anything, and going through a render backend just to count
 * would make "layout boxes" of a broken scan depend on Poppler opening it.
 */
int pageCountOf(const QString &input, QString *error)
{
    return static_cast<int>(PageLayout::boxesOf(input, error).size());
}

/** The pages @p range names, or an empty list with @p code already explained. */
QVector<int> pagesOf(const QString &input, const QString &range, int *code)
{
    QString error;
    const int count = pageCountOf(input, &error);
    if (count <= 0) {
        *code = fail(error.isEmpty() ? i18n("“%1” has no pages.", input) : error);
        return {};
    }

    const QVector<int> pages = PageRange::parse(range, count, &error);
    if (pages.isEmpty()) {
        *code = fail(error, UsageError);
    }
    return pages;
}

QString describeBox(const QRectF &rect)
{
    return i18nc("@info a page box: its corner, then its size in points and in millimetres",
                 "%1, %2   %3 × %4 pt   (%5 × %6 mm)", QString::number(rect.x(), 'f', 1),
                 QString::number(rect.y(), 'f', 1), QString::number(rect.width(), 'f', 1),
                 QString::number(rect.height(), 'f', 1), QString::number(rect.width() / kPointsPerMillimetre, 'f', 1),
                 QString::number(rect.height() / kPointsPerMillimetre, 'f', 1));
}

QJsonValue boxJson(const QRectF &rect)
{
    if (!rect.isValid()) {
        // Null rather than the fallback the specification prescribes. A script
        // has to be able to tell "this page has no /TrimBox" from "this page's
        // /TrimBox happens to equal its /MediaBox": only one of the two is a
        // document a print shop can put on a press.
        return QJsonValue(QJsonValue::Null);
    }
    return QJsonObject { { QStringLiteral("x"), rect.x() },
                         { QStringLiteral("y"), rect.y() },
                         { QStringLiteral("width"), rect.width() },
                         { QStringLiteral("height"), rect.height() } };
}

/** The five boxes, named the way the file names them and the way JSON should. */
struct BoxField {
    const char *pdfName;
    const char *jsonName;
    const char *optionName;
    QRectF PageLayout::Boxes::*member;
};

constexpr BoxField kBoxFields[] = {
    { "MediaBox", "media", "media-box", &PageLayout::Boxes::media },
    { "CropBox", "crop", "crop-box", &PageLayout::Boxes::crop },
    { "BleedBox", "bleed", "bleed-box", &PageLayout::Boxes::bleed },
    { "TrimBox", "trim", "trim-box", &PageLayout::Boxes::trim },
    { "ArtBox", "art", "art-box", &PageLayout::Boxes::art },
};

QString outputFor(const QCommandLineParser &parser, const QString &input, const QString &suffix)
{
    return outputOr(parser.value(QStringLiteral("output")), input, suffix);
}

// ── Commands ──────────────────────────────────────────────────────────────
//
// Each takes the parser rather than a dozen loose strings: the group is handed
// the parser whole, and a command that reads its own options by name says what
// it depends on where a reader is looking.

int commandBoxes(const QStringList &arguments, const QCommandLineParser &parser)
{
    QString input;
    int code = Success;
    if (!oneInputFile(arguments, QStringLiteral("layout boxes"), &input, &code)) {
        return code;
    }

    QString error;
    const QVector<PageLayout::Boxes> pages = PageLayout::boxesOf(input, &error);
    if (pages.isEmpty()) {
        return fail(error.isEmpty() ? i18n("“%1” has no pages.", input) : error);
    }

    if (parser.isSet(QStringLiteral("json"))) {
        QJsonArray array;
        for (int i = 0; i < pages.size(); ++i) {
            QJsonObject page { { QStringLiteral("number"), i + 1 } };
            for (const BoxField &field : kBoxFields) {
                page.insert(QString::fromLatin1(field.jsonName), boxJson(pages.at(i).*field.member));
            }
            array.append(page);
        }
        const QJsonObject root { { QStringLiteral("file"), QFileInfo(input).absoluteFilePath() },
                                 { QStringLiteral("pageCount"), pages.size() },
                                 { QStringLiteral("pages"), array } };
        out() << QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Indented));
        return Success;
    }

    for (int i = 0; i < pages.size(); ++i) {
        out() << i18nc("@title heading before the boxes of one page", "Page %1", i + 1) << Qt::endl;
        for (const BoxField &field : kBoxFields) {
            const QRectF rect = pages.at(i).*field.member;
            out() << QStringLiteral("  ") << QString::fromLatin1(field.pdfName).leftJustified(10)
                  << (rect.isValid() ? describeBox(rect)
                                     : i18nc("@info a page box this page does not have at all", "absent"))
                  << Qt::endl;
        }
    }
    return Success;
}

int commandSetBoxes(const QStringList &arguments, const QCommandLineParser &parser)
{
    QString input;
    int code = Success;
    if (!oneInputFile(arguments, QStringLiteral("layout set-boxes"), &input, &code)) {
        return code;
    }

    PageLayout::Boxes boxes;
    QStringList given;
    QStringList dropped;
    for (const BoxField &field : kBoxFields) {
        const QString option = QString::fromLatin1(field.optionName);
        const QString pdfName = QString::fromLatin1(field.pdfName);
        if (!parser.isSet(option)) {
            if (field.member != &PageLayout::Boxes::media) {
                dropped.append(pdfName);
            }
            continue;
        }
        if (!parseBox(parser.value(option), &(boxes.*field.member))) {
            return fail(i18n("“%1” is not a box. Give X,Y,WIDTH,HEIGHT in points from the bottom-left corner, "
                             "or “none” to take the box away.",
                             parser.value(option)),
                        UsageError);
        }
        if (field.member == &PageLayout::Boxes::media && !boxes.media.isValid()) {
            return fail(i18n("A page without a /MediaBox is not a page, so --media-box cannot be “none”."), UsageError);
        }
        given.append(pdfName);
    }

    if (given.isEmpty()) {
        return fail(i18n("layout set-boxes needs at least one of --media-box, --crop-box, --bleed-box, --trim-box "
                         "or --art-box."),
                    UsageError);
    }

    const QVector<int> pages = pagesOf(input, parser.value(QStringLiteral("pages")), &code);
    if (pages.isEmpty()) {
        return code;
    }

    const QString output = outputFor(parser, input, QStringLiteral("-boxes.pdf"));

    // Said before it happens, not after. This writes all four optional boxes at
    // once, so a box left unnamed is a box removed, which is right for a
    // command called "set the boxes" and a disaster for someone who thought
    // --trim-box would leave the /BleedBox where it was.
    if (!dropped.isEmpty()) {
        out() << i18n("Setting %1. These are taken away where the pages have them: %2.",
                      given.join(QStringLiteral(", ")), dropped.join(QStringLiteral(", ")))
              << Qt::endl;
    }

    QString error;
    if (!PageLayout::setBoxes(input, output, pages, boxes, &error)) {
        return fail(error, OutputError);
    }

    out() << i18ncp("@info", "Wrote the boxes of %1 page to %2.", "Wrote the boxes of %1 pages to %2.", pages.size(),
                    output)
          << Qt::endl;
    return Success;
}

int commandResize(const QStringList &arguments, const QCommandLineParser &parser)
{
    QString input;
    int code = Success;
    if (!oneInputFile(arguments, QStringLiteral("layout resize"), &input, &code)) {
        return code;
    }

    // --size is shared with the stamping commands, where it means a fraction and
    // carries a default of 0.3; only isSet() tells a real answer from that.
    if (!parser.isSet(QStringLiteral("size"))) {
        return fail(i18n("layout resize needs a paper size (--size), for example a4, letter or 210x297mm."),
                    UsageError);
    }
    QSizeF paper;
    if (!parsePaperSize(parser.value(QStringLiteral("size")), &paper)) {
        return fail(i18n("“%1” is not a paper size. Try a4, a3, a5, letter, legal, or WIDTHxHEIGHT with mm, cm, in "
                         "or pt after it.",
                         parser.value(QStringLiteral("size"))),
                    UsageError);
    }

    PageLayout::Fit fit = PageLayout::Fit::Scale;
    if (!parseFit(parser.value(QStringLiteral("fit")), &fit)) {
        return fail(i18n("“%1” is not a way to fit. Use scale, centre or shrink.", parser.value(QStringLiteral("fit"))),
                    UsageError);
    }

    const QVector<int> pages = pagesOf(input, parser.value(QStringLiteral("pages")), &code);
    if (pages.isEmpty()) {
        return code;
    }

    const QString output = outputFor(parser, input, QStringLiteral("-resized.pdf"));

    QString error;
    if (!PageLayout::resize(input, output, pages, paper, fit, &error)) {
        return fail(error, OutputError);
    }

    out() << i18ncp("@info", "Resized %1 page to %2 × %3 mm.", "Resized %1 pages to %2 × %3 mm.", pages.size(),
                    QString::number(paper.width() / kPointsPerMillimetre, 'f', 1),
                    QString::number(paper.height() / kPointsPerMillimetre, 'f', 1))
          << Qt::endl;
    out() << i18n("Wrote %1.", output) << Qt::endl;
    return Success;
}

int commandUnify(const QStringList &arguments, const QCommandLineParser &parser)
{
    QString input;
    int code = Success;
    if (!oneInputFile(arguments, QStringLiteral("layout unify"), &input, &code)) {
        return code;
    }
    if (parser.isSet(QStringLiteral("pages"))) {
        return fail(i18n("layout unify works on the whole document; leave --pages off."), UsageError);
    }

    // No size means "the largest page there is", which is the whole point of the
    // command for a document somebody assembled out of several scans.
    QSizeF paper;
    if (parser.isSet(QStringLiteral("size")) && !parsePaperSize(parser.value(QStringLiteral("size")), &paper)) {
        return fail(i18n("“%1” is not a paper size. Try a4, a3, a5, letter, legal, or WIDTHxHEIGHT with mm, cm, in "
                         "or pt after it.",
                         parser.value(QStringLiteral("size"))),
                    UsageError);
    }

    PageLayout::Fit fit = PageLayout::Fit::Scale;
    if (!parseFit(parser.value(QStringLiteral("fit")), &fit)) {
        return fail(i18n("“%1” is not a way to fit. Use scale, centre or shrink.", parser.value(QStringLiteral("fit"))),
                    UsageError);
    }

    const QString output = outputFor(parser, input, QStringLiteral("-unified.pdf"));

    QString error;
    if (!PageLayout::unify(input, output, paper, fit, &error)) {
        return fail(error, OutputError);
    }

    out() << i18n("Brought every page to one size. Wrote %1.", output) << Qt::endl;
    return Success;
}

int commandBleed(const QStringList &arguments, const QCommandLineParser &parser)
{
    QString input;
    int code = Success;
    if (!oneInputFile(arguments, QStringLiteral("layout bleed"), &input, &code)) {
        return code;
    }
    if (!parser.isSet(QStringLiteral("bleed"))) {
        return fail(i18n("layout bleed needs a bleed (--bleed), in millimetres. Three is the usual answer."),
                    UsageError);
    }

    double bleed = 0.0;
    if (!parseLength(parser.value(QStringLiteral("bleed")), kPointsPerMillimetre, &bleed)) {
        return fail(i18n("“%1” is not a length. Give millimetres, or a number with mm, cm, in or pt after it.",
                         parser.value(QStringLiteral("bleed"))),
                    UsageError);
    }

    const QVector<int> pages = pagesOf(input, parser.value(QStringLiteral("pages")), &code);
    if (pages.isEmpty()) {
        return code;
    }

    const QString output = outputFor(parser, input, QStringLiteral("-bleed.pdf"));

    QString error;
    if (!PageLayout::setBleed(input, output, pages, bleed, &error)) {
        return fail(error, OutputError);
    }

    out() << i18ncp("@info", "Gave %1 page a %2 mm bleed.", "Gave %1 pages a %2 mm bleed.", pages.size(),
                    QString::number(bleed / kPointsPerMillimetre, 'f', 1))
          << Qt::endl;
    out() << i18n("The trim is what the pages showed before; the sheet grew where it had to. Wrote %1.", output)
          << Qt::endl;
    return Success;
}

int commandMarks(const QStringList &arguments, const QCommandLineParser &parser)
{
    QString input;
    int code = Success;
    if (!oneInputFile(arguments, QStringLiteral("layout marks"), &input, &code)) {
        return code;
    }
    // Marks have to line up sheet to sheet, and the engine has no page
    // selection: better to say so than to ignore a range that was meant.
    if (parser.isSet(QStringLiteral("pages"))) {
        return fail(i18n("layout marks marks every page; leave --pages off."), UsageError);
    }

    PageLayout::Marks marks;
    if (!parseMarks(parser.value(QStringLiteral("marks")), &marks)) {
        return fail(i18n("“%1” is not a list of marks. Use crop, registration, colour-bar, page-info or all, "
                         "separated by commas.",
                         parser.value(QStringLiteral("marks"))),
                    UsageError);
    }

    if (parser.isSet(QStringLiteral("bleed"))
        && !parseLength(parser.value(QStringLiteral("bleed")), kPointsPerMillimetre, &marks.offsetPoints)) {
        return fail(i18n("“%1” is not a length. Give millimetres, or a number with mm, cm, in or pt after it.",
                         parser.value(QStringLiteral("bleed"))),
                    UsageError);
    }
    if (parser.isSet(QStringLiteral("mark-length"))
        && !parseLength(parser.value(QStringLiteral("mark-length")), kPointsPerMillimetre, &marks.lengthPoints)) {
        return fail(i18n("“%1” is not a length. Give millimetres, or a number with mm, cm, in or pt after it.",
                         parser.value(QStringLiteral("mark-length"))),
                    UsageError);
    }
    if (parser.isSet(QStringLiteral("mark-line-width"))
        && !parseLength(parser.value(QStringLiteral("mark-line-width")), 1.0, &marks.lineWidth)) {
        return fail(i18n("“%1” is not a length. Give points, or a number with mm, cm, in or pt after it.",
                         parser.value(QStringLiteral("mark-line-width"))),
                    UsageError);
    }

    const QString output = outputFor(parser, input, QStringLiteral("-marks.pdf"));

    QString error;
    if (!PageLayout::addMarks(input, output, marks, &error)) {
        return fail(error, OutputError);
    }

    QStringList added;
    if (marks.cropMarks) {
        added.append(i18nc("@item a kind of printer's mark", "crop marks"));
    }
    if (marks.registrationMarks) {
        added.append(i18nc("@item a kind of printer's mark", "registration marks"));
    }
    if (marks.colourBars) {
        added.append(i18nc("@item a kind of printer's mark", "colour bars"));
    }
    if (marks.pageInformation) {
        added.append(i18nc("@item a kind of printer's mark", "page information"));
    }

    out() << i18n("Added %1, %2 mm outside the trim.", added.join(QStringLiteral(", ")),
                  QString::number(marks.offsetPoints / kPointsPerMillimetre, 'f', 1))
          << Qt::endl;
    out() << i18n("Wrote %1.", output) << Qt::endl;
    return Success;
}

int commandSplit(const QStringList &arguments, const QCommandLineParser &parser)
{
    QString input;
    int code = Success;
    if (!oneInputFile(arguments, QStringLiteral("layout split"), &input, &code)) {
        return code;
    }

    bool ok = false;
    const double fraction = parser.value(QStringLiteral("cut-at")).toDouble(&ok);
    if (!ok || fraction <= 0.0 || fraction >= 1.0) {
        return fail(i18n("“%1” is not a place to cut. Give a fraction between 0 and 1, for example 0.5 for the "
                         "middle.",
                         parser.value(QStringLiteral("cut-at"))),
                    UsageError);
    }

    const QVector<int> pages = pagesOf(input, parser.value(QStringLiteral("pages")), &code);
    if (pages.isEmpty()) {
        return code;
    }

    const bool vertical = !parser.isSet(QStringLiteral("cut-horizontally"));
    const QString output = outputFor(parser, input, QStringLiteral("-split.pdf"));

    QString error;
    if (!PageLayout::splitPages(input, output, pages, vertical, fraction, &error)) {
        return fail(error, OutputError);
    }

    out() << i18ncp("@info", "Cut %1 page in two.", "Cut %1 pages in two.", pages.size()) << Qt::endl;
    out() << (vertical ? i18n("Left half first, then the right. Wrote %1.", output)
                       : i18n("Top half first, then the bottom. Wrote %1.", output))
          << Qt::endl;
    return Success;
}

int commandOverlay(const QStringList &arguments, const QCommandLineParser &parser)
{
    if (arguments.size() != 2) {
        return fail(i18n("layout overlay needs two files: the document, then what to lay over it."), UsageError);
    }
    for (const QString &path : arguments) {
        const QFileInfo file(path);
        if (!file.exists()) {
            return fail(i18n("“%1” does not exist.", path));
        }
        if (!file.isReadable()) {
            return fail(i18n("“%1” cannot be read.", path));
        }
    }

    const QString base = QFileInfo(arguments.constFirst()).absoluteFilePath();
    const QString over = QFileInfo(arguments.constLast()).absoluteFilePath();
    const QString output = outputFor(parser, base, QStringLiteral("-overlaid.pdf"));

    QString error;
    if (!PageLayout::overlayPages(base, over, output, parser.isSet(QStringLiteral("repeat-overlay")), &error)) {
        return fail(error, OutputError);
    }

    out() << i18n("Laid %1 over %2. Wrote %3.", QFileInfo(over).fileName(), QFileInfo(base).fileName(), output)
          << Qt::endl;
    return Success;
}

int commandPoster(const QStringList &arguments, const QCommandLineParser &parser)
{
    QString input;
    int code = Success;
    if (!oneInputFile(arguments, QStringLiteral("layout poster"), &input, &code)) {
        return code;
    }
    if (!parser.isSet(QStringLiteral("size"))) {
        return fail(i18n("layout poster needs a sheet size (--size), for example a4 or 210x297mm."), UsageError);
    }

    QSizeF sheet;
    if (!parsePaperSize(parser.value(QStringLiteral("size")), &sheet)) {
        return fail(i18n("“%1” is not a paper size. Try a4, a3, a5, letter, legal, or WIDTHxHEIGHT with mm, cm, in "
                         "or pt after it.",
                         parser.value(QStringLiteral("size"))),
                    UsageError);
    }

    double overlap = 0.0;
    if (!parseLength(parser.value(QStringLiteral("overlap")), kPointsPerMillimetre, &overlap)) {
        return fail(i18n("“%1” is not a length. Give millimetres, or a number with mm, cm, in or pt after it.",
                         parser.value(QStringLiteral("overlap"))),
                    UsageError);
    }

    bool ok = false;
    const int page = parser.value(QStringLiteral("page")).toInt(&ok);
    if (!ok || page < 1) {
        return fail(i18n("“%1” is not a page number.", parser.value(QStringLiteral("page"))), UsageError);
    }

    const QString output = outputFor(parser, input, QStringLiteral("-poster.pdf"));

    int sheets = 0;
    QString error;
    if (!PageLayout::poster(input, output, page - 1, sheet, overlap, parser.isSet(QStringLiteral("cut-lines")), &sheets,
                            &error)) {
        return fail(error, OutputError);
    }

    out() << i18np("Tiled page %2 across %1 sheet.", "Tiled page %2 across %1 sheets.", sheets, page) << Qt::endl;
    out() << i18n("The page is tiled at its own size. Resize it first for a bigger poster. Wrote %1.", output)
          << Qt::endl;
    return Success;
}

int commandBooklet(const QStringList &arguments, const QCommandLineParser &parser)
{
    QString input;
    int code = Success;
    if (!oneInputFile(arguments, QStringLiteral("layout booklet"), &input, &code)) {
        return code;
    }

    const bool toPrinter = !parser.isSet(QStringLiteral("reader-order"));
    const QString output = outputFor(parser, input, QStringLiteral("-booklet.pdf"));

    QString error;
    if (!PageLayout::reorderForBinding(input, output, toPrinter, &error)) {
        return fail(error, OutputError);
    }

    out() << (toPrinter ? i18n("Reordered into printer's spreads. Run “nup --per-sheet 2” over this to get the "
                               "sheets themselves.")
                        : i18n("Put the pages back into reading order."))
          << Qt::endl;
    out() << i18n("Wrote %1.", output) << Qt::endl;
    return Success;
}

int commandLabels(const QStringList &arguments, const QCommandLineParser &parser)
{
    QString input;
    int code = Success;
    if (!oneInputFile(arguments, QStringLiteral("layout labels"), &input, &code)) {
        return code;
    }

    QString error;
    const QStringList labels = PageLayout::labelsOf(input, &error);
    if (labels.isEmpty() && !error.isEmpty()) {
        return fail(error);
    }

    if (parser.isSet(QStringLiteral("json"))) {
        QJsonArray array;
        for (int i = 0; i < labels.size(); ++i) {
            array.append(
                QJsonObject { { QStringLiteral("number"), i + 1 }, { QStringLiteral("label"), labels.at(i) } });
        }
        const QJsonObject root { { QStringLiteral("file"), QFileInfo(input).absoluteFilePath() },
                                 { QStringLiteral("pages"), array } };
        out() << QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Indented));
        return Success;
    }

    if (labels.isEmpty()) {
        out() << i18n("This document has no page labels, so readers show the physical page numbers.") << Qt::endl;
        return Success;
    }
    for (int i = 0; i < labels.size(); ++i) {
        out() << i18nc("@info physical page number, then the label the reader shows", "Page %1: %2", i + 1,
                       labels.at(i))
              << Qt::endl;
    }
    return Success;
}

int commandSetLabels(const QStringList &arguments, const QCommandLineParser &parser)
{
    QString input;
    int code = Success;
    if (!oneInputFile(arguments, QStringLiteral("layout set-labels"), &input, &code)) {
        return code;
    }

    const QStringList specifications = parser.values(QStringLiteral("page-label"));
    if (specifications.isEmpty()) {
        return fail(i18n("layout set-labels needs at least one --page-label, as PAGE:STYLE[:PREFIX[:START]]."),
                    UsageError);
    }

    QVector<PageLayout::Labels> ranges;
    for (const QString &specification : specifications) {
        const QStringList fields = specification.split(QLatin1Char(':'));
        if (fields.size() < 2 || fields.size() > 4) {
            return fail(i18n("“%1” is not a page label. The form is PAGE:STYLE[:PREFIX[:START]], for example "
                             "1:roman-lower or 9:decimal:A-:1.",
                             specification),
                        UsageError);
        }

        PageLayout::Labels range;
        bool ok = false;
        const int from = fields.at(0).trimmed().toInt(&ok);
        if (!ok || from < 1) {
            return fail(i18n("“%1” is not a page number.", fields.at(0)), UsageError);
        }
        range.fromPage = from - 1;

        if (!parseLabelStyle(fields.at(1), &range.style)) {
            return fail(i18n("“%1” is not a numbering style. Use decimal, roman-upper, roman-lower, letter-upper, "
                             "letter-lower or none.",
                             fields.at(1)),
                        UsageError);
        }
        if (fields.size() > 2) {
            range.prefix = fields.at(2);
        }
        if (fields.size() > 3) {
            range.startAt = fields.at(3).trimmed().toInt(&ok);
            if (!ok) {
                return fail(i18n("“%1” is not a number to start at.", fields.at(3)), UsageError);
            }
        }
        ranges.append(range);
    }

    const QString output = outputFor(parser, input, QStringLiteral("-labelled.pdf"));

    QString error;
    if (!PageLayout::setLabels(input, output, ranges, &error)) {
        return fail(error, OutputError);
    }

    out() << i18ncp("@info", "Wrote %1 numbering range.", "Wrote %1 numbering ranges.", ranges.size()) << Qt::endl;
    out() << i18n("Wrote %1.", output) << Qt::endl;
    return Success;
}

int commandBlanks(const QStringList &arguments, const QCommandLineParser &parser)
{
    QString input;
    int code = Success;
    if (!oneInputFile(arguments, QStringLiteral("layout blanks"), &input, &code)) {
        return code;
    }

    bool ok = false;
    const double threshold = parser.value(QStringLiteral("ink-threshold")).toDouble(&ok);
    if (!ok || threshold < 0.0) {
        return fail(i18n("“%1” is not a number of painting operators.", parser.value(QStringLiteral("ink-threshold"))),
                    UsageError);
    }

    QString error;
    const QVector<int> blanks = PageLayout::findBlankPages(input, threshold, &error);
    if (blanks.isEmpty() && !error.isEmpty()) {
        return fail(error);
    }

    if (parser.isSet(QStringLiteral("json"))) {
        QJsonArray array;
        for (const int page : blanks) {
            array.append(page + 1);
        }
        const QJsonObject root { { QStringLiteral("file"), QFileInfo(input).absoluteFilePath() },
                                 { QStringLiteral("inkThreshold"), threshold },
                                 { QStringLiteral("blankPages"), array } };
        out() << QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Indented));
        return Success;
    }

    if (blanks.isEmpty()) {
        out() << i18n("Every page paints something.") << Qt::endl;
        return Success;
    }
    out() << i18ncp("@info", "%1 page paints nothing: %2", "%1 pages paint nothing: %2", blanks.size(),
                    PageRange::format(blanks))
          << Qt::endl;
    out() << i18n("Take them out with “layout remove --pages %1”.", PageRange::format(blanks)) << Qt::endl;
    return Success;
}

int commandRemove(const QStringList &arguments, const QCommandLineParser &parser)
{
    QString input;
    int code = Success;
    if (!oneInputFile(arguments, QStringLiteral("layout remove"), &input, &code)) {
        return code;
    }
    // An empty range means "all pages" everywhere else in this program, and here
    // that would empty the document. Removal has to be asked for by name.
    if (!parser.isSet(QStringLiteral("pages"))) {
        return fail(i18n("layout remove needs the pages to take out (--pages), e.g. 3,7-9."), UsageError);
    }

    const QVector<int> pages = pagesOf(input, parser.value(QStringLiteral("pages")), &code);
    if (pages.isEmpty()) {
        return code;
    }

    const QString output = outputFor(parser, input, QStringLiteral("-shorter.pdf"));

    QString error;
    if (!PageLayout::removePages(input, output, pages, &error)) {
        return fail(error, OutputError);
    }

    out() << i18ncp("@info", "Took out %1 page.", "Took out %1 pages.", pages.size()) << Qt::endl;
    out() << i18n("Wrote %1.", output) << Qt::endl;
    return Success;
}

int commandLimits(const QStringList &arguments)
{
    if (!arguments.isEmpty()) {
        return fail(i18n("layout limits takes no arguments."), UsageError);
    }
    out() << i18nc("@title heading before a list", "What page layout cannot promise:") << Qt::endl;
    for (const QString &limitation : PageLayout::limitations()) {
        out() << QStringLiteral("  · ") << limitation << Qt::endl;
    }
    return Success;
}

// ── The group ─────────────────────────────────────────────────────────────

void registerOptions(QCommandLineParser &parser)
{
    // -o/--output, --pages, --page, --size and --json belong to the program as a
    // whole and are read by name; only what this area alone needs is added here.
    parser.addOptions({
        QCommandLineOption(QStringLiteral("fit"), i18n("How content lands on a new sheet: scale, centre or shrink."),
                           QStringLiteral("mode"), QStringLiteral("scale")),
        QCommandLineOption(QStringLiteral("bleed"),
                           i18n("Bleed in millimetres. On “marks”, how far outside the trim the marks sit."),
                           QStringLiteral("mm")),
        QCommandLineOption(QStringLiteral("marks"),
                           i18n("Printer's marks to add: crop, registration, colour-bar, page-info or all."),
                           QStringLiteral("list"), QStringLiteral("crop")),
        QCommandLineOption(QStringLiteral("mark-length"), i18n("How long a crop mark is, in millimetres."),
                           QStringLiteral("mm")),
        QCommandLineOption(QStringLiteral("mark-line-width"), i18n("How thick a printer's mark is, in points."),
                           QStringLiteral("points")),
        QCommandLineOption(QStringLiteral("media-box"),
                           i18n("New /MediaBox as X,Y,WIDTH,HEIGHT in points from the bottom-left corner."),
                           QStringLiteral("rect")),
        QCommandLineOption(QStringLiteral("crop-box"), i18n("New /CropBox, or “none” to take it away."),
                           QStringLiteral("rect")),
        QCommandLineOption(QStringLiteral("bleed-box"), i18n("New /BleedBox, or “none” to take it away."),
                           QStringLiteral("rect")),
        QCommandLineOption(QStringLiteral("trim-box"), i18n("New /TrimBox, or “none” to take it away."),
                           QStringLiteral("rect")),
        QCommandLineOption(QStringLiteral("art-box"), i18n("New /ArtBox, or “none” to take it away."),
                           QStringLiteral("rect")),
        QCommandLineOption(QStringLiteral("cut-at"), i18n("Where to cut a page in two, as a fraction."),
                           QStringLiteral("fraction"), QStringLiteral("0.5")),
        QCommandLineOption(QStringLiteral("cut-horizontally"),
                           i18n("Cut into a top and a bottom half instead of a left and a right.")),
        QCommandLineOption(QStringLiteral("repeat-overlay"),
                           i18n("Start the overlay again when it runs out, for a letterhead.")),
        QCommandLineOption(QStringLiteral("overlap"), i18n("How far poster sheets overlap, in millimetres."),
                           QStringLiteral("mm"), QStringLiteral("10")),
        QCommandLineOption(QStringLiteral("cut-lines"), i18n("Draw the cut line and number each poster sheet.")),
        QCommandLineOption(QStringLiteral("page-label"),
                           i18n("A numbering range, as PAGE:STYLE[:PREFIX[:START]]. Repeat for more."),
                           QStringLiteral("spec")),
        QCommandLineOption(QStringLiteral("ink-threshold"),
                           i18n("How much painting a page may still hold and count as blank."),
                           QStringLiteral("operators"), QStringLiteral("0")),
        QCommandLineOption(QStringLiteral("reader-order"), i18n("Turn printer's spreads back into reading order.")),
    });
}

std::optional<int> runLayout(const QString &verb, const QStringList &arguments, const QCommandLineParser &parser)
{
    if (verb != QLatin1String("layout")) {
        return std::nullopt;
    }

    const QString actions = i18nc("@info a list of the actions “layout” takes",
                                  "boxes, set-boxes, resize, unify, bleed, marks, split, overlay, poster, booklet, "
                                  "labels, set-labels, blanks, remove or limits");
    if (arguments.isEmpty()) {
        return fail(i18n("layout needs something to do: %1.", actions), UsageError);
    }

    QStringList rest = arguments;
    const QString action = rest.takeFirst().toLower();

    if (action == QLatin1String("boxes")) {
        return commandBoxes(rest, parser);
    }
    if (action == QLatin1String("set-boxes")) {
        return commandSetBoxes(rest, parser);
    }
    if (action == QLatin1String("resize")) {
        return commandResize(rest, parser);
    }
    if (action == QLatin1String("unify")) {
        return commandUnify(rest, parser);
    }
    if (action == QLatin1String("bleed")) {
        return commandBleed(rest, parser);
    }
    if (action == QLatin1String("marks")) {
        return commandMarks(rest, parser);
    }
    if (action == QLatin1String("split")) {
        return commandSplit(rest, parser);
    }
    if (action == QLatin1String("overlay")) {
        return commandOverlay(rest, parser);
    }
    if (action == QLatin1String("poster")) {
        return commandPoster(rest, parser);
    }
    if (action == QLatin1String("booklet")) {
        return commandBooklet(rest, parser);
    }
    if (action == QLatin1String("labels")) {
        return commandLabels(rest, parser);
    }
    if (action == QLatin1String("set-labels")) {
        return commandSetLabels(rest, parser);
    }
    if (action == QLatin1String("blanks")) {
        return commandBlanks(rest, parser);
    }
    if (action == QLatin1String("remove")) {
        return commandRemove(rest, parser);
    }
    if (action == QLatin1String("limits") || action == QLatin1String("limitations")) {
        return commandLimits(rest);
    }

    return fail(i18n("“layout %1” is not something layout does. Try %2.", action, actions), UsageError);
}

QStringList layoutHelp()
{
    // The alignment is part of the line: these are sorted in with every other
    // group's and printed as one table.
    return {
        i18nc("@info:shell one line of the command list; keep the columns lined up",
              "  layout boxes      FILE                 Show the five page boxes, and which are absent"),
        i18nc("@info:shell one line of the command list; keep the columns lined up",
              "  layout set-boxes  FILE      -o OUT     Write /MediaBox, /CropBox, /BleedBox, /TrimBox, /ArtBox"),
        i18nc("@info:shell one line of the command list; keep the columns lined up",
              "  layout resize     FILE      -o OUT     Put the pages on a different paper size"),
        i18nc("@info:shell one line of the command list; keep the columns lined up",
              "  layout unify      FILE      -o OUT     Bring every page to one size"),
        i18nc("@info:shell one line of the command list; keep the columns lined up",
              "  layout bleed      FILE      -o OUT     Give the pages a bleed, growing the sheet"),
        i18nc("@info:shell one line of the command list; keep the columns lined up",
              "  layout marks      FILE      -o OUT     Add printer's marks outside the trim"),
        i18nc("@info:shell one line of the command list; keep the columns lined up",
              "  layout split      FILE      -o OUT     Cut each page in two, for scanned books"),
        i18nc("@info:shell one line of the command list; keep the columns lined up",
              "  layout overlay    BASE OVER -o OUT     Lay one document over another"),
        i18nc("@info:shell one line of the command list; keep the columns lined up",
              "  layout poster     FILE      -o OUT     Tile one page across several sheets"),
        i18nc("@info:shell one line of the command list; keep the columns lined up",
              "  layout booklet    FILE      -o OUT     Reorder pages into printer's spreads"),
        i18nc("@info:shell one line of the command list; keep the columns lined up",
              "  layout labels     FILE                 Show the numbers the reader displays"),
        i18nc("@info:shell one line of the command list; keep the columns lined up",
              "  layout set-labels FILE      -o OUT     Write /PageLabels, e.g. i, ii, iii and then 1"),
        i18nc("@info:shell one line of the command list; keep the columns lined up",
              "  layout blanks     FILE                 Find pages that paint nothing"),
        i18nc("@info:shell one line of the command list; keep the columns lined up",
              "  layout remove     FILE      -o OUT     Take pages out"),
        i18nc("@info:shell one line of the command list; keep the columns lined up",
              "  layout limits                          What page layout cannot promise"),
    };
}

} // namespace

Group layoutGroup()
{
    return { registerOptions, runLayout, layoutHelp };
}

} // namespace ps::cli
