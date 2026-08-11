/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later

    Text going onto pages, and the words coming back off them.

    Typeset is the one part of this program that makes a document out of
    nothing, so it is the one command that takes something other than a PDF as
    its input. Convert's text side is here too, because it is the same journey
    in the other direction and the two are only useful when they agree about
    what a paragraph is.
*/
#include "Commands.h"

#include "core/Convert.h"
#include "core/Document.h"
#include "core/PageRange.h"
#include "core/Typeset.h"
#include "render/PopplerBackend.h"

#include <QColor>
#include <QCommandLineParser>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSizeF>
#include <QStringDecoder>
#include <QTemporaryFile>
#include <QTextStream>

#include <KLocalizedString>

#include <optional>

namespace ps::cli {

namespace {

constexpr double MillimetresToPoints = 72.0 / 25.4;
constexpr double PointsToPoints = 1.0;

/**
 * The value of @p name, but only when it was really typed.
 *
 * Most of the options this group reads were registered by another command and
 * carry that command's default: --size means "0.3 of the page" to the signature
 * stamp and --font-size means ten points to the page numberer. Calling value()
 * on those would quietly impose a number nobody asked for, so what matters is
 * whether the name appeared on the line at all. optionNames() answers that, and
 * unlike isSet() it stays silent about a name no group happens to register.
 */
std::optional<QString> typed(const QCommandLineParser &parser, const QString &name)
{
    if (!parser.optionNames().contains(name)) {
        return std::nullopt;
    }
    return parser.value(name);
}

/** Points per unit for a trailing mm, cm, in or pt; zero when there is no suffix. */
double trailingUnit(const QString &text)
{
    if (text.endsWith(QLatin1String("mm"))) {
        return MillimetresToPoints;
    }
    if (text.endsWith(QLatin1String("cm"))) {
        return 72.0 / 2.54;
    }
    if (text.endsWith(QLatin1String("in"))) {
        return 72.0;
    }
    if (text.endsWith(QLatin1String("pt"))) {
        return PointsToPoints;
    }
    return 0.0;
}

/**
 * A length in points.
 *
 * A unit may be written on the number, and @p fallbackFactor decides what a
 * bare number means. Paper is talked about in millimetres and type in points,
 * so the two kinds of measurement default differently rather than forcing
 * everyone to write "297mm" for A4 height or "11pt" for a body size.
 */
bool lengthInPoints(const QString &text, double fallbackFactor, double *points)
{
    QString value = text.trimmed().toLower();
    double factor = trailingUnit(value);
    if (factor > 0.0) {
        value.chop(2);
    } else {
        factor = fallbackFactor;
    }
    bool ok = false;
    const double number = value.trimmed().toDouble(&ok);
    if (!ok) {
        return false;
    }
    *points = number * factor;
    return true;
}

/** A paper name, or WIDTHxHEIGHT with an optional unit. */
bool parsePageSize(const QString &text, QSizeF *size)
{
    static const QHash<QString, QSizeF> named {
        { QStringLiteral("a3"), QSizeF(841.89, 1190.55) },  { QStringLiteral("a4"), QSizeF(595.276, 841.89) },
        { QStringLiteral("a5"), QSizeF(419.528, 595.276) }, { QStringLiteral("letter"), QSizeF(612, 792) },
        { QStringLiteral("legal"), QSizeF(612, 1008) },
    };

    const QString lower = text.trimmed().toLower();
    const auto it = named.constFind(lower);
    if (it != named.constEnd()) {
        *size = *it;
        return true;
    }

    // A unit written once, at the end, applies to both numbers: "595x842pt"
    // should not silently make the width 595 millimetres. It is required, and no
    // longer guessed: a bare "210x297" was read here as millimetres and by
    // "layout resize" as points, so one pair of numbers asked for A4 in one
    // command and for a page a third that size in the other. Named papers still
    // need no unit, because "a4" already carries one.
    const double shared = trailingUnit(lower);
    if (shared <= 0.0) {
        return false;
    }
    const QStringList parts = lower.split(QLatin1Char('x'), Qt::SkipEmptyParts);
    if (parts.size() != 2) {
        return false;
    }
    double width = 0.0;
    double height = 0.0;
    if (!lengthInPoints(parts.at(0), shared, &width) || !lengthInPoints(parts.at(1), shared, &height)) {
        return false;
    }
    if (width <= 0.0 || height <= 0.0) {
        return false;
    }
    *size = QSizeF(width, height);
    return true;
}

/** The words people type for the four alignments this can set, in both interface languages. */
bool parseAlignment(const QString &text, Qt::Alignment *alignment)
{
    static const QHash<QString, Qt::Alignment> names {
        { QStringLiteral("left"), Qt::AlignLeft },         { QStringLiteral("links"), Qt::AlignLeft },
        { QStringLiteral("right"), Qt::AlignRight },       { QStringLiteral("rechts"), Qt::AlignRight },
        { QStringLiteral("centre"), Qt::AlignHCenter },    { QStringLiteral("center"), Qt::AlignHCenter },
        { QStringLiteral("mitte"), Qt::AlignHCenter },     { QStringLiteral("zentriert"), Qt::AlignHCenter },
        { QStringLiteral("justify"), Qt::AlignJustify },   { QStringLiteral("justified"), Qt::AlignJustify },
        { QStringLiteral("blocksatz"), Qt::AlignJustify },
    };

    const auto it = names.constFind(text.trimmed().toLower());
    if (it == names.constEnd()) {
        return false;
    }
    *alignment = *it;
    return true;
}

bool parseBoolean(const QString &value, bool *result)
{
    if (value.isEmpty()) {
        *result = true; // "bold" on its own means bold.
        return true;
    }
    static const QStringList yes { QStringLiteral("true"), QStringLiteral("yes"), QStringLiteral("on"),
                                   QStringLiteral("1"), QStringLiteral("ja") };
    static const QStringList no { QStringLiteral("false"), QStringLiteral("no"), QStringLiteral("off"),
                                  QStringLiteral("0"), QStringLiteral("nein") };
    const QString lower = value.trimmed().toLower();
    if (yes.contains(lower)) {
        *result = true;
        return true;
    }
    if (no.contains(lower)) {
        *result = false;
        return true;
    }
    return false;
}

/**
 * UTF-8, falling back to Latin-1 for bytes that are not.
 *
 * The same two-step Typeset::fromTextFile uses, repeated here for the text that
 * arrives on standard input. Letting QTextStream pick the local eight-bit codec
 * instead is how a German text file reaches the page with two wrong glyphs
 * wherever it had an umlaut.
 */
QString decodeUtf8(const QByteArray &bytes)
{
    QStringDecoder decoder(QStringConverter::Utf8);
    const QString text = decoder(bytes);
    return decoder.hasError() ? QString::fromLatin1(bytes) : text;
}

/**
 * The heading style Typeset itself would derive for @p level.
 *
 * Repeated here, and nowhere else, because Typeset derives every level a caller
 * leaves out and that derivation is private to it. Styling h3 on the command
 * line means handing over h1 and h2 as well, and filling those with a bare
 * Style would quietly flatten two levels nobody mentioned.
 */
ps::Typeset::Style derivedHeading(const ps::Typeset::Style &body, int level)
{
    constexpr double scale[] = { 1.9, 1.55, 1.3, 1.15, 1.05, 1.0 };
    const int index = qBound(1, level, 6) - 1;

    ps::Typeset::Style style = body;
    style.fontSize = body.fontSize * scale[index];
    style.leading = style.fontSize * 1.18;
    style.bold = true;
    style.italic = index >= 5;
    style.alignment = Qt::AlignLeft;
    style.indentFirst = 0.0;
    style.spaceBefore = index == 0 ? style.fontSize * 0.4 : style.fontSize * 0.9;
    style.spaceAfter = style.fontSize * 0.35;
    style.keepWithNext = true;
    return style;
}

/** "h1", "heading1" or "1"; zero when @p target is not a heading level. */
int headingLevelOf(const QString &target)
{
    QString digits = target;
    if (digits.startsWith(QLatin1String("heading"))) {
        digits = digits.mid(7);
    } else if (digits.startsWith(QLatin1Char('h'))) {
        digits = digits.mid(1);
    }
    bool ok = false;
    const int level = digits.toInt(&ok);
    return ok && level >= 1 && level <= 6 ? level : 0;
}

/** One "key=value,key=value" list applied to a single style. */
bool applyStyle(const QString &spec, ps::Typeset::Style *style, QString *error)
{
    const QStringList items = spec.split(QLatin1Char(','), Qt::SkipEmptyParts);
    if (items.isEmpty()) {
        *error = i18n("“%1” says nothing to change.", spec);
        return false;
    }

    static const QStringList switches { QStringLiteral("bold"), QStringLiteral("italic"),
                                        QStringLiteral("keep-with-next") };

    for (const QString &item : items) {
        const int equals = item.indexOf(QLatin1Char('='));
        QString key = (equals < 0 ? item : item.left(equals)).trimmed().toLower();
        const QString value = equals < 0 ? QString() : item.mid(equals + 1).trimmed();

        bool negated = false;
        if (key.startsWith(QLatin1String("no-")) && switches.contains(key.mid(3))) {
            negated = true;
            key = key.mid(3);
        }

        const auto onOff = [&](bool *target) {
            bool on = true;
            if (!parseBoolean(value, &on)) {
                *error = i18n("“%1” is not yes or no.", value);
                return false;
            }
            *target = negated ? !on : on;
            return true;
        };
        const auto length = [&](double *target) {
            if (!lengthInPoints(value, PointsToPoints, target)) {
                *error = i18n("“%1” is not a length.", value);
                return false;
            }
            return true;
        };

        if (key == QLatin1String("font") || key == QLatin1String("family") || key == QLatin1String("typeface")) {
            if (value.isEmpty()) {
                *error = i18n("“%1” needs a font name.", key);
                return false;
            }
            style->family = value;
        } else if (key == QLatin1String("size") || key == QLatin1String("font-size")) {
            if (!length(&style->fontSize)) {
                return false;
            }
        } else if (key == QLatin1String("leading")) {
            if (!length(&style->leading)) {
                return false;
            }
        } else if (key == QLatin1String("space-before")) {
            if (!length(&style->spaceBefore)) {
                return false;
            }
        } else if (key == QLatin1String("space-after")) {
            if (!length(&style->spaceAfter)) {
                return false;
            }
        } else if (key == QLatin1String("indent-first")) {
            if (!length(&style->indentFirst)) {
                return false;
            }
        } else if (key == QLatin1String("indent-left")) {
            if (!length(&style->indentLeft)) {
                return false;
            }
        } else if (key == QLatin1String("indent-right")) {
            if (!length(&style->indentRight)) {
                return false;
            }
        } else if (key == QLatin1String("bold")) {
            if (!onOff(&style->bold)) {
                return false;
            }
        } else if (key == QLatin1String("italic")) {
            if (!onOff(&style->italic)) {
                return false;
            }
        } else if (key == QLatin1String("keep-with-next")) {
            if (!onOff(&style->keepWithNext)) {
                return false;
            }
        } else if (key == QLatin1String("align") || key == QLatin1String("alignment")) {
            if (!parseAlignment(value, &style->alignment)) {
                *error = i18n("“%1” is not an alignment. Try left, right, centre or justify.", value);
                return false;
            }
        } else if (key == QLatin1String("colour") || key == QLatin1String("color")) {
            const QColor parsed(value);
            if (!parsed.isValid()) {
                *error = i18n("“%1” is not a colour. Try a name or #rrggbb.", value);
                return false;
            }
            style->colour = parsed;
        } else {
            *error = i18n("“%1” is not something a style has. Try font, size, leading, colour, bold, italic, align, "
                          "space-before, space-after, indent-first, indent-left, indent-right or keep-with-next.",
                          key);
            return false;
        }
    }
    return true;
}

/** "body:size=11" or "h2:bold,size=16", applied to the right style of @p document. */
bool applyStyleSpec(const QString &spec, ps::Typeset::Document *document, QString *error)
{
    const int colon = spec.indexOf(QLatin1Char(':'));
    if (colon <= 0) {
        *error = i18n("“%1” is not a style. The form is body:size=11 or h1:bold,size=20.", spec);
        return false;
    }
    const QString target = spec.left(colon).trimmed().toLower();
    const QString rest = spec.mid(colon + 1);

    if (target == QLatin1String("body") || target == QLatin1String("text")) {
        return applyStyle(rest, &document->body, error);
    }

    const int level = headingLevelOf(target);
    if (level < 1) {
        *error = i18n("“%1” is not a part of the document. Use body, or h1 to h6.", target);
        return false;
    }
    while (document->headings.size() < level) {
        document->headings.append(derivedHeading(document->body, document->headings.size() + 1));
    }
    return applyStyle(rest, &document->headings[level - 1], error);
}

/** Reads a length option, leaving @p points untouched when the option was not typed. */
bool readLength(const QCommandLineParser &parser, const QString &name, double fallbackFactor, double *points, int *code)
{
    const std::optional<QString> value = typed(parser, name);
    if (!value) {
        return true;
    }
    if (!lengthInPoints(*value, fallbackFactor, points)) {
        *code = fail(i18n("“%1” is not a length for --%2. Try 12, 12mm or 34pt.", *value, name), UsageError);
        return false;
    }
    return true;
}

/**
 * Turns down a margin named in points, rather than reading it as millimetres.
 *
 * These five names were read here as millimetres and mean points everywhere else
 * on this command line: --margin stamps a signature 36 points from the edge,
 * --left trims a crop box in points. One word for two lengths is a trap, and
 * quietly taking the millimetre reading away would shrink somebody's margins to
 * a third of what their script has been producing for months without a word. So
 * the line is refused and the millimetre spelling is named.
 */
bool refusesAMarginInPoints(const QCommandLineParser &parser, int *code)
{
    static const QStringList moved { QStringLiteral("margin"), QStringLiteral("top"), QStringLiteral("bottom"),
                                     QStringLiteral("left"), QStringLiteral("right") };
    for (const QString &name : moved) {
        if (!typed(parser, name)) {
            continue;
        }
        *code = fail(i18n("typeset measures paper in millimetres, so its margin is --%1-mm. --%1 is points here as "
                          "everywhere else, and belongs to the commands that stamp and trim.",
                          name),
                     UsageError);
        return true;
    }
    return false;
}

/** Reads a plain number option, leaving @p number untouched when it was not typed. */
bool readNumber(const QCommandLineParser &parser, const QString &name, double *number, int *code)
{
    const std::optional<QString> value = typed(parser, name);
    if (!value) {
        return true;
    }
    bool ok = false;
    const double parsed = value->trimmed().toDouble(&ok);
    if (!ok) {
        *code = fail(i18n("“%1” is not a number for --%2.", *value, name), UsageError);
        return false;
    }
    *number = parsed;
    return true;
}

int printNotes(const QStringList &notes, bool asJson)
{
    if (asJson) {
        out() << QString::fromUtf8(QJsonDocument(QJsonArray::fromStringList(notes)).toJson(QJsonDocument::Indented));
        return Success;
    }
    for (const QString &note : notes) {
        out() << QStringLiteral("  · ") << note << Qt::endl;
    }
    return Success;
}

// ── typeset ───────────────────────────────────────────────────────────────

/** Everything the command line has to say about how the pages should look. */
bool buildDocument(const QCommandLineParser &parser, ps::Typeset::Document *document, int *code)
{
    if (refusesAMarginInPoints(parser, code)) {
        return false;
    }

    if (const std::optional<QString> value = typed(parser, QStringLiteral("size"))) {
        if (!parsePageSize(*value, &document->pageSize)) {
            *code = fail(i18n("“%1” is not a page size. Try a4, a3, a5, letter, legal, or a pair with its unit "
                              "written, as 210x297mm or 595x842pt.",
                              *value),
                         UsageError);
            return false;
        }
    }

    // One --margin-mm sets all four sides; --top-mm and the rest then override a
    // side each. The unit is in the name because it is millimetres, which is how
    // people describe a margin on paper and not what --margin means anywhere
    // else here; a number may still carry its own unit and be taken at its word.
    double margin = 0.0;
    if (!readLength(parser, QStringLiteral("margin-mm"), MillimetresToPoints, &margin, code)) {
        return false;
    }
    if (typed(parser, QStringLiteral("margin-mm"))) {
        document->marginTop = document->marginBottom = document->marginLeft = document->marginRight = margin;
    }
    if (!readLength(parser, QStringLiteral("top-mm"), MillimetresToPoints, &document->marginTop, code)
        || !readLength(parser, QStringLiteral("bottom-mm"), MillimetresToPoints, &document->marginBottom, code)
        || !readLength(parser, QStringLiteral("left-mm"), MillimetresToPoints, &document->marginLeft, code)
        || !readLength(parser, QStringLiteral("right-mm"), MillimetresToPoints, &document->marginRight, code)) {
        return false;
    }

    double columns = document->columns;
    if (!readNumber(parser, QStringLiteral("columns"), &columns, code)) {
        return false;
    }
    if (columns < 1 || columns > 12) {
        *code = fail(i18n("Columns run from 1 to 12."), UsageError);
        return false;
    }
    document->columns = int(columns);

    // --gutter is the nup command's word for the same gap; borrowing it beats
    // inventing a second name for "space between two things side by side".
    if (!readLength(parser, QStringLiteral("gutter"), PointsToPoints, &document->columnGap, code)) {
        return false;
    }

    document->header = parser.value(QStringLiteral("header"));
    document->footer = parser.value(QStringLiteral("footer"));
    if (!readLength(parser, QStringLiteral("header-size"), PointsToPoints, &document->headerSize, code)) {
        return false;
    }

    document->title = parser.value(QStringLiteral("title"));
    document->author = parser.value(QStringLiteral("author-name"));
    document->hyphenate = parser.isSet(QStringLiteral("hyphenate"));
    if (const std::optional<QString> value = typed(parser, QStringLiteral("language"))) {
        document->language = *value;
    }

    // --font belongs to the font commands; this reads it rather than declaring
    // it a second time, and --typeface is the name to reach for when one line
    // has to say both which font to report on and which font to set in.
    if (const std::optional<QString> value = typed(parser, QStringLiteral("font"))) {
        document->body.family = *value;
    }
    if (const std::optional<QString> value = typed(parser, QStringLiteral("typeface"))) {
        document->body.family = *value;
    }
    if (!readLength(parser, QStringLiteral("font-size"), PointsToPoints, &document->body.fontSize, code)
        || !readLength(parser, QStringLiteral("leading"), PointsToPoints, &document->body.leading, code)
        || !readLength(parser, QStringLiteral("space-before"), PointsToPoints, &document->body.spaceBefore, code)
        || !readLength(parser, QStringLiteral("space-after"), PointsToPoints, &document->body.spaceAfter, code)
        || !readLength(parser, QStringLiteral("indent-first"), PointsToPoints, &document->body.indentFirst, code)
        || !readLength(parser, QStringLiteral("indent-left"), PointsToPoints, &document->body.indentLeft, code)
        || !readLength(parser, QStringLiteral("indent-right"), PointsToPoints, &document->body.indentRight, code)) {
        return false;
    }
    document->body.bold = parser.isSet(QStringLiteral("bold"));
    document->body.italic = parser.isSet(QStringLiteral("italic"));

    if (const std::optional<QString> value = typed(parser, QStringLiteral("colour"))) {
        const QColor colour(*value);
        if (!colour.isValid()) {
            *code = fail(i18n("“%1” is not a colour. Try a name or #rrggbb.", *value), UsageError);
            return false;
        }
        document->body.colour = colour;
    }
    if (const std::optional<QString> value = typed(parser, QStringLiteral("align"))) {
        if (!parseAlignment(*value, &document->body.alignment)) {
            *code = fail(i18n("“%1” is not an alignment. Try left, right, centre or justify.", *value), UsageError);
            return false;
        }
    }

    // The body first, whatever order the specs were given in: a heading level
    // the caller has not styled is derived from the body, and deriving it from
    // a body that is about to change would set the headings in the wrong font.
    const QStringList specs = parser.values(QStringLiteral("style"));
    for (int pass = 0; pass < 2; ++pass) {
        for (const QString &spec : specs) {
            const bool isBody = spec.startsWith(QLatin1String("body:"), Qt::CaseInsensitive)
                || spec.startsWith(QLatin1String("text:"), Qt::CaseInsensitive);
            if (isBody != (pass == 0)) {
                continue;
            }
            QString error;
            if (!applyStyleSpec(spec, document, &error)) {
                *code = fail(error, UsageError);
                return false;
            }
        }
    }
    return true;
}

int commandMeasure(const QStringList &arguments, const QCommandLineParser &parser)
{
    const ps::Typeset::Style defaults;

    QString text = parser.value(QStringLiteral("text"));
    if (text.isEmpty() && !arguments.isEmpty()) {
        text = arguments.constFirst(); // "typeset measure Hello" is the obvious thing to try.
    }
    if (text.isEmpty()) {
        return fail(i18n("measure needs some text (--text)."), UsageError);
    }

    QString family = defaults.family;
    if (const std::optional<QString> value = typed(parser, QStringLiteral("font"))) {
        family = *value;
    }
    if (const std::optional<QString> value = typed(parser, QStringLiteral("typeface"))) {
        family = *value;
    }

    double fontSize = defaults.fontSize;
    int code = Success;
    if (!readLength(parser, QStringLiteral("font-size"), PointsToPoints, &fontSize, &code)) {
        return code;
    }
    if (fontSize <= 0.0) {
        return fail(i18n("A font size has to be more than nothing."), UsageError);
    }

    const bool bold = parser.isSet(QStringLiteral("bold"));
    const bool italic = parser.isSet(QStringLiteral("italic"));
    const double width = ps::Typeset::textWidth(text, family, bold, italic, fontSize);

    if (parser.isSet(QStringLiteral("json"))) {
        const QJsonObject root { { QStringLiteral("text"), text },
                                 { QStringLiteral("font"), family },
                                 { QStringLiteral("bold"), bold },
                                 { QStringLiteral("italic"), italic },
                                 { QStringLiteral("fontSize"), fontSize },
                                 { QStringLiteral("widthPoints"), width },
                                 { QStringLiteral("widthMillimetres"), width / MillimetresToPoints } };
        out() << QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Indented));
        return Success;
    }

    out() << i18nc("@info a measured width in points and in millimetres", "%1 pt  (%2 mm)",
                   QString::number(width, 'f', 2), QString::number(width / MillimetresToPoints, 'f', 2))
          << Qt::endl;
    return Success;
}

enum class Reading { Auto, Plain, Markdown };

int commandTypeset(const QStringList &arguments, const QCommandLineParser &parser)
{
    const bool asJson = parser.isSet(QStringLiteral("json"));
    if (parser.isSet(QStringLiteral("limits"))) {
        return printNotes(ps::Typeset::limitations(), asJson);
    }

    QStringList rest = arguments;
    Reading reading = Reading::Auto;

    // An action word only counts when nothing of that name is on the disk: a
    // file really called "measure" is still a file, and losing it to a verb
    // would be the sort of surprise that costs an afternoon.
    if (!rest.isEmpty() && !QFileInfo::exists(rest.constFirst())) {
        const QString first = rest.constFirst().toLower();
        if (first == QLatin1String("measure")) {
            rest.removeFirst();
            return commandMeasure(rest, parser);
        }
        if (first == QLatin1String("text") || first == QLatin1String("plain")) {
            reading = Reading::Plain;
            rest.removeFirst();
        } else if (first == QLatin1String("markdown") || first == QLatin1String("md")) {
            reading = Reading::Markdown;
            rest.removeFirst();
        }
    }

    if (rest.isEmpty()) {
        return fail(i18n("typeset needs a text file, or “-” to read standard input."), UsageError);
    }
    if (rest.size() > 1) {
        return fail(i18n("typeset sets one file at a time."), UsageError);
    }

    // oneInputFile() insists the path exists, which "-" never does, so standard
    // input is sorted out here and everything else goes through the shared
    // check for the message it gives when a file is missing or unreadable.
    const bool fromStdin = rest.constFirst() == QLatin1String("-");
    QString input;
    if (!fromStdin) {
        int code = Success;
        if (!oneInputFile(rest, QStringLiteral("typeset"), &input, &code)) {
            return code;
        }
    }

    QString output = parser.value(QStringLiteral("output"));
    if (fromStdin) {
        if (output.isEmpty()) {
            return fail(i18n("Reading from standard input needs an output file (-o)."), UsageError);
        }
    } else {
        output = outputOr(output, input, QStringLiteral(".pdf"));
    }

    ps::Typeset::Document document;
    int code = Success;
    if (!buildDocument(parser, &document, &code)) {
        return code;
    }

    ps::Typeset::Report report;
    QString error;
    bool ok = false;

    if (fromStdin || reading != Reading::Auto) {
        QString source;
        if (fromStdin) {
            QFile stream;
            if (!stream.open(stdin, QIODevice::ReadOnly)) {
                return fail(i18n("Standard input could not be read."));
            }
            source = decodeUtf8(stream.readAll());
        } else {
            QFile file(input);
            if (!file.open(QIODevice::ReadOnly)) {
                return fail(i18n("“%1” could not be read.", input));
            }
            source = decodeUtf8(file.readAll());
        }

        // Markdown by name is Typeset's own rule for a file; asked for a reading
        // explicitly, or handed a stream with no name at all, the caller decides.
        ok = reading == Reading::Markdown ? ps::Typeset::fromMarkdown(source, output, document, &report, &error)
                                          : ps::Typeset::fromPlainText(source, output, document, &report, &error);
    } else {
        ok = ps::Typeset::fromTextFile(input, output, document, &report, &error);
    }

    if (!ok) {
        return fail(error, OutputError);
    }

    if (asJson) {
        const QJsonObject root { { QStringLiteral("output"), output },
                                 { QStringLiteral("pages"), report.pages },
                                 { QStringLiteral("paragraphs"), report.paragraphs },
                                 { QStringLiteral("notes"), QJsonArray::fromStringList(report.overflows) } };
        out() << QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Indented));
        return Success;
    }

    out() << i18np("Set %1 paragraph.", "Set %1 paragraphs.", report.paragraphs) << Qt::endl;
    out() << i18np("Wrote %1 page to %2.", "Wrote %1 pages to %2.", report.pages, output) << Qt::endl;

    // Notes, not a failure: the document is on disk and readable. They say what
    // a reader would notice (a word broken to make it fit, a character the
    // standard fonts cannot write), and a hyphenated web address is no reason
    // to stop a script that runs under "set -e".
    for (const QString &note : report.overflows) {
        err() << i18n("Note: %1", note) << Qt::endl;
    }
    return Success;
}

// ── to-text, to-markdown, to-html ─────────────────────────────────────────

enum class Flavour { Text, Markdown, Html };

int commandExtract(Flavour flavour, const QString &verb, const QStringList &arguments, const QCommandLineParser &parser)
{
    const bool asJson = parser.isSet(QStringLiteral("json"));
    if (parser.isSet(QStringLiteral("limits"))) {
        return printNotes(ps::Convert::limitations(), asJson);
    }

    QString input;
    int code = Success;
    if (!oneInputFile(arguments, verb, &input, &code)) {
        return code;
    }

    ps::Convert::TextOptions options;
    options.preserveLayout = !parser.isSet(QStringLiteral("no-layout"));
    if (!readLength(parser, QStringLiteral("line-tolerance"), PointsToPoints, &options.lineTolerance, &code)
        || !readNumber(parser, QStringLiteral("paragraph-factor"), &options.paragraphFactor, &code)) {
        return code;
    }

    if (const std::optional<QString> range = typed(parser, QStringLiteral("pages"))) {
        // Opened once here only to learn how long it is, which is what "last"
        // and "1-" in a range have to be measured against.
        ps::PopplerBackend backend;
        ps::Document document;
        document.setRenderBackend(&backend);
        QString error;
        if (!document.open(input, &error)) {
            return fail(error);
        }
        options.pages = ps::PageRange::parse(*range, document.pageCount(), &error);
        if (options.pages.isEmpty()) {
            return fail(error, UsageError);
        }
    }

    const QString suffix = flavour == Flavour::Text ? QStringLiteral(".txt")
        : flavour == Flavour::Markdown              ? QStringLiteral(".md")
                                                    : QStringLiteral(".html");
    const QString given = parser.value(QStringLiteral("output"));
    const bool toStdout = given == QLatin1String("-");

    // Convert writes files, which is awkward in a pipeline, so "-o -" runs it
    // into a temporary and copies the result out.
    QTemporaryFile scratch(QDir::tempPath() + QStringLiteral("/pdf-smithy-XXXXXX") + suffix);
    QString target;
    if (toStdout) {
        if (!scratch.open()) {
            return fail(i18n("A temporary file could not be made."), OutputError);
        }
        scratch.close();
        target = scratch.fileName();
    } else {
        target = outputOr(given, input, suffix);
    }

    QString error;
    bool ok = false;
    switch (flavour) {
    case Flavour::Text:
        ok = ps::Convert::toText(input, target, options, &error);
        break;
    case Flavour::Markdown:
        ok = ps::Convert::toMarkdown(input, target, options, &error);
        break;
    case Flavour::Html:
        ok = ps::Convert::toHtml(input, target, options, &error);
        break;
    }
    if (!ok) {
        return fail(error, OutputError);
    }

    if (toStdout) {
        QFile written(target);
        if (!written.open(QIODevice::ReadOnly)) {
            return fail(i18n("The converted text could not be read back."), OutputError);
        }
        out() << decodeUtf8(written.readAll());
        out().flush();
        return Success;
    }

    out() << i18n("Wrote %1  (%2)", target, humanSize(QFileInfo(target).size())) << Qt::endl;
    return Success;
}

// ── The group ─────────────────────────────────────────────────────────────

void options(QCommandLineParser &parser)
{
    // --size, --gutter, --font, --font-size, --text, --title, --colour,
    // --author-name, --pages, --json and -o all belong to other commands and are
    // read by name. The four paper margins are not among them: they are
    // millimetres and those names are points, so this group declares its own.
    parser.addOptions({
        { QStringLiteral("margin-mm"), i18n("Margin on all four sides, in millimetres."), QStringLiteral("mm") },
        { QStringLiteral("top-mm"), i18n("Top margin, in millimetres. Overrides --margin-mm."), QStringLiteral("mm") },
        { QStringLiteral("bottom-mm"), i18n("Bottom margin, in millimetres. Overrides --margin-mm."),
          QStringLiteral("mm") },
        { QStringLiteral("left-mm"), i18n("Left margin, in millimetres. Overrides --margin-mm."),
          QStringLiteral("mm") },
        { QStringLiteral("right-mm"), i18n("Right margin, in millimetres. Overrides --margin-mm."),
          QStringLiteral("mm") },
        { QStringLiteral("typeface"),
          i18n("Font to set the body in: Helvetica, Times, Courier, or a name that suggests one."),
          QStringLiteral("name") },
        { QStringLiteral("leading"), i18n("Distance between baselines, in points. Default: 1.35 times the size."),
          QStringLiteral("points") },
        { QStringLiteral("align"), i18n("How the body is set: left, right, centre or justify."),
          QStringLiteral("how") },
        { QStringLiteral("columns"), i18n("Set the text in this many columns, 1 to 12."), QStringLiteral("number") },
        { QStringLiteral("bold"), i18n("Set the body in bold.") },
        { QStringLiteral("italic"), i18n("Set the body in italic.") },
        { QStringLiteral("space-before"), i18n("Space above each paragraph, in points."), QStringLiteral("points") },
        { QStringLiteral("space-after"), i18n("Space below each paragraph, in points."), QStringLiteral("points") },
        { QStringLiteral("indent-first"), i18n("Indent for the first line of a paragraph, in points."),
          QStringLiteral("points") },
        { QStringLiteral("indent-left"), i18n("Indent for the whole paragraph from the left, in points."),
          QStringLiteral("points") },
        { QStringLiteral("indent-right"), i18n("Indent for the whole paragraph from the right, in points."),
          QStringLiteral("points") },
        { QStringLiteral("header"), i18n("Running head. {page}, {pages}, {title} and {date} are filled in."),
          QStringLiteral("text") },
        { QStringLiteral("footer"), i18n("Running foot, with the same placeholders as --header."),
          QStringLiteral("text") },
        { QStringLiteral("header-size"), i18n("Text size of the running head and foot, in points."),
          QStringLiteral("points") },
        { QStringLiteral("hyphenate"), i18n("Break long words at the end of a line.") },
        { QStringLiteral("language"), i18n("Language the text is in, which decides how it hyphenates, e.g. de."),
          QStringLiteral("code") },
        { QStringLiteral("style"),
          i18n("Style one part, as body:size=11,align=justify or h1:bold,size=20. Repeat for more."),
          QStringLiteral("spec") },
        { QStringLiteral("limits"), i18n("Say what this command cannot promise, and do nothing else.") },
        { QStringLiteral("no-layout"), i18n("Take the text as it was drawn, without paragraphs or columns.") },
        { QStringLiteral("line-tolerance"), i18n("Baselines closer together than this many points are one line."),
          QStringLiteral("points") },
        { QStringLiteral("paragraph-factor"), i18n("A gap this many times the text size starts a new paragraph."),
          QStringLiteral("number") },
    });
}

std::optional<int> run(const QString &verb, const QStringList &arguments, const QCommandLineParser &parser)
{
    if (verb == QLatin1String("typeset")) {
        return commandTypeset(arguments, parser);
    }
    if (verb == QLatin1String("to-text")) {
        return commandExtract(Flavour::Text, verb, arguments, parser);
    }
    if (verb == QLatin1String("to-markdown")) {
        return commandExtract(Flavour::Markdown, verb, arguments, parser);
    }
    if (verb == QLatin1String("to-html")) {
        return commandExtract(Flavour::Html, verb, arguments, parser);
    }
    return std::nullopt;
}

QString helpLine(const QString &usage, const QString &what)
{
    return QStringLiteral("  %1  %2").arg(usage.leftJustified(34), what);
}

QStringList help()
{
    return {
        helpLine(QStringLiteral("typeset   FILE|-   [-o OUT]"), i18n("Set a text or Markdown file as pages")),
        helpLine(QStringLiteral("typeset   measure  --text T"), i18n("Measure how wide text would be set")),
        helpLine(QStringLiteral("to-text   FILE     [-o OUT]"), i18n("Read the words back out as plain text")),
        helpLine(QStringLiteral("to-markdown FILE   [-o OUT]"), i18n("Read the words back out as Markdown")),
        helpLine(QStringLiteral("to-html   FILE     [-o OUT]"), i18n("Read the words back out as one HTML file")),
    };
}

} // namespace

Group typesetGroup()
{
    return { &options, &run, &help };
}

} // namespace ps::cli
