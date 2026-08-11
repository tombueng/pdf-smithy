/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#include "Forms.h"

#include "PdfFile.h"
#include "PdfGeometry.h"

#include <KLocalizedString>

#include <QTransform>

#include <qpdf/Buffer.hh>
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFAcroFormDocumentHelper.hh>
#include <qpdf/QPDFAnnotationObjectHelper.hh>
#include <qpdf/QPDFFormFieldObjectHelper.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>
#include <qpdf/QPDFWriter.hh>

#include <QSet>
#include <QStringConverter>

#include <algorithm>
#include <map>

namespace ps {

namespace {

/** Field flags that matter here, from the field-flag table. */
constexpr int flagReadOnly = 1 << 0;
constexpr int flagRequired = 1 << 1;
constexpr int flagMultiline = 1 << 12;
constexpr int flagPassword = 1 << 13;
constexpr int flagFileSelect = 1 << 20;
constexpr int flagComb = 1 << 24;

FormField::Kind kindOf(QPDFFormFieldObjectHelper &field)
{
    // Order matters: a checkbox and a radio button are both /Btn, and a push
    // button is one too, so the specific tests come before the general one.
    if (field.isCheckbox()) {
        return FormField::Kind::Checkbox;
    }
    if (field.isRadioButton()) {
        return FormField::Kind::Radio;
    }
    if (field.isPushbutton()) {
        return FormField::Kind::Button;
    }
    if (field.isChoice()) {
        return FormField::Kind::Choice;
    }
    if (field.isText()) {
        return FormField::Kind::Text;
    }
    return field.getFieldType() == "/Sig" ? FormField::Kind::Signature : FormField::Kind::Unknown;
}

/** The states a checkbox or radio widget will accept, other than /Off. */
QStringList onStatesOf(QPDFObjectHandle widget)
{
    QStringList states;
    QPDFObjectHandle appearance = widget.getKey("/AP");
    QPDFObjectHandle normal = appearance.isDictionary() ? appearance.getKey("/N") : QPDFObjectHandle::newNull();
    if (!normal.isDictionary()) {
        return states;
    }
    for (const auto &[name, value] : normal.getDictAsMap()) {
        Q_UNUSED(value)
        if (name != "/Off") {
            states << QString::fromStdString(name);
        }
    }
    return states;
}

/** Whether a written value means "ticked". */
bool meansOn(const QString &value)
{
    static const QStringList yes { QStringLiteral("on"),  QStringLiteral("yes"), QStringLiteral("true"),
                                   QStringLiteral("1"),   QStringLiteral("x"),   QStringLiteral("ja"),
                                   QStringLiteral("wahr") };
    return yes.contains(value.trimmed().toLower());
}

double clamp01(double value)
{
    return std::clamp(value, 0.0, 1.0);
}

/** PDF 32000-1, 10.4: the specification's own DeviceCMYK to DeviceRGB. */
QColor fromCmyk(double c, double m, double y, double k)
{
    return QColor::fromRgbF(float(1.0 - std::min(1.0, c + k)), float(1.0 - std::min(1.0, m + k)),
                            float(1.0 - std::min(1.0, y + k)));
}

/**
 * A colour out of an /MK entry, whose length is what says which space it is in.
 *
 * One number is grey, three are RGB, four are CMYK, and none at all means
 * transparent, which is why an empty array comes back as an invalid colour
 * rather than as black. A field that asks for no background and a field that
 * asks for a black one look identical to anything that cannot tell those apart.
 */
QColor colourOf(QPDFObjectHandle array)
{
    if (!array.isArray()) {
        return {};
    }
    const auto channel = [&array](int index) { return clamp01(PdfGeometry::numberAt(array, index, 0.0)); };
    switch (array.getArrayNItems()) {
    case 1: {
        const float grey = float(channel(0));
        return QColor::fromRgbF(grey, grey, grey);
    }
    case 3:
        return QColor::fromRgbF(float(channel(0)), float(channel(1)), float(channel(2)));
    case 4:
        return fromCmyk(channel(0), channel(1), channel(2), channel(3));
    default:
        return {};
    }
}

FormField::BorderStyle borderStyleOf(QPDFObjectHandle style)
{
    const std::string name = style.isName() ? style.getName() : std::string();
    if (name == "/D") {
        return FormField::BorderStyle::Dashed;
    }
    if (name == "/B") {
        return FormField::BorderStyle::Beveled;
    }
    if (name == "/I") {
        return FormField::BorderStyle::Inset;
    }
    if (name == "/U") {
        return FormField::BorderStyle::Underline;
    }
    return FormField::BorderStyle::Solid;
}

/**
 * Splits a fragment of a content stream into its tokens.
 *
 * A name needs no space in front of it, so `0 0 1 rg/Helv 9 Tf` is a perfectly
 * legal /DA that a plain split on whitespace reads as the single token "rg/Helv",
 * and then reports no font at all.
 */
QStringList tokensOf(const QString &stream)
{
    QStringList tokens;
    QString current;
    const auto flush = [&tokens, &current] {
        if (!current.isEmpty()) {
            tokens.append(current);
            current.clear();
        }
    };
    for (const QChar character : stream) {
        if (character.isSpace()) {
            flush();
        } else if (character == QLatin1Char('/')) {
            flush();
            current = character;
        } else {
            current += character;
        }
    }
    flush();
    return tokens;
}

/**
 * Reads the font, the size and the colour out of a /DA string.
 *
 * /DA is a miniature content stream (`/Helv 12 Tf 0 g`) in which the operands
 * come before their operator and the operators may appear in any order, so it is
 * read by keeping the operands seen so far and acting when one of the four that
 * matter arrives.
 *
 * The numbers go through QString::toDouble, which is always C-locale, for the
 * same reason PdfGeometry::numericValue() exists: strtod honours LC_NUMERIC, and
 * on a German system `0.5 g` would parse as 0 and paint a black the document
 * never asked for.
 */
void readDefaultAppearance(const QString &appearance, FormField &field)
{
    QStringList operands;
    const auto operand = [&operands](int fromTheEnd) { return operands.at(operands.size() - fromTheEnd).toDouble(); };

    for (const QString &token : tokensOf(appearance)) {
        if (token == QLatin1String("Tf")) {
            if (operands.size() >= 2 && operands.at(operands.size() - 2).startsWith(QLatin1Char('/'))) {
                field.fontResource = operands.at(operands.size() - 2).mid(1);
                // A size of 0 is the specification's way of saying "fit it to
                // the box", so it is passed on as 0 rather than as a guess only
                // whoever knows the box could make honestly.
                field.fontSize = qMax(0.0, operand(1));
            }
        } else if (token == QLatin1String("g") && operands.size() >= 1) {
            const float grey = float(clamp01(operand(1)));
            field.textColour = QColor::fromRgbF(grey, grey, grey);
        } else if (token == QLatin1String("rg") && operands.size() >= 3) {
            field.textColour
                = QColor::fromRgbF(float(clamp01(operand(3))), float(clamp01(operand(2))), float(clamp01(operand(1))));
        } else if (token == QLatin1String("k") && operands.size() >= 4) {
            field.textColour
                = fromCmyk(clamp01(operand(4)), clamp01(operand(3)), clamp01(operand(2)), clamp01(operand(1)));
        } else {
            operands.append(token);
            continue;
        }
        operands.clear();
    }
}

/** The JavaScript of one entry of an /AA dictionary, and nothing else. */
QString scriptOf(QPDFObjectHandle actions, const std::string &key)
{
    if (!actions.isDictionary()) {
        return {};
    }
    QPDFObjectHandle action = actions.getKey(key);
    if (!action.isDictionary()) {
        return {};
    }

    QPDFObjectHandle script = action.getKey("/JS");
    if (script.isString()) {
        return QString::fromStdString(script.getUTF8Value());
    }
    if (!script.isStream()) {
        return {};
    }

    // Anything longer than a line or two is routinely put in a stream instead,
    // and a calculation script is exactly that long.
    try {
        std::shared_ptr<Buffer> buffer = script.getStreamData();
        const QByteArray bytes(reinterpret_cast<const char *>(buffer->getBuffer()), qsizetype(buffer->getSize()));
        if (bytes.startsWith("\xFE\xFF")) {
            // A text stream may be UTF-16BE behind a byte-order mark, and reading
            // one as UTF-8 yields a page of replacement characters rather than a
            // script anybody could recognise.
            QStringDecoder decoder(QStringConverter::Utf16BE);
            return decoder(bytes.mid(2));
        }
        return QString::fromUtf8(bytes);
    } catch (const std::exception &) {
        // A script this cannot be decompressed is not worth failing the whole
        // read over; every other field still has something to say.
        return {};
    }
}

QRectF rectOf(QPDFObjectHandle widget)
{
    QPDFObjectHandle rect = widget.getKey("/Rect");
    if (!rect.isArray() || rect.getArrayNItems() != 4) {
        return {};
    }
    const double left = PdfGeometry::boxValue(rect, 0, 0.0);
    const double bottom = PdfGeometry::boxValue(rect, 1, 0.0);
    const double right = PdfGeometry::boxValue(rect, 2, 0.0);
    const double top = PdfGeometry::boxValue(rect, 3, 0.0);
    return QRectF(std::min(left, right), std::min(bottom, top), std::abs(right - left), std::abs(top - bottom));
}

} // namespace

bool Forms::hasForm(const QString &pdf)
{
    try {
        QPDF document;
        PdfFile::open(document, pdf);
        QPDFAcroFormDocumentHelper forms(document);
        return forms.hasAcroForm() && !forms.getFormFields().empty();
    } catch (const std::exception &) {
        return false;
    }
}

QString Forms::describe(FormField::Kind kind)
{
    switch (kind) {
    case FormField::Kind::Text:
        return i18nc("@item form field kind", "Text");
    case FormField::Kind::Checkbox:
        return i18nc("@item form field kind", "Tick box");
    case FormField::Kind::Radio:
        return i18nc("@item form field kind", "Choice of one");
    case FormField::Kind::Choice:
        return i18nc("@item form field kind", "List");
    case FormField::Kind::Button:
        return i18nc("@item form field kind", "Button");
    case FormField::Kind::Signature:
        return i18nc("@item form field kind", "Signature");
    case FormField::Kind::Unknown:
        break;
    }
    return i18nc("@item form field kind", "Unknown");
}

QVector<FormField> Forms::read(const QString &pdf, QString *error)
{
    QVector<FormField> fields;
    try {
        QPDF document;
        PdfFile::open(document, pdf);

        QPDFAcroFormDocumentHelper forms(document);
        if (!forms.hasAcroForm()) {
            return fields;
        }

        // Widget to page, so a field can say where it is without walking the
        // page tree once per field.
        std::map<QPDFObjGen, int> pageOfWidget;
        std::map<int, QTransform> toDisplay;
        std::vector<QPDFPageObjectHelper> pages = QPDFPageDocumentHelper(document).getAllPages();
        for (size_t i = 0; i < pages.size(); ++i) {
            const QRectF media = PdfGeometry::mediaBoxOf(pages[i]);
            bool invertible = false;
            const QTransform inverse
                = (PdfGeometry::displayToPageTransform(PdfGeometry::rotationOf(pages[i]), media.width(), media.height())
                   * QTransform::fromTranslate(media.x(), media.y()))
                      .inverted(&invertible);
            if (invertible) {
                toDisplay[int(i)] = inverse;
            }

            QPDFObjectHandle annots = pages[i].getObjectHandle().getKey("/Annots");
            if (!annots.isArray()) {
                continue;
            }
            for (int a = 0; a < annots.getArrayNItems(); ++a) {
                pageOfWidget[annots.getArrayItem(a).getObjGen()] = int(i);
            }
        }

        // Where each name landed, for folding the widgets of one field together.
        QHash<QString, int> indexOfName;

        for (QPDFFormFieldObjectHelper &field : forms.getFormFields()) {
            FormField entry;
            entry.name = QString::fromStdString(field.getFullyQualifiedName());
            entry.label = QString::fromStdString(field.getAlternativeName());
            if (entry.label.isEmpty()) {
                entry.label = entry.name;
            }
            entry.kind = kindOf(field);
            entry.value = QString::fromStdString(field.getValueAsString());

            // A tick box's value is a name, not a string, so asking for it as a
            // string gives nothing back and the box reads as unset whatever it
            // actually is.
            if (entry.kind == FormField::Kind::Checkbox || entry.kind == FormField::Kind::Radio) {
                QPDFObjectHandle state = field.getInheritableFieldValue("/V");
                entry.value = state.isName() ? QString::fromStdString(state.getName()) : QStringLiteral("/Off");
            }

            const int flags = field.getFlags();
            entry.readOnly = (flags & flagReadOnly) != 0;
            entry.required = (flags & flagRequired) != 0;
            entry.multiline = entry.kind == FormField::Kind::Text && (flags & flagMultiline) != 0;

            if (entry.kind == FormField::Kind::Choice) {
                for (const std::string &choice : field.getChoices()) {
                    entry.options << QString::fromStdString(choice);
                }
            }

            QPDFObjectHandle handle = field.getObjectHandle();

            // getFormFields() answers with terminal fields only, so every kid of
            // one of them is a widget rather than another field; a field with no
            // kids is its own widget, the arrangement most documents use for the
            // fields that appear once. Walking /Kids here rather than asking qpdf
            // for the annotations of the field keeps the widgets of a document
            // whose pages never listed them in their /Annots, which is broken but
            // readable and still has a rectangle worth reporting.
            QVector<QPDFObjectHandle> widgetHandles;
            QPDFObjectHandle kids = handle.getKey("/Kids");
            for (int k = 0; kids.isArray() && k < kids.getArrayNItems(); ++k) {
                widgetHandles.append(kids.getArrayItem(k));
            }
            if (widgetHandles.isEmpty()) {
                widgetHandles.append(handle);
            }

            for (const QPDFObjectHandle &handleOfWidget : std::as_const(widgetHandles)) {
                FormField::Widget widget;
                const auto found = pageOfWidget.find(handleOfWidget.getObjGen());
                if (found != pageOfWidget.end()) {
                    widget.page = found->second;
                    if (toDisplay.count(widget.page) > 0) {
                        widget.rect = toDisplay.at(widget.page).mapRect(rectOf(handleOfWidget));
                    }
                }

                if (entry.kind == FormField::Kind::Checkbox || entry.kind == FormField::Kind::Radio) {
                    // A tick box's "on" name is whatever its appearance calls it
                    // (/Yes, /On, /1, anything), and writing the wrong one
                    // leaves the box empty with a value set, which looks like a
                    // bug in the reader. A well-made widget states exactly one;
                    // where a document states several, the first is the one its
                    // /AP would show and the rest are still reported in options.
                    widget.onState = onStatesOf(handleOfWidget).value(0);
                }

                QPDFObjectHandle widgetAppearance = handleOfWidget.getKey("/AP");
                widget.hasAppearance = widgetAppearance.isDictionary() && !widgetAppearance.getKey("/N").isNull();

                entry.widgets.append(widget);
            }

            if (entry.kind == FormField::Kind::Checkbox || entry.kind == FormField::Kind::Radio) {
                for (const QPDFObjectHandle &handleOfWidget : std::as_const(widgetHandles)) {
                    entry.options += onStatesOf(handleOfWidget);
                }
                entry.options.removeDuplicates();
            }

            // What the field reports as its own box is the first widget a page
            // admits to holding. A widget nothing lists is drawable nowhere, and
            // answering with it would put the field on a page it is not on.
            int primary = 0;
            for (int w = 0; w < entry.widgets.size(); ++w) {
                if (entry.widgets.at(w).page >= 0) {
                    primary = w;
                    break;
                }
            }
            entry.page = entry.widgets.at(primary).page;
            entry.rect = entry.widgets.at(primary).rect;
            entry.hasAppearance = entry.widgets.at(primary).hasAppearance;

            // The styling lives on the widget, not on the field. When no widget
            // turned up on a page (a document whose /Annots never listed it),
            // the first one is still a far better source than the field itself,
            // which in that arrangement carries no appearance keys at all.
            QPDFObjectHandle styled = widgetHandles.at(primary);

            QPDFObjectHandle maxLength = field.getInheritableFieldValue("/MaxLen");
            if (maxLength.isNumber()) {
                entry.maxLength = int(PdfGeometry::numericValue(maxLength, -1.0));
            }

            // The comb flag means nothing on its own: the specification ignores it
            // without a /MaxLen to say how many cells there are, and on a
            // multi-line, password or file-selection field as well.
            entry.comb = entry.kind == FormField::Kind::Text && (flags & flagComb) != 0 && entry.maxLength > 0
                && !entry.multiline && (flags & (flagPassword | flagFileSelect)) == 0;

            QPDFObjectHandle characteristics = styled.getKey("/MK");
            if (characteristics.isDictionary()) {
                entry.background = colourOf(characteristics.getKey("/BG"));
                entry.border = colourOf(characteristics.getKey("/BC"));
                QPDFObjectHandle caption = characteristics.getKey("/CA");
                if (caption.isString()) {
                    entry.caption = QString::fromStdString(caption.getUTF8Value());
                }
            }

            QPDFObjectHandle borderStyle = styled.getKey("/BS");
            QPDFObjectHandle legacyBorder = styled.getKey("/Border");
            if (borderStyle.isDictionary()) {
                entry.borderWidth = qMax(0.0, PdfGeometry::numericValue(borderStyle.getKey("/W"), 1.0));
                entry.borderStyle = borderStyleOf(borderStyle.getKey("/S"));
            } else if (legacyBorder.isArray() && legacyBorder.getArrayNItems() >= 3) {
                // /BS supersedes /Border, but documents written before /BS existed
                // still say "no border" the only way they could, and drawing them
                // a one-point one would be inventing it.
                entry.borderWidth = qMax(0.0, PdfGeometry::numberAt(legacyBorder, 2, 1.0));
            }

            // qpdf resolves both of these up the field tree and then falls back to
            // /AcroForm, which is where most documents state them exactly once.
            readDefaultAppearance(QString::fromStdString(field.getDefaultAppearance()), entry);
            entry.alignment = qBound(0, field.getQuadding(), 2);

            // /AA sits on the field for the four that concern a value, and readers
            // accept it on the widget too when the two dictionaries are merged.
            QPDFObjectHandle actions = handle.getKey("/AA");
            if (!actions.isDictionary()) {
                actions = styled.getKey("/AA");
            }
            entry.formatScript = scriptOf(actions, "/F");
            entry.keystrokeScript = scriptOf(actions, "/K");
            entry.validateScript = scriptOf(actions, "/V");
            entry.calculateScript = scriptOf(actions, "/C");

            if (entry.name.isEmpty()) {
                continue;
            }

            // A terminal field is one with no field among its children, and the
            // buttons of a radio group are widgets rather than fields: they
            // carry no /T of their own, so qpdf hands back one terminal field
            // per button, every one of them under the group's name. Two entries
            // with the same fully qualified name *are* one field (that is what
            // the name qualifies), sharing one value, so they are folded into
            // one here. Left apart, they would be several fields answering for
            // each other: setting the value on one would read as set on all of
            // them, which is a group in which every choice is chosen at once.
            const auto already = indexOfName.constFind(entry.name);
            if (already == indexOfName.constEnd()) {
                indexOfName.insert(entry.name, int(fields.size()));
                fields.append(entry);
                continue;
            }

            FormField &existing = fields[already.value()];
            existing.widgets += entry.widgets;
            if (existing.kind == FormField::Kind::Checkbox || existing.kind == FormField::Kind::Radio) {
                // Each button knows only its own state; the field's options are
                // what the group as a whole will accept.
                existing.options += entry.options;
                existing.options.removeDuplicates();
            }
            if (existing.page < 0 && entry.page >= 0) {
                // The first widget any page admits to holding is the one the
                // field reports as its own, whichever entry it arrived in.
                existing.page = entry.page;
                existing.rect = entry.rect;
                existing.hasAppearance = entry.hasAppearance;
            }
        }
    } catch (const std::exception &e) {
        if (error) {
            *error = QString::fromUtf8(e.what());
        }
        return {};
    }

    return fields;
}

bool Forms::fill(const QString &inputPdf, const QString &outputPdf, const QHash<QString, QString> &values, int *filled,
                 QStringList *unknown, QString *error)
{
    if (values.isEmpty()) {
        if (error) {
            *error = i18n("There is nothing to fill in.");
        }
        return false;
    }

    int count = 0;
    QStringList missing = values.keys();

    try {
        QPDF pdf;
        PdfFile::open(pdf, inputPdf);

        QPDFAcroFormDocumentHelper forms(pdf);
        if (!forms.hasAcroForm()) {
            if (error) {
                *error = i18n("This document has no form to fill in.");
            }
            return false;
        }

        // A radio group arrives as several widgets sharing one name, so the name
        // has to be settled once rather than once per widget: otherwise the last
        // widget, whose own state is not the chosen one, writes /Off over the
        // choice the one before it just made.
        QSet<QString> settled;

        for (QPDFFormFieldObjectHelper &field : forms.getFormFields()) {
            const QString name = QString::fromStdString(field.getFullyQualifiedName());
            const auto wanted = values.constFind(name);
            if (wanted == values.constEnd()) {
                continue;
            }
            missing.removeAll(name);
            if (settled.contains(name)) {
                continue;
            }

            if ((field.getFlags() & flagReadOnly) != 0) {
                if (unknown) {
                    // Not unknown, but equally worth saying: the form author
                    // locked it, and silently not filling it would be worse.
                    *unknown << i18n("“%1” is locked by the form and was left as it is.", name);
                }
                continue;
            }

            const FormField::Kind kind = kindOf(field);
            if (kind == FormField::Kind::Checkbox || kind == FormField::Kind::Radio) {
                // The value belongs on the field that carries the name, which for
                // a group of widgets is their parent. Writing it on a widget
                // leaves the group unset and the widget disagreeing with it.
                QPDFObjectHandle owner = field.getObjectHandle();
                QVector<QPDFObjectHandle> widgets;
                if (owner.getKey("/Kids").isArray()) {
                    QPDFObjectHandle kids = owner.getKey("/Kids");
                    for (int k = 0; k < kids.getArrayNItems(); ++k) {
                        widgets.append(kids.getArrayItem(k));
                    }
                } else if (owner.getKey("/Parent").isDictionary()
                           && owner.getKey("/Parent").getKey("/Kids").isArray()) {
                    owner = owner.getKey("/Parent");
                    QPDFObjectHandle kids = owner.getKey("/Kids");
                    for (int k = 0; k < kids.getArrayNItems(); ++k) {
                        widgets.append(kids.getArrayItem(k));
                    }
                } else {
                    widgets.append(owner);
                }

                QStringList states;
                for (const QPDFObjectHandle &widget : std::as_const(widgets)) {
                    states += onStatesOf(widget);
                }
                states.removeDuplicates();

                QString state = QStringLiteral("/Off");
                if (kind == FormField::Kind::Radio && states.contains(wanted.value())) {
                    state = wanted.value(); // Naming one option of a group.
                } else if (kind == FormField::Kind::Radio && states.contains(QLatin1Char('/') + wanted.value())) {
                    state = QLatin1Char('/') + wanted.value();
                } else if (meansOn(wanted.value())) {
                    state = states.isEmpty() ? QStringLiteral("/Yes") : states.constFirst();
                }
                QPDFFormFieldObjectHelper(owner).setV(QPDFObjectHandle::newName(state.toStdString()), true);

                // Each widget shows the chosen state or nothing. Without /AS the
                // group holds the right value and every button looks empty.
                for (QPDFObjectHandle widget : std::as_const(widgets)) {
                    const QStringList mine = onStatesOf(widget);
                    widget.replaceKey("/AS",
                                      QPDFObjectHandle::newName(mine.contains(state) ? state.toStdString()
                                                                                     : std::string("/Off")));
                }
                settled.insert(name);
            } else {
                field.setV(wanted.value().toStdString(), true);
            }
            ++count;
        }

        // Values without appearances show up blank in a good half of the readers
        // in use, printing ones included.
        forms.generateAppearancesIfNeeded();

        QPDFWriter writer(pdf, outputPdf.toUtf8().constData());
        writer.setObjectStreamMode(qpdf_o_generate);
        writer.setStreamDataMode(qpdf_s_preserve);
        writer.write();
    } catch (const std::exception &e) {
        if (error) {
            *error = QString::fromUtf8(e.what());
        }
        return false;
    }

    if (unknown) {
        for (const QString &name : std::as_const(missing)) {
            *unknown << i18n("This document has no field called “%1”.", name);
        }
    }
    if (filled) {
        *filled = count;
    }
    return true;
}

bool Forms::flatten(const QString &inputPdf, const QString &outputPdf, int *flattened, QString *error)
{
    int count = 0;
    try {
        QPDF pdf;
        PdfFile::open(pdf, inputPdf);

        QPDFAcroFormDocumentHelper forms(pdf);
        if (!forms.hasAcroForm()) {
            if (error) {
                *error = i18n("This document has no form to make permanent.");
            }
            return false;
        }
        count = int(forms.getFormFields().size());

        // Draw whatever is missing before flattening, or a field filled by
        // another tool would flatten to an empty box.
        forms.generateAppearancesIfNeeded();
        QPDFPageDocumentHelper(pdf).flattenAnnotations();

        // flattenAnnotations draws the widgets into the pages but leaves the
        // form dictionary behind; a reader finding one with no fields offers to
        // "repair" the document.
        pdf.getRoot().removeKey("/AcroForm");
        pdf.getRoot().removeKey("/Perms");

        QPDFWriter writer(pdf, outputPdf.toUtf8().constData());
        writer.setObjectStreamMode(qpdf_o_generate);
        writer.setStreamDataMode(qpdf_s_preserve);
        writer.write();
    } catch (const std::exception &e) {
        if (error) {
            *error = QString::fromUtf8(e.what());
        }
        return false;
    }

    if (flattened) {
        *flattened = count;
    }
    return true;
}

} // namespace ps
