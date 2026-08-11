/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#include "FormBuilder.h"

#include "Core14Widths.h"
#include "Forms.h"
#include "PdfFile.h"
#include "PdfGeometry.h"

#include <KLocalizedString>

#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSaveFile>
#include <QTransform>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>

#include <qpdf/QPDF.hh>
#include <qpdf/QPDFAcroFormDocumentHelper.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>
#include <qpdf/QPDFWriter.hh>

#include <algorithm>
#include <functional>
#include <map>

namespace ps {

namespace {

using PdfGeometry::number;

/**
 * The field-flag bits, from the table in the specification.
 *
 * Numbered from one there and from zero here, which is the single most common
 * way to get a form subtly wrong: a drop-down built with bit 17 instead of bit
 * 18 is a list box that swallows the first click.
 */
constexpr int flagReadOnly = 1 << 0;
constexpr int flagRequired = 1 << 1;
constexpr int flagMultiline = 1 << 12;
constexpr int flagPassword = 1 << 13;
constexpr int flagRadio = 1 << 15;
constexpr int flagPushButton = 1 << 16;
constexpr int flagCombo = 1 << 17;
constexpr int flagEdit = 1 << 18;
constexpr int flagMultiSelect = 1 << 21;
constexpr int flagComb = 1 << 24;

/** The resource names Acrobat has used for the standard fonts since 1996. */
struct FontAlias {
    const char *resource;
    const char *base;
};

constexpr FontAlias fontAliases[] = {
    { "Helv", "Helvetica" },
    { "HeBo", "Helvetica-Bold" },
    { "HeOb", "Helvetica-Oblique" },
    { "HeBO", "Helvetica-BoldOblique" },
    { "TiRo", "Times-Roman" },
    { "TiBo", "Times-Bold" },
    { "TiIt", "Times-Italic" },
    { "TiBI", "Times-BoldItalic" },
    { "Cour", "Courier" },
    { "CoBo", "Courier-Bold" },
    { "CoOb", "Courier-Oblique" },
    { "CoBO", "Courier-BoldOblique" },
    { "Symb", "Symbol" },
    { "ZaDb", "ZapfDingbats" },
};

QString baseFontFor(const QString &resource)
{
    for (const FontAlias &alias : fontAliases) {
        if (resource == QLatin1String(alias.resource)) {
            return QString::fromLatin1(alias.base);
        }
    }
    // An unrecognised name is taken at face value: someone naming a font they
    // have embedded themselves is better served by being believed than by
    // silently getting Helvetica.
    return resource;
}

const quint16 *widthsFor(const QString &baseFont)
{
    for (const Core14::Entry &entry : Core14::table) {
        if (baseFont == QLatin1String(entry.name)) {
            return entry.widths;
        }
    }
    return nullptr;
}

int averageWidthFor(const QString &baseFont)
{
    for (const Core14::Entry &entry : Core14::table) {
        if (baseFont == QLatin1String(entry.name)) {
            return entry.averageWidth;
        }
    }
    return 500;
}

/** How wide @p text draws, so that centring and shrinking are not guesses. */
double textWidth(const QString &text, const QString &baseFont, double size)
{
    const quint16 *widths = widthsFor(baseFont);
    const int average = averageWidthFor(baseFont);
    double thousandths = 0.0;
    for (const QChar &character : text) {
        const int code = character.unicode();
        if (widths && code >= Core14::firstCode && code <= Core14::lastCode) {
            thousandths += widths[code - Core14::firstCode];
        } else {
            thousandths += average;
        }
    }
    return thousandths * size / 1000.0;
}

std::string colourArray(const QColor &colour)
{
    return "[" + number(colour.redF()) + " " + number(colour.greenF()) + " " + number(colour.blueF()) + "]";
}

std::string setFill(const QColor &colour)
{
    return number(colour.redF()) + " " + number(colour.greenF()) + " " + number(colour.blueF()) + " rg\n";
}

std::string setStroke(const QColor &colour)
{
    return number(colour.redF()) + " " + number(colour.greenF()) + " " + number(colour.blueF()) + " RG\n";
}

std::string rectOperator(double x, double y, double w, double h)
{
    return number(x) + " " + number(y) + " " + number(w) + " " + number(h) + " re\n";
}

std::string rectArray(const QRectF &rect)
{
    return "[" + number(rect.left()) + " " + number(rect.bottom()) + " " + number(rect.right()) + " "
        + number(rect.top()) + "]";
}

/** A circle as the four Béziers everyone draws it with. */
std::string circlePath(double cx, double cy, double r)
{
    // 0.5523 is the classic circle-to-Bézier constant; less than that gives a
    // radio button with visibly flat sides.
    const double k = r * 0.5523;
    std::string path = number(cx - r) + " " + number(cy) + " m\n";
    path += number(cx - r) + " " + number(cy + k) + " " + number(cx - k) + " " + number(cy + r) + " " + number(cx) + " "
        + number(cy + r) + " c\n";
    path += number(cx + k) + " " + number(cy + r) + " " + number(cx + r) + " " + number(cy + k) + " " + number(cx + r)
        + " " + number(cy) + " c\n";
    path += number(cx + r) + " " + number(cy - k) + " " + number(cx + k) + " " + number(cy - r) + " " + number(cx) + " "
        + number(cy - r) + " c\n";
    path += number(cx - k) + " " + number(cy - r) + " " + number(cx - r) + " " + number(cy - k) + " " + number(cx - r)
        + " " + number(cy) + " c\n";
    return path;
}

/** A PDF literal string with the three characters that must be escaped handled. */
std::string literalString(const QString &text)
{
    std::string out = "(";
    for (const QChar &character : text) {
        const ushort code = character.unicode();
        if (code == '(' || code == ')' || code == '\\') {
            out += '\\';
        }
        if (code < 256) {
            out += char(code);
        } else {
            out += '?'; // WinAnsiEncoding cannot carry it; /V holds the real text.
        }
    }
    return out + ")";
}

/** Text for the file, in UTF-16 so that any language survives a field label. */
QPDFObjectHandle unicodeString(const QString &text)
{
    QByteArray bytes("\xFE\xFF", 2);
    for (const QChar &character : text) {
        bytes.append(char(character.unicode() >> 8));
        bytes.append(char(character.unicode() & 0xFF));
    }
    return QPDFObjectHandle::newString(std::string(bytes.constData(), size_t(bytes.size())));
}

/** A state name with the slash it must have, whether or not the caller wrote one. */
QString normalisedState(const QString &state)
{
    QString name = state.trimmed();
    if (name.isEmpty()) {
        return QStringLiteral("/Yes");
    }
    if (!name.startsWith(QLatin1Char('/'))) {
        name.prepend(QLatin1Char('/'));
    }
    // A space in a name would have to be escaped as #20; refusing to produce one
    // is easier to explain than a state that only some readers can match.
    name.replace(QLatin1Char(' '), QLatin1Char('_'));
    return name;
}

bool hasXfa(QPDF &pdf)
{
    QPDFObjectHandle acroForm = pdf.getRoot().getKey("/AcroForm");
    return acroForm.isDictionary() && !acroForm.getKey("/XFA").isNull();
}

bool checkNoXfa(QPDF &pdf, QString *error)
{
    if (!hasXfa(pdf)) {
        return true;
    }
    if (error) {
        *error = i18n("This document carries an XFA form, which is a second and incompatible description of the "
                      "same fields. Adding to it would give a file that behaves differently in every reader.");
    }
    return false;
}

void writeDocument(QPDF &pdf, const QString &path)
{
    QPDFWriter writer(pdf, path.toUtf8().constData());
    writer.setObjectStreamMode(qpdf_o_generate);
    writer.setStreamDataMode(qpdf_s_preserve);
    writer.write();
}

/** One terminal field, with the name that addresses it and where it is kept. */
struct Located {
    QPDFObjectHandle field;
    QPDFObjectHandle container; //!< the /Fields or /Kids array holding it
    QString name;
};

/**
 * Walks the field tree, which is not the same shape as the list of fields.
 *
 * A field with kids that carry their own /T is a group and has no value of its
 * own; a field with kids that do not is a radio group, and the parent is the
 * field. Getting that test the wrong way round turns one radio group into three
 * nameless fields.
 */
void walkFields(QPDFObjectHandle container, const QString &prefix, QVector<Located> *out, int depth)
{
    if (!container.isArray() || depth > 32) {
        return;
    }
    for (int i = 0; i < container.getArrayNItems(); ++i) {
        QPDFObjectHandle item = container.getArrayItem(i);
        if (!item.isDictionary()) {
            continue;
        }
        QPDFObjectHandle partialName = item.getKey("/T");
        const QString partial = partialName.isString() ? QString::fromStdString(partialName.getUTF8Value()) : QString();
        const QString full = partial.isEmpty() ? prefix
            : prefix.isEmpty()                 ? partial
                                               : prefix + QLatin1Char('.') + partial;

        QPDFObjectHandle kids = item.getKey("/Kids");
        bool group = false;
        for (int k = 0; kids.isArray() && k < kids.getArrayNItems(); ++k) {
            QPDFObjectHandle kid = kids.getArrayItem(k);
            if (kid.isDictionary() && kid.getKey("/T").isString()) {
                group = true;
                break;
            }
        }
        if (group) {
            walkFields(kids, full, out, depth + 1);
            continue;
        }
        out->append({ item, container, full });
    }
}

QVector<Located> allFields(QPDFObjectHandle acroForm)
{
    QVector<Located> fields;
    walkFields(acroForm.getKey("/Fields"), QString(), &fields, 0);
    return fields;
}

const Located *findField(const QVector<Located> &fields, const QString &name)
{
    for (const Located &field : fields) {
        if (field.name == name) {
            return &field;
        }
    }
    return nullptr;
}

/** The widgets a field draws itself with: itself, or its kids. */
QVector<QPDFObjectHandle> widgetsOf(QPDFObjectHandle field)
{
    QVector<QPDFObjectHandle> widgets;
    QPDFObjectHandle kids = field.getKey("/Kids");
    if (kids.isArray() && kids.getArrayNItems() > 0) {
        for (int k = 0; k < kids.getArrayNItems(); ++k) {
            QPDFObjectHandle kid = kids.getArrayItem(k);
            if (kid.isDictionary() && kid.hasKey("/Rect")) {
                widgets.append(kid);
            }
        }
        return widgets;
    }
    if (field.hasKey("/Rect")) {
        widgets.append(field);
    }
    return widgets;
}

/** Display points to page space, /Rotate and a non-zero media origin included. */
QTransform pageTransformFor(QPDFPageObjectHelper &page)
{
    const QRectF media = PdfGeometry::mediaBoxOf(page);
    return PdfGeometry::displayToPageTransform(PdfGeometry::rotationOf(page), media.width(), media.height())
        * QTransform::fromTranslate(media.x(), media.y());
}

/** Everything one run of building needs to hold on to. */
struct Build {
    QPDF *pdf = nullptr;
    QPDFObjectHandle acroForm;
    QPDFObjectHandle fieldsArray;
    QPDFObjectHandle drFonts;
    std::vector<QPDFPageObjectHelper> pages;
    QHash<QString, QPDFObjectHandle> fontCache;
};

/**
 * The font resource named by a /DA string, put where /DA can reach it.
 *
 * `/DR` in `/AcroForm` must carry the font that `/DA` names. When it does not,
 * the text of every field draws with no font at all, which in most readers
 * means nothing appears, and in the rest means a default nobody chose.
 */
QPDFObjectHandle fontFor(Build &build, const QString &resource)
{
    const auto cached = build.fontCache.constFind(resource);
    if (cached != build.fontCache.constEnd()) {
        return cached.value();
    }

    const std::string key = QString(QLatin1Char('/') + resource).toStdString();
    QPDFObjectHandle existing = build.drFonts.isDictionary() ? build.drFonts.getKey(key) : QPDFObjectHandle::newNull();
    if (existing.isDictionary()) {
        build.fontCache.insert(resource, existing);
        return existing;
    }

    const QString base = baseFontFor(resource);
    std::string definition = "<< /Type /Font /Subtype /Type1 /BaseFont /" + base.toStdString() + " >>";
    if (base != QLatin1String("Symbol") && base != QLatin1String("ZapfDingbats")) {
        // The two symbol fonts have their own built-in encoding; forcing WinAnsi
        // on them replaces every glyph with something Latin.
        definition
            = "<< /Type /Font /Subtype /Type1 /BaseFont /" + base.toStdString() + " /Encoding /WinAnsiEncoding >>";
    }
    QPDFObjectHandle font = build.pdf->makeIndirectObject(QPDFObjectHandle::parse(definition));
    if (build.drFonts.isDictionary()) {
        build.drFonts.replaceKey(key, font);
    }
    build.fontCache.insert(resource, font);
    return font;
}

double paddingOf(const FormBuilder::Field &field)
{
    return 1.0 + qMax(0.0, field.borderWidth);
}

/** The size to draw at, which is not always the size that was asked for. */
double effectiveSize(const FormBuilder::Field &field, double width, double height, const QString &text,
                     const QString &baseFont)
{
    if (field.fontSize > 0.0) {
        return field.fontSize;
    }
    // `0 Tf` in /DA is the legal, widely honoured way of saying "reader, you fit
    // it", but a reader that draws nothing of its own still needs a number in
    // the appearance stream, so this is our answer to the same question.
    double size = field.multiline ? 11.0 : qBound(4.0, height * 0.62, 12.0);
    if (!field.multiline && !text.isEmpty()) {
        const double available = qMax(1.0, width - 2.0 * paddingOf(field));
        const double unitWidth = textWidth(text, baseFont, 1.0);
        if (unitWidth > 0.0 && size * unitWidth > available) {
            size = qMax(4.0, available / unitWidth);
        }
    }
    return size;
}

/** The two triangles that make a button look raised, or pressed. */
std::string bevelPath(double w, double h, double inset, const QColor &light, const QColor &dark)
{
    const double d = inset;
    std::string out = setFill(light);
    out += number(d) + " " + number(d) + " m " + number(d) + " " + number(h - d) + " l " + number(w - d) + " "
        + number(h - d) + " l " + number(w - 2 * d) + " " + number(h - 2 * d) + " l " + number(2 * d) + " "
        + number(h - 2 * d) + " l " + number(2 * d) + " " + number(2 * d) + " l f\n";
    out += setFill(dark);
    out += number(w - d) + " " + number(h - d) + " m " + number(w - d) + " " + number(d) + " l " + number(d) + " "
        + number(d) + " l " + number(2 * d) + " " + number(2 * d) + " l " + number(w - 2 * d) + " " + number(2 * d)
        + " l " + number(w - 2 * d) + " " + number(h - 2 * d) + " l f\n";
    return out;
}

/**
 * The background and the border.
 *
 * Drawn before, and outside, the marked content that holds the value, because
 * that pair is exactly what a reader replaces when someone types. A border drawn
 * inside it survives until the first keystroke.
 */
std::string drawFrame(const FormBuilder::Field &field, double w, double h, bool circular)
{
    const QString style = field.borderStyle.trimmed().toLower();
    const double bw = qMax(0.0, field.borderWidth);
    const double radius = qMax(0.0, qMin(w, h) / 2.0);

    std::string out = "q\n";
    if (field.backgroundColour.isValid()) {
        out += setFill(field.backgroundColour);
        out += circular ? circlePath(w / 2.0, h / 2.0, radius) + "f\n" : rectOperator(0, 0, w, h) + "f\n";
    }

    if (field.borderColour.isValid() && bw > 0.0) {
        out += setStroke(field.borderColour) + number(bw) + " w\n";
        if (style == QLatin1String("underline")) {
            out += number(0.0) + " " + number(bw / 2.0) + " m " + number(w) + " " + number(bw / 2.0) + " l S\n";
        } else {
            if (style == QLatin1String("dashed")) {
                out += "[" + number(bw * 3.0) + " " + number(bw * 2.0) + "] 0 d\n";
            }
            out += circular ? circlePath(w / 2.0, h / 2.0, qMax(0.0, radius - bw / 2.0)) + "S\n"
                            : rectOperator(bw / 2.0, bw / 2.0, qMax(0.0, w - bw), qMax(0.0, h - bw)) + "S\n";
        }
        if (!circular && (style == QLatin1String("beveled") || style == QLatin1String("inset"))) {
            const QColor light = style == QLatin1String("beveled") ? QColor(Qt::white) : QColor(128, 128, 128);
            const QColor dark = style == QLatin1String("beveled") ? QColor(128, 128, 128) : QColor(Qt::white);
            out += bevelPath(w, h, bw, light, dark);
        }
    }
    out += "Q\n";
    return out;
}

/** The value, between the markers a reader is allowed to replace. */
std::string drawValue(const FormBuilder::Field &field, double w, double h, const QString &text, const QString &baseFont)
{
    const double pad = paddingOf(field);
    const double size = effectiveSize(field, w, h, text, baseFont);

    std::string out = "/Tx BMC\nq\n";
    out += rectOperator(pad, pad, qMax(0.0, w - 2 * pad), qMax(0.0, h - 2 * pad)) + "W n\n";
    if (!text.isEmpty()) {
        out += "BT\n/" + field.fontName.toStdString() + " " + number(size) + " Tf\n";
        out += setFill(field.textColour.isValid() ? field.textColour : QColor(Qt::black));

        QStringList lines;
        if (field.multiline) {
            lines = text.split(QLatin1Char('\n'));
        } else {
            lines << text;
        }

        double y = field.multiline ? h - pad - size : (h - size) / 2.0 + size * 0.22;
        for (const QString &line : std::as_const(lines)) {
            if (y < -size) {
                break;
            }
            double x = pad;
            if (field.comb && field.maxLength > 0) {
                // A comb field draws one character per cell, which is what makes
                // a row of little boxes for a postcode line up with the letters.
                const double cell = w / double(field.maxLength);
                for (int i = 0; i < line.size() && i < field.maxLength; ++i) {
                    const QString one = line.mid(i, 1);
                    const double centre = cell * (double(i) + 0.5) - textWidth(one, baseFont, size) / 2.0;
                    out += "1 0 0 1 " + number(centre) + " " + number(y) + " Tm\n" + literalString(one) + " Tj\n";
                }
                y -= size * 1.16;
                continue;
            }
            const double drawn = textWidth(line, baseFont, size);
            if (field.alignment.testFlag(Qt::AlignHCenter)) {
                x = (w - drawn) / 2.0;
            } else if (field.alignment.testFlag(Qt::AlignRight)) {
                x = w - pad - drawn;
            }
            out += "1 0 0 1 " + number(x) + " " + number(y) + " Tm\n" + literalString(line) + " Tj\n";
            y -= size * 1.16;
        }
        out += "ET\n";
    }
    out += "Q\nEMC\n";
    return out;
}

/** The tick, drawn rather than left to the reader, because half of them draw none. */
std::string drawTick(const FormBuilder::Field &field, double w, double h)
{
    const double size = field.fontSize > 0.0 ? field.fontSize : qMin(w, h) * 0.8;
    const double glyphWidth = textWidth(QStringLiteral("4"), QStringLiteral("ZapfDingbats"), size);
    std::string out = "q\nBT\n/ZaDb " + number(size) + " Tf\n";
    out += setFill(field.textColour.isValid() ? field.textColour : QColor(Qt::black));
    out += "1 0 0 1 " + number((w - glyphWidth) / 2.0) + " " + number((h - size * 0.72) / 2.0) + " Tm\n";
    // Character 4 of ZapfDingbats is the tick mark, which is why /MK /CA says (4).
    out += "(4) Tj\nET\nQ\n";
    return out;
}

std::string drawDot(const FormBuilder::Field &field, double w, double h)
{
    std::string out = "q\n";
    out += setFill(field.textColour.isValid() ? field.textColour : QColor(Qt::black));
    out += circlePath(w / 2.0, h / 2.0, qMin(w, h) * 0.22) + "f\nQ\n";
    return out;
}

/** The caption of a push button, centred the only way that works: by measuring. */
std::string drawCaption(const FormBuilder::Field &field, double w, double h, const QString &text,
                        const QString &baseFont)
{
    if (text.isEmpty()) {
        return {};
    }
    const double size = effectiveSize(field, w, h, text, baseFont);
    const double drawn = textWidth(text, baseFont, size);
    std::string out = "q\nBT\n/" + field.fontName.toStdString() + " " + number(size) + " Tf\n";
    out += setFill(field.textColour.isValid() ? field.textColour : QColor(Qt::black));
    out += "1 0 0 1 " + number((w - drawn) / 2.0) + " " + number((h - size * 0.72) / 2.0) + " Tm\n";
    out += literalString(text) + " Tj\nET\nQ\n";
    return out;
}

QPDFObjectHandle makeForm(Build &build, const std::string &content, double w, double h, int rotate,
                          const QStringList &fonts)
{
    QPDFObjectHandle stream = QPDFObjectHandle::newStream(build.pdf, content);
    QPDFObjectHandle dict = QPDFObjectHandle::parse("<< /Type /XObject /Subtype /Form >>");
    dict.replaceKey("/BBox", QPDFObjectHandle::parse("[0 0 " + number(w) + " " + number(h) + "]"));
    if (rotate != 0) {
        // The appearance is drawn the way the page is displayed; /Matrix turns it
        // into page space, so the same stream is right on a page that is turned.
        const QTransform turn = PdfGeometry::displayToPageTransform(rotate, 0.0, 0.0);
        dict.replaceKey("/Matrix",
                        QPDFObjectHandle::parse("[" + number(turn.m11()) + " " + number(turn.m12()) + " "
                                                + number(turn.m21()) + " " + number(turn.m22()) + " 0 0]"));
    }
    QPDFObjectHandle resources = QPDFObjectHandle::parse("<< /ProcSet [/PDF /Text] >>");
    if (!fonts.isEmpty()) {
        QPDFObjectHandle fontDict = QPDFObjectHandle::newDictionary();
        for (const QString &name : fonts) {
            fontDict.replaceKey(QString(QLatin1Char('/') + name).toStdString(), fontFor(build, name));
        }
        resources.replaceKey("/Font", fontDict);
    }
    dict.replaceKey("/Resources", resources);
    stream.replaceDict(dict);
    return build.pdf->makeIndirectObject(stream);
}

int flagsFor(const FormBuilder::Field &spec)
{
    int flags = 0;
    if (spec.readOnly) {
        flags |= flagReadOnly;
    }
    if (spec.required) {
        flags |= flagRequired;
    }
    switch (spec.kind) {
    case FormBuilder::Field::Kind::Text:
        if (spec.multiline) {
            flags |= flagMultiline;
        }
        if (spec.password) {
            flags |= flagPassword;
        }
        // A comb field without a character count has nothing to divide the box
        // into, so the flag is dropped rather than written and ignored.
        if (spec.comb && spec.maxLength > 0 && !spec.multiline) {
            flags |= flagComb;
        }
        break;
    case FormBuilder::Field::Kind::Dropdown:
        flags |= flagCombo;
        if (spec.editable) {
            flags |= flagEdit;
        }
        break;
    case FormBuilder::Field::Kind::ListBox:
        if (spec.multiSelect) {
            flags |= flagMultiSelect;
        }
        break;
    case FormBuilder::Field::Kind::Radio:
        flags |= flagRadio;
        break;
    case FormBuilder::Field::Kind::PushButton:
        flags |= flagPushButton;
        break;
    case FormBuilder::Field::Kind::Checkbox:
    case FormBuilder::Field::Kind::Signature:
        break;
    }
    return flags;
}

std::string fieldTypeFor(FormBuilder::Field::Kind kind)
{
    switch (kind) {
    case FormBuilder::Field::Kind::Text:
        return "/Tx";
    case FormBuilder::Field::Kind::Dropdown:
    case FormBuilder::Field::Kind::ListBox:
        return "/Ch";
    case FormBuilder::Field::Kind::Signature:
        return "/Sig";
    case FormBuilder::Field::Kind::Checkbox:
    case FormBuilder::Field::Kind::Radio:
    case FormBuilder::Field::Kind::PushButton:
        break;
    }
    return "/Btn";
}

int quaddingFor(Qt::Alignment alignment)
{
    if (alignment.testFlag(Qt::AlignHCenter)) {
        return 1;
    }
    return alignment.testFlag(Qt::AlignRight) ? 2 : 0;
}

std::string defaultAppearanceFor(const FormBuilder::Field &spec)
{
    const QColor colour = spec.textColour.isValid() ? spec.textColour : QColor(Qt::black);
    std::string da = "/" + spec.fontName.toStdString() + " " + number(spec.fontSize) + " Tf ";
    if (colour == QColor(Qt::black)) {
        da += "0 g";
    } else {
        da += number(colour.redF()) + " " + number(colour.greenF()) + " " + number(colour.blueF()) + " rg";
    }
    return da;
}

/** Everything that belongs on the field dictionary rather than on a widget. */
void configureField(Build &build, QPDFObjectHandle field, const FormBuilder::Field &spec)
{
    field.replaceKey("/FT", QPDFObjectHandle::newName(fieldTypeFor(spec.kind)));
    field.replaceKey("/Ff", QPDFObjectHandle::newInteger(flagsFor(spec)));

    if (spec.label.isEmpty()) {
        field.removeKey("/TU");
    } else {
        field.replaceKey("/TU", unicodeString(spec.label));
    }

    const bool textual = spec.kind == FormBuilder::Field::Kind::Text || spec.kind == FormBuilder::Field::Kind::Dropdown
        || spec.kind == FormBuilder::Field::Kind::ListBox;
    if (textual) {
        field.replaceKey("/DA", QPDFObjectHandle::newString(defaultAppearanceFor(spec)));
        field.replaceKey("/Q", QPDFObjectHandle::newInteger(quaddingFor(spec.alignment)));
    }
    if (spec.kind == FormBuilder::Field::Kind::Text && spec.maxLength > 0) {
        field.replaceKey("/MaxLen", QPDFObjectHandle::newInteger(spec.maxLength));
    } else {
        field.removeKey("/MaxLen");
    }

    if (spec.kind == FormBuilder::Field::Kind::Dropdown || spec.kind == FormBuilder::Field::Kind::ListBox) {
        QPDFObjectHandle options = QPDFObjectHandle::newArray();
        for (qsizetype i = 0; i < spec.options.size(); ++i) {
            if (i < spec.exportValues.size() && spec.exportValues.at(i) != spec.options.at(i)) {
                // The pair form: what gets stored, then what is shown. Only used
                // when the two really differ, because a reader asked for the
                // choices of a field written this way has more work to do.
                QPDFObjectHandle pair = QPDFObjectHandle::newArray();
                pair.appendItem(unicodeString(spec.exportValues.at(i)));
                pair.appendItem(unicodeString(spec.options.at(i)));
                options.appendItem(pair);
            } else {
                options.appendItem(unicodeString(spec.options.at(i)));
            }
        }
        if (options.getArrayNItems() > 0) {
            field.replaceKey("/Opt", options);
        }
    }

    if (spec.kind == FormBuilder::Field::Kind::PushButton || spec.kind == FormBuilder::Field::Kind::Signature) {
        return; // Neither holds a value.
    }

    if (spec.kind == FormBuilder::Field::Kind::Checkbox || spec.kind == FormBuilder::Field::Kind::Radio) {
        const QString state = normalisedState(spec.onState);
        const bool on = spec.kind == FormBuilder::Field::Kind::Checkbox
            && (spec.defaultValue.compare(QStringLiteral("on"), Qt::CaseInsensitive) == 0
                || spec.defaultValue.compare(QStringLiteral("yes"), Qt::CaseInsensitive) == 0
                || spec.defaultValue == state || spec.defaultValue == state.mid(1));
        const std::string value = (on ? state : QStringLiteral("/Off")).toStdString();
        if (!field.getKey("/V").isName() || spec.kind == FormBuilder::Field::Kind::Checkbox) {
            field.replaceKey("/V", QPDFObjectHandle::newName(value));
            field.replaceKey("/DV", QPDFObjectHandle::newName(value));
        }
        return;
    }

    if (spec.defaultValue.isEmpty()) {
        field.removeKey("/V");
        field.removeKey("/DV");
    } else {
        field.replaceKey("/V", unicodeString(spec.defaultValue));
        field.replaceKey("/DV", unicodeString(spec.defaultValue));
    }
    Q_UNUSED(build)
}

/** Everything that belongs on the widget: where it is and what it looks like. */
void configureWidget(Build &build, QPDFObjectHandle widget, const FormBuilder::Field &spec, const QRectF &pageRect,
                     int rotate, const QString &onState)
{
    widget.replaceKey("/Type", QPDFObjectHandle::newName("/Annot"));
    widget.replaceKey("/Subtype", QPDFObjectHandle::newName("/Widget"));
    widget.replaceKey("/Rect", QPDFObjectHandle::parse(rectArray(pageRect)));
    // Bit 3 is Print. Without it the field is on screen only, which is not what
    // anybody means by putting a form on a document.
    widget.replaceKey("/F", QPDFObjectHandle::newInteger(4));

    QPDFObjectHandle appearanceCharacteristics = QPDFObjectHandle::newDictionary();
    if (spec.backgroundColour.isValid()) {
        appearanceCharacteristics.replaceKey("/BG", QPDFObjectHandle::parse(colourArray(spec.backgroundColour)));
    }
    if (spec.borderColour.isValid()) {
        appearanceCharacteristics.replaceKey("/BC", QPDFObjectHandle::parse(colourArray(spec.borderColour)));
    }
    if (spec.kind == FormBuilder::Field::Kind::Checkbox) {
        appearanceCharacteristics.replaceKey("/CA", QPDFObjectHandle::newString("4"));
    } else if (spec.kind == FormBuilder::Field::Kind::Radio) {
        appearanceCharacteristics.replaceKey("/CA", QPDFObjectHandle::newString("l"));
    } else if (spec.kind == FormBuilder::Field::Kind::PushButton) {
        appearanceCharacteristics.replaceKey("/CA", unicodeString(spec.label.isEmpty() ? spec.name : spec.label));
    }
    if (rotate != 0) {
        appearanceCharacteristics.replaceKey("/R", QPDFObjectHandle::newInteger(rotate));
    }
    widget.replaceKey("/MK", appearanceCharacteristics);

    const QString style = spec.borderStyle.trimmed().toLower();
    std::string borderStyleName = "/S";
    if (style == QLatin1String("dashed")) {
        borderStyleName = "/D";
    } else if (style == QLatin1String("beveled")) {
        borderStyleName = "/B";
    } else if (style == QLatin1String("inset")) {
        borderStyleName = "/I";
    } else if (style == QLatin1String("underline")) {
        borderStyleName = "/U";
    }
    QPDFObjectHandle borderStyle = QPDFObjectHandle::parse("<< /Type /Border >>");
    borderStyle.replaceKey("/W", QPDFObjectHandle::newReal(qMax(0.0, spec.borderWidth), 3));
    borderStyle.replaceKey("/S", QPDFObjectHandle::newName(borderStyleName));
    if (borderStyleName == "/D") {
        borderStyle.replaceKey("/D",
                               QPDFObjectHandle::parse("[" + number(qMax(1.0, spec.borderWidth * 3.0)) + " "
                                                       + number(qMax(1.0, spec.borderWidth * 2.0)) + "]"));
    }
    widget.replaceKey("/BS", borderStyle);

    const double w = spec.rect.width();
    const double h = spec.rect.height();
    const QString baseFont = baseFontFor(spec.fontName);

    switch (spec.kind) {
    case FormBuilder::Field::Kind::Checkbox:
    case FormBuilder::Field::Kind::Radio: {
        const bool circular = spec.kind == FormBuilder::Field::Kind::Radio;
        const std::string frame = drawFrame(spec, w, h, circular);
        const std::string mark = circular ? drawDot(spec, w, h) : drawTick(spec, w, h);
        QStringList fonts;
        if (!circular) {
            fonts << QStringLiteral("ZaDb");
        }
        QPDFObjectHandle normal = QPDFObjectHandle::newDictionary();
        // Two states, named exactly as /V and /AS will name them. A box whose
        // appearance calls its on-state /Yes while the value says /On holds a
        // value and shows nothing, which reads as a broken reader.
        normal.replaceKey(onState.toStdString(), makeForm(build, frame + mark, w, h, rotate, fonts));
        normal.replaceKey("/Off", makeForm(build, frame, w, h, rotate, {}));
        QPDFObjectHandle appearance = QPDFObjectHandle::newDictionary();
        appearance.replaceKey("/N", normal);
        widget.replaceKey("/AP", appearance);

        QPDFObjectHandle value = widget.getKey("/V");
        if (!value.isName()) {
            QPDFObjectHandle parent = widget.getKey("/Parent");
            value = parent.isDictionary() ? parent.getKey("/V") : QPDFObjectHandle::newNull();
        }
        const bool on = value.isName() && QString::fromStdString(value.getName()) == onState;
        widget.replaceKey("/AS", QPDFObjectHandle::newName(on ? onState.toStdString() : "/Off"));
        break;
    }
    case FormBuilder::Field::Kind::PushButton: {
        const std::string content = drawFrame(spec, w, h, false)
            + drawCaption(spec, w, h, spec.label.isEmpty() ? spec.name : spec.label, baseFont);
        QPDFObjectHandle appearance = QPDFObjectHandle::newDictionary();
        appearance.replaceKey("/N", makeForm(build, content, w, h, rotate, { spec.fontName }));
        widget.replaceKey("/AP", appearance);
        break;
    }
    case FormBuilder::Field::Kind::Signature: {
        QPDFObjectHandle appearance = QPDFObjectHandle::newDictionary();
        appearance.replaceKey("/N", makeForm(build, drawFrame(spec, w, h, false), w, h, rotate, {}));
        widget.replaceKey("/AP", appearance);
        break;
    }
    case FormBuilder::Field::Kind::Text:
    case FormBuilder::Field::Kind::Dropdown:
    case FormBuilder::Field::Kind::ListBox: {
        const std::string content = drawFrame(spec, w, h, false) + drawValue(spec, w, h, spec.defaultValue, baseFont);
        QPDFObjectHandle appearance = QPDFObjectHandle::newDictionary();
        appearance.replaceKey("/N", makeForm(build, content, w, h, rotate, { spec.fontName }));
        widget.replaceKey("/AP", appearance);
        break;
    }
    }
}

void addToPage(Build &build, int page, QPDFObjectHandle widget)
{
    QPDFPageObjectHelper &target = build.pages[size_t(page)];
    widget.replaceKey("/P", target.getObjectHandle());
    QPDFObjectHandle annots = target.getObjectHandle().getKey("/Annots");
    if (!annots.isArray()) {
        annots = QPDFObjectHandle::newArray();
        target.getObjectHandle().replaceKey("/Annots", annots);
    }
    annots.appendItem(widget);
}

/** Where a dotted name says a field belongs, making the groups it names. */
struct Slot {
    QPDFObjectHandle container;
    QPDFObjectHandle parent;
    QString partial;
};

bool slotFor(Build &build, const QString &fullName, Slot *slot, QString *error)
{
    const QStringList parts = fullName.split(QLatin1Char('.'), Qt::SkipEmptyParts);
    if (parts.isEmpty()) {
        if (error) {
            *error = i18n("A field needs a name.");
        }
        return false;
    }

    QPDFObjectHandle container = build.fieldsArray;
    QPDFObjectHandle parent = QPDFObjectHandle::newNull();
    for (qsizetype i = 0; i + 1 < parts.size(); ++i) {
        QPDFObjectHandle found = QPDFObjectHandle::newNull();
        for (int k = 0; k < container.getArrayNItems(); ++k) {
            QPDFObjectHandle item = container.getArrayItem(k);
            if (item.isDictionary() && item.getKey("/T").isString()
                && QString::fromStdString(item.getKey("/T").getUTF8Value()) == parts.at(i)) {
                found = item;
                break;
            }
        }
        if (found.isDictionary() && !found.getKey("/Kids").isArray()) {
            if (error) {
                *error = i18n("“%1” is already a field of its own and cannot also hold other fields.",
                              parts.mid(0, int(i) + 1).join(QLatin1Char('.')));
            }
            return false;
        }
        if (!found.isDictionary()) {
            found = QPDFObjectHandle::newDictionary();
            found.replaceKey("/T", unicodeString(parts.at(i)));
            found.replaceKey("/Kids", QPDFObjectHandle::newArray());
            found = build.pdf->makeIndirectObject(found);
            if (parent.isDictionary()) {
                found.replaceKey("/Parent", parent);
            }
            container.appendItem(found);
        }
        parent = found;
        container = found.getKey("/Kids");
    }

    slot->container = container;
    slot->parent = parent;
    slot->partial = parts.constLast();
    return true;
}

bool prepare(Build &build, QPDF &pdf, QString *error)
{
    if (!checkNoXfa(pdf, error)) {
        return false;
    }
    build.pdf = &pdf;
    build.pages = QPDFPageDocumentHelper(pdf).getAllPages();

    QPDFObjectHandle root = pdf.getRoot();
    if (!root.getKey("/AcroForm").isDictionary()) {
        root.replaceKey("/AcroForm", pdf.makeIndirectObject(QPDFObjectHandle::newDictionary()));
    }
    build.acroForm = root.getKey("/AcroForm");
    if (!build.acroForm.getKey("/Fields").isArray()) {
        build.acroForm.replaceKey("/Fields", QPDFObjectHandle::newArray());
    }
    build.fieldsArray = build.acroForm.getKey("/Fields");
    if (!build.acroForm.getKey("/DR").isDictionary()) {
        build.acroForm.replaceKey("/DR", QPDFObjectHandle::newDictionary());
    }
    QPDFObjectHandle resources = build.acroForm.getKey("/DR");
    if (!resources.getKey("/Font").isDictionary()) {
        resources.replaceKey("/Font", QPDFObjectHandle::newDictionary());
    }
    build.drFonts = resources.getKey("/Font");
    if (!build.acroForm.getKey("/DA").isString()) {
        build.acroForm.replaceKey("/DA", QPDFObjectHandle::newString("/Helv 0 Tf 0 g"));
    }
    // The document-wide /DA names Helv, so /DR must be able to produce it.
    fontFor(build, QStringLiteral("Helv"));
    return true;
}

/**
 * Draws what is missing, then asks the reader to draw it again anyway.
 *
 * Both halves earn their place: generating now means a reader that has no
 * appearance generator still shows the form, and leaving /NeedAppearances set
 * means a reader that has one is free to lay the text out better than we can.
 */
void settleAppearances(QPDF &pdf)
{
    QPDFAcroFormDocumentHelper forms(pdf);
    forms.setNeedAppearances(true);
    forms.generateAppearancesIfNeeded();
    forms.setNeedAppearances(true);
}

// ---------------------------------------------------------------------------
// Data files
// ---------------------------------------------------------------------------

QString csvCell(const QString &value)
{
    QString cell = value;
    if (cell.contains(QLatin1Char('"')) || cell.contains(QLatin1Char(',')) || cell.contains(QLatin1Char('\n'))
        || cell.contains(QLatin1Char('\r'))) {
        cell.replace(QLatin1Char('"'), QStringLiteral("\"\""));
        return QLatin1Char('"') + cell + QLatin1Char('"');
    }
    return cell;
}

QVector<QStringList> parseCsv(const QString &text)
{
    QVector<QStringList> rows;
    QStringList row;
    QString cell;
    bool quoted = false;
    for (qsizetype i = 0; i < text.size(); ++i) {
        const QChar c = text.at(i);
        if (quoted) {
            if (c == QLatin1Char('"')) {
                if (i + 1 < text.size() && text.at(i + 1) == QLatin1Char('"')) {
                    cell.append(QLatin1Char('"'));
                    ++i;
                } else {
                    quoted = false;
                }
            } else {
                cell.append(c);
            }
            continue;
        }
        if (c == QLatin1Char('"')) {
            quoted = true;
        } else if (c == QLatin1Char(',')) {
            row << cell;
            cell.clear();
        } else if (c == QLatin1Char('\n') || c == QLatin1Char('\r')) {
            if (c == QLatin1Char('\r') && i + 1 < text.size() && text.at(i + 1) == QLatin1Char('\n')) {
                ++i;
            }
            row << cell;
            cell.clear();
            rows.append(row);
            row.clear();
        } else {
            cell.append(c);
        }
    }
    if (!cell.isEmpty() || !row.isEmpty()) {
        row << cell;
        rows.append(row);
    }
    return rows;
}

bool writeTextFile(const QString &path, const QByteArray &bytes, QString *error)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error) {
            *error = i18n("“%1” cannot be written to.", path);
        }
        return false;
    }
    file.write(bytes);
    if (!file.commit()) {
        if (error) {
            *error = i18n("“%1” cannot be written to.", path);
        }
        return false;
    }
    return true;
}

/** A field value as a PDF string: hexadecimal UTF-16 whenever Latin-1 will not do. */
std::string pdfTextLiteral(const QString &text)
{
    bool plain = true;
    for (const QChar &character : text) {
        if (character.unicode() > 0x7E || character.unicode() < 0x20) {
            plain = false;
            break;
        }
    }
    if (plain) {
        return literalString(text);
    }
    std::string out = "<FEFF";
    for (const QChar &character : text) {
        out += QByteArray::number(character.unicode(), 16).rightJustified(4, '0').toUpper().toStdString();
    }
    return out + ">";
}

QString decodePdfText(const QByteArray &raw)
{
    if (raw.size() >= 2 && uchar(raw.at(0)) == 0xFE && uchar(raw.at(1)) == 0xFF) {
        QString out;
        for (qsizetype i = 2; i + 1 < raw.size(); i += 2) {
            out.append(QChar(ushort((uchar(raw.at(i)) << 8) | uchar(raw.at(i + 1)))));
        }
        return out;
    }
    return QString::fromLatin1(raw);
}

/**
 * The /T and /V pairs of a flat FDF, read without pretending to be a PDF parser.
 *
 * An FDF written by anything else may nest fields under /Kids, carry an
 * appearance or hold a whole page of its own; this reads the case that matters
 * (a list of names and values) and says so in the limitations rather than
 * failing halfway through something more elaborate.
 */
QHash<QString, QString> parseFdf(const QByteArray &bytes)
{
    QHash<QString, QString> values;
    QString pendingName;

    auto readObject = [&bytes](qsizetype &at, bool *isName) -> QString {
        *isName = false;
        while (at < bytes.size() && QChar::isSpace(uchar(bytes.at(at)))) {
            ++at;
        }
        if (at >= bytes.size()) {
            return {};
        }
        if (bytes.at(at) == '(') {
            ++at;
            QByteArray raw;
            int depth = 1;
            while (at < bytes.size()) {
                const char c = bytes.at(at++);
                if (c == '\\' && at < bytes.size()) {
                    const char escaped = bytes.at(at++);
                    switch (escaped) {
                    case 'n':
                        raw.append('\n');
                        break;
                    case 'r':
                        raw.append('\r');
                        break;
                    case 't':
                        raw.append('\t');
                        break;
                    default:
                        raw.append(escaped);
                        break;
                    }
                    continue;
                }
                if (c == '(') {
                    ++depth;
                } else if (c == ')') {
                    if (--depth == 0) {
                        break;
                    }
                }
                raw.append(c);
            }
            return decodePdfText(raw);
        }
        if (bytes.at(at) == '<' && at + 1 < bytes.size() && bytes.at(at + 1) != '<') {
            ++at;
            QByteArray hex;
            while (at < bytes.size() && bytes.at(at) != '>') {
                hex.append(bytes.at(at++));
            }
            ++at;
            return decodePdfText(QByteArray::fromHex(hex));
        }
        if (bytes.at(at) == '/') {
            *isName = true;
            QByteArray name;
            name.append(bytes.at(at++));
            while (at < bytes.size() && !QChar::isSpace(uchar(bytes.at(at))) && bytes.at(at) != '/'
                   && bytes.at(at) != '>' && bytes.at(at) != '[' && bytes.at(at) != ']' && bytes.at(at) != '(') {
                name.append(bytes.at(at++));
            }
            return QString::fromLatin1(name);
        }
        return {};
    };

    for (qsizetype i = 0; i + 1 < bytes.size(); ++i) {
        if (bytes.at(i) != '/') {
            continue;
        }
        const bool isT = bytes.at(i + 1) == 'T' && (i + 2 >= bytes.size() || !std::isalpha(uchar(bytes.at(i + 2))));
        const bool isV = bytes.at(i + 1) == 'V' && (i + 2 >= bytes.size() || !std::isalpha(uchar(bytes.at(i + 2))));
        if (!isT && !isV) {
            continue;
        }
        qsizetype at = i + 2;
        bool isName = false;
        const QString read = readObject(at, &isName);
        i = at - 1;
        if (isT) {
            pendingName = read;
        } else if (!pendingName.isEmpty()) {
            values.insert(pendingName, read);
            pendingName.clear();
        }
    }
    return values;
}

QHash<QString, QString> parseXfdf(const QByteArray &bytes)
{
    QHash<QString, QString> values;
    QXmlStreamReader reader(bytes);
    QStringList path;
    while (!reader.atEnd()) {
        reader.readNext();
        if (reader.isStartElement()) {
            if (reader.name() == QLatin1String("field")) {
                path << reader.attributes().value(QLatin1String("name")).toString();
            } else if (reader.name() == QLatin1String("value") && !path.isEmpty()) {
                values.insert(path.join(QLatin1Char('.')), reader.readElementText());
            }
        } else if (reader.isEndElement() && reader.name() == QLatin1String("field") && !path.isEmpty()) {
            path.removeLast();
        }
    }
    return values;
}

/** The fields of a document as one row, with radio groups counted once. */
QVector<FormField> uniqueFields(const QString &pdf, QString *error)
{
    const QVector<FormField> raw = Forms::read(pdf, error);
    QVector<FormField> unique;
    for (const FormField &field : raw) {
        bool merged = false;
        for (FormField &kept : unique) {
            if (kept.name != field.name) {
                continue;
            }
            merged = true;
            // A radio group comes back once per button; the one holding the
            // answer is the one worth keeping.
            if (kept.value.isEmpty() || kept.value == QLatin1String("/Off")) {
                kept.value = field.value;
            }
            kept.options += field.options;
            kept.options.removeDuplicates();
            break;
        }
        if (!merged) {
            unique.append(field);
        }
    }
    return unique;
}

QString actionScript(FormBuilder::Format format, int decimals, const QString &currencySymbol, bool keystroke)
{
    const QString count = QString::number(qBound(0, decimals, 6));
    switch (format) {
    case FormBuilder::Format::Number:
        return (keystroke ? QStringLiteral("AFNumber_Keystroke(") : QStringLiteral("AFNumber_Format(")) + count
            + QStringLiteral(", 0, 0, 0, \"\", true)");
    case FormBuilder::Format::Currency:
        return (keystroke ? QStringLiteral("AFNumber_Keystroke(") : QStringLiteral("AFNumber_Format(")) + count
            + QStringLiteral(", 0, 0, 0, \"") + currencySymbol + QStringLiteral(" \", true)");
    case FormBuilder::Format::Percent:
        return (keystroke ? QStringLiteral("AFPercent_Keystroke(") : QStringLiteral("AFPercent_Format(")) + count
            + QStringLiteral(", 0)");
    case FormBuilder::Format::Date:
        return keystroke ? QStringLiteral("AFDate_KeystrokeEx(\"yyyy-mm-dd\")")
                         : QStringLiteral("AFDate_FormatEx(\"yyyy-mm-dd\")");
    case FormBuilder::Format::Time:
        return keystroke ? QStringLiteral("AFTime_Keystroke(0)") : QStringLiteral("AFTime_Format(0)");
    case FormBuilder::Format::ZipCode:
        return keystroke ? QStringLiteral("AFSpecial_Keystroke(0)") : QStringLiteral("AFSpecial_Format(0)");
    case FormBuilder::Format::Phone:
        return keystroke ? QStringLiteral("AFSpecial_Keystroke(2)") : QStringLiteral("AFSpecial_Format(2)");
    case FormBuilder::Format::None:
        break;
    }
    return {};
}

void addScriptWarnings(QStringList *warnings)
{
    if (!warnings) {
        return;
    }
    *warnings << i18n("This is a JavaScript action, which is how every PDF form does it and which PDF/A "
                      "forbids outright. A document that has to be archived cannot carry it.");
    *warnings << i18n("Readers other than Acrobat vary in how much of this they run, and several run none of "
                      "it. Treat the behaviour as a convenience, and never as a guarantee that what arrives "
                      "back is valid.");
}

QPDFObjectHandle javaScriptAction(const QString &script)
{
    QPDFObjectHandle action = QPDFObjectHandle::newDictionary();
    action.replaceKey("/S", QPDFObjectHandle::newName("/JavaScript"));
    action.replaceKey("/JS", QPDFObjectHandle::newString(script.toStdString()));
    return action;
}

QPDFObjectHandle additionalActions(QPDFObjectHandle field)
{
    if (!field.getKey("/AA").isDictionary()) {
        field.replaceKey("/AA", QPDFObjectHandle::newDictionary());
    }
    return field.getKey("/AA");
}

using FieldAction = std::function<bool(QPDF &pdf, QPDFObjectHandle acroForm, QPDFObjectHandle field, QString *error)>;

/** Opens, finds one field by its fully qualified name, changes it, writes. */
bool editField(const QString &in, const QString &out, const QString &fieldName, const FieldAction &action,
               QString *error)
{
    try {
        QPDF pdf;
        PdfFile::open(pdf, in);
        if (!checkNoXfa(pdf, error)) {
            return false;
        }
        QPDFObjectHandle acroForm = pdf.getRoot().getKey("/AcroForm");
        if (!acroForm.isDictionary()) {
            if (error) {
                *error = i18n("This document has no form.");
            }
            return false;
        }
        const QVector<Located> fields = allFields(acroForm);
        const Located *found = findField(fields, fieldName);
        if (!found) {
            if (error) {
                *error = i18n("This document has no field called “%1”.", fieldName);
            }
            return false;
        }
        if (!action(pdf, acroForm, found->field, error)) {
            return false;
        }
        writeDocument(pdf, out);
    } catch (const std::exception &e) {
        if (error) {
            *error = QString::fromUtf8(e.what());
        }
        return false;
    }
    return true;
}

} // namespace

bool FormBuilder::addFields(const QString &in, const QString &out, const QVector<Field> &fields, int *added,
                            QString *error)
{
    if (fields.isEmpty()) {
        if (error) {
            *error = i18n("There are no fields to add.");
        }
        return false;
    }

    int count = 0;
    try {
        QPDF pdf;
        PdfFile::open(pdf, in);

        Build build;
        if (!prepare(build, pdf, error)) {
            return false;
        }

        QHash<QString, QPDFObjectHandle> radioGroups;
        bool wantsSignature = false;

        for (const Field &spec : fields) {
            const QString name
                = spec.kind == Field::Kind::Radio && !spec.radioGroup.isEmpty() ? spec.radioGroup : spec.name;
            if (name.isEmpty()) {
                if (error) {
                    *error = i18n("A field needs a name.");
                }
                return false;
            }
            if (spec.page < 0 || size_t(spec.page) >= build.pages.size()) {
                if (error) {
                    *error = i18n("“%1” asks for page %2, which this document does not have.", name, spec.page + 1);
                }
                return false;
            }
            if (spec.rect.width() <= 0.0 || spec.rect.height() <= 0.0) {
                if (error) {
                    *error = i18n("“%1” has no size, so nothing could be drawn for it.", name);
                }
                return false;
            }

            QPDFPageObjectHelper &page = build.pages[size_t(spec.page)];
            const int rotate = PdfGeometry::rotationOf(page);
            const QRectF pageRect = pageTransformFor(page).mapRect(spec.rect.normalized());
            const QString onState = normalisedState(spec.onState);

            if (spec.kind == Field::Kind::Radio) {
                // One field, several widgets. Three fields with one widget each
                // gives three buttons that toggle as one, which is the classic
                // way of getting a radio group wrong.
                QPDFObjectHandle group = radioGroups.value(name);
                if (!group.isInitialized() || !group.isDictionary()) {
                    Slot slot;
                    if (!slotFor(build, name, &slot, error)) {
                        return false;
                    }
                    if (findField(allFields(build.acroForm), name)) {
                        if (error) {
                            *error = i18n("This document already has a field called “%1”.", name);
                        }
                        return false;
                    }
                    group = QPDFObjectHandle::newDictionary();
                    group.replaceKey("/T", unicodeString(slot.partial));
                    group.replaceKey("/Kids", QPDFObjectHandle::newArray());
                    group = pdf.makeIndirectObject(group);
                    Field groupSpec = spec;
                    groupSpec.name = name;
                    configureField(build, group, groupSpec);
                    if (slot.parent.isDictionary()) {
                        group.replaceKey("/Parent", slot.parent);
                    }
                    slot.container.appendItem(group);
                    radioGroups.insert(name, group);
                }

                QPDFObjectHandle kid = pdf.makeIndirectObject(QPDFObjectHandle::newDictionary());
                kid.replaceKey("/Parent", group);
                configureWidget(build, kid, spec, pageRect, rotate, onState);
                group.getKey("/Kids").appendItem(kid);
                addToPage(build, spec.page, kid);

                // /Opt lets a reader put a label against each button, and is the
                // only place the order of the buttons is written down.
                if (!group.getKey("/Opt").isArray()) {
                    group.replaceKey("/Opt", QPDFObjectHandle::newArray());
                }
                group.getKey("/Opt").appendItem(unicodeString(spec.label.isEmpty() ? spec.name : spec.label));
                ++count;
                continue;
            }

            Slot slot;
            if (!slotFor(build, spec.name, &slot, error)) {
                return false;
            }
            if (findField(allFields(build.acroForm), spec.name)) {
                if (error) {
                    *error = i18n("This document already has a field called “%1”.", spec.name);
                }
                return false;
            }

            // Field and widget in one object. Legal whenever the field has a
            // single widget, and what every real form does.
            QPDFObjectHandle field = pdf.makeIndirectObject(QPDFObjectHandle::newDictionary());
            field.replaceKey("/T", unicodeString(slot.partial));
            configureField(build, field, spec);
            configureWidget(build, field, spec, pageRect, rotate, onState);
            if (slot.parent.isDictionary()) {
                field.replaceKey("/Parent", slot.parent);
            }
            slot.container.appendItem(field);
            addToPage(build, spec.page, field);
            wantsSignature = wantsSignature || spec.kind == Field::Kind::Signature;
            ++count;
        }

        if (wantsSignature) {
            // Bit 1 says the document holds signature fields, bit 2 that saving
            // it any other way than as an incremental update breaks them.
            build.acroForm.replaceKey("/SigFlags", QPDFObjectHandle::newInteger(3));
        }

        settleAppearances(pdf);
        writeDocument(pdf, out);
    } catch (const std::exception &e) {
        if (error) {
            *error = QString::fromUtf8(e.what());
        }
        return false;
    }

    if (added) {
        *added = count;
    }
    return true;
}

bool FormBuilder::updateFields(const QString &in, const QString &out, const QVector<Field> &fields, int *updated,
                               QString *error)
{
    if (fields.isEmpty()) {
        if (error) {
            *error = i18n("There are no fields to change.");
        }
        return false;
    }

    int count = 0;
    try {
        QPDF pdf;
        PdfFile::open(pdf, in);

        Build build;
        if (!prepare(build, pdf, error)) {
            return false;
        }

        std::map<QPDFObjGen, int> pageOfWidget;
        for (size_t i = 0; i < build.pages.size(); ++i) {
            QPDFObjectHandle annots = build.pages[i].getObjectHandle().getKey("/Annots");
            for (int a = 0; annots.isArray() && a < annots.getArrayNItems(); ++a) {
                pageOfWidget[annots.getArrayItem(a).getObjGen()] = int(i);
            }
        }

        const QVector<Located> existing = allFields(build.acroForm);
        for (const Field &spec : fields) {
            const QString name
                = spec.kind == Field::Kind::Radio && !spec.radioGroup.isEmpty() ? spec.radioGroup : spec.name;
            const Located *found = findField(existing, name);
            if (!found) {
                continue;
            }
            QPDFObjectHandle field = found->field;
            configureField(build, field, spec);

            const QVector<QPDFObjectHandle> widgets = widgetsOf(field);
            for (QPDFObjectHandle widget : widgets) {
                const auto onPage = pageOfWidget.find(widget.getObjGen());
                const int pageIndex = onPage == pageOfWidget.end() ? 0 : onPage->second;
                QPDFPageObjectHelper &page = build.pages[size_t(pageIndex)];
                const int rotate = PdfGeometry::rotationOf(page);

                Field applied = spec;
                if (applied.rect.width() <= 0.0 || applied.rect.height() <= 0.0 || widgets.size() > 1) {
                    // Without a size to move to, or with several widgets that
                    // would all pile onto the same box, the field stays where the
                    // form's author put it.
                    bool invertible = false;
                    const QTransform toDisplay = pageTransformFor(page).inverted(&invertible);
                    QPDFObjectHandle rect = widget.getKey("/Rect");
                    const QRectF pageRect
                        = QRectF(QPointF(PdfGeometry::boxValue(rect, 0, 0.0), PdfGeometry::boxValue(rect, 1, 0.0)),
                                 QPointF(PdfGeometry::boxValue(rect, 2, 0.0), PdfGeometry::boxValue(rect, 3, 0.0)))
                              .normalized();
                    applied.rect = invertible ? toDisplay.mapRect(pageRect) : pageRect;
                }
                const QRectF pageRect = pageTransformFor(page).mapRect(applied.rect.normalized());
                configureWidget(build, widget, applied, pageRect, rotate, normalisedState(spec.onState));
            }
            ++count;
        }

        settleAppearances(pdf);
        writeDocument(pdf, out);
    } catch (const std::exception &e) {
        if (error) {
            *error = QString::fromUtf8(e.what());
        }
        return false;
    }

    if (updated) {
        *updated = count;
    }
    return true;
}

bool FormBuilder::removeFields(const QString &in, const QString &out, const QStringList &names, int *removed,
                               QString *error)
{
    if (names.isEmpty()) {
        if (error) {
            *error = i18n("There are no fields to remove.");
        }
        return false;
    }

    int count = 0;
    try {
        QPDF pdf;
        PdfFile::open(pdf, in);
        QPDFObjectHandle acroForm = pdf.getRoot().getKey("/AcroForm");
        if (!acroForm.isDictionary()) {
            if (error) {
                *error = i18n("This document has no form.");
            }
            return false;
        }

        std::set<QPDFObjGen> doomedWidgets;
        std::set<QPDFObjGen> doomedFields;
        for (const QString &name : names) {
            const Located *found = findField(allFields(acroForm), name);
            if (!found) {
                continue;
            }
            for (QPDFObjectHandle widget : widgetsOf(found->field)) {
                doomedWidgets.insert(widget.getObjGen());
            }
            doomedFields.insert(found->field.getObjGen());
            ++count;
        }

        // Out of the pages first, because a widget left in /Annots without a
        // field behind it is what makes a reader offer to repair the document.
        for (QPDFPageObjectHelper &page : QPDFPageDocumentHelper(pdf).getAllPages()) {
            QPDFObjectHandle annots = page.getObjectHandle().getKey("/Annots");
            if (!annots.isArray()) {
                continue;
            }
            QPDFObjectHandle kept = QPDFObjectHandle::newArray();
            for (int a = 0; a < annots.getArrayNItems(); ++a) {
                if (doomedWidgets.count(annots.getArrayItem(a).getObjGen()) == 0) {
                    kept.appendItem(annots.getArrayItem(a));
                }
            }
            if (kept.getArrayNItems() > 0) {
                page.getObjectHandle().replaceKey("/Annots", kept);
            } else {
                page.getObjectHandle().removeKey("/Annots");
            }
        }

        const std::function<void(QPDFObjectHandle)> prune = [&](QPDFObjectHandle container) {
            if (!container.isArray()) {
                return;
            }
            QPDFObjectHandle kept = QPDFObjectHandle::newArray();
            for (int i = 0; i < container.getArrayNItems(); ++i) {
                QPDFObjectHandle item = container.getArrayItem(i);
                if (item.isDictionary() && doomedFields.count(item.getObjGen()) > 0) {
                    continue;
                }
                if (item.isDictionary() && item.getKey("/Kids").isArray()) {
                    prune(item.getKey("/Kids"));
                    if (item.getKey("/Kids").getArrayNItems() == 0) {
                        continue; // An empty group is a field nobody can fill in.
                    }
                }
                kept.appendItem(item);
            }
            container.setArrayFromVector(kept.getArrayAsVector());
        };
        prune(acroForm.getKey("/Fields"));

        QPDFObjectHandle calculated = acroForm.getKey("/CO");
        if (calculated.isArray()) {
            QPDFObjectHandle kept = QPDFObjectHandle::newArray();
            for (int i = 0; i < calculated.getArrayNItems(); ++i) {
                if (doomedFields.count(calculated.getArrayItem(i).getObjGen()) == 0) {
                    kept.appendItem(calculated.getArrayItem(i));
                }
            }
            if (kept.getArrayNItems() > 0) {
                acroForm.replaceKey("/CO", kept);
            } else {
                acroForm.removeKey("/CO");
            }
        }

        if (acroForm.getKey("/Fields").getArrayNItems() == 0) {
            pdf.getRoot().removeKey("/AcroForm");
        }

        writeDocument(pdf, out);
    } catch (const std::exception &e) {
        if (error) {
            *error = QString::fromUtf8(e.what());
        }
        return false;
    }

    if (removed) {
        *removed = count;
    }
    return true;
}

bool FormBuilder::renameField(const QString &in, const QString &out, const QString &from, const QString &to,
                              QString *error)
{
    if (to.trimmed().isEmpty()) {
        if (error) {
            *error = i18n("A field needs a name.");
        }
        return false;
    }

    const QStringList oldParts = from.split(QLatin1Char('.'), Qt::SkipEmptyParts);
    const QStringList newParts = to.split(QLatin1Char('.'), Qt::SkipEmptyParts);
    if (oldParts.size() != newParts.size()
        || oldParts.mid(0, oldParts.size() - 1) != newParts.mid(0, newParts.size() - 1)) {
        if (error) {
            // Moving a field into another group means rewriting whatever
            // JavaScript in the document refers to it, and there is no honest way
            // to promise that; renaming in place is a rename, and this is not.
            *error = i18n("A field can be renamed but not moved into another group, so “%1” cannot become “%2”.", from,
                          to);
        }
        return false;
    }

    return editField(
        in, out, from,
        [&](QPDF &pdf, QPDFObjectHandle acroForm, QPDFObjectHandle field, QString *why) {
            Q_UNUSED(pdf)
            if (findField(allFields(acroForm), to)) {
                if (why) {
                    *why = i18n("This document already has a field called “%1”.", to);
                }
                return false;
            }
            field.replaceKey("/T", unicodeString(newParts.constLast()));
            return true;
        },
        error);
}

bool FormBuilder::setTabOrder(const QString &in, const QString &out, int page, const QStringList &names, QString *error)
{
    try {
        QPDF pdf;
        PdfFile::open(pdf, in);
        QPDFObjectHandle acroForm = pdf.getRoot().getKey("/AcroForm");
        if (!acroForm.isDictionary()) {
            if (error) {
                *error = i18n("This document has no form.");
            }
            return false;
        }
        std::vector<QPDFPageObjectHelper> pages = QPDFPageDocumentHelper(pdf).getAllPages();
        if (page < 0 || size_t(page) >= pages.size()) {
            if (error) {
                *error = i18n("This document has no page %1.", page + 1);
            }
            return false;
        }
        QPDFObjectHandle annots = pages[size_t(page)].getObjectHandle().getKey("/Annots");
        if (!annots.isArray()) {
            if (error) {
                *error = i18n("There is nothing on page %1 to put in order.", page + 1);
            }
            return false;
        }

        const QVector<Located> fields = allFields(acroForm);
        QPDFObjectHandle ordered = QPDFObjectHandle::newArray();
        std::set<QPDFObjGen> placed;
        for (const QString &name : names) {
            const Located *found = findField(fields, name);
            if (!found) {
                if (error) {
                    *error = i18n("This document has no field called “%1”.", name);
                }
                return false;
            }
            for (QPDFObjectHandle widget : widgetsOf(found->field)) {
                for (int a = 0; a < annots.getArrayNItems(); ++a) {
                    if (annots.getArrayItem(a).getObjGen() == widget.getObjGen()
                        && placed.count(widget.getObjGen()) == 0) {
                        ordered.appendItem(widget);
                        placed.insert(widget.getObjGen());
                    }
                }
            }
        }
        for (int a = 0; a < annots.getArrayNItems(); ++a) {
            if (placed.count(annots.getArrayItem(a).getObjGen()) == 0) {
                ordered.appendItem(annots.getArrayItem(a));
            }
        }
        pages[size_t(page)].getObjectHandle().replaceKey("/Annots", ordered);

        // /Tabs /R and /C tell the reader to work the order out from the
        // geometry, which would quietly override everything just done.
        QPDFObjectHandle tabs = pages[size_t(page)].getObjectHandle().getKey("/Tabs");
        if (tabs.isName() && (tabs.getName() == "/R" || tabs.getName() == "/C")) {
            pages[size_t(page)].getObjectHandle().removeKey("/Tabs");
        }

        writeDocument(pdf, out);
    } catch (const std::exception &e) {
        if (error) {
            *error = QString::fromUtf8(e.what());
        }
        return false;
    }
    return true;
}

bool FormBuilder::setFormat(const QString &in, const QString &out, const QString &fieldName, Format format,
                            int decimals, const QString &currencySymbol, QStringList *warnings, QString *error)
{
    const bool ok = editField(
        in, out, fieldName,
        [&](QPDF &pdf, QPDFObjectHandle acroForm, QPDFObjectHandle field, QString *why) {
            Q_UNUSED(pdf)
            Q_UNUSED(acroForm)
            Q_UNUSED(why)
            if (format == Format::None) {
                QPDFObjectHandle actions = field.getKey("/AA");
                if (actions.isDictionary()) {
                    actions.removeKey("/F");
                    actions.removeKey("/K");
                }
                return true;
            }
            QPDFObjectHandle actions = additionalActions(field);
            actions.replaceKey("/F", javaScriptAction(actionScript(format, decimals, currencySymbol, false)));
            // The keystroke action is what stops "12,x4" being
            // typed at all; without it the field only tidies up
            // once focus leaves, which fools nobody.
            actions.replaceKey("/K", javaScriptAction(actionScript(format, decimals, currencySymbol, true)));
            return true;
        },
        error);
    if (ok && format != Format::None) {
        addScriptWarnings(warnings);
    }
    return ok;
}

bool FormBuilder::setValidation(const QString &in, const QString &out, const QString &fieldName, double minimum,
                                double maximum, QStringList *warnings, QString *error)
{
    if (minimum > maximum) {
        if (error) {
            *error = i18n("The smallest value allowed is larger than the largest.");
        }
        return false;
    }
    const QString script = QStringLiteral("AFRange_Validate(true, ") + QString::fromStdString(number(minimum))
        + QStringLiteral(", true, ") + QString::fromStdString(number(maximum)) + QStringLiteral(")");
    const bool ok = editField(
        in, out, fieldName,
        [&](QPDF &pdf, QPDFObjectHandle acroForm, QPDFObjectHandle field, QString *why) {
            Q_UNUSED(pdf)
            Q_UNUSED(acroForm)
            Q_UNUSED(why)
            additionalActions(field).replaceKey("/V", javaScriptAction(script));
            return true;
        },
        error);
    if (ok) {
        addScriptWarnings(warnings);
    }
    return ok;
}

bool FormBuilder::setCalculation(const QString &in, const QString &out, const QString &fieldName, Calculation how,
                                 const QStringList &sourceFields, QStringList *warnings, QString *error)
{
    if (sourceFields.isEmpty()) {
        if (error) {
            *error = i18n("A calculation needs at least one field to work from.");
        }
        return false;
    }

    QString operation = QStringLiteral("SUM");
    switch (how) {
    case Calculation::Product:
        operation = QStringLiteral("PRD");
        break;
    case Calculation::Average:
        operation = QStringLiteral("AVG");
        break;
    case Calculation::Minimum:
        operation = QStringLiteral("MIN");
        break;
    case Calculation::Maximum:
        operation = QStringLiteral("MAX");
        break;
    case Calculation::Sum:
        break;
    }

    QStringList quoted;
    for (const QString &source : sourceFields) {
        QString escaped = source;
        escaped.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
        escaped.replace(QLatin1Char('"'), QStringLiteral("\\\""));
        quoted << QLatin1Char('"') + escaped + QLatin1Char('"');
    }
    const QString script = QStringLiteral("AFSimple_CALC(\"") + operation + QStringLiteral("\", [")
        + quoted.join(QStringLiteral(", ")) + QStringLiteral("])");

    const bool ok = editField(
        in, out, fieldName,
        [&](QPDF &pdf, QPDFObjectHandle acroForm, QPDFObjectHandle field, QString *why) {
            for (const QString &source : sourceFields) {
                if (!findField(allFields(acroForm), source)) {
                    if (why) {
                        *why = i18n("This document has no field called “%1”.", source);
                    }
                    return false;
                }
            }
            additionalActions(field).replaceKey("/C", javaScriptAction(script));

            // /CO is the order the calculations run in, and a
            // field that adds up others has to come after them
            // or it adds up last time's answers. Appending is
            // what gets that right.
            if (!acroForm.getKey("/CO").isArray()) {
                acroForm.replaceKey("/CO", QPDFObjectHandle::newArray());
            }
            QPDFObjectHandle order = acroForm.getKey("/CO");
            QPDFObjectHandle kept = QPDFObjectHandle::newArray();
            for (int i = 0; i < order.getArrayNItems(); ++i) {
                if (order.getArrayItem(i).getObjGen() != field.getObjGen()) {
                    kept.appendItem(order.getArrayItem(i));
                }
            }
            kept.appendItem(field.isIndirect() ? field : pdf.makeIndirectObject(field));
            acroForm.replaceKey("/CO", kept);
            return true;
        },
        error);
    if (ok) {
        addScriptWarnings(warnings);
    }
    return ok;
}

bool FormBuilder::setButtonAction(const QString &in, const QString &out, const QString &fieldName, ButtonAction action,
                                  const QString &target, QString *error)
{
    return editField(
        in, out, fieldName,
        [&](QPDF &pdf, QPDFObjectHandle acroForm, QPDFObjectHandle field, QString *why) {
            Q_UNUSED(acroForm)
            QPDFObjectHandle entry = QPDFObjectHandle::newDictionary();
            entry.replaceKey("/Type", QPDFObjectHandle::newName("/Action"));
            switch (action) {
            case ButtonAction::ResetForm:
                entry.replaceKey("/S", QPDFObjectHandle::newName("/ResetForm"));
                break;
            case ButtonAction::SubmitForm: {
                if (target.isEmpty()) {
                    if (why) {
                        *why = i18n("A button that submits a form needs an address to submit it to.");
                    }
                    return false;
                }
                entry.replaceKey("/S", QPDFObjectHandle::newName("/SubmitForm"));
                QPDFObjectHandle destination = QPDFObjectHandle::newDictionary();
                destination.replaceKey("/FS", QPDFObjectHandle::newName("/URL"));
                destination.replaceKey("/F", QPDFObjectHandle::newString(target.toStdString()));
                entry.replaceKey("/F", destination);
                // No flags: the whole form, as FDF, which is the one
                // shape of submission every reader that submits at all
                // agrees on.
                entry.replaceKey("/Flags", QPDFObjectHandle::newInteger(0));
                break;
            }
            case ButtonAction::GoToPage: {
                bool numeric = false;
                const int wanted = target.toInt(&numeric) - 1;
                std::vector<QPDFPageObjectHelper> pages = QPDFPageDocumentHelper(pdf).getAllPages();
                if (!numeric || wanted < 0 || size_t(wanted) >= pages.size()) {
                    if (why) {
                        *why = i18n("“%1” is not a page of this document.", target);
                    }
                    return false;
                }
                entry.replaceKey("/S", QPDFObjectHandle::newName("/GoTo"));
                QPDFObjectHandle destination = QPDFObjectHandle::newArray();
                destination.appendItem(pages[size_t(wanted)].getObjectHandle());
                destination.appendItem(QPDFObjectHandle::newName("/Fit"));
                entry.replaceKey("/D", destination);
                break;
            }
            case ButtonAction::OpenUrl:
                if (target.isEmpty()) {
                    if (why) {
                        *why = i18n("A button that opens a link needs an address.");
                    }
                    return false;
                }
                entry.replaceKey("/S", QPDFObjectHandle::newName("/URI"));
                entry.replaceKey("/URI", QPDFObjectHandle::newString(target.toStdString()));
                break;
            }

            const QVector<QPDFObjectHandle> widgets = widgetsOf(field);
            if (widgets.isEmpty()) {
                if (why) {
                    *why = i18n("“%1” has nothing on a page for anyone to press.", fieldName);
                }
                return false;
            }
            for (QPDFObjectHandle widget : widgets) {
                widget.replaceKey("/A", entry);
            }
            return true;
        },
        error);
}

bool FormBuilder::copyFieldsFrom(const QString &templatePdf, const QString &in, const QString &out, int *copied,
                                 QString *error)
{
    int count = 0;
    try {
        QPDF source;
        PdfFile::open(source, templatePdf);
        if (!checkNoXfa(source, error)) {
            return false;
        }
        QPDFObjectHandle sourceForm = source.getRoot().getKey("/AcroForm");
        if (!sourceForm.isDictionary() || !sourceForm.getKey("/Fields").isArray()) {
            if (error) {
                *error = i18n("The form to copy from has no fields.");
            }
            return false;
        }

        QPDF pdf;
        PdfFile::open(pdf, in);
        Build build;
        if (!prepare(build, pdf, error)) {
            return false;
        }

        std::map<QPDFObjGen, int> pageOfWidget;
        std::vector<QPDFPageObjectHelper> sourcePages = QPDFPageDocumentHelper(source).getAllPages();
        for (size_t i = 0; i < sourcePages.size(); ++i) {
            QPDFObjectHandle annots = sourcePages[i].getObjectHandle().getKey("/Annots");
            for (int a = 0; annots.isArray() && a < annots.getArrayNItems(); ++a) {
                pageOfWidget[annots.getArrayItem(a).getObjGen()] = int(i);
            }
        }

        const QVector<Located> incoming = allFields(sourceForm);
        const QVector<Located> already = allFields(build.acroForm);
        for (const Located &field : incoming) {
            if (findField(already, field.name)) {
                if (error) {
                    *error = i18n("This document already has a field called “%1”; remove it first, or the form "
                                  "would end up with two fields of one name.",
                                  field.name);
                }
                return false;
            }
        }

        // The widgets, and which page each belongs on, before /P is taken off
        // them: copying an object that still points at its page drags the whole
        // page across and the document gains a duplicate.
        QVector<QPDFObjectHandle> sourceTop;
        QVector<QVector<int>> pagesPerField;
        for (int i = 0; i < sourceForm.getKey("/Fields").getArrayNItems(); ++i) {
            QPDFObjectHandle top = sourceForm.getKey("/Fields").getArrayItem(i);
            if (!top.isDictionary()) {
                continue;
            }
            QVector<int> where;
            const std::function<void(QPDFObjectHandle)> visit = [&](QPDFObjectHandle node) {
                if (!node.isDictionary()) {
                    return;
                }
                if (node.hasKey("/Rect")) {
                    const auto found = pageOfWidget.find(node.getObjGen());
                    where.append(found == pageOfWidget.end() ? 0 : found->second);
                    node.removeKey("/P");
                }
                QPDFObjectHandle kids = node.getKey("/Kids");
                for (int k = 0; kids.isArray() && k < kids.getArrayNItems(); ++k) {
                    visit(kids.getArrayItem(k));
                }
            };
            visit(top);
            sourceTop.append(top);
            pagesPerField.append(where);
        }

        for (qsizetype i = 0; i < sourceTop.size(); ++i) {
            QPDFObjectHandle brought = pdf.copyForeignObject(sourceTop.at(i));
            int seen = 0;
            bool beyondTheEnd = false;
            const std::function<void(QPDFObjectHandle)> place = [&](QPDFObjectHandle node) {
                if (!node.isDictionary()) {
                    return;
                }
                if (node.hasKey("/Rect")) {
                    const int page = seen < pagesPerField.at(i).size() ? pagesPerField.at(i).at(seen) : 0;
                    ++seen;
                    if (size_t(page) >= build.pages.size()) {
                        beyondTheEnd = true;
                        return;
                    }
                    addToPage(build, page, node);
                }
                QPDFObjectHandle kids = node.getKey("/Kids");
                for (int k = 0; kids.isArray() && k < kids.getArrayNItems(); ++k) {
                    place(kids.getArrayItem(k));
                }
            };
            place(brought);
            if (beyondTheEnd) {
                if (error) {
                    *error = i18n("The form has fields on pages this document does not have.");
                }
                return false;
            }
            build.fieldsArray.appendItem(brought);
            ++count;
        }

        // The fonts the copied /DA strings name have to come too, or every one of
        // them draws with no font at all.
        QPDFObjectHandle sourceFonts = sourceForm.getKey("/DR").isDictionary()
            ? sourceForm.getKey("/DR").getKey("/Font")
            : QPDFObjectHandle::newNull();
        if (sourceFonts.isDictionary()) {
            for (const auto &[name, font] : sourceFonts.getDictAsMap()) {
                if (!build.drFonts.getKey(name).isDictionary()) {
                    build.drFonts.replaceKey(name, pdf.copyForeignObject(font));
                }
            }
        }
        if (sourceForm.getKey("/DA").isString() && !build.acroForm.getKey("/DA").isString()) {
            build.acroForm.replaceKey("/DA", QPDFObjectHandle::newString(sourceForm.getKey("/DA").getUTF8Value()));
        }
        if (sourceForm.getKey("/SigFlags").isInteger()) {
            build.acroForm.replaceKey("/SigFlags",
                                      QPDFObjectHandle::newInteger(sourceForm.getKey("/SigFlags").getIntValueAsInt()));
        }

        settleAppearances(pdf);
        writeDocument(pdf, out);
    } catch (const std::exception &e) {
        if (error) {
            *error = QString::fromUtf8(e.what());
        }
        return false;
    }

    if (copied) {
        *copied = count;
    }
    return true;
}

bool FormBuilder::exportData(const QString &pdf, const QString &toFile, QString *error)
{
    QString why;
    const QVector<FormField> fields = uniqueFields(pdf, &why);
    if (!why.isEmpty()) {
        if (error) {
            *error = why;
        }
        return false;
    }
    if (fields.isEmpty()) {
        if (error) {
            *error = i18n("This document has no form to take data out of.");
        }
        return false;
    }

    const QString suffix = QFileInfo(toFile).suffix().toLower();
    if (suffix == QLatin1String("csv")) {
        QStringList names;
        QStringList values;
        for (const FormField &field : fields) {
            if (field.kind == FormField::Kind::Button || field.kind == FormField::Kind::Signature) {
                continue;
            }
            names << csvCell(field.name);
            values << csvCell(field.value);
        }
        const QString text
            = names.join(QLatin1Char(',')) + QLatin1Char('\n') + values.join(QLatin1Char(',')) + QLatin1Char('\n');
        return writeTextFile(toFile, text.toUtf8(), error);
    }

    if (suffix == QLatin1String("xfdf")) {
        QByteArray bytes;
        QXmlStreamWriter writer(&bytes);
        writer.setAutoFormatting(true);
        writer.writeStartDocument();
        writer.writeStartElement(QStringLiteral("xfdf"));
        writer.writeAttribute(QStringLiteral("xmlns"), QStringLiteral("http://ns.adobe.com/xfdf/"));
        writer.writeStartElement(QStringLiteral("fields"));
        for (const FormField &field : fields) {
            if (field.kind == FormField::Kind::Button || field.kind == FormField::Kind::Signature) {
                continue;
            }
            // Flat, even for a dotted name: readers accept it, and nesting would
            // put the burden of matching names onto whoever edits the file.
            writer.writeStartElement(QStringLiteral("field"));
            writer.writeAttribute(QStringLiteral("name"), field.name);
            writer.writeTextElement(QStringLiteral("value"), field.value);
            writer.writeEndElement();
        }
        writer.writeEndElement();
        writer.writeStartElement(QStringLiteral("f"));
        writer.writeAttribute(QStringLiteral("href"), QFileInfo(pdf).fileName());
        writer.writeEndElement();
        writer.writeEndElement();
        writer.writeEndDocument();
        return writeTextFile(toFile, bytes, error);
    }

    if (suffix != QLatin1String("fdf")) {
        if (error) {
            *error = i18n("“%1” is not a kind of data file this can write. Use .fdf, .xfdf or .csv.", toFile);
        }
        return false;
    }

    std::string body = "%FDF-1.2\n1 0 obj\n<< /FDF << /Fields [";
    for (const FormField &field : fields) {
        if (field.kind == FormField::Kind::Button || field.kind == FormField::Kind::Signature) {
            continue;
        }
        body += "\n<< /T " + pdfTextLiteral(field.name) + " /V ";
        if (field.kind == FormField::Kind::Checkbox || field.kind == FormField::Kind::Radio) {
            // A button's value is a name, not a string; written as a string it
            // imports as a box that is set to the text "/Yes" and shows nothing.
            body += field.value.startsWith(QLatin1Char('/')) ? field.value.toStdString()
                                                             : ("/" + field.value.toStdString());
        } else {
            body += pdfTextLiteral(field.value);
        }
        body += " >>";
    }
    body += "\n] /F " + pdfTextLiteral(QFileInfo(pdf).fileName())
        + " >> >>\nendobj\ntrailer\n<< /Root 1 0 R >>\n%%EOF\n";
    return writeTextFile(toFile, QByteArray::fromStdString(body), error);
}

bool FormBuilder::importData(const QString &in, const QString &out, const QString &fromFile, int *filled,
                             QString *error)
{
    QFile file(fromFile);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) {
            *error = i18n("“%1” cannot be read.", fromFile);
        }
        return false;
    }
    const QByteArray bytes = file.readAll();
    file.close();

    const QString suffix = QFileInfo(fromFile).suffix().toLower();
    QHash<QString, QString> values;
    if (suffix == QLatin1String("csv")) {
        const QVector<QStringList> rows = parseCsv(QString::fromUtf8(bytes));
        if (rows.size() < 2) {
            if (error) {
                *error = i18n("“%1” has a heading row but no data under it.", fromFile);
            }
            return false;
        }
        for (qsizetype i = 0; i < rows.constFirst().size() && i < rows.at(1).size(); ++i) {
            values.insert(rows.constFirst().at(i), rows.at(1).at(i));
        }
    } else if (suffix == QLatin1String("xfdf")) {
        values = parseXfdf(bytes);
    } else if (suffix == QLatin1String("fdf")) {
        values = parseFdf(bytes);
    } else {
        if (error) {
            *error = i18n("“%1” is not a kind of data file this can read. Use .fdf, .xfdf or .csv.", fromFile);
        }
        return false;
    }

    if (values.isEmpty()) {
        if (error) {
            *error = i18n("“%1” holds no field values.", fromFile);
        }
        return false;
    }

    // A tick box's exported value is its own on-state name, which is exactly what
    // filling reads as "not one of the words meaning yes" and turns off again.
    QString why;
    const QVector<FormField> existing = uniqueFields(in, &why);
    QHash<QString, QString> prepared;
    for (auto value = values.constBegin(); value != values.constEnd(); ++value) {
        QString written = value.value();
        for (const FormField &field : existing) {
            if (field.name != value.key() || field.kind != FormField::Kind::Checkbox) {
                continue;
            }
            if (field.options.contains(written) || field.options.contains(QLatin1Char('/') + written)) {
                written = QStringLiteral("on");
            } else if (written == QLatin1String("/Off") || written.isEmpty()) {
                written = QStringLiteral("off");
            }
            break;
        }
        prepared.insert(value.key(), written);
    }

    return Forms::fill(in, out, prepared, filled, nullptr, error);
}

bool FormBuilder::collect(const QStringList &pdfs, const QString &toCsv, QString *error)
{
    if (pdfs.isEmpty()) {
        if (error) {
            *error = i18n("There are no documents to collect.");
        }
        return false;
    }

    QStringList columns;
    QVector<QHash<QString, QString>> rows;
    for (const QString &pdf : pdfs) {
        QString why;
        const QVector<FormField> fields = uniqueFields(pdf, &why);
        if (!why.isEmpty()) {
            if (error) {
                *error = why;
            }
            return false;
        }
        QHash<QString, QString> row;
        for (const FormField &field : fields) {
            if (field.kind == FormField::Kind::Button || field.kind == FormField::Kind::Signature) {
                continue;
            }
            if (!columns.contains(field.name)) {
                columns << field.name;
            }
            row.insert(field.name, field.value);
        }
        rows.append(row);
    }

    if (columns.isEmpty()) {
        if (error) {
            *error = i18n("None of these documents has a form.");
        }
        return false;
    }

    QStringList heading;
    heading << csvCell(i18nc("@title:column the file a row of form answers came from", "Document"));
    for (const QString &column : std::as_const(columns)) {
        heading << csvCell(column);
    }
    QString text = heading.join(QLatin1Char(',')) + QLatin1Char('\n');
    for (qsizetype i = 0; i < rows.size(); ++i) {
        QStringList cells;
        cells << csvCell(QFileInfo(pdfs.at(i)).fileName());
        for (const QString &column : std::as_const(columns)) {
            cells << csvCell(rows.at(i).value(column));
        }
        text += cells.join(QLatin1Char(',')) + QLatin1Char('\n');
    }
    return writeTextFile(toCsv, text.toUtf8(), error);
}

QStringList FormBuilder::limitations()
{
    return {
        i18n("Every field is drawn with a border, a background and, for a tick box, its tick, because a field "
             "with no drawing of its own is invisible in about half the readers in use."),
        i18n("A font size of zero means the reader fits the text to the box. That is legal and widely honoured, "
             "but the size drawn here is our own guess at the same answer, so the two can differ slightly."),
        i18n("Only the fourteen fonts every reader already knows can be used. A field in a font of your own "
             "would need that font embedding, which is a separate job."),
        i18n("Formatting, validation and calculation are JavaScript, which PDF/A forbids and which several "
             "readers do not run at all."),
        i18n("A document with an XFA form is refused rather than added to, because the result would behave "
             "differently in every reader."),
        i18n("A field can be renamed but not moved into another group, since anything in the document "
             "referring to it by name would then be pointing at nothing."),
        i18n("Data files are read as a flat list of names and values. A file that nests fields, or carries "
             "appearances and pages of its own, gives up only its values."),
        i18n("Signature fields are created empty and ready to be signed; signing itself is a separate job."),
    };
}

} // namespace ps
