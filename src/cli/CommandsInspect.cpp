/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later

    The file as it is stored, on the command line.

    "objects" addresses what is drawn on a page; "inspect" addresses what is in
    the file — the numbered indirect objects and the graph of references between
    them, from the trailer down. It is the view iText's RUPS gives, read on a
    terminal: a tree you can walk, an object you can open by its number, and a
    flat table of everything the file holds. It only reads.
*/
#include "Commands.h"

#include "core/PdfInspect.h"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>

#include <KLocalizedString>

using namespace Qt::Literals::StringLiterals;

namespace ps::cli {

namespace {

constexpr int kDefaultTreeDepth = 4;
constexpr int kDefaultObjectDepth = 8;

int depthFrom(const QCommandLineParser &parser, int fallback)
{
    if (!parser.isSet(u"depth"_s)) {
        return fallback;
    }
    bool ok = false;
    const int given = parser.value(u"depth"_s).toInt(&ok);
    return ok && given > 0 ? given : fallback;
}

QString typeOf(const PdfInspect::Node &node)
{
    return PdfInspect::typeToken(node.type);
}

// ---- human-readable tree ----

void printNode(QTextStream &stream, const PdfInspect::Node &node, int indent)
{
    QString line = QString(indent * 2, u' ');
    if (!node.label.isEmpty()) {
        line += node.label + u": "_s;
    }
    if (node.indirect) {
        line += node.truncated
            ? i18nc("@info a reference to an object, not expanded here", "→ %1 %2 R  ", node.obj, node.gen)
            : QStringLiteral("%1 %2 obj  ").arg(node.obj).arg(node.gen);
    }
    line += node.summary;
    if (!node.streamFilters.isEmpty()) {
        line += i18nc("@info the stream's /Filter", "  filter %1", node.streamFilters);
    }
    if (node.streamLength >= 0) {
        line += i18nc("@info stream length in bytes", "  %1 bytes", node.streamLength);
    }
    if (node.truncated && !node.indirect) {
        line += u"  "_s + QChar(0x2026);
    }
    stream << line << Qt::endl;

    for (const PdfInspect::Node &child : node.children) {
        printNode(stream, child, indent + 1);
    }
}

// ---- JSON ----

QJsonObject nodeToJson(const PdfInspect::Node &node)
{
    QJsonObject object;
    if (!node.label.isEmpty()) {
        object.insert(u"label"_s, node.label);
    }
    object.insert(u"type"_s, typeOf(node));
    object.insert(u"summary"_s, node.summary);
    if (node.indirect) {
        object.insert(u"object"_s, node.obj);
        object.insert(u"generation"_s, node.gen);
    }
    if (node.children.isEmpty() && !node.value.isEmpty()) {
        object.insert(u"value"_s, node.value);
    }
    if (node.type == PdfInspect::Type::Stream) {
        if (!node.streamFilters.isEmpty()) {
            object.insert(u"filter"_s, node.streamFilters);
        }
        if (node.streamLength >= 0) {
            object.insert(u"length"_s, static_cast<double>(node.streamLength));
        }
    }
    if (node.truncated) {
        object.insert(u"truncated"_s, true);
    }
    if (!node.children.isEmpty()) {
        QJsonArray children;
        for (const PdfInspect::Node &child : node.children) {
            children.append(nodeToJson(child));
        }
        object.insert(u"children"_s, children);
    }
    return object;
}

void emitJson(QTextStream &stream, const QJsonObject &object)
{
    stream << QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Indented));
}

// ---- the sub-commands ----

int commandTree(const QStringList &arguments, const QCommandLineParser &parser,
                bool (*build)(const QString &, int, PdfInspect::Node *, QString *), const QString &verb)
{
    QString input;
    int code = 0;
    if (!oneInputFile(arguments, verb, &input, &code)) {
        return code;
    }

    PdfInspect::Node node;
    QString error;
    if (!build(input, depthFrom(parser, kDefaultTreeDepth), &node, &error)) {
        return fail(error.isEmpty() ? i18n("Could not read %1.", input) : error);
    }

    if (parser.isSet(u"json"_s)) {
        emitJson(out(), nodeToJson(node));
        return Success;
    }
    printNode(out(), node, 0);
    return Success;
}

int commandObject(const QStringList &arguments, const QCommandLineParser &parser)
{
    QString input;
    int code = 0;
    if (!oneInputFile(arguments, u"inspect object"_s, &input, &code)) {
        return code;
    }
    if (!parser.isSet(u"object"_s)) {
        return fail(i18n("Say which object: inspect object FILE --object N. “inspect objects FILE” lists the numbers."),
                    UsageError);
    }
    bool ok = false;
    const int number = parser.value(u"object"_s).toInt(&ok);
    if (!ok || number < 0) {
        return fail(i18n("“%1” is not an object number.", parser.value(u"object"_s)), UsageError);
    }
    int generation = -1;
    if (parser.isSet(u"gen"_s)) {
        bool genOk = false;
        generation = parser.value(u"gen"_s).toInt(&genOk);
        if (!genOk || generation < 0) {
            return fail(i18n("“%1” is not a generation number.", parser.value(u"gen"_s)), UsageError);
        }
    }

    PdfInspect::Node node;
    QString error;
    if (!PdfInspect::object(input, number, generation, depthFrom(parser, kDefaultObjectDepth), &node, &error)) {
        return fail(error.isEmpty() ? i18n("Could not read object %1.", number) : error);
    }

    if (parser.isSet(u"json"_s)) {
        emitJson(out(), nodeToJson(node));
        return Success;
    }
    printNode(out(), node, 0);
    return Success;
}

int commandObjects(const QStringList &arguments, const QCommandLineParser &parser)
{
    QString input;
    int code = 0;
    if (!oneInputFile(arguments, u"inspect objects"_s, &input, &code)) {
        return code;
    }

    QVector<PdfInspect::ObjectSummary> rows;
    QString error;
    if (!PdfInspect::objects(input, &rows, &error)) {
        return fail(error.isEmpty() ? i18n("Could not read %1.", input) : error);
    }

    if (parser.isSet(u"json"_s)) {
        QJsonArray array;
        for (const PdfInspect::ObjectSummary &row : rows) {
            QJsonObject entry;
            entry.insert(u"object"_s, row.obj);
            entry.insert(u"generation"_s, row.gen);
            entry.insert(u"type"_s, row.typeName);
            if (!row.pdfType.isEmpty()) {
                entry.insert(u"pdfType"_s, row.pdfType);
            }
            entry.insert(u"summary"_s, row.summary);
            array.append(entry);
        }
        emitJson(out(), { { u"objects"_s, array } });
        return Success;
    }

    for (const PdfInspect::ObjectSummary &row : rows) {
        QString number = QStringLiteral("%1 %2").arg(row.obj).arg(row.gen);
        out() << number.leftJustified(8) << row.typeName.leftJustified(12);
        if (!row.pdfType.isEmpty()) {
            out() << row.pdfType.leftJustified(22);
        } else {
            out() << QString(22, u' ');
        }
        out() << row.summary << Qt::endl;
    }
    out() << i18ncp("@info total count of indirect objects", "%1 object.", "%1 objects.", rows.size()) << Qt::endl;
    return Success;
}

int commandInfo(const QStringList &arguments, const QCommandLineParser &parser)
{
    QString input;
    int code = 0;
    if (!oneInputFile(arguments, u"inspect info"_s, &input, &code)) {
        return code;
    }

    PdfInspect::Overview overview;
    QString error;
    if (!PdfInspect::overview(input, &overview, &error)) {
        return fail(error.isEmpty() ? i18n("Could not read %1.", input) : error);
    }

    if (parser.isSet(u"json"_s)) {
        QJsonObject object;
        object.insert(u"version"_s, overview.version);
        object.insert(u"objects"_s, overview.objectCount);
        object.insert(u"pages"_s, overview.pageCount);
        object.insert(u"encrypted"_s, overview.encrypted);
        object.insert(u"linearized"_s, overview.linearized);
        if (!overview.producer.isEmpty()) {
            object.insert(u"producer"_s, overview.producer);
        }
        if (!overview.warnings.isEmpty()) {
            object.insert(u"warnings"_s, overview.warnings);
        }
        emitJson(out(), object);
        return Success;
    }

    out() << i18nc("@info PDF header version", "PDF version: %1", overview.version) << Qt::endl;
    out() << i18nc("@info count of indirect objects", "Objects: %1", overview.objectCount) << Qt::endl;
    if (overview.pageCount >= 0) {
        out() << i18nc("@info page count", "Pages: %1", overview.pageCount) << Qt::endl;
    }
    out() << i18nc("@info whether the file is encrypted", "Encrypted: %1",
                   overview.encrypted ? i18nc("@info yes", "yes") : i18nc("@info no", "no"))
          << Qt::endl;
    out() << i18nc("@info whether the file is linearized for the web", "Linearized: %1",
                   overview.linearized ? i18nc("@info yes", "yes") : i18nc("@info no", "no"))
          << Qt::endl;
    if (!overview.producer.isEmpty()) {
        out() << i18nc("@info the /Producer string", "Producer: %1", overview.producer) << Qt::endl;
    }
    if (!overview.warnings.isEmpty()) {
        out() << overview.warnings << Qt::endl;
    }
    return Success;
}

int commandLimits()
{
    for (const QString &limit : PdfInspect::limitations()) {
        out() << u"• "_s << limit << Qt::endl;
    }
    return Success;
}

// ---- the group ----

void inspectOptions(QCommandLineParser &parser)
{
    const QCommandLineOption depthOption(u"depth"_s, i18n("How many levels of the tree to open. Default: 4."),
                                         u"levels"_s);
    const QCommandLineOption genOption(u"gen"_s, i18n("The generation number, when an object has more than one."),
                                       u"number"_s);
    parser.addOptions({ depthOption, genOption });
}

std::optional<int> inspectRun(const QString &verb, const QStringList &arguments, const QCommandLineParser &parser)
{
    if (verb != QLatin1String("inspect")) {
        return std::nullopt;
    }

    QStringList rest = arguments;
    const QString action = rest.isEmpty() ? QString() : rest.takeFirst().toLower();

    if (action.isEmpty()) {
        return fail(i18n("Say what to inspect: info, objects, object, trailer, root or limits."), UsageError);
    }
    if (action == QLatin1String("limits") || action == QLatin1String("limitations")) {
        return commandLimits();
    }
    if (action == QLatin1String("info")) {
        return commandInfo(rest, parser);
    }
    if (action == QLatin1String("objects")) {
        return commandObjects(rest, parser);
    }
    if (action == QLatin1String("object")) {
        return commandObject(rest, parser);
    }
    if (action == QLatin1String("trailer")) {
        return commandTree(rest, parser, PdfInspect::trailer, u"inspect trailer"_s);
    }
    if (action == QLatin1String("root")) {
        return commandTree(rest, parser, PdfInspect::root, u"inspect root"_s);
    }

    return fail(i18n("“inspect %1” is not a command. Try info, objects, object, trailer, root or limits.", action),
                UsageError);
}

QStringList inspectHelp()
{
    const auto line
        = [](const QString &syntax, const QString &what) { return u"  "_s + syntax.leftJustified(46) + what; };
    return {
        line(u"inspect info     FILE"_s, i18n("Version, object and page counts, how it is secured")),
        line(u"inspect objects  FILE"_s, i18n("Every indirect object, with its number and type")),
        line(u"inspect object   FILE --object N"_s, i18n("One object opened out as a tree, by its number")),
        line(u"inspect trailer  FILE"_s, i18n("The trailer, the way into everything else")),
        line(u"inspect root     FILE"_s, i18n("The catalogue, the document's own root")),
        line(u"inspect limits"_s, i18n("What this low-level view does not reach")),
    };
}

} // namespace

Group inspectGroup()
{
    return { inspectOptions, inspectRun, inspectHelp };
}

} // namespace ps::cli
