/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later

    A page as things a person can point at, on the command line.

    A PDF page is a list of drawing instructions, so there is nothing in the file
    to address. PageObjects invents the handle: every painting operation gets an
    index, a boundary and a place in the stacking order. Every command here takes
    that index, which is why “objects list” prints it in the first column and why
    “objects at” exists at all: it is what clicking looks like when there is no
    mouse.

    Coordinates are display points with the origin at the bottom left, the way
    the page itself is measured. Everybody assumes the top left once.
*/
#include "Commands.h"

#include "core/PageObjects.h"

#include <QCommandLineParser>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>

#include <KLocalizedString>

#include <algorithm>

using namespace Qt::Literals::StringLiterals;

namespace ps::cli {

namespace {

/** One measurement, printed the way a page is measured: points, one decimal. */
QString points(double value)
{
    return QString::number(value, 'f', 1);
}

QString colourText(const QColor &colour)
{
    return colour.isValid() ? colour.name() : i18nc("@item no colour is set", "none");
}

/** A name a script may match on, which must not follow the interface language. */
QString kindToken(PageObject::Kind kind)
{
    switch (kind) {
    case PageObject::Kind::Text:
        return u"text"_s;
    case PageObject::Kind::Image:
        return u"image"_s;
    case PageObject::Kind::Path:
        return u"path"_s;
    case PageObject::Kind::Drawing:
        return u"drawing"_s;
    case PageObject::Kind::Shading:
        return u"shading"_s;
    case PageObject::Kind::InlineImage:
        return u"inlineImage"_s;
    }
    return u"path"_s;
}

/** The part of an object that only its kind has. */
QString detailOf(const PageObject &object)
{
    switch (object.kind) {
    case PageObject::Kind::Text:
        return i18nc(
            "@info:shell what a text object says, in which font and at which size", "“%1”  %2 %3 pt", object.text,
            object.fontResource.isEmpty() ? i18nc("@item the object names no font", "none") : object.fontResource,
            points(object.fontSize));
    case PageObject::Kind::Image:
    case PageObject::Kind::InlineImage:
        return i18nc("@info:shell a picture: its resource name and how many pixels it holds", "%1  %2 × %3 px",
                     object.resourceName.isEmpty() ? i18nc("@item the picture has no resource name", "none")
                                                   : object.resourceName,
                     object.pixelSize.width(), object.pixelSize.height());
    case PageObject::Kind::Drawing:
    case PageObject::Kind::Shading:
        return object.resourceName.isEmpty() ? QString() : object.resourceName;
    case PageObject::Kind::Path:
        return i18nc("@info:shell how a shape is painted", "fill %1  stroke %2  %3 pt", colourText(object.fill),
                     colourText(object.stroke), points(object.lineWidth));
    }
    return {};
}

/** One row of the table, so that the heading and the rows cannot drift apart. */
QString row(const QString &index, const QString &kind, const QString &x, const QString &y, const QString &width,
            const QString &height, const QString &layer, const QString &detail)
{
    return index.rightJustified(4) + u"  "_s + kind.leftJustified(13) + x.rightJustified(8) + y.rightJustified(8)
        + width.rightJustified(8) + height.rightJustified(8) + layer.rightJustified(7) + u"  "_s + detail;
}

void printObject(const PageObject &object)
{
    out() << row(QString::number(object.index), PageObjects::describe(object.kind), points(object.bounds.x()),
                 points(object.bounds.y()), points(object.bounds.width()), points(object.bounds.height()),
                 QString::number(object.layer), detailOf(object))
          << Qt::endl;
}

void printHeading()
{
    // Short on purpose: these are columns, and a long word here pushes the
    // numbers out from under their own heading.
    out() << row(i18nc("@title:column the number that names an object", "No"),
                 i18nc("@title:column what kind of thing it is", "Kind"),
                 i18nc("@title:column left edge, in points", "x"),
                 i18nc("@title:column bottom edge, in points", "y"), i18nc("@title:column width, in points", "w"),
                 i18nc("@title:column height, in points", "h"),
                 i18nc("@title:column place in the stacking order", "Layer"),
                 i18nc("@title:column what only this kind of object has", "Detail"))
          << Qt::endl;
}

QJsonObject encode(const PageObject &object)
{
    QJsonObject json {
        { u"index"_s, object.index },
        { u"page"_s, object.page + 1 },
        // Both: the token for scripts, the label for people.
        { u"kind"_s, kindToken(object.kind) },
        { u"kindLabel"_s, PageObjects::describe(object.kind) },
        { u"bounds"_s,
          QJsonObject { { u"x"_s, object.bounds.x() },
                        { u"y"_s, object.bounds.y() },
                        { u"width"_s, object.bounds.width() },
                        { u"height"_s, object.bounds.height() } } },
        { u"layer"_s, object.layer },
        { u"placement"_s,
          QJsonArray { object.placement.m11(), object.placement.m12(), object.placement.m21(), object.placement.m22(),
                       object.placement.dx(), object.placement.dy() } },
    };

    switch (object.kind) {
    case PageObject::Kind::Text:
        json.insert(u"text"_s, object.text);
        json.insert(u"font"_s, object.fontResource);
        json.insert(u"fontSize"_s, object.fontSize);
        break;
    case PageObject::Kind::Image:
    case PageObject::Kind::InlineImage:
        json.insert(u"resource"_s, object.resourceName);
        json.insert(u"pixelWidth"_s, object.pixelSize.width());
        json.insert(u"pixelHeight"_s, object.pixelSize.height());
        break;
    case PageObject::Kind::Drawing:
    case PageObject::Kind::Shading:
        json.insert(u"resource"_s, object.resourceName);
        break;
    case PageObject::Kind::Path:
        json.insert(u"fill"_s, object.fill.isValid() ? QJsonValue(object.fill.name()) : QJsonValue());
        json.insert(u"stroke"_s, object.stroke.isValid() ? QJsonValue(object.stroke.name()) : QJsonValue());
        json.insert(u"lineWidth"_s, object.lineWidth);
        // The count, not the curves: the outline is an approximation for hit
        // testing, and handing it out as geometry would invite trusting it.
        json.insert(u"outlineElements"_s, object.outline.elementCount());
        break;
    }
    return json;
}

void printJson(const QVector<PageObject> &objects)
{
    QJsonArray array;
    for (const PageObject &object : objects) {
        array.append(encode(object));
    }
    out() << QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Indented));
}

// ── reading the arguments ─────────────────────────────────────────────────

/** "X,Y" or "X,Y,W,H" and so on, in display points. */
bool parseNumbers(const QString &spec, int wanted, double *values)
{
    const QStringList parts = spec.split(u',');
    if (parts.size() != wanted) {
        return false;
    }
    for (int i = 0; i < wanted; ++i) {
        bool ok = false;
        values[i] = parts.at(i).trimmed().toDouble(&ok);
        if (!ok) {
            return false;
        }
    }
    return true;
}

bool wantedPage(const QCommandLineParser &parser, int *page, int *code)
{
    bool ok = false;
    const int number = parser.value(u"page"_s).toInt(&ok);
    if (!ok || number < 1) {
        *code = fail(i18n("“%1” is not a page number.", parser.value(u"page"_s)), UsageError);
        return false;
    }
    *page = number - 1;
    return true;
}

/**
 * Every --object, from all of them, in one list.
 *
 * They are gathered rather than acted on one at a time because an edit rewrites
 * the page's instruction stream: after deleting object 3, what used to be object
 * 5 is object 4. Two runs would number the second edit against a page the first
 * one had already changed, so "delete 3 and 5" would take out the wrong shape.
 * One call, one numbering.
 */
bool wantedObjects(const QCommandLineParser &parser, QVector<int> *objects, int *code)
{
    const QStringList given = parser.values(u"object"_s);
    for (const QString &value : given) {
        const QStringList parts = value.split(u',', Qt::SkipEmptyParts);
        for (const QString &part : parts) {
            bool ok = false;
            const int index = part.trimmed().toInt(&ok);
            if (!ok || index < 0) {
                *code = fail(i18n("“%1” is not an object number. Use the number in the first column of "
                                  "“objects list”.",
                                  part.trimmed()),
                             UsageError);
                return false;
            }
            objects->append(index);
        }
    }
    if (objects->isEmpty()) {
        *code = fail(i18n("Say which object with --object. “objects list FILE --page N” prints the numbers."),
                     UsageError);
        return false;
    }
    return true;
}

bool objectsOn(const QString &input, int page, QVector<PageObject> *objects, int *code)
{
    QString error;
    *objects = PageObjects::read(input, page, &error);
    if (!error.isEmpty()) {
        *code = fail(error);
        return false;
    }
    return true;
}

const PageObject *find(const QVector<PageObject> &objects, int index)
{
    for (const PageObject &object : objects) {
        if (object.index == index) {
            return &object;
        }
    }
    return nullptr;
}

/**
 * Refuses a number the page does not have.
 *
 * The composer simply finds nothing to change and reports zero, which reads as
 * "it did not work" rather than "you asked for something that is not there".
 */
bool knownObjects(const QVector<PageObject> &objects, const QVector<int> &wanted, int page, int *code)
{
    for (const int index : wanted) {
        if (find(objects, index)) {
            continue;
        }
        if (objects.isEmpty()) {
            *code = fail(i18n("Page %1 draws nothing, so there is no object %2 on it.", page + 1, index), UsageError);
        } else {
            *code = fail(i18n("Page %1 has no object %2. Its numbers run from 0 to %3.", page + 1, index,
                              objects.size() - 1),
                         UsageError);
        }
        return false;
    }
    return true;
}

/** A colour option that may be absent, in which case the value is left alone. */
bool wantedColour(const QCommandLineParser &parser, const QString &option, QColor *colour, int *code)
{
    if (!parser.isSet(option)) {
        return true;
    }
    const QColor parsed(parser.value(option));
    if (!parsed.isValid()) {
        *code = fail(i18n("“%1” is not a colour. Try a name or #rrggbb.", parser.value(option)), UsageError);
        return false;
    }
    *colour = parsed;
    return true;
}

// ── writing the result ────────────────────────────────────────────────────

/**
 * Prints what happened, refusals and all.
 *
 * The counts alone would let a run that changed nothing look like a success. A
 * refusal that is not shown is the difference between a tool people trust and a
 * tool that quietly does nothing.
 */
int reportOn(const PageComposer::Report &report, const QString &output)
{
    out() << i18nc("@info what one editing pass did", "Moved or resized %1, deleted %2, restyled %3, added %4.",
                   report.transformed, report.deleted, report.restyled, report.inserted)
          << Qt::endl;

    for (const QString &refusal : report.refusals) {
        err() << i18n("Note: %1", refusal) << Qt::endl;
    }

    if (report.transformed + report.deleted + report.restyled + report.inserted == 0) {
        out() << i18n("Nothing on the page changed. The file was still written, unchanged.") << Qt::endl;
    }
    out() << i18n("Wrote %1.", output) << Qt::endl;
    return report.refusals.isEmpty() ? Success : InputError;
}

/** One apply() for the whole run: see wantedObjects() for why that matters. */
int compose(const QString &input, const QString &output, const QVector<PageEdit> &edits,
            const QVector<NewContent> &insertions)
{
    PageComposer::Report report;
    QString error;
    if (!PageComposer::apply(input, output, edits, insertions, &report, &error)) {
        return fail(error, OutputError);
    }
    return reportOn(report, output);
}

QString outputFor(const QCommandLineParser &parser, const QString &input)
{
    return outputOr(parser.value(u"output"_s), input, u"-edited.pdf"_s);
}

// ── the commands ──────────────────────────────────────────────────────────

int commandList(const QStringList &arguments, const QCommandLineParser &parser)
{
    QString input;
    int code = Success;
    if (!oneInputFile(arguments, u"objects list"_s, &input, &code)) {
        return code;
    }

    // --page all reaches readAll(), for an inventory of the whole document.
    const QString wanted = parser.value(u"page"_s).trimmed().toLower();
    const bool everyPage = wanted == QLatin1String("all") || wanted == QLatin1String("alle");
    int page = 0;
    if (!everyPage && !wantedPage(parser, &page, &code)) {
        return code;
    }

    QString error;
    const QVector<PageObject> objects
        = everyPage ? PageObjects::readAll(input, &error) : PageObjects::read(input, page, &error);
    if (!error.isEmpty()) {
        return fail(error);
    }

    if (parser.isSet(u"json"_s)) {
        printJson(objects);
        return Success;
    }

    if (objects.isEmpty()) {
        out() << (everyPage ? i18n("This document draws nothing at all.")
                            : i18n("Page %1 draws nothing.", page + 1))
              << Qt::endl;
        return Success;
    }

    if (!everyPage) {
        printHeading();
    }
    int shownPage = -1;
    for (const PageObject &object : objects) {
        // A page column would push the handle out of the first place, which it
        // has to keep, so whole-document listings announce each page instead.
        if (everyPage && object.page != shownPage) {
            shownPage = object.page;
            out() << Qt::endl << i18nc("@title heading above the objects of one page", "Page %1", shownPage + 1)
                  << Qt::endl;
            printHeading();
        }
        printObject(object);
    }

    out() << i18np("%1 object. The number in the first column is what --object takes.",
                   "%1 objects. The number in the first column is what --object takes.", objects.size())
          << Qt::endl;
    out() << i18n("Boundaries are in points from the bottom-left corner of the page.") << Qt::endl;
    return Success;
}

int commandAt(const QStringList &arguments, const QCommandLineParser &parser)
{
    QString input;
    int code = Success;
    if (!oneInputFile(arguments, u"objects at"_s, &input, &code)) {
        return code;
    }
    int page = 0;
    if (!wantedPage(parser, &page, &code)) {
        return code;
    }

    // --at carries a corner name for the stamping commands, so only an explicit
    // one is a point; its default would otherwise silently mean "bottom-right".
    const bool byPoint = parser.isSet(u"at"_s);
    const bool byArea = parser.isSet(u"area"_s);
    if (byPoint == byArea) {
        return fail(i18n("Give a point with --at X,Y, or an area with --area X,Y,WIDTH,HEIGHT, in points from the "
                         "bottom-left corner of the page."),
                    UsageError);
    }

    QPointF where;
    QRectF area;
    if (byPoint) {
        double values[2] = { 0, 0 };
        if (!parseNumbers(parser.value(u"at"_s), 2, values)) {
            return fail(i18n("“%1” is not a point. The form is X,Y, in points from the bottom-left corner of the "
                             "page.",
                             parser.value(u"at"_s)),
                        UsageError);
        }
        where = QPointF(values[0], values[1]);
    } else {
        // redact spells an area PAGE:X,Y,WIDTH,HEIGHT, and the same flag in the
        // same session should not need a different habit; the page in front, if
        // there is one, wins over --page.
        QString spec = parser.value(u"area"_s);
        const int colon = spec.indexOf(u':');
        if (colon > 0) {
            bool ok = false;
            const int number = spec.left(colon).toInt(&ok);
            if (!ok || number < 1) {
                return fail(i18n("“%1” is not a page number.", spec.left(colon)), UsageError);
            }
            page = number - 1;
            spec = spec.mid(colon + 1);
        }
        double values[4] = { 0, 0, 0, 0 };
        if (!parseNumbers(spec, 4, values)) {
            return fail(i18n("“%1” is not an area. The form is X,Y,WIDTH,HEIGHT, in points from the bottom-left "
                             "corner of the page.",
                             parser.value(u"area"_s)),
                        UsageError);
        }
        area = QRectF(values[0], values[1], values[2], values[3]).normalized();
    }

    QVector<PageObject> objects;
    if (!objectsOn(input, page, &objects, &code)) {
        return code;
    }

    QVector<PageObject> found;
    if (byPoint) {
        // hitTest is asked again with the object it just named taken out, so
        // this list is ordered by the rule the window uses when you click
        // through a stack. Deciding here what else lies under the point would be
        // a second copy of that rule, free to drift from the first.
        QVector<PageObject> remaining = objects;
        for (int index = PageObjects::hitTest(remaining, where); index >= 0;
             index = PageObjects::hitTest(remaining, where)) {
            bool removed = false;
            for (int i = 0; i < remaining.size(); ++i) {
                if (remaining.at(i).index == index) {
                    found.append(remaining.at(i));
                    remaining.remove(i);
                    removed = true;
                    break;
                }
            }
            if (!removed) {
                break; // Cannot happen, but a loop that cannot end is worse.
            }
        }
    } else {
        for (const int index : PageObjects::within(objects, area)) {
            if (const PageObject *object = find(objects, index)) {
                found.append(*object);
            }
        }
        // Topmost first here too, so both forms answer in the same order.
        std::reverse(found.begin(), found.end());
    }

    // Nothing there is an answer, not a failure, but scripts want to branch on
    // it: the exit code is the one compare uses for "they differ".
    const int outcome = found.isEmpty() ? DifferencesFound : Success;

    if (parser.isSet(u"json"_s)) {
        printJson(found);
        return outcome;
    }

    if (found.isEmpty()) {
        out() << (byPoint ? i18n("Nothing is drawn at %1, %2 on page %3.", points(where.x()), points(where.y()),
                                 page + 1)
                          : i18n("Nothing on page %1 meets that area.", page + 1))
              << Qt::endl;
        return outcome;
    }

    printHeading();
    for (const PageObject &object : found) {
        printObject(object);
    }
    out() << i18np("%1 object, topmost first.", "%1 objects, topmost first.", found.size()) << Qt::endl;
    return outcome;
}

enum class Motion { Move, Scale, Rotate };

int commandTransform(Motion motion, const QStringList &arguments, const QCommandLineParser &parser)
{
    const QString verb = motion == Motion::Move ? u"objects move"_s
        : motion == Motion::Scale               ? u"objects scale"_s
                                                : u"objects rotate"_s;
    QString input;
    int code = Success;
    if (!oneInputFile(arguments, verb, &input, &code)) {
        return code;
    }
    int page = 0;
    if (!wantedPage(parser, &page, &code)) {
        return code;
    }
    QVector<int> wanted;
    if (!wantedObjects(parser, &wanted, &code)) {
        return code;
    }

    double dx = 0;
    double dy = 0;
    double scaleX = 1;
    double scaleY = 1;
    double degrees = 0;

    if (motion == Motion::Move) {
        double values[2] = { 0, 0 };
        if (!parser.isSet(u"by"_s) || !parseNumbers(parser.value(u"by"_s), 2, values)) {
            return fail(i18n("Say how far with --by DX,DY, in points. Positive numbers move right and up."),
                        UsageError);
        }
        dx = values[0];
        dy = values[1];
    } else if (motion == Motion::Scale) {
        double values[2] = { 0, 0 };
        if (parser.isSet(u"by"_s) && parseNumbers(parser.value(u"by"_s), 1, values)) {
            scaleX = values[0];
            scaleY = values[0];
        } else if (parser.isSet(u"by"_s) && parseNumbers(parser.value(u"by"_s), 2, values)) {
            scaleX = values[0];
            scaleY = values[1];
        } else {
            return fail(i18n("Say how much with --by FACTOR, e.g. 1.5, or --by FX,FY to stretch one way only."),
                        UsageError);
        }
        if (qFuzzyIsNull(scaleX) || qFuzzyIsNull(scaleY)) {
            return fail(i18n("A factor of zero would flatten the object out of existence."), UsageError);
        }
    } else {
        // --angle defaults to 45 for watermarks; an unasked-for rotation of a
        // caption is not a default anyone wants.
        bool ok = false;
        degrees = parser.value(u"angle"_s).toDouble(&ok);
        if (!parser.isSet(u"angle"_s) || !ok) {
            return fail(i18n("Say how far round with --angle DEGREES, counter-clockwise."), UsageError);
        }
    }

    QVector<PageObject> objects;
    if (!objectsOn(input, page, &objects, &code)) {
        return code;
    }
    if (!knownObjects(objects, wanted, page, &code)) {
        return code;
    }

    bool haveCentre = false;
    QPointF centre;
    if (parser.isSet(u"around"_s)) {
        double values[2] = { 0, 0 };
        if (!parseNumbers(parser.value(u"around"_s), 2, values)) {
            return fail(i18n("“%1” is not a point. The form is X,Y, in points from the bottom-left corner of the "
                             "page.",
                             parser.value(u"around"_s)),
                        UsageError);
        }
        centre = QPointF(values[0], values[1]);
        haveCentre = true;
    }

    // Built once and by itself: a rotation applied to an identity matrix is the
    // rotation, with no doubt about which side it lands on.
    QTransform rotation;
    rotation.rotate(degrees);

    QVector<PageEdit> edits;
    edits.reserve(wanted.size());
    for (const int index : wanted) {
        const PageObject *object = find(objects, index);
        // Without --around each object turns on its own middle, which is what
        // "turn this picture" means to the person asking.
        const QPointF pivot = haveCentre ? centre : object->bounds.center();

        PageEdit edit;
        edit.kind = PageEdit::Kind::Transform;
        edit.page = page;
        edit.object = index;
        switch (motion) {
        case Motion::Move:
            edit.transform = QTransform::fromTranslate(dx, dy);
            break;
        case Motion::Scale:
            edit.transform = QTransform::fromTranslate(-pivot.x(), -pivot.y()) * QTransform::fromScale(scaleX, scaleY)
                * QTransform::fromTranslate(pivot.x(), pivot.y());
            break;
        case Motion::Rotate:
            edit.transform = QTransform::fromTranslate(-pivot.x(), -pivot.y()) * rotation
                * QTransform::fromTranslate(pivot.x(), pivot.y());
            break;
        }
        edits.append(edit);
    }

    return compose(input, outputFor(parser, input), edits, {});
}

int commandDelete(const QStringList &arguments, const QCommandLineParser &parser)
{
    QString input;
    int code = Success;
    if (!oneInputFile(arguments, u"objects delete"_s, &input, &code)) {
        return code;
    }
    int page = 0;
    if (!wantedPage(parser, &page, &code)) {
        return code;
    }
    QVector<int> wanted;
    if (!wantedObjects(parser, &wanted, &code)) {
        return code;
    }

    QVector<PageObject> objects;
    if (!objectsOn(input, page, &objects, &code)) {
        return code;
    }
    if (!knownObjects(objects, wanted, page, &code)) {
        return code;
    }

    QVector<PageEdit> edits;
    edits.reserve(wanted.size());
    for (const int index : wanted) {
        PageEdit edit;
        edit.kind = PageEdit::Kind::Delete;
        edit.page = page;
        edit.object = index;
        edits.append(edit);
    }

    return compose(input, outputFor(parser, input), edits, {});
}

int commandRestyle(const QStringList &arguments, const QCommandLineParser &parser)
{
    QString input;
    int code = Success;
    if (!oneInputFile(arguments, u"objects restyle"_s, &input, &code)) {
        return code;
    }
    int page = 0;
    if (!wantedPage(parser, &page, &code)) {
        return code;
    }
    QVector<int> wanted;
    if (!wantedObjects(parser, &wanted, &code)) {
        return code;
    }

    QColor fill;
    QColor stroke;
    if (!wantedColour(parser, u"fill"_s, &fill, &code) || !wantedColour(parser, u"stroke"_s, &stroke, &code)) {
        return code;
    }

    double lineWidth = -1.0; // Negative leaves the width as it is.
    if (parser.isSet(u"line-width"_s)) {
        bool ok = false;
        lineWidth = parser.value(u"line-width"_s).toDouble(&ok);
        if (!ok || lineWidth < 0) {
            return fail(i18n("“%1” is not a line width in points.", parser.value(u"line-width"_s)), UsageError);
        }
    }

    if (!fill.isValid() && !stroke.isValid() && lineWidth < 0) {
        return fail(i18n("Say what to change: --fill, --stroke or --line-width."), UsageError);
    }

    QVector<PageObject> objects;
    if (!objectsOn(input, page, &objects, &code)) {
        return code;
    }
    if (!knownObjects(objects, wanted, page, &code)) {
        return code;
    }

    QVector<PageEdit> edits;
    edits.reserve(wanted.size());
    for (const int index : wanted) {
        PageEdit edit;
        edit.kind = PageEdit::Kind::Restyle;
        edit.page = page;
        edit.object = index;
        edit.fill = fill;
        edit.stroke = stroke;
        edit.lineWidth = lineWidth;
        edits.append(edit);
    }

    return compose(input, outputFor(parser, input), edits, {});
}

bool parseShape(const QString &name, NewContent::Kind *kind)
{
    static const QHash<QString, NewContent::Kind> shapes {
        { u"text"_s, NewContent::Kind::Text },
        { u"image"_s, NewContent::Kind::Image },
        { u"picture"_s, NewContent::Kind::Image },
        { u"bild"_s, NewContent::Kind::Image },
        { u"rectangle"_s, NewContent::Kind::Rectangle },
        { u"rect"_s, NewContent::Kind::Rectangle },
        { u"rechteck"_s, NewContent::Kind::Rectangle },
        { u"ellipse"_s, NewContent::Kind::Ellipse },
        { u"line"_s, NewContent::Kind::Line },
        { u"linie"_s, NewContent::Kind::Line },
    };
    const auto it = shapes.constFind(name.trimmed().toLower());
    if (it == shapes.constEnd()) {
        return false;
    }
    *kind = *it;
    return true;
}

int commandInsert(const QStringList &arguments, const QCommandLineParser &parser)
{
    QString input;
    int code = Success;
    if (!oneInputFile(arguments, u"objects insert"_s, &input, &code)) {
        return code;
    }
    int page = 0;
    if (!wantedPage(parser, &page, &code)) {
        return code;
    }

    // Read the page although nothing here needs its objects: the composer drops
    // an insertion meant for a page that is not there and reports a plain zero,
    // and "it did nothing" is a much worse answer than "there is no page 99".
    QVector<PageObject> onPage;
    if (!objectsOn(input, page, &onPage, &code)) {
        return code;
    }

    NewContent::Kind kind = NewContent::Kind::Rectangle;
    if (!parser.isSet(u"shape"_s) || !parseShape(parser.value(u"shape"_s), &kind)) {
        return fail(i18n("Say what to insert with --shape: text, image, rectangle, ellipse or line."), UsageError);
    }

    const QStringList rectangles = parser.values(u"rect"_s);
    if (rectangles.isEmpty()) {
        return fail(i18n("Say where with --rect X,Y,WIDTH,HEIGHT, in points from the bottom-left corner of the "
                         "page."),
                    UsageError);
    }

    NewContent prototype;
    prototype.kind = kind;
    prototype.page = page;

    if (kind == NewContent::Kind::Text) {
        if (!parser.isSet(u"text"_s) || parser.value(u"text"_s).isEmpty()) {
            return fail(i18n("Say what it should read with --text."), UsageError);
        }
        prototype.text = parser.value(u"text"_s);
        if (parser.isSet(u"font-family"_s)) {
            prototype.fontFamily = parser.value(u"font-family"_s);
        }
        if (parser.isSet(u"font-size"_s)) {
            bool ok = false;
            const double size = parser.value(u"font-size"_s).toDouble(&ok);
            if (!ok || size <= 0) {
                return fail(i18n("“%1” is not a text size in points.", parser.value(u"font-size"_s)), UsageError);
            }
            prototype.fontSize = size;
        }
    }

    if (kind == NewContent::Kind::Image) {
        if (!parser.isSet(u"image"_s)) {
            return fail(i18n("Say which picture with --image."), UsageError);
        }
        prototype.image = QImage(parser.value(u"image"_s));
        if (prototype.image.isNull()) {
            return fail(i18n("“%1” could not be read as an image.", parser.value(u"image"_s)));
        }
        // Without --quality the pixels go in untouched; a photo that is already
        // a JPEG is the only case where re-encoding pays for itself.
        if (parser.isSet(u"quality"_s)) {
            prototype.jpegQuality = std::clamp(parser.value(u"quality"_s).toInt(), 0, 100);
        }
    }

    if (!wantedColour(parser, u"fill"_s, &prototype.fill, &code)
        || !wantedColour(parser, u"stroke"_s, &prototype.stroke, &code)) {
        return code;
    }
    if (parser.isSet(u"line-width"_s)) {
        bool ok = false;
        prototype.lineWidth = parser.value(u"line-width"_s).toDouble(&ok);
        if (!ok || prototype.lineWidth < 0) {
            return fail(i18n("“%1” is not a line width in points.", parser.value(u"line-width"_s)), UsageError);
        }
    }
    if (parser.isSet(u"opacity"_s)) {
        prototype.opacity = std::clamp(parser.value(u"opacity"_s).toInt(), 0, 100) / 100.0;
    }

    QVector<NewContent> insertions;
    insertions.reserve(rectangles.size());
    for (const QString &spec : rectangles) {
        double values[4] = { 0, 0, 0, 0 };
        if (!parseNumbers(spec, 4, values)) {
            return fail(i18n("“%1” is not a place. The form is X,Y,WIDTH,HEIGHT, in points from the bottom-left "
                             "corner of the page.",
                             spec),
                        UsageError);
        }
        NewContent item = prototype;
        item.rect = QRectF(values[0], values[1], values[2], values[3]);
        insertions.append(item);
    }

    return compose(input, outputFor(parser, input), {}, insertions);
}

int commandLimits()
{
    out() << i18nc("@title heading before a list", "Reading a page as objects:") << Qt::endl;
    for (const QString &limit : PageObjects::limitations()) {
        out() << u"  · "_s << limit << Qt::endl;
    }
    out() << Qt::endl << i18nc("@title heading before a list", "Changing what is on a page:") << Qt::endl;
    for (const QString &limit : PageComposer::limitations()) {
        out() << u"  · "_s << limit << Qt::endl;
    }
    return Success;
}

// ── the group ─────────────────────────────────────────────────────────────

void objectOptions(QCommandLineParser &parser)
{
    const QCommandLineOption objectOption(
        u"object"_s,
        i18n("Which object, by the number “objects list” prints. Repeat, or separate with commas, to change "
             "several at once."),
        u"number"_s);
    const QCommandLineOption byOption(
        u"by"_s, i18n("How far to move, as DX,DY in points, or how much to scale, as a factor."), u"amount"_s);
    const QCommandLineOption aroundOption(
        u"around"_s, i18n("Point to scale or turn around, as X,Y in points from the bottom-left corner. Without it, "
                          "each object uses its own middle."),
        u"point"_s);
    const QCommandLineOption fillOption(u"fill"_s,
                                        i18n("Fill colour, a name or #rrggbb. For text, the colour of the letters."),
                                        u"colour"_s);
    const QCommandLineOption strokeOption(u"stroke"_s, i18n("Outline colour, a name or #rrggbb."), u"colour"_s);
    const QCommandLineOption lineWidthOption(u"line-width"_s, i18n("Outline width in points."), u"points"_s);
    const QCommandLineOption shapeOption(u"shape"_s,
                                         i18n("What to insert: text, image, rectangle, ellipse or line."), u"kind"_s);
    const QCommandLineOption rectOption(
        u"rect"_s,
        i18n("Where it goes, as X,Y,WIDTH,HEIGHT in points from the bottom-left corner of the page. Repeat to "
             "insert several."),
        u"spec"_s);
    // Not --font: that name belongs to the font commands, and one flag with two
    // meanings is how someone restyles the wrong thing.
    const QCommandLineOption fontFamilyOption(
        u"font-family"_s, i18n("Standard font for inserted text: Helvetica, Times-Roman or Courier."), u"name"_s);

    parser.addOptions({ objectOption, byOption, aroundOption, fillOption, strokeOption, lineWidthOption, shapeOption,
                        rectOption, fontFamilyOption });
}

QStringList objectHelp()
{
    const auto line = [](const QString &syntax, const QString &what) {
        return u"  "_s + syntax.leftJustified(46) + what;
    };
    return {
        line(u"objects list     FILE --page N|all"_s,
             i18n("Everything drawn on the page, with the number to grab it by")),
        line(u"objects at       FILE --page N --at X,Y"_s, i18n("What is under a point, topmost first")),
        line(u"objects move     FILE --object I --by DX,DY"_s, i18n("Shift an object across the page")),
        line(u"objects scale    FILE --object I --by FACTOR"_s, i18n("Make an object larger or smaller")),
        line(u"objects rotate   FILE --object I --angle DEG"_s, i18n("Turn an object, counter-clockwise")),
        line(u"objects delete   FILE --object I"_s, i18n("Take an object off the page for good")),
        line(u"objects restyle  FILE --object I --fill C"_s, i18n("Change the colours or the line width")),
        line(u"objects insert   FILE --shape S --rect R"_s, i18n("Put text, a picture or a shape on the page")),
        line(u"objects limits"_s, i18n("What reading and editing objects cannot reach")),
    };
}

std::optional<int> objectRun(const QString &verb, const QStringList &arguments, const QCommandLineParser &parser)
{
    if (verb != QLatin1String("objects")) {
        return std::nullopt;
    }

    QStringList rest = arguments;
    const QString action = rest.isEmpty() ? QString() : rest.takeFirst().toLower();

    if (action.isEmpty()) {
        return fail(i18n("Say what to do: objects list, at, move, scale, rotate, delete, restyle, insert or limits."),
                    UsageError);
    }
    if (action == QLatin1String("limits") || action == QLatin1String("limitations")) {
        return commandLimits();
    }
    if (action == QLatin1String("list")) {
        return commandList(rest, parser);
    }
    if (action == QLatin1String("at")) {
        return commandAt(rest, parser);
    }
    if (action == QLatin1String("move")) {
        return commandTransform(Motion::Move, rest, parser);
    }
    if (action == QLatin1String("scale")) {
        return commandTransform(Motion::Scale, rest, parser);
    }
    if (action == QLatin1String("rotate")) {
        return commandTransform(Motion::Rotate, rest, parser);
    }
    if (action == QLatin1String("delete")) {
        return commandDelete(rest, parser);
    }
    if (action == QLatin1String("restyle")) {
        return commandRestyle(rest, parser);
    }
    if (action == QLatin1String("insert")) {
        return commandInsert(rest, parser);
    }

    return fail(i18n("“objects %1” is not a command. Try list, at, move, scale, rotate, delete, restyle, insert or "
                     "limits.",
                     action),
                UsageError);
}

} // namespace

Group objectGroup()
{
    return { objectOptions, objectRun, objectHelp };
}

} // namespace ps::cli
