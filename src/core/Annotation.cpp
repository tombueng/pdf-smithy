/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#include "Annotation.h"

#include "PdfFile.h"
#include "PdfGeometry.h"

#include <KLocalizedString>

#include <QFile>
#include <QSaveFile>
#include <QTransform>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>
#include <QUuid>

#include <qpdf/QPDF.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>
#include <qpdf/QPDFWriter.hh>

#include <cmath>

namespace ps {

namespace {

using PdfGeometry::number;

/** The PDF /Subtype for each kind, and back again. */
struct TypeName {
    Annotation::Type type;
    const char *name;
};

constexpr TypeName typeNames[] = {
    { Annotation::Type::Highlight, "/Highlight" },
    { Annotation::Type::Underline, "/Underline" },
    { Annotation::Type::StrikeOut, "/StrikeOut" },
    { Annotation::Type::Squiggly, "/Squiggly" },
    { Annotation::Type::Ink, "/Ink" },
    { Annotation::Type::Square, "/Square" },
    { Annotation::Type::Circle, "/Circle" },
    { Annotation::Type::Line, "/Line" },
    { Annotation::Type::FreeText, "/FreeText" },
    { Annotation::Type::Note, "/Text" },
};

std::string subtypeOf(Annotation::Type type)
{
    for (const TypeName &entry : typeNames) {
        if (entry.type == type) {
            return entry.name;
        }
    }
    return "/Square";
}

bool typeFromSubtype(const std::string &subtype, Annotation::Type *type)
{
    for (const TypeName &entry : typeNames) {
        if (subtype == entry.name) {
            *type = entry.type;
            return true;
        }
    }
    return false;
}

bool isTextMarkup(Annotation::Type type)
{
    return type == Annotation::Type::Highlight || type == Annotation::Type::Underline
        || type == Annotation::Type::StrikeOut || type == Annotation::Type::Squiggly;
}

std::string colourArray(const QColor &colour)
{
    return "[" + number(colour.redF()) + " " + number(colour.greenF()) + " " + number(colour.blueF()) + "]";
}

std::string setColour(const QColor &colour, bool stroking)
{
    return number(colour.redF()) + " " + number(colour.greenF()) + " " + number(colour.blueF())
        + (stroking ? " RG\n" : " rg\n");
}

std::string rectArray(const QRectF &rect)
{
    return "[" + number(rect.left()) + " " + number(rect.top()) + " " + number(rect.right()) + " "
        + number(rect.bottom()) + "]";
}

/** An ellipse inscribed in @p box, as the four Béziers everyone draws it with. */
std::string ellipsePath(const QRectF &box)
{
    // 0.5523 is the classic circle-to-Bézier constant; anything less makes a
    // visibly flat-sided circle at large sizes.
    const double kappa = 0.5523;
    const double cx = box.center().x();
    const double cy = box.center().y();
    const double rx = box.width() / 2.0;
    const double ry = box.height() / 2.0;
    const double ox = rx * kappa;
    const double oy = ry * kappa;

    std::string path = number(cx - rx) + " " + number(cy) + " m\n";
    path += number(cx - rx) + " " + number(cy + oy) + " " + number(cx - ox) + " " + number(cy + ry) + " " + number(cx)
        + " " + number(cy + ry) + " c\n";
    path += number(cx + ox) + " " + number(cy + ry) + " " + number(cx + rx) + " " + number(cy + oy) + " "
        + number(cx + rx) + " " + number(cy) + " c\n";
    path += number(cx + rx) + " " + number(cy - oy) + " " + number(cx + ox) + " " + number(cy - ry) + " " + number(cx)
        + " " + number(cy - ry) + " c\n";
    path += number(cx - ox) + " " + number(cy - ry) + " " + number(cx - rx) + " " + number(cy - oy) + " "
        + number(cx - rx) + " " + number(cy) + " c\n";
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
            out += '?'; // PDFDocEncoding cannot carry it; /Contents holds the real text.
        }
    }
    return out + ")";
}

/** The whole comment as PDF text, in UTF-16 so any language survives. */
QPDFObjectHandle unicodeString(const QString &text)
{
    QByteArray bytes("\xFE\xFF", 2);
    for (const QChar &character : text) {
        bytes.append(char(character.unicode() >> 8));
        bytes.append(char(character.unicode() & 0xFF));
    }
    return QPDFObjectHandle::newString(std::string(bytes.constData(), size_t(bytes.size())));
}

QString readString(QPDFObjectHandle value)
{
    if (!value.isString()) {
        return {};
    }
    return QString::fromStdString(value.getUTF8Value());
}

/**
 * The marks themselves, drawn in display coordinates.
 *
 * The form's /Matrix carries them into page space afterwards, which is what
 * keeps this readable: "the underline goes along the bottom of the word" stays
 * true on a page turned ninety degrees, instead of quietly becoming "along the
 * left edge".
 */
std::string appearanceStream(const Annotation &annotation, bool *needsBlend, bool *needsFont)
{
    *needsBlend = false;
    *needsFont = false;

    std::string content = "/GS gs\n";

    switch (annotation.type) {
    case Annotation::Type::Highlight: {
        // Multiply rather than transparency, so black text under yellow stays
        // black instead of turning grey.
        *needsBlend = true;
        content += setColour(annotation.colour, false);
        for (const QRectF &quad : annotation.quads) {
            content += number(quad.x()) + " " + number(quad.y()) + " " + number(quad.width()) + " "
                + number(quad.height()) + " re\n";
        }
        content += "f\n";
        break;
    }

    case Annotation::Type::Underline:
    case Annotation::Type::StrikeOut: {
        content += setColour(annotation.colour, false);
        for (const QRectF &quad : annotation.quads) {
            const double thickness = qMax(0.6, quad.height() / 14.0);
            const double y = annotation.type == Annotation::Type::Underline ? quad.y() + quad.height() * 0.06
                                                                            : quad.y() + quad.height() * 0.42;
            content
                += number(quad.x()) + " " + number(y) + " " + number(quad.width()) + " " + number(thickness) + " re\n";
        }
        content += "f\n";
        break;
    }

    case Annotation::Type::Squiggly: {
        content += setColour(annotation.colour, true);
        for (const QRectF &quad : annotation.quads) {
            const double amplitude = qMax(1.0, quad.height() / 12.0);
            const double step = amplitude * 2.0;
            const double baseline = quad.y() + amplitude;
            content += number(qMax(0.5, amplitude / 2.0)) + " w 1 J 1 j\n";
            content += number(quad.x()) + " " + number(baseline) + " m\n";
            bool up = true;
            for (double x = quad.x() + step; x < quad.right(); x += step) {
                content += number(x) + " " + number(baseline + (up ? amplitude : -amplitude)) + " l\n";
                up = !up;
            }
            content += "S\n";
        }
        break;
    }

    case Annotation::Type::Ink:
    case Annotation::Type::Line: {
        content += setColour(annotation.colour, true);
        content += number(annotation.lineWidth) + " w 1 J 1 j\n";
        for (const QVector<QPointF> &stroke : annotation.strokes) {
            if (stroke.size() < 2) {
                // A single tap still deserves a mark, or the user taps again.
                if (stroke.size() == 1) {
                    content += setColour(annotation.colour, false);
                    content += ellipsePath(QRectF(stroke.first().x() - annotation.lineWidth / 2.0,
                                                  stroke.first().y() - annotation.lineWidth / 2.0, annotation.lineWidth,
                                                  annotation.lineWidth));
                    content += "f\n";
                    content += setColour(annotation.colour, true);
                }
                continue;
            }
            content += number(stroke.first().x()) + " " + number(stroke.first().y()) + " m\n";
            for (int i = 1; i < stroke.size(); ++i) {
                content += number(stroke.at(i).x()) + " " + number(stroke.at(i).y()) + " l\n";
            }
            content += "S\n";
        }
        break;
    }

    case Annotation::Type::Square:
    case Annotation::Type::Circle: {
        // Inset by half the pen, or the stroke hangs outside the rectangle the
        // user drew and the shape looks bigger than it was.
        const QRectF box = annotation.rect.adjusted(annotation.lineWidth / 2.0, annotation.lineWidth / 2.0,
                                                    -annotation.lineWidth / 2.0, -annotation.lineWidth / 2.0);
        if (annotation.interior.isValid()) {
            content += setColour(annotation.interior, false);
        }
        content += setColour(annotation.colour, true);
        content += number(annotation.lineWidth) + " w\n";

        if (annotation.type == Annotation::Type::Circle) {
            content += ellipsePath(box);
        } else {
            content += number(box.x()) + " " + number(box.y()) + " " + number(box.width()) + " " + number(box.height())
                + " re\n";
        }
        content += annotation.interior.isValid() ? "B\n" : "S\n";
        break;
    }

    case Annotation::Type::FreeText: {
        *needsFont = true;
        const QRectF box = annotation.rect;
        content += setColour(annotation.interior.isValid() ? annotation.interior : QColor(255, 255, 255), false);
        content += setColour(annotation.colour, true);
        content += "1 w\n";
        content += number(box.x() + 0.5) + " " + number(box.y() + 0.5) + " " + number(box.width() - 1.0) + " "
            + number(box.height() - 1.0) + " re\n";
        content += annotation.interior.isValid() ? "B\n" : "S\n";

        content += "BT\n/Helv " + number(annotation.fontSize) + " Tf\n";
        content += "0 0 0 rg\n";
        // Wrapping by measurement would need font metrics here; the text is
        // laid out line by line and the box is the user's to size.
        const QStringList lines = annotation.contents.split(QLatin1Char('\n'));
        double y = box.top() - annotation.fontSize * 1.1;
        for (const QString &line : lines) {
            if (y < box.y()) {
                break;
            }
            content += "1 0 0 1 " + number(box.x() + 3.0) + " " + number(y) + " Tm\n";
            content += literalString(line) + " Tj\n";
            y -= annotation.fontSize * 1.25;
        }
        content += "ET\n";
        break;
    }

    case Annotation::Type::Note: {
        // Drawn rather than left to the reader, because every reader draws a
        // different icon and some draw none at all.
        const QRectF box = annotation.rect;
        content += setColour(annotation.colour, false);
        content += number(box.x()) + " " + number(box.y() + box.height() * 0.25) + " " + number(box.width()) + " "
            + number(box.height() * 0.75) + " re\nf\n";
        // The little tail that makes it read as a speech bubble.
        content += number(box.x() + box.width() * 0.2) + " " + number(box.y() + box.height() * 0.3) + " m\n";
        content += number(box.x() + box.width() * 0.2) + " " + number(box.y()) + " l\n";
        content += number(box.x() + box.width() * 0.45) + " " + number(box.y() + box.height() * 0.3) + " l\nf\n";

        content += "1 1 1 RG " + number(qMax(0.7, box.height() / 14.0)) + " w 1 J\n";
        for (int line = 0; line < 3; ++line) {
            const double y = box.y() + box.height() * (0.42 + line * 0.19);
            content += number(box.x() + box.width() * 0.18) + " " + number(y) + " m "
                + number(box.right() - box.width() * 0.18) + " " + number(y) + " l S\n";
        }
        break;
    }
    }

    return content;
}

/** Display points to page space, /Rotate and a non-zero media origin included. */
QTransform pageTransformFor(QPDFPageObjectHelper &page)
{
    const QRectF media = PdfGeometry::mediaBoxOf(page);
    return PdfGeometry::displayToPageTransform(PdfGeometry::rotationOf(page), media.width(), media.height())
        * QTransform::fromTranslate(media.x(), media.y());
}

QPDFObjectHandle buildAnnotation(QPDF &pdf, const Annotation &annotation, const QTransform &toPage)
{
    Annotation prepared = annotation;
    if (prepared.rect.isEmpty() && !prepared.quads.isEmpty()) {
        prepared.rect = prepared.quads.constFirst();
        for (const QRectF &quad : prepared.quads) {
            prepared.rect = prepared.rect.united(quad);
        }
    }
    if (prepared.rect.isEmpty() && !prepared.strokes.isEmpty()) {
        for (const QVector<QPointF> &stroke : prepared.strokes) {
            for (const QPointF &point : stroke) {
                prepared.rect = prepared.rect.isNull() ? QRectF(point, QSizeF(0.01, 0.01))
                                                       : prepared.rect.united(QRectF(point, QSizeF(0.01, 0.01)));
            }
        }
        const double pad = prepared.lineWidth + 1.0;
        prepared.rect.adjust(-pad, -pad, pad, pad);
    }
    if (prepared.rect.isEmpty()) {
        return QPDFObjectHandle::newNull();
    }

    bool needsBlend = false;
    bool needsFont = false;
    const std::string content = appearanceStream(prepared, &needsBlend, &needsFont);

    // The appearance is drawn in display coordinates and carried into page
    // space by /Matrix, so the same stream is correct on a turned page.
    QPDFObjectHandle form = QPDFObjectHandle::newStream(&pdf, content);
    QPDFObjectHandle formDict = QPDFObjectHandle::parse("<< /Type /XObject /Subtype /Form >>");
    formDict.replaceKey("/BBox", QPDFObjectHandle::parse(rectArray(prepared.rect.normalized())));
    formDict.replaceKey("/Matrix",
                        QPDFObjectHandle::parse("[" + number(toPage.m11()) + " " + number(toPage.m12()) + " "
                                                + number(toPage.m21()) + " " + number(toPage.m22()) + " "
                                                + number(toPage.dx()) + " " + number(toPage.dy()) + "]"));

    std::string state = "<< /Type /ExtGState /CA " + number(prepared.opacity) + " /ca " + number(prepared.opacity);
    if (needsBlend) {
        state += " /BM /Multiply";
    }
    state += " >>";

    std::string resources = "<< /ExtGState << /GS " + state + " >>";
    if (needsFont) {
        resources += " /Font << /Helv << /Type /Font /Subtype /Type1 /BaseFont /Helvetica"
                     " /Encoding /WinAnsiEncoding >> >>";
    }
    resources += " >>";
    formDict.replaceKey("/Resources", QPDFObjectHandle::parse(resources));
    form.replaceDict(formDict);

    QPDFObjectHandle object = QPDFObjectHandle::parse("<< /Type /Annot >>");
    object.replaceKey("/Subtype", QPDFObjectHandle::newName(subtypeOf(prepared.type)));
    object.replaceKey("/Rect", QPDFObjectHandle::parse(rectArray(toPage.mapRect(prepared.rect.normalized()))));
    // Bit 3 is Print. Without it the comment is on screen only, which is not
    // what anyone means by "mark up this document".
    object.replaceKey("/F", QPDFObjectHandle::newInteger(4));
    object.replaceKey("/C", QPDFObjectHandle::parse(colourArray(prepared.colour)));
    object.replaceKey("/CA", QPDFObjectHandle::newReal(prepared.opacity, 3));
    object.replaceKey("/AP", QPDFObjectHandle::parse("<< >>"));
    object.getKey("/AP").replaceKey("/N", pdf.makeIndirectObject(form));

    if (!prepared.contents.isEmpty()) {
        object.replaceKey("/Contents", unicodeString(prepared.contents));
    }
    if (!prepared.author.isEmpty()) {
        object.replaceKey("/T", unicodeString(prepared.author));
    }

    const QDateTime stamp = prepared.created.isValid() ? prepared.created : QDateTime::currentDateTime();
    object.replaceKey("/CreationDate", QPDFObjectHandle::newString(PdfFile::formatDate(stamp).toStdString()));
    object.replaceKey("/M", QPDFObjectHandle::newString(PdfFile::formatDate(stamp).toStdString()));

    // A name of our own, so that removing one particular comment later does not
    // come down to guessing from coordinates.
    const QString name = prepared.identifier.isEmpty()
        ? QStringLiteral("ps-") + QUuid::createUuid().toString(QUuid::WithoutBraces)
        : prepared.identifier;
    object.replaceKey("/NM", QPDFObjectHandle::newString(name.toStdString()));

    if (isTextMarkup(prepared.type)) {
        std::string quads = "[";
        for (const QRectF &quad : prepared.quads) {
            const QRectF mapped = toPage.mapRect(quad.normalized());
            // Upper-left, upper-right, lower-left, lower-right: the order the
            // specification gives, which is not the order it looks like.
            quads += " " + number(mapped.left()) + " " + number(mapped.bottom()) + " " + number(mapped.right()) + " "
                + number(mapped.bottom()) + " " + number(mapped.left()) + " " + number(mapped.top()) + " "
                + number(mapped.right()) + " " + number(mapped.top());
        }
        quads += "]";
        object.replaceKey("/QuadPoints", QPDFObjectHandle::parse(quads));
    }

    if (prepared.type == Annotation::Type::Ink) {
        std::string list = "[";
        for (const QVector<QPointF> &stroke : prepared.strokes) {
            list += "[";
            for (const QPointF &point : stroke) {
                const QPointF mapped = toPage.map(point);
                list += " " + number(mapped.x()) + " " + number(mapped.y());
            }
            list += "]";
        }
        list += "]";
        object.replaceKey("/InkList", QPDFObjectHandle::parse(list));
        object.replaceKey("/BS", QPDFObjectHandle::parse("<< /W " + number(prepared.lineWidth) + " >>"));
    }

    if (prepared.type == Annotation::Type::Line && !prepared.strokes.isEmpty()
        && prepared.strokes.constFirst().size() >= 2) {
        const QPointF from = toPage.map(prepared.strokes.constFirst().constFirst());
        const QPointF to = toPage.map(prepared.strokes.constFirst().at(1));
        object.replaceKey("/L",
                          QPDFObjectHandle::parse("[" + number(from.x()) + " " + number(from.y()) + " " + number(to.x())
                                                  + " " + number(to.y()) + "]"));
        object.replaceKey("/BS", QPDFObjectHandle::parse("<< /W " + number(prepared.lineWidth) + " >>"));
    }

    if (prepared.type == Annotation::Type::Square || prepared.type == Annotation::Type::Circle
        || prepared.type == Annotation::Type::FreeText) {
        object.replaceKey("/BS", QPDFObjectHandle::parse("<< /W " + number(prepared.lineWidth) + " >>"));
        if (prepared.interior.isValid()) {
            object.replaceKey("/IC", QPDFObjectHandle::parse(colourArray(prepared.interior)));
        }
    }

    if (prepared.type == Annotation::Type::FreeText) {
        object.replaceKey("/DA", QPDFObjectHandle::newString("/Helv " + number(prepared.fontSize) + " Tf 0 g"));
    }

    if (prepared.type == Annotation::Type::Note) {
        object.replaceKey("/Name", QPDFObjectHandle::newName("/Comment"));
        object.replaceKey("/Open", QPDFObjectHandle::newBool(false));
    }

    return pdf.makeIndirectObject(object);
}

} // namespace

bool Annotations::add(const QString &inputPdf, const QString &outputPdf, const QVector<Annotation> &annotations,
                      QString *error)
{
    if (annotations.isEmpty()) {
        if (error) {
            *error = i18n("There is nothing to add.");
        }
        return false;
    }

    try {
        QPDF pdf;
        PdfFile::open(pdf, inputPdf);

        QPDFPageDocumentHelper documents(pdf);
        std::vector<QPDFPageObjectHelper> pages = documents.getAllPages();

        for (const Annotation &annotation : annotations) {
            if (annotation.page < 0 || annotation.page >= int(pages.size())) {
                continue;
            }
            QPDFPageObjectHelper page = pages.at(size_t(annotation.page));
            QPDFObjectHandle object = buildAnnotation(pdf, annotation, pageTransformFor(page));
            if (object.isNull()) {
                continue;
            }

            QPDFObjectHandle existing = page.getObjectHandle().getKey("/Annots");
            if (!existing.isArray()) {
                existing = QPDFObjectHandle::newArray();
                page.getObjectHandle().replaceKey("/Annots", existing);
            }
            existing.appendItem(object);
        }

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

    return true;
}

QVector<Annotation> Annotations::read(const QString &pdf, const QVector<int> &pages, QString *error)
{
    QVector<Annotation> found;
    try {
        QPDF document;
        PdfFile::open(document, pdf);

        QPDFPageDocumentHelper helper(document);
        std::vector<QPDFPageObjectHelper> all = helper.getAllPages();

        for (size_t index = 0; index < all.size(); ++index) {
            if (!pages.isEmpty() && !pages.contains(int(index))) {
                continue;
            }
            QPDFPageObjectHelper page = all.at(index);

            bool invertible = false;
            const QTransform toDisplay = pageTransformFor(page).inverted(&invertible);
            if (!invertible) {
                continue;
            }

            QPDFObjectHandle annotations = page.getObjectHandle().getKey("/Annots");
            if (!annotations.isArray()) {
                continue;
            }

            for (int i = 0; i < annotations.getArrayNItems(); ++i) {
                QPDFObjectHandle object = annotations.getArrayItem(i);
                if (!object.isDictionary()) {
                    continue;
                }
                QPDFObjectHandle subtype = object.getKey("/Subtype");
                Annotation annotation;
                if (!subtype.isName() || !typeFromSubtype(subtype.getName(), &annotation.type)) {
                    continue; // Widgets, links and popups are somebody else's business.
                }

                annotation.page = int(index);
                QPDFObjectHandle rect = object.getKey("/Rect");
                if (rect.isArray() && rect.getArrayNItems() == 4) {
                    const double left = PdfGeometry::boxValue(rect, 0, 0.0);
                    const double bottom = PdfGeometry::boxValue(rect, 1, 0.0);
                    const double right = PdfGeometry::boxValue(rect, 2, 0.0);
                    const double top = PdfGeometry::boxValue(rect, 3, 0.0);
                    annotation.rect = toDisplay.mapRect(QRectF(std::min(left, right), std::min(bottom, top),
                                                               std::abs(right - left), std::abs(top - bottom)));
                }

                QPDFObjectHandle colour = object.getKey("/C");
                if (colour.isArray() && colour.getArrayNItems() == 3) {
                    annotation.colour
                        = QColor::fromRgbF(PdfGeometry::numberAt(colour, 0, 0.0), PdfGeometry::numberAt(colour, 1, 0.0),
                                           PdfGeometry::numberAt(colour, 2, 0.0));
                }
                QPDFObjectHandle interior = object.getKey("/IC");
                if (interior.isArray() && interior.getArrayNItems() == 3) {
                    annotation.interior = QColor::fromRgbF(PdfGeometry::numberAt(interior, 0, 0.0),
                                                           PdfGeometry::numberAt(interior, 1, 0.0),
                                                           PdfGeometry::numberAt(interior, 2, 0.0));
                }
                if (object.getKey("/CA").isNumber()) {
                    annotation.opacity = PdfGeometry::numericValue(object.getKey("/CA"), 1.0);
                }

                annotation.contents = readString(object.getKey("/Contents"));
                annotation.author = readString(object.getKey("/T"));
                annotation.identifier = readString(object.getKey("/NM"));
                annotation.created = PdfFile::parseDate(readString(object.getKey("/CreationDate")));

                QPDFObjectHandle quads = object.getKey("/QuadPoints");
                if (quads.isArray()) {
                    for (int q = 0; q + 7 < quads.getArrayNItems(); q += 8) {
                        double xs[4];
                        double ys[4];
                        for (int corner = 0; corner < 4; ++corner) {
                            xs[corner] = PdfGeometry::numberAt(quads, q + corner * 2, 0.0);
                            ys[corner] = PdfGeometry::numberAt(quads, q + corner * 2 + 1, 0.0);
                        }
                        const double left = *std::min_element(xs, xs + 4);
                        const double right = *std::max_element(xs, xs + 4);
                        const double bottom = *std::min_element(ys, ys + 4);
                        const double top = *std::max_element(ys, ys + 4);
                        annotation.quads.append(toDisplay.mapRect(QRectF(left, bottom, right - left, top - bottom)));
                    }
                }

                QPDFObjectHandle ink = object.getKey("/InkList");
                if (ink.isArray()) {
                    for (int stroke = 0; stroke < ink.getArrayNItems(); ++stroke) {
                        QPDFObjectHandle points = ink.getArrayItem(stroke);
                        if (!points.isArray()) {
                            continue;
                        }
                        QVector<QPointF> path;
                        for (int point = 0; point + 1 < points.getArrayNItems(); point += 2) {
                            path.append(toDisplay.map(QPointF(PdfGeometry::numberAt(points, point, 0.0),
                                                              PdfGeometry::numberAt(points, point + 1, 0.0))));
                        }
                        annotation.strokes.append(path);
                    }
                }

                found.append(annotation);
            }
        }
    } catch (const std::exception &e) {
        if (error) {
            *error = QString::fromUtf8(e.what());
        }
        return {};
    }

    return found;
}

bool Annotations::remove(const QString &inputPdf, const QString &outputPdf, const QVector<int> &pages,
                         const QStringList &identifiers, int *removed, QString *error)
{
    int count = 0;
    try {
        QPDF pdf;
        PdfFile::open(pdf, inputPdf);

        QPDFPageDocumentHelper documents(pdf);
        std::vector<QPDFPageObjectHelper> all = documents.getAllPages();

        for (size_t index = 0; index < all.size(); ++index) {
            if (!pages.isEmpty() && !pages.contains(int(index))) {
                continue;
            }
            QPDFPageObjectHelper page = all.at(index);
            QPDFObjectHandle annotations = page.getObjectHandle().getKey("/Annots");
            if (!annotations.isArray()) {
                continue;
            }

            QPDFObjectHandle kept = QPDFObjectHandle::newArray();
            for (int i = 0; i < annotations.getArrayNItems(); ++i) {
                QPDFObjectHandle object = annotations.getArrayItem(i);
                const std::string subtype = object.isDictionary() && object.getKey("/Subtype").isName()
                    ? object.getKey("/Subtype").getName()
                    : std::string();

                Annotation::Type type;
                const bool isComment = typeFromSubtype(subtype, &type);
                const bool named = identifiers.isEmpty() || identifiers.contains(readString(object.getKey("/NM")));

                // /Widget is a form field. Deleting the form because someone
                // asked to clear the comments would be indefensible.
                if (isComment && named) {
                    ++count;
                } else {
                    kept.appendItem(object);
                }
            }

            if (kept.getArrayNItems() == 0) {
                page.getObjectHandle().removeKey("/Annots");
            } else {
                page.getObjectHandle().replaceKey("/Annots", kept);
            }
        }

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

    if (removed) {
        *removed = count;
    }
    return true;
}

bool Annotations::flatten(const QString &inputPdf, const QString &outputPdf, const QVector<int> &pages, int *flattened,
                          QString *error)
{
    int count = 0;
    try {
        QPDF pdf;
        PdfFile::open(pdf, inputPdf);

        QPDFPageDocumentHelper documents(pdf);
        std::vector<QPDFPageObjectHelper> all = documents.getAllPages();

        for (size_t index = 0; index < all.size(); ++index) {
            if (!pages.isEmpty() && !pages.contains(int(index))) {
                continue;
            }
            QPDFPageObjectHelper page = all.at(index);
            QPDFObjectHandle annotations = page.getObjectHandle().getKey("/Annots");
            if (!annotations.isArray() || annotations.getArrayNItems() == 0) {
                continue;
            }

            QPDFObjectHandle resources = page.getAttribute("/Resources", true);
            if (!resources.isDictionary()) {
                resources = QPDFObjectHandle::parse("<< >>");
                page.getObjectHandle().replaceKey("/Resources", resources);
            }
            if (!resources.getKey("/XObject").isDictionary()) {
                resources.replaceKey("/XObject", QPDFObjectHandle::newDictionary());
            }
            QPDFObjectHandle xobjects = resources.getKey("/XObject");

            std::string content;
            QPDFObjectHandle kept = QPDFObjectHandle::newArray();
            int suffix = 1;

            for (int i = 0; i < annotations.getArrayNItems(); ++i) {
                QPDFObjectHandle object = annotations.getArrayItem(i);
                Annotation::Type type;
                const std::string subtype = object.isDictionary() && object.getKey("/Subtype").isName()
                    ? object.getKey("/Subtype").getName()
                    : std::string();
                QPDFObjectHandle appearance
                    = object.isDictionary() ? object.getKey("/AP") : QPDFObjectHandle::newNull();
                QPDFObjectHandle normal
                    = appearance.isDictionary() ? appearance.getKey("/N") : QPDFObjectHandle::newNull();

                // Only what we can actually draw; anything else stays a live
                // annotation rather than silently disappearing.
                if (!typeFromSubtype(subtype, &type) || !normal.isStream()) {
                    kept.appendItem(object);
                    continue;
                }

                const std::string name = xobjects.getUniqueResourceName("/PsAnnot", suffix);
                xobjects.replaceKey(name, normal);
                // The appearance already carries its own /Matrix, so drawing it
                // needs nothing beyond isolating the graphics state.
                content += "q\n" + name + " Do\nQ\n";
                ++count;
            }

            if (content.empty()) {
                continue;
            }

            PdfGeometry::isolateExistingContent(page, page.getObjectHandle());
            page.addPageContents(QPDFObjectHandle::newStream(&pdf, content), false);

            if (kept.getArrayNItems() == 0) {
                page.getObjectHandle().removeKey("/Annots");
            } else {
                page.getObjectHandle().replaceKey("/Annots", kept);
            }
        }

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

// ── XFDF ──────────────────────────────────────────────────────────────────

namespace {

/** The XFDF element name for each kind, and back again. */
struct ElementName {
    Annotation::Type type;
    const char *element;
};

constexpr ElementName elementNames[] = {
    { Annotation::Type::Highlight, "highlight" },
    { Annotation::Type::Underline, "underline" },
    { Annotation::Type::StrikeOut, "strikeout" },
    { Annotation::Type::Squiggly, "squiggly" },
    { Annotation::Type::Ink, "ink" },
    { Annotation::Type::Square, "square" },
    { Annotation::Type::Circle, "circle" },
    { Annotation::Type::Line, "line" },
    { Annotation::Type::FreeText, "freetext" },
    { Annotation::Type::Note, "text" },
};

QString elementFor(Annotation::Type type)
{
    for (const ElementName &entry : elementNames) {
        if (entry.type == type) {
            return QString::fromLatin1(entry.element);
        }
    }
    return QStringLiteral("square");
}

bool typeFromElement(const QString &element, Annotation::Type *type)
{
    for (const ElementName &entry : elementNames) {
        if (element.compare(QLatin1StringView(entry.element), Qt::CaseInsensitive) == 0) {
            *type = entry.type;
            return true;
        }
    }
    return false;
}

/** XFDF writes numbers plainly; QString::number is locale-independent. */
QString fourDecimals(double value)
{
    return QString::number(value, 'f', 4);
}

QString rectAttribute(const QRectF &rect)
{
    const QRectF normalised = rect.normalized();
    return fourDecimals(normalised.left()) + QLatin1Char(',') + fourDecimals(normalised.top()) + QLatin1Char(',')
        + fourDecimals(normalised.right()) + QLatin1Char(',') + fourDecimals(normalised.bottom());
}

/** "l,b,r,t" back into a rectangle, or a null one when it is malformed. */
QRectF rectFromAttribute(const QString &text)
{
    const QStringList parts = text.split(QLatin1Char(','), Qt::SkipEmptyParts);
    if (parts.size() != 4) {
        return {};
    }
    bool ok = true;
    double values[4];
    for (int i = 0; i < 4 && ok; ++i) {
        values[i] = parts.at(i).trimmed().toDouble(&ok);
    }
    if (!ok) {
        return {};
    }
    return QRectF(std::min(values[0], values[2]), std::min(values[1], values[3]), std::abs(values[2] - values[0]),
                  std::abs(values[3] - values[1]));
}

/** The transform for a page of a document opened only to be measured. */
QTransform transformForPage(QPDF &pdf, int page)
{
    QPDFPageDocumentHelper documents(pdf);
    std::vector<QPDFPageObjectHelper> pages = documents.getAllPages();
    if (page < 0 || page >= int(pages.size())) {
        return {};
    }
    QPDFPageObjectHelper handle = pages.at(size_t(page));
    return pageTransformFor(handle);
}

} // namespace

bool Annotations::exportXfdf(const QString &pdf, const QString &xfdfPath, const QVector<int> &pages,
                             const QString &sourceName, QString *error)
{
    QString readError;
    const QVector<Annotation> found = read(pdf, pages, &readError);
    if (!readError.isEmpty()) {
        if (error) {
            *error = readError;
        }
        return false;
    }
    if (found.isEmpty()) {
        if (error) {
            *error = i18n("This document has no comments to write out.");
        }
        return false;
    }

    // Comments come back in display coordinates; XFDF is in the page's own
    // space, which is what every other reader expects to find there.
    QHash<int, QTransform> transforms;
    try {
        QPDF document;
        PdfFile::open(document, pdf);
        for (const Annotation &annotation : found) {
            if (!transforms.contains(annotation.page)) {
                transforms.insert(annotation.page, transformForPage(document, annotation.page));
            }
        }
    } catch (const std::exception &e) {
        if (error) {
            *error = QString::fromUtf8(e.what());
        }
        return false;
    }

    QSaveFile file(xfdfPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (error) {
            *error = i18n("“%1” could not be opened for writing.", xfdfPath);
        }
        return false;
    }

    QXmlStreamWriter xml(&file);
    xml.setAutoFormatting(true);
    xml.writeStartDocument();
    xml.writeStartElement(QStringLiteral("xfdf"));
    xml.writeDefaultNamespace(QStringLiteral("http://ns.adobe.com/xfdf/"));
    xml.writeAttribute(QStringLiteral("xml:space"), QStringLiteral("preserve"));

    xml.writeStartElement(QStringLiteral("annots"));
    for (const Annotation &annotation : found) {
        const QTransform toPage = transforms.value(annotation.page);

        xml.writeStartElement(elementFor(annotation.type));
        xml.writeAttribute(QStringLiteral("page"), QString::number(annotation.page));
        xml.writeAttribute(QStringLiteral("rect"), rectAttribute(toPage.mapRect(annotation.rect)));
        xml.writeAttribute(QStringLiteral("color"), annotation.colour.name(QColor::HexRgb).toUpper());
        xml.writeAttribute(QStringLiteral("opacity"), fourDecimals(annotation.opacity));
        xml.writeAttribute(QStringLiteral("width"), fourDecimals(annotation.lineWidth));
        if (!annotation.author.isEmpty()) {
            xml.writeAttribute(QStringLiteral("title"), annotation.author);
        }
        if (!annotation.identifier.isEmpty()) {
            xml.writeAttribute(QStringLiteral("name"), annotation.identifier);
        }
        if (annotation.created.isValid()) {
            xml.writeAttribute(QStringLiteral("creationdate"), PdfFile::formatDate(annotation.created));
        }
        if (annotation.interior.isValid()) {
            xml.writeAttribute(QStringLiteral("interior-color"), annotation.interior.name(QColor::HexRgb).toUpper());
        }

        if (!annotation.quads.isEmpty()) {
            // XFDF lists the four corners of every quad in one attribute.
            QStringList numbers;
            for (const QRectF &quad : annotation.quads) {
                const QRectF mapped = toPage.mapRect(quad).normalized();
                for (const QPointF &corner :
                     { mapped.topLeft(), mapped.topRight(), mapped.bottomLeft(), mapped.bottomRight() }) {
                    numbers << fourDecimals(corner.x()) << fourDecimals(corner.y());
                }
            }
            xml.writeAttribute(QStringLiteral("coords"), numbers.join(QLatin1Char(',')));
        }

        if (!annotation.contents.isEmpty()) {
            xml.writeTextElement(QStringLiteral("contents"), annotation.contents);
        }

        if (!annotation.strokes.isEmpty()) {
            xml.writeStartElement(QStringLiteral("inklist"));
            for (const QVector<QPointF> &stroke : annotation.strokes) {
                QStringList points;
                for (const QPointF &point : stroke) {
                    const QPointF mapped = toPage.map(point);
                    points << fourDecimals(mapped.x()) + QLatin1Char(',') + fourDecimals(mapped.y());
                }
                xml.writeTextElement(QStringLiteral("gesture"), points.join(QLatin1Char(';')));
            }
            xml.writeEndElement();
        }

        xml.writeEndElement();
    }
    xml.writeEndElement(); // annots

    if (!sourceName.isEmpty()) {
        xml.writeStartElement(QStringLiteral("f"));
        xml.writeAttribute(QStringLiteral("href"), sourceName);
        xml.writeEndElement();
    }

    xml.writeEndElement(); // xfdf
    xml.writeEndDocument();

    if (!file.commit()) {
        if (error) {
            *error = i18n("“%1” could not be written.", xfdfPath);
        }
        return false;
    }
    return true;
}

bool Annotations::importXfdf(const QString &inputPdf, const QString &xfdfPath, const QString &outputPdf, int *added,
                             QStringList *warnings, QString *error)
{
    QFile file(xfdfPath);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) {
            *error = i18n("“%1” could not be read.", xfdfPath);
        }
        return false;
    }

    int pageCount = 0;
    QHash<int, QTransform> toDisplay;
    try {
        QPDF document;
        PdfFile::open(document, inputPdf);
        QPDFPageDocumentHelper documents(document);
        pageCount = int(documents.getAllPages().size());
        for (int page = 0; page < pageCount; ++page) {
            bool invertible = false;
            const QTransform inverse = transformForPage(document, page).inverted(&invertible);
            if (invertible) {
                toDisplay.insert(page, inverse);
            }
        }
    } catch (const std::exception &e) {
        if (error) {
            *error = QString::fromUtf8(e.what());
        }
        return false;
    }

    QVector<Annotation> incoming;
    int skipped = 0;

    QXmlStreamReader xml(&file);
    while (!xml.atEnd()) {
        if (xml.readNext() != QXmlStreamReader::StartElement) {
            continue;
        }

        Annotation::Type type;
        if (!typeFromElement(xml.name().toString(), &type)) {
            continue;
        }

        Annotation annotation;
        annotation.type = type;
        const QXmlStreamAttributes attributes = xml.attributes();
        annotation.page = attributes.value(QStringLiteral("page")).toInt();

        if (annotation.page < 0 || annotation.page >= pageCount) {
            ++skipped;
            xml.skipCurrentElement();
            continue;
        }
        const QTransform inverse = toDisplay.value(annotation.page);

        annotation.rect = inverse.mapRect(rectFromAttribute(attributes.value(QStringLiteral("rect")).toString()));
        const QColor colour(attributes.value(QStringLiteral("color")).toString());
        if (colour.isValid()) {
            annotation.colour = colour;
        }
        const QColor interior(attributes.value(QStringLiteral("interior-color")).toString());
        if (interior.isValid()) {
            annotation.interior = interior;
        }
        if (attributes.hasAttribute(QStringLiteral("opacity"))) {
            annotation.opacity = qBound(0.0, attributes.value(QStringLiteral("opacity")).toDouble(), 1.0);
        }
        if (attributes.hasAttribute(QStringLiteral("width"))) {
            annotation.lineWidth = qMax(0.1, attributes.value(QStringLiteral("width")).toDouble());
        }
        annotation.author = attributes.value(QStringLiteral("title")).toString();
        annotation.identifier = attributes.value(QStringLiteral("name")).toString();
        annotation.created = PdfFile::parseDate(attributes.value(QStringLiteral("creationdate")).toString());

        const QString coords = attributes.value(QStringLiteral("coords")).toString();
        if (!coords.isEmpty()) {
            const QStringList numbers = coords.split(QLatin1Char(','), Qt::SkipEmptyParts);
            for (int i = 0; i + 7 < numbers.size(); i += 8) {
                double xs[4];
                double ys[4];
                bool ok = true;
                for (int corner = 0; corner < 4 && ok; ++corner) {
                    xs[corner] = numbers.at(i + corner * 2).toDouble(&ok);
                    if (ok) {
                        ys[corner] = numbers.at(i + corner * 2 + 1).toDouble(&ok);
                    }
                }
                if (!ok) {
                    continue;
                }
                const double left = *std::min_element(xs, xs + 4);
                const double right = *std::max_element(xs, xs + 4);
                const double bottom = *std::min_element(ys, ys + 4);
                const double top = *std::max_element(ys, ys + 4);
                annotation.quads.append(inverse.mapRect(QRectF(left, bottom, right - left, top - bottom)));
            }
        }

        // The children: the text, and any freehand strokes.
        while (xml.readNextStartElement()) {
            if (xml.name() == QLatin1String("contents")) {
                annotation.contents = xml.readElementText();
            } else if (xml.name() == QLatin1String("inklist")) {
                while (xml.readNextStartElement()) {
                    if (xml.name() != QLatin1String("gesture")) {
                        xml.skipCurrentElement();
                        continue;
                    }
                    QVector<QPointF> stroke;
                    const QStringList points = xml.readElementText().split(QLatin1Char(';'), Qt::SkipEmptyParts);
                    for (const QString &point : points) {
                        const QStringList pair = point.split(QLatin1Char(','), Qt::SkipEmptyParts);
                        if (pair.size() != 2) {
                            continue;
                        }
                        bool ok = true;
                        const double x = pair.at(0).toDouble(&ok);
                        const double y = ok ? pair.at(1).toDouble(&ok) : 0.0;
                        if (ok) {
                            stroke.append(inverse.map(QPointF(x, y)));
                        }
                    }
                    if (!stroke.isEmpty()) {
                        annotation.strokes.append(stroke);
                    }
                }
            } else {
                xml.skipCurrentElement();
            }
        }

        incoming.append(annotation);
    }

    if (xml.hasError()) {
        if (error) {
            *error = i18n("“%1” is not a valid XFDF file: %2", xfdfPath, xml.errorString());
        }
        return false;
    }
    if (incoming.isEmpty()) {
        if (error) {
            *error = i18n("“%1” holds no comments this can use.", xfdfPath);
        }
        return false;
    }

    if (warnings && skipped > 0) {
        *warnings << i18np("One comment names a page this document does not have and was left out.",
                           "%1 comments name pages this document does not have and were left out.", skipped);
    }
    if (added) {
        *added = incoming.size();
    }
    return add(inputPdf, outputPdf, incoming, error);
}

} // namespace ps
