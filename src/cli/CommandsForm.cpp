/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later

    Making a form, as against filling one in.

    `form` is already the verb for the form somebody else made: list it, fill it
    in, flatten it. Everything here is the other half (putting the fields on the
    page in the first place), so it has a verb of its own, `field`, with the job
    as the word after it. That keeps one word at the top level for the sixteen
    things ps::FormBuilder offers, and it reads the way it is meant:
    `field add letter.pdf --type text --name Anrede`.
*/
#include "Commands.h"

#include "core/FormBuilder.h"
#include "core/Forms.h"

#include <KLocalizedString>

#include <QColor>
#include <QCommandLineParser>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QTextStream>

using namespace Qt::Literals::StringLiterals;

namespace ps::cli {
namespace {

using Field = FormBuilder::Field;

// ── Reading the command line ──────────────────────────────────────────────

/** "X,Y", in points from the bottom-left corner of the page as it is shown. */
bool parsePoint(const QString &text, QPointF *point)
{
    const QStringList parts = text.split(u',');
    if (parts.size() != 2) {
        return false;
    }
    bool okX = false;
    bool okY = false;
    const double x = parts.constFirst().trimmed().toDouble(&okX);
    const double y = parts.constLast().trimmed().toDouble(&okY);
    *point = QPointF(x, y);
    return okX && okY;
}

/** "WIDTHxHEIGHT", or with a comma for anyone who reaches for that first. */
bool parseSize(const QString &text, QSizeF *size)
{
    QStringList parts = text.split(u'x', Qt::KeepEmptyParts, Qt::CaseInsensitive);
    if (parts.size() != 2) {
        parts = text.split(u',');
    }
    if (parts.size() != 2) {
        return false;
    }
    bool okW = false;
    bool okH = false;
    const double width = parts.constFirst().trimmed().toDouble(&okW);
    const double height = parts.constLast().trimmed().toDouble(&okH);
    *size = QSizeF(width, height);
    return okW && okH && width > 0.0 && height > 0.0;
}

/** A comma-separated list, which may also be given as the option repeated. */
QStringList commaList(const QCommandLineParser &parser, const QString &option)
{
    QStringList items;
    const QStringList given = parser.values(option);
    for (const QString &value : given) {
        const QStringList parts = value.split(u',', Qt::SkipEmptyParts);
        for (const QString &part : parts) {
            const QString trimmed = part.trimmed();
            if (!trimmed.isEmpty()) {
                items << trimmed;
            }
        }
    }
    return items;
}

/** A measurement in points, without the trailing nought nobody needs. */
QString points(double value)
{
    QString text = QString::number(value, 'f', 1);
    if (text.endsWith(QLatin1String(".0"))) {
        text.chop(2);
    }
    return text;
}

/** The word after --type. Two of them are a text field with a flag set. */
bool applyKind(const QString &token, Field *field)
{
    const QString word = token.trimmed().toLower();
    if (word == QLatin1String("text")) {
        field->kind = Field::Kind::Text;
    } else if (word == QLatin1String("password")) {
        field->kind = Field::Kind::Text;
        field->password = true;
    } else if (word == QLatin1String("comb")) {
        field->kind = Field::Kind::Text;
        field->comb = true;
    } else if (word == QLatin1String("checkbox") || word == QLatin1String("check")) {
        field->kind = Field::Kind::Checkbox;
    } else if (word == QLatin1String("radio")) {
        field->kind = Field::Kind::Radio;
    } else if (word == QLatin1String("choice") || word == QLatin1String("dropdown") || word == QLatin1String("combo")) {
        field->kind = Field::Kind::Dropdown;
    } else if (word == QLatin1String("list") || word == QLatin1String("listbox")) {
        field->kind = Field::Kind::ListBox;
    } else if (word == QLatin1String("button") || word == QLatin1String("push-button")) {
        field->kind = Field::Kind::PushButton;
    } else if (word == QLatin1String("signature") || word == QLatin1String("sign")) {
        field->kind = Field::Kind::Signature;
    } else {
        return false;
    }
    return true;
}

/** The stable word for a kind, so that a script may read it back. */
QString typeToken(FormField::Kind kind)
{
    switch (kind) {
    case FormField::Kind::Text:
        return u"text"_s;
    case FormField::Kind::Checkbox:
        return u"checkbox"_s;
    case FormField::Kind::Radio:
        return u"radio"_s;
    case FormField::Kind::Choice:
        return u"choice"_s;
    case FormField::Kind::Button:
        return u"button"_s;
    case FormField::Kind::Signature:
        return u"signature"_s;
    case FormField::Kind::Unknown:
        break;
    }
    return u"unknown"_s;
}

bool applyAlignment(const QString &token, Field *field)
{
    const QString word = token.trimmed().toLower();
    if (word == QLatin1String("left")) {
        field->alignment = Qt::AlignLeft;
    } else if (word == QLatin1String("centre") || word == QLatin1String("center")) {
        field->alignment = Qt::AlignHCenter;
    } else if (word == QLatin1String("right")) {
        field->alignment = Qt::AlignRight;
    } else {
        return false;
    }
    return true;
}

bool isBorderStyle(const QString &word)
{
    return word == QLatin1String("solid") || word == QLatin1String("dashed") || word == QLatin1String("beveled")
        || word == QLatin1String("inset") || word == QLatin1String("underline");
}

/**
 * Everything a field of any kind may be given, taken off the command line.
 *
 * Only what was actually asked for is touched, so that `field update` can start
 * from the field the document already has and change the one thing meant.
 */
bool applyOptions(const QCommandLineParser &parser, Field *field, QString *error)
{
    if (parser.isSet(u"tooltip"_s)) {
        field->label = parser.value(u"tooltip"_s);
    }
    if (parser.isSet(u"required"_s)) {
        field->required = true;
    }
    if (parser.isSet(u"read-only"_s)) {
        field->readOnly = true;
    }
    if (parser.isSet(u"multiline"_s)) {
        field->multiline = true;
    }
    if (parser.isSet(u"comb"_s)) {
        field->comb = true;
    }
    if (parser.isSet(u"editable"_s)) {
        field->editable = true;
    }
    if (parser.isSet(u"multi-select"_s)) {
        field->multiSelect = true;
    }
    if (parser.isSet(u"value"_s)) {
        field->defaultValue = parser.value(u"value"_s);
    }
    if (parser.isSet(u"on-state"_s)) {
        field->onState = parser.value(u"on-state"_s);
    }
    if (parser.isSet(u"field-font"_s)) {
        field->fontName = parser.value(u"field-font"_s);
    }
    if (parser.isSet(u"options"_s)) {
        field->options = commaList(parser, u"options"_s);
    }
    if (parser.isSet(u"export-values"_s)) {
        field->exportValues = commaList(parser, u"export-values"_s);
    }

    if (parser.isSet(u"max-length"_s)) {
        bool ok = false;
        const int length = parser.value(u"max-length"_s).toInt(&ok);
        if (!ok || length < 0) {
            *error = i18n("“%1” is not a number of characters.", parser.value(u"max-length"_s));
            return false;
        }
        field->maxLength = length;
    }
    if (parser.isSet(u"font-size"_s)) {
        bool ok = false;
        const double size = parser.value(u"font-size"_s).toDouble(&ok);
        if (!ok || size < 0.0) {
            *error = i18n("“%1” is not a text size.", parser.value(u"font-size"_s));
            return false;
        }
        // Zero is not "nothing given": it asks the reader to fit the text to the
        // box, which is what an unset size means here too.
        field->fontSize = size;
    }
    if (parser.isSet(u"border-width"_s)) {
        bool ok = false;
        const double width = parser.value(u"border-width"_s).toDouble(&ok);
        if (!ok || width < 0.0) {
            *error = i18n("“%1” is not a border width.", parser.value(u"border-width"_s));
            return false;
        }
        field->borderWidth = width;
    }
    if (parser.isSet(u"border-style"_s)) {
        const QString style = parser.value(u"border-style"_s).trimmed().toLower();
        if (!isBorderStyle(style)) {
            *error = i18n("“%1” is not a border. Use solid, dashed, beveled, inset or underline.", style);
            return false;
        }
        field->borderStyle = style;
    }
    if (parser.isSet(u"field-align"_s) && !applyAlignment(parser.value(u"field-align"_s), field)) {
        *error = i18n("“%1” is not an alignment. Use left, centre or right.", parser.value(u"field-align"_s));
        return false;
    }

    struct {
        const char16_t *option;
        QColor *target;
    } const colours[] = { { u"colour", &field->textColour },
                          { u"background", &field->backgroundColour },
                          { u"border-colour", &field->borderColour } };
    for (const auto &entry : colours) {
        const QString option = QString::fromUtf16(entry.option);
        if (!parser.isSet(option)) {
            continue;
        }
        const QColor colour(parser.value(option));
        if (!colour.isValid()) {
            *error = i18n("“%1” is not a colour. Try a name or #rrggbb.", parser.value(option));
            return false;
        }
        *entry.target = colour;
    }
    return true;
}

/** Where the widgets go: --at with --size, repeated once per radio button. */
bool rectangles(const QCommandLineParser &parser, QVector<QRectF> *rects, QString *error)
{
    if (!parser.isSet(u"at"_s) || !parser.isSet(u"size"_s)) {
        *error = i18n("Say where the field goes and how big it is, e.g. --at 72,700 --size 200x18.");
        return false;
    }
    const QStringList places = parser.values(u"at"_s);
    const QStringList sizes = parser.values(u"size"_s);
    if (sizes.size() != 1 && sizes.size() != places.size()) {
        *error = i18n("Give one --size for them all, or one for every --at.");
        return false;
    }

    for (qsizetype i = 0; i < places.size(); ++i) {
        QPointF corner;
        if (!parsePoint(places.at(i), &corner)) {
            *error = i18n("“%1” is not a place. The form is X,Y, in points from the bottom-left corner of the "
                          "page as it is shown.",
                          places.at(i));
            return false;
        }
        const QString text = sizes.size() == 1 ? sizes.constFirst() : sizes.at(i);
        QSizeF box;
        if (!parseSize(text, &box)) {
            *error = i18n("“%1” is not a size. The form is WIDTHxHEIGHT, in points.", text);
            return false;
        }
        rects->append(QRectF(corner, box));
    }
    return true;
}

/**
 * One field from the options given, or several widgets for a radio group.
 *
 * A radio group is one field with one value and a widget per choice, so the
 * command line spells it as one --options list and one --at for each entry in
 * it. Three separate fields would give three buttons that toggle as one, which
 * is the classic way of getting a radio group wrong.
 */
bool buildFields(const QCommandLineParser &parser, QVector<Field> *fields, QString *error)
{
    Field prototype;
    if (!parser.isSet(u"type"_s)) {
        *error = i18n("Say what kind of field with --type: text, password, checkbox, radio, choice, list, button "
                      "or signature.");
        return false;
    }
    if (!applyKind(parser.value(u"type"_s), &prototype)) {
        *error = i18n("“%1” is not a kind of field. Use text, password, checkbox, radio, choice, list, button "
                      "or signature.",
                      parser.value(u"type"_s));
        return false;
    }

    prototype.name = parser.value(u"name"_s).trimmed();
    if (prototype.name.isEmpty()) {
        *error = i18n("Give the field a name with --name. That is the name filling in addresses, and a full stop "
                      "in it makes a group: Adresse.Strasse and Adresse.Ort belong together.");
        return false;
    }

    bool ok = false;
    const int page = parser.value(u"page"_s).toInt(&ok);
    if (!ok || page < 1) {
        *error = i18n("“%1” is not a page number.", parser.value(u"page"_s));
        return false;
    }
    prototype.page = page - 1;

    if (!applyOptions(parser, &prototype, error)) {
        return false;
    }

    QVector<QRectF> rects;
    if (!rectangles(parser, &rects, error)) {
        return false;
    }

    if (prototype.kind != Field::Kind::Radio) {
        if (rects.size() != 1) {
            *error = i18n("Only a radio group takes more than one --at.");
            return false;
        }
        prototype.rect = rects.constFirst();
        fields->append(prototype);
        return true;
    }

    const QStringList choices = prototype.options;
    if (choices.size() < 2) {
        *error = i18n("A radio group needs at least two choices, as --options Frau,Herr.");
        return false;
    }
    if (rects.size() != choices.size()) {
        *error = i18n("A radio group needs one --at for every choice: %1 given for %2 choices.", int(rects.size()),
                      int(choices.size()));
        return false;
    }
    for (qsizetype i = 0; i < choices.size(); ++i) {
        Field button = prototype;
        button.radioGroup = prototype.name;
        button.rect = rects.at(i);
        button.options.clear();
        button.exportValues.clear();
        // What gets stored for this button, and what a reader shows beside it.
        // The label is the only place the order of the buttons is written down.
        button.onState = i < prototype.exportValues.size() ? prototype.exportValues.at(i) : choices.at(i);
        button.label = choices.at(i);
        fields->append(button);
    }
    return true;
}

// ── The field a document already has ──────────────────────────────────────

/**
 * Warns that an update is a rewrite, because the difference matters.
 *
 * ps::FormBuilder::updateFields takes every entry as a complete description:
 * whatever is left at its default is written as that default. Name, kind,
 * place, flags, choices and value can be read back out of the document and are
 * kept; colours, border and font cannot, and come back at the standard setting.
 */
void warnAboutAppearance()
{
    err() << i18n("Note: the field is written back with the standard border, background and font. What its "
                  "author set beyond its name, place, flags, choices and value cannot be read out of the "
                  "document, so give --colour, --background, --border-colour and the rest again to keep them.")
          << Qt::endl;
}

/** The field as the document has it now, so that changing one thing keeps the rest. */
bool seedFromDocument(const QString &input, const QString &name, Field *field, QString *error)
{
    const QVector<FormField> existing = Forms::read(input, error);
    if (!error->isEmpty()) {
        return false;
    }

    for (const FormField &found : existing) {
        if (found.name != name) {
            continue;
        }
        field->name = found.name;
        field->page = found.page;
        field->rect = found.rect;
        // Forms::read falls back to the name when there is no /TU; writing that
        // back would give every field a tooltip repeating its own name.
        field->label = found.label == found.name ? QString() : found.label;
        field->required = found.required;
        field->readOnly = found.readOnly;
        field->multiline = found.multiline;
        field->defaultValue = found.value;

        switch (found.kind) {
        case FormField::Kind::Text:
            field->kind = Field::Kind::Text;
            break;
        case FormField::Kind::Checkbox:
            field->kind = Field::Kind::Checkbox;
            break;
        case FormField::Kind::Radio:
            field->kind = Field::Kind::Radio;
            field->radioGroup = found.name;
            break;
        case FormField::Kind::Choice:
            // A drop-down and a list box read back the same, so the one that is
            // not the common case has to be said again with --type list.
            field->kind = Field::Kind::Dropdown;
            field->options = found.options;
            break;
        case FormField::Kind::Button:
            field->kind = Field::Kind::PushButton;
            break;
        case FormField::Kind::Signature:
            field->kind = Field::Kind::Signature;
            break;
        case FormField::Kind::Unknown:
            *error = i18n("“%1” is a kind of field this cannot change.", name);
            return false;
        }

        if (field->kind == Field::Kind::Checkbox || field->kind == Field::Kind::Radio) {
            // The "on" name is whatever the appearance calls it: /Yes, /On, /1.
            // Writing the wrong one leaves a ticked box empty with a value set.
            for (const QString &state : found.options) {
                if (state != QLatin1String("/Off")) {
                    field->onState = state;
                    break;
                }
            }
        }
        return true;
    }

    *error = i18n("This document has no field called “%1”.", name);
    return false;
}

// ── A whole form from a description ───────────────────────────────────────

bool rectFromJson(const QJsonValue &value, QRectF *rect)
{
    if (value.isArray()) {
        const QJsonArray numbers = value.toArray();
        if (numbers.size() != 4) {
            return false;
        }
        *rect = QRectF(numbers.at(0).toDouble(), numbers.at(1).toDouble(), numbers.at(2).toDouble(),
                       numbers.at(3).toDouble());
    } else if (value.isObject()) {
        const QJsonObject object = value.toObject();
        *rect = QRectF(object.value(u"x"_s).toDouble(), object.value(u"y"_s).toDouble(),
                       object.value(u"width"_s).toDouble(), object.value(u"height"_s).toDouble());
    } else {
        return false;
    }
    return rect->width() > 0.0 && rect->height() > 0.0;
}

QStringList stringsFromJson(const QJsonValue &value)
{
    QStringList items;
    if (value.isArray()) {
        const QJsonArray array = value.toArray();
        for (const QJsonValue &entry : array) {
            items << entry.toString();
        }
    } else if (value.isString()) {
        const QStringList parts = value.toString().split(u',', Qt::SkipEmptyParts);
        for (const QString &part : parts) {
            items << part.trimmed();
        }
    }
    return items;
}

/** One entry of a JSON description, in the shape `field list --json` writes. */
bool fieldFromJson(const QJsonObject &object, QVector<Field> *fields, QString *error)
{
    Field field;
    if (!applyKind(object.value(u"type"_s).toString(u"text"_s), &field)) {
        *error = i18n("“%1” is not a kind of field.", object.value(u"type"_s).toString());
        return false;
    }
    field.name = object.value(u"name"_s).toString().trimmed();
    if (field.name.isEmpty()) {
        *error = i18n("A field needs a name.");
        return false;
    }
    const int page = object.value(u"page"_s).toInt(1);
    if (page < 1) {
        *error = i18n("“%1” asks for page %2, which is not a page.", field.name, page);
        return false;
    }
    field.page = page - 1;

    const QString label
        = object.contains(u"tooltip"_s) ? object.value(u"tooltip"_s).toString() : object.value(u"label"_s).toString();
    if (label != field.name) {
        field.label = label;
    }
    field.defaultValue = object.value(u"value"_s).toString();
    field.required = object.value(u"required"_s).toBool(field.required);
    field.readOnly = object.value(u"readOnly"_s).toBool(field.readOnly);
    field.multiline = object.value(u"multiline"_s).toBool(field.multiline);
    field.password = object.value(u"password"_s).toBool(field.password);
    field.comb = object.value(u"comb"_s).toBool(field.comb);
    field.multiSelect = object.value(u"multiSelect"_s).toBool(field.multiSelect);
    field.editable = object.value(u"editable"_s).toBool(field.editable);
    field.maxLength = object.value(u"maxLength"_s).toInt(0);
    field.options = stringsFromJson(object.value(u"options"_s));
    field.exportValues = stringsFromJson(object.value(u"exportValues"_s));
    field.fontName = object.value(u"fontName"_s).toString(field.fontName);
    field.fontSize = object.value(u"fontSize"_s).toDouble(field.fontSize);
    field.borderWidth = object.value(u"borderWidth"_s).toDouble(field.borderWidth);
    if (object.contains(u"onState"_s)) {
        field.onState = object.value(u"onState"_s).toString();
    }

    if (object.contains(u"borderStyle"_s)) {
        const QString style = object.value(u"borderStyle"_s).toString().trimmed().toLower();
        if (!isBorderStyle(style)) {
            *error = i18n("“%1” is not a border. Use solid, dashed, beveled, inset or underline.", style);
            return false;
        }
        field.borderStyle = style;
    }
    if (object.contains(u"align"_s) && !applyAlignment(object.value(u"align"_s).toString(), &field)) {
        *error = i18n("“%1” is not an alignment. Use left, centre or right.", object.value(u"align"_s).toString());
        return false;
    }

    struct {
        const char16_t *key;
        QColor *target;
    } const colours[] = { { u"colour", &field.textColour },
                          { u"background", &field.backgroundColour },
                          { u"borderColour", &field.borderColour } };
    for (const auto &entry : colours) {
        const QString key = QString::fromUtf16(entry.key);
        if (!object.contains(key)) {
            continue;
        }
        const QColor colour(object.value(key).toString());
        if (!colour.isValid()) {
            *error = i18n("“%1” is not a colour. Try a name or #rrggbb.", object.value(key).toString());
            return false;
        }
        *entry.target = colour;
    }

    // A radio group is written either as one entry per button sharing a group,
    // or as one entry with a rectangle for each of its choices.
    if (field.kind == Field::Kind::Radio && object.contains(u"rects"_s)) {
        const QJsonArray places = object.value(u"rects"_s).toArray();
        if (places.size() != field.options.size() || field.options.size() < 2) {
            *error = i18n("“%1” needs at least two choices and one rectangle for each of them.", field.name);
            return false;
        }
        for (qsizetype i = 0; i < places.size(); ++i) {
            Field button = field;
            if (!rectFromJson(places.at(i), &button.rect)) {
                *error = i18n("“%1” has a place that is not a rectangle.", field.name);
                return false;
            }
            button.radioGroup = field.name;
            button.label = field.options.at(i);
            button.onState = i < field.exportValues.size() ? field.exportValues.at(i) : field.options.at(i);
            button.options.clear();
            button.exportValues.clear();
            fields->append(button);
        }
        return true;
    }

    if (!rectFromJson(object.value(u"rect"_s), &field.rect)) {
        *error = i18n("“%1” has no rectangle. Give it as \"rect\": { \"x\": 72, \"y\": 700, \"width\": 200, "
                      "\"height\": 18 }.",
                      field.name);
        return false;
    }
    if (field.kind == Field::Kind::Radio) {
        field.radioGroup = object.value(u"group"_s).toString(field.name);
        field.name = field.radioGroup;
    }
    fields->append(field);
    return true;
}

// ── The commands ──────────────────────────────────────────────────────────

/** Where the result goes when nobody said. */
QString destination(const QCommandLineParser &parser, const QString &input)
{
    return outputOr(parser.value(u"output"_s), input, u"-form.pdf"_s);
}

int listAction(const QString &input, bool asJson)
{
    QString error;
    const QVector<FormField> fields = Forms::read(input, &error);
    if (!error.isEmpty()) {
        return fail(error);
    }

    if (asJson) {
        QJsonArray array;
        for (const FormField &field : fields) {
            QJsonObject object { { u"name"_s, field.name },          { u"type"_s, typeToken(field.kind) },
                                 { u"label"_s, field.label },        { u"value"_s, field.value },
                                 { u"required"_s, field.required },  { u"readOnly"_s, field.readOnly },
                                 { u"multiline"_s, field.multiline } };
            object.insert(u"page"_s, field.page >= 0 ? QJsonValue(field.page + 1) : QJsonValue());
            if (field.page >= 0 && !field.rect.isEmpty()) {
                object.insert(u"rect"_s,
                              QJsonObject { { u"x"_s, field.rect.x() },
                                            { u"y"_s, field.rect.y() },
                                            { u"width"_s, field.rect.width() },
                                            { u"height"_s, field.rect.height() } });
            }
            if (!field.options.isEmpty()) {
                object.insert(u"options"_s, QJsonArray::fromStringList(field.options));
            }
            array.append(object);
        }
        // An empty array rather than a sentence: a script asking for JSON gets
        // JSON even from a document that has no form at all.
        out() << QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Indented));
        return Success;
    }

    if (fields.isEmpty()) {
        out() << i18n("This document has no form fields.") << Qt::endl;
        return Success;
    }

    for (const FormField &field : fields) {
        if (field.page >= 0) {
            out() << i18nc("@info one form field: page, kind, name", "Page %1  %2  %3", field.page + 1,
                           Forms::describe(field.kind), field.name);
        } else {
            out() << i18nc("@info a form field with nothing on any page: kind, name", "No page  %1  %2",
                           Forms::describe(field.kind), field.name);
        }
        if (field.required) {
            out() << i18nc("@info marks a form field that must be filled in", "  (required)");
        }
        if (field.readOnly) {
            out() << i18nc("@info marks a form field that cannot be filled in", "  (locked)");
        }
        if (field.multiline) {
            out() << i18nc("@info marks a form field that holds more than one line", "  (several lines)");
        }
        out() << Qt::endl;

        if (field.page >= 0 && !field.rect.isEmpty()) {
            out() << QStringLiteral("      ")
                  << i18nc("@info where a field sits and how big it is, in points", "at %1,%2 · %3 × %4 pt",
                           points(field.rect.x()), points(field.rect.y()), points(field.rect.width()),
                           points(field.rect.height()))
                  << Qt::endl;
        }
        if (!field.value.isEmpty() && field.value != QLatin1String("/Off")) {
            out() << QStringLiteral("      = ") << field.value << Qt::endl;
        }
        if (!field.options.isEmpty()) {
            out() << QStringLiteral("      ") << i18n("choices: %1", field.options.join(QStringLiteral(", ")))
                  << Qt::endl;
        }
        if (!field.label.isEmpty() && field.label != field.name) {
            out() << QStringLiteral("      ") << i18n("label: %1", field.label) << Qt::endl;
        }
    }
    out() << i18np("%1 field.", "%1 fields.", int(fields.size())) << Qt::endl;
    return Success;
}

int addAction(const QString &input, const QCommandLineParser &parser)
{
    QVector<Field> fields;
    QString error;
    if (!buildFields(parser, &fields, &error)) {
        return fail(error, UsageError);
    }
    if (parser.isSet(u"tooltip"_s) && fields.constFirst().kind == Field::Kind::Radio) {
        err() << i18n("Note: a radio group takes its labels from --options, so --tooltip is not used for it.")
              << Qt::endl;
    }

    const QString output = destination(parser, input);
    int added = 0;
    if (!FormBuilder::addFields(input, output, fields, &added, &error)) {
        return fail(error, OutputError);
    }
    if (fields.constFirst().kind == Field::Kind::Radio) {
        out() << i18np("Added a radio group with %1 button to “%2”.", "Added a radio group with %1 buttons to “%2”.",
                       added, output)
              << Qt::endl;
        return Success;
    }
    out() << i18np("Added %1 field to “%2”.", "Added %1 fields to “%2”.", added, output) << Qt::endl;
    return Success;
}

int fromJsonAction(const QString &input, const QCommandLineParser &parser)
{
    const QString specification = parser.value(u"from-json"_s);
    if (specification.isEmpty()) {
        return fail(i18n("Give the description to build from with --from-json."), UsageError);
    }
    QFile file(specification);
    if (!file.open(QIODevice::ReadOnly)) {
        return fail(i18n("“%1” could not be read.", specification));
    }
    QJsonParseError parseError;
    const QJsonDocument json = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !json.isArray()) {
        return fail(i18n("“%1” is not a list of fields: %2", specification, parseError.errorString()), UsageError);
    }

    QVector<Field> fields;
    QString error;
    const QJsonArray entries = json.array();
    for (qsizetype i = 0; i < entries.size(); ++i) {
        if (!entries.at(i).isObject()) {
            return fail(i18n("Entry %1 of “%2” is not a field.", int(i + 1), specification), UsageError);
        }
        if (!fieldFromJson(entries.at(i).toObject(), &fields, &error)) {
            return fail(i18n("Entry %1 of “%2”: %3", int(i + 1), specification, error), UsageError);
        }
    }
    if (fields.isEmpty()) {
        return fail(i18n("“%1” describes no fields.", specification), UsageError);
    }

    const QString output = destination(parser, input);
    int added = 0;
    if (!FormBuilder::addFields(input, output, fields, &added, &error)) {
        return fail(error, OutputError);
    }
    out() << i18np("Added %1 field to “%2”.", "Added %1 fields to “%2”.", added, output) << Qt::endl;
    return Success;
}

int removeAction(const QString &input, const QCommandLineParser &parser)
{
    const QStringList names = parser.values(u"name"_s);
    if (names.isEmpty()) {
        return fail(i18n("Say which field to take out with --name. Repeat it for more."), UsageError);
    }
    const QString output = destination(parser, input);
    QString error;
    int removed = 0;
    if (!FormBuilder::removeFields(input, output, names, &removed, &error)) {
        return fail(error, OutputError);
    }
    if (removed < names.size()) {
        err() << i18np("Note: %1 of the names given is not in this document.",
                       "Note: %1 of the names given are not in this document.", int(names.size()) - removed)
              << Qt::endl;
    }
    out() << i18np("Took %1 field out of “%2”.", "Took %1 fields out of “%2”.", removed, output) << Qt::endl;
    return removed == names.size() ? Success : InputError;
}

int renameAction(const QString &input, const QCommandLineParser &parser)
{
    const QString from = parser.value(u"name"_s);
    const QString to = parser.value(u"to-name"_s);
    if (from.isEmpty() || to.isEmpty()) {
        return fail(i18n("Say which field and what to call it: --name Alt --to-name Neu."), UsageError);
    }
    const QString output = destination(parser, input);
    QString error;
    if (!FormBuilder::renameField(input, output, from, to, &error)) {
        return fail(error, OutputError);
    }
    out() << i18n("“%1” is now called “%2” in %3.", from, to, output) << Qt::endl;
    return Success;
}

/** move and update are the same write; only how much of the field they change differs. */
int changeAction(const QString &input, const QCommandLineParser &parser, bool geometryOnly)
{
    const QString name = parser.value(u"name"_s);
    if (name.isEmpty()) {
        return fail(i18n("Say which field with --name."), UsageError);
    }

    Field field;
    QString error;
    if (!seedFromDocument(input, name, &field, &error)) {
        return fail(error);
    }
    const bool wasChoice = field.kind == Field::Kind::Dropdown;
    if (parser.isSet(u"page"_s)) {
        // A widget belongs to the page it is listed on, and changing that means
        // taking it off one page and putting it on another, which is remove and
        // add rather than a change.
        err() << i18n("Note: --page is not used here. To put “%1” on another page, take it out and add it again.", name)
              << Qt::endl;
    }

    if (!geometryOnly) {
        if (parser.isSet(u"type"_s) && !applyKind(parser.value(u"type"_s), &field)) {
            return fail(i18n("“%1” is not a kind of field.", parser.value(u"type"_s)), UsageError);
        }
        if (!applyOptions(parser, &field, &error)) {
            return fail(error, UsageError);
        }
    }

    if (parser.isSet(u"at"_s)) {
        QPointF corner;
        if (!parsePoint(parser.value(u"at"_s), &corner)) {
            return fail(i18n("“%1” is not a place. The form is X,Y, in points from the bottom-left corner of the "
                             "page as it is shown.",
                             parser.value(u"at"_s)),
                        UsageError);
        }
        QSizeF box = field.rect.size();
        if (parser.isSet(u"size"_s) && !parseSize(parser.value(u"size"_s), &box)) {
            return fail(i18n("“%1” is not a size. The form is WIDTHxHEIGHT, in points.", parser.value(u"size"_s)),
                        UsageError);
        }
        if (box.isEmpty()) {
            return fail(i18n("“%1” has nothing on a page to take a size from, so give one with --size.", name),
                        UsageError);
        }
        if (field.kind == Field::Kind::Radio) {
            // Several widgets under one field would all pile onto the same box,
            // so ps::FormBuilder leaves them where they are.
            err() << i18n("Note: a radio group keeps where its buttons are, unless it has only one. To move "
                          "them, take the group out and add it again.")
                  << Qt::endl;
        }
        field.rect = QRectF(corner, box);
    } else if (geometryOnly) {
        return fail(i18n("Say where the field goes with --at, e.g. --at 72,700."), UsageError);
    } else if (parser.isSet(u"size"_s)) {
        QSizeF box;
        if (!parseSize(parser.value(u"size"_s), &box)) {
            return fail(i18n("“%1” is not a size. The form is WIDTHxHEIGHT, in points.", parser.value(u"size"_s)),
                        UsageError);
        }
        field.rect = QRectF(field.rect.topLeft(), box);
    }

    const QString output = destination(parser, input);
    int updated = 0;
    if (!FormBuilder::updateFields(input, output, { field }, &updated, &error)) {
        return fail(error, OutputError);
    }
    if (updated == 0) {
        return fail(i18n("This document has no field called “%1”.", name));
    }

    warnAboutAppearance();
    if (wasChoice && !parser.isSet(u"type"_s)) {
        err() << i18n("Note: a list box and a drop-down read back the same, so “%1” is written as a drop-down. "
                      "Add --type list to keep a list box.",
                      name)
              << Qt::endl;
    }
    out() << i18n("Changed “%1” in %2.", name, output) << Qt::endl;
    return Success;
}

int tabOrderAction(const QString &input, const QCommandLineParser &parser)
{
    const QStringList order = commaList(parser, u"order"_s);
    if (order.isEmpty()) {
        return fail(i18n("Give the order with --order, as one list of field names."), UsageError);
    }
    bool ok = false;
    const int page = parser.value(u"page"_s).toInt(&ok);
    if (!ok || page < 1) {
        return fail(i18n("“%1” is not a page number.", parser.value(u"page"_s)), UsageError);
    }

    const QString output = destination(parser, input);
    QString error;
    if (!FormBuilder::setTabOrder(input, output, page - 1, order, &error)) {
        return fail(error, OutputError);
    }
    out() << i18np("The tab key now walks %1 field of page %2 in that order. The result is in %3.",
                   "The tab key now walks %1 fields of page %2 in that order. The result is in %3.", int(order.size()),
                   page, output)
          << Qt::endl;
    return Success;
}

/** Format, validation and calculation are scripts, so they all report the same caveats. */
void reportWarnings(const QStringList &warnings)
{
    for (const QString &warning : warnings) {
        err() << i18n("Note: %1", warning) << Qt::endl;
    }
}

int formatAction(const QString &input, const QCommandLineParser &parser)
{
    const QString name = parser.value(u"name"_s);
    if (name.isEmpty() || !parser.isSet(u"as"_s)) {
        return fail(i18n("Say which field and how it should look: --name Betrag --as currency."), UsageError);
    }
    const QString word = parser.value(u"as"_s).trimmed().toLower();
    FormBuilder::Format format = FormBuilder::Format::None;
    if (word == QLatin1String("none")) {
        format = FormBuilder::Format::None;
    } else if (word == QLatin1String("number")) {
        format = FormBuilder::Format::Number;
    } else if (word == QLatin1String("currency") || word == QLatin1String("money")) {
        format = FormBuilder::Format::Currency;
    } else if (word == QLatin1String("percent")) {
        format = FormBuilder::Format::Percent;
    } else if (word == QLatin1String("date")) {
        format = FormBuilder::Format::Date;
    } else if (word == QLatin1String("time")) {
        format = FormBuilder::Format::Time;
    } else if (word == QLatin1String("zip") || word == QLatin1String("postcode")) {
        format = FormBuilder::Format::ZipCode;
    } else if (word == QLatin1String("phone")) {
        format = FormBuilder::Format::Phone;
    } else {
        return fail(i18n("“%1” is not a way of showing a value. Use none, number, currency, percent, date, time, "
                         "zip or phone.",
                         word),
                    UsageError);
    }

    bool ok = false;
    const int decimals = parser.value(u"decimals"_s).toInt(&ok);
    if (!ok || decimals < 0) {
        return fail(i18n("“%1” is not a number of decimal places.", parser.value(u"decimals"_s)), UsageError);
    }

    const QString output = destination(parser, input);
    QStringList warnings;
    QString error;
    if (!FormBuilder::setFormat(input, output, name, format, decimals, parser.value(u"symbol"_s), &warnings, &error)) {
        return fail(error, OutputError);
    }
    reportWarnings(warnings);
    if (format == FormBuilder::Format::None) {
        out() << i18n("“%1” no longer formats what is typed into it. The result is in %2.", name, output) << Qt::endl;
    } else {
        out() << i18n("“%1” now shows what is typed into it as %2. The result is in %3.", name, word, output)
              << Qt::endl;
    }
    return Success;
}

int validateAction(const QString &input, const QCommandLineParser &parser)
{
    const QString name = parser.value(u"name"_s);
    if (name.isEmpty() || !parser.isSet(u"between"_s)) {
        return fail(i18n("Say which field and what it may hold: --name Alter --between 18,120."), UsageError);
    }
    QPointF range;
    if (!parsePoint(parser.value(u"between"_s), &range)) {
        return fail(i18n("“%1” is not a range. The form is SMALLEST,LARGEST.", parser.value(u"between"_s)), UsageError);
    }

    const QString output = destination(parser, input);
    QStringList warnings;
    QString error;
    if (!FormBuilder::setValidation(input, output, name, range.x(), range.y(), &warnings, &error)) {
        return fail(error, OutputError);
    }
    reportWarnings(warnings);
    out() << i18n("“%1” now takes numbers from %2 to %3 only. The result is in %4.", name, points(range.x()),
                  points(range.y()), output)
          << Qt::endl;
    return Success;
}

int calculateAction(const QString &input, const QCommandLineParser &parser)
{
    const QString name = parser.value(u"name"_s);
    const QStringList sources = commaList(parser, u"sources"_s);
    if (name.isEmpty() || sources.isEmpty() || !parser.isSet(u"how"_s)) {
        return fail(i18n("Say which field, what to work out and from where: --name Summe --how sum "
                         "--sources Netto,Steuer."),
                    UsageError);
    }
    const QString word = parser.value(u"how"_s).trimmed().toLower();
    FormBuilder::Calculation how = FormBuilder::Calculation::Sum;
    if (word == QLatin1String("sum") || word == QLatin1String("total")) {
        how = FormBuilder::Calculation::Sum;
    } else if (word == QLatin1String("product")) {
        how = FormBuilder::Calculation::Product;
    } else if (word == QLatin1String("average")) {
        how = FormBuilder::Calculation::Average;
    } else if (word == QLatin1String("minimum") || word == QLatin1String("min")) {
        how = FormBuilder::Calculation::Minimum;
    } else if (word == QLatin1String("maximum") || word == QLatin1String("max")) {
        how = FormBuilder::Calculation::Maximum;
    } else {
        return fail(i18n("“%1” is not a calculation. Use sum, product, average, minimum or maximum.", word),
                    UsageError);
    }

    const QString output = destination(parser, input);
    QStringList warnings;
    QString error;
    if (!FormBuilder::setCalculation(input, output, name, how, sources, &warnings, &error)) {
        return fail(error, OutputError);
    }
    reportWarnings(warnings);
    out() << i18np("“%1” is now worked out from %2 field. The result is in %3.",
                   "“%1” is now worked out from %2 fields. The result is in %3.", int(sources.size()), name, output)
          << Qt::endl;
    return Success;
}

int buttonAction(const QString &input, const QCommandLineParser &parser)
{
    const QString name = parser.value(u"name"_s);
    if (name.isEmpty() || !parser.isSet(u"action"_s)) {
        return fail(i18n("Say which button and what it should do: --name Senden --action submit "
                         "--target https://example.org/eingang."),
                    UsageError);
    }
    const QString word = parser.value(u"action"_s).trimmed().toLower();
    FormBuilder::ButtonAction action = FormBuilder::ButtonAction::ResetForm;
    if (word == QLatin1String("reset")) {
        action = FormBuilder::ButtonAction::ResetForm;
    } else if (word == QLatin1String("submit")) {
        action = FormBuilder::ButtonAction::SubmitForm;
    } else if (word == QLatin1String("goto") || word == QLatin1String("page")) {
        action = FormBuilder::ButtonAction::GoToPage;
    } else if (word == QLatin1String("url") || word == QLatin1String("link")) {
        action = FormBuilder::ButtonAction::OpenUrl;
    } else {
        return fail(i18n("“%1” is not something a button can do. Use reset, submit, goto or url.", word), UsageError);
    }

    const QString output = destination(parser, input);
    QString error;
    if (!FormBuilder::setButtonAction(input, output, name, action, parser.value(u"target"_s), &error)) {
        return fail(error, OutputError);
    }
    out() << i18n("Pressing “%1” now does that. The result is in %2.", name, output) << Qt::endl;
    return Success;
}

int copyAction(const QString &input, const QCommandLineParser &parser)
{
    const QString templatePdf = parser.value(u"template"_s);
    if (templatePdf.isEmpty()) {
        return fail(i18n("Say which document to take the fields from with --template."), UsageError);
    }
    const QFileInfo file(templatePdf);
    if (!file.exists() || !file.isReadable()) {
        return fail(i18n("“%1” cannot be read.", templatePdf));
    }

    const QString output = destination(parser, input);
    QString error;
    int copied = 0;
    if (!FormBuilder::copyFieldsFrom(file.absoluteFilePath(), input, output, &copied, &error)) {
        return fail(error, OutputError);
    }
    out() << i18np("Took %1 field from “%2” and put it on “%3”.", "Took %1 fields from “%2” and put them on “%3”.",
                   copied, file.fileName(), output)
          << Qt::endl;
    return Success;
}

int exportAction(const QString &input, const QCommandLineParser &parser)
{
    const QString target = parser.value(u"output"_s);
    if (target.isEmpty()) {
        return fail(i18n("Say where the answers go with -o, in a file ending .fdf, .xfdf or .csv."), UsageError);
    }
    QString error;
    if (!FormBuilder::exportData(input, target, &error)) {
        return fail(error, OutputError);
    }
    out() << i18n("Wrote the answers to %1. The document itself is not in it.", target) << Qt::endl;
    return Success;
}

int importAction(const QString &input, const QCommandLineParser &parser)
{
    const QString from = parser.value(u"data"_s);
    if (from.isEmpty()) {
        return fail(i18n("Say which answers to read with --data, in a file ending .fdf, .xfdf or .csv."), UsageError);
    }
    const QString output = destination(parser, input);
    QString error;
    int filled = 0;
    if (!FormBuilder::importData(input, output, from, &filled, &error)) {
        return fail(error, OutputError);
    }
    out() << i18np("Filled in %1 field. The result is in %2.", "Filled in %1 fields. The result is in %2.", filled,
                   output)
          << Qt::endl;
    return Success;
}

int collectAction(const QStringList &files, const QCommandLineParser &parser)
{
    if (files.isEmpty()) {
        return fail(i18n("field collect needs the filled-in documents."), UsageError);
    }
    const QString target = parser.value(u"output"_s);
    if (target.isEmpty()) {
        return fail(i18n("Say where the table goes with -o, in a file ending .csv."), UsageError);
    }
    QStringList paths;
    for (const QString &file : files) {
        const QFileInfo info(file);
        if (!info.exists() || !info.isReadable()) {
            return fail(i18n("“%1” cannot be read.", file));
        }
        paths << info.absoluteFilePath();
    }

    QString error;
    if (!FormBuilder::collect(paths, target, &error)) {
        return fail(error, OutputError);
    }
    out() << i18np("Wrote the answers of %1 document to %2, one row each.",
                   "Wrote the answers of %1 documents to %2, one row each.", int(paths.size()), target)
          << Qt::endl;
    return Success;
}

// ── The group ─────────────────────────────────────────────────────────────

/** The actions, for the message that lists them when one is missing or misspelt. */
QString actionList()
{
    return QStringLiteral("add, button, calculate, collect, copy, export, format, from-json, import, limitations, "
                          "list, move, remove, rename, tab-order, update, validate");
}

void formOptions(QCommandLineParser &parser)
{
    parser.addOptions({
        { u"type"_s, i18n("What kind of field: text, password, checkbox, radio, choice, list, button or signature."),
          i18nc("@label:chooser command-line placeholder", "kind") },
        { u"name"_s, i18n("Which field. This is the name filling in addresses; a full stop in it makes a group."),
          i18nc("@label command-line placeholder", "name") },
        { u"to-name"_s, i18n("What to call the field instead."), i18nc("@label command-line placeholder", "name") },
        { u"value"_s, i18n("What the field holds to start with."), i18nc("@label command-line placeholder", "text") },
        { u"options"_s, i18n("What may be chosen, as A,B,C. For a radio group, one --at goes with each."),
          i18nc("@label command-line placeholder", "list") },
        { u"export-values"_s, i18n("What gets stored for each choice, when that differs from what is shown."),
          i18nc("@label command-line placeholder", "list") },
        { u"on-state"_s, i18n("What a ticked box stores. Default: Yes."),
          i18nc("@label command-line placeholder", "name") },
        { u"tooltip"_s, i18n("What a person sees next to the field."),
          i18nc("@label command-line placeholder", "text") },
        { u"required"_s, i18n("The form cannot be sent without it.") },
        { u"read-only"_s, i18n("Show the field but do not let anybody change it.") },
        { u"multiline"_s, i18n("A text field of several lines.") },
        { u"comb"_s, i18n("One character per cell, which needs --max-length.") },
        { u"editable"_s, i18n("A drop-down that also takes typing.") },
        { u"multi-select"_s, i18n("A list of which several entries may be picked.") },
        { u"max-length"_s, i18n("How many characters a text field takes. 0 for no limit."),
          i18nc("@label command-line placeholder", "number"), QStringLiteral("0") },
        { u"field-align"_s, i18n("Where the text sits: left, centre or right."),
          i18nc("@label command-line placeholder", "side") },
        { u"field-font"_s, i18n("One of the fourteen fonts every reader knows, e.g. Helv or TiRo."),
          i18nc("@label command-line placeholder", "name"), QStringLiteral("Helv") },
        { u"background"_s, i18n("Colour behind the field, a name or #rrggbb."),
          i18nc("@label command-line placeholder", "colour") },
        { u"border-colour"_s, i18n("Colour of the frame, a name or #rrggbb."),
          i18nc("@label command-line placeholder", "colour") },
        { u"border-width"_s, i18n("Thickness of the frame, in points."),
          i18nc("@label command-line placeholder", "points"), QStringLiteral("1") },
        { u"border-style"_s, i18n("Frame: solid, dashed, beveled, inset or underline."),
          i18nc("@label command-line placeholder", "style") },
        { u"order"_s, i18n("The order the tab key walks the fields of a page, as NAME,NAME,…"),
          i18nc("@label command-line placeholder", "names") },
        { u"template"_s, i18n("An empty form whose fields are to be copied."),
          i18nc("@label command-line placeholder", "file") },
        { u"data"_s, i18n("Answers to read in, from an .fdf, .xfdf or .csv file."),
          i18nc("@label command-line placeholder", "file") },
        { u"as"_s, i18n("How a value is shown: none, number, currency, percent, date, time, zip or phone."),
          i18nc("@label command-line placeholder", "kind") },
        { u"decimals"_s, i18n("Places after the decimal point."), i18nc("@label command-line placeholder", "number"),
          QStringLiteral("2") },
        { u"symbol"_s, i18n("Currency symbol to show."), i18nc("@label command-line placeholder", "text") },
        { u"between"_s, i18n("The numbers a field may hold, as SMALLEST,LARGEST."),
          i18nc("@label command-line placeholder", "range") },
        { u"how"_s, i18n("What to work out: sum, product, average, minimum or maximum."),
          i18nc("@label command-line placeholder", "way") },
        { u"sources"_s, i18n("The fields to work it out from, as NAME,NAME,…"),
          i18nc("@label command-line placeholder", "names") },
        { u"action"_s, i18n("What pressing a button does: reset, submit, goto or url."),
          i18nc("@label command-line placeholder", "what") },
        { u"target"_s, i18n("Where the button sends or goes: an address, or a page number."),
          i18nc("@label command-line placeholder", "address") },
    });
}

std::optional<int> formRun(const QString &verb, const QStringList &arguments, const QCommandLineParser &parser)
{
    if (verb != QLatin1String("field")) {
        return std::nullopt;
    }
    if (arguments.isEmpty()) {
        return fail(i18n("field needs something to do: %1.", actionList()), UsageError);
    }

    QStringList rest = arguments;
    const QString action = rest.takeFirst().toLower();

    if (action == QLatin1String("limitations") || action == QLatin1String("limits")) {
        const QStringList notes = FormBuilder::limitations();
        for (const QString &note : notes) {
            out() << QStringLiteral("• ") << note << Qt::endl;
        }
        return Success;
    }
    if (action == QLatin1String("collect")) {
        return collectAction(rest, parser);
    }

    QString input;
    int code = Success;
    if (!oneInputFile(rest, verb + u' ' + action, &input, &code)) {
        return code;
    }

    if (action == QLatin1String("list")) {
        return listAction(input, parser.isSet(u"json"_s));
    }
    if (action == QLatin1String("add")) {
        return addAction(input, parser);
    }
    if (action == QLatin1String("from-json")) {
        return fromJsonAction(input, parser);
    }
    if (action == QLatin1String("remove")) {
        return removeAction(input, parser);
    }
    if (action == QLatin1String("rename")) {
        return renameAction(input, parser);
    }
    if (action == QLatin1String("move")) {
        return changeAction(input, parser, true);
    }
    if (action == QLatin1String("update")) {
        return changeAction(input, parser, false);
    }
    if (action == QLatin1String("tab-order")) {
        return tabOrderAction(input, parser);
    }
    if (action == QLatin1String("format")) {
        return formatAction(input, parser);
    }
    if (action == QLatin1String("validate")) {
        return validateAction(input, parser);
    }
    if (action == QLatin1String("calculate")) {
        return calculateAction(input, parser);
    }
    if (action == QLatin1String("button")) {
        return buttonAction(input, parser);
    }
    if (action == QLatin1String("copy")) {
        return copyAction(input, parser);
    }
    if (action == QLatin1String("export")) {
        return exportAction(input, parser);
    }
    if (action == QLatin1String("import")) {
        return importAction(input, parser);
    }

    return fail(i18n("“%1” is not something field can do. Try one of: %2.", action, actionList()), UsageError);
}

QStringList formHelp()
{
    return { i18n("  field add       FILE  -o OUT      Put a new field on a page"),
             i18n("  field button    FILE  -o OUT      Make a button reset, submit, jump or open a link"),
             i18n("  field calculate FILE  -o OUT      Work a field's value out from other fields"),
             i18n("  field collect   FILE... -o CSV    Gather many filled-in copies into one table"),
             i18n("  field copy      FILE  -o OUT      Take the fields from an empty form of the same layout"),
             i18n("  field export    FILE  -o DATA     Write the answers to FDF, XFDF or CSV"),
             i18n("  field format    FILE  -o OUT      Show a value as a number, a date or an amount"),
             i18n("  field from-json FILE  -o OUT      Build a whole form from a description"),
             i18n("  field import    FILE  -o OUT      Fill the fields from FDF, XFDF or CSV"),
             i18n("  field limitations                 What making forms cannot promise"),
             i18n("  field list      FILE              Show every field, where it sits and what it holds"),
             i18n("  field move      FILE  -o OUT      Put a field somewhere else on its page"),
             i18n("  field remove    FILE  -o OUT      Take fields out"),
             i18n("  field rename    FILE  -o OUT      Call a field something else"),
             i18n("  field tab-order FILE  -o OUT      The order the tab key walks a page"),
             i18n("  field update    FILE  -o OUT      Change what a field is and how it looks"),
             i18n("  field validate  FILE  -o OUT      Refuse a number outside a range") };
}

} // namespace

Group formGroup()
{
    return { formOptions, formRun, formHelp };
}

} // namespace ps::cli
