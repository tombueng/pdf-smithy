/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#include "Convert.h"

#include "PdfFile.h"
#include "PdfGeometry.h"
#include "TextEdit.h"

#include <KLocalizedString>

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QImage>
#include <QLineF>
#include <QProcess>
#include <QRectF>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QTransform>

#include <qpdf/Pl_Buffer.hh>
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>
#include <qpdf/QPDFTokenizer.hh>
#include <qpdf/QPDFWriter.hh>
#include <qpdf/QPDFXRefEntry.hh>

#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace Qt::Literals::StringLiterals;

namespace ps {

namespace {

// ── How the layout reasoning is tuned ────────────────────────────────────────
//
// Every one of these is a threshold on a measurement, and every one of them is
// a judgement that some document will disagree with. They are gathered here
// rather than sprinkled through the code so that the judgements can be read in
// one place.

/** A run's box is this many times its font size tall, as TextEdit builds it. */
constexpr double BoxToFontSize = 1.2;

/** How deep the recursive cut may go before it gives up and emits what is left. */
constexpr int MaxCutDepth = 64;

/** A horizontal band of white this many font sizes tall separates two regions. */
constexpr double HorizontalCutFactor = 1.2;

/** A gutter has to be at least this wide, in font sizes and in points. */
constexpr double GutterFactor = 1.2;
constexpr double GutterMinimumPoints = 6.0;

/** Columns are only looked for in a region at least this many font sizes tall. */
constexpr double GutterMinimumHeightFactor = 4.0;

/** Each side of a gutter has to span this much of the region's height. */
constexpr double ColumnHeightShare = 0.6;

/** A gap between two runs wider than this many font sizes is a word space. */
constexpr double SpaceFactor = 0.2;

/** A paragraph set this much larger than the document's body size is a heading. */
constexpr double HeadingFactor = 1.15;

/** Deeper than this and Markdown has no heading level left. */
constexpr int MaxHeadingLevel = 6;

/** A gap between two runs wider than this many font sizes divides two table cells. */
constexpr double CellGapFactor = 1.3;
constexpr double CellGapMinimumPoints = 6.0;

/** Fewer rows than this is a coincidence rather than a table. */
constexpr int MinimumTableRows = 3;

/**
 * Above this, cells fill their columns like set text and the candidate is prose.
 *
 * Two columns of justified prose line up exactly as well as a table does; what
 * tells them apart is that a line of prose runs to the far edge of its measure
 * and a table cell does not.
 */
constexpr double CellFillCeiling = 0.75;

/** Pictures smaller than this in either direction are rules and ornaments. */
constexpr double SmallestInterestingPicture = 24.0;

/** Beyond this many pixels wide a picture is downscaled before it is embedded. */
constexpr int WidestEmbeddedPicture = 1600;

/** Converting a long document for print is not quick; give it room. */
constexpr int GhostscriptTimeoutMs = 15 * 60 * 1000;

// ── Page coordinates ─────────────────────────────────────────────────────────

/**
 * The two y edges of a box, named after the page rather than after Qt.
 *
 * Page coordinates count y upwards and QRectF counts it downwards, so a box's
 * `bottom()` is the edge nearest the top of the page. Naming them this way round
 * is the only way the rest of this file stays readable.
 */
double topOf(const QRectF &box)
{
    return box.bottom();
}

double baseOf(const QRectF &box)
{
    return box.top();
}

/**
 * How big the letters of a run actually are.
 *
 * Not `Run::fontSize`, which is what the `Tf` operator said, and which a great
 * many producers set to one and then scale in the text matrix: every run of the
 * magazine this was developed against reports a font size of exactly 1.0. The
 * height of the run's box has the matrix in it already, and TextEdit builds that
 * box at a known multiple of the size, so the box is the only honest measure.
 */
double sizeOf(const TextEdit::Run &run)
{
    const double fromBox = run.rect.height() / BoxToFontSize;
    return fromBox > 0.1 ? fromBox : std::max(run.fontSize, 1.0);
}

// ── What the layout is made of ───────────────────────────────────────────────

/** One text-showing operation, once its geometry has been made sense of. */
struct Piece {
    QString text;
    QRectF rect;
    double size = 10.0;
    bool bold = false;
    bool italic = false;
};

/** Pieces that a reader would see as one line, left to right. */
struct Line {
    QVector<Piece> pieces;
    double baseline = 0.0;
    QRectF box;

    /** The size most of the line's characters are set in. */
    double size = 10.0;
};

/** Lines that a reader would see as one paragraph. */
struct Paragraph {
    QVector<Line> lines;
    QRectF box;
    double size = 10.0;
};

/** A picture on the page, already turned into something an HTML file can carry. */
struct Picture {
    QRectF rect;
    QString dataUri;
    QSize pixels;
};

struct PageAnalysis {
    int page = 0;
    QSizeF size;

    /** In reading order. */
    QVector<Paragraph> paragraphs;

    /** Every line on the page, ignoring columns: what table detection works on. */
    QVector<Line> lines;

    QVector<Picture> pictures;

    /** Axis-aligned rules, which is what a table's ruling looks like from here. */
    QVector<QLineF> rules;
};

struct Analysis {
    QVector<PageAnalysis> pages;

    /** The size most of the document's text is set in. */
    double bodySize = 10.0;

    /** The heading sizes found, largest first, which is where the levels come from. */
    QVector<double> headingSizes;
};

// ── Fonts, as far as emphasis is concerned ───────────────────────────────────

/** The `/BaseFont` behind each resource name on a page, e.g. "/F1" to "Minion-Bold". */
QHash<QString, QString> baseFontsOf(QPDFPageObjectHelper &page)
{
    QHash<QString, QString> names;
    QPDFObjectHandle resources = page.getAttribute("/Resources", false);
    if (!resources.isDictionary()) {
        return names;
    }
    QPDFObjectHandle fonts = resources.getKey("/Font");
    if (!fonts.isDictionary()) {
        return names;
    }
    for (const auto &[key, font] : fonts.getDictAsMap()) {
        if (!font.isDictionary()) {
            continue;
        }
        QPDFObjectHandle base = font.getKey("/BaseFont");
        if (base.isName()) {
            names.insert(QString::fromStdString(key), QString::fromStdString(base.getName()));
        }
    }
    return names;
}

/**
 * Whether a font's own name claims it is bold or slanted.
 *
 * Inference, and the place where Markdown emphasis will be wrong: a family whose
 * bold face is called "Black" or "Heavy" is caught here, and one named after its
 * designer or given a foundry's private label is not. There is nothing else in a
 * PDF to ask: the weight is not recorded anywhere a reader could trust.
 */
void emphasisOf(const QString &baseFont, bool *bold, bool *italic)
{
    QString name = baseFont;
    const qsizetype plus = name.indexOf(u'+'); // The six-letter subsetting tag.
    if (plus >= 0) {
        name = name.mid(plus + 1);
    }
    name = name.toLower().remove(u'-').remove(u' ').remove(u'/').remove(u'_').remove(u',');

    *bold = name.contains(u"bold"_s) || name.contains(u"black"_s) || name.contains(u"heavy"_s)
        || name.contains(u"semibold"_s) || name.contains(u"demibold"_s);
    *italic = name.contains(u"italic"_s) || name.contains(u"oblique"_s);
}

// ── Recursive cutting ────────────────────────────────────────────────────────

using Region = QVector<qsizetype>;

QRectF boxOf(const QVector<Piece> &pieces, const Region &region)
{
    QRectF box;
    bool first = true;
    for (const qsizetype index : region) {
        // A flag rather than isNull(), because a run of a single full stop has a
        // box a fraction of a point wide and QRectF calls that null.
        box = first ? pieces.at(index).rect : box.united(pieces.at(index).rect);
        first = false;
    }
    return box;
}

double medianSize(const QVector<Piece> &pieces, const Region &region)
{
    if (region.isEmpty()) {
        return 10.0;
    }
    QVector<double> sizes;
    sizes.reserve(region.size());
    for (const qsizetype index : region) {
        sizes.append(pieces.at(index).size);
    }
    std::nth_element(sizes.begin(), sizes.begin() + sizes.size() / 2, sizes.end());
    return std::max(sizes.at(sizes.size() / 2), 0.5);
}

/** The intervals a set of runs occupies on one axis, overlaps merged away. */
QVector<QPointF> mergedSpans(QVector<QPointF> spans)
{
    std::sort(spans.begin(), spans.end(), [](const QPointF &a, const QPointF &b) { return a.x() < b.x(); });
    QVector<QPointF> merged;
    for (const QPointF &span : std::as_const(spans)) {
        if (!merged.isEmpty() && span.x() <= merged.last().y()) {
            merged.last().setY(std::max(merged.last().y(), span.y()));
        } else {
            merged.append(span);
        }
    }
    return merged;
}

/**
 * Whether the lines on one side of a candidate gutter run to the far edge of it.
 *
 * The test that keeps the date in the corner of a letter from becoming a column:
 * set text fills its measure, and a stray line does not.
 */
bool reachesFarSide(const QVector<Piece> &pieces, const Region &region, const QRectF &box)
{
    if (box.width() <= 0.0 || region.isEmpty()) {
        return false;
    }
    const double edge = box.right() - 0.25 * box.width();
    qsizetype reaching = 0;
    for (const qsizetype index : region) {
        if (pieces.at(index).rect.right() >= edge) {
            ++reaching;
        }
    }
    return reaching * 5 >= region.size() * 2;
}

/**
 * The x of a gutter between two columns, or NaN when there is none.
 *
 * A gutter is a band of white space that no run crosses, wide enough not to be a
 * word space, with something on both sides of it that spans nearly the whole
 * height of the region and reads like set text. All four of those conditions are
 * needed: the first two alone find the gaps between a table's columns and the
 * space either side of a centred heading.
 */
double findGutter(const QVector<Piece> &pieces, const Region &region, double fontSize, const QRectF &box)
{
    if (region.size() < 4 || box.height() < GutterMinimumHeightFactor * fontSize) {
        return std::nan("");
    }

    QVector<QPointF> spans;
    spans.reserve(region.size());
    for (const qsizetype index : region) {
        spans.append(QPointF(pieces.at(index).rect.left(), pieces.at(index).rect.right()));
    }
    const QVector<QPointF> merged = mergedSpans(std::move(spans));

    QVector<QPointF> gaps;
    for (qsizetype i = 0; i + 1 < merged.size(); ++i) {
        gaps.append(QPointF(merged.at(i).y(), merged.at(i + 1).x()));
    }
    // Widest first: where a page really has two columns, their gutter is the
    // widest white band across it, and trying narrower ones first only finds the
    // space after a full stop.
    std::sort(gaps.begin(), gaps.end(),
              [](const QPointF &a, const QPointF &b) { return (a.y() - a.x()) > (b.y() - b.x()); });

    const double minimum = std::max(GutterMinimumPoints, GutterFactor * fontSize);
    for (const QPointF &gap : std::as_const(gaps)) {
        if (gap.y() - gap.x() < minimum) {
            break;
        }
        const double split = (gap.x() + gap.y()) / 2.0;
        Region left;
        Region right;
        for (const qsizetype index : region) {
            (pieces.at(index).rect.center().x() < split ? left : right).append(index);
        }
        if (left.size() < 2 || right.size() < 2) {
            continue;
        }
        const QRectF leftBox = boxOf(pieces, left);
        const QRectF rightBox = boxOf(pieces, right);
        if (leftBox.height() < ColumnHeightShare * box.height()
            || rightBox.height() < ColumnHeightShare * box.height()) {
            continue;
        }
        if (!reachesFarSide(pieces, left, leftBox) || !reachesFarSide(pieces, right, rightBox)) {
            continue;
        }
        return split;
    }
    return std::nan("");
}

/** The y of the widest band of white across a region, or NaN when there is none. */
double findHorizontalSplit(const QVector<Piece> &pieces, const Region &region, double fontSize)
{
    QVector<QPointF> spans;
    spans.reserve(region.size());
    for (const qsizetype index : region) {
        spans.append(QPointF(baseOf(pieces.at(index).rect), topOf(pieces.at(index).rect)));
    }
    const QVector<QPointF> merged = mergedSpans(std::move(spans));

    double widest = 0.0;
    double split = std::nan("");
    for (qsizetype i = 0; i + 1 < merged.size(); ++i) {
        const double width = merged.at(i + 1).x() - merged.at(i).y();
        if (width > widest) {
            widest = width;
            split = (merged.at(i).y() + merged.at(i + 1).x()) / 2.0;
        }
    }
    return widest >= HorizontalCutFactor * fontSize ? split : std::nan("");
}

/**
 * Splits a region until nothing more can be split, in reading order.
 *
 * Across the page first, then down it: the horizontal cut peels off the headline
 * and the running foot, and only what is left of the middle is asked whether it
 * has a gutter. Cutting at the single widest gap and recursing, rather than at
 * every gap at once, is what keeps a paragraph break that happens to fall at the
 * same height in both columns from splitting the article into bands and reading
 * them left, right, left, right.
 */
void cutRegion(const QVector<Piece> &pieces, const Region &region, int depth, QVector<Region> &out)
{
    if (region.size() <= 1 || depth >= MaxCutDepth) {
        if (!region.isEmpty()) {
            out.append(region);
        }
        return;
    }

    const double fontSize = medianSize(pieces, region);
    const QRectF box = boxOf(pieces, region);

    const double gutter = findGutter(pieces, region, fontSize, box);
    if (!std::isnan(gutter)) {
        Region left;
        Region right;
        for (const qsizetype index : region) {
            (pieces.at(index).rect.center().x() < gutter ? left : right).append(index);
        }
        cutRegion(pieces, left, depth + 1, out);
        cutRegion(pieces, right, depth + 1, out);
        return;
    }

    const double split = findHorizontalSplit(pieces, region, fontSize);
    if (!std::isnan(split)) {
        Region above;
        Region below;
        for (const qsizetype index : region) {
            (pieces.at(index).rect.center().y() > split ? above : below).append(index);
        }
        if (!above.isEmpty() && !below.isEmpty()) {
            cutRegion(pieces, above, depth + 1, out);
            cutRegion(pieces, below, depth + 1, out);
            return;
        }
    }

    out.append(region);
}

// ── Lines and paragraphs ─────────────────────────────────────────────────────

/** Pieces of a region gathered into lines, top of the page first. */
QVector<Line> buildLines(const QVector<Piece> &pieces, const Region &region, double tolerance)
{
    QVector<qsizetype> ordered = region;
    std::sort(ordered.begin(), ordered.end(), [&pieces](qsizetype a, qsizetype b) {
        const double first = baseOf(pieces.at(a).rect) + 0.25 * pieces.at(a).size;
        const double second = baseOf(pieces.at(b).rect) + 0.25 * pieces.at(b).size;
        if (std::abs(first - second) > 0.001) {
            return first > second;
        }
        return pieces.at(a).rect.left() < pieces.at(b).rect.left();
    });

    QVector<Line> lines;
    for (const qsizetype index : std::as_const(ordered)) {
        const Piece &piece = pieces.at(index);
        const double baseline = baseOf(piece.rect) + 0.25 * piece.size;
        if (lines.isEmpty() || std::abs(lines.last().baseline - baseline) > tolerance) {
            Line line;
            line.baseline = baseline;
            line.box = piece.rect;
            line.pieces.append(piece);
            lines.append(line);
        } else {
            lines.last().pieces.append(piece);
            lines.last().box = lines.last().box.united(piece.rect);
        }
    }

    for (Line &line : lines) {
        std::sort(line.pieces.begin(), line.pieces.end(),
                  [](const Piece &a, const Piece &b) { return a.rect.left() < b.rect.left(); });
        // The size of the piece carrying most of the characters, so that a
        // footnote marker does not decide how big a line of body text is.
        qsizetype longest = 0;
        for (qsizetype i = 1; i < line.pieces.size(); ++i) {
            if (line.pieces.at(i).text.size() > line.pieces.at(longest).text.size()) {
                longest = i;
            }
        }
        line.size = line.pieces.at(longest).size;
    }
    return lines;
}

/**
 * Lines gathered into paragraphs.
 *
 * Three things end a paragraph: the lines moving further apart than the leading
 * of the text, the size of the text changing, and the next line being indented
 * under a line that ran to the end of its measure. The last of those is the one
 * that reads a book set without spaces between paragraphs.
 */
QVector<Paragraph> buildParagraphs(const QVector<Line> &lines, double paragraphFactor)
{
    QVector<Paragraph> paragraphs;
    for (const Line &line : lines) {
        bool fresh = paragraphs.isEmpty();
        if (!fresh) {
            const Line &previous = paragraphs.last().lines.last();
            const double reference = std::max(previous.size, line.size);
            const double step = previous.baseline - line.baseline;
            const bool spread = step > paragraphFactor * reference;
            const bool resized = std::abs(previous.size - line.size) > 0.2 * reference;
            const bool indented = line.box.left() > previous.box.left() + reference
                && previous.box.right() >= paragraphs.last().box.right() - 0.1 * reference;
            fresh = spread || resized || indented;
        }
        if (fresh) {
            Paragraph paragraph;
            paragraph.box = line.box;
            paragraph.lines.append(line);
            paragraph.size = line.size;
            paragraphs.append(paragraph);
        } else {
            paragraphs.last().lines.append(line);
            paragraphs.last().box = paragraphs.last().box.united(line.box);
            paragraphs.last().size = std::max(paragraphs.last().size, line.size);
        }
    }
    return paragraphs;
}

// ── Turning pieces back into words ───────────────────────────────────────────

/** One stretch of text that is all in the same face. */
struct Segment {
    QString text;
    bool bold = false;
    bool italic = false;
};

/**
 * Whether two neighbouring pieces need a space between them.
 *
 * The gap is compared with the font size rather than with a measured space
 * width, because the space between "Arbeits" and the hyphen that follows it is
 * zero and the space between two words is a fifth of the size or more. Getting
 * this wrong in one direction glues words together and in the other breaks
 * hyphenated ones apart.
 */
bool needsSpace(const QString &before, const Piece &piece, double cursor)
{
    if (before.isEmpty() || before.endsWith(u' ') || piece.text.startsWith(u' ')) {
        return false;
    }
    return piece.rect.left() - cursor > SpaceFactor * piece.size;
}

QVector<Segment> segmentsOfLine(const Line &line)
{
    QVector<Segment> segments;
    QString sofar;
    double cursor = 0.0;
    for (const Piece &piece : line.pieces) {
        QString text = piece.text;
        if (needsSpace(sofar, piece, cursor)) {
            text.prepend(u' ');
        }
        sofar += text;
        cursor = piece.rect.right();
        if (!segments.isEmpty() && segments.last().bold == piece.bold && segments.last().italic == piece.italic) {
            segments.last().text += text;
        } else {
            segments.append(Segment { text, piece.bold, piece.italic });
        }
    }
    return segments;
}

QString textOfLine(const Line &line)
{
    QString text;
    double cursor = 0.0;
    for (const Piece &piece : line.pieces) {
        if (needsSpace(text, piece, cursor)) {
            text.append(u' ');
        }
        text += piece.text;
        cursor = piece.rect.right();
    }
    return text.trimmed();
}

/**
 * Whether a hyphen at the end of a line was put there by the typesetter.
 *
 * A word broken across a line is joined back up when the next line starts in
 * lower case, which is right nearly always in German and English. It is wrong
 * for a compound whose hyphen was real and whose second half happens to be
 * lower case, and there is nothing in the file that could tell the difference.
 */
bool hyphenIsABreak(const QString &before, const QString &after)
{
    if (!before.endsWith(u'-') || before.size() < 2 || after.isEmpty()) {
        return false;
    }
    return before.at(before.size() - 2).isLetter() && after.at(0).isLower();
}

/** A paragraph's lines joined into flowing text, hyphenated breaks put back together. */
QVector<Segment> reflow(const QVector<Line> &lines)
{
    QVector<Segment> flowing;
    const auto append = [&flowing](const Segment &segment) {
        if (segment.text.isEmpty()) {
            return;
        }
        if (!flowing.isEmpty() && flowing.last().bold == segment.bold && flowing.last().italic == segment.italic) {
            flowing.last().text += segment.text;
        } else {
            flowing.append(segment);
        }
    };

    for (const Line &line : lines) {
        QVector<Segment> segments = segmentsOfLine(line);
        if (segments.isEmpty()) {
            continue;
        }
        segments.first().text = segments.first().text.trimmed();
        segments.last().text = QString(segments.last().text).replace(QRegularExpression(u"\\s+$"_s), QString());

        if (!flowing.isEmpty()) {
            const QString joined = flowing.last().text;
            QString next;
            for (const Segment &segment : std::as_const(segments)) {
                next += segment.text;
            }
            if (hyphenIsABreak(joined, next)) {
                flowing.last().text.chop(1);
            } else if (!joined.endsWith(u' ') && !next.startsWith(u' ')) {
                flowing.last().text += u' ';
            }
        }
        for (const Segment &segment : std::as_const(segments)) {
            append(segment);
        }
    }
    return flowing;
}

QString plainText(const QVector<Segment> &segments)
{
    QString text;
    for (const Segment &segment : segments) {
        text += segment.text;
    }
    return text.trimmed();
}

// ── Lists ───────────────────────────────────────────────────────────────────

/**
 * The bullet or number at the start of a line, if there is one.
 *
 * @p number is set to the number of an ordered item and to zero for a bullet.
 * Inference again, and the sentence "1902. Ein gutes Jahr" is where it fails.
 */
bool listMarkerOf(const QString &text, int *number, qsizetype *length)
{
    static const QRegularExpression bullet(u"^([•·–⁃▪∙−●°]|--?)\\s+"_s);
    static const QRegularExpression ordered(u"^(\\d{1,3})[.)]\\s+"_s);

    const QRegularExpressionMatch bulletMatch = bullet.match(text);
    if (bulletMatch.hasMatch()) {
        *number = 0;
        *length = bulletMatch.capturedLength(0);
        return true;
    }
    const QRegularExpressionMatch orderedMatch = ordered.match(text);
    if (orderedMatch.hasMatch()) {
        *number = orderedMatch.captured(1).toInt();
        *length = orderedMatch.capturedLength(0);
        return true;
    }
    return false;
}

/** One item of a list, with the lines that were wrapped under it folded in. */
struct Item {
    int number = 0;
    QVector<Line> lines;
};

bool paragraphIsAList(const Paragraph &paragraph, QVector<Item> *items)
{
    bool any = false;
    for (const Line &line : paragraph.lines) {
        int number = 0;
        qsizetype length = 0;
        if (listMarkerOf(textOfLine(line), &number, &length)) {
            Item item;
            item.number = number;
            item.lines.append(line);
            items->append(item);
            any = true;
        } else if (!items->isEmpty()) {
            items->last().lines.append(line);
        } else {
            // Text before the first bullet is not part of the list, and folding
            // it in would silently promote it to one.
            return false;
        }
    }
    return any;
}

/** An item's text with its marker taken off, so nothing is emitted twice. */
QVector<Segment> itemSegments(const Item &item)
{
    QVector<Segment> segments = reflow(item.lines);
    if (segments.isEmpty()) {
        return segments;
    }
    int number = 0;
    qsizetype length = 0;
    if (listMarkerOf(plainText(segments), &number, &length)) {
        qsizetype remaining = length;
        while (remaining > 0 && !segments.isEmpty()) {
            const qsizetype take = std::min(remaining, segments.first().text.size());
            segments.first().text.remove(0, take);
            remaining -= take;
            if (segments.first().text.isEmpty()) {
                segments.removeFirst();
            }
        }
    }
    return segments;
}

// ── Pictures and rules ───────────────────────────────────────────────────────

QStringList filtersOf(QPDFObjectHandle dict)
{
    QStringList names;
    QPDFObjectHandle filter = dict.isDictionary() ? dict.getKey("/Filter") : QPDFObjectHandle::newNull();
    if (filter.isName()) {
        names << QString::fromStdString(filter.getName());
    } else if (filter.isArray()) {
        for (int i = 0; i < filter.getArrayNItems(); ++i) {
            QPDFObjectHandle item = filter.getArrayItem(i);
            if (item.isName()) {
                names << QString::fromStdString(item.getName());
            }
        }
    }
    return names;
}

int componentsOf(QPDFObjectHandle colourSpace)
{
    if (colourSpace.isName()) {
        const std::string name = colourSpace.getName();
        if (name == "/DeviceGray" || name == "/G" || name == "/CalGray") {
            return 1;
        }
        return (name == "/DeviceRGB" || name == "/RGB" || name == "/CalRGB") ? 3 : 0;
    }
    if (colourSpace.isArray() && colourSpace.getArrayNItems() >= 2) {
        QPDFObjectHandle family = colourSpace.getArrayItem(0);
        const std::string name = family.isName() ? family.getName() : std::string();
        if (name == "/ICCBased") {
            QPDFObjectHandle stream = colourSpace.getArrayItem(1);
            QPDFObjectHandle n = stream.isStream() ? stream.getDict().getKey("/N") : QPDFObjectHandle::newNull();
            const int components = n.isInteger() ? n.getIntValueAsInt() : 0;
            return (components == 1 || components == 3) ? components : 0;
        }
        if (name == "/CalGray") {
            return 1;
        }
        if (name == "/CalRGB") {
            return 3;
        }
    }
    return 0;
}

/** Raw stream bytes with everything QPDF knows how to unwrap taken off. */
QByteArray unwrapped(QPDFObjectHandle image, qpdf_stream_decode_level_e level, bool *filtered)
{
    Pl_Buffer buffer("picture");
    bool attempted = false;
    if (!image.pipeStreamData(&buffer, &attempted, 0, level, true, false)) {
        return {};
    }
    *filtered = attempted;
    const auto data = buffer.getBufferSharedPointer();
    return QByteArray(reinterpret_cast<const char *>(data->getBuffer()), qsizetype(data->getSize()));
}

QImage decodeImage(QPDFObjectHandle image)
{
    QPDFObjectHandle dict = image.getDict();
    const QStringList filters = filtersOf(dict);
    QPDFObjectHandle widthKey = dict.getKey("/Width");
    QPDFObjectHandle heightKey = dict.getKey("/Height");
    const int width = widthKey.isInteger() ? widthKey.getIntValueAsInt() : 0;
    const int height = heightKey.isInteger() ? heightKey.getIntValueAsInt() : 0;
    if (width <= 0 || height <= 0) {
        return {};
    }

    if (filters.contains(u"/DCTDecode"_s)) {
        bool filtered = false;
        const QByteArray jpeg = unwrapped(image, qpdf_dl_specialized, &filtered);
        return QImage::fromData(jpeg);
    }

    const int components = componentsOf(dict.getKey("/ColorSpace"));
    QPDFObjectHandle bits = dict.getKey("/BitsPerComponent");
    const int bitsPerComponent = bits.isInteger() ? bits.getIntValueAsInt() : 8;

    bool filtered = false;
    const QByteArray samples = unwrapped(image, qpdf_dl_all, &filtered);
    if (samples.isEmpty() || (!filtered && !filters.isEmpty())) {
        return {};
    }
    const auto *bytes = reinterpret_cast<const uchar *>(samples.constData());

    if (components == 1 && bitsPerComponent == 1) {
        const qsizetype stride = (qsizetype(width) + 7) / 8;
        if (samples.size() < stride * height) {
            return {};
        }
        QImage mono(bytes, width, height, stride, QImage::Format_Mono);
        mono.setColorTable({ qRgb(0, 0, 0), qRgb(255, 255, 255) });
        QPDFObjectHandle decode = dict.getKey("/Decode");
        if (decode.isArray() && decode.getArrayNItems() == 2 && PdfGeometry::numberAt(decode, 0, 0.0) > 0.5) {
            mono.setColorTable({ qRgb(255, 255, 255), qRgb(0, 0, 0) });
        }
        return mono.convertToFormat(QImage::Format_RGB32);
    }
    if (components == 1 && bitsPerComponent == 8) {
        if (samples.size() < qsizetype(width) * height) {
            return {};
        }
        return QImage(bytes, width, height, width, QImage::Format_Grayscale8).convertToFormat(QImage::Format_RGB32);
    }
    if (components == 3 && bitsPerComponent == 8) {
        const qsizetype stride = qsizetype(width) * 3;
        if (samples.size() < stride * height) {
            return {};
        }
        return QImage(bytes, width, height, stride, QImage::Format_RGB888).convertToFormat(QImage::Format_RGB32);
    }
    return {};
}

/**
 * A picture as a data URI, or empty when it cannot be read.
 *
 * A JPEG small enough to carry as it stands is carried as it stands: decoding
 * and re-encoding it would throw away quality for nothing. Anything else is
 * decoded, scaled down if it is enormous, and written out afresh, because a
 * three-thousand-pixel scan in a data URI makes an HTML file nobody can open.
 */
QString pictureDataUri(QPDFObjectHandle image, QSize *pixels)
{
    QPDFObjectHandle dict = image.getDict();
    const QStringList filters = filtersOf(dict);
    QPDFObjectHandle widthKey = dict.getKey("/Width");
    const int width = widthKey.isInteger() ? widthKey.getIntValueAsInt() : 0;
    const bool masked = dict.hasKey("/SMask") || dict.hasKey("/Mask");

    if (filters.contains(u"/DCTDecode"_s) && !masked && width > 0 && width <= WidestEmbeddedPicture) {
        bool filtered = false;
        const QByteArray jpeg = unwrapped(image, qpdf_dl_specialized, &filtered);
        if (!jpeg.isEmpty()) {
            QPDFObjectHandle heightKey = dict.getKey("/Height");
            *pixels = QSize(width, heightKey.isInteger() ? heightKey.getIntValueAsInt() : 0);
            return u"data:image/jpeg;base64,"_s + QString::fromLatin1(jpeg.toBase64());
        }
    }

    QImage decoded = decodeImage(image);
    if (decoded.isNull()) {
        return {};
    }
    if (decoded.width() > WidestEmbeddedPicture) {
        decoded = decoded.scaledToWidth(WidestEmbeddedPicture, Qt::SmoothTransformation);
    }
    *pixels = decoded.size();

    QBuffer buffer;
    buffer.open(QIODevice::WriteOnly);
    const bool lossless = decoded.hasAlphaChannel() || decoded.width() * decoded.height() < 40000;
    if (!decoded.save(&buffer, lossless ? "PNG" : "JPEG", lossless ? -1 : 85)) {
        return {};
    }
    return (lossless ? u"data:image/png;base64,"_s : u"data:image/jpeg;base64,"_s)
        + QString::fromLatin1(buffer.data().toBase64());
}

/**
 * Walks a page for everything on it that is not text.
 *
 * Pictures, so that an HTML file can put them back where they were, and
 * axis-aligned rules, because a table that is ruled says so and that is worth
 * knowing. Only what the page draws itself: a picture placed inside a form
 * XObject is not followed, which is a real gap and is admitted in limitations().
 */
class FurnitureFilter : public QPDFObjectHandle::TokenFilter
{
public:
    FurnitureFilter(QPDFObjectHandle resources, const QTransform &toDisplay, bool wantPictures)
        : m_resources(std::move(resources))
        , m_toDisplay(toDisplay)
        , m_wantPictures(wantPictures)
    {
    }

    void handleToken(QPDFTokenizer::Token const &token) override
    {
        switch (token.getType()) {
        case QPDFTokenizer::tt_space:
        case QPDFTokenizer::tt_comment:
        case QPDFTokenizer::tt_eof:
            return;
        case QPDFTokenizer::tt_word:
            handleOperator(QString::fromStdString(token.getValue()));
            m_operands.clear();
            return;
        case QPDFTokenizer::tt_inline_image:
            m_operands.clear();
            return;
        default:
            m_operands.append(token);
            return;
        }
    }

    QVector<QPDFObjectHandle> images() const { return m_images; }
    QVector<QRectF> placements() const { return m_placements; }
    QVector<QLineF> rules() const { return m_rules; }

private:
    double number(int index) const
    {
        if (index < 0 || index >= m_operands.size()) {
            return 0.0;
        }
        // QString::toDouble is C-locale whatever LC_NUMERIC says, which is the
        // whole reason it is used here rather than strtod.
        return QString::fromStdString(m_operands.at(index).getValue()).toDouble();
    }

    void addRule(const QPointF &from, const QPointF &to)
    {
        const QPointF a = m_toDisplay.map(m_ctm.map(from));
        const QPointF b = m_toDisplay.map(m_ctm.map(to));
        const double dx = std::abs(a.x() - b.x());
        const double dy = std::abs(a.y() - b.y());
        if (std::min(dx, dy) > 0.8 || std::max(dx, dy) < 6.0) {
            return;
        }
        m_rules.append(QLineF(a, b));
    }

    void addRectangle(const QRectF &rect)
    {
        const QRectF mapped = m_toDisplay.mapRect(m_ctm.mapRect(rect));
        if (std::min(mapped.width(), mapped.height()) <= 2.0 && std::max(mapped.width(), mapped.height()) >= 6.0) {
            const double y = mapped.center().y();
            const double x = mapped.center().x();
            if (mapped.width() >= mapped.height()) {
                m_rules.append(QLineF(mapped.left(), y, mapped.right(), y));
            } else {
                m_rules.append(QLineF(x, mapped.top(), x, mapped.bottom()));
            }
        }
        m_pendingRects.append(rect);
    }

    void handleOperator(const QString &op)
    {
        if (op == u"q"_s) {
            m_stack.append(m_ctm);
        } else if (op == u"Q"_s) {
            if (!m_stack.isEmpty()) {
                m_ctm = m_stack.takeLast();
            }
        } else if (op == u"cm"_s && m_operands.size() >= 6) {
            m_ctm = QTransform(number(0), number(1), number(2), number(3), number(4), number(5)) * m_ctm;
        } else if (op == u"re"_s && m_operands.size() >= 4) {
            addRectangle(QRectF(number(0), number(1), number(2), number(3)).normalized());
        } else if (op == u"m"_s && m_operands.size() >= 2) {
            m_current = QPointF(number(0), number(1));
            m_subpathStart = m_current;
            m_segments.clear();
        } else if (op == u"l"_s && m_operands.size() >= 2) {
            const QPointF next(number(0), number(1));
            m_segments.append(QLineF(m_current, next));
            m_current = next;
        } else if (op == u"h"_s) {
            m_segments.append(QLineF(m_current, m_subpathStart));
            m_current = m_subpathStart;
        } else if (op == u"S"_s || op == u"s"_s) {
            for (const QLineF &segment : std::as_const(m_segments)) {
                addRule(segment.p1(), segment.p2());
            }
            for (const QRectF &rect : std::as_const(m_pendingRects)) {
                addRule(rect.topLeft(), rect.topRight());
                addRule(rect.bottomLeft(), rect.bottomRight());
                addRule(rect.topLeft(), rect.bottomLeft());
                addRule(rect.topRight(), rect.bottomRight());
            }
            m_segments.clear();
            m_pendingRects.clear();
        } else if (op == u"f"_s || op == u"F"_s || op == u"f*"_s || op == u"B"_s || op == u"B*"_s || op == u"b"_s
                   || op == u"b*"_s || op == u"n"_s) {
            m_segments.clear();
            m_pendingRects.clear();
        } else if (op == u"Do"_s && m_wantPictures && !m_operands.isEmpty()) {
            lookUpXObject(QString::fromStdString(m_operands.at(0).getValue()));
        }
    }

    void lookUpXObject(const QString &name)
    {
        QPDFObjectHandle xobjects
            = m_resources.isDictionary() ? m_resources.getKey("/XObject") : QPDFObjectHandle::newNull();
        if (!xobjects.isDictionary()) {
            return;
        }
        QPDFObjectHandle object = xobjects.getKey(name.toStdString());
        if (!object.isStream()) {
            return;
        }
        QPDFObjectHandle subtype = object.getDict().getKey("/Subtype");
        if (!subtype.isName() || subtype.getName() != "/Image") {
            return;
        }
        m_images.append(object);
        m_placements.append(m_toDisplay.mapRect(m_ctm.mapRect(QRectF(0.0, 0.0, 1.0, 1.0))));
    }

    QPDFObjectHandle m_resources;
    QTransform m_toDisplay;
    bool m_wantPictures = false;

    QVector<QPDFTokenizer::Token> m_operands;
    QTransform m_ctm;
    QVector<QTransform> m_stack;
    QPointF m_current;
    QPointF m_subpathStart;
    QVector<QLineF> m_segments;
    QVector<QRectF> m_pendingRects;

    QVector<QPDFObjectHandle> m_images;
    QVector<QRectF> m_placements;
    QVector<QLineF> m_rules;
};

/** Display points to page space, /Rotate and a shifted media box included. */
QTransform pageTransformFor(QPDFPageObjectHelper &page)
{
    const QRectF media = PdfGeometry::mediaBoxOf(page);
    return PdfGeometry::displayToPageTransform(PdfGeometry::rotationOf(page), media.width(), media.height())
        * QTransform::fromTranslate(media.x(), media.y());
}

// ── Reading a whole document ─────────────────────────────────────────────────

struct Request {
    QVector<int> pages;
    double lineTolerance = 2.0;
    double paragraphFactor = 1.6;
    bool wantPictures = false;
    bool wantRules = false;
};

bool analyse(const QString &pdf, const Request &request, Analysis *analysis, QString *error)
{
    try {
        QPDF document;
        PdfFile::open(document, pdf);
        QPDFPageDocumentHelper helper(document);
        std::vector<QPDFPageObjectHelper> all = helper.getAllPages();
        const int count = int(all.size());

        QVector<int> wanted = request.pages;
        if (wanted.isEmpty()) {
            wanted.reserve(count);
            for (int i = 0; i < count; ++i) {
                wanted.append(i);
            }
        }

        QHash<double, qsizetype> sizeWeights;

        for (const int index : std::as_const(wanted)) {
            if (index < 0 || index >= count) {
                if (error) {
                    *error = i18n("The document has no page %1.", index + 1);
                }
                return false;
            }
            QPDFPageObjectHelper page = all.at(size_t(index));

            PageAnalysis result;
            result.page = index;
            const QRectF media = PdfGeometry::mediaBoxOf(page);
            const int rotation = PdfGeometry::rotationOf(page);
            result.size = (rotation == 90 || rotation == 270) ? QSizeF(media.height(), media.width())
                                                              : QSizeF(media.width(), media.height());

            const QHash<QString, QString> baseFonts = baseFontsOf(page);

            QString runError;
            const QVector<TextEdit::Run> runs = TextEdit::runsOn(pdf, index, &runError);

            QVector<Piece> pieces;
            pieces.reserve(runs.size());
            for (const TextEdit::Run &run : runs) {
                if (run.text.trimmed().isEmpty() || run.rect.width() <= 0.0) {
                    continue;
                }
                Piece piece;
                piece.text = run.text;
                piece.rect = run.rect;
                piece.size = sizeOf(run);
                emphasisOf(baseFonts.value(run.fontResource), &piece.bold, &piece.italic);
                pieces.append(piece);
                sizeWeights[std::round(piece.size * 2.0) / 2.0] += piece.text.trimmed().size();
            }

            Region everything;
            everything.reserve(pieces.size());
            for (qsizetype i = 0; i < pieces.size(); ++i) {
                everything.append(i);
            }

            QVector<Region> regions;
            cutRegion(pieces, everything, 0, regions);
            for (const Region &region : std::as_const(regions)) {
                result.paragraphs
                    += buildParagraphs(buildLines(pieces, region, request.lineTolerance), request.paragraphFactor);
            }
            result.lines = buildLines(pieces, everything, request.lineTolerance);

            if (request.wantPictures || request.wantRules) {
                bool invertible = false;
                const QTransform toDisplay = pageTransformFor(page).inverted(&invertible);
                FurnitureFilter filter(page.getAttribute("/Resources", false), invertible ? toDisplay : QTransform(),
                                       request.wantPictures);
                page.filterContents(&filter, nullptr);
                result.rules = filter.rules();

                const QVector<QPDFObjectHandle> images = filter.images();
                const QVector<QRectF> placements = filter.placements();
                for (qsizetype i = 0; request.wantPictures && i < images.size(); ++i) {
                    if (std::min(placements.at(i).width(), placements.at(i).height()) < SmallestInterestingPicture) {
                        continue;
                    }
                    Picture picture;
                    picture.rect = placements.at(i);
                    picture.dataUri = pictureDataUri(images.at(i), &picture.pixels);
                    if (!picture.dataUri.isEmpty()) {
                        result.pictures.append(picture);
                    }
                }
                std::sort(result.pictures.begin(), result.pictures.end(),
                          [](const Picture &a, const Picture &b) { return topOf(a.rect) > topOf(b.rect); });
            }

            analysis->pages.append(result);
        }

        // The body size is whatever most of the characters are set in, not the
        // average: an average lands between the body and the headings and calls
        // neither of them anything.
        qsizetype best = 0;
        for (auto it = sizeWeights.constBegin(); it != sizeWeights.constEnd(); ++it) {
            if (it.value() > best) {
                best = it.value();
                analysis->bodySize = it.key();
            }
        }

        QVector<double> headings;
        for (const PageAnalysis &page : std::as_const(analysis->pages)) {
            for (const Paragraph &paragraph : page.paragraphs) {
                const double rounded = std::round(paragraph.size * 2.0) / 2.0;
                if (rounded > analysis->bodySize * HeadingFactor && !headings.contains(rounded)) {
                    headings.append(rounded);
                }
            }
        }
        std::sort(headings.begin(), headings.end(), std::greater<double>());
        analysis->headingSizes = headings;
    } catch (const std::exception &e) {
        if (error) {
            *error = QString::fromUtf8(e.what());
        }
        return false;
    }
    return true;
}

/**
 * The heading level of a paragraph, or zero when it is body text.
 *
 * A heading is a short paragraph set noticeably bigger than the rest of the
 * document. Both halves matter: without the size test every paragraph is a
 * heading, and without the length test a page set entirely in large type becomes
 * a stack of them.
 *
 * The only place this judgement is made. Convert::toMarkdown(), Convert::toHtml()
 * and Convert::findHeadings() all come through here, so the shapes of document
 * it gets wrong (written out in full on Convert::findHeadings()) are the same
 * for all three, and stay that way.
 */
int headingLevelOf(const Paragraph &paragraph, const Analysis &analysis)
{
    if (paragraph.lines.size() > 3) {
        return 0;
    }
    const double rounded = std::round(paragraph.size * 2.0) / 2.0;
    const qsizetype rank = analysis.headingSizes.indexOf(rounded);
    if (rank < 0) {
        return 0;
    }
    return int(std::min(rank + 1, qsizetype(MaxHeadingLevel)));
}

// ── Escaping ─────────────────────────────────────────────────────────────────

QString escapeMarkdown(const QString &text)
{
    QString escaped;
    escaped.reserve(text.size());
    for (const QChar character : text) {
        if (QStringLiteral("\\`*_[]<>").contains(character)) {
            escaped.append(u'\\');
        }
        escaped.append(character);
    }
    // Only at the start of a line do these mean anything, and escaping them
    // everywhere turns ordinary prose into a thicket of backslashes.
    static const QRegularExpression leading(u"^([#>|+=-]|\\d+[.)])"_s);
    const QRegularExpressionMatch match = leading.match(escaped);
    if (match.hasMatch()) {
        escaped.insert(match.capturedLength(0) - 1, u'\\');
    }
    return escaped;
}

QString escapeHtml(const QString &text)
{
    QString escaped = text;
    escaped.replace(u'&', u"&amp;"_s);
    escaped.replace(u'<', u"&lt;"_s);
    escaped.replace(u'>', u"&gt;"_s);
    return escaped;
}

QString markdownOf(const QVector<Segment> &segments, bool emphasise)
{
    QString out;
    for (const Segment &segment : segments) {
        const QString escaped = escapeMarkdown(segment.text);
        if (!emphasise || (!segment.bold && !segment.italic) || segment.text.trimmed().isEmpty()) {
            out += escaped;
            continue;
        }
        // The markers have to sit against the letters: "** bold **" is not
        // emphasis in any Markdown dialect, it is four asterisks.
        static const QRegularExpression parts(u"^(\\s*)(.*?)(\\s*)$"_s);
        const QRegularExpressionMatch match = parts.match(escaped);
        const QString marker = segment.bold ? (segment.italic ? u"***"_s : u"**"_s) : u"*"_s;
        out += match.captured(1) + marker + match.captured(2) + marker + match.captured(3);
    }
    return out;
}

QString htmlOf(const QVector<Segment> &segments)
{
    QString out;
    for (const Segment &segment : segments) {
        QString text = escapeHtml(segment.text);
        if (!segment.text.trimmed().isEmpty()) {
            if (segment.italic) {
                text = u"<em>"_s + text + u"</em>"_s;
            }
            if (segment.bold) {
                text = u"<strong>"_s + text + u"</strong>"_s;
            }
        }
        out += text;
    }
    return out;
}

bool writeTextFile(const QString &path, const QString &content, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error) {
            *error = i18n("Could not write “%1”.", path);
        }
        return false;
    }
    const QByteArray bytes = content.toUtf8();
    if (file.write(bytes) != bytes.size()) {
        if (error) {
            *error = i18n("Could not write all of “%1”.", path);
        }
        return false;
    }
    file.close();
    return true;
}

// ── Tables ───────────────────────────────────────────────────────────────────

struct Cell {
    QString text;
    double left = 0.0;
    double right = 0.0;
};

/**
 * A line divided into cells.
 *
 * Two things divide them: a gap between two runs much wider than a word space,
 * and a run of three or more spaces inside one run's own text. The second is
 * needed because a good many producers draw a whole table row with a single
 * text-showing operator and pad the columns out with spaces, and without it
 * those tables are invisible. The positions of cells found that way are
 * estimated from the character count, which is exact for a monospaced font and
 * approximate for everything else.
 */
QVector<Cell> cellsOf(const Line &line, double fontSize)
{
    const double threshold = std::max(CellGapMinimumPoints, CellGapFactor * fontSize);

    QVector<Cell> cells;
    double cursor = 0.0;
    for (const Piece &piece : line.pieces) {
        if (cells.isEmpty() || piece.rect.left() - cursor > threshold) {
            cells.append(Cell { piece.text, piece.rect.left(), piece.rect.right() });
        } else {
            if (needsSpace(cells.last().text, piece, cursor)) {
                cells.last().text.append(u' ');
            }
            cells.last().text += piece.text;
            cells.last().right = piece.rect.right();
        }
        cursor = piece.rect.right();
    }

    static const QRegularExpression padding(u"\\s{3,}"_s);
    QVector<Cell> divided;
    for (const Cell &cell : std::as_const(cells)) {
        const QStringList parts = cell.text.split(padding, Qt::SkipEmptyParts);
        if (parts.size() < 2 || cell.text.trimmed().isEmpty()) {
            Cell trimmed = cell;
            trimmed.text = trimmed.text.trimmed();
            if (!trimmed.text.isEmpty()) {
                divided.append(trimmed);
            }
            continue;
        }
        const double perCharacter = cell.text.size() > 0 ? (cell.right - cell.left) / cell.text.size() : 0.0;
        qsizetype at = 0;
        for (const QString &part : parts) {
            const qsizetype found = cell.text.indexOf(part, at);
            const qsizetype start = found >= 0 ? found : at;
            divided.append(Cell { part.trimmed(), cell.left + perCharacter * start,
                                  cell.left + perCharacter * (start + part.size()) });
            at = start + part.size();
        }
    }
    return divided;
}

bool cellsAlign(const QVector<Cell> &a, const QVector<Cell> &b, double tolerance)
{
    if (a.size() != b.size()) {
        return false;
    }
    for (qsizetype i = 0; i < a.size(); ++i) {
        if (std::abs(a.at(i).left - b.at(i).left) > tolerance) {
            return false;
        }
    }
    return true;
}

double median(QVector<double> values)
{
    if (values.isEmpty()) {
        return 0.0;
    }
    std::nth_element(values.begin(), values.begin() + values.size() / 2, values.end());
    return values.at(values.size() / 2);
}

/** How much of its column a cell fills, which is what tells a table from prose. */
double fillRatioOf(const QVector<QVector<Cell>> &rows)
{
    QVector<double> ratios;
    const qsizetype columns = rows.first().size();
    for (qsizetype column = 0; column + 1 < columns; ++column) {
        QVector<double> widths;
        double pitch = 0.0;
        for (const QVector<Cell> &row : rows) {
            widths.append(row.at(column).right - row.at(column).left);
            pitch = std::max(pitch, row.at(column + 1).left - row.at(column).left);
        }
        if (pitch > 0.1) {
            ratios.append(median(widths) / pitch);
        }
    }
    return median(ratios);
}

QVector<Convert::Table> tablesOnPage(const PageAnalysis &page)
{
    QVector<Convert::Table> tables;

    qsizetype at = 0;
    while (at < page.lines.size()) {
        const QVector<Cell> first = cellsOf(page.lines.at(at), page.lines.at(at).size);
        if (first.size() < 2) {
            ++at;
            continue;
        }

        QVector<QVector<Cell>> rows { first };
        QRectF box = page.lines.at(at).box;
        qsizetype next = at + 1;
        while (next < page.lines.size()) {
            const Line &previous = page.lines.at(next - 1);
            const Line &line = page.lines.at(next);
            const double reference = std::max(previous.size, line.size);
            if (previous.baseline - line.baseline > 2.5 * reference) {
                break;
            }
            const QVector<Cell> cells = cellsOf(line, line.size);
            if (!cellsAlign(cells, first, std::max(3.0, 0.5 * reference))) {
                break;
            }
            rows.append(cells);
            box = box.united(line.box);
            ++next;
        }

        if (rows.size() < MinimumTableRows || fillRatioOf(rows) > CellFillCeiling) {
            ++at;
            continue;
        }

        Convert::Table table;
        table.page = page.page;
        for (const QVector<Cell> &row : std::as_const(rows)) {
            QStringList texts;
            for (const Cell &cell : row) {
                texts << cell.text;
            }
            table.rows.append(texts);
        }

        // How sure this is. Rows are the main evidence, columns that line up to
        // within a point are better evidence, and ruling is the only thing here
        // that the document itself put on the page rather than being inferred.
        double confidence = 0.4 + std::min(0.2, 0.04 * double(rows.size() - MinimumTableRows));
        double worst = 0.0;
        for (const QVector<Cell> &row : std::as_const(rows)) {
            for (qsizetype i = 0; i < row.size(); ++i) {
                worst = std::max(worst, std::abs(row.at(i).left - first.at(i).left));
            }
        }
        if (worst < 1.0) {
            confidence += 0.15;
        }
        int ruling = 0;
        const QRectF around = box.adjusted(-4.0, -4.0, 4.0, 4.0);
        for (const QLineF &rule : page.rules) {
            if (around.contains(rule.p1()) || around.contains(rule.p2())) {
                ++ruling;
            }
        }
        if (ruling >= 2) {
            confidence += 0.25;
        } else if (ruling == 1) {
            confidence += 0.1;
        }
        table.confidence = std::clamp(confidence, 0.05, 0.99);
        tables.append(table);

        at = next;
    }
    return tables;
}

QString csvField(const QString &text)
{
    if (!text.contains(u',') && !text.contains(u'"') && !text.contains(u'\n') && !text.contains(u'\r')) {
        return text;
    }
    QString quoted = text;
    quoted.replace(u"\""_s, u"\"\""_s);
    return QLatin1Char('"') + quoted + QLatin1Char('"');
}

// ── Ghostscript, at arm's length ─────────────────────────────────────────────

QString ghostscript()
{
    return QStandardPaths::findExecutable(u"gs"_s);
}

/** Directories that hold ICC profiles on a Linux desktop. */
QStringList iccSearchDirectories()
{
    QStringList directories {
        u"/usr/share/color/icc"_s,
        u"/usr/share/color/icc/colord"_s,
        u"/usr/local/share/color/icc"_s,
        u"/usr/share/colord/icc"_s,
    };
    const QDir gs(u"/usr/share/ghostscript"_s);
    const QStringList versions = gs.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString &version : versions) {
        directories.append(gs.filePath(version) + QLatin1String("/iccprofiles"));
    }
    return directories;
}

/**
 * A CMYK printing profile from the system, or empty.
 *
 * Not sRGB, which is what PDF/A wants: the output intent of a file going to
 * press describes a printing condition, and the commonest one in Europe by a
 * wide margin is ISO Coated v2. Anything else that names itself as coated or
 * uncoated stock will do; what will not do is guessing, which is why an empty
 * result is reported rather than papered over.
 */
QString findCmykProfile()
{
    static const QStringList wanted {
        u"isocoated_v2_bas.icc"_s, u"isocoated_v2_300_bas.icc"_s, u"isocoated_v2_eci.icc"_s,     u"psocoated_v3.icc"_s,
        u"default_cmyk.icc"_s,     u"uswebcoatedswop.icc"_s,      u"coated_fogra39l_argl.icc"_s,
    };
    for (const QString &path : iccSearchDirectories()) {
        const QDir directory(path);
        if (!directory.exists()) {
            continue;
        }
        const QFileInfoList files = directory.entryInfoList(QDir::Files, QDir::Name);
        for (const QString &name : wanted) {
            for (const QFileInfo &file : files) {
                if (file.fileName().toLower() == name) {
                    return file.absoluteFilePath();
                }
            }
        }
        for (const QFileInfo &file : files) {
            const QString name = file.fileName().toLower();
            if ((name.endsWith(u".icc"_s) || name.endsWith(u".icm"_s))
                && (name.contains(u"coated"_s) || name.contains(u"cmyk"_s))) {
                return file.absoluteFilePath();
            }
        }
    }
    return {};
}

/** How many components a profile describes, read out of its own header. */
int profileComponents(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return 0;
    }
    const QByteArray header = file.read(20);
    if (header.size() < 20) {
        return 0;
    }
    const QByteArray space = header.mid(16, 4);
    if (space == QByteArray("CMYK")) {
        return 4;
    }
    if (space == QByteArray("GRAY")) {
        return 1;
    }
    return space == QByteArray("RGB ") ? 3 : 0;
}

/** A PostScript byte string, `(…)`, safe for any path. */
QString postScriptLiteral(const QString &text)
{
    QString escaped;
    const QByteArray utf8 = text.toUtf8();
    for (const char byte : utf8) {
        const unsigned char value = static_cast<unsigned char>(byte);
        if (value == '(' || value == ')' || value == '\\') {
            escaped += QLatin1Char('\\');
            escaped += QLatin1Char(byte);
        } else if (value < 32 || value > 126) {
            escaped += QLatin1Char('\\');
            escaped += QStringLiteral("%1").arg(static_cast<uint>(value), 3, 8, QLatin1Char('0'));
        } else {
            escaped += QLatin1Char(byte);
        }
    }
    return QLatin1Char('(') + escaped + QLatin1Char(')');
}

QString pdfxVersionLabel(Convert::XLevel level)
{
    // A standard's name is the same in every language, so it is deliberately
    // not translated.
    switch (level) {
    case Convert::XLevel::X1a:
        return QStringLiteral("PDF/X-1a:2003");
    case Convert::XLevel::X3:
        return QStringLiteral("PDF/X-3:2003");
    case Convert::XLevel::X4:
        return QStringLiteral("PDF/X-4");
    }
    return QStringLiteral("PDF/X-4");
}

/**
 * Writes the prologue Ghostscript needs to make a PDF/X file.
 *
 * The output intent and the version claim in the document information
 * dictionary: everything the standard wants that is not a command-line switch.
 */
bool writePdfxPrologue(const QString &path, const QString &profile, int components, Convert::XLevel level,
                       QString *error)
{
    QString program = QStringLiteral("%!\n");
    program += QLatin1String("[ /GTS_PDFXVersion ") + postScriptLiteral(pdfxVersionLabel(level))
        + QLatin1String("\n  /Trapped /False\n  /DOCINFO pdfmark\n");

    if (!profile.isEmpty()) {
        program += QLatin1String("/ICCProfile ") + postScriptLiteral(profile) + QLatin1String(" def\n");
        program += QLatin1String("[/_objdef {icc_PDFX} /type /stream /OBJ pdfmark\n");
        // Stated rather than guessed: the sample prologue Ghostscript ships asks
        // the device how many components it has, and gets it wrong under a
        // device-independent colour strategy.
        program
            += QLatin1String("[{icc_PDFX} << /N ") + QString::number(components) + QLatin1String(" >> /PUT pdfmark\n");
        program += QLatin1String("[{icc_PDFX} ICCProfile (r) file /PUT pdfmark\n");
        program += QLatin1String("[/_objdef {OutputIntent_PDFX} /type /dict /OBJ pdfmark\n");
        program += QLatin1String("[{OutputIntent_PDFX} <<\n");
        program += QLatin1String("  /Type /OutputIntent\n");
        program += QLatin1String("  /S /GTS_PDFX\n");
        program += QLatin1String("  /OutputCondition (Commercial and specialty printing)\n");
        program += QLatin1String("  /OutputConditionIdentifier ") + postScriptLiteral(QFileInfo(profile).baseName())
            + QLatin1String("\n");
        program += QLatin1String("  /RegistryName (http://www.color.org)\n");
        program += QLatin1String("  /DestOutputProfile {icc_PDFX}\n");
        program += QLatin1String(">> /PUT pdfmark\n");
        program += QLatin1String("[{Catalog} <</OutputIntents [ {OutputIntent_PDFX} ]>> /PUT pdfmark\n");
    }

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error) {
            *error = i18n("Could not write “%1”.", path);
        }
        return false;
    }
    file.write(program.toLatin1());
    file.close();
    return true;
}

/**
 * The XMP packet identifying a file as PDF/X.
 *
 * Written by hand rather than by Ghostscript, which puts the version in the
 * document information dictionary and stops there. Every version of the standard
 * from 2003 onwards wants it in the metadata too, and a preflight check that
 * looks only at the metadata (most of them do) will otherwise not see it.
 */
QString pdfxPacket(const QString &label, const QString &conformance)
{
    // The byte-order mark has to be the character U+FEFF and not three escaped
    // bytes: written as a narrow literal it would come out of QStringLiteral as
    // three Latin-1 characters and be encoded again on the way to UTF-8.
    QString packet = QStringLiteral("<?xpacket begin=\"") + QChar(0xFEFF)
        + QStringLiteral("\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>\n");
    packet += QLatin1String("<x:xmpmeta xmlns:x=\"adobe:ns:meta/\">\n");
    packet += QLatin1String(" <rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">\n");
    packet += QLatin1String("  <rdf:Description rdf:about=\"\" xmlns:pdfxid=\"http://www.npes.org/pdfx/ns/id/\">\n");
    packet += QLatin1String("   <pdfxid:GTS_PDFXVersion>") + label + QLatin1String("</pdfxid:GTS_PDFXVersion>\n");
    packet += QLatin1String("  </rdf:Description>\n");
    packet += QLatin1String("  <rdf:Description rdf:about=\"\" xmlns:pdfx=\"http://ns.adobe.com/pdfx/1.3/\">\n");
    packet += QLatin1String("   <pdfx:GTS_PDFXVersion>") + label + QLatin1String("</pdfx:GTS_PDFXVersion>\n");
    packet += QLatin1String("   <pdfx:GTS_PDFXConformance>") + conformance
        + QLatin1String("</pdfx:GTS_PDFXConformance>\n");
    packet += QLatin1String("  </rdf:Description>\n");
    packet += QLatin1String("  <rdf:Description rdf:about=\"\" xmlns:dc=\"http://purl.org/dc/elements/1.1/\">\n");
    packet += QLatin1String("   <dc:format>application/pdf</dc:format>\n");
    packet += QLatin1String("  </rdf:Description>\n");
    packet += QLatin1String(" </rdf:RDF>\n</x:xmpmeta>\n");
    // The padding every XMP packet carries so that a tool can edit it in place
    // without rewriting the file around it.
    for (int i = 0; i < 20; ++i) {
        packet += QLatin1String("                                                                        \n");
    }
    packet += QLatin1String("<?xpacket end=\"w\"?>\n");
    return packet;
}

/** Replaces @p path with itself plus an XMP packet, at the given minimum version. */
bool stampMetadata(const QString &path, const QString &packet, const QString &minimumVersion, QString *error)
{
    const QString temporary = path + QLatin1String(".pdf-smithy-xmp");
    try {
        QPDF pdf;
        PdfFile::open(pdf, path);

        QPDFObjectHandle metadata = QPDFObjectHandle::newStream(&pdf, packet.toUtf8().toStdString());
        metadata.getDict().replaceKey("/Type", QPDFObjectHandle::newName("/Metadata"));
        metadata.getDict().replaceKey("/Subtype", QPDFObjectHandle::newName("/XML"));
        pdf.getRoot().replaceKey("/Metadata", pdf.makeIndirectObject(metadata));

        QPDFWriter writer(pdf, QFile::encodeName(temporary).constData());
        writer.setMinimumPDFVersion(minimumVersion.toStdString());
        // The packet has to stay readable by a tool that does not decompress
        // streams, which is what every version of the standard asks for, and it
        // is why nothing here is allowed to deflate it.
        writer.setStreamDataMode(qpdf_s_preserve);
        writer.setObjectStreamMode(qpdf_o_disable);
        writer.write();
    } catch (const std::exception &e) {
        QFile::remove(temporary);
        if (error) {
            *error = QString::fromUtf8(e.what());
        }
        return false;
    }

    QFile::setPermissions(temporary, QFile::ReadOwner | QFile::WriteOwner | QFile::ReadGroup | QFile::ReadOther);
    if (::rename(QFile::encodeName(temporary).constData(), QFile::encodeName(path).constData()) != 0) {
        QFile::remove(temporary);
        if (error) {
            *error = i18n("Could not replace “%1”.", path);
        }
        return false;
    }
    return true;
}

// ── What a file's version has to be ──────────────────────────────────────────

struct Requirement {
    QString version;
    QString reason;
};

QPair<int, int> parseVersion(const QString &version)
{
    static const QRegularExpression shape(u"^(\\d+)\\.(\\d+)$"_s);
    const QRegularExpressionMatch match = shape.match(version.trimmed());
    if (!match.hasMatch()) {
        return { -1, -1 };
    }
    return { match.captured(1).toInt(), match.captured(2).toInt() };
}

bool versionAtLeast(const QString &have, const QString &wanted)
{
    const QPair<int, int> a = parseVersion(have);
    const QPair<int, int> b = parseVersion(wanted);
    return a.first > b.first || (a.first == b.first && a.second >= b.second);
}

bool hasTransparency(QPDFObjectHandle resources, int depth = 0)
{
    if (depth > 4 || !resources.isDictionary()) {
        return false;
    }
    QPDFObjectHandle states = resources.getKey("/ExtGState");
    if (states.isDictionary()) {
        for (const auto &[key, state] : states.getDictAsMap()) {
            Q_UNUSED(key)
            if (!state.isDictionary()) {
                continue;
            }
            QPDFObjectHandle mask = state.getKey("/SMask");
            if (mask.isDictionary() || (mask.isName() && mask.getName() != "/None")) {
                return true;
            }
            for (const char *key2 : { "/ca", "/CA" }) {
                QPDFObjectHandle alpha = state.getKey(key2);
                if (alpha.isNumber() && PdfGeometry::numericValue(alpha, 1.0) < 0.999) {
                    return true;
                }
            }
            QPDFObjectHandle blend = state.getKey("/BM");
            if (blend.isName() && blend.getName() != "/Normal" && blend.getName() != "/Compatible") {
                return true;
            }
        }
    }
    QPDFObjectHandle xobjects = resources.getKey("/XObject");
    if (!xobjects.isDictionary()) {
        return false;
    }
    for (const auto &[key, object] : xobjects.getDictAsMap()) {
        Q_UNUSED(key)
        if (!object.isStream()) {
            continue;
        }
        QPDFObjectHandle group = object.getDict().getKey("/Group");
        if (group.isDictionary()) {
            QPDFObjectHandle subtype = group.getKey("/S");
            if (subtype.isName() && subtype.getName() == "/Transparency") {
                return true;
            }
        }
        if (hasTransparency(object.getDict().getKey("/Resources"), depth + 1)) {
            return true;
        }
    }
    return false;
}

/**
 * Which features of a document oblige it to claim a version, and which version.
 *
 * The list is short on purpose: these are the structures that a reader written
 * to an older version will meet and not understand, rather than every difference
 * between one version of the specification and the next.
 */
QVector<Requirement> versionRequirementsOf(QPDF &pdf)
{
    QVector<Requirement> found;

    const std::map<QPDFObjGen, QPDFXRefEntry> table = pdf.getXRefTable();
    for (const auto &[objgen, entry] : table) {
        Q_UNUSED(objgen)
        if (entry.getType() == 2) {
            found.append(Requirement {
                QStringLiteral("1.5"),
                i18n("The document keeps its objects in compressed object streams, which no reader older than "
                     "PDF %1 can unpack.",
                     QStringLiteral("1.5")) });
            break;
        }
    }

    int r = 0;
    int p = 0;
    int v = 0;
    QPDF::encryption_method_e streams = QPDF::e_none;
    QPDF::encryption_method_e strings = QPDF::e_none;
    QPDF::encryption_method_e files = QPDF::e_none;
    if (pdf.isEncrypted(r, p, v, streams, strings, files)) {
        if (v >= 5) {
            found.append(Requirement {
                QStringLiteral("1.7"),
                i18n("The document is encrypted with AES-256, which needs PDF %1 or newer.", QStringLiteral("1.7")) });
        } else if (v == 4) {
            found.append(Requirement {
                QStringLiteral("1.6"),
                i18n("The document is encrypted with AES-128, which needs PDF %1 or newer.", QStringLiteral("1.6")) });
        } else if (v == 2) {
            found.append(Requirement {
                QStringLiteral("1.4"),
                i18n("The document uses encryption with a longer key than PDF %1 allows.", QStringLiteral("1.3")) });
        }
    }

    if (pdf.getRoot().hasKey("/OCProperties")) {
        found.append(
            Requirement { QStringLiteral("1.5"),
                          i18n("The document has layers, which need PDF %1 or newer.", QStringLiteral("1.5")) });
    }

    bool transparency = false;
    bool jbig2 = false;
    bool jpeg2000 = false;
    for (QPDFPageObjectHelper &page : QPDFPageDocumentHelper(pdf).getAllPages()) {
        QPDFObjectHandle group = page.getObjectHandle().getKey("/Group");
        if (group.isDictionary() && group.getKey("/S").isName() && group.getKey("/S").getName() == "/Transparency") {
            transparency = true;
        }
        QPDFObjectHandle resources = page.getAttribute("/Resources", false);
        if (!transparency && hasTransparency(resources)) {
            transparency = true;
        }
        QPDFObjectHandle xobjects
            = resources.isDictionary() ? resources.getKey("/XObject") : QPDFObjectHandle::newNull();
        if (xobjects.isDictionary()) {
            for (const auto &[key, object] : xobjects.getDictAsMap()) {
                Q_UNUSED(key)
                if (!object.isStream()) {
                    continue;
                }
                const QStringList filters = filtersOf(object.getDict());
                jbig2 = jbig2 || filters.contains(u"/JBIG2Decode"_s);
                jpeg2000 = jpeg2000 || filters.contains(u"/JPXDecode"_s);
            }
        }
    }
    if (transparency) {
        found.append(Requirement {
            QStringLiteral("1.4"),
            i18n("The document uses transparency, which needs PDF %1 or newer.", QStringLiteral("1.4")) });
    }
    if (jbig2) {
        found.append(Requirement { QStringLiteral("1.4"),
                                   i18n("A picture in the document is compressed with JBIG2, which needs PDF %1 "
                                        "or newer.",
                                        QStringLiteral("1.4")) });
    }
    if (jpeg2000) {
        found.append(Requirement { QStringLiteral("1.5"),
                                   i18n("A picture in the document is compressed with JPEG 2000, which needs "
                                        "PDF %1 or newer.",
                                        QStringLiteral("1.5")) });
    }
    return found;
}

QString paddedNumber(int value, int width)
{
    return QStringLiteral("%1").arg(value, width, 10, QLatin1Char('0'));
}

QString outputNameFor(const QString &tmpl, int pageNumber, int width)
{
    if (tmpl.contains(QLatin1String("%1"))) {
        return QString(tmpl).replace(QLatin1String("%1"), paddedNumber(pageNumber, width));
    }
    const QFileInfo info(tmpl);
    const QString suffix = info.completeSuffix();
    const QString stem = suffix.isEmpty() ? tmpl : tmpl.chopped(suffix.size() + 1);
    return stem + QLatin1Char('-') + paddedNumber(pageNumber, width)
        + (suffix.isEmpty() ? QString() : QLatin1Char('.') + suffix);
}

} // namespace

// ── Text ─────────────────────────────────────────────────────────────────────

bool Convert::toText(const QString &pdf, const QString &outFile, const TextOptions &options, QString *error)
{
    Request request;
    request.pages = options.pages;
    request.lineTolerance = options.lineTolerance;
    request.paragraphFactor = options.paragraphFactor;

    Analysis analysis;
    if (!analyse(pdf, request, &analysis, error)) {
        return false;
    }

    QString out;
    for (qsizetype index = 0; index < analysis.pages.size(); ++index) {
        const PageAnalysis &page = analysis.pages.at(index);
        if (index > 0) {
            // A form feed between pages, which is what every other text
            // extractor writes and what anything reading the result expects.
            out += QLatin1Char('\f');
        }
        if (!options.preserveLayout) {
            for (const Line &line : page.lines) {
                for (const Piece &piece : line.pieces) {
                    out += piece.text.trimmed() + QLatin1Char('\n');
                }
            }
            continue;
        }
        for (const Paragraph &paragraph : page.paragraphs) {
            for (const Line &line : paragraph.lines) {
                out += textOfLine(line) + QLatin1Char('\n');
            }
            out += QLatin1Char('\n');
        }
    }

    return writeTextFile(outFile, out, error);
}

// ── Markdown ─────────────────────────────────────────────────────────────────

bool Convert::toMarkdown(const QString &pdf, const QString &outFile, const TextOptions &options, QString *error)
{
    Request request;
    request.pages = options.pages;
    request.lineTolerance = options.lineTolerance;
    request.paragraphFactor = options.paragraphFactor;

    Analysis analysis;
    if (!analyse(pdf, request, &analysis, error)) {
        return false;
    }

    QString out;
    for (const PageAnalysis &page : std::as_const(analysis.pages)) {
        for (const Paragraph &paragraph : page.paragraphs) {
            const int level = headingLevelOf(paragraph, analysis);
            if (level > 0) {
                const QString text = markdownOf(reflow(paragraph.lines), false).trimmed();
                if (!text.isEmpty()) {
                    out += QString(level, QLatin1Char('#')) + QLatin1Char(' ') + text + QLatin1String("\n\n");
                }
                continue;
            }

            QVector<Item> items;
            if (paragraphIsAList(paragraph, &items)) {
                for (const Item &item : std::as_const(items)) {
                    const QString text = markdownOf(itemSegments(item), true).trimmed();
                    if (text.isEmpty()) {
                        continue;
                    }
                    out += (item.number > 0 ? QString::number(item.number) + QStringLiteral(". ")
                                            : QStringLiteral("- "))
                        + text + QLatin1Char('\n');
                }
                out += QLatin1Char('\n');
                continue;
            }

            const QString text = markdownOf(reflow(paragraph.lines), true).trimmed();
            if (!text.isEmpty()) {
                out += text + QLatin1String("\n\n");
            }
        }
    }

    return writeTextFile(outFile, out, error);
}

// ── Headings ─────────────────────────────────────────────────────────────────

QVector<Convert::Heading> Convert::findHeadings(const QString &pdf, const QVector<int> &pages, QString *error)
{
    Request request;
    request.pages = pages;

    Analysis analysis;
    if (!analyse(pdf, request, &analysis, error)) {
        return {};
    }

    QVector<Heading> headings;
    for (const PageAnalysis &page : std::as_const(analysis.pages)) {
        for (const Paragraph &paragraph : page.paragraphs) {
            const int level = headingLevelOf(paragraph, analysis);
            if (level <= 0) {
                continue;
            }

            Heading heading;
            heading.page = page.page;
            heading.level = level;
            // Plain text, not the Markdown escaping toMarkdown() puts on the
            // same paragraph: this ends up in a bookmark or a dialogue, where a
            // backslash before every full stop would be shown as written.
            heading.text = plainText(reflow(paragraph.lines)).simplified();
            if (!heading.text.isEmpty()) {
                headings.append(heading);
            }
        }
    }
    return headings;
}

// ── HTML ─────────────────────────────────────────────────────────────────────

bool Convert::toHtml(const QString &pdf, const QString &outFile, const TextOptions &options, QString *error)
{
    Request request;
    request.pages = options.pages;
    request.lineTolerance = options.lineTolerance;
    request.paragraphFactor = options.paragraphFactor;
    request.wantPictures = true;

    Analysis analysis;
    if (!analyse(pdf, request, &analysis, error)) {
        return false;
    }

    const QString title = escapeHtml(QFileInfo(pdf).completeBaseName());

    QString out = QStringLiteral("<!DOCTYPE html>\n<html>\n<head>\n<meta charset=\"utf-8\">\n");
    out += QLatin1String("<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n");
    out += QLatin1String("<title>") + title + QLatin1String("</title>\n");
    // Relative units and a measure rather than a page: the point of this export
    // is a document that reads on a telephone, and a fixed layout would not.
    out += QLatin1String("<style>\n");
    out += QLatin1String("body { margin: 0 auto; max-width: 42em; padding: 1.5em 1em;\n");
    out += QLatin1String("       font-family: Georgia, 'Times New Roman', serif; line-height: 1.5; }\n");
    out += QLatin1String("figure { margin: 1.5em 0; }\n");
    out += QLatin1String("img { max-width: 100%; height: auto; display: block; }\n");
    out += QLatin1String("h1, h2, h3, h4, h5, h6 { line-height: 1.2; }\n");
    out += QLatin1String("</style>\n</head>\n<body>\n");

    for (const PageAnalysis &page : std::as_const(analysis.pages)) {
        qsizetype nextPicture = 0;
        const auto emitPicture = [&out](const Picture &picture) {
            out += QLatin1String("<figure><img src=\"") + picture.dataUri + QLatin1String("\" alt=\"\"");
            if (!picture.pixels.isEmpty()) {
                out += QLatin1String(" width=\"") + QString::number(picture.pixels.width())
                    + QLatin1String("\" height=\"") + QString::number(picture.pixels.height()) + QLatin1Char('"');
            }
            out += QLatin1String("></figure>\n");
        };

        for (const Paragraph &paragraph : page.paragraphs) {
            // Pictures go in where they sat on the page, which is the nearest an
            // export can get to "in place" once the layout is gone.
            while (nextPicture < page.pictures.size()
                   && topOf(page.pictures.at(nextPicture).rect) > topOf(paragraph.box)) {
                emitPicture(page.pictures.at(nextPicture));
                ++nextPicture;
            }

            const int level = headingLevelOf(paragraph, analysis);
            if (level > 0) {
                const QString text = htmlOf(reflow(paragraph.lines)).trimmed();
                if (!text.isEmpty()) {
                    const QString tag = QLatin1String("h") + QString::number(level);
                    out += QLatin1Char('<') + tag + QLatin1Char('>') + text + QLatin1String("</") + tag
                        + QLatin1String(">\n");
                }
                continue;
            }

            QVector<Item> items;
            if (paragraphIsAList(paragraph, &items)) {
                const bool ordered = items.first().number > 0;
                out += ordered ? QLatin1String("<ol>\n") : QLatin1String("<ul>\n");
                for (const Item &item : std::as_const(items)) {
                    const QString text = htmlOf(itemSegments(item)).trimmed();
                    if (!text.isEmpty()) {
                        out += QLatin1String("<li>") + text + QLatin1String("</li>\n");
                    }
                }
                out += ordered ? QLatin1String("</ol>\n") : QLatin1String("</ul>\n");
                continue;
            }

            const QString text = htmlOf(reflow(paragraph.lines)).trimmed();
            if (!text.isEmpty()) {
                out += QLatin1String("<p>") + text + QLatin1String("</p>\n");
            }
        }
        while (nextPicture < page.pictures.size()) {
            emitPicture(page.pictures.at(nextPicture));
            ++nextPicture;
        }
    }

    out += QLatin1String("</body>\n</html>\n");
    return writeTextFile(outFile, out, error);
}

// ── SVG ──────────────────────────────────────────────────────────────────────

bool Convert::toSvg(const QString &pdf, const QString &outTemplate, const QVector<int> &pages, QString *error)
{
    if (!QFileInfo::exists(pdf)) {
        if (error) {
            *error = i18n("“%1” does not exist.", pdf);
        }
        return false;
    }

    int count = 0;
    try {
        QPDF document;
        PdfFile::open(document, pdf);
        count = int(QPDFPageDocumentHelper(document).getAllPages().size());
    } catch (const std::exception &e) {
        if (error) {
            *error = QString::fromUtf8(e.what());
        }
        return false;
    }

    QVector<int> wanted = pages;
    if (wanted.isEmpty()) {
        wanted.reserve(count);
        for (int i = 0; i < count; ++i) {
            wanted.append(i);
        }
    }
    const int width = std::max(3, int(QString::number(count).size()));

    const QString cairo = QStandardPaths::findExecutable(u"pdftocairo"_s);
    const QString gs = ghostscript();
    if (cairo.isEmpty() && gs.isEmpty()) {
        if (error) {
            *error = i18n("Neither pdftocairo nor Ghostscript is installed, so pages cannot be saved as SVG. "
                          "Install the “poppler-utils” package.");
        }
        return false;
    }

    for (const int page : std::as_const(wanted)) {
        if (page < 0 || page >= count) {
            if (error) {
                *error = i18n("The document has no page %1.", page + 1);
            }
            return false;
        }
        const QString target = outputNameFor(outTemplate, page + 1, width);

        QProcess process;
        if (!cairo.isEmpty()) {
            process.start(cairo,
                          { QStringLiteral("-svg"), QStringLiteral("-f"), QString::number(page + 1),
                            QStringLiteral("-l"), QString::number(page + 1), pdf, target });
        } else {
            // Ghostscript's own SVG device was withdrawn, so this is a last
            // resort and produces one page's worth of outlines rather than text.
            process.start(gs,
                          { QStringLiteral("-q"), QStringLiteral("-dBATCH"), QStringLiteral("-dNOPAUSE"),
                            QStringLiteral("-sDEVICE=svg"), QStringLiteral("-dFirstPage=%1").arg(page + 1),
                            QStringLiteral("-dLastPage=%1").arg(page + 1),
                            QStringLiteral("-sOutputFile=%1").arg(target), pdf });
        }
        if (!process.waitForStarted(10000) || !process.waitForFinished(GhostscriptTimeoutMs)) {
            process.kill();
            process.waitForFinished(2000);
            if (error) {
                *error = i18n("Saving page %1 as SVG did not finish.", page + 1);
            }
            return false;
        }
        const QString diagnostics = QString::fromLocal8Bit(process.readAllStandardError()).trimmed();
        if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0 || QFileInfo(target).size() <= 0) {
            if (error) {
                *error = diagnostics.isEmpty() ? i18n("Page %1 could not be saved as SVG.", page + 1) : diagnostics;
            }
            return false;
        }
    }
    return true;
}

// ── Tables ───────────────────────────────────────────────────────────────────

QVector<Convert::Table> Convert::findTables(const QString &pdf, const QVector<int> &pages, QString *error)
{
    Request request;
    request.pages = pages;
    request.wantRules = true;

    Analysis analysis;
    if (!analyse(pdf, request, &analysis, error)) {
        return {};
    }

    QVector<Table> tables;
    for (const PageAnalysis &page : std::as_const(analysis.pages)) {
        tables += tablesOnPage(page);
    }
    return tables;
}

bool Convert::tablesToCsv(const QString &pdf, const QString &outFile, const QVector<int> &pages, QString *error)
{
    QString local;
    const QVector<Table> tables = findTables(pdf, pages, &local);
    if (!local.isEmpty()) {
        if (error) {
            *error = local;
        }
        return false;
    }
    if (tables.isEmpty()) {
        if (error) {
            *error = i18n("No table was found in this document. There may be none, or its columns may not line up "
                          "well enough to be recognised.");
        }
        return false;
    }

    QString out;
    for (qsizetype index = 0; index < tables.size(); ++index) {
        if (index > 0) {
            // A blank line between tables, so that a file holding several of
            // them can be taken apart again.
            out += QLatin1Char('\n');
        }
        for (const QStringList &row : tables.at(index).rows) {
            QStringList fields;
            for (const QString &cell : row) {
                fields << csvField(cell);
            }
            out += fields.join(QLatin1Char(',')) + QLatin1Char('\n');
        }
    }
    return writeTextFile(outFile, out, error);
}

// ── PDF/X ────────────────────────────────────────────────────────────────────

bool Convert::isPdfXAvailable()
{
    return !ghostscript().isEmpty();
}

bool Convert::toPdfX(const QString &in, const QString &out, XLevel level, const QString &iccProfilePath,
                     QStringList *changes, QStringList *warnings, QString *error)
{
    if (!QFileInfo::exists(in)) {
        if (error) {
            *error = i18n("“%1” does not exist.", in);
        }
        return false;
    }
    const QString gs = ghostscript();
    if (gs.isEmpty()) {
        if (error) {
            *error = i18n("Ghostscript is not installed, so documents cannot be prepared for print. "
                          "Install the “ghostscript” package.");
        }
        return false;
    }

    QStringList madeChanges;
    QStringList raised;

    QString profile = iccProfilePath;
    if (!profile.isEmpty() && !QFileInfo::exists(profile)) {
        if (error) {
            *error = i18n("The colour profile “%1” does not exist.", profile);
        }
        return false;
    }
    if (profile.isEmpty()) {
        profile = findCmykProfile();
    }
    int components = 4;
    if (profile.isEmpty()) {
        raised.append(i18n("No CMYK printing profile was found on this system, so the file has no output intent "
                           "and is not yet PDF/X. Install the “icc-profiles-free” package, or name the profile "
                           "your printer asked for."));
    } else {
        components = profileComponents(profile);
        if (components == 0) {
            if (error) {
                *error = i18n("“%1” is not a colour profile this can read.", profile);
            }
            return false;
        }
        if (components != 4) {
            raised.append(i18n("“%1” is not a CMYK profile. It has been used as the output intent because it was "
                               "the one asked for, but a printer will expect a CMYK printing condition.",
                               QFileInfo(profile).fileName()));
        }
    }

    QTemporaryDir workspace;
    if (!workspace.isValid()) {
        if (error) {
            *error = i18n("Could not create a temporary directory.");
        }
        return false;
    }
    const QString prologue = workspace.filePath(u"pdf-smithy-pdfx.ps"_s);
    if (!writePdfxPrologue(prologue, profile, components, level, error)) {
        return false;
    }

    // Work into a temporary beside the destination so a failure never leaves a
    // half-written file where the user expects their document.
    QTemporaryFile temp(QFileInfo(out).absolutePath() + QLatin1String("/.pdf-smithy-pdfx-XXXXXX.pdf"));
    temp.setAutoRemove(false);
    if (!temp.open()) {
        if (error) {
            *error = i18n("Cannot write to “%1”.", QFileInfo(out).absolutePath());
        }
        return false;
    }
    const QString tempPath = temp.fileName();
    temp.close();
    const auto cleanUp = [&tempPath] { QFile::remove(tempPath); };

    // PDF/X-1a is built on PDF 1.3, which has no transparency at all, so asking
    // Ghostscript for that level is how the transparency gets flattened. X-4
    // exists precisely so that it need not be.
    const QString compatibility = level == XLevel::X4 ? QStringLiteral("1.6") : QStringLiteral("1.3");
    // X-3 allows colour to stay device-independent and be converted on the
    // press; X-1a does not, and requires everything in the file's own CMYK.
    const QString strategy
        = level == XLevel::X1a ? QStringLiteral("CMYK") : QStringLiteral("UseDeviceIndependentColor");

    QStringList arguments;
    if (!profile.isEmpty()) {
        // Ghostscript's sandbox is on by default and the prologue opens the
        // profile itself, so that one file has to be named as readable. It must
        // come first, before the switches it governs.
        arguments << QStringLiteral("--permit-file-read=%1").arg(profile);
    }
    arguments << QStringLiteral("-dPDFX") << QStringLiteral("-dBATCH") << QStringLiteral("-dNOPAUSE")
              << QStringLiteral("-dNOOUTERSAVE")
              // 1 means "leave out what cannot be made conformant and carry on",
              // which is what somebody with a deadline wants; the alternative is
              // no file at all.
              << QStringLiteral("-dPDFACompatibilityPolicy=1")
              << QStringLiteral("-sColorConversionStrategy=%1").arg(strategy)
              << QStringLiteral("-dProcessColorModel=/DeviceCMYK")
              << QStringLiteral("-dCompatibilityLevel=%1").arg(compatibility) << QStringLiteral("-sDEVICE=pdfwrite")
              << QStringLiteral("-sOutputFile=%1").arg(tempPath) << prologue << in;

    QProcess process;
    process.start(gs, arguments);
    if (!process.waitForStarted(10000)) {
        cleanUp();
        if (error) {
            *error = i18n("Ghostscript could not be started.");
        }
        return false;
    }
    if (!process.waitForFinished(GhostscriptTimeoutMs)) {
        process.kill();
        process.waitForFinished(2000);
        cleanUp();
        if (error) {
            *error = i18n("Preparing the document for print took too long and was stopped.");
        }
        return false;
    }
    const QString diagnostics = QString::fromLocal8Bit(process.readAllStandardError()).trimmed();
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        cleanUp();
        if (error) {
            *error = diagnostics.isEmpty() ? i18n("Ghostscript reported an error.") : diagnostics;
        }
        return false;
    }
    if (QFileInfo(tempPath).size() <= 0) {
        cleanUp();
        if (error) {
            *error = i18n("Preparing the document for print produced an empty file.");
        }
        return false;
    }

    const QString label = pdfxVersionLabel(level);
    const QString minimum = level == XLevel::X4 ? QStringLiteral("1.6") : QStringLiteral("1.4");
    if (!stampMetadata(tempPath, pdfxPacket(label, label), minimum, error)) {
        cleanUp();
        return false;
    }

    QFile::setPermissions(tempPath, QFile::ReadOwner | QFile::WriteOwner | QFile::ReadGroup | QFile::ReadOther);
    if (::rename(QFile::encodeName(tempPath).constData(), QFile::encodeName(out).constData()) != 0) {
        cleanUp();
        if (error) {
            *error = i18n("Could not replace “%1”.", out);
        }
        return false;
    }

    madeChanges.append(i18n("The file now identifies itself as %1.", label));
    madeChanges.append(i18n("Fonts were embedded, so the printer sets the text in the faces you chose rather than "
                            "in whatever the press happens to have."));
    if (!profile.isEmpty()) {
        madeChanges.append(i18n("An output intent was added from “%1”, so the file states the printing condition "
                                "its colours are meant for instead of leaving it to be guessed.",
                                QFileInfo(profile).fileName()));
    }
    if (level == XLevel::X1a) {
        madeChanges.append(i18n("Colours were converted to CMYK and transparency was flattened, neither of which "
                                "PDF/X-1a allows to be left as it was."));
    } else if (level == XLevel::X3) {
        madeChanges.append(i18n("Colours were kept device-independent where they already were, which is what "
                                "PDF/X-3 exists to allow."));
    } else {
        madeChanges.append(i18n("Transparency was left live, which is what PDF/X-4 exists to allow."));
    }

    if (diagnostics.contains(QStringLiteral("cannot guarantee creating a conformant"))) {
        raised.append(i18n("Some colours could not be converted for the stated printing condition, so a preflight "
                           "check may object to them."));
    }
    raised.append(i18n("The colour conversion is Ghostscript's, not a printer's. A proof is still the only way to "
                       "know what will come off the press."));

    if (changes) {
        *changes = madeChanges;
    }
    if (warnings) {
        *warnings = raised;
    }
    return true;
}

// ── Linearisation and version ────────────────────────────────────────────────

bool Convert::linearise(const QString &in, const QString &out, QString *error)
{
    try {
        QPDF pdf;
        PdfFile::open(pdf, in);
        QPDFWriter writer(pdf, QFile::encodeName(out).constData());
        writer.setLinearization(true);
        writer.setObjectStreamMode(qpdf_o_preserve);
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

bool Convert::setVersion(const QString &in, const QString &out, const QString &version, QStringList *warnings,
                         QString *error)
{
    const QPair<int, int> asked = parseVersion(version);
    if (asked.first < 0) {
        if (error) {
            *error = i18n("“%1” is not a PDF version. It looks like “1.7”.", version);
        }
        return false;
    }

    QStringList raised;
    try {
        QPDF pdf;
        PdfFile::open(pdf, in);

        QString needed = QStringLiteral("1.0");
        const QVector<Requirement> requirements = versionRequirementsOf(pdf);
        for (const Requirement &requirement : requirements) {
            if (!versionAtLeast(needed, requirement.version)) {
                needed = requirement.version;
            }
        }

        if (!versionAtLeast(version, needed)) {
            for (const Requirement &requirement : requirements) {
                if (!versionAtLeast(version, requirement.version)) {
                    raised.append(requirement.reason);
                }
            }
            if (warnings) {
                *warnings = raised;
            }
            if (error) {
                *error = i18n("This document cannot claim to be PDF %1: it uses features that need at least "
                              "PDF %2. Lowering the number would only mislead a reader that then meets them.",
                              version, needed);
            }
            return false;
        }

        const QString current = QString::fromStdString(pdf.getPDFVersion());
        if (!current.isEmpty() && versionAtLeast(version, current) && version != current) {
            raised.append(i18n("Raising the number from PDF %1 to PDF %2 does not add anything to the document; "
                               "it only says that a newer reader is expected.",
                               current, version));
        }

        QPDFWriter writer(pdf, QFile::encodeName(out).constData());
        writer.forcePDFVersion(version.toStdString());
        // Object streams and cross-reference streams arrived in 1.5, so writing
        // an older version has to leave them out rather than claim they are not
        // there.
        writer.setObjectStreamMode(asked.first > 1 || asked.second >= 5 ? qpdf_o_preserve : qpdf_o_disable);
        writer.setStreamDataMode(qpdf_s_preserve);
        writer.write();
    } catch (const std::exception &e) {
        if (error) {
            *error = QString::fromUtf8(e.what());
        }
        return false;
    }

    if (warnings) {
        *warnings = raised;
    }
    return true;
}

QStringList Convert::limitations()
{
    return {
        i18n("A PDF holds glyphs at coordinates, not sentences. Reading order, paragraphs and columns are worked "
             "out from the geometry, which is inference and not extraction."),
        i18n("Text that is part of a scanned picture is not text and does not come out. Run the page through text "
             "recognition first."),
        i18n("Two columns are found by the white gutter between them. Three or more columns are found the same "
             "way, but a layout with the columns interleaved by hand, or one whose gutter is filled by a picture, "
             "will come out in the wrong order."),
        i18n("A table is read column by column by the plain-text export, because its columns look exactly like a "
             "page's columns. Use the table detection for tables."),
        i18n("Which paragraph is a heading, which line is a list item and which words are emphasised are all "
             "guessed: from size, from leading characters and from the font's own name. A document with no "
             "consistent body size, or a font family whose bold face is not called bold, will be guessed wrong."),
        i18n("A word broken by a hyphen at the end of a line is joined back together when the next line starts in "
             "lower case, which is wrong for a compound whose hyphen was real."),
        i18n("Pictures are only carried into HTML when the page draws them itself and they are in a format that "
             "can be decoded here. Fax and JPEG 2000 pictures, and pictures drawn inside an embedded drawing, are "
             "left out."),
        i18n("Table detection needs every row to have the same number of cells, so a table with cells left empty "
             "or merged across columns is found in pieces or not at all."),
        i18n("Confidence is a judgement about how table-like the geometry is, not a probability. Check the result "
             "before relying on it."),
        i18n("PDF/X conversion is Ghostscript's, and this is not a preflight check. A file that comes out of it "
             "still has to pass whatever your printer runs."),
        i18n("The claimed PDF version is only a claim. Lowering it is refused where the document uses features an "
             "older reader would not understand, and raising it adds nothing."),
    };
}

} // namespace ps
