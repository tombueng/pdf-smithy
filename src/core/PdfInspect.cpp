/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#include "PdfInspect.h"

#include "PdfFile.h"

#include <KLocalizedString>

#include <qpdf/Constants.h>
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFObjGen.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFWriter.hh>

#include <QFile>

#include <algorithm>
#include <set>
#include <string>
#include <vector>

using namespace Qt::Literals::StringLiterals;

namespace ps {

namespace {

/** The whole walk shares one budget, so a pathological file cannot run us dry. */
struct Budget {
    int remaining = 20000;
};

PdfInspect::Type mapType(qpdf_object_type_e code)
{
    switch (code) {
    case ot_boolean:
        return PdfInspect::Type::Boolean;
    case ot_integer:
        return PdfInspect::Type::Integer;
    case ot_real:
        return PdfInspect::Type::Real;
    case ot_string:
        return PdfInspect::Type::String;
    case ot_name:
        return PdfInspect::Type::Name;
    case ot_array:
        return PdfInspect::Type::Array;
    case ot_dictionary:
        return PdfInspect::Type::Dictionary;
    case ot_stream:
        return PdfInspect::Type::Stream;
    case ot_operator:
        return PdfInspect::Type::Operator;
    case ot_inlineimage:
        return PdfInspect::Type::InlineImage;
    case ot_null:
        return PdfInspect::Type::Null;
    default:
        return PdfInspect::Type::Other;
    }
}

/** The dictionary half of @p object, whether it is a bare dictionary or a stream. */
QPDFObjectHandle dictOf(QPDFObjectHandle object)
{
    return object.isStream() ? object.getDict() : object;
}

/** A /Name value at @p key, without the slash lost, or empty when it is not a name. */
QString nameAt(QPDFObjectHandle dict, const char *key)
{
    if (!dict.isDictionary() || !dict.hasKey(key)) {
        return {};
    }
    QPDFObjectHandle value = dict.getKey(key);
    return value.isName() ? QString::fromStdString(value.getName()) : QString();
}

QString elide(const QString &text, int limit = 70)
{
    QString flat = text.simplified();
    return flat.size() > limit ? flat.left(limit - 1) + QChar(0x2026) : flat;
}

/** One line that says what an object is, enough to decide whether to open it. */
QString summarize(QPDFObjectHandle object)
{
    switch (object.getTypeCode()) {
    case ot_dictionary:
    case ot_stream: {
        QPDFObjectHandle dict = dictOf(object);
        QStringList lead;
        const QString type = nameAt(dict, "/Type");
        const QString sub = nameAt(dict, "/Subtype");
        if (!type.isEmpty()) {
            lead << type;
        }
        if (!sub.isEmpty()) {
            lead << sub;
        }
        const int keys = static_cast<int>(dict.getKeys().size());
        QString head = lead.join(u' ');
        QString tail = i18ncp("@info number of dictionary keys", "%1 key", "%1 keys", keys);
        QString body = head.isEmpty() ? tail : head + u", "_s + tail;
        if (object.isStream()) {
            body += i18nc("@info a stream object follows its dictionary", " · stream");
        }
        return body;
    }
    case ot_array:
        return i18ncp("@info number of array elements", "%1 item", "%1 items", object.getArrayNItems());
    case ot_integer:
        return QString::number(object.getIntValue());
    case ot_real:
        return QString::fromStdString(object.getRealValue());
    case ot_boolean:
        return object.getBoolValue() ? u"true"_s : u"false"_s;
    case ot_name:
        return QString::fromStdString(object.getName());
    case ot_string:
        return elide(QString::fromStdString(object.unparse()));
    case ot_null:
        return u"null"_s;
    default:
        return QString::fromStdString(object.getTypeName());
    }
}

void expand(QPDFObjectHandle object, int depth, std::set<QPDFObjGen> &path, Budget &budget, PdfInspect::Node &node)
{
    node.type = mapType(object.getTypeCode());
    node.summary = summarize(object);

    const bool indirect = object.isIndirect();
    QPDFObjGen og;
    if (indirect) {
        og = object.getObjGen();
        node.indirect = true;
        node.obj = og.getObj();
        node.gen = og.getGen();
    }

    if (object.isStream()) {
        QPDFObjectHandle dict = object.getDict();
        if (dict.hasKey("/Filter")) {
            node.streamFilters = QString::fromStdString(dict.getKey("/Filter").unparse());
        }
        if (dict.hasKey("/Length") && dict.getKey("/Length").isInteger()) {
            node.streamLength = dict.getKey("/Length").getIntValue();
        }
    }

    const bool container = object.isDictionary() || object.isArray() || object.isStream();
    if (!container) {
        node.value = node.summary;
        return;
    }

    const bool looping = indirect && path.count(og) > 0;
    if (depth <= 0 || budget.remaining <= 0 || looping) {
        node.truncated = true;
        return;
    }

    if (indirect) {
        path.insert(og);
    }

    if (object.isArray()) {
        const int n = object.getArrayNItems();
        for (int i = 0; i < n; ++i) {
            if (budget.remaining <= 0) {
                node.truncated = true;
                break;
            }
            --budget.remaining;
            PdfInspect::Node child;
            child.label = u"["_s + QString::number(i) + u"]"_s;
            expand(object.getArrayItem(i), depth - 1, path, budget, child);
            node.children.push_back(child);
        }
    } else {
        QPDFObjectHandle dict = dictOf(object);
        for (const std::string &key : dict.getKeys()) {
            if (budget.remaining <= 0) {
                node.truncated = true;
                break;
            }
            --budget.remaining;
            PdfInspect::Node child;
            child.label = QString::fromStdString(key);
            expand(dict.getKey(key), depth - 1, path, budget, child);
            node.children.push_back(child);
        }
    }

    if (indirect) {
        path.erase(og);
    }
}

/** The /Type /Subtype pair of an indirect object, for the flat table's column. */
QString pdfTypeOf(QPDFObjectHandle object)
{
    if (!object.isDictionary() && !object.isStream()) {
        return {};
    }
    QPDFObjectHandle dict = dictOf(object);
    QStringList parts;
    const QString type = nameAt(dict, "/Type");
    const QString sub = nameAt(dict, "/Subtype");
    if (!type.isEmpty()) {
        parts << type;
    }
    if (!sub.isEmpty()) {
        parts << sub;
    }
    return parts.join(u' ');
}

PdfInspect::ObjectSummary summaryOf(QPDFObjectHandle object)
{
    PdfInspect::ObjectSummary row;
    const QPDFObjGen og = object.getObjGen();
    row.obj = og.getObj();
    row.gen = og.getGen();
    row.typeName = QString::fromStdString(object.getTypeName());
    row.pdfType = pdfTypeOf(object);
    row.summary = summarize(object);
    return row;
}

/** A PDF name with its leading slash, so "Font" and "/Font" both match /Font. */
QString withSlash(const QString &name)
{
    if (name.isEmpty() || name.startsWith(u'/')) {
        return name;
    }
    return u"/"_s + name;
}

} // namespace

namespace PdfInspect {

bool overview(const QString &pdf, Overview *out, QString *error)
{
    try {
        QPDF document;
        PdfFile::open(document, pdf);

        out->version = QString::fromStdString(document.getPDFVersion());
        out->objectCount = static_cast<int>(document.getAllObjects().size());
        out->encrypted = document.isEncrypted();
        out->linearized = document.isLinearized();

        QPDFObjectHandle trailer = document.getTrailer();
        if (trailer.isDictionary() && trailer.hasKey("/Info")) {
            QPDFObjectHandle info = trailer.getKey("/Info");
            if (info.isDictionary() && info.hasKey("/Producer") && info.getKey("/Producer").isString()) {
                out->producer = QString::fromStdString(info.getKey("/Producer").getUTF8Value());
            }
        }

        try {
            out->pageCount = static_cast<int>(QPDFPageDocumentHelper(document).getAllPages().size());
        } catch (const std::exception &) {
            out->pageCount = -1;
        }

        out->warnings = PdfFile::warningSummary(document);
        return true;
    } catch (const std::exception &problem) {
        if (error) {
            *error = QString::fromUtf8(problem.what());
        }
        return false;
    }
}

bool objects(const QString &pdf, QVector<ObjectSummary> *out, QString *error)
{
    try {
        QPDF document;
        PdfFile::open(document, pdf);

        std::vector<QPDFObjectHandle> all = document.getAllObjects();
        out->reserve(static_cast<int>(all.size()));
        for (QPDFObjectHandle &object : all) {
            out->push_back(summaryOf(object));
        }
        std::sort(out->begin(), out->end(),
                  [](const ObjectSummary &a, const ObjectSummary &b) { return a.obj < b.obj; });
        return true;
    } catch (const std::exception &problem) {
        if (error) {
            *error = QString::fromUtf8(problem.what());
        }
        return false;
    }
}

bool find(const QString &pdf, const Query &query, QVector<ObjectSummary> *out, QString *error)
{
    try {
        QPDF document;
        PdfFile::open(document, pdf);

        const QString wantType = withSlash(query.type);
        const std::string wantKey = withSlash(query.key).toStdString();

        for (QPDFObjectHandle &object : document.getAllObjects()) {
            if (!query.type.isEmpty()) {
                QPDFObjectHandle dict = dictOf(object);
                if (nameAt(dict, "/Type") != wantType && nameAt(dict, "/Subtype") != wantType) {
                    continue;
                }
            }
            if (!query.key.isEmpty()) {
                if (!object.isDictionary() && !object.isStream()) {
                    continue;
                }
                if (!dictOf(object).hasKey(wantKey)) {
                    continue;
                }
            }
            ObjectSummary row = summaryOf(object);
            if (!query.text.isEmpty()) {
                const QString hay = row.pdfType + u' ' + row.summary;
                if (!hay.contains(query.text, Qt::CaseInsensitive)) {
                    continue;
                }
            }
            out->push_back(row);
        }
        std::sort(out->begin(), out->end(),
                  [](const ObjectSummary &a, const ObjectSummary &b) { return a.obj < b.obj; });
        return true;
    } catch (const std::exception &problem) {
        if (error) {
            *error = QString::fromUtf8(problem.what());
        }
        return false;
    }
}

bool trailer(const QString &pdf, int depth, Node *out, QString *error)
{
    try {
        QPDF document;
        PdfFile::open(document, pdf);
        std::set<QPDFObjGen> path;
        Budget budget;
        expand(document.getTrailer(), depth, path, budget, *out);
        return true;
    } catch (const std::exception &problem) {
        if (error) {
            *error = QString::fromUtf8(problem.what());
        }
        return false;
    }
}

bool root(const QString &pdf, int depth, Node *out, QString *error)
{
    try {
        QPDF document;
        PdfFile::open(document, pdf);
        std::set<QPDFObjGen> path;
        Budget budget;
        expand(document.getRoot(), depth, path, budget, *out);
        return true;
    } catch (const std::exception &problem) {
        if (error) {
            *error = QString::fromUtf8(problem.what());
        }
        return false;
    }
}

bool object(const QString &pdf, int obj, int gen, int depth, Node *out, QString *error)
{
    try {
        QPDF document;
        PdfFile::open(document, pdf);

        // getObject() invents a null for a number that is not there, so existence
        // is checked against the real set rather than trusted to the lookup.
        bool found = false;
        int actualGen = gen;
        for (QPDFObjectHandle &candidate : document.getAllObjects()) {
            QPDFObjGen og = candidate.getObjGen();
            if (og.getObj() == obj && (gen < 0 || og.getGen() == gen)) {
                found = true;
                actualGen = og.getGen();
                break;
            }
        }
        if (!found) {
            if (error) {
                *error = i18n("There is no object %1 in this file.", obj);
            }
            return false;
        }

        std::set<QPDFObjGen> path;
        Budget budget;
        expand(document.getObject(obj, actualGen), depth, path, budget, *out);
        return true;
    } catch (const std::exception &problem) {
        if (error) {
            *error = QString::fromUtf8(problem.what());
        }
        return false;
    }
}

bool setScalar(const QString &in, const QString &out, int obj, int gen, const QString &key, const QString &value,
               Scalar as, QString *oldRendering, QString *error)
{
    try {
        QPDF document;
        PdfFile::open(document, in);

        bool found = false;
        int actualGen = gen;
        for (QPDFObjectHandle &candidate : document.getAllObjects()) {
            QPDFObjGen og = candidate.getObjGen();
            if (og.getObj() == obj && (gen < 0 || og.getGen() == gen)) {
                found = true;
                actualGen = og.getGen();
                break;
            }
        }
        if (!found) {
            if (error) {
                *error = i18n("There is no object %1 in this file.", obj);
            }
            return false;
        }

        QPDFObjectHandle object = document.getObject(obj, actualGen);
        if (!object.isDictionary()) {
            if (error) {
                *error = i18n("Object %1 is a %2, which has no keys to set.", obj,
                              QString::fromStdString(object.getTypeName()));
            }
            return false;
        }

        const std::string k = withSlash(key).toStdString();
        if (!object.hasKey(k)) {
            if (error) {
                *error = i18n("Object %1 has no key %2.", obj, withSlash(key));
            }
            return false;
        }

        QPDFObjectHandle current = object.getKey(k);
        if (current.isDictionary() || current.isArray() || current.isStream()) {
            if (error) {
                *error = i18n("%1 holds a %2, not a single value this can set.", withSlash(key),
                              QString::fromStdString(current.getTypeName()));
            }
            return false;
        }
        if (oldRendering) {
            *oldRendering = QString::fromStdString(current.unparse());
        }

        Scalar target = as;
        if (target == Scalar::Keep) {
            if (current.isName()) {
                target = Scalar::Name;
            } else if (current.isInteger()) {
                target = Scalar::Integer;
            } else if (current.isReal()) {
                target = Scalar::Real;
            } else if (current.isBool()) {
                target = Scalar::Boolean;
            } else {
                target = Scalar::String;
            }
        }

        QPDFObjectHandle replacement;
        switch (target) {
        case Scalar::Name:
            replacement = QPDFObjectHandle::newName(withSlash(value).toStdString());
            break;
        case Scalar::String:
            replacement = QPDFObjectHandle::newString(value.toStdString());
            break;
        case Scalar::Integer: {
            bool ok = false;
            const long long number = value.toLongLong(&ok);
            if (!ok) {
                if (error) {
                    *error = i18n("“%1” is not a whole number.", value);
                }
                return false;
            }
            replacement = QPDFObjectHandle::newInteger(number);
            break;
        }
        case Scalar::Real: {
            bool ok = false;
            value.toDouble(&ok);
            if (!ok) {
                if (error) {
                    *error = i18n("“%1” is not a number.", value);
                }
                return false;
            }
            replacement = QPDFObjectHandle::newReal(value.toStdString());
            break;
        }
        case Scalar::Boolean: {
            const QString lowered = value.trimmed().toLower();
            if (lowered != u"true"_s && lowered != u"false"_s) {
                if (error) {
                    *error = i18n("“%1” is not true or false.", value);
                }
                return false;
            }
            replacement = QPDFObjectHandle::newBool(lowered == u"true"_s);
            break;
        }
        case Scalar::Keep:
            break; // resolved above
        }

        object.replaceKey(k, replacement);

        QPDFWriter writer(document);
        const QByteArray nativePath = QFile::encodeName(out);
        writer.setOutputFilename(nativePath.constData());
        // Keep the file as it was built — change the one value, not the shape.
        writer.setObjectStreamMode(qpdf_o_preserve);
        writer.setStreamDataMode(qpdf_s_preserve);
        writer.write();
        return true;
    } catch (const std::exception &problem) {
        if (error) {
            *error = QString::fromUtf8(problem.what());
        }
        return false;
    }
}

QString typeToken(Type type)
{
    switch (type) {
    case Type::Null:
        return u"null"_s;
    case Type::Boolean:
        return u"boolean"_s;
    case Type::Integer:
        return u"integer"_s;
    case Type::Real:
        return u"real"_s;
    case Type::String:
        return u"string"_s;
    case Type::Name:
        return u"name"_s;
    case Type::Array:
        return u"array"_s;
    case Type::Dictionary:
        return u"dictionary"_s;
    case Type::Stream:
        return u"stream"_s;
    case Type::Operator:
        return u"operator"_s;
    case Type::InlineImage:
        return u"inline-image"_s;
    case Type::Other:
        return u"other"_s;
    }
    return u"other"_s;
}

QStringList limitations()
{
    return {
        i18n("Only numbered indirect objects are listed; a value written straight into "
             "another object is shown inside it, not on its own."),
        i18n("Stream contents are summarised by filter and length, not decoded or shown."),
        i18n("An object kept inside a compressed object stream is shown resolved, not by "
             "its place within that stream."),
        i18n("Editing (inspect set) changes one existing scalar value at a time; it does not add keys, nor alter "
             "arrays, streams or the document's structure."),
        i18n("Saving an edit writes the whole file afresh, so object numbers may change; inspect the saved copy to "
             "find them again."),
        i18n("An encrypted document must be opened with its password before anything here "
             "can read it."),
    };
}

} // namespace PdfInspect

} // namespace ps
