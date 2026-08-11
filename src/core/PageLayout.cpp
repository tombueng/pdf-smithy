/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#include "PageLayout.h"
#include "PdfFile.h"

#include "Overlay.h"
#include "PdfGeometry.h"

#include <KLocalizedString>

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QTemporaryFile>
#include <QTransform>

#include <qpdf/Constants.h>
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFNumberTreeObjectHelper.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageLabelDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>
#include <qpdf/QPDFWriter.hh>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <limits>
#include <set>

namespace ps {

namespace {

using PdfGeometry::number;

/**
 * How far a box may stick out of its parent before it counts as wrong.
 *
 * Boxes are written to four decimal places, so two rectangles meant to be
 * identical can differ in the fifth. A thousandth of a point is a third of a
 * micrometre, far below anything a press can hold, and far above the rounding.
 */
constexpr double kBoxTolerance = 0.001;

/** A box that has collapsed to nothing is a mistake rather than an instruction. */
constexpr double kSmallestBox = 0.5;

std::string boxArray(const QRectF &box)
{
    return "[" + number(box.x()) + " " + number(box.y()) + " " + number(box.x() + box.width()) + " "
        + number(box.y() + box.height()) + "]";
}

std::string matrixArray(const QTransform &transform)
{
    return "[" + number(transform.m11()) + " " + number(transform.m12()) + " " + number(transform.m21()) + " "
        + number(transform.m22()) + " " + number(transform.dx()) + " " + number(transform.dy()) + "]";
}

/**
 * A box entry as a rectangle, invalid when the entry is missing or unusable.
 *
 * The four numbers are two opposite corners rather than an origin and a size,
 * and the specification does not say which corner comes first, and plenty of
 * documents write them the other way round. Sorting them here means nothing
 * downstream has to wonder.
 */
QRectF rectOf(QPDFObjectHandle box)
{
    if (!box.isArray() || box.getArrayNItems() != 4) {
        return {};
    }
    const double missing = std::numeric_limits<double>::quiet_NaN();
    double value[4];
    for (int i = 0; i < 4; ++i) {
        value[i] = PdfGeometry::boxValue(box, i, missing);
        if (std::isnan(value[i])) {
            return {};
        }
    }
    const QRectF rect(qMin(value[0], value[2]), qMin(value[1], value[3]), qAbs(value[2] - value[0]),
                      qAbs(value[3] - value[1]));
    return rect.isValid() ? rect : QRectF();
}

/** A page's own box, with /MediaBox and /CropBox followed up the page tree. */
QRectF pageBox(QPDFPageObjectHelper &page, const char *key)
{
    const std::string name(key);
    if (name == "/MediaBox" || name == "/CropBox") {
        return rectOf(page.getAttribute(name, false));
    }
    return rectOf(page.getObjectHandle().getKey(name));
}

/** What the page shows: its /CropBox, or the whole sheet where it has none. */
QRectF visibleBox(QPDFPageObjectHelper &page)
{
    const QRectF crop = pageBox(page, "/CropBox");
    return crop.isValid() ? crop : PdfGeometry::mediaBoxOf(page);
}

bool fitsInside(const QRectF &inner, const QRectF &outer)
{
    return inner.x() >= outer.x() - kBoxTolerance && inner.y() >= outer.y() - kBoxTolerance
        && inner.x() + inner.width() <= outer.x() + outer.width() + kBoxTolerance
        && inner.y() + inner.height() <= outer.y() + outer.height() + kBoxTolerance;
}

/**
 * Names the box that escapes its parent, or returns an empty string.
 *
 * A viewer handed a /TrimBox outside its /MediaBox has no defined behaviour to
 * fall back on: some clip it, some ignore it, and an imposition program will
 * happily place the page a centimetre off. Saying which of the five is wrong is
 * the difference between a fixable message and a mystery.
 *
 * Only the boxes that are actually there are checked. An absent one makes no
 * claim about the page and so cannot be wrong: the specification's fallback
 * makes a missing /ArtBox equal the /CropBox, which on any normal page with a
 * smaller trim would otherwise read as an error on every file in existence.
 */
QString nestingProblem(const PageLayout::Boxes &boxes, int pageNumber)
{
    struct Frame {
        QRectF box;
        const char *name;
    };

    const Frame media { boxes.media, "/MediaBox" };
    const Frame bleedFrame = boxes.bleed.isValid() ? Frame { boxes.bleed, "/BleedBox" } : media;
    const Frame trimFrame = boxes.trim.isValid() ? Frame { boxes.trim, "/TrimBox" } : bleedFrame;

    struct Check {
        QRectF inner;
        const char *innerName;
        Frame outer;
    };
    const Check checks[] = {
        { boxes.crop, "/CropBox", media },
        { boxes.bleed, "/BleedBox", media },
        { boxes.trim, "/TrimBox", bleedFrame },
        { boxes.art, "/ArtBox", trimFrame },
    };

    for (const Check &check : checks) {
        if (check.inner.isValid() && !fitsInside(check.inner, check.outer.box)) {
            return i18n("On page %1 the %2 does not fit inside the %3. Every box has to lie within the one around it.",
                        pageNumber, QString::fromLatin1(check.innerName), QString::fromLatin1(check.outer.name));
        }
    }
    return {};
}

void writeBox(QPDFObjectHandle dictionary, const char *key, const QRectF &box)
{
    if (box.isValid()) {
        dictionary.replaceKey(key, QPDFObjectHandle::parse(boxArray(box)));
    } else {
        dictionary.removeKey(key);
    }
}

/**
 * Display space: the page as the reader sees it, origin at its bottom left.
 *
 * Everything a printer's mark or a split has to be measured against ("the left
 * half", "outside the bottom edge") is meant in display terms, while content
 * streams and box entries are written in page space. Prefixing a stream with
 * this makes the two the same for the operators that follow, which is far safer
 * than turning every coordinate by hand.
 */
std::string displayPrefix(const QRectF &media, int rotate)
{
    return "1 0 0 1 " + number(media.x()) + " " + number(media.y()) + " cm\n"
        + PdfGeometry::displayToPageMatrix(rotate, media.width(), media.height());
}

QTransform pageToDisplay(const QRectF &media, int rotate)
{
    return QTransform::fromTranslate(-media.x(), -media.y())
        * PdfGeometry::displayToPageTransform(rotate, media.width(), media.height()).inverted();
}

QRectF toDisplay(const QRectF &pageRect, const QRectF &media, int rotate)
{
    return pageToDisplay(media, rotate).mapRect(pageRect);
}

QRectF toPageSpace(const QRectF &displayRect, const QRectF &media, int rotate)
{
    return pageToDisplay(media, rotate).inverted().mapRect(displayRect);
}

std::string strokedLine(double x1, double y1, double x2, double y2)
{
    return number(x1) + " " + number(y1) + " m " + number(x2) + " " + number(y2) + " l S\n";
}

std::string filledRect(const QRectF &rect)
{
    return number(rect.x()) + " " + number(rect.y()) + " " + number(rect.width()) + " " + number(rect.height())
        + " re f\n";
}

/**
 * A literal string for a content stream, encoded the way the mark font reads it.
 *
 * WinAnsi rather than Latin-1, and the difference is not academic: the quotes a
 * translator will use, and the ellipsis and bullet a mark may carry, all live in
 * the 0x80 to 0x9F range that Latin-1 leaves empty, so Qt's toLatin1() turns every
 * one of them into a question mark. Anything the encoding genuinely cannot hold
 * become a question mark, because an obviously wrong file name in a printer's
 * mark is better than a quietly shortened one.
 */
std::string pdfString(const QString &text)
{
    return PdfGeometry::winAnsiLiteral(text);
}

/**
 * The colour that appears on every plate.
 *
 * Printer's marks in black would come out on the black plate alone, which is
 * useless for aligning the other three. /Separation /All is the one colour space
 * that means "all the ink there is", and its tint transform is a straight ramp
 * to solid in all four channels.
 */
QPDFObjectHandle allInkColourSpace(QPDF &pdf)
{
    QPDFObjectHandle tint = pdf.makeIndirectObject(
        QPDFObjectHandle::parse("<< /FunctionType 2 /Domain [0 1] /C0 [0 0 0 0] /C1 [1 1 1 1] /N 1 >>"));

    QPDFObjectHandle space = QPDFObjectHandle::newArray();
    space.appendItem(QPDFObjectHandle::newName("/Separation"));
    space.appendItem(QPDFObjectHandle::newName("/All"));
    space.appendItem(QPDFObjectHandle::newName("/DeviceCMYK"));
    space.appendItem(tint);
    return pdf.makeIndirectObject(space);
}

QPDFObjectHandle markFont(QPDF &pdf)
{
    // One of the fourteen fonts every reader has, so nothing has to be embedded
    // for a line of text nobody reads twice.
    return pdf.makeIndirectObject(
        QPDFObjectHandle::parse("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>"));
}

/** The page's own /Resources, made writable and given the subdictionaries @p wanted names. */
QPDFObjectHandle writableResources(QPDFPageObjectHelper &page, const char *wanted)
{
    QPDFObjectHandle resources = page.getAttribute("/Resources", true);
    if (!resources.isDictionary()) {
        resources = QPDFObjectHandle::newDictionary();
        page.getObjectHandle().replaceKey("/Resources", resources);
    }
    resources.mergeResources(QPDFObjectHandle::parse(wanted));
    return resources;
}

/**
 * A registration mark: a ring with a cross through it.
 *
 * The ring is four Bézier quarters; 0.5523 is the control-point distance that
 * makes a cubic curve indistinguishable from a circular arc at any size a press
 * can resolve. @p reach is given separately from @p radius so the cross can be
 * held inside the room the sheet was grown by; a mark clipped at the paper edge
 * is a mark the operator cannot align on.
 */
std::string registrationMark(double cx, double cy, double radius, double reach)
{
    const double k = radius * 0.5523;
    std::string out;
    out += number(cx + radius) + " " + number(cy) + " m\n";
    out += number(cx + radius) + " " + number(cy + k) + " " + number(cx + k) + " " + number(cy + radius) + " "
        + number(cx) + " " + number(cy + radius) + " c\n";
    out += number(cx - k) + " " + number(cy + radius) + " " + number(cx - radius) + " " + number(cy + k) + " "
        + number(cx - radius) + " " + number(cy) + " c\n";
    out += number(cx - radius) + " " + number(cy - k) + " " + number(cx - k) + " " + number(cy - radius) + " "
        + number(cx) + " " + number(cy - radius) + " c\n";
    out += number(cx + k) + " " + number(cy - radius) + " " + number(cx + radius) + " " + number(cy - k) + " "
        + number(cx + radius) + " " + number(cy) + " c\n";
    out += "S\n";
    out += strokedLine(cx - reach, cy, cx + reach, cy);
    out += strokedLine(cx, cy - reach, cx, cy + reach);
    return out;
}

QString romanNumeral(int value, bool upper)
{
    if (value < 1) {
        return {};
    }
    struct Symbol {
        int value;
        const char *text;
    };
    static const Symbol symbols[]
        = { { 1000, "M" }, { 900, "CM" }, { 500, "D" }, { 400, "CD" }, { 100, "C" }, { 90, "XC" }, { 50, "L" },
            { 40, "XL" },  { 10, "X" },   { 9, "IX" },  { 5, "V" },    { 4, "IV" },  { 1, "I" } };
    QString out;
    for (const Symbol &symbol : symbols) {
        while (value >= symbol.value) {
            out += QLatin1String(symbol.text);
            value -= symbol.value;
        }
    }
    return upper ? out : out.toLower();
}

/**
 * The alphabetic style, which repeats rather than carries.
 *
 * Page 27 is "AA" and page 53 is "AAA", not "AB". That looks like a mistake
 * until you read the specification, which says exactly that.
 */
QString letterLabel(int value, bool upper)
{
    if (value < 1) {
        return {};
    }
    const int repeats = (value - 1) / 26 + 1;
    const QChar letter = QLatin1Char(char('A' + (value - 1) % 26));
    return QString(repeats, upper ? letter : letter.toLower());
}

QString renderLabel(QPDFObjectHandle entry)
{
    if (!entry.isDictionary()) {
        return {};
    }

    QString prefix;
    QPDFObjectHandle prefixObject = entry.getKey("/P");
    if (prefixObject.isString()) {
        prefix = QString::fromStdString(prefixObject.getUTF8Value());
    }

    int start = 1;
    QPDFObjectHandle startObject = entry.getKey("/St");
    if (startObject.isInteger()) {
        start = startObject.getIntValueAsInt();
    }

    QString style;
    QPDFObjectHandle styleObject = entry.getKey("/S");
    if (styleObject.isName()) {
        style = QString::fromStdString(styleObject.getName());
    }

    QString body;
    if (style == QLatin1String("/D")) {
        body = QString::number(start);
    } else if (style == QLatin1String("/R")) {
        body = romanNumeral(start, true);
    } else if (style == QLatin1String("/r")) {
        body = romanNumeral(start, false);
    } else if (style == QLatin1String("/A")) {
        body = letterLabel(start, true);
    } else if (style == QLatin1String("/a")) {
        body = letterLabel(start, false);
    }
    return prefix + body;
}

qpdf_page_label_e labelStyle(PageLayout::Labels::Style style)
{
    switch (style) {
    case PageLayout::Labels::Style::Decimal:
        return pl_digits;
    case PageLayout::Labels::Style::RomanUpper:
        return pl_roman_upper;
    case PageLayout::Labels::Style::RomanLower:
        return pl_roman_lower;
    case PageLayout::Labels::Style::LetterUpper:
        return pl_alpha_upper;
    case PageLayout::Labels::Style::LetterLower:
        return pl_alpha_lower;
    case PageLayout::Labels::Style::None:
        break;
    }
    return pl_none;
}

/** The transform that puts one page's content onto a differently sized sheet. */
struct Placement {
    double scale = 1.0;
    double dx = 0.0;
    double dy = 0.0;
    QSizeF paper;
};

Placement placementFor(QPDFPageObjectHelper &page, const QSizeF &displaySize, PageLayout::Fit fit)
{
    Placement placement;

    // The caller asks for paper as the reader sees it, so a sideways page needs
    // the sheet turned before any of the arithmetic below.
    placement.paper = displaySize;
    const int rotate = PdfGeometry::rotationOf(page);
    if (rotate == 90 || rotate == 270) {
        placement.paper.transpose();
    }

    const QRectF source = visibleBox(page);
    if (source.width() <= 0.0 || source.height() <= 0.0) {
        return placement;
    }

    if (fit != PageLayout::Fit::Centre) {
        placement.scale = qMin(placement.paper.width() / source.width(), placement.paper.height() / source.height());
        if (fit == PageLayout::Fit::ScaleAndCentre) {
            placement.scale = qMin(1.0, placement.scale);
        }
    }

    placement.dx = (placement.paper.width() - placement.scale * source.width()) / 2.0 - placement.scale * source.x();
    placement.dy = (placement.paper.height() - placement.scale * source.height()) / 2.0 - placement.scale * source.y();
    return placement;
}

void movePoints(QPDFObjectHandle array, double scale, double dx, double dy)
{
    if (!array.isArray()) {
        return;
    }
    const int count = array.getArrayNItems();
    for (int i = 0; i < count; ++i) {
        QPDFObjectHandle item = array.getArrayItem(i);
        if (item.isArray()) {
            // /InkList is a list of paths, each of which starts its own x,y run.
            movePoints(item, scale, dx, dy);
            continue;
        }
        const double missing = std::numeric_limits<double>::quiet_NaN();
        const double value = PdfGeometry::numericValue(item, missing);
        if (std::isnan(value)) {
            continue;
        }
        array.setArrayItem(i, QPDFObjectHandle::parse(number(scale * value + ((i % 2 == 0) ? dx : dy))));
    }
}

/**
 * Moves the annotations with the content.
 *
 * An annotation's rectangle lives in page space, not in the content stream, so
 * scaling a page without touching them would leave every note and every link
 * hovering where the text used to be. The appearance stream follows for free:
 * readers fit it to the rectangle.
 */
void moveAnnotations(QPDFPageObjectHelper &page, double scale, double dx, double dy)
{
    QPDFObjectHandle annotations = page.getObjectHandle().getKey("/Annots");
    if (!annotations.isArray()) {
        return;
    }
    for (int i = 0; i < annotations.getArrayNItems(); ++i) {
        QPDFObjectHandle annotation = annotations.getArrayItem(i);
        if (!annotation.isDictionary()) {
            continue;
        }
        for (const char *key : { "/Rect", "/QuadPoints", "/Vertices", "/L", "/CL", "/InkList", "/Path" }) {
            movePoints(annotation.getKey(key), scale, dx, dy);
        }
    }
}

void applyPlacement(QPDFPageObjectHelper &page, const Placement &placement)
{
    QPDFObjectHandle dictionary = page.getObjectHandle();
    const QRectF paper(0.0, 0.0, placement.paper.width(), placement.paper.height());

    const bool unchanged = qFuzzyCompare(placement.scale, 1.0) && qAbs(placement.dx) < kBoxTolerance
        && qAbs(placement.dy) < kBoxTolerance;
    if (!unchanged) {
        // The content stream itself is never rewritten; it is wrapped, so a
        // scanned page keeps its exact bytes and a subset font keeps its exact
        // glyph programme.
        PdfGeometry::isolateExistingContent(page, dictionary,
                                            number(placement.scale) + " 0 0 " + number(placement.scale) + " "
                                                + number(placement.dx) + " " + number(placement.dy) + " cm\n");
        moveAnnotations(page, placement.scale, placement.dx, placement.dy);
    }

    // Read before the /MediaBox changes underneath them.
    for (const char *key : { "/CropBox", "/BleedBox", "/TrimBox", "/ArtBox" }) {
        const QRectF box = rectOf(dictionary.getKey(key));
        if (!box.isValid()) {
            continue;
        }
        const QRectF moved = QRectF(placement.scale * box.x() + placement.dx, placement.scale * box.y() + placement.dy,
                                    placement.scale * box.width(), placement.scale * box.height())
                                 .intersected(paper);
        writeBox(dictionary, key, (moved.width() > kSmallestBox && moved.height() > kSmallestBox) ? moved : QRectF());
    }
    dictionary.replaceKey("/MediaBox", QPDFObjectHandle::parse(boxArray(paper)));
}

QPDFPageObjectHelper makeBlankPage(QPDF &pdf, const QRectF &media)
{
    QPDFObjectHandle page = QPDFObjectHandle::parse("<< /Type /Page >>");
    page.replaceKey("/MediaBox", QPDFObjectHandle::parse(boxArray(media)));
    page.replaceKey("/Resources", QPDFObjectHandle::newDictionary());
    page.replaceKey("/Contents", QPDFObjectHandle::newStream(&pdf, std::string()));
    return QPDFPageObjectHelper(pdf.makeIndirectObject(page));
}

/** The order a saddle-stitched booklet is laid down in: 8, 1, 2, 7, 6, 3, 4, 5. */
QVector<int> spreadOrder(int count)
{
    QVector<int> order;
    order.reserve(count);
    int front = 0;
    int back = count - 1;
    bool backFirst = true;
    while (front < back) {
        if (backFirst) {
            order.append(back);
            order.append(front);
        } else {
            order.append(front);
            order.append(back);
        }
        ++front;
        --back;
        backFirst = !backFirst;
    }
    return order;
}

QVector<int> invertOrder(const QVector<int> &order)
{
    QVector<int> inverse(order.size(), 0);
    for (int slot = 0; slot < order.size(); ++slot) {
        inverse[order.at(slot)] = slot;
    }
    return inverse;
}

QVector<int> tidyPageList(const QVector<int> &pages)
{
    QVector<int> wanted = pages;
    std::sort(wanted.begin(), wanted.end());
    wanted.erase(std::unique(wanted.begin(), wanted.end()), wanted.end());
    return wanted;
}

/**
 * Reads, edits and writes a document, never over the top of the original.
 *
 * The result goes to a temporary file beside the destination and is renamed into
 * place, so a document opened and saved onto itself cannot be half-written when
 * something throws. @p work returns false to abandon the whole operation, and
 * nothing reaches the destination when it does.
 */
bool rewrite(const QString &input, const QString &output,
             const std::function<bool(QPDF &, QPDFPageDocumentHelper &, QString *)> &work, QString *error)
{
    QTemporaryFile temp(QFileInfo(output).absolutePath() + QLatin1String("/.pdf-smithy-layout-XXXXXX.pdf"));
    temp.setAutoRemove(false);
    if (!temp.open()) {
        if (error) {
            *error = i18n("Cannot write to “%1”.", QFileInfo(output).absolutePath());
        }
        return false;
    }
    const QString tempPath = temp.fileName();
    temp.close();

    try {
        QPDF pdf;
        PdfFile::open(pdf, input);

        QPDFPageDocumentHelper documents(pdf);
        // An inherited /MediaBox is the commonest reason a page looks as though
        // it has no boxes at all. Pushing the inheritable attributes down once,
        // here, means every step below can read and write the page's own
        // dictionary and be right.
        documents.pushInheritedAttributesToPage();

        if (!work(pdf, documents, error)) {
            QFile::remove(tempPath);
            return false;
        }

        QPDFWriter writer(pdf);
        writer.setOutputFilename(QFile::encodeName(tempPath).constData());
        writer.setObjectStreamMode(qpdf_o_generate);
        // Form XObjects built out of existing pages arrive with no filter on
        // them, and preserving stream data would write them that way, so a
        // five-megabyte page becomes a fifty-megabyte poster. Compressing costs
        // nothing else, since QPDF never re-encodes a JPEG.
        writer.setCompressStreams(true);
        writer.setDeterministicID(true);
        writer.write();
    } catch (const std::exception &e) {
        QFile::remove(tempPath);
        if (error) {
            *error = QString::fromUtf8(e.what());
        }
        return false;
    }

    QFile::setPermissions(tempPath, QFile::ReadOwner | QFile::WriteOwner | QFile::ReadGroup | QFile::ReadOther);
    if (::rename(QFile::encodeName(tempPath).constData(), QFile::encodeName(output).constData()) != 0) {
        QFile::remove(tempPath);
        if (error) {
            *error = i18n("Could not write “%1”.", output);
        }
        return false;
    }
    return true;
}

/**
 * Counts the operators on a page that put something on the paper.
 *
 * A text-showing operator whose string is nothing but spaces is not counted:
 * generated documents are full of them, and a page of empty `Tj` calls is a
 * blank page by any reading. Everything else that can mark paper is counted,
 * including `Do`: a form XObject might well be empty, but assuming so would
 * risk calling a page with a picture on it blank, and that is the one mistake
 * this must not make.
 */
class InkCounter : public QPDFObjectHandle::ParserCallbacks
{
public:
    int marks() const { return m_marks; }

    void handleObject(QPDFObjectHandle object) override
    {
        if (object.isInlineImage()) {
            ++m_marks;
            return;
        }
        if (!object.isOperator()) {
            m_operands.append(object);
            return;
        }
        if (paints(object.getOperatorValue())) {
            ++m_marks;
        }
        m_operands.clear();
    }

    void handleEOF() override { }

private:
    static bool onlySpaces(const std::string &text)
    {
        for (const char c : text) {
            // A two-byte font writes its space as 0x00 0x20, so a run of nulls
            // and spaces covers both the simple and the composite case.
            if (c != ' ' && c != '\0' && c != '\t' && c != '\n' && c != '\r') {
                return false;
            }
        }
        return true;
    }

    bool paints(const std::string &op) const
    {
        if (op == "Tj" || op == "'" || op == "\"") {
            for (int i = m_operands.size() - 1; i >= 0; --i) {
                if (m_operands.at(i).isString()) {
                    return !onlySpaces(m_operands.at(i).getStringValue());
                }
            }
            return false;
        }
        if (op == "TJ") {
            for (int i = m_operands.size() - 1; i >= 0; --i) {
                QPDFObjectHandle array = m_operands.at(i);
                if (!array.isArray()) {
                    continue;
                }
                for (int j = 0; j < array.getArrayNItems(); ++j) {
                    QPDFObjectHandle item = array.getArrayItem(j);
                    if (item.isString() && !onlySpaces(item.getStringValue())) {
                        return true;
                    }
                }
                return false;
            }
            return false;
        }
        static const std::set<std::string> painting
            = { "Do", "sh", "f", "F", "f*", "B", "B*", "b", "b*", "S", "s", "EI" };
        return painting.count(op) > 0;
    }

    int m_marks = 0;
    QVector<QPDFObjectHandle> m_operands;
};

} // namespace

QVector<PageLayout::Boxes> PageLayout::boxesOf(const QString &pdf, QString *error)
{
    QVector<Boxes> result;

    try {
        QPDF document;
        PdfFile::open(document, pdf);

        std::vector<QPDFPageObjectHelper> pages = QPDFPageDocumentHelper(document).getAllPages();
        result.reserve(int(pages.size()));
        for (QPDFPageObjectHelper &page : pages) {
            Boxes boxes;
            boxes.media = pageBox(page, "/MediaBox");
            boxes.crop = pageBox(page, "/CropBox");
            boxes.bleed = pageBox(page, "/BleedBox");
            boxes.trim = pageBox(page, "/TrimBox");
            boxes.art = pageBox(page, "/ArtBox");
            result.append(boxes);
        }
    } catch (const std::exception &e) {
        if (error) {
            *error = QString::fromUtf8(e.what());
        }
        return {};
    }

    return result;
}

bool PageLayout::setBoxes(const QString &in, const QString &out, const QVector<int> &pages, const Boxes &boxes,
                          QString *error)
{
    if (pages.isEmpty()) {
        if (error) {
            *error = i18n("There are no pages to change.");
        }
        return false;
    }

    const QVector<int> wanted = tidyPageList(pages);

    return rewrite(
        in, out,
        [&wanted, &boxes](QPDF &, QPDFPageDocumentHelper &documents, QString *why) {
            std::vector<QPDFPageObjectHelper> all = documents.getAllPages();

            // Checked over every page before a single one is written. A
            // half-applied set of boxes is worse than a refusal, because nothing
            // in the file afterwards says which pages got through.
            for (const int index : wanted) {
                if (index < 0 || index >= int(all.size())) {
                    if (why) {
                        *why = i18n("There is no page %1 in this document.", index + 1);
                    }
                    return false;
                }
                QPDFPageObjectHelper &page = all[size_t(index)];

                Boxes proposed = boxes;
                if (!proposed.media.isValid()) {
                    proposed.media = pageBox(page, "/MediaBox");
                }
                if (!proposed.media.isValid()) {
                    if (why) {
                        *why = i18n("Page %1 has no page size of its own, so one has to be given.", index + 1);
                    }
                    return false;
                }

                const QString problem = nestingProblem(proposed, index + 1);
                if (!problem.isEmpty()) {
                    if (why) {
                        *why = problem;
                    }
                    return false;
                }
            }

            for (const int index : wanted) {
                QPDFObjectHandle dictionary = all[size_t(index)].getObjectHandle();
                if (boxes.media.isValid()) {
                    dictionary.replaceKey("/MediaBox", QPDFObjectHandle::parse(boxArray(boxes.media)));
                }
                writeBox(dictionary, "/CropBox", boxes.crop);
                writeBox(dictionary, "/BleedBox", boxes.bleed);
                writeBox(dictionary, "/TrimBox", boxes.trim);
                writeBox(dictionary, "/ArtBox", boxes.art);
            }
            return true;
        },
        error);
}

bool PageLayout::setBleed(const QString &in, const QString &out, const QVector<int> &pages, double bleedPoints,
                          QString *error)
{
    if (pages.isEmpty()) {
        if (error) {
            *error = i18n("There are no pages to change.");
        }
        return false;
    }
    if (bleedPoints <= 0.0) {
        if (error) {
            *error = i18n("The bleed has to be greater than zero.");
        }
        return false;
    }

    const QVector<int> wanted = tidyPageList(pages);

    return rewrite(
        in, out,
        [&wanted, bleedPoints](QPDF &, QPDFPageDocumentHelper &documents, QString *why) {
            std::vector<QPDFPageObjectHelper> all = documents.getAllPages();

            for (const int index : wanted) {
                if (index < 0 || index >= int(all.size())) {
                    if (why) {
                        *why = i18n("There is no page %1 in this document.", index + 1);
                    }
                    return false;
                }
                QPDFPageObjectHelper &page = all[size_t(index)];

                Boxes proposed;
                proposed.trim = visibleBox(page);
                proposed.bleed = proposed.trim.adjusted(-bleedPoints, -bleedPoints, bleedPoints, bleedPoints);
                // Bleed is ink that runs past the cut, so it needs paper to run
                // onto: the sheet grows to hold it rather than the bleed being
                // quietly clipped back to a sheet that cannot carry it.
                proposed.media = PdfGeometry::mediaBoxOf(page).united(proposed.bleed);
                proposed.crop = pageBox(page, "/CropBox");
                proposed.art = pageBox(page, "/ArtBox");

                const QString problem = nestingProblem(proposed, index + 1);
                if (!problem.isEmpty()) {
                    if (why) {
                        *why = problem;
                    }
                    return false;
                }

                QPDFObjectHandle dictionary = page.getObjectHandle();
                dictionary.replaceKey("/MediaBox", QPDFObjectHandle::parse(boxArray(proposed.media)));
                dictionary.replaceKey("/TrimBox", QPDFObjectHandle::parse(boxArray(proposed.trim)));
                dictionary.replaceKey("/BleedBox", QPDFObjectHandle::parse(boxArray(proposed.bleed)));
            }
            return true;
        },
        error);
}

bool PageLayout::resize(const QString &in, const QString &out, const QVector<int> &pages, const QSizeF &sizePoints,
                        Fit fit, QString *error)
{
    if (pages.isEmpty()) {
        if (error) {
            *error = i18n("There are no pages to resize.");
        }
        return false;
    }
    if (sizePoints.width() <= 1.0 || sizePoints.height() <= 1.0) {
        if (error) {
            *error = i18n("The paper size has to be at least one point in each direction.");
        }
        return false;
    }

    const QVector<int> wanted = tidyPageList(pages);

    return rewrite(
        in, out,
        [&wanted, &sizePoints, fit](QPDF &, QPDFPageDocumentHelper &documents, QString *why) {
            std::vector<QPDFPageObjectHelper> all = documents.getAllPages();
            for (const int index : wanted) {
                if (index < 0 || index >= int(all.size())) {
                    if (why) {
                        *why = i18n("There is no page %1 in this document.", index + 1);
                    }
                    return false;
                }
                QPDFPageObjectHelper &page = all[size_t(index)];
                applyPlacement(page, placementFor(page, sizePoints, fit));
            }
            return true;
        },
        error);
}

bool PageLayout::unify(const QString &in, const QString &out, const QSizeF &sizePoints, Fit fit, QString *error)
{
    return rewrite(
        in, out,
        [&sizePoints, fit](QPDF &, QPDFPageDocumentHelper &documents, QString *why) {
            std::vector<QPDFPageObjectHelper> all = documents.getAllPages();
            if (all.empty()) {
                if (why) {
                    *why = i18n("The document has no pages.");
                }
                return false;
            }

            QSizeF target = sizePoints;
            if (target.width() <= 1.0 || target.height() <= 1.0) {
                double largest = -1.0;
                for (QPDFPageObjectHelper &page : all) {
                    QSizeF shown = visibleBox(page).size();
                    const int rotate = PdfGeometry::rotationOf(page);
                    if (rotate == 90 || rotate == 270) {
                        shown.transpose();
                    }
                    const double area = shown.width() * shown.height();
                    if (area > largest) {
                        largest = area;
                        target = shown;
                    }
                }
            }
            if (target.width() <= 1.0 || target.height() <= 1.0) {
                if (why) {
                    *why = i18n("None of the pages has a usable size.");
                }
                return false;
            }

            for (QPDFPageObjectHelper &page : all) {
                applyPlacement(page, placementFor(page, target, fit));
            }
            return true;
        },
        error);
}

bool PageLayout::splitPages(const QString &in, const QString &out, const QVector<int> &pages, bool vertical,
                            double atFraction, QString *error)
{
    if (pages.isEmpty()) {
        if (error) {
            *error = i18n("There are no pages to split.");
        }
        return false;
    }
    if (atFraction < 0.05 || atFraction > 0.95) {
        if (error) {
            *error = i18n("The split has to fall between a twentieth and nineteen twentieths of the page.");
        }
        return false;
    }

    const QVector<int> wanted = tidyPageList(pages);

    return rewrite(
        in, out,
        [&wanted, vertical, atFraction](QPDF &, QPDFPageDocumentHelper &documents, QString *why) {
            std::vector<QPDFPageObjectHelper> all = documents.getAllPages();

            for (const int index : wanted) {
                if (index < 0 || index >= int(all.size())) {
                    if (why) {
                        *why = i18n("There is no page %1 in this document.", index + 1);
                    }
                    return false;
                }
                QPDFPageObjectHelper &page = all[size_t(index)];

                const QRectF media = PdfGeometry::mediaBoxOf(page);
                const int rotate = PdfGeometry::rotationOf(page);
                const QRectF shown = toDisplay(visibleBox(page), media, rotate);

                QRectF halves[2];
                if (vertical) {
                    const double cut = shown.x() + atFraction * shown.width();
                    halves[0] = QRectF(shown.x(), shown.y(), cut - shown.x(), shown.height());
                    halves[1] = QRectF(cut, shown.y(), shown.x() + shown.width() - cut, shown.height());
                } else {
                    // Display space has y pointing up, so the fraction is
                    // measured down from the top, which is what anyone looking
                    // at the page means by it.
                    const double cut = shown.y() + (1.0 - atFraction) * shown.height();
                    halves[0] = QRectF(shown.x(), cut, shown.width(), shown.y() + shown.height() - cut);
                    halves[1] = QRectF(shown.x(), shown.y(), shown.width(), cut - shown.y());
                }

                for (const QRectF &half : halves) {
                    // A shallow copy shares the content stream, so both halves
                    // are windows onto the one original: nothing is resampled and
                    // the file barely grows.
                    QPDFPageObjectHelper copy = page.shallowCopyPage();
                    const QRectF frame = toPageSpace(half, media, rotate);

                    QPDFObjectHandle dictionary = copy.getObjectHandle();
                    dictionary.replaceKey("/MediaBox", QPDFObjectHandle::parse(boxArray(frame)));
                    dictionary.replaceKey("/CropBox", QPDFObjectHandle::parse(boxArray(frame)));
                    for (const char *key : { "/BleedBox", "/TrimBox", "/ArtBox" }) {
                        const QRectF box = rectOf(dictionary.getKey(key));
                        const QRectF kept = box.isValid() ? box.intersected(frame) : QRectF();
                        writeBox(dictionary, key,
                                 (kept.width() > kSmallestBox && kept.height() > kSmallestBox) ? kept : QRectF());
                    }
                    documents.addPageAt(copy, true, page);
                }
                documents.removePage(page);
            }
            return true;
        },
        error);
}

bool PageLayout::overlayPages(const QString &basePdf, const QString &overlayPdf, const QString &out, bool repeatOverlay,
                              QString *error)
{
    // Declared out here on purpose: copyForeignObject leaves references into the
    // overlay document that QPDFWriter only follows when it writes, so it has to
    // outlive the editing step.
    QPDF overlay;
    std::vector<QPDFPageObjectHelper> overlays;

    struct Imported {
        QPDFObjectHandle form;
        QRectF shown;
    };
    QHash<int, Imported> imported;

    return rewrite(
        basePdf, out,
        [&](QPDF &pdf, QPDFPageDocumentHelper &documents, QString *why) {
            PdfFile::open(overlay, overlayPdf);
            QPDFPageDocumentHelper overlayDocuments(overlay);
            overlayDocuments.pushInheritedAttributesToPage();
            overlays = overlayDocuments.getAllPages();
            if (overlays.empty()) {
                if (why) {
                    *why = i18n("The overlay document has no pages.");
                }
                return false;
            }

            std::vector<QPDFPageObjectHelper> bases = documents.getAllPages();
            if (bases.empty()) {
                if (why) {
                    *why = i18n("The document has no pages.");
                }
                return false;
            }

            for (size_t i = 0; i < bases.size(); ++i) {
                int which = int(i);
                if (which >= int(overlays.size())) {
                    if (!repeatOverlay) {
                        break;
                    }
                    which = which % int(overlays.size());
                }

                if (!imported.contains(which)) {
                    QPDFPageObjectHelper &source = overlays[size_t(which)];
                    const QRectF sourceMedia = PdfGeometry::mediaBoxOf(source);
                    const int sourceRotate = PdfGeometry::rotationOf(source);
                    const QRectF sourceVisible = visibleBox(source);

                    QPDFObjectHandle form = source.getFormXObjectForPage(false);
                    // QPDF bounds the form by the trim box; what has to land on
                    // the base page is what the overlay shows, which is its crop
                    // box.
                    form.getDict().replaceKey("/BBox", QPDFObjectHandle::parse(boxArray(sourceVisible)));
                    // Drawing it through the overlay's own display transform
                    // means a sideways overlay arrives the way it is read rather
                    // than the way it happens to be stored.
                    form.getDict().replaceKey(
                        "/Matrix", QPDFObjectHandle::parse(matrixArray(pageToDisplay(sourceMedia, sourceRotate))));

                    Imported entry;
                    entry.form = pdf.copyForeignObject(form);
                    entry.shown = toDisplay(sourceVisible, sourceMedia, sourceRotate);
                    imported.insert(which, entry);
                }

                const Imported entry = imported.value(which);
                if (entry.shown.width() <= 0.0 || entry.shown.height() <= 0.0) {
                    continue;
                }

                QPDFPageObjectHelper &base = bases[i];
                const QRectF baseMedia = PdfGeometry::mediaBoxOf(base);
                const int baseRotate = PdfGeometry::rotationOf(base);
                const QRectF target = toDisplay(visibleBox(base), baseMedia, baseRotate);

                const double scale = qMin(target.width() / entry.shown.width(), target.height() / entry.shown.height());
                const double dx
                    = target.x() + (target.width() - scale * entry.shown.width()) / 2.0 - scale * entry.shown.x();
                const double dy
                    = target.y() + (target.height() - scale * entry.shown.height()) / 2.0 - scale * entry.shown.y();

                QPDFObjectHandle resources = writableResources(base, "<< /XObject << >> >>");
                int suffix = 1;
                const std::string name = resources.getUniqueResourceName("/PsOverlay", suffix);
                resources.getKey("/XObject").replaceKey(name, entry.form);

                std::string content = "q\n";
                content += displayPrefix(baseMedia, baseRotate);
                content += number(scale) + " 0 0 " + number(scale) + " " + number(dx) + " " + number(dy) + " cm\n";
                content += name + " Do\nQ\n";

                PdfGeometry::isolateExistingContent(base, base.getObjectHandle());
                base.addPageContents(QPDFObjectHandle::newStream(&pdf, content), false);
            }
            return true;
        },
        error);
}

bool PageLayout::addMarks(const QString &in, const QString &out, const Marks &marks, QString *error)
{
    if (!marks.cropMarks && !marks.registrationMarks && !marks.colourBars && !marks.pageInformation) {
        if (error) {
            *error = i18n("No printer's marks were asked for.");
        }
        return false;
    }

    const QString sourceName = QFileInfo(in).fileName();
    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd hh:mm"));

    const double offset = qMax(0.0, marks.offsetPoints);
    const double length = qMax(2.0, marks.lengthPoints);
    const double lineWidth = qMax(0.05, marks.lineWidth);
    constexpr double kPatchSize = 9.0;
    constexpr double kInfoSize = 6.0;

    double room = 0.0;
    if (marks.cropMarks || marks.registrationMarks) {
        room = qMax(room, offset + length);
    }
    if (marks.colourBars) {
        room = qMax(room, offset + kPatchSize + 2.0);
    }
    if (marks.pageInformation) {
        room = qMax(room, offset + kInfoSize + 4.0);
    }

    return rewrite(
        in, out,
        [&](QPDF &pdf, QPDFPageDocumentHelper &documents, QString *why) {
            std::vector<QPDFPageObjectHelper> all = documents.getAllPages();
            if (all.empty()) {
                if (why) {
                    *why = i18n("The document has no pages.");
                }
                return false;
            }

            QPDFObjectHandle registrationInk = allInkColourSpace(pdf);
            QPDFObjectHandle font = marks.pageInformation ? markFont(pdf) : QPDFObjectHandle::newNull();

            for (size_t i = 0; i < all.size(); ++i) {
                QPDFPageObjectHelper &page = all[i];

                const QRectF oldMedia = PdfGeometry::mediaBoxOf(page);
                QRectF trim = pageBox(page, "/TrimBox");
                if (!trim.isValid()) {
                    trim = visibleBox(page);
                }

                // Nothing moves: the sheet is enlarged around the content where
                // it stands, so the page keeps its own coordinates and every
                // annotation on it stays put.
                const QRectF media = oldMedia.united(trim.adjusted(-room, -room, room, room));
                QPDFObjectHandle dictionary = page.getObjectHandle();
                dictionary.replaceKey("/MediaBox", QPDFObjectHandle::parse(boxArray(media)));
                // Marks nobody can see are not marks, so the visible area grows
                // with the sheet.
                dictionary.replaceKey("/CropBox", QPDFObjectHandle::parse(boxArray(media)));

                const int rotate = PdfGeometry::rotationOf(page);
                const QRectF shown = toDisplay(trim, media, rotate);
                const double left = shown.x();
                const double right = shown.x() + shown.width();
                const double bottom = shown.y();
                const double top = shown.y() + shown.height();

                QPDFObjectHandle resources = writableResources(page, "<< /ColorSpace << >> /Font << >> >>");
                int suffix = 1;
                const std::string inkName = resources.getUniqueResourceName("/PsAllInk", suffix);
                resources.getKey("/ColorSpace").replaceKey(inkName, registrationInk);

                std::string content = "q\n";
                content += displayPrefix(media, rotate);
                content += number(lineWidth) + " w 0 J 0 j\n";
                content += inkName + " CS 1 SCN\n";
                content += inkName + " cs 1 scn\n";

                if (marks.cropMarks) {
                    content += strokedLine(left - offset - length, bottom, left - offset, bottom);
                    content += strokedLine(left, bottom - offset - length, left, bottom - offset);
                    content += strokedLine(right + offset, bottom, right + offset + length, bottom);
                    content += strokedLine(right, bottom - offset - length, right, bottom - offset);
                    content += strokedLine(left - offset - length, top, left - offset, top);
                    content += strokedLine(left, top + offset, left, top + offset + length);
                    content += strokedLine(right + offset, top, right + offset + length, top);
                    content += strokedLine(right, top + offset, right, top + offset + length);
                }

                if (marks.registrationMarks) {
                    const double radius = length / 3.5;
                    const double reach = length / 2.0 - lineWidth;
                    const double away = offset + length / 2.0;
                    const double midX = (left + right) / 2.0;
                    const double midY = (bottom + top) / 2.0;
                    content += registrationMark(midX, bottom - away, radius, reach);
                    content += registrationMark(midX, top + away, radius, reach);
                    content += registrationMark(left - away, midY, radius, reach);
                    content += registrationMark(right + away, midY, radius, reach);
                }

                if (marks.colourBars) {
                    // Solids, black tints and the two-ink overprints, which is
                    // what a press operator reads density and registration off.
                    static const double patches[][4] = {
                        { 0.0, 0.0, 0.0, 1.0 },  { 1.0, 0.0, 0.0, 0.0 },  { 0.0, 1.0, 0.0, 0.0 },
                        { 0.0, 0.0, 1.0, 0.0 },  { 0.0, 0.0, 0.0, 0.75 }, { 0.0, 0.0, 0.0, 0.5 },
                        { 0.0, 0.0, 0.0, 0.25 }, { 1.0, 1.0, 0.0, 0.0 },  { 0.0, 1.0, 1.0, 0.0 },
                        { 1.0, 0.0, 1.0, 0.0 },
                    };
                    const int count = int(sizeof(patches) / sizeof(patches[0]));
                    // Narrower patches on a narrow page rather than a row that
                    // runs off past the corner marks.
                    const double patchSize = qMin(kPatchSize, shown.width() / count);
                    double x = (left + right) / 2.0 - count * patchSize / 2.0;
                    const double y = bottom - offset - patchSize;

                    content += "q\n";
                    for (const double *patch : patches) {
                        content += number(patch[0]) + " " + number(patch[1]) + " " + number(patch[2]) + " "
                            + number(patch[3]) + " k\n";
                        content += filledRect(QRectF(x, y, patchSize, patchSize));
                        x += patchSize;
                    }
                    content += "Q\n";
                }

                if (marks.pageInformation) {
                    const std::string fontName = resources.getUniqueResourceName("/PsMarkFont", suffix);
                    resources.getKey("/Font").replaceKey(fontName, font);

                    QString info = i18nc("Printer's mark identifying a proof: file, page, and time of output",
                                         "%1 · page %2 of %3 · %4", sourceName, QString::number(int(i) + 1),
                                         QString::number(int(all.size())), stamp);
                    // Truncated rather than allowed to run over the marks beside
                    // it; a proof with an unreadable corner is a proof someone
                    // has to ask about.
                    while (info.size() > 8 && Overlay::estimateTextWidth(info, kInfoSize) > shown.width()) {
                        info.chop(2);
                    }

                    content += "BT " + fontName + " " + number(kInfoSize) + " Tf " + number(left) + " "
                        + number(top + offset + 2.0) + " Td " + pdfString(info) + " Tj ET\n";
                }

                content += "Q\n";

                PdfGeometry::isolateExistingContent(page, dictionary);
                page.addPageContents(QPDFObjectHandle::newStream(&pdf, content), false);
            }
            return true;
        },
        error);
}

bool PageLayout::poster(const QString &in, const QString &out, int page, const QSizeF &sheetPoints,
                        double overlapPoints, bool marks, int *sheets, QString *error)
{
    if (sheets) {
        *sheets = 0;
    }
    if (sheetPoints.width() <= 1.0 || sheetPoints.height() <= 1.0) {
        if (error) {
            *error = i18n("The sheet size has to be at least one point in each direction.");
        }
        return false;
    }
    const double overlap = qMax(0.0, overlapPoints);
    if (overlap >= sheetPoints.width() / 2.0 || overlap >= sheetPoints.height() / 2.0) {
        if (error) {
            *error = i18n("The overlap has to be less than half the sheet, or the tiles never advance.");
        }
        return false;
    }

    return rewrite(
        in, out,
        [&, page](QPDF &pdf, QPDFPageDocumentHelper &documents, QString *why) {
            std::vector<QPDFPageObjectHelper> all = documents.getAllPages();
            if (page < 0 || page >= int(all.size())) {
                if (why) {
                    *why = i18n("There is no page %1 in this document.", page + 1);
                }
                return false;
            }

            QPDFPageObjectHelper &source = all[size_t(page)];
            const QRectF media = PdfGeometry::mediaBoxOf(source);
            const int rotate = PdfGeometry::rotationOf(source);
            const QRectF visible = visibleBox(source);

            QPDFObjectHandle form = source.getFormXObjectForPage(false);
            form.getDict().replaceKey("/BBox", QPDFObjectHandle::parse(boxArray(visible)));

            // Drawn in a space where the visible page starts at the origin, which
            // turns the tiling below into plain addition.
            QTransform toTile = pageToDisplay(media, rotate);
            const QRectF shown = toTile.mapRect(visible);
            toTile = toTile * QTransform::fromTranslate(-shown.x(), -shown.y());
            form.getDict().replaceKey("/Matrix", QPDFObjectHandle::parse(matrixArray(toTile)));

            const double sheetWidth = sheetPoints.width();
            const double sheetHeight = sheetPoints.height();
            const int columns
                = qMax(1, int(std::ceil((shown.width() - overlap) / (sheetWidth - overlap) - kBoxTolerance)));
            const int rows
                = qMax(1, int(std::ceil((shown.height() - overlap) / (sheetHeight - overlap) - kBoxTolerance)));

            // The tiles nearly always cover more than the page; sharing the
            // surplus between the outer sheets keeps the picture in the middle
            // instead of piling all the blank paper onto two edges.
            const double coveredWidth = columns * sheetWidth - (columns - 1) * overlap;
            const double coveredHeight = rows * sheetHeight - (rows - 1) * overlap;
            const double originX = -(coveredWidth - shown.width()) / 2.0;
            const double originY = -(coveredHeight - shown.height()) / 2.0;

            QPDFObjectHandle font = marks ? markFont(pdf) : QPDFObjectHandle::newNull();

            for (QPDFPageObjectHelper &existing : all) {
                documents.removePage(existing);
            }

            int sheetNumber = 0;
            for (int row = rows - 1; row >= 0; --row) {
                for (int column = 0; column < columns; ++column) {
                    ++sheetNumber;
                    const double tileX = originX + column * (sheetWidth - overlap);
                    const double tileY = originY + row * (sheetHeight - overlap);

                    QPDFObjectHandle sheet = QPDFObjectHandle::parse("<< /Type /Page >>");
                    sheet.replaceKey("/MediaBox",
                                     QPDFObjectHandle::parse(boxArray(QRectF(0.0, 0.0, sheetWidth, sheetHeight))));
                    sheet.replaceKey("/Resources", QPDFObjectHandle::parse("<< /XObject << >> /Font << >> >>"));

                    QPDFPageObjectHelper tile(pdf.makeIndirectObject(sheet));
                    QPDFObjectHandle resources = tile.getObjectHandle().getKey("/Resources");
                    resources.getKey("/XObject").replaceKey("/PsTile", form);

                    std::string content = "q 1 0 0 1 " + number(-tileX) + " " + number(-tileY) + " cm\n/PsTile Do\nQ\n";

                    if (marks) {
                        resources.getKey("/Font").replaceKey("/PsTileFont", font);

                        // A cut line on every edge a neighbour overlaps, and on
                        // no other: the outer edges of a poster are its edges.
                        content += "q 0.25 w 0.5 G [3 3] 0 d\n";
                        if (column > 0) {
                            content += strokedLine(overlap, 0.0, overlap, sheetHeight);
                        }
                        if (column < columns - 1) {
                            content += strokedLine(sheetWidth - overlap, 0.0, sheetWidth - overlap, sheetHeight);
                        }
                        if (row > 0) {
                            content += strokedLine(0.0, overlap, sheetWidth, overlap);
                        }
                        if (row < rows - 1) {
                            content += strokedLine(0.0, sheetHeight - overlap, sheetWidth, sheetHeight - overlap);
                        }
                        content += "Q\n";

                        const QString label
                            = i18nc("Label on one sheet of a tiled poster", "Sheet %1 of %2, column %3, row %4",
                                    QString::number(sheetNumber), QString::number(columns * rows),
                                    QString::number(column + 1), QString::number(rows - row));
                        content += "q 0.4 g BT /PsTileFont 7 Tf 6 6 Td " + pdfString(label) + " Tj ET Q\n";
                    }

                    tile.getObjectHandle().replaceKey("/Contents", QPDFObjectHandle::newStream(&pdf, content));
                    documents.addPage(tile, false);
                }
            }

            if (sheets) {
                *sheets = columns * rows;
            }
            return true;
        },
        error);
}

bool PageLayout::setLabels(const QString &in, const QString &out, const QVector<Labels> &ranges, QString *error)
{
    if (ranges.isEmpty()) {
        if (error) {
            *error = i18n("No page-numbering ranges were given.");
        }
        return false;
    }

    return rewrite(
        in, out,
        [&ranges](QPDF &pdf, QPDFPageDocumentHelper &documents, QString *why) {
            const int count = int(documents.getAllPages().size());

            QVector<Labels> sorted = ranges;
            std::stable_sort(sorted.begin(), sorted.end(),
                             [](const Labels &a, const Labels &b) { return a.fromPage < b.fromPage; });

            for (const Labels &range : sorted) {
                if (range.fromPage < 0 || range.fromPage >= count) {
                    if (why) {
                        *why = i18n("A numbering range starts at page %1, which this document does not have.",
                                    range.fromPage + 1);
                    }
                    return false;
                }
                if (range.startAt < 1) {
                    if (why) {
                        *why = i18n("Page numbering has to start at one or more.");
                    }
                    return false;
                }
            }

            QPDFNumberTreeObjectHelper tree = QPDFNumberTreeObjectHelper::newEmpty(pdf);

            // The tree has to describe page zero. Where the first range starts
            // later, the pages before it would otherwise have no entry at all,
            // and a reader that finds none has nothing to count from.
            if (sorted.constFirst().fromPage > 0) {
                tree.insert(0, QPDFPageLabelDocumentHelper::pageLabelDict(pl_digits, 1, ""));
            }

            for (const Labels &range : sorted) {
                QPDFObjectHandle entry = QPDFPageLabelDocumentHelper::pageLabelDict(
                    labelStyle(range.style), range.startAt, range.prefix.toStdString());
                if (!range.prefix.isEmpty()) {
                    // pageLabelDict writes the prefix as raw bytes; anything
                    // outside ASCII has to go in as a proper text string or the
                    // reader shows mojibake.
                    entry.replaceKey("/P", QPDFObjectHandle::newUnicodeString(range.prefix.toStdString()));
                }
                tree.insert(range.fromPage, entry);
            }

            pdf.getRoot().replaceKey("/PageLabels", tree.getObjectHandle());
            return true;
        },
        error);
}

QStringList PageLayout::labelsOf(const QString &pdf, QString *error)
{
    QStringList labels;

    try {
        QPDF document;
        PdfFile::open(document, pdf);

        QPDFPageLabelDocumentHelper helper(document);
        if (!helper.hasPageLabels()) {
            return {};
        }

        const int count = int(QPDFPageDocumentHelper(document).getAllPages().size());
        labels.reserve(count);
        for (int i = 0; i < count; ++i) {
            labels.append(renderLabel(helper.getLabelForPage(i)));
        }
    } catch (const std::exception &e) {
        if (error) {
            *error = QString::fromUtf8(e.what());
        }
        return {};
    }

    return labels;
}

QVector<int> PageLayout::findBlankPages(const QString &pdf, double inkThreshold, QString *error)
{
    QVector<int> blanks;
    const int allowed = int(std::floor(qMax(0.0, inkThreshold)));

    try {
        QPDF document;
        PdfFile::open(document, pdf);

        std::vector<QPDFPageObjectHelper> pages = QPDFPageDocumentHelper(document).getAllPages();
        for (size_t i = 0; i < pages.size(); ++i) {
            InkCounter counter;
            try {
                pages[i].parseContents(&counter);
            } catch (const std::exception &) {
                // A stream this cannot read is not a stream this can call empty.
                continue;
            }
            if (counter.marks() <= allowed) {
                blanks.append(int(i));
            }
        }
    } catch (const std::exception &e) {
        if (error) {
            *error = QString::fromUtf8(e.what());
        }
        return {};
    }

    return blanks;
}

bool PageLayout::removePages(const QString &in, const QString &out, const QVector<int> &pages, QString *error)
{
    if (pages.isEmpty()) {
        if (error) {
            *error = i18n("There are no pages to remove.");
        }
        return false;
    }

    const QVector<int> wanted = tidyPageList(pages);

    return rewrite(
        in, out,
        [&wanted](QPDF &, QPDFPageDocumentHelper &documents, QString *why) {
            std::vector<QPDFPageObjectHelper> all = documents.getAllPages();
            for (const int index : wanted) {
                if (index < 0 || index >= int(all.size())) {
                    if (why) {
                        *why = i18n("There is no page %1 in this document.", index + 1);
                    }
                    return false;
                }
            }
            if (wanted.size() >= int(all.size())) {
                if (why) {
                    *why = i18n("Removing those pages would leave no document at all.");
                }
                return false;
            }
            for (const int index : wanted) {
                documents.removePage(all[size_t(index)]);
            }
            return true;
        },
        error);
}

bool PageLayout::reorderForBinding(const QString &in, const QString &out, bool toPrinterSpreads, QString *error)
{
    return rewrite(
        in, out,
        [toPrinterSpreads](QPDF &pdf, QPDFPageDocumentHelper &documents, QString *why) {
            std::vector<QPDFPageObjectHelper> all = documents.getAllPages();
            const int count = int(all.size());
            if (count < 4) {
                if (why) {
                    *why = i18n("A saddle-stitched booklet needs at least four pages.");
                }
                return false;
            }

            QVector<QPDFPageObjectHelper> pool;
            pool.reserve(count);
            for (QPDFPageObjectHelper &page : all) {
                pool.append(page);
            }

            // A folded sheet carries four pages, so a booklet only exists in
            // multiples of four. The blanks belong at the back of the reader
            // order, where they end up inside the last fold.
            const int padded = ((count + 3) / 4) * 4;
            const QRectF lastSize = PdfGeometry::mediaBoxOf(all.back());
            while (pool.size() < padded) {
                pool.append(makeBlankPage(pdf, lastSize));
            }

            const QVector<int> order = toPrinterSpreads ? spreadOrder(padded) : invertOrder(spreadOrder(padded));

            QVector<QPDFPageObjectHelper> ordered;
            ordered.reserve(padded);
            for (const int slot : order) {
                ordered.append(pool.at(slot));
            }

            for (QPDFPageObjectHelper &page : all) {
                documents.removePage(page);
            }
            for (QPDFPageObjectHelper &page : ordered) {
                documents.addPage(page, false);
            }
            return true;
        },
        error);
}

QStringList PageLayout::limitations()
{
    return {
        i18n("A blank page is found by reading the page's instructions, not by looking at it. A page that paints "
             "white over white, or one whose only content is an annotation, counts as painted and is not reported."),
        i18n("Resizing scales the page's content as a whole. Line widths, hairlines and font sizes scale with it, so "
             "a page reduced far enough can end up with rules too fine for a press to hold."),
        i18n("Splitting a page gives each half a window onto the same content. Both halves inherit the same "
             "annotations, and an annotation that straddles the cut appears on both."),
        i18n("Printer's marks are drawn in a /Separation /All colour space. A workflow that flattens spot colours "
             "before output may convert them to black, which still prints but no longer serves for registration."),
        i18n("Marks are placed around the trim box. A document whose trim box is missing gets them around what the "
             "page shows instead, which is right for most files and wrong for one that was already imposed."),
        i18n("Page labels are written as far as the specification describes them. Readers differ in what they show "
             "for a range whose style is left out, and this cannot make them agree."),
        i18n("Reordering for binding changes the page order only. Pairing the pages onto sheets and adding the fold "
             "is a separate step."),
        i18n("Overlaying two documents draws one over the other; it does not merge their form fields, and two forms "
             "with a field of the same name keep only the base document's."),
    };
}

} // namespace ps
