/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#include "PageObjects.h"

#include "Core14Widths.h"
#include "PdfFile.h"
#include "PdfGeometry.h"
#include "PdfImage.h"

#include <KLocalizedString>

#include <QHash>
#include <QSet>

#include <qpdf/Pl_Buffer.hh>
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>
#include <qpdf/QPDFTokenizer.hh>
#include <qpdf/QPDFWriter.hh>

#include <cmath>

using namespace Qt::Literals::StringLiterals;

namespace ps {

namespace {

using PdfGeometry::number;

/** Graphics state, as far as knowing where and how something is drawn needs it. */
struct State {
    QTransform ctm;
    QColor fill = QColor(Qt::black);
    QColor stroke = QColor(Qt::black);
    double lineWidth = 1.0;

    /** In page space. A null rectangle means nothing is clipped away. */
    QRectF clip;

    QString fontName;
    double fontSize = 0.0;
    double charSpacing = 0.0;
    double wordSpacing = 0.0;
    double horizScale = 1.0;
    double leading = 0.0;
    double rise = 0.0;
};

/** The operators that actually put ink on the page. */
bool isPainting(const QString &op)
{
    static const QSet<QString> painting { u"S"_s,  u"s"_s,  u"f"_s,  u"F"_s, u"f*"_s, u"B"_s,  u"B*"_s, u"b"_s,
                                          u"b*"_s, u"Tj"_s, u"TJ"_s, u"'"_s, u"\""_s, u"Do"_s, u"sh"_s };
    return painting.contains(op);
}

/** Path construction, which is buffered so a whole shape can be wrapped at once. */
bool isPathBuilding(const QString &op)
{
    static const QSet<QString> building { u"m"_s, u"l"_s, u"c"_s, u"v"_s, u"y"_s, u"h"_s, u"re"_s };
    return building.contains(op);
}

/** The six numbers of a matrix, in the order an operator takes them. */
std::string matrix(const QTransform &transform)
{
    return number(transform.m11()) + " " + number(transform.m12()) + " " + number(transform.m21()) + " "
        + number(transform.m22()) + " " + number(transform.dx()) + " " + number(transform.dy());
}

/** A colour from however many components the operator carried. */
QColor colourFrom(const QVector<double> &values)
{
    if (values.size() == 1) {
        const int grey = qBound(0, int(std::lround(values.at(0) * 255.0)), 255);
        return QColor(grey, grey, grey);
    }
    if (values.size() == 3) {
        return QColor::fromRgbF(qBound(0.0, values.at(0), 1.0), qBound(0.0, values.at(1), 1.0),
                                qBound(0.0, values.at(2), 1.0));
    }
    if (values.size() == 4) {
        // Naïve CMYK, knowingly so: this is for the swatch beside a selected
        // object, not for anything that goes on a press.
        const double k = qBound(0.0, values.at(3), 1.0);
        return QColor::fromRgbF((1.0 - qBound(0.0, values.at(0), 1.0)) * (1.0 - k),
                                (1.0 - qBound(0.0, values.at(1), 1.0)) * (1.0 - k),
                                (1.0 - qBound(0.0, values.at(2), 1.0)) * (1.0 - k));
    }
    return {};
}

/**
 * Walks a page and either records what it finds or rewrites it.
 *
 * One filter for both jobs on purpose. Reading and writing have to agree exactly
 * on which painting operator is the eleventh on the page; two separate walkers
 * would eventually disagree, and the symptom would be the wrong object moving,
 * which looks like a coordinate bug and is not one.
 */
class ObjectFilter : public QPDFObjectHandle::TokenFilter
{
public:
    ObjectFilter(QPDFObjectHandle resources, const QTransform &toDisplay, int page)
        : m_resources(std::move(resources))
        , m_toDisplay(toDisplay)
        , m_page(page)
    {
    }

    void setReading(bool reading) { m_reading = reading; }
    void setEdits(const QHash<int, PageEdit> &edits) { m_edits = edits; }

    void handleToken(QPDFTokenizer::Token const &token) override;

    QVector<PageObject> objects() const { return m_objects; }
    int transformed() const { return m_transformed; }
    int deleted() const { return m_deleted; }
    int restyled() const { return m_restyled; }
    QStringList refusals() const { return m_refusals; }

private:
    void handleOperator(const QString &op);
    void finishObject(const QString &op);
    void writeBuffered();
    void nextLine();

    double operandNumber(int index) const;
    QVector<double> operandNumbers() const;
    QTransform operandMatrix() const;
    QRectF boundsOfText(QString *shown, double *advance);
    QRectF applyClip(const QRectF &box) const;

    QPDFObjectHandle m_resources;
    QTransform m_toDisplay;
    int m_page = 0;

    bool m_reading = true;
    QHash<int, PageEdit> m_edits;

    QVector<QPDFTokenizer::Token> m_operands;

    /**
     * The operators belonging to the object being assembled.
     *
     * A shape is `m l l … f`. To wrap it in a matrix the whole run has to be held
     * back, because the wrapping goes in front of the first `m` and not in front
     * of the `f`.
     */
    struct Buffered {
        QVector<QPDFTokenizer::Token> operands;
        QString op;
    };
    QVector<Buffered> m_buffer;

    QPainterPath m_path;
    QPointF m_pathStart;
    QPointF m_pathCurrent;
    bool m_clipPending = false;

    State m_state;
    QVector<State> m_stack;
    QTransform m_textMatrix;
    QTransform m_lineMatrix;

    /** Per font resource, the advance widths, worked out once. */
    QHash<QString, QHash<int, double>> m_widths;
    QHash<QString, double> m_fallbackWidth;

    int m_index = -1;
    QVector<PageObject> m_objects;
    int m_transformed = 0;
    int m_deleted = 0;
    int m_restyled = 0;
    QStringList m_refusals;
};

double ObjectFilter::operandNumber(int index) const
{
    if (index < 0 || index >= m_operands.size()) {
        return 0.0;
    }
    return QString::fromStdString(m_operands.at(index).getValue()).toDouble();
}

QVector<double> ObjectFilter::operandNumbers() const
{
    QVector<double> values;
    for (const QPDFTokenizer::Token &token : m_operands) {
        const auto type = token.getType();
        if (type == QPDFTokenizer::tt_integer || type == QPDFTokenizer::tt_real) {
            values.append(QString::fromStdString(token.getValue()).toDouble());
        }
    }
    return values;
}

QTransform ObjectFilter::operandMatrix() const
{
    return QTransform(operandNumber(0), operandNumber(1), operandNumber(2), operandNumber(3), operandNumber(4),
                      operandNumber(5));
}

QRectF ObjectFilter::applyClip(const QRectF &box) const
{
    if (m_state.clip.isNull()) {
        return box;
    }
    // A boundary bigger than the visible part would give a selection rectangle
    // reaching beyond anything a person can see.
    return box.intersected(m_toDisplay.mapRect(m_state.clip));
}

QRectF ObjectFilter::boundsOfText(QString *shown, double *advance)
{
    if (!m_widths.contains(m_state.fontName)) {
        QHash<int, double> table;
        double fallback = 0.5;

        QPDFObjectHandle fonts = m_resources.isDictionary() ? m_resources.getKey("/Font") : QPDFObjectHandle::newNull();
        QPDFObjectHandle font
            = fonts.isDictionary() ? fonts.getKey(m_state.fontName.toStdString()) : QPDFObjectHandle::newNull();
        if (font.isDictionary()) {
            QPDFObjectHandle widths = font.getKey("/Widths");
            QPDFObjectHandle firstChar = font.getKey("/FirstChar");
            if (widths.isArray() && firstChar.isInteger()) {
                const int first = firstChar.getIntValueAsInt();
                for (int i = 0; i < widths.getArrayNItems(); ++i) {
                    table.insert(first + i, PdfGeometry::numberAt(widths, i, 0.0) / 1000.0);
                }
            }
        }
        if (table.isEmpty()) {
            // Helvetica's metrics, which is what a reader substitutes anyway when
            // a document names a standard font without declaring its widths.
            for (const Core14::Entry &entry : Core14::table) {
                if (QLatin1StringView(entry.name) == u"Helvetica"_s) {
                    for (int code = Core14::firstCode; code <= Core14::lastCode; ++code) {
                        table.insert(code, entry.widths[code - Core14::firstCode] / 1000.0);
                    }
                    fallback = entry.averageWidth / 1000.0;
                    break;
                }
            }
        }
        m_widths.insert(m_state.fontName, table);
        m_fallbackWidth.insert(m_state.fontName, fallback);
    }

    const QHash<int, double> &widths = m_widths[m_state.fontName];
    const double fallback = m_fallbackWidth.value(m_state.fontName, 0.5);

    double moved = 0.0;
    QString text;
    for (const QPDFTokenizer::Token &token : m_operands) {
        if (token.getType() == QPDFTokenizer::tt_string) {
            for (char byte : token.getValue()) {
                const int code = static_cast<unsigned char>(byte);
                text.append(code >= 32 && code < 127 ? QChar(ushort(code)) : QChar(u'·'));
                moved += (widths.value(code, fallback) * m_state.fontSize + m_state.charSpacing
                          + (code == 32 ? m_state.wordSpacing : 0.0))
                    * m_state.horizScale;
            }
        } else if (token.getType() == QPDFTokenizer::tt_integer || token.getType() == QPDFTokenizer::tt_real) {
            moved
                -= QString::fromStdString(token.getValue()).toDouble() / 1000.0 * m_state.fontSize * m_state.horizScale;
        }
    }

    if (shown) {
        *shown = text;
    }
    if (advance) {
        *advance = moved;
    }
    const QRectF inTextSpace(0.0, m_state.rise - 0.25 * m_state.fontSize, moved, 1.2 * m_state.fontSize);
    return (m_textMatrix * m_state.ctm).mapRect(inTextSpace);
}

void ObjectFilter::nextLine()
{
    m_lineMatrix = QTransform(1, 0, 0, 1, 0, -m_state.leading) * m_lineMatrix;
    m_textMatrix = m_lineMatrix;
}

void ObjectFilter::writeBuffered()
{
    for (const Buffered &entry : std::as_const(m_buffer)) {
        for (const QPDFTokenizer::Token &token : entry.operands) {
            writeToken(token);
            write(" ");
        }
        write(entry.op.toStdString());
        write("\n");
    }
}

void ObjectFilter::handleToken(QPDFTokenizer::Token const &token)
{
    switch (token.getType()) {
    case QPDFTokenizer::tt_space:
    case QPDFTokenizer::tt_comment:
        if (m_operands.isEmpty() && m_buffer.isEmpty() && !m_reading) {
            writeToken(token);
        }
        return;

    case QPDFTokenizer::tt_word:
        handleOperator(QString::fromStdString(token.getValue()));
        m_operands.clear();
        return;

    case QPDFTokenizer::tt_inline_image: {
        ++m_index;
        if (m_reading) {
            PageObject object;
            object.index = m_index;
            object.page = m_page;
            object.kind = PageObject::Kind::InlineImage;
            object.placement = m_state.ctm;
            object.bounds = applyClip(m_toDisplay.mapRect(m_state.ctm.mapRect(QRectF(0, 0, 1, 1))));
            object.layer = int(m_objects.size());
            m_objects.append(object);
        } else {
            const auto edit = m_edits.constFind(m_index);
            if (edit != m_edits.constEnd() && edit->kind == PageEdit::Kind::Delete) {
                ++m_deleted;
            } else {
                writeToken(token);
            }
        }
        m_operands.clear();
        return;
    }

    case QPDFTokenizer::tt_eof:
        return;

    default:
        m_operands.append(token);
        return;
    }
}

void ObjectFilter::handleOperator(const QString &op)
{
    // ── graphics and text state ───────────────────────────────────────────
    if (op == u"q"_s) {
        m_stack.append(m_state);
    } else if (op == u"Q"_s) {
        if (!m_stack.isEmpty()) {
            m_state = m_stack.takeLast();
        }
    } else if (op == u"cm"_s && m_operands.size() >= 6) {
        m_state.ctm = operandMatrix() * m_state.ctm;
    } else if (op == u"w"_s) {
        m_state.lineWidth = operandNumber(0);
    } else if (op == u"g"_s || op == u"rg"_s || op == u"k"_s || op == u"sc"_s || op == u"scn"_s) {
        const QColor colour = colourFrom(operandNumbers());
        if (colour.isValid()) {
            m_state.fill = colour;
        }
    } else if (op == u"G"_s || op == u"RG"_s || op == u"K"_s || op == u"SC"_s || op == u"SCN"_s) {
        const QColor colour = colourFrom(operandNumbers());
        if (colour.isValid()) {
            m_state.stroke = colour;
        }
    } else if (op == u"W"_s || op == u"W*"_s) {
        m_clipPending = true;
    } else if (op == u"BT"_s) {
        m_textMatrix = QTransform();
        m_lineMatrix = QTransform();
    } else if (op == u"Tf"_s && m_operands.size() >= 2) {
        m_state.fontName = QString::fromStdString(m_operands.at(0).getValue());
        m_state.fontSize = operandNumber(1);
    } else if (op == u"Tm"_s && m_operands.size() >= 6) {
        m_lineMatrix = operandMatrix();
        m_textMatrix = m_lineMatrix;
    } else if ((op == u"Td"_s || op == u"TD"_s) && m_operands.size() >= 2) {
        if (op == u"TD"_s) {
            m_state.leading = -operandNumber(1);
        }
        m_lineMatrix = QTransform(1, 0, 0, 1, operandNumber(0), operandNumber(1)) * m_lineMatrix;
        m_textMatrix = m_lineMatrix;
    } else if (op == u"T*"_s) {
        nextLine();
    } else if (op == u"TL"_s) {
        m_state.leading = operandNumber(0);
    } else if (op == u"Tc"_s) {
        m_state.charSpacing = operandNumber(0);
    } else if (op == u"Tw"_s) {
        m_state.wordSpacing = operandNumber(0);
    } else if (op == u"Tz"_s) {
        m_state.horizScale = operandNumber(0) / 100.0;
    } else if (op == u"Ts"_s) {
        m_state.rise = operandNumber(0);
    }

    // ── path construction, held until its painting operator arrives ───────
    if (isPathBuilding(op)) {
        if (op == u"m"_s) {
            m_pathCurrent = QPointF(operandNumber(0), operandNumber(1));
            m_pathStart = m_pathCurrent;
            m_path.moveTo(m_pathCurrent);
        } else if (op == u"l"_s) {
            m_pathCurrent = QPointF(operandNumber(0), operandNumber(1));
            m_path.lineTo(m_pathCurrent);
        } else if (op == u"c"_s) {
            m_pathCurrent = QPointF(operandNumber(4), operandNumber(5));
            m_path.cubicTo(QPointF(operandNumber(0), operandNumber(1)), QPointF(operandNumber(2), operandNumber(3)),
                           m_pathCurrent);
        } else if (op == u"v"_s) {
            m_pathCurrent = QPointF(operandNumber(2), operandNumber(3));
            m_path.cubicTo(m_pathCurrent, QPointF(operandNumber(0), operandNumber(1)), m_pathCurrent);
        } else if (op == u"y"_s) {
            const QPointF end(operandNumber(2), operandNumber(3));
            m_path.cubicTo(QPointF(operandNumber(0), operandNumber(1)), end, end);
            m_pathCurrent = end;
        } else if (op == u"h"_s) {
            m_path.closeSubpath();
            m_pathCurrent = m_pathStart;
        } else if (op == u"re"_s) {
            m_path.addRect(QRectF(operandNumber(0), operandNumber(1), operandNumber(2), operandNumber(3)));
            m_pathStart = QPointF(operandNumber(0), operandNumber(1));
            m_pathCurrent = m_pathStart;
        }
        m_buffer.append({ m_operands, op });
        return;
    }

    if (isPainting(op)) {
        finishObject(op);
        return;
    }

    // A clip set by `W n`: remember the region, then forget the path, which drew
    // nothing, so it is no object.
    if (op == u"n"_s) {
        if (m_clipPending && !m_path.isEmpty()) {
            const QRectF region = m_state.ctm.mapRect(m_path.boundingRect());
            m_state.clip = m_state.clip.isNull() ? region : m_state.clip.intersected(region);
        }
        m_clipPending = false;
    }

    if (!m_reading) {
        writeBuffered();
        for (const QPDFTokenizer::Token &token : std::as_const(m_operands)) {
            writeToken(token);
            write(" ");
        }
        write(op.toStdString());
        write("\n");
    }
    m_buffer.clear();
    if (op == u"n"_s) {
        m_path = QPainterPath();
    }
}

void ObjectFilter::finishObject(const QString &op)
{
    ++m_index;

    PageObject object;
    object.index = m_index;
    object.page = m_page;
    object.placement = m_state.ctm;
    object.fill = m_state.fill;
    object.stroke = m_state.stroke;
    object.lineWidth = m_state.lineWidth;
    object.layer = int(m_objects.size());

    double advance = 0.0;

    if (op == u"Tj"_s || op == u"TJ"_s || op == u"'"_s || op == u"\""_s) {
        if (op == u"'"_s || op == u"\""_s) {
            if (op == u"\""_s && m_operands.size() >= 3) {
                m_state.wordSpacing = operandNumber(0);
                m_state.charSpacing = operandNumber(1);
            }
            nextLine();
        }
        object.kind = PageObject::Kind::Text;
        object.fontResource = m_state.fontName;
        object.fontSize = m_state.fontSize;
        QString shown;
        object.bounds = applyClip(m_toDisplay.mapRect(boundsOfText(&shown, &advance)));
        object.text = shown;
    } else if (op == u"Do"_s) {
        const std::string name = m_operands.isEmpty() ? std::string() : m_operands.constLast().getValue();
        object.resourceName = QString::fromStdString(name);

        QPDFObjectHandle xobjects
            = m_resources.isDictionary() ? m_resources.getKey("/XObject") : QPDFObjectHandle::newNull();
        QPDFObjectHandle xobject = xobjects.isDictionary() ? xobjects.getKey(name) : QPDFObjectHandle::newNull();
        const std::string subtype = xobject.isStream() && xobject.getDict().getKey("/Subtype").isName()
            ? xobject.getDict().getKey("/Subtype").getName()
            : std::string();

        if (subtype == "/Image") {
            object.kind = PageObject::Kind::Image;
            QPDFObjectHandle dict = xobject.getDict();
            object.pixelSize
                = QSize(dict.getKey("/Width").isInteger() ? dict.getKey("/Width").getIntValueAsInt() : 0,
                        dict.getKey("/Height").isInteger() ? dict.getKey("/Height").getIntValueAsInt() : 0);
        } else {
            object.kind = PageObject::Kind::Drawing;
        }
        // An image and a form XObject are both drawn in the unit square.
        object.bounds = applyClip(m_toDisplay.mapRect(m_state.ctm.mapRect(QRectF(0, 0, 1, 1))));
    } else if (op == u"sh"_s) {
        object.kind = PageObject::Kind::Shading;
        // A shading fills the clip, so without one there is nothing to bound.
        object.bounds = m_state.clip.isNull() ? QRectF() : m_toDisplay.mapRect(m_state.clip);
    } else {
        object.kind = PageObject::Kind::Path;
        object.outline = (m_state.ctm * m_toDisplay).map(m_path);
        const double half = m_state.lineWidth / 2.0;
        object.bounds = applyClip(object.outline.boundingRect().adjusted(-half, -half, half, half));
        // A stroke-only shape has no interior; calling it black would make the
        // properties panel lie.
        if (op == u"S"_s || op == u"s"_s) {
            object.fill = QColor();
        }
    }

    if (m_reading) {
        m_objects.append(object);
        if (object.kind == PageObject::Kind::Text) {
            m_textMatrix = QTransform(1, 0, 0, 1, advance, 0) * m_textMatrix;
        }
        m_buffer.clear();
        m_path = QPainterPath();
        return;
    }

    // ── writing ───────────────────────────────────────────────────────────
    const auto edit = m_edits.constFind(m_index);
    const bool wanted = edit != m_edits.constEnd();

    if (wanted && edit->kind == PageEdit::Kind::Delete) {
        ++m_deleted;
        m_buffer.clear();
        m_path = QPainterPath();
        if (object.kind == PageObject::Kind::Text) {
            // The text matrix still advances, or whatever follows on the line
            // slides left into the gap.
            m_textMatrix = QTransform(1, 0, 0, 1, advance, 0) * m_textMatrix;
        }
        return;
    }

    std::string prefix;
    std::string suffix;

    if (wanted && edit->kind == PageEdit::Kind::Transform) {
        bool invertible = false;
        const QTransform inverse = m_state.ctm.inverted(&invertible);
        if (!invertible) {
            m_refusals << i18n("One object could not be moved: its placement cannot be reversed arithmetically.");
        } else {
            // The transform arrives in display space: that is where the
            // boundaries this hands out are measured, and where the hand that
            // made the gesture was pointing. On a sheet with a /Rotate the two
            // spaces are a quarter turn apart, so it is carried into page space
            // before anything else is done with it. Otherwise "to the right on
            // screen" is written down as "up the page", which is how a picture
            // dragged on a turned scan ends up somewhere nobody pointed at.
            const QTransform inPage = m_toDisplay * edit->transform * m_toDisplay.inverted();

            // Inside the stream a matrix acts *before* the one already in force,
            // so it is conjugated by the current matrix as well: what results is
            // CTM × T, the movement that was actually asked for.
            const QTransform inStream = m_state.ctm * inPage * inverse;
            prefix = "q " + matrix(inStream) + " cm\n";
            suffix = "Q\n";
            ++m_transformed;
        }
    } else if (wanted && edit->kind == PageEdit::Kind::Restyle) {
        std::string style;
        if (edit->fill.isValid()) {
            style += number(edit->fill.redF()) + " " + number(edit->fill.greenF()) + " " + number(edit->fill.blueF())
                + " rg\n";
        }
        if (edit->stroke.isValid()) {
            style += number(edit->stroke.redF()) + " " + number(edit->stroke.greenF()) + " "
                + number(edit->stroke.blueF()) + " RG\n";
        }
        if (edit->lineWidth >= 0.0) {
            style += number(edit->lineWidth) + " w\n";
        }
        if (!style.empty()) {
            prefix = "q " + style;
            suffix = "Q\n";
            ++m_restyled;
        }
    }

    write(prefix);
    writeBuffered();
    for (const QPDFTokenizer::Token &token : std::as_const(m_operands)) {
        writeToken(token);
        write(" ");
    }
    write(op.toStdString());
    write("\n");
    write(suffix);

    if (object.kind == PageObject::Kind::Text) {
        m_textMatrix = QTransform(1, 0, 0, 1, advance, 0) * m_textMatrix;
    }
    m_buffer.clear();
    m_path = QPainterPath();
}

/** Display points to page space, /Rotate and a shifted media box included. */
QTransform pageTransformFor(QPDFPageObjectHelper &page)
{
    const QRectF media = PdfGeometry::mediaBoxOf(page);
    return PdfGeometry::displayToPageTransform(PdfGeometry::rotationOf(page), media.width(), media.height())
        * QTransform::fromTranslate(media.x(), media.y());
}

/**
 * The content stream for one insertion, written in display space.
 *
 * Everything below is drawn in the box the insertion was measured in, and one
 * matrix at the top carries the whole of it onto the page: how the thing has
 * been turned, and the turn the page itself carries in its /Rotate. Both at once
 * rather than one and then the other, because they compose: a picture put down
 * upright on a sheet the reader has turned is upright *to the reader*, and a box
 * mapped corner by corner onto page space instead would land in the right place
 * lying on its side.
 *
 * An upright box on an untouched sheet leaves the identity there and writes
 * nothing, which is why the ordinary case comes out byte for byte as it always
 * did.
 */
std::string streamFor(QPDF &pdf, const NewContent &item, const QTransform &toPage, QPDFObjectHandle resources,
                      QStringList *refusals)
{
    const QRectF rect = item.rect.normalized();
    std::string content = "q\n";

    const QTransform onPage = item.placement * toPage;
    if (!onPage.isIdentity()) {
        content += matrix(onPage) + " cm\n";
    }

    if (item.opacity < 1.0) {
        QPDFObjectHandle states = resources.getKey("/ExtGState");
        if (!states.isDictionary()) {
            states = QPDFObjectHandle::newDictionary();
            resources.replaceKey("/ExtGState", states);
        }
        int suffix = 1;
        const std::string name = states.getUniqueResourceName("/PsGs", suffix);
        states.replaceKey(name,
                          QPDFObjectHandle::parse("<< /Type /ExtGState /CA " + number(item.opacity) + " /ca "
                                                  + number(item.opacity) + " >>"));
        content += name + " gs\n";
    }

    switch (item.kind) {
    case NewContent::Kind::Text: {
        QPDFObjectHandle fonts = resources.getKey("/Font");
        if (!fonts.isDictionary()) {
            fonts = QPDFObjectHandle::newDictionary();
            resources.replaceKey("/Font", fonts);
        }
        int suffix = 1;
        const std::string name = fonts.getUniqueResourceName("/PsF", suffix);
        fonts.replaceKey(name,
                         pdf.makeIndirectObject(QPDFObjectHandle::parse("<< /Type /Font /Subtype /Type1 /BaseFont /"
                                                                        + item.fontFamily.toStdString()
                                                                        + " /Encoding /WinAnsiEncoding >>")));

        const QColor colour = item.fill.isValid() ? item.fill : QColor(Qt::black);
        content += number(colour.redF()) + " " + number(colour.greenF()) + " " + number(colour.blueF()) + " rg\n";
        content += "BT\n" + name + " " + number(item.fontSize) + " Tf\n";

        // One line per line, placed absolutely. Wrapping is the typesetting
        // engine's job, not a single inserted label's.
        double y = rect.bottom() - item.fontSize;
        for (const QString &line : item.text.split(u'\n')) {
            content += "1 0 0 1 " + number(rect.left()) + " " + number(y) + " Tm\n(";
            for (const QChar &character : line) {
                const ushort code = character.unicode();
                if (code == '(' || code == ')' || code == '\\') {
                    content += '\\';
                }
                if (code < 256) {
                    content += char(code);
                } else if (refusals) {
                    *refusals << i18n("“%1” cannot be written with a standard font and was left out.",
                                      QString(character));
                }
            }
            content += ") Tj\n";
            y -= item.fontSize * 1.35;
        }
        content += "ET\n";
        break;
    }

    case NewContent::Kind::Image: {
        QPDFObjectHandle embedded = item.jpegQuality >= 0 ? PdfImage::embedAsJpeg(pdf, item.image, item.jpegQuality)
                                                          : PdfImage::embed(pdf, item.image);
        if (!embedded.isStream()) {
            if (refusals) {
                *refusals << i18n("A picture could not be embedded.");
            }
            return {};
        }
        QPDFObjectHandle xobjects = resources.getKey("/XObject");
        if (!xobjects.isDictionary()) {
            xobjects = QPDFObjectHandle::newDictionary();
            resources.replaceKey("/XObject", xobjects);
        }
        int suffix = 1;
        const std::string name = xobjects.getUniqueResourceName("/PsIm", suffix);
        xobjects.replaceKey(name, embedded);
        content += number(rect.width()) + " 0 0 " + number(rect.height()) + " " + number(rect.left()) + " "
            + number(rect.top()) + " cm\n";
        content += name + " Do\n";
        break;
    }

    case NewContent::Kind::Rectangle:
    case NewContent::Kind::Ellipse:
    case NewContent::Kind::Line: {
        if (item.fill.isValid()) {
            content += number(item.fill.redF()) + " " + number(item.fill.greenF()) + " " + number(item.fill.blueF())
                + " rg\n";
        }
        if (item.stroke.isValid()) {
            content += number(item.stroke.redF()) + " " + number(item.stroke.greenF()) + " "
                + number(item.stroke.blueF()) + " RG\n";
        }
        content += number(item.lineWidth) + " w\n";

        if (item.kind == NewContent::Kind::Line) {
            content += number(rect.left()) + " " + number(rect.bottom()) + " m " + number(rect.right()) + " "
                + number(rect.top()) + " l\nS\n";
        } else if (item.kind == NewContent::Kind::Ellipse) {
            // 0.5523 is the circle-to-Bézier constant; anything less gives a
            // visibly flat-sided oval.
            const double kappa = 0.5523;
            const double cx = rect.center().x();
            const double cy = rect.center().y();
            const double rx = rect.width() / 2.0;
            const double ry = rect.height() / 2.0;
            content += number(cx - rx) + " " + number(cy) + " m\n";
            content += number(cx - rx) + " " + number(cy + ry * kappa) + " " + number(cx - rx * kappa) + " "
                + number(cy + ry) + " " + number(cx) + " " + number(cy + ry) + " c\n";
            content += number(cx + rx * kappa) + " " + number(cy + ry) + " " + number(cx + rx) + " "
                + number(cy + ry * kappa) + " " + number(cx + rx) + " " + number(cy) + " c\n";
            content += number(cx + rx) + " " + number(cy - ry * kappa) + " " + number(cx + rx * kappa) + " "
                + number(cy - ry) + " " + number(cx) + " " + number(cy - ry) + " c\n";
            content += number(cx - rx * kappa) + " " + number(cy - ry) + " " + number(cx - rx) + " "
                + number(cy - ry * kappa) + " " + number(cx - rx) + " " + number(cy) + " c\n";
            content += item.fill.isValid() ? "B\n" : "S\n";
        } else {
            content += number(rect.left()) + " " + number(rect.top()) + " " + number(rect.width()) + " "
                + number(rect.height()) + " re\n";
            content += item.fill.isValid() ? "B\n" : "S\n";
        }
        break;
    }
    }

    content += "Q\n";
    return content;
}

} // namespace

QVector<PageObject> PageObjects::read(const QString &pdf, int page, QString *error)
{
    QVector<PageObject> objects;
    try {
        QPDF document;
        PdfFile::open(document, pdf);

        QPDFPageDocumentHelper documents(document);
        std::vector<QPDFPageObjectHelper> pages = documents.getAllPages();
        if (page < 0 || page >= int(pages.size())) {
            if (error) {
                *error = i18n("The document has no page %1.", page + 1);
            }
            return objects;
        }

        QPDFPageObjectHelper handle = pages.at(size_t(page));
        bool invertible = false;
        const QTransform toDisplay = pageTransformFor(handle).inverted(&invertible);
        if (!invertible) {
            return objects;
        }

        ObjectFilter filter(handle.getAttribute("/Resources", false), toDisplay, page);
        filter.setReading(true);
        handle.filterContents(&filter, nullptr);
        objects = filter.objects();
    } catch (const std::exception &e) {
        if (error) {
            *error = QString::fromUtf8(e.what());
        }
        return {};
    }
    return objects;
}

QVector<PageObject> PageObjects::readAll(const QString &pdf, QString *error)
{
    QVector<PageObject> all;
    int count = 0;
    try {
        QPDF document;
        PdfFile::open(document, pdf);
        count = int(QPDFPageDocumentHelper(document).getAllPages().size());
    } catch (const std::exception &e) {
        if (error) {
            *error = QString::fromUtf8(e.what());
        }
        return {};
    }
    for (int page = 0; page < count; ++page) {
        all += read(pdf, page, error);
    }
    return all;
}

int PageObjects::hitTest(const QVector<PageObject> &objects, const QPointF &where)
{
    // Backwards, so what lies on top is what gets picked.
    for (int i = int(objects.size()) - 1; i >= 0; --i) {
        const PageObject &object = objects.at(i);
        if (object.kind == PageObject::Kind::Path && !object.outline.isEmpty()) {
            // For vector art the shape is a fairer target than its box: a thin
            // diagonal line has a bounding rectangle the size of a page.
            if (object.outline.contains(where)) {
                return object.index;
            }
            const double slack = qMax(2.0, object.lineWidth);
            if (object.outline.intersects(QRectF(where.x() - slack, where.y() - slack, slack * 2, slack * 2))) {
                return object.index;
            }
            continue;
        }
        if (!object.bounds.isEmpty() && object.bounds.contains(where)) {
            return object.index;
        }
    }
    return -1;
}

QVector<int> PageObjects::within(const QVector<PageObject> &objects, const QRectF &area)
{
    QVector<int> found;
    for (const PageObject &object : objects) {
        if (!object.bounds.isEmpty() && area.intersects(object.bounds)) {
            found.append(object.index);
        }
    }
    return found;
}

QString PageObjects::describe(PageObject::Kind kind)
{
    switch (kind) {
    case PageObject::Kind::Text:
        return i18nc("@item kind of page object", "Text");
    case PageObject::Kind::Image:
        return i18nc("@item kind of page object", "Picture");
    case PageObject::Kind::Path:
        return i18nc("@item kind of page object", "Shape");
    case PageObject::Kind::Drawing:
        return i18nc("@item kind of page object", "Drawing");
    case PageObject::Kind::Shading:
        return i18nc("@item kind of page object", "Gradient");
    case PageObject::Kind::InlineImage:
        return i18nc("@item kind of page object", "Inline picture");
    }
    return {};
}

QStringList PageObjects::limitations()
{
    return {
        i18n("Objects inside an embedded drawing are handled as one unit. There is no way in without dissolving "
             "the drawing first."),
        i18n("A shape's outline is approximated so that it can be clicked on. What appears on the page is still "
             "drawn by the renderer, exactly as the file says."),
        i18n("Text is one object per drawing operation, which is usually a line rather than a paragraph."),
        i18n("Clipping is followed only as far as a rectangle. An object clipped to an unusual shape reports a "
             "slightly larger boundary than it fills."),
    };
}

bool PageComposer::apply(const QString &inputPdf, const QString &outputPdf, const QVector<PageEdit> &edits,
                         const QVector<NewContent> &insertions, Report *report, QString *error)
{
    return apply(inputPdf, outputPdf, edits, insertions, Options(), report, error);
}

bool PageComposer::apply(const QString &inputPdf, const QString &outputPdf, const QVector<PageEdit> &edits,
                         const QVector<NewContent> &insertions, const Options &options, Report *report, QString *error)
{
    if (edits.isEmpty() && insertions.isEmpty() && !options.backdrop.isValid() && options.annotations) {
        if (error) {
            *error = i18n("There is nothing to change.");
        }
        return false;
    }

    Report collected;
    try {
        QPDF pdf;
        PdfFile::open(pdf, inputPdf);

        QPDFPageDocumentHelper documents(pdf);
        std::vector<QPDFPageObjectHelper> pages = documents.getAllPages();

        QHash<int, QHash<int, PageEdit>> byPage;
        for (const PageEdit &edit : edits) {
            if (edit.page >= 0 && edit.page < int(pages.size()) && edit.object >= 0) {
                byPage[edit.page].insert(edit.object, edit);
            }
        }

        QList<int> touched = byPage.keys();
        std::sort(touched.begin(), touched.end());

        for (int index : std::as_const(touched)) {
            QPDFPageObjectHelper page = pages.at(size_t(index));
            bool invertible = false;
            const QTransform toDisplay = pageTransformFor(page).inverted(&invertible);

            Pl_Buffer buffer("composed");
            ObjectFilter filter(page.getAttribute("/Resources", true), invertible ? toDisplay : QTransform(), index);
            filter.setReading(false);
            filter.setEdits(byPage.value(index));
            page.filterContents(&filter, &buffer);

            const auto data = buffer.getBufferSharedPointer();
            page.getObjectHandle().replaceKey(
                "/Contents",
                QPDFObjectHandle::newStream(
                    &pdf, std::string(reinterpret_cast<const char *>(data->getBuffer()), data->getSize())));

            collected.transformed += filter.transformed();
            collected.deleted += filter.deleted();
            collected.restyled += filter.restyled();
            collected.refusals += filter.refusals();
        }

        // ── insertions ────────────────────────────────────────────────────
        QHash<int, QVector<NewContent>> newByPage;
        for (const NewContent &item : insertions) {
            if (item.page >= 0 && item.page < int(pages.size())) {
                newByPage[item.page].append(item);
            }
        }
        for (auto it = newByPage.constBegin(); it != newByPage.constEnd(); ++it) {
            QPDFPageObjectHelper page = pages.at(size_t(it.key()));
            QPDFObjectHandle resources = page.getAttribute("/Resources", true);
            if (!resources.isDictionary()) {
                resources = QPDFObjectHandle::parse("<< >>");
                page.getObjectHandle().replaceKey("/Resources", resources);
            }
            const QTransform toPage = pageTransformFor(page);

            std::string content;
            for (const NewContent &item : it.value()) {
                const std::string one = streamFor(pdf, item, toPage, resources, &collected.refusals);
                if (!one.empty()) {
                    content += one;
                    ++collected.inserted;
                }
            }
            if (content.empty()) {
                continue;
            }

            // Existing content is wrapped in q/Q first: `cm` concatenates rather
            // than replaces, and page content routinely ends without restoring
            // what it changed.
            PdfGeometry::isolateExistingContent(page, page.getObjectHandle());
            page.addPageContents(QPDFObjectHandle::newStream(&pdf, content), false);
        }

        // ── what the page is for, rather than what has changed on it ──────
        for (QPDFPageObjectHelper &page : pages) {
            if (!options.annotations) {
                page.getObjectHandle().removeKey("/Annots");
            }
            if (!options.backdrop.isValid()) {
                continue;
            }

            // First rather than last, which is the one thing an insertion
            // cannot be. In page space and over the media box, so that a sheet
            // the file turns with /Rotate is covered corner to corner rather
            // than across a rectangle lying on its side.
            const QRectF media = PdfGeometry::mediaBoxOf(page);
            const std::string paint = "q " + number(options.backdrop.redF()) + " " + number(options.backdrop.greenF())
                + " " + number(options.backdrop.blueF()) + " rg\n" + number(media.x()) + " " + number(media.y()) + " "
                + number(media.width()) + " " + number(media.height()) + " re f\nQ\n";
            page.addPageContents(QPDFObjectHandle::newStream(&pdf, paint), true);
        }

        QPDFWriter writer(pdf, outputPdf.toUtf8().constData());
        writer.setObjectStreamMode(qpdf_o_generate);
        writer.setStreamDataMode(qpdf_s_compress);
        writer.write();
    } catch (const std::exception &e) {
        if (error) {
            *error = QString::fromUtf8(e.what());
        }
        return false;
    }

    if (report) {
        *report = collected;
    }
    return true;
}

QStringList PageComposer::limitations()
{
    return {
        i18n("A moved or restyled object is wrapped in the change rather than rewritten, so everything else on the "
             "page keeps its bytes."),
        i18n("Changing the stacking order is not offered yet. It would mean reordering operator blocks, which is "
             "only safe when each one is properly enclosed."),
        i18n("Inserted text uses one of the standard fonts and can carry only the characters those hold."),
    };
}

} // namespace ps
