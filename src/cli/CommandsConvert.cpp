/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later

    The other half of Convert.

    Its text side reached the command line early, because "give me the words" is
    what everybody asks for first, and to-text, to-markdown and to-html live with
    typeset for that reason. The rest of the engine never got a verb at all: it
    could already draw a page as SVG, find the tables on it, prepare a file for a
    printing press, rewrite one so it opens over a slow link, and correct the
    version its header claims, and no script could ask for any of it. One verb
    each, here.
*/
#include "Commands.h"

#include "core/Convert.h"
#include "core/Document.h"
#include "core/PageRange.h"
#include "render/PopplerBackend.h"

#include <KLocalizedString>

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>
#include <QTextStream>
#include <QVector>

using namespace Qt::Literals::StringLiterals;

namespace ps::cli {
namespace {

void printList(const QString &heading, const QStringList &items)
{
    if (items.isEmpty()) {
        return;
    }
    out() << heading << Qt::endl;
    for (const QString &item : items) {
        out() << u"  · "_s << item << Qt::endl;
    }
}

/**
 * Answers --limits, and says whether it did.
 *
 * The flag itself belongs to the typeset group, which is where the first
 * command to need one was written; registering a second option of that name
 * would leave Qt to pick a winner and the other one silently unread. Asking
 * optionNames() instead reports what was typed without claiming the name, and
 * without a warning when nobody registered it at all.
 */
bool answeredLimits(const QCommandLineParser &parser, int *code)
{
    if (!parser.optionNames().contains(u"limits"_s)) {
        return false;
    }

    // What this build cannot do belongs in the same list as what the method
    // cannot do: to a user asking "why did that not work", a missing program is
    // the same kind of answer as a page that holds no text.
    QStringList notes = ps::Convert::limitations();
    notes.append(ps::Convert::isPdfXAvailable()
                     ? i18n("Ghostscript is installed, so “to-pdfx” can run on this machine.")
                     : i18n("Ghostscript is not installed, so “to-pdfx” cannot run on this machine. It comes in "
                            "the “ghostscript” package."));

    if (parser.isSet(u"json"_s)) {
        out() << QString::fromUtf8(QJsonDocument(QJsonArray::fromStringList(notes)).toJson(QJsonDocument::Indented));
    } else {
        printList(i18nc("@title heading before a list", "What this cannot promise:"), notes);
    }
    *code = Success;
    return true;
}

/** Page indexes for --pages, which needs the document's length to resolve "last". */
bool wantedPages(const QString &input, const QCommandLineParser &parser, QVector<int> *pages, int *code)
{
    ps::PopplerBackend backend;
    ps::Document document;
    document.setRenderBackend(&backend);

    QString error;
    if (!document.open(input, &error)) {
        *code = fail(error);
        return false;
    }
    *pages = ps::PageRange::parse(parser.value(u"pages"_s), document.pageCount(), &error);
    if (pages->isEmpty()) {
        *code = fail(error, UsageError);
        return false;
    }
    return true;
}

/** The words people type for a PDF/X level, with and without the letter. */
bool parseXLevel(const QString &text, ps::Convert::XLevel *level)
{
    static const QHash<QString, ps::Convert::XLevel> names {
        { u"x1a"_s, ps::Convert::XLevel::X1a }, { u"x-1a"_s, ps::Convert::XLevel::X1a },
        { u"1a"_s, ps::Convert::XLevel::X1a },  { u"x3"_s, ps::Convert::XLevel::X3 },
        { u"x-3"_s, ps::Convert::XLevel::X3 },  { u"3"_s, ps::Convert::XLevel::X3 },
        { u"x4"_s, ps::Convert::XLevel::X4 },   { u"x-4"_s, ps::Convert::XLevel::X4 },
        { u"4"_s, ps::Convert::XLevel::X4 },
    };

    const auto it = names.constFind(text.trimmed().toLower());
    if (it == names.constEnd()) {
        return false;
    }
    *level = *it;
    return true;
}

/** The standard's own name for a level, which is not translated anywhere. */
QString describeXLevel(ps::Convert::XLevel level)
{
    switch (level) {
    case ps::Convert::XLevel::X1a:
        return u"PDF/X-1a"_s;
    case ps::Convert::XLevel::X3:
        return u"PDF/X-3"_s;
    case ps::Convert::XLevel::X4:
        return u"PDF/X-4"_s;
    }
    return u"PDF/X"_s;
}

int commandSvg(const QStringList &arguments, const QCommandLineParser &parser)
{
    int code = Success;
    if (answeredLimits(parser, &code)) {
        return code;
    }

    QString input;
    if (!oneInputFile(arguments, u"to-svg"_s, &input, &code)) {
        return code;
    }

    QVector<int> pages;
    if (!wantedPages(input, parser, &pages, &code)) {
        return code;
    }

    // %1 is where the page number goes, padded so that page 2 and page 12 sort
    // the way a person expects them to. A name without one gets the number put
    // in before the extension, so "-o poster.svg" also writes a file per page
    // rather than the same file eleven times.
    const QString target = outputOr(parser.value(u"output"_s), input, u"-%1.svg"_s);

    QString error;
    if (!ps::Convert::toSvg(input, target, pages, &error)) {
        return fail(error, OutputError);
    }

    out() << i18np("Wrote %1 page as SVG.", "Wrote %1 pages as SVG.", int(pages.size())) << Qt::endl;
    // Said because the difference matters to whoever opens the result: one route
    // gives a drawing that can still be searched and edited as text, the other
    // gives the same shapes with no words left in them.
    out() << i18n("Where pdftocairo did the work the text is still text. Where Ghostscript had to stand in for it, "
                  "the letters are outlines and no longer searchable.")
          << Qt::endl;
    return Success;
}

int commandTables(const QStringList &arguments, const QCommandLineParser &parser)
{
    int code = Success;
    if (answeredLimits(parser, &code)) {
        return code;
    }

    QString input;
    if (!oneInputFile(arguments, u"tables"_s, &input, &code)) {
        return code;
    }

    QVector<int> pages;
    if (!wantedPages(input, parser, &pages, &code)) {
        return code;
    }

    QString error;
    const QVector<ps::Convert::Table> tables = ps::Convert::findTables(input, pages, &error);
    if (!error.isEmpty()) {
        return fail(error);
    }

    if (parser.isSet(u"json"_s)) {
        QJsonArray array;
        for (const ps::Convert::Table &table : tables) {
            QJsonArray rows;
            for (const QStringList &row : table.rows) {
                rows.append(QJsonArray::fromStringList(row));
            }
            array.append(QJsonObject {
                { u"page"_s, table.page + 1 }, { u"confidence"_s, table.confidence }, { u"rows"_s, rows } });
        }
        out() << QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Indented));
        return Success;
    }

    if (tables.isEmpty()) {
        out() << i18n("No table was found. There may be none, or its columns may not line up well enough to be "
                      "recognised; “to-text” gives the words either way.")
              << Qt::endl;
        return Success;
    }

    for (const ps::Convert::Table &table : tables) {
        const int columns = table.rows.isEmpty() ? 0 : int(table.rows.constFirst().size());
        out() << i18nc("@info a table that was found: its page, its size and how sure the detection is",
                       "Page %1  %2 × %3  confidence %4", table.page + 1, int(table.rows.size()), columns,
                       QString::number(table.confidence, 'f', 2))
              << Qt::endl;
        for (const QStringList &row : table.rows) {
            out() << u"  "_s << row.join(u" | "_s) << Qt::endl;
        }
    }

    // Printed every time, because there is no table in a PDF to find, only text
    // that happens to line up, and a number nobody is shown is a number nobody
    // can weigh.
    out() << i18np("%1 table found. Confidence is how much of a guess it was; below 0.5, check it against the page.",
                   "%1 tables found. Confidence is how much of a guess each was; below 0.5, check them against the "
                   "page.",
                   int(tables.size()))
          << Qt::endl;
    return Success;
}

int commandCsv(const QStringList &arguments, const QCommandLineParser &parser)
{
    int code = Success;
    if (answeredLimits(parser, &code)) {
        return code;
    }

    QString input;
    if (!oneInputFile(arguments, u"to-csv"_s, &input, &code)) {
        return code;
    }

    QVector<int> pages;
    if (!wantedPages(input, parser, &pages, &code)) {
        return code;
    }

    // Looked for first and written second. tablesToCsv() reports "the file would
    // not open" and "there is no table in it" as the same false, and a script
    // has to tell a broken input from an honest empty result; finding them here
    // is also what gives the count to report afterwards.
    QString error;
    const QVector<ps::Convert::Table> tables = ps::Convert::findTables(input, pages, &error);
    if (!error.isEmpty()) {
        return fail(error);
    }
    if (tables.isEmpty()) {
        return fail(i18n("No table was found in this document. There may be none, or its columns may not line up "
                         "well enough to be recognised; “tables” shows what was considered."));
    }

    const QString output = outputOr(parser.value(u"output"_s), input, u"-tables.csv"_s);
    if (!ps::Convert::tablesToCsv(input, output, pages, &error)) {
        return fail(error, OutputError);
    }

    out() << i18n("Wrote %1.", output) << Qt::endl;
    out() << i18np("%1 table, taken from where the text sits and not from any structure in the file.",
                   "%1 tables, separated by a blank line, taken from where the text sits and not from any structure "
                   "in the file.",
                   int(tables.size()))
          << Qt::endl;
    return Success;
}

int commandPdfX(const QStringList &arguments, const QCommandLineParser &parser)
{
    int code = Success;
    if (answeredLimits(parser, &code)) {
        return code;
    }

    QString input;
    if (!oneInputFile(arguments, u"to-pdfx"_s, &input, &code)) {
        return code;
    }

    // Answered before the document is touched. A missing program is a fact about
    // the machine and not about the file, and a user left to infer it from a
    // conversion that failed will spend the afternoon blaming the document.
    if (!ps::Convert::isPdfXAvailable()) {
        return fail(i18n("Ghostscript is not installed, and it does the conversion to PDF/X. Install the "
                         "“ghostscript” package and try again."));
    }

    ps::Convert::XLevel level = ps::Convert::XLevel::X1a;
    const QString wanted = parser.value(u"pdfx-level"_s);
    if (!parseXLevel(wanted, &level)) {
        return fail(i18n("“%1” is not a PDF/X level. Use x1a, x3 or x4.", wanted), UsageError);
    }

    const QString output = outputOr(parser.value(u"output"_s), input, u"-pdfx.pdf"_s);

    QStringList changes;
    QStringList warnings;
    QString error;
    if (!ps::Convert::toPdfX(input, output, level, parser.value(u"icc-profile"_s), &changes, &warnings, &error)) {
        return fail(error, OutputError);
    }

    out() << i18n("Wrote %1 as %2.", output, describeXLevel(level)) << Qt::endl;
    // In full, never summarised: a print job that comes back the wrong colour is
    // argued about from this list, and a conversion nobody can inspect is one
    // nobody can defend.
    printList(i18nc("@title heading before a list", "What happened:"), changes);
    for (const QString &warning : warnings) {
        err() << i18n("Note: %1", warning) << Qt::endl;
    }
    return Success;
}

int commandLinearise(const QStringList &arguments, const QCommandLineParser &parser)
{
    int code = Success;
    if (answeredLimits(parser, &code)) {
        return code;
    }

    QString input;
    if (!oneInputFile(arguments, u"linearise"_s, &input, &code)) {
        return code;
    }

    const QString output = outputOr(parser.value(u"output"_s), input, u"-web.pdf"_s);

    QString error;
    if (!ps::Convert::linearise(input, output, &error)) {
        return fail(error, OutputError);
    }

    out() << i18n("Wrote %1.", output) << Qt::endl;
    // Said plainly, because "optimised for the web" sounds like a file that got
    // smaller and this one usually got very slightly larger.
    out() << i18n("A reader can now show page one before it has the rest. That is worth nothing on a local disk "
                  "and a great deal over a slow link; the file itself is no smaller.")
          << Qt::endl;
    return Success;
}

int commandSetVersion(const QStringList &arguments, const QCommandLineParser &parser)
{
    int code = Success;
    if (answeredLimits(parser, &code)) {
        return code;
    }

    QString input;
    if (!oneInputFile(arguments, u"set-version"_s, &input, &code)) {
        return code;
    }

    const QString version = parser.value(u"pdf-version"_s);
    if (version.isEmpty()) {
        return fail(i18n("Say which version the header should claim: --pdf-version 1.7."), UsageError);
    }

    const QString output = outputOr(parser.value(u"output"_s), input, u"-version.pdf"_s);

    QStringList warnings;
    QString error;
    if (!ps::Convert::setVersion(input, output, version, &warnings, &error)) {
        // The features first, then the refusal. The refusal names a version; the
        // list names what asked for it, and that is the part somebody has to act
        // on before asking again.
        for (const QString &warning : warnings) {
            err() << i18n("Note: %1", warning) << Qt::endl;
        }
        return fail(error);
    }

    out() << i18n("Wrote %1, claiming PDF %2.", output, version) << Qt::endl;
    for (const QString &warning : warnings) {
        err() << i18n("Note: %1", warning) << Qt::endl;
    }
    return Success;
}

void convertOptions(QCommandLineParser &parser)
{
    // -o/--output, --pages, --json and --icc-profile belong to the program as a
    // whole and are read by name; only what these verbs alone need is added.
    const QCommandLineOption levelOption(u"pdfx-level"_s,
                                         i18n("Which PDF/X to write: x1a, x3 or x4. Most printers still ask for "
                                              "x1a."),
                                         u"level"_s, u"x1a"_s);
    const QCommandLineOption versionOption(u"pdf-version"_s, i18n("The version the PDF header should claim, e.g. 1.7."),
                                           u"version"_s);

    parser.addOptions({ levelOption, versionOption });
}

std::optional<int> runConvert(const QString &verb, const QStringList &arguments, const QCommandLineParser &parser)
{
    if (verb == QLatin1String("to-svg")) {
        return commandSvg(arguments, parser);
    }
    if (verb == QLatin1String("tables")) {
        return commandTables(arguments, parser);
    }
    if (verb == QLatin1String("to-csv")) {
        return commandCsv(arguments, parser);
    }
    if (verb == QLatin1String("to-pdfx")) {
        return commandPdfX(arguments, parser);
    }
    // The American spelling is accepted the way "color" is accepted for
    // "colour": nobody should have to guess which one this project chose.
    if (verb == QLatin1String("linearise") || verb == QLatin1String("linearize")) {
        return commandLinearise(arguments, parser);
    }
    if (verb == QLatin1String("set-version")) {
        return commandSetVersion(arguments, parser);
    }
    return std::nullopt;
}

/** One line each, with the verb left in English and only the explanation translated. */
QString helpLine(const QString &usage, const QString &description)
{
    return u"  "_s + usage.leftJustified(28) + u"\t"_s + description;
}

QStringList convertHelp()
{
    return {
        helpLine(u"to-svg FILE      [-o TMPL]"_s, i18n("Draw each page as an SVG file")),
        helpLine(u"tables FILE"_s, i18n("Show the tables it can find, and how sure it is")),
        helpLine(u"to-csv FILE      [-o OUT]"_s, i18n("Write those tables out as comma-separated values")),
        helpLine(u"to-pdfx FILE     [-o OUT]"_s, i18n("Convert to PDF/X for a commercial printer")),
        helpLine(u"linearise FILE   [-o OUT]"_s, i18n("Rewrite so page one arrives over a slow link first")),
        helpLine(u"set-version FILE [-o OUT]"_s, i18n("Set the PDF version the header claims")),
    };
}

} // namespace

Group convertGroup()
{
    return { convertOptions, runConvert, convertHelp };
}

} // namespace ps::cli
