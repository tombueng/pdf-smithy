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
#include <QFile>
#include <QFileInfo>
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

void emitRows(const QVector<PdfInspect::ObjectSummary> &rows)
{
    for (const PdfInspect::ObjectSummary &row : rows) {
        const QString number = QStringLiteral("%1 %2").arg(row.obj).arg(row.gen);
        out() << number.leftJustified(8) << row.typeName.leftJustified(12);
        out() << (row.pdfType.isEmpty() ? QString(22, u' ') : row.pdfType.leftJustified(22));
        out() << row.summary << Qt::endl;
    }
}

void emitRowsJson(const QVector<PdfInspect::ObjectSummary> &rows)
{
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
        emitRowsJson(rows);
        return Success;
    }
    emitRows(rows);
    out() << i18ncp("@info total count of indirect objects", "%1 object.", "%1 objects.", rows.size()) << Qt::endl;
    return Success;
}

int commandFind(const QStringList &arguments, const QCommandLineParser &parser)
{
    QString input;
    int code = 0;
    if (!oneInputFile(arguments, u"inspect find"_s, &input, &code)) {
        return code;
    }

    PdfInspect::Query query;
    query.type = parser.value(u"of-type"_s);
    query.key = parser.value(u"has-key"_s);
    query.text = parser.value(u"matching"_s);
    if (query.type.isEmpty() && query.key.isEmpty() && query.text.isEmpty()) {
        return fail(i18n("Say what to look for: --of-type /Font, --has-key /JavaScript or --matching TEXT."),
                    UsageError);
    }

    QVector<PdfInspect::ObjectSummary> rows;
    QString error;
    if (!PdfInspect::find(input, query, &rows, &error)) {
        return fail(error.isEmpty() ? i18n("Could not read %1.", input) : error);
    }

    if (parser.isSet(u"json"_s)) {
        emitRowsJson(rows);
        return Success;
    }
    if (rows.isEmpty()) {
        out() << i18n("Nothing matched.") << Qt::endl;
        return Success;
    }
    emitRows(rows);
    out() << i18ncp("@info count of matching objects", "%1 object matched.", "%1 objects matched.", rows.size())
          << Qt::endl;
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

/** Whether bytes are safe to print: no NUL, and few control characters. */
bool looksTextual(const QByteArray &bytes)
{
    if (bytes.isEmpty()) {
        return true;
    }
    const int sample = qMin<int>(bytes.size(), 4096);
    int suspicious = 0;
    for (int i = 0; i < sample; ++i) {
        const unsigned char c = static_cast<unsigned char>(bytes.at(i));
        if (c == 0) {
            return false;
        }
        if (c < 9 || (c > 13 && c < 32)) {
            ++suspicious;
        }
    }
    return suspicious * 20 < sample; // under about 5% control bytes
}

int commandStream(const QStringList &arguments, const QCommandLineParser &parser)
{
    QString input;
    int code = 0;
    if (!oneInputFile(arguments, u"inspect stream"_s, &input, &code)) {
        return code;
    }
    if (!parser.isSet(u"object"_s)) {
        return fail(i18n("Say which object: inspect stream FILE --object N."), UsageError);
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

    QByteArray bytes;
    QString filters;
    QString error;
    const bool wantRaw = parser.isSet(u"raw"_s);
    if (!PdfInspect::streamData(input, number, generation, wantRaw, &bytes, &filters, &error)) {
        if (wantRaw) {
            return fail(error.isEmpty() ? i18n("Could not read the stream.") : error);
        }
        // A filter QPDF cannot undo (a JPEG, say): show the bytes as stored
        // instead of failing — for an image, those stored bytes are the image.
        QString rawError;
        if (!PdfInspect::streamData(input, number, generation, true, &bytes, &filters, &rawError)) {
            return fail(error.isEmpty() ? rawError : error);
        }
        err() << i18nc("@info", "The filters on object %1 could not be undone; showing the bytes as stored.", number)
              << Qt::endl;
    }

    if (parser.isSet(u"output"_s)) {
        const QString path = parser.value(u"output"_s);
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            return fail(file.errorString(), OutputError);
        }
        file.write(bytes);
        out() << i18ncp("@info", "Wrote %1 byte to %2.", "Wrote %1 bytes to %2.", bytes.size(),
                        QFileInfo(path).fileName())
              << Qt::endl;
        return Success;
    }

    if (looksTextual(bytes)) {
        out() << QString::fromUtf8(bytes);
        if (!bytes.endsWith('\n')) {
            out() << Qt::endl;
        }
        return Success;
    }

    out() << i18ncp("@info", "Object %2 is a binary stream of %1 byte.", "Object %2 is a binary stream of %1 bytes.",
                    bytes.size(), number);
    if (!filters.isEmpty()) {
        out() << u" "_s << i18nc("@info the stream's /Filter", "Filter %1.", filters);
    }
    out() << u" "_s << i18nc("@info", "Use -o FILE to save it, or --raw for the bytes as stored.") << Qt::endl;
    return Success;
}

std::optional<PdfInspect::Scalar> scalarFrom(const QString &given)
{
    const QString name = given.trimmed().toLower();
    if (name.isEmpty() || name == u"keep"_s || name == u"auto"_s) {
        return PdfInspect::Scalar::Keep;
    }
    if (name == u"name"_s) {
        return PdfInspect::Scalar::Name;
    }
    if (name == u"string"_s || name == u"text"_s) {
        return PdfInspect::Scalar::String;
    }
    if (name == u"int"_s || name == u"integer"_s) {
        return PdfInspect::Scalar::Integer;
    }
    if (name == u"real"_s || name == u"number"_s || name == u"float"_s) {
        return PdfInspect::Scalar::Real;
    }
    if (name == u"bool"_s || name == u"boolean"_s) {
        return PdfInspect::Scalar::Boolean;
    }
    return std::nullopt;
}

int commandSet(const QStringList &arguments, const QCommandLineParser &parser)
{
    QString input;
    int code = 0;
    if (!oneInputFile(arguments, u"inspect set"_s, &input, &code)) {
        return code;
    }
    if (!parser.isSet(u"object"_s)) {
        return fail(i18n("Say which object: inspect set FILE --object N --key /Foo --new-value X."), UsageError);
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
    if (!parser.isSet(u"key"_s)) {
        return fail(i18n("Say which key: --key /Foo. “inspect object FILE --object N” shows an object's keys."),
                    UsageError);
    }
    if (!parser.isSet(u"new-value"_s)) {
        return fail(i18n("Say the new value: --new-value X."), UsageError);
    }
    const std::optional<PdfInspect::Scalar> as = scalarFrom(parser.value(u"as-type"_s));
    if (!as.has_value()) {
        return fail(i18n("“%1” is not a type. Use name, string, int, real or bool.", parser.value(u"as-type"_s)),
                    UsageError);
    }

    const QString key = parser.value(u"key"_s);
    const QString value = parser.value(u"new-value"_s);
    const QString output = outputOr(parser.value(u"output"_s), input, u"-edited.pdf"_s);

    QString oldRendering;
    QString error;
    if (!PdfInspect::setScalar(input, output, number, generation, key, value, *as, &oldRendering, &error)) {
        return fail(error.isEmpty() ? i18n("Could not set the value.") : error, OutputError);
    }

    out() << i18nc("@info the edit that was made, saved to a file", "Object %1 %2: %3 → %4, saved as %5.", number, key,
                   oldRendering, value, QFileInfo(output).fileName())
          << Qt::endl;
    return Success;
}

// ---- the group ----

void inspectOptions(QCommandLineParser &parser)
{
    const QCommandLineOption depthOption(u"depth"_s, i18n("How many levels of the tree to open. Default: 4."),
                                         u"levels"_s);
    const QCommandLineOption genOption(u"gen"_s, i18n("The generation number, when an object has more than one."),
                                       u"number"_s);
    const QCommandLineOption ofTypeOption(u"of-type"_s, i18n("For find: only objects of this /Type or /Subtype."),
                                          u"name"_s);
    const QCommandLineOption hasKeyOption(u"has-key"_s, i18n("For find: only objects whose dictionary has this key."),
                                          u"name"_s);
    const QCommandLineOption matchingOption(u"matching"_s,
                                            i18n("For find: only objects whose type or summary contains "
                                                 "this."),
                                            u"text"_s);
    const QCommandLineOption keyOption(u"key"_s, i18n("For set: the key to change, e.g. /Producer."), u"name"_s);
    const QCommandLineOption newValueOption(u"new-value"_s, i18n("For set: the value to store."), u"value"_s);
    const QCommandLineOption asTypeOption(
        u"as-type"_s, i18n("For set: store it as name, string, int, real or bool. Default: the old value's type."),
        u"type"_s);
    const QCommandLineOption rawOption(u"raw"_s, i18n("For stream: the bytes exactly as stored, filters and all."));
    parser.addOptions({ depthOption, genOption, ofTypeOption, hasKeyOption, matchingOption, keyOption, newValueOption,
                        asTypeOption, rawOption });
}

std::optional<int> inspectRun(const QString &verb, const QStringList &arguments, const QCommandLineParser &parser)
{
    if (verb != QLatin1String("inspect")) {
        return std::nullopt;
    }

    QStringList rest = arguments;
    const QString action = rest.isEmpty() ? QString() : rest.takeFirst().toLower();

    if (action.isEmpty()) {
        return fail(i18n("Say what to inspect: info, objects, object, find, set, stream, trailer, root or limits."),
                    UsageError);
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
    if (action == QLatin1String("find")) {
        return commandFind(rest, parser);
    }
    if (action == QLatin1String("set")) {
        return commandSet(rest, parser);
    }
    if (action == QLatin1String("stream")) {
        return commandStream(rest, parser);
    }

    return fail(i18n("“inspect %1” is not a command. Try info, objects, object, find, set, stream, trailer, root or "
                     "limits.",
                     action),
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
        line(u"inspect find     FILE --of-type /Font"_s, i18n("The objects that match a type, a key or some text")),
        line(u"inspect set      FILE --object N --key K"_s, i18n("Change one value, saving an edited copy")),
        line(u"inspect stream   FILE --object N"_s, i18n("Read a stream object's content, or save it with -o")),
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
