/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "Commands.h"

#include "core/ColourTools.h"
#include "core/Document.h"
#include "core/PageRange.h"
#include "render/PopplerBackend.h"

#include <KLocalizedString>

#include <QColor>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QHash>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QTextStream>

#include <algorithm>
#include <cmath>

using namespace Qt::Literals::StringLiterals;

namespace ps::cli {
namespace {

/** The words people type for a colour target, in English and in German. */
bool parseTarget(const QString &text, ps::ColourTools::Target *target)
{
    static const QHash<QString, ps::ColourTools::Target> names {
        { u"grey"_s, ps::ColourTools::Target::Grayscale },
        { u"gray"_s, ps::ColourTools::Target::Grayscale },
        { u"greyscale"_s, ps::ColourTools::Target::Grayscale },
        { u"grayscale"_s, ps::ColourTools::Target::Grayscale },
        { u"graustufen"_s, ps::ColourTools::Target::Grayscale },
        { u"bw"_s, ps::ColourTools::Target::BlackWhite },
        { u"black-white"_s, ps::ColourTools::Target::BlackWhite },
        { u"mono"_s, ps::ColourTools::Target::BlackWhite },
        { u"sw"_s, ps::ColourTools::Target::BlackWhite },
        { u"rgb"_s, ps::ColourTools::Target::Rgb },
        { u"cmyk"_s, ps::ColourTools::Target::Cmyk },
    };

    const auto it = names.constFind(text.trimmed().toLower());
    if (it == names.constEnd()) {
        return false;
    }
    *target = *it;
    return true;
}

/** What an unnamed output is called, so two targets do not overwrite each other. */
QString suffixFor(ps::ColourTools::Target target)
{
    switch (target) {
    case ps::ColourTools::Target::Grayscale:
        return u"-grey.pdf"_s;
    case ps::ColourTools::Target::BlackWhite:
        return u"-bw.pdf"_s;
    case ps::ColourTools::Target::Rgb:
        return u"-rgb.pdf"_s;
    case ps::ColourTools::Target::Cmyk:
        return u"-cmyk.pdf"_s;
    }
    return u"-colour.pdf"_s;
}

/** Page indexes for @p range, which needs the document's length to resolve "last". */
QVector<int> resolvePages(const QString &input, const QString &range, QString *error)
{
    ps::PopplerBackend backend;
    ps::Document document;
    document.setRenderBackend(&backend);
    if (!document.open(input, error)) {
        return {};
    }
    return ps::PageRange::parse(range, document.pageCount(), error);
}

QString yesNo(bool value)
{
    return value ? i18nc("@item something the document does", "yes")
                 : i18nc("@item something the document does not do", "no");
}

/** A label and its value, one per line, aligned. */
void reportLine(const QString &label, const QString &value)
{
    out() << QStringLiteral("%1 %2").arg(label.leftJustified(17), value) << Qt::endl;
}

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

int commandInspect(const QStringList &arguments, const QCommandLineParser &parser)
{
    QString input;
    int code = Success;
    if (!oneInputFile(arguments, u"colour inspect"_s, &input, &code)) {
        return code;
    }

    QString error;
    const ps::ColourTools::Inventory inventory = ps::ColourTools::inspect(input, &error);
    if (!error.isEmpty()) {
        return fail(error);
    }

    const bool strokesSeen = std::isfinite(inventory.thinnestStroke);

    if (parser.isSet(u"json"_s)) {
        const QJsonObject root { { u"file"_s, input },
                                 { u"pages"_s, inventory.pages },
                                 { u"spaces"_s, QJsonArray::fromStringList(inventory.spaces) },
                                 { u"spotColours"_s, QJsonArray::fromStringList(inventory.spotColours) },
                                 { u"hasIccProfile"_s, inventory.hasIccProfile },
                                 { u"hasOutputIntent"_s, inventory.hasOutputIntent },
                                 { u"usesTransparency"_s, inventory.usesTransparency },
                                 { u"usesOverprint"_s, inventory.usesOverprint },
                                 { u"pagesWithRgb"_s, inventory.pagesWithRgb },
                                 { u"pagesWithCmyk"_s, inventory.pagesWithCmyk },
                                 { u"hairlines"_s, inventory.hairlines },
                                 { u"strokes"_s, inventory.strokes },
                                 // JSON has no infinity, so a document that strokes nothing says null
                                 // rather than a number no reader could parse back.
                                 { u"thinnestStrokePt"_s,
                                   strokesSeen ? QJsonValue(inventory.thinnestStroke) : QJsonValue() },
                                 { u"notes"_s, QJsonArray::fromStringList(inventory.notes) } };
        out() << QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Indented));
        return Success;
    }

    reportLine(i18n("File:"), input);
    reportLine(i18n("Pages:"), QString::number(inventory.pages));
    reportLine(i18n("Colour spaces:"),
               inventory.spaces.isEmpty() ? i18nc("@item nothing was found", "none") : inventory.spaces.join(u", "_s));
    reportLine(i18n("Spot colours:"),
               inventory.spotColours.isEmpty() ? i18nc("@item nothing was found", "none")
                                               : inventory.spotColours.join(u", "_s));
    reportLine(i18n("ICC profile:"), yesNo(inventory.hasIccProfile));
    reportLine(i18n("Output intent:"), yesNo(inventory.hasOutputIntent));
    reportLine(i18n("Transparency:"), yesNo(inventory.usesTransparency));
    reportLine(i18n("Overprint:"), yesNo(inventory.usesOverprint));
    reportLine(i18n("RGB pages:"),
               i18nc("@info a count out of the whole document", "%1 of %2", inventory.pagesWithRgb, inventory.pages));
    reportLine(i18n("CMYK pages:"),
               i18nc("@info a count out of the whole document", "%1 of %2", inventory.pagesWithCmyk, inventory.pages));
    reportLine(i18n("Strokes:"),
               strokesSeen ? i18nc("@info how many strokes, and the finest of them", "%1, thinnest %2 pt",
                                   inventory.strokes, QString::number(inventory.thinnestStroke, 'f', 3))
                           : i18nc("@item nothing was found", "none"));
    reportLine(i18n("Hairlines:"), QString::number(inventory.hairlines));

    // Said out loud, because a clean hairline count is not the same as a safe
    // one: a rule finer than the press holds will still be lost.
    if (inventory.hairlines > 0) {
        out() << i18np("%1 stroke is thinner than a quarter point and may vanish in print. "
                       "“colour hairlines” thickens them.",
                       "%1 strokes are thinner than a quarter point and may vanish in print. "
                       "“colour hairlines” thickens them.",
                       inventory.hairlines)
              << Qt::endl;
    }

    printList(i18nc("@title heading before a list", "Notes:"), inventory.notes);
    return Success;
}

int commandConvert(const QStringList &arguments, const QCommandLineParser &parser)
{
    QString input;
    int code = Success;
    if (!oneInputFile(arguments, u"colour convert"_s, &input, &code)) {
        return code;
    }

    const QString wanted = parser.value(u"to"_s);
    if (wanted.isEmpty()) {
        return fail(i18n("Say what to convert to: --to grey, bw, rgb or cmyk."), UsageError);
    }

    ps::ColourTools::ConvertOptions options;
    if (!parseTarget(wanted, &options.target)) {
        return fail(i18n("“%1” is not a colour target. Use grey, bw, rgb or cmyk.", wanted), UsageError);
    }
    options.iccProfilePath = parser.value(u"icc-profile"_s);

    bool ok = false;
    options.threshold = parser.value(u"threshold"_s).toDouble(&ok);
    if (!ok || options.threshold < 0.0 || options.threshold > 1.0) {
        return fail(i18n("--threshold is a brightness between 0 and 1, e.g. 0.5."), UsageError);
    }

    // --quality means the same here as it does on from-images, zero included:
    // asking for no re-encoding is how a lossless workflow keeps its pixels.
    if (parser.isSet(u"quality"_s)) {
        const int quality = parser.value(u"quality"_s).toInt(&ok);
        if (!ok || quality < 0 || quality > 100) {
            return fail(i18n("--quality is a number between 0 and 100."), UsageError);
        }
        options.recompressPhotographs = quality > 0;
        if (quality > 0) {
            options.jpegQuality = quality;
        }
    }

    QString error;
    if (parser.isSet(u"pages"_s)) {
        options.pages = resolvePages(input, parser.value(u"pages"_s), &error);
        if (options.pages.isEmpty()) {
            return fail(error, UsageError);
        }
    }

    const QString output = outputOr(parser.value(u"output"_s), input, suffixFor(options.target));

    QStringList changes;
    if (!ps::ColourTools::convert(input, output, options, &changes, &error)) {
        return fail(error, OutputError);
    }

    out() << i18n("Wrote %1.", output) << Qt::endl;
    if (changes.isEmpty()) {
        out() << i18n("Nothing in this document needed converting.") << Qt::endl;
        return Success;
    }
    // Printed in full, never summarised: which route was taken is the whole
    // point of this command, and a conversion nobody can inspect is one nobody
    // can trust.
    printList(i18nc("@title heading before a list", "What happened:"), changes);
    return Success;
}

int commandReplace(const QStringList &arguments, const QCommandLineParser &parser)
{
    QString input;
    int code = Success;
    if (!oneInputFile(arguments, u"colour replace"_s, &input, &code)) {
        return code;
    }

    const QString fromText = parser.value(u"from"_s);
    const QString toText = parser.value(u"to-colour"_s);
    if (fromText.isEmpty() || toText.isEmpty()) {
        return fail(i18n("Give both colours: --from red --to-colour '#0044cc'."), UsageError);
    }

    const QColor from(fromText);
    if (!from.isValid()) {
        return fail(i18n("“%1” is not a colour. Try a name or #rrggbb.", fromText), UsageError);
    }
    const QColor to(toText);
    if (!to.isValid()) {
        return fail(i18n("“%1” is not a colour. Try a name or #rrggbb.", toText), UsageError);
    }

    // Exact by default. The shared --tolerance carries a pixel default meant for
    // compare, which is nonsense as a distance in unit RGB, so it is read only
    // when the user actually set it.
    double tolerance = 0.0;
    if (parser.isSet(u"tolerance"_s)) {
        bool ok = false;
        tolerance = parser.value(u"tolerance"_s).toDouble(&ok);
        if (!ok || tolerance < 0.0 || tolerance > 1.0) {
            return fail(i18n("--tolerance is a distance in colour between 0 and 1; 0.05 forgives rounding."),
                        UsageError);
        }
    }

    const QString output = outputOr(parser.value(u"output"_s), input, u"-recoloured.pdf"_s);

    int replaced = 0;
    QString error;
    if (!ps::ColourTools::replaceColour(input, output, from, to, tolerance, &replaced, &error)) {
        return fail(error, OutputError);
    }

    out() << i18n("Wrote %1.", output) << Qt::endl;
    out() << i18np("Replaced %1 colour.", "Replaced %1 colours.", replaced) << Qt::endl;
    if (replaced == 0) {
        out() << i18n("Nothing matched. Colours are matched in the drawing operators, not in the pixels of "
                      "photographs; --tolerance 0.05 forgives the rounding a design tool leaves behind.")
              << Qt::endl;
    }
    return Success;
}

int commandSpot(const QStringList &arguments, const QCommandLineParser &parser)
{
    QString input;
    int code = Success;
    if (!oneInputFile(arguments, u"colour spot"_s, &input, &code)) {
        return code;
    }

    QHash<QString, QString> renames;
    for (const QString &pair : parser.values(u"rename"_s)) {
        const int equals = pair.indexOf(u'=');
        if (equals <= 0 || equals == pair.size() - 1) {
            return fail(i18n("“%1” should look like OLD=NEW, for example “Pantone 300 C=Hausblau”.", pair), UsageError);
        }
        renames.insert(pair.left(equals), pair.mid(equals + 1));
    }

    const QStringList toProcess = parser.values(u"to-process"_s);
    if (renames.isEmpty() && toProcess.isEmpty()) {
        return fail(i18n("Say what to do with the inks: --rename OLD=NEW, or --to-process INK to dissolve one "
                         "into process colour."),
                    UsageError);
    }

    const QString output = outputOr(parser.value(u"output"_s), input, u"-inks.pdf"_s);

    int changed = 0;
    QString error;
    if (!ps::ColourTools::mapSpotColours(input, output, renames, toProcess, &changed, &error)) {
        return fail(error, OutputError);
    }

    out() << i18n("Wrote %1.", output) << Qt::endl;
    out() << i18np("Changed %1 separation.", "Changed %1 separations.", changed) << Qt::endl;
    if (changed == 0) {
        out() << i18n("No ink of that name is in this document. “colour inspect” lists the ones that are.") << Qt::endl;
    }
    return Success;
}

int commandHairlines(const QStringList &arguments, const QCommandLineParser &parser)
{
    QString input;
    int code = Success;
    if (!oneInputFile(arguments, u"colour hairlines"_s, &input, &code)) {
        return code;
    }

    bool ok = false;
    const double minimum = parser.value(u"min-width"_s).toDouble(&ok);
    if (!ok || minimum <= 0.0) {
        return fail(i18n("--min-width is a stroke width in points, e.g. 0.25."), UsageError);
    }

    const QString output = outputOr(parser.value(u"output"_s), input, u"-hairlines.pdf"_s);

    int fixed = 0;
    QString error;
    if (!ps::ColourTools::fixHairlines(input, output, minimum, &fixed, &error)) {
        return fail(error, OutputError);
    }

    out() << i18n("Wrote %1.", output) << Qt::endl;
    if (fixed == 0) {
        out() << i18n("No stroke was thinner than %1 pt.", QString::number(minimum)) << Qt::endl;
        return Success;
    }
    out() << i18np("Thickened %1 stroke to %2 pt.", "Thickened %1 strokes to %2 pt.", fixed, QString::number(minimum))
          << Qt::endl;
    return Success;
}

int commandOverprint(const QStringList &arguments, const QCommandLineParser &parser)
{
    QString input;
    int code = Success;
    if (!oneInputFile(arguments, u"colour overprint"_s, &input, &code)) {
        return code;
    }

    const bool on = !parser.isSet(u"off"_s);
    const QString output = outputOr(parser.value(u"output"_s), input, u"-overprint.pdf"_s);

    int changed = 0;
    QString error;
    if (!ps::ColourTools::setBlackOverprint(input, output, on, &changed, &error)) {
        return fail(error, OutputError);
    }

    out() << i18n("Wrote %1.", output) << Qt::endl;
    out() << (on ? i18np("Black now overprints in %1 place.", "Black now overprints in %1 places.", changed)
                 : i18np("Black is now set not to overprint in %1 place.",
                         "Black is now set not to overprint in %1 places.", changed))
          << Qt::endl;
    return Success;
}

int commandCoverage(const QStringList &arguments, const QCommandLineParser &parser)
{
    QString input;
    int code = Success;
    if (!oneInputFile(arguments, u"colour coverage"_s, &input, &code)) {
        return code;
    }

    QString error;
    const QVector<double> coverage = ps::ColourTools::inkCoverage(input, &error);
    if (coverage.isEmpty()) {
        return fail(error);
    }

    const double highest = *std::max_element(coverage.cbegin(), coverage.cend());

    if (parser.isSet(u"json"_s)) {
        QJsonArray pages;
        for (int i = 0; i < coverage.size(); ++i) {
            pages.append(QJsonObject { { u"page"_s, i + 1 }, { u"percent"_s, coverage.at(i) } });
        }
        const QJsonObject root { { u"file"_s, input }, { u"maximumPercent"_s, highest }, { u"pages"_s, pages } };
        out() << QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Indented));
        return Success;
    }

    for (int i = 0; i < coverage.size(); ++i) {
        out() << QStringLiteral("%1  %2 %").arg(i + 1, 4).arg(QString::number(coverage.at(i), 'f', 1), 7) << Qt::endl;
    }
    out() << i18n("Highest total ink: %1 % of a page.", QString::number(highest, 'f', 1)) << Qt::endl;
    // The number people are actually looking for. Left as a note rather than a
    // failing exit code, because the limit is the print shop's to set.
    if (highest > 300.0) {
        out() << i18n("Over 300 %, which most presses cannot dry. Ask the print shop for their ink limit.") << Qt::endl;
    }
    return Success;
}

int commandSeparate(const QStringList &arguments, const QCommandLineParser &parser)
{
    QString input;
    int code = Success;
    if (!oneInputFile(arguments, u"colour separate"_s, &input, &code)) {
        return code;
    }

    const QString ink = parser.value(u"ink"_s);
    if (ink.isEmpty()) {
        return fail(i18n("Say which plate: --ink Cyan, Magenta, Yellow, Black, or a separation name as "
                         "“colour inspect” reports it."),
                    UsageError);
    }

    bool ok = false;
    const int page = parser.value(u"page"_s).toInt(&ok) - 1;
    if (!ok || page < 0) {
        return fail(i18n("--page is a page number, counting from 1."), UsageError);
    }
    const double dpi = parser.value(u"dpi"_s).toDouble(&ok);
    if (!ok || dpi <= 0.0) {
        return fail(i18n("--dpi is a resolution in dots per inch, e.g. 150."), UsageError);
    }

    // An ink name may hold spaces and punctuation ("Pantone 300 C"), and a
    // default filename should hold neither.
    QString slug;
    for (const QChar letter : ink) {
        slug += letter.isLetterOrNumber() ? letter.toLower() : u'-';
    }
    const QString output = outputOr(parser.value(u"output"_s), input, u'-' + slug + u".png"_s);

    QString error;
    const QImage plate = ps::ColourTools::separation(input, page, ink, dpi, &error);
    if (plate.isNull()) {
        return fail(error);
    }
    if (!plate.save(output)) {
        return fail(i18n("“%1” could not be written.", output), OutputError);
    }

    out() << i18n("Wrote the %1 plate of page %2 to %3.", ink, page + 1, output) << Qt::endl;
    out() << i18n("It is an ink density map: black is full ink, white is bare paper.") << Qt::endl;
    return Success;
}

int commandLimits(const QStringList &arguments)
{
    if (!arguments.isEmpty()) {
        return fail(i18n("“colour limits” takes no file."), UsageError);
    }

    out() << (ps::ColourTools::hasColourManagement()
                  ? i18n("This build converts into CMYK itself, given an ICC profile.")
                  : i18n("This build has no colour management, so converting into CMYK goes through Ghostscript."))
          << Qt::endl;

    const QString profile = ps::ColourTools::defaultIccProfile(ps::ColourTools::Target::Cmyk);
    out() << (profile.isEmpty()
                  ? i18n("No CMYK profile was found on this system. A print job names the profile it wants; "
                         "pass it with --icc-profile.")
                  : i18n("CMYK profile found on this system: %1", profile))
          << Qt::endl;

    out() << Qt::endl;
    printList(i18nc("@title heading before a list", "What this cannot promise:"), ps::ColourTools::limitations());
    return Success;
}

void colourOptions(QCommandLineParser &parser)
{
    const QCommandLineOption toOption(u"to"_s, i18n("Colour to convert to: grey, bw, rgb or cmyk."), u"target"_s);
    const QCommandLineOption thresholdOption(
        u"threshold"_s, i18n("For --to bw: brighter than this becomes white, 0 to 1."), u"0-1"_s, u"0.5"_s);
    const QCommandLineOption fromOption(u"from"_s, i18n("The colour to replace, a name or #rrggbb."), u"colour"_s);
    const QCommandLineOption toColourOption(u"to-colour"_s, i18n("The colour to put in its place."), u"colour"_s);
    const QCommandLineOption renameOption(
        u"rename"_s,
        i18n("Rename a spot colour, as OLD=NEW. Repeat for more; two inks given the same name are merged."), u"pair"_s);
    const QCommandLineOption toProcessOption(
        u"to-process"_s, i18n("Dissolve this spot colour into process colour. Repeat for more."), u"ink"_s);
    const QCommandLineOption minWidthOption(u"min-width"_s, i18n("Thinnest stroke that still prints, in points."),
                                            u"points"_s, u"0.25"_s);
    const QCommandLineOption offOption(u"off"_s, i18n("Turn black overprint off instead of on."));
    const QCommandLineOption inkOption(
        u"ink"_s, i18n("Which plate to render: Cyan, Magenta, Yellow, Black or a separation name."), u"name"_s);

    parser.addOptions({ toOption, thresholdOption, fromOption, toColourOption, renameOption, toProcessOption,
                        minWidthOption, offOption, inkOption });
}

std::optional<int> runColour(const QString &verb, const QStringList &arguments, const QCommandLineParser &parser)
{
    // The American spelling is accepted the way "center" is accepted for
    // "centre" elsewhere: nobody should have to guess which one this project
    // chose.
    if (verb != QLatin1String("colour") && verb != QLatin1String("color")) {
        return std::nullopt;
    }

    const QString action = arguments.value(0).toLower();
    const QStringList rest = arguments.mid(1);

    if (action == QLatin1String("inspect")) {
        return commandInspect(rest, parser);
    }
    if (action == QLatin1String("convert")) {
        return commandConvert(rest, parser);
    }
    if (action == QLatin1String("replace")) {
        return commandReplace(rest, parser);
    }
    if (action == QLatin1String("spot")) {
        return commandSpot(rest, parser);
    }
    if (action == QLatin1String("hairlines")) {
        return commandHairlines(rest, parser);
    }
    if (action == QLatin1String("overprint")) {
        return commandOverprint(rest, parser);
    }
    if (action == QLatin1String("coverage")) {
        return commandCoverage(rest, parser);
    }
    if (action == QLatin1String("separate")) {
        return commandSeparate(rest, parser);
    }
    if (action == QLatin1String("limits") || action == QLatin1String("limitations")) {
        return commandLimits(rest);
    }

    if (action.isEmpty()) {
        return fail(i18n("“colour” needs something to do: inspect, convert, replace, spot, hairlines, "
                         "overprint, coverage, separate or limits."),
                    UsageError);
    }
    return fail(i18n("“colour %1” is not something this can do. Try inspect, convert, replace, spot, "
                     "hairlines, overprint, coverage, separate or limits.",
                     action),
                UsageError);
}

/** One line each, with the verb left in English and only the explanation translated. */
QString helpLine(const QString &usage, const QString &description)
{
    return u"  "_s + usage.leftJustified(24) + u"\t"_s + description;
}

QStringList colourHelp()
{
    return {
        helpLine(u"colour inspect FILE"_s, i18n("What the document does with colour")),
        helpLine(u"colour convert FILE"_s, i18n("Convert to grey, black and white, RGB or CMYK")),
        helpLine(u"colour replace FILE"_s, i18n("Swap one colour for another, everywhere")),
        helpLine(u"colour spot FILE"_s, i18n("Rename, merge or dissolve named separations")),
        helpLine(u"colour hairlines FILE"_s, i18n("Thicken strokes too thin to print")),
        helpLine(u"colour overprint FILE"_s, i18n("Set or clear overprint on black")),
        helpLine(u"colour coverage FILE"_s, i18n("Measure the total ink on each page")),
        helpLine(u"colour separate FILE"_s, i18n("Render one printing plate as an image")),
        helpLine(u"colour limits"_s, i18n("What the colour work here cannot promise")),
    };
}

} // namespace

Group colourGroup()
{
    return { colourOptions, runColour, colourHelp };
}

} // namespace ps::cli
