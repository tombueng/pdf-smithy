/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#include "Typeset.h"

#include "Core14Widths.h"
#include "PdfFile.h"
#include "PdfGeometry.h"

#include <KLocalizedString>

#include <QDate>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QLocale>
#include <QRectF>
#include <QRegularExpression>
#include <QSet>
#include <QStringConverter>

#include <qpdf/QPDF.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>
#include <qpdf/QPDFWriter.hh>

#include <algorithm>
#include <set>

namespace ps {

namespace {

using PdfGeometry::number;

// ---------------------------------------------------------------------------
//  Fonts
// ---------------------------------------------------------------------------

/**
 * The three families a document can actually be set in.
 *
 * Symbol and ZapfDingbats are among the standard fourteen, but they encode
 * pictures rather than language, so offering them as a text family would only
 * produce pages of arrows; a request for one lands on Helvetica instead.
 */
enum class Family { Sans, Serif, Mono };

Family familyFromName(const QString &name)
{
    const QString key = name.toLower().remove(QLatin1Char(' ')).remove(QLatin1Char('-'));
    if (key.contains(QLatin1String("courier")) || key.contains(QLatin1String("mono"))) {
        return Family::Mono;
    }
    // Tested before the serif names, because "sans-serif" contains "serif".
    if (key.contains(QLatin1String("sans")) || key.contains(QLatin1String("helvetica"))
        || key.contains(QLatin1String("arial"))) {
        return Family::Sans;
    }
    if (key.contains(QLatin1String("times")) || key.contains(QLatin1String("serif"))
        || key.contains(QLatin1String("roman")) || key.contains(QLatin1String("georgia"))
        || key.contains(QLatin1String("garamond")) || key.contains(QLatin1String("palatino"))) {
        return Family::Serif;
    }
    return Family::Sans;
}

/** Index into Core14::table, whose four variants of each family are in this order. */
int core14IndexFor(Family family, bool bold, bool italic)
{
    const int base = family == Family::Serif ? 4 : family == Family::Mono ? 8 : 0;
    return base + (bold ? 1 : 0) + (italic ? 2 : 0);
}

std::string resourceName(int fontIndex)
{
    return "/F" + std::to_string(fontIndex);
}

// ---------------------------------------------------------------------------
//  WinAnsiEncoding
// ---------------------------------------------------------------------------

/**
 * The part of WinAnsiEncoding that is neither ASCII nor Latin-1.
 *
 * Codes 0x80 to 0x9F are where the typographic characters live (the real
 * quotation marks, the dashes, the ellipsis), and getting them onto the page is
 * the difference between proper quotes and two apostrophes.
 */
struct WinAnsiHigh {
    char16_t character;
    quint8 code;
};

constexpr WinAnsiHigh winAnsiHigh[] = {
    { 0x20AC, 0x80 }, { 0x201A, 0x82 }, { 0x0192, 0x83 }, { 0x201E, 0x84 }, { 0x2026, 0x85 }, { 0x2020, 0x86 },
    { 0x2021, 0x87 }, { 0x02C6, 0x88 }, { 0x2030, 0x89 }, { 0x0160, 0x8A }, { 0x2039, 0x8B }, { 0x0152, 0x8C },
    { 0x017D, 0x8E }, { 0x2018, 0x91 }, { 0x2019, 0x92 }, { 0x201C, 0x93 }, { 0x201D, 0x94 }, { 0x2022, 0x95 },
    { 0x2013, 0x96 }, { 0x2014, 0x97 }, { 0x02DC, 0x98 }, { 0x2122, 0x99 }, { 0x0161, 0x9A }, { 0x203A, 0x9B },
    { 0x0153, 0x9C }, { 0x017E, 0x9E }, { 0x0178, 0x9F },
};

/** Characters that carry no width and no mark, and whose loss nobody needs telling about. */
bool isIgnorable(QChar character)
{
    const char16_t code = character.unicode();
    // The soft hyphen belongs to whichever engine breaks the line, and this one
    // decides for itself where a word may be split.
    return code == 0x00AD || code == 0x200B || code == 0x200C || code == 0x200D || code == 0x2060 || code == 0xFEFF
        || (code < 0x20 && code != 0x0A);
}

/** The WinAnsi code for a character, or -1 when a standard font cannot write it. */
int winAnsiCode(QChar character)
{
    const char16_t code = character.unicode();
    if (code >= 0x20 && code <= 0x7E) {
        return int(code);
    }
    // WinAnsi calls 0xA0 "space", and keeping the character as U+00A0 until here
    // is what stops a line breaking inside it.
    if (code == 0x00A0) {
        return 0x20;
    }
    if (code >= 0x00A1 && code <= 0x00FF && code != 0x00AD) {
        return int(code);
    }
    for (const WinAnsiHigh &entry : winAnsiHigh) {
        if (entry.character == code) {
            return entry.code;
        }
    }
    return -1;
}

/** A PDF literal string, in WinAnsi bytes, with the three escapes that matter. */
std::string literalString(const QString &text)
{
    std::string out = "(";
    for (const QChar &character : text) {
        const int code = winAnsiCode(character);
        if (code < 0) {
            continue;
        }
        if (code == '(' || code == ')' || code == '\\') {
            out += '\\';
        }
        out += char(quint8(code));
    }
    return out + ")";
}

// ---------------------------------------------------------------------------
//  Measuring
// ---------------------------------------------------------------------------

/**
 * Advance in thousandths of the size, summed as integers.
 *
 * The integer sum is not tidiness. A caller that measures "Wo" at ten points and
 * compares the answer against (833 + 556) / 100 has every right to exact
 * agreement, and per-character floating point does not deliver it.
 */
quint32 advanceThousandths(const QString &text, int fontIndex)
{
    const Core14::Entry &entry = Core14::table[fontIndex];
    quint32 total = 0;
    for (const QChar &character : text) {
        const int code = winAnsiCode(character);
        if (code < Core14::firstCode || code > Core14::lastCode) {
            continue;
        }
        total += entry.widths[code - Core14::firstCode];
    }
    return total;
}

double advanceOf(const QString &text, int fontIndex, double fontSize)
{
    return double(advanceThousandths(text, fontIndex)) * fontSize / 1000.0;
}

int spacesIn(const QString &text)
{
    int count = 0;
    for (const QChar &character : text) {
        if (winAnsiCode(character) == 0x20) {
            ++count;
        }
    }
    return count;
}

// ---------------------------------------------------------------------------
//  Telling the caller what happened
// ---------------------------------------------------------------------------

/** Collects the report's notes, each thing said once however often it happened. */
struct Notes {
    Typeset::Report report;
    QSet<QString> seen;

    bool first(const QString &key)
    {
        if (seen.contains(key)) {
            return false;
        }
        seen.insert(key);
        return true;
    }

    void character(QChar value)
    {
        const QString text = QString(value);
        if (!first(QStringLiteral("char:") + text)) {
            return;
        }
        report.overflows.append(i18n("The character “%1” is outside WinAnsi, which is all the standard fonts can "
                                     "write, and was left out.",
                                     text));
    }

    void wideWord(const QString &word)
    {
        if (!first(QStringLiteral("word:") + word)) {
            return;
        }
        report.overflows.append(i18n("“%1” is wider than the column and had to be broken across lines.", word));
    }

    void tallItem()
    {
        if (!first(QStringLiteral("tall"))) {
            return;
        }
        report.overflows.append(i18n("Something is taller than the column holding it and runs past the margin."));
    }
};

/** Drops what cannot be written and says so, so that nothing changes glyph in silence. */
QString sanitise(const QString &text, Notes *notes)
{
    QString out;
    out.reserve(text.size());
    for (const QChar &character : text) {
        if (character == QLatin1Char('\t')) {
            out += QLatin1Char(' ');
            continue;
        }
        if (isIgnorable(character)) {
            continue;
        }
        if (winAnsiCode(character) >= 0) {
            out += character;
            continue;
        }
        if (notes) {
            notes->character(character);
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
//  The document as blocks
// ---------------------------------------------------------------------------

/** One run of inline text, or the white space between two of them. */
struct Token {
    QString text;
    bool space = false;
    bool bold = false;
    bool italic = false;
    bool mono = false;
    bool faint = false; //!< the parenthesised target of a link, and nothing else
    QString link;
};

enum class BlockKind { Paragraph, Heading, ListItem, Quote, Code, Rule, Table };

struct Block {
    BlockKind kind = BlockKind::Paragraph;
    Typeset::Style style;
    QVector<Token> tokens;

    QString marker; //!< the bullet or the number of a list item
    bool orderedMarker = false; //!< numbers are ranged right in the gutter, bullets left
    double indent = 0.0; //!< nesting, on top of the style's own left indent

    QStringList verbatim; //!< code, exactly as it was typed
    QVector<QVector<QVector<Token>>> rows; //!< table: row, then cell, then the cell's tokens
    QVector<Qt::Alignment> columnAlignments;
};

double leadingOf(const Typeset::Style &style)
{
    return style.leading > 0.0 ? style.leading : style.fontSize * 1.35;
}

/** How far below the top of a line box its baseline sits. */
double ascentOf(double fontSize)
{
    return fontSize * 0.85;
}

/**
 * The style for a heading level, derived from the body when the caller has not
 * given one.
 *
 * Deriving rather than hard-coding is the difference between a document set in
 * Times and a document set in Times with Helvetica headings, which is what
 * every naïve converter produces and nobody wants.
 */
Typeset::Style headingStyle(const Typeset::Document &document, int level)
{
    const int index = qBound(1, level, 6) - 1;
    if (index < document.headings.size()) {
        return document.headings.at(index);
    }

    constexpr double scale[] = { 1.9, 1.55, 1.3, 1.15, 1.05, 1.0 };
    Typeset::Style style = document.body;
    style.fontSize = document.body.fontSize * scale[index];
    style.leading = style.fontSize * 1.18;
    style.bold = true;
    style.italic = index >= 5;
    // A justified heading is never right, whatever the body does.
    style.alignment = Qt::AlignLeft;
    style.indentFirst = 0.0;
    style.spaceBefore = index == 0 ? style.fontSize * 0.4 : style.fontSize * 0.9;
    style.spaceAfter = style.fontSize * 0.35;
    style.keepWithNext = true;
    return style;
}

Typeset::Style codeStyle(const Typeset::Document &document)
{
    Typeset::Style style = document.body;
    style.family = QStringLiteral("Courier");
    style.fontSize = document.body.fontSize * 0.95;
    style.leading = style.fontSize * 1.25;
    style.bold = false;
    style.italic = false;
    style.alignment = Qt::AlignLeft;
    style.indentFirst = 0.0;
    style.indentLeft = document.body.indentLeft + document.body.fontSize * 0.8;
    style.spaceBefore = document.body.fontSize * 0.5;
    style.spaceAfter = document.body.fontSize * 0.5;
    style.keepWithNext = false;
    return style;
}

Typeset::Style quoteStyle(const Typeset::Document &document)
{
    Typeset::Style style = document.body;
    style.indentLeft = document.body.indentLeft + document.body.fontSize * 1.5;
    style.indentFirst = 0.0;
    style.colour = QColor(70, 70, 70);
    style.spaceBefore = document.body.fontSize * 0.5;
    style.spaceAfter = document.body.fontSize * 0.5;
    return style;
}

// ---------------------------------------------------------------------------
//  Words, lines, and where a long word may be split
// ---------------------------------------------------------------------------

struct Word {
    QString text;
    int font = 0;
    double size = 11.0;
    QColor colour;
    QString link;
    double width = 0.0;
    bool glued = false; //!< follows the word before it with no space, as in "**bold**face"
};

struct DraftLine {
    QVector<Word> words;
    bool lastOfParagraph = false;
};

bool isVowel(QChar character)
{
    static const QString vowels = QStringLiteral("aeiouyäöüáàâéèêíìîóòôúùûåæø");
    return vowels.contains(character.toLower());
}

/** Consonant pairs no language on offer here splits, whatever the pattern says. */
bool isDigraph(QChar first, QChar second)
{
    static const QStringList pairs = { QStringLiteral("ch"), QStringLiteral("ck"), QStringLiteral("ph"),
                                       QStringLiteral("sh"), QStringLiteral("th"), QStringLiteral("gh") };
    const QString pair = QString(first.toLower()) + QString(second.toLower());
    return pairs.contains(pair);
}

/**
 * Places where a long word may be split, as an offset and whether a hyphen has
 * to be added there.
 *
 * An existing hyphen is always a fair break and needs nothing added. The pattern
 * rule (vowel, two consonants, vowel, break between the consonants) is the one
 * piece of hyphenation that is right far more often than wrong in English and
 * German alike, once the consonant pairs that stand for a single sound are left
 * whole. It is not a dictionary and does not pretend to be, which is why
 * Document::hyphenate is off unless asked for.
 *
 * @p language decides how much of a word has to stay behind: German composition
 * has always allowed two characters, English typography asks for three.
 */
QVector<QPair<int, bool>> breakPoints(const QString &word, bool patterns, const QString &language)
{
    const int minimumHead = language.startsWith(QLatin1String("de"), Qt::CaseInsensitive) ? 2 : 3;
    const int minimumTail = 2;

    QVector<QPair<int, bool>> points;
    const auto offer = [&](int offset, bool hyphen) {
        if (offset >= minimumHead && word.size() - offset >= minimumTail) {
            points.append({ offset, hyphen });
        }
    };

    for (int i = 1; i < word.size() - 1; ++i) {
        if (word.at(i) == QLatin1Char('-') || word.at(i) == QLatin1Char('/')) {
            offer(i + 1, false);
        }
    }
    if (patterns) {
        for (int i = 2; i + 1 < word.size(); ++i) {
            if (!word.at(i - 1).isLetter() || !word.at(i).isLetter() || isDigraph(word.at(i - 1), word.at(i))) {
                continue;
            }
            if (isVowel(word.at(i - 2)) && !isVowel(word.at(i - 1)) && !isVowel(word.at(i))
                && isVowel(word.at(i + 1))) {
                offer(i, true);
            }
        }
        std::sort(points.begin(), points.end());
    }
    return points;
}

/** The widest split of @p word that fits @p budget, or a zero offset when there is none. */
QPair<int, bool> widestSplit(const Word &word, double budget, bool patterns, const QString &language)
{
    QPair<int, bool> best(0, false);
    for (const QPair<int, bool> &point : breakPoints(word.text, patterns, language)) {
        const QString head = word.text.left(point.first) + (point.second ? QStringLiteral("-") : QString());
        if (advanceOf(head, word.font, word.size) <= budget) {
            best = point;
        }
    }
    return best;
}

Word pieceOf(const Word &word, const QString &text, bool glued)
{
    Word piece = word;
    piece.text = text;
    piece.glued = glued;
    piece.width = advanceOf(text, word.font, word.size);
    return piece;
}

/**
 * Greedy first-fit line breaking.
 *
 * Greedy rather than the whole-paragraph optimum, and that is a deliberate
 * trade. What matters far more than the last few percent of evenness is that no
 * word is ever lost and no line ever runs past the column, and a greedy pass can
 * be made to guarantee both line by line.
 */
QVector<DraftLine> breakIntoLines(const QVector<Word> &input, double firstWidth, double otherWidth, bool hyphenate,
                                  const QString &language, Notes *notes)
{
    QVector<DraftLine> lines;
    DraftLine current;
    double used = 0.0;
    double avail = qMax(1.0, firstWidth);

    const auto flush = [&]() {
        lines.append(current);
        current = DraftLine();
        used = 0.0;
        avail = qMax(1.0, otherWidth);
    };

    QVector<Word> queue = input;
    for (int i = 0; i < queue.size(); ++i) {
        Word word = queue.at(i);
        if (word.text.isEmpty()) {
            continue;
        }

        const bool atLineStart = current.words.isEmpty();
        const double gap = atLineStart || word.glued
            ? 0.0
            : advanceOf(QStringLiteral(" "), current.words.constLast().font, current.words.constLast().size);
        if (atLineStart) {
            word.glued = false;
        }

        if (used + gap + word.width <= avail + 0.001) {
            used += gap + word.width;
            current.words.append(word);
            continue;
        }

        // Hyphenation earns its keep here and nowhere else: the word does not
        // fit, and part of it might.
        if (hyphenate && !atLineStart && !word.glued) {
            const QPair<int, bool> split = widestSplit(word, avail - used - gap, true, language);
            if (split.first >= 2) {
                const QString head = word.text.left(split.first) + (split.second ? QStringLiteral("-") : QString());
                current.words.append(pieceOf(word, head, false));
                queue[i] = pieceOf(word, word.text.mid(split.first), false);
                --i; // the remainder still has to be placed
                flush();
                continue;
            }
        }

        if (!atLineStart) {
            flush();
            --i; // try the same word again on the fresh line
            continue;
        }

        // A single word wider than the whole column. Breaking it is the lesser
        // evil (the alternative is text running off the paper), but the caller
        // is told, because the real fix is a wider column or a smaller size.
        if (notes) {
            notes->wideWord(word.text);
        }
        QString rest = word.text;
        while (!rest.isEmpty()) {
            const Word probe = pieceOf(word, rest, false);
            if (probe.width <= avail + 0.001) {
                current.words.append(probe);
                used = probe.width;
                break;
            }
            const QPair<int, bool> split = widestSplit(probe, avail, hyphenate, language);
            int take = split.first;
            bool hyphen = split.second;
            if (take < 1) {
                hyphen = false;
                take = 1;
                while (take < rest.size() && advanceOf(rest.left(take + 1), word.font, word.size) <= avail) {
                    ++take;
                }
            }
            current.words.append(pieceOf(word, rest.left(take) + (hyphen ? QStringLiteral("-") : QString()), false));
            rest = rest.mid(take);
            flush();
        }
    }

    if (!current.words.isEmpty()) {
        lines.append(current);
    }
    if (!lines.isEmpty()) {
        lines.last().lastOfParagraph = true;
    }
    return lines;
}

// ---------------------------------------------------------------------------
//  What goes on a page
// ---------------------------------------------------------------------------

struct Fragment {
    QString text;
    double x = 0.0; //!< from the column's left edge
    double dy = 0.0; //!< the baseline, downwards from the top of the item
    int font = 0;
    double size = 11.0;
    QColor colour;
    double wordSpacing = 0.0;
    QString link;
};

struct Decoration {
    double x = 0.0;
    double dy = 0.0; //!< top edge, downwards from the top of the item
    double width = 0.0;
    double height = 0.0;
    QColor colour;
};

/** The smallest thing the page breaker will not cut in half: a line, or a table row. */
struct Item {
    double height = 0.0;
    QVector<Fragment> fragments;
    QVector<Decoration> decorations;
};

/** One block, laid out. Widows and orphans are decided within a group. */
struct Group {
    QVector<Item> items;
    double spaceBefore = 0.0;
    double spaceAfter = 0.0;
    bool keepWithNext = false;
};

QVector<Word> wordsFromTokens(const QVector<Token> &tokens, const Typeset::Style &style)
{
    const Family family = familyFromName(style.family);
    QVector<Word> words;
    bool pendingSpace = true; // there is nothing to glue the first word to

    for (const Token &token : tokens) {
        if (token.space) {
            pendingSpace = true;
            continue;
        }
        Word word;
        word.text = token.text;
        word.size = token.mono ? style.fontSize * 0.95 : style.fontSize;
        word.font = core14IndexFor(token.mono ? Family::Mono : family, token.bold || style.bold,
                                   token.italic || style.italic);
        word.colour = token.faint ? QColor(90, 90, 110) : style.colour;
        word.link = token.link;
        word.width = advanceOf(word.text, word.font, word.size);
        word.glued = !pendingSpace;
        words.append(word);
        pendingSpace = false;
    }
    return words;
}

struct FragmentDraft {
    QString text;
    int font = 0;
    double size = 11.0;
    QColor colour;
    QString link;
};

/**
 * Runs of one font become one fragment, and the space that separates two
 * fragments joins the earlier one.
 *
 * That is what lets `Tw` do the justifying: the operator widens the space
 * character wherever it occurs, so every gap between words has to be a real
 * space inside some string rather than a jump in the text matrix.
 */
QVector<FragmentDraft> mergeWords(const DraftLine &line)
{
    QVector<FragmentDraft> out;
    for (int i = 0; i < line.words.size(); ++i) {
        const Word &word = line.words.at(i);
        const bool needSpace = i > 0 && !word.glued;
        const bool sameRun = !out.isEmpty() && out.constLast().font == word.font && out.constLast().size == word.size
            && out.constLast().colour == word.colour && out.constLast().link == word.link;
        if (sameRun) {
            if (needSpace) {
                out.last().text += QLatin1Char(' ');
            }
            out.last().text += word.text;
            continue;
        }
        if (needSpace && !out.isEmpty()) {
            out.last().text += QLatin1Char(' ');
        }
        FragmentDraft fragment;
        fragment.text = word.text;
        fragment.font = word.font;
        fragment.size = word.size;
        fragment.colour = word.colour;
        fragment.link = word.link;
        out.append(fragment);
    }
    return out;
}

/** One drawn line: alignment applied, word spacing worked out, positions final. */
QVector<Fragment> positionLine(const DraftLine &line, const Typeset::Style &style, double indent, double lineWidth,
                               bool justify, double baseline)
{
    const QVector<FragmentDraft> drafts = mergeWords(line);

    double natural = 0.0;
    int spaces = 0;
    for (const FragmentDraft &draft : drafts) {
        natural += advanceOf(draft.text, draft.font, draft.size);
        spaces += spacesIn(draft.text);
    }

    const Qt::Alignment horizontal = style.alignment & Qt::AlignHorizontal_Mask;
    double wordSpacing = 0.0;
    double x = indent;

    if (justify && horizontal == Qt::AlignJustify && spaces > 0 && natural < lineWidth) {
        wordSpacing = (lineWidth - natural) / double(spaces);
        // Beyond about an em a justified line reads as a row of islands, and
        // left-aligned is the better-looking failure.
        if (wordSpacing > style.fontSize) {
            wordSpacing = 0.0;
        }
    } else if (horizontal == Qt::AlignRight) {
        x = indent + lineWidth - natural;
    } else if (horizontal == Qt::AlignHCenter) {
        x = indent + (lineWidth - natural) / 2.0;
    }

    QVector<Fragment> out;
    for (const FragmentDraft &draft : drafts) {
        Fragment fragment;
        fragment.text = draft.text;
        fragment.font = draft.font;
        fragment.size = draft.size;
        fragment.colour = draft.colour;
        fragment.link = draft.link;
        fragment.wordSpacing = wordSpacing;
        fragment.x = x;
        fragment.dy = baseline;
        out.append(fragment);
        x += advanceOf(draft.text, draft.font, draft.size) + wordSpacing * double(spacesIn(draft.text));
    }
    return out;
}

// ---------------------------------------------------------------------------
//  Blocks to items
// ---------------------------------------------------------------------------

Group layoutParagraph(const Block &block, double columnWidth, const Typeset::Document &document, Notes *notes)
{
    const Typeset::Style &style = block.style;
    Group group;
    group.spaceBefore = style.spaceBefore;
    group.spaceAfter = style.spaceAfter;
    group.keepWithNext = style.keepWithNext;

    const double gutter = block.marker.isEmpty() ? 0.0 : style.fontSize * 1.7;
    const double left = style.indentLeft + block.indent + gutter;
    const double bodyWidth = qMax(style.fontSize, columnWidth - left - style.indentRight);
    const double firstWidth = qMax(style.fontSize, bodyWidth - style.indentFirst);

    const QVector<Word> words = wordsFromTokens(block.tokens, style);
    const QVector<DraftLine> lines
        = breakIntoLines(words, firstWidth, bodyWidth, document.hyphenate, document.language, notes);

    for (int i = 0; i < lines.size(); ++i) {
        const bool first = i == 0;
        const double baseline = ascentOf(style.fontSize);
        Item item;
        item.height = leadingOf(style);
        item.fragments = positionLine(lines.at(i), style, first ? left + style.indentFirst : left,
                                      first ? firstWidth : bodyWidth, !lines.at(i).lastOfParagraph, baseline);

        if (first && !block.marker.isEmpty()) {
            Fragment marker;
            marker.text = block.marker;
            marker.font = core14IndexFor(familyFromName(style.family), style.bold, false);
            marker.size = style.fontSize;
            marker.colour = style.colour;
            marker.dy = baseline;
            // Numbers are ranged right against the text, so that "9." and "10."
            // keep one another's company instead of drifting apart.
            marker.x = block.orderedMarker
                ? left - style.fontSize * 0.45 - advanceOf(marker.text, marker.font, marker.size)
                : style.indentLeft + block.indent;
            item.fragments.prepend(marker);
        }

        if (block.kind == BlockKind::Quote) {
            Decoration bar;
            bar.x = style.indentLeft + block.indent + style.fontSize * 0.35;
            bar.width = qMax(1.2, style.fontSize * 0.15);
            bar.height = item.height;
            bar.colour = QColor(160, 160, 170);
            item.decorations.append(bar);
        }

        group.items.append(item);
    }
    return group;
}

Group layoutCode(const Block &block, double columnWidth, Notes *notes)
{
    const Typeset::Style &style = block.style;
    Group group;
    group.spaceBefore = style.spaceBefore;
    group.spaceAfter = style.spaceAfter;

    const int font = core14IndexFor(Family::Mono, false, false);
    const double left = style.indentLeft;
    const double width = qMax(style.fontSize, columnWidth - left - style.indentRight);

    for (const QString &raw : block.verbatim) {
        const QString text = sanitise(raw, notes);
        QStringList pieces;
        QString rest = text;
        // Measured rather than counted, even though Courier is monospaced: a
        // wrapped code line is ugly, one running into the margin is worse.
        while (advanceOf(rest, font, style.fontSize) > width + 0.001 && rest.size() > 1) {
            int take = 1;
            while (take < rest.size() && advanceOf(rest.left(take + 1), font, style.fontSize) <= width) {
                ++take;
            }
            pieces.append(rest.left(take));
            rest = rest.mid(take);
            if (notes) {
                notes->wideWord(text.trimmed());
            }
        }
        pieces.append(rest);

        for (const QString &piece : pieces) {
            Item item;
            item.height = leadingOf(style);

            Decoration background;
            background.x = qMax(0.0, left - style.fontSize * 0.4);
            background.width = qMin(columnWidth, width + style.fontSize * 0.8);
            background.height = item.height;
            background.colour = QColor(244, 244, 247);
            item.decorations.append(background);

            Fragment fragment;
            fragment.text = piece;
            fragment.font = font;
            fragment.size = style.fontSize;
            fragment.colour = style.colour;
            fragment.x = left;
            fragment.dy = ascentOf(style.fontSize);
            item.fragments.append(fragment);

            group.items.append(item);
        }
    }
    return group;
}

Group layoutRule(const Block &block, double columnWidth)
{
    Group group;
    group.spaceBefore = block.style.fontSize * 0.7;
    group.spaceAfter = block.style.fontSize * 0.7;

    Item item;
    item.height = qMax(1.0, block.style.fontSize * 0.5);
    Decoration line;
    line.width = columnWidth;
    line.dy = item.height / 2.0;
    line.height = 0.6;
    line.colour = QColor(150, 150, 160);
    item.decorations.append(line);
    group.items.append(item);
    return group;
}

Group layoutTable(const Block &block, double columnWidth, const Typeset::Document &document, Notes *notes)
{
    const Typeset::Style &style = block.style;
    Group group;
    group.spaceBefore = style.fontSize * 0.7;
    group.spaceAfter = style.fontSize * 0.7;

    int columns = 0;
    for (const QVector<QVector<Token>> &row : block.rows) {
        columns = qMax(columns, int(row.size()));
    }
    if (columns == 0) {
        return group;
    }

    const double padding = qMax(2.0, style.fontSize * 0.35);
    const double ruleWidth = 0.5;

    // Columns take their share of the width from the widest thing in them, so a
    // table of dates and sentences does not give the dates half the page.
    QVector<double> natural(columns, 0.0);
    for (const QVector<QVector<Token>> &row : block.rows) {
        for (int c = 0; c < row.size(); ++c) {
            const QVector<Word> words = wordsFromTokens(row.at(c), style);
            double sum = 0.0;
            for (int i = 0; i < words.size(); ++i) {
                sum += words.at(i).width;
                if (i > 0 && !words.at(i).glued) {
                    sum += advanceOf(QStringLiteral(" "), words.at(i).font, words.at(i).size);
                }
            }
            natural[c] = qMax(natural.at(c), sum + 2.0 * padding);
        }
    }

    double total = 0.0;
    for (const double value : natural) {
        total += value;
    }
    QVector<double> widths(columns, columnWidth / double(columns));
    if (total > 0.0) {
        for (int c = 0; c < columns; ++c) {
            widths[c] = natural.at(c) * columnWidth / total;
        }
    }
    QVector<double> offsets(columns + 1, 0.0);
    for (int c = 0; c < columns; ++c) {
        offsets[c + 1] = offsets.at(c) + widths.at(c);
    }

    for (int r = 0; r < block.rows.size(); ++r) {
        const QVector<QVector<Token>> &row = block.rows.at(r);
        Item item;

        QVector<QVector<DraftLine>> cellLines;
        int tallest = 1;
        for (int c = 0; c < columns; ++c) {
            const QVector<Token> tokens = c < row.size() ? row.at(c) : QVector<Token>();
            const double inner = qMax(style.fontSize, widths.at(c) - 2.0 * padding);
            const QVector<DraftLine> lines = breakIntoLines(wordsFromTokens(tokens, style), inner, inner,
                                                            document.hyphenate, document.language, notes);
            tallest = qMax(tallest, int(lines.size()));
            cellLines.append(lines);
        }
        item.height = 2.0 * padding + double(tallest) * leadingOf(style);

        for (int c = 0; c < columns; ++c) {
            Typeset::Style cellStyle = style;
            cellStyle.alignment = c < block.columnAlignments.size() ? block.columnAlignments.at(c) : Qt::AlignLeft;
            cellStyle.indentFirst = 0.0;
            const double inner = qMax(style.fontSize, widths.at(c) - 2.0 * padding);
            for (int l = 0; l < cellLines.at(c).size(); ++l) {
                item.fragments += positionLine(cellLines.at(c).at(l), cellStyle, offsets.at(c) + padding, inner, false,
                                               padding + ascentOf(style.fontSize) + double(l) * leadingOf(style));
            }
        }

        const QColor grid(120, 120, 130);
        if (r == 0) {
            Decoration top;
            top.width = columnWidth;
            top.height = ruleWidth;
            top.colour = grid;
            item.decorations.append(top);
        }
        Decoration bottom;
        bottom.dy = item.height - ruleWidth;
        bottom.width = columnWidth;
        bottom.height = ruleWidth;
        bottom.colour = grid;
        item.decorations.append(bottom);
        for (int c = 0; c <= columns; ++c) {
            Decoration vertical;
            vertical.x = qMin(offsets.at(c), columnWidth - ruleWidth);
            vertical.width = ruleWidth;
            vertical.height = item.height;
            vertical.colour = grid;
            item.decorations.append(vertical);
        }

        group.items.append(item);
    }
    return group;
}

// ---------------------------------------------------------------------------
//  Filling columns and pages
// ---------------------------------------------------------------------------

struct DrawText {
    QString text;
    double x = 0.0;
    double y = 0.0;
    int font = 0;
    double size = 11.0;
    QColor colour;
    double wordSpacing = 0.0;
    QString link;
};

struct DrawBox {
    QRectF box;
    QColor colour;
};

struct OutPage {
    QVector<DrawText> texts;
    QVector<DrawBox> boxes;
};

/**
 * The page breaker.
 *
 * Everything interesting about it is in place(): where a group of lines may be
 * cut, and where it has to be moved along whole. The rest is arithmetic.
 */
struct Flow {
    const Typeset::Document *document = nullptr;
    Notes *notes = nullptr;
    double columnWidth = 0.0;
    int columnCount = 1;

    QVector<OutPage> pages;
    int column = 0;
    double cursor = 0.0;
    double pendingSpaceAfter = 0.0;
    bool atTop = true;

    double columnTop() const { return document->pageSize.height() - document->marginTop; }
    double columnBottom() const { return document->marginBottom; }
    double leftOf(int index) const
    {
        return document->marginLeft + double(index) * (columnWidth + document->columnGap);
    }

    void newPage()
    {
        pages.append(OutPage());
        column = 0;
        cursor = columnTop();
        atTop = true;
        pendingSpaceAfter = 0.0;
    }

    void nextColumn()
    {
        if (pages.isEmpty() || column + 1 >= columnCount) {
            newPage();
            return;
        }
        ++column;
        cursor = columnTop();
        atTop = true;
        pendingSpaceAfter = 0.0;
    }

    /** How many of a group's remaining items fit below @p from. */
    int fitCount(const Group &group, int start, double from) const
    {
        double y = from;
        int count = 0;
        for (int i = start; i < group.items.size(); ++i) {
            const double next = y - group.items.at(i).height;
            if (next < columnBottom() - 0.001) {
                break;
            }
            y = next;
            ++count;
        }
        return count;
    }

    void emitItem(const Item &item, double top)
    {
        OutPage &page = pages.last();
        const double left = leftOf(column);
        for (const Fragment &fragment : item.fragments) {
            DrawText text;
            text.text = fragment.text;
            text.x = left + fragment.x;
            text.y = top - fragment.dy;
            text.font = fragment.font;
            text.size = fragment.size;
            text.colour = fragment.colour;
            text.wordSpacing = fragment.wordSpacing;
            text.link = fragment.link;
            page.texts.append(text);
        }
        for (const Decoration &decoration : item.decorations) {
            DrawBox box;
            box.box = QRectF(left + decoration.x, top - decoration.dy - decoration.height, decoration.width,
                             decoration.height);
            box.colour = decoration.colour;
            page.boxes.append(box);
        }
    }

    void place(const Group &group, const Group *next)
    {
        if (pages.isEmpty()) {
            newPage();
        }
        const int total = group.items.size();
        if (total == 0) {
            pendingSpaceAfter = qMax(pendingSpaceAfter, group.spaceAfter);
            return;
        }

        int placed = 0;
        int guard = 0;
        while (placed < total && ++guard < 100000) {
            const double gap = placed == 0 && !atTop ? qMax(pendingSpaceAfter, group.spaceBefore) : 0.0;
            const int remaining = total - placed;
            const int fits = fitCount(group, placed, cursor - gap);
            int want = fits;
            bool moveOn = fits == 0;

            if (fits < remaining) {
                // Leaving one line behind is the widow; taking two over instead
                // costs nothing and is what a compositor would do.
                if (remaining - fits == 1 && fits >= 2) {
                    --want;
                }
                // And a lone first line at the foot of a column is the orphan.
                if (want < 2 && remaining >= 2) {
                    moveOn = true;
                }
            } else if (group.keepWithNext && next && !next->items.isEmpty()) {
                // A heading at the foot of a page with its text overleaf is the
                // one break no reader forgives.
                double y = cursor - gap;
                for (int i = placed; i < total; ++i) {
                    y -= group.items.at(i).height;
                }
                y -= qMax(group.spaceAfter, next->spaceBefore);
                if (y - next->items.constFirst().height < columnBottom() - 0.001) {
                    moveOn = true;
                }
            }

            if (moveOn && !atTop) {
                nextColumn();
                continue;
            }
            if (fits == 0 && notes) {
                notes->tallItem();
            }
            want = qMax(want, 1);

            double y = cursor - gap;
            pendingSpaceAfter = 0.0;
            for (int i = 0; i < want; ++i) {
                emitItem(group.items.at(placed + i), y);
                y -= group.items.at(placed + i).height;
            }
            cursor = y;
            atTop = false;
            placed += want;

            if (placed < total) {
                nextColumn();
            } else {
                pendingSpaceAfter = group.spaceAfter;
            }
        }
    }
};

// ---------------------------------------------------------------------------
//  Running heads
// ---------------------------------------------------------------------------

QString substitute(const QString &pattern, const Typeset::Document &document, int page, int pages)
{
    QString text = pattern;
    text.replace(QLatin1String("{page}"), QString::number(page));
    text.replace(QLatin1String("{pages}"), QString::number(pages));
    text.replace(QLatin1String("{title}"), document.title);
    text.replace(QLatin1String("{date}"), QLocale().toString(QDate::currentDate(), QLocale::ShortFormat));
    return text;
}

void addRunningHeads(QVector<OutPage> &pages, const Typeset::Document &document, Notes *notes)
{
    if (document.header.isEmpty() && document.footer.isEmpty()) {
        return;
    }
    const int font = core14IndexFor(familyFromName(document.body.family), false, false);
    const double size = qMax(4.0, document.headerSize);
    const double width = document.pageSize.width() - document.marginLeft - document.marginRight;

    for (int i = 0; i < pages.size(); ++i) {
        const auto draw = [&](const QString &pattern, double y) {
            QString text = sanitise(substitute(pattern, document, i + 1, int(pages.size())), notes);
            if (text.trimmed().isEmpty()) {
                return;
            }
            // A running head that overhangs the margin looks like a mistake, so
            // it is cut rather than allowed to.
            while (advanceOf(text, font, size) > width && text.size() > 1) {
                text = text.left(text.size() - 2) + QChar(0x2026);
            }
            DrawText head;
            head.text = text;
            head.font = font;
            head.size = size;
            head.colour = QColor(90, 90, 90);
            head.x = document.marginLeft + (width - advanceOf(text, font, size)) / 2.0;
            head.y = y;
            pages[i].texts.append(head);
        };

        // Outside the text area on purpose: a running head that ate into the
        // column would push the first line down on some pages and not others.
        draw(document.header, document.pageSize.height() - document.marginTop + size * 1.2);
        draw(document.footer, qMax(size * 0.3, document.marginBottom - size * 1.8));
    }
}

// ---------------------------------------------------------------------------
//  Writing the file
// ---------------------------------------------------------------------------

bool isFollowableLink(const QString &target)
{
    static const QStringList schemes = { QStringLiteral("http://"), QStringLiteral("https://"),
                                         QStringLiteral("mailto:"), QStringLiteral("ftp://") };
    for (const QString &scheme : schemes) {
        if (target.startsWith(scheme, Qt::CaseInsensitive)) {
            return true;
        }
    }
    return false;
}

/** One operator per line, which costs a few bytes and saves an afternoon of squinting. */
std::string streamFor(const OutPage &page, std::set<int> *usedFonts)
{
    std::string out;
    QColor fill;

    for (const DrawBox &box : page.boxes) {
        if (box.colour != fill) {
            fill = box.colour;
            out += number(fill.redF()) + " " + number(fill.greenF()) + " " + number(fill.blueF()) + " rg\n";
        }
        out += number(box.box.x()) + " " + number(box.box.y()) + " " + number(box.box.width()) + " "
            + number(box.box.height()) + " re f\n";
    }

    out += "BT\n";
    int font = -1;
    double size = -1.0;
    double wordSpacing = 0.0;
    fill = QColor();
    for (const DrawText &text : page.texts) {
        if (text.text.isEmpty()) {
            continue;
        }
        usedFonts->insert(text.font);
        if (text.font != font || text.size != size) {
            font = text.font;
            size = text.size;
            out += resourceName(font) + " " + number(size) + " Tf\n";
        }
        if (text.colour != fill) {
            fill = text.colour;
            out += number(fill.redF()) + " " + number(fill.greenF()) + " " + number(fill.blueF()) + " rg\n";
        }
        if (text.wordSpacing != wordSpacing) {
            wordSpacing = text.wordSpacing;
            out += number(wordSpacing) + " Tw\n";
        }
        out += "1 0 0 1 " + number(text.x) + " " + number(text.y) + " Tm\n";
        out += literalString(text.text) + " Tj\n";
    }
    if (wordSpacing != 0.0) {
        // Tw outlives ET, so anything appended to this page later would inherit
        // a stretched space.
        out += "0 Tw\n";
    }
    out += "ET\n";
    return out;
}

bool writePdf(const QVector<OutPage> &pages, const Typeset::Document &document, const QString &outPdf, QString *error)
{
    try {
        QPDF pdf;
        pdf.emptyPDF();
        QPDFPageDocumentHelper helper(pdf);
        QHash<int, QPDFObjectHandle> fonts;

        for (const OutPage &out : pages) {
            std::set<int> usedFonts;
            const std::string content = streamFor(out, &usedFonts);

            QPDFObjectHandle fontDict = QPDFObjectHandle::newDictionary();
            for (const int index : usedFonts) {
                if (!fonts.contains(index)) {
                    // No /Widths and no /FirstChar: every reader is required to
                    // know the standard fourteen, which is the whole reason to
                    // use them and why the file stays a few kilobytes.
                    const std::string dict = std::string("<< /Type /Font /Subtype /Type1 /BaseFont /")
                        + Core14::table[index].name + " /Encoding /WinAnsiEncoding >>";
                    fonts.insert(index, pdf.makeIndirectObject(QPDFObjectHandle::parse(dict)));
                }
                fontDict.replaceKey(resourceName(index), fonts.value(index));
            }

            QPDFObjectHandle resources = QPDFObjectHandle::newDictionary();
            resources.replaceKey("/Font", fontDict);

            QPDFObjectHandle page = QPDFObjectHandle::newDictionary();
            page.replaceKey("/Type", QPDFObjectHandle::newName("/Page"));
            page.replaceKey("/MediaBox",
                            QPDFObjectHandle::parse("[0 0 " + number(document.pageSize.width()) + " "
                                                    + number(document.pageSize.height()) + "]"));
            page.replaceKey("/Resources", resources);
            page.replaceKey("/Contents", QPDFObjectHandle::newStream(&pdf, content));

            QPDFObjectHandle annots = QPDFObjectHandle::newArray();
            for (const DrawText &text : out.texts) {
                if (text.link.isEmpty() || !isFollowableLink(text.link)) {
                    continue;
                }
                const double width
                    = advanceOf(text.text, text.font, text.size) + text.wordSpacing * double(spacesIn(text.text));
                QPDFObjectHandle link = QPDFObjectHandle::parse("<< /Type /Annot /Subtype /Link /Border [0 0 0] >>");
                link.replaceKey("/Rect",
                                QPDFObjectHandle::parse("[" + number(text.x) + " " + number(text.y - text.size * 0.25)
                                                        + " " + number(text.x + width) + " "
                                                        + number(text.y + text.size * 0.9) + "]"));
                QPDFObjectHandle action = QPDFObjectHandle::parse("<< /Type /Action /S /URI >>");
                action.replaceKey("/URI", QPDFObjectHandle::newString(text.link.toStdString()));
                link.replaceKey("/A", action);
                annots.appendItem(pdf.makeIndirectObject(link));
            }
            if (annots.getArrayNItems() > 0) {
                page.replaceKey("/Annots", annots);
            }

            helper.addPage(QPDFPageObjectHelper(pdf.makeIndirectObject(page)), false);
        }

        if (!document.title.isEmpty() || !document.author.isEmpty()) {
            QPDFObjectHandle info = pdf.makeIndirectObject(QPDFObjectHandle::newDictionary());
            if (!document.title.isEmpty()) {
                info.replaceKey("/Title", QPDFObjectHandle::newUnicodeString(document.title.toStdString()));
            }
            if (!document.author.isEmpty()) {
                info.replaceKey("/Author", QPDFObjectHandle::newUnicodeString(document.author.toStdString()));
            }
            info.replaceKey(
                "/CreationDate",
                QPDFObjectHandle::newString(PdfFile::formatDate(QDateTime::currentDateTime()).toStdString()));
            pdf.getTrailer().replaceKey("/Info", info);
        }

        QPDFWriter writer(pdf);
        writer.setOutputFilename(QFile::encodeName(outPdf).constData());
        writer.setObjectStreamMode(qpdf_o_generate);
        writer.setCompressStreams(true);
        writer.setDeterministicID(true);
        writer.write();
    } catch (const std::exception &e) {
        if (error) {
            *error = i18n("Cannot write “%1”: %2", QFileInfo(outPdf).fileName(), QString::fromUtf8(e.what()));
        }
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
//  Text to blocks
// ---------------------------------------------------------------------------

void pushText(QVector<Token> *tokens, const QString &text, const Token &attributes, Notes *notes)
{
    const QString clean = sanitise(text, notes);
    QString word;
    const auto flush = [&]() {
        if (word.isEmpty()) {
            return;
        }
        Token token = attributes;
        token.text = word;
        token.space = false;
        tokens->append(token);
        word.clear();
    };

    for (const QChar &character : clean) {
        // U+00A0 is deliberately not white space here: that is what "no-break"
        // means, and a date or a unit ought to survive the line break.
        if (character.isSpace() && character != QChar(0x00A0)) {
            flush();
            if (!tokens->isEmpty() && !tokens->constLast().space) {
                Token space;
                space.space = true;
                tokens->append(space);
            }
            continue;
        }
        word += character;
    }
    flush();
}

bool isPunctuation(QChar character)
{
    static const QString marks = QStringLiteral("\\`*_{}[]()#+-.!|<>~\"'");
    return marks.contains(character);
}

/** Bold, italic, code and links: the inline Markdown that means something on paper. */
QVector<Token> parseInline(const QString &source, Notes *notes)
{
    QVector<Token> tokens;
    Token state;
    QString buffer;

    const auto flush = [&]() {
        if (!buffer.isEmpty()) {
            pushText(&tokens, buffer, state, notes);
            buffer.clear();
        }
    };

    int i = 0;
    while (i < source.size()) {
        const QChar character = source.at(i);

        if (character == QLatin1Char('\\') && i + 1 < source.size() && isPunctuation(source.at(i + 1))) {
            buffer += source.at(i + 1);
            i += 2;
            continue;
        }

        if (character == QLatin1Char('`')) {
            int length = 0;
            while (i + length < source.size() && source.at(i + length) == QLatin1Char('`')) {
                ++length;
            }
            const QString fence = QString(length, QLatin1Char('`'));
            const int close = source.indexOf(fence, i + length);
            if (close > 0) {
                flush();
                Token code = state;
                code.mono = true;
                pushText(&tokens, source.mid(i + length, close - i - length), code, notes);
                i = close + length;
                continue;
            }
        }

        if (character == QLatin1Char('*') || character == QLatin1Char('_')) {
            int length = 0;
            while (i + length < source.size() && source.at(i + length) == character) {
                ++length;
            }
            // An underscore inside a word is a word, not emphasis; snake_case
            // would otherwise come out half italic.
            const bool insideWord = character == QLatin1Char('_') && i > 0 && source.at(i - 1).isLetterOrNumber()
                && i + length < source.size() && source.at(i + length).isLetterOrNumber();
            if (!insideWord && length <= 3) {
                flush();
                if (length != 2) {
                    state.italic = !state.italic;
                }
                if (length >= 2) {
                    state.bold = !state.bold;
                }
                i += length;
                continue;
            }
        }

        if (character == QLatin1Char('[')
            || (character == QLatin1Char('!') && i + 1 < source.size() && source.at(i + 1) == QLatin1Char('['))) {
            const bool image = character == QLatin1Char('!');
            const int open = image ? i + 1 : i;
            const int close = source.indexOf(QLatin1Char(']'), open);
            if (close > open && close + 1 < source.size() && source.at(close + 1) == QLatin1Char('(')) {
                const int end = source.indexOf(QLatin1Char(')'), close + 2);
                if (end > close) {
                    const QString label = source.mid(open + 1, close - open - 1);
                    const QString target = source.mid(close + 2, end - close - 2).trimmed();
                    flush();
                    if (image) {
                        // A picture cannot be fetched from here, and its size
                        // would be a guess; the words that stood for it are more
                        // use than a grey box.
                        Token alt = state;
                        alt.italic = true;
                        pushText(&tokens, label, alt, notes);
                    } else {
                        Token anchor = state;
                        anchor.link = target;
                        pushText(&tokens, label, anchor, notes);
                        if (!target.isEmpty() && target != label) {
                            Token trailer = state;
                            trailer.faint = true;
                            pushText(&tokens, QStringLiteral("(") + target + QStringLiteral(")"), trailer, notes);
                        }
                    }
                    i = end + 1;
                    continue;
                }
            }
        }

        buffer += character;
        ++i;
    }
    flush();

    while (!tokens.isEmpty() && tokens.constLast().space) {
        tokens.removeLast();
    }
    return tokens;
}

int indentOf(const QString &line)
{
    int indent = 0;
    for (const QChar &character : line) {
        if (character == QLatin1Char(' ')) {
            ++indent;
        } else if (character == QLatin1Char('\t')) {
            indent += 4;
        } else {
            break;
        }
    }
    return indent;
}

bool isBlank(const QString &line)
{
    return line.trimmed().isEmpty();
}

/** A row of pipes and dashes, which is the only thing that makes a table a table. */
bool isTableDelimiter(const QString &line)
{
    const QString trimmed = line.trimmed();
    if (!trimmed.contains(QLatin1Char('-')) || !trimmed.contains(QLatin1Char('|'))) {
        return false;
    }
    for (const QChar &character : trimmed) {
        if (character != QLatin1Char('-') && character != QLatin1Char('|') && character != QLatin1Char(':')
            && character != QLatin1Char(' ')) {
            return false;
        }
    }
    return true;
}

QStringList splitTableRow(const QString &line)
{
    QString trimmed = line.trimmed();
    if (trimmed.startsWith(QLatin1Char('|'))) {
        trimmed = trimmed.mid(1);
    }
    if (trimmed.endsWith(QLatin1Char('|')) && !trimmed.endsWith(QLatin1String("\\|"))) {
        trimmed.chop(1);
    }
    QStringList cells;
    QString cell;
    for (int i = 0; i < trimmed.size(); ++i) {
        if (trimmed.at(i) == QLatin1Char('\\') && i + 1 < trimmed.size() && trimmed.at(i + 1) == QLatin1Char('|')) {
            cell += QLatin1Char('|');
            ++i;
            continue;
        }
        if (trimmed.at(i) == QLatin1Char('|')) {
            cells.append(cell.trimmed());
            cell.clear();
            continue;
        }
        cell += trimmed.at(i);
    }
    cells.append(cell.trimmed());
    return cells;
}

QStringList splitLines(const QString &text)
{
    QString normalised = text;
    normalised.replace(QLatin1String("\r\n"), QLatin1String("\n"));
    normalised.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    return normalised.split(QLatin1Char('\n'));
}

QVector<Block> blocksFromPlainText(const QString &text, const Typeset::Document &document, Notes *notes)
{
    QVector<Block> blocks;
    QStringList paragraph;

    const auto flush = [&]() {
        if (paragraph.isEmpty()) {
            return;
        }
        Block block;
        block.style = document.body;
        const Token plain;
        pushText(&block.tokens, paragraph.join(QLatin1Char(' ')), plain, notes);
        paragraph.clear();
        if (!block.tokens.isEmpty()) {
            blocks.append(block);
        }
    };

    for (const QString &line : splitLines(text)) {
        if (isBlank(line)) {
            flush();
            continue;
        }
        paragraph.append(line.trimmed());
    }
    flush();
    return blocks;
}

/** Markdown, line by line, with the constructs that survive being printed. */
QVector<Block> blocksFromMarkdown(const QString &markdown, const Typeset::Document &document, Notes *notes)
{
    static const QRegularExpression headingLine(QStringLiteral("^\\s{0,3}(#{1,6})\\s+(.*?)\\s*#*\\s*$"));
    static const QRegularExpression ruleLine(QStringLiteral("^\\s{0,3}([-*_])\\s*(\\1\\s*){2,}$"));
    static const QRegularExpression bulletLine(QStringLiteral("^(\\s*)([-*+])\\s+(.*)$"));
    static const QRegularExpression orderedLine(QStringLiteral("^(\\s*)(\\d{1,9})[.)]\\s+(.*)$"));
    static const QRegularExpression quoteLine(QStringLiteral("^\\s{0,3}>\\s?(.*)$"));
    static const QRegularExpression fenceLine(QStringLiteral("^\\s{0,3}(```+|~~~+)\\s*(\\S*)\\s*$"));
    static const QRegularExpression setextOne(QStringLiteral("^\\s{0,3}=+\\s*$"));
    static const QRegularExpression setextTwo(QStringLiteral("^\\s{0,3}-+\\s*$"));

    const QStringList lines = splitLines(markdown);
    QVector<Block> blocks;
    QStringList paragraph;

    QVector<int> listIndents;
    QVector<int> listCounters;
    const double indentStep = document.body.fontSize * 1.6;

    const auto flushParagraph = [&]() {
        if (paragraph.isEmpty()) {
            return;
        }
        Block block;
        block.style = document.body;
        block.tokens = parseInline(paragraph.join(QLatin1Char(' ')), notes);
        paragraph.clear();
        if (!block.tokens.isEmpty()) {
            blocks.append(block);
        }
    };

    const auto endLists = [&]() {
        listIndents.clear();
        listCounters.clear();
    };

    const auto appendHeading = [&](int level, const QString &text) {
        Block block;
        block.kind = BlockKind::Heading;
        block.style = headingStyle(document, level);
        block.tokens = parseInline(text, notes);
        if (!block.tokens.isEmpty()) {
            blocks.append(block);
        }
    };

    for (int i = 0; i < lines.size(); ++i) {
        const QString &line = lines.at(i);

        const QRegularExpressionMatch fence = fenceLine.match(line);
        if (fence.hasMatch()) {
            flushParagraph();
            endLists();
            const QString marker = fence.captured(1);
            Block block;
            block.kind = BlockKind::Code;
            block.style = codeStyle(document);
            ++i;
            while (i < lines.size() && !lines.at(i).trimmed().startsWith(marker)) {
                block.verbatim.append(lines.at(i));
                ++i;
            }
            if (!block.verbatim.isEmpty()) {
                blocks.append(block);
            }
            continue;
        }

        if (isBlank(line)) {
            flushParagraph();
            continue;
        }

        if (paragraph.isEmpty() && ruleLine.match(line).hasMatch()) {
            endLists();
            Block block;
            block.kind = BlockKind::Rule;
            block.style = document.body;
            blocks.append(block);
            continue;
        }

        const QRegularExpressionMatch heading = headingLine.match(line);
        if (heading.hasMatch()) {
            flushParagraph();
            endLists();
            appendHeading(int(heading.captured(1).size()), heading.captured(2));
            continue;
        }

        if (!paragraph.isEmpty() && setextOne.match(line).hasMatch()) {
            const QString text = paragraph.join(QLatin1Char(' '));
            paragraph.clear();
            appendHeading(1, text);
            continue;
        }
        if (!paragraph.isEmpty() && setextTwo.match(line).hasMatch()) {
            const QString text = paragraph.join(QLatin1Char(' '));
            paragraph.clear();
            appendHeading(2, text);
            continue;
        }

        if (line.contains(QLatin1Char('|')) && i + 1 < lines.size() && isTableDelimiter(lines.at(i + 1))) {
            flushParagraph();
            endLists();
            Block block;
            block.kind = BlockKind::Table;
            block.style = document.body;

            for (const QString &delimiter : splitTableRow(lines.at(i + 1))) {
                if (delimiter.startsWith(QLatin1Char(':')) && delimiter.endsWith(QLatin1Char(':'))) {
                    block.columnAlignments.append(Qt::AlignHCenter);
                } else if (delimiter.endsWith(QLatin1Char(':'))) {
                    block.columnAlignments.append(Qt::AlignRight);
                } else {
                    block.columnAlignments.append(Qt::AlignLeft);
                }
            }

            const auto rowOf = [&](const QStringList &cells, bool bold) {
                QVector<QVector<Token>> row;
                for (const QString &cell : cells) {
                    QVector<Token> tokens = parseInline(cell, notes);
                    if (bold) {
                        for (Token &token : tokens) {
                            token.bold = true;
                        }
                    }
                    row.append(tokens);
                }
                return row;
            };

            block.rows.append(rowOf(splitTableRow(line), true));
            i += 2;
            while (i < lines.size() && !isBlank(lines.at(i)) && lines.at(i).contains(QLatin1Char('|'))) {
                block.rows.append(rowOf(splitTableRow(lines.at(i)), false));
                ++i;
            }
            --i;
            blocks.append(block);
            continue;
        }

        const QRegularExpressionMatch quote = quoteLine.match(line);
        if (quote.hasMatch()) {
            flushParagraph();
            endLists();
            QStringList gathered;
            int j = i;
            while (j < lines.size()) {
                const QRegularExpressionMatch inner = quoteLine.match(lines.at(j));
                if (inner.hasMatch()) {
                    gathered.append(inner.captured(1));
                } else if (!isBlank(lines.at(j)) && !gathered.isEmpty() && !gathered.constLast().isEmpty()) {
                    gathered.append(lines.at(j).trimmed()); // lazy continuation
                } else {
                    break;
                }
                ++j;
            }
            i = j - 1;

            QStringList current;
            const auto flushQuote = [&]() {
                if (current.isEmpty()) {
                    return;
                }
                Block block;
                block.kind = BlockKind::Quote;
                block.style = quoteStyle(document);
                block.tokens = parseInline(current.join(QLatin1Char(' ')), notes);
                current.clear();
                if (!block.tokens.isEmpty()) {
                    blocks.append(block);
                }
            };
            for (const QString &part : gathered) {
                if (part.trimmed().isEmpty()) {
                    flushQuote();
                } else {
                    current.append(part.trimmed());
                }
            }
            flushQuote();
            continue;
        }

        const QRegularExpressionMatch bullet = bulletLine.match(line);
        const QRegularExpressionMatch ordered = orderedLine.match(line);
        if (bullet.hasMatch() || ordered.hasMatch()) {
            flushParagraph();
            const bool isOrdered = !bullet.hasMatch();
            const QRegularExpressionMatch &match = isOrdered ? ordered : bullet;
            const int indent = int(match.captured(1).size());
            QString text = match.captured(3);

            // A stack of indents rather than a division, so that a list written
            // with two spaces per level nests the same as one written with four.
            while (!listIndents.isEmpty() && indent < listIndents.constLast() - 1) {
                listIndents.removeLast();
                listCounters.removeLast();
            }
            if (listIndents.isEmpty() || indent > listIndents.constLast() + 1) {
                listIndents.append(indent);
                listCounters.append(isOrdered ? match.captured(2).toInt() : 1);
            }
            const int depth = qMin(int(listIndents.size()) - 1, 5);

            // Continuation lines of the same item, indented or merely lazy.
            int j = i + 1;
            while (j < lines.size() && !isBlank(lines.at(j)) && !bulletLine.match(lines.at(j)).hasMatch()
                   && !orderedLine.match(lines.at(j)).hasMatch() && !headingLine.match(lines.at(j)).hasMatch()
                   && !quoteLine.match(lines.at(j)).hasMatch() && !fenceLine.match(lines.at(j)).hasMatch()
                   && !ruleLine.match(lines.at(j)).hasMatch()) {
                text += QLatin1Char(' ') + lines.at(j).trimmed();
                ++j;
            }
            i = j - 1;

            Block block;
            block.kind = BlockKind::ListItem;
            block.style = document.body;
            block.style.indentFirst = 0.0;
            block.style.spaceBefore = document.body.fontSize * 0.15;
            block.style.spaceAfter = document.body.fontSize * 0.15;
            block.indent = double(depth) * indentStep;
            block.orderedMarker = isOrdered;
            if (isOrdered) {
                block.marker = QString::number(listCounters.constLast()) + QLatin1Char('.');
                listCounters[listCounters.size() - 1] += 1;
            } else {
                // Three levels of bullet, all of them inside WinAnsi.
                static const char16_t bullets[] = { 0x2022, 0x2013, 0x00B7 };
                block.marker = QString(QChar(bullets[depth % 3]));
            }
            block.tokens = parseInline(text, notes);
            if (!block.tokens.isEmpty()) {
                blocks.append(block);
            }
            continue;
        }

        if (paragraph.isEmpty() && listIndents.isEmpty() && indentOf(line) >= 4) {
            Block block;
            block.kind = BlockKind::Code;
            block.style = codeStyle(document);
            int j = i;
            while (j < lines.size() && (isBlank(lines.at(j)) || indentOf(lines.at(j)) >= 4)) {
                block.verbatim.append(isBlank(lines.at(j)) ? QString() : lines.at(j).mid(4));
                ++j;
            }
            while (!block.verbatim.isEmpty() && block.verbatim.constLast().isEmpty()) {
                block.verbatim.removeLast();
            }
            i = j - 1;
            if (!block.verbatim.isEmpty()) {
                blocks.append(block);
            }
            continue;
        }

        if (paragraph.isEmpty()) {
            endLists();
        }
        paragraph.append(line.trimmed());
    }
    flushParagraph();
    return blocks;
}

// ---------------------------------------------------------------------------
//  Putting it together
// ---------------------------------------------------------------------------

bool render(const QVector<Block> &blocks, const QString &outPdf, const Typeset::Document &document, Notes *notes,
            QString *error)
{
    if (outPdf.isEmpty()) {
        if (error) {
            *error = i18n("No file to write to was given.");
        }
        return false;
    }
    if (document.body.fontSize <= 0.0) {
        if (error) {
            *error = i18n("The type size has to be greater than zero.");
        }
        return false;
    }
    if (document.pageSize.width() < 72.0 || document.pageSize.height() < 72.0) {
        if (error) {
            *error = i18n("A page smaller than an inch square cannot hold text.");
        }
        return false;
    }

    const int columnCount = qBound(1, document.columns, 12);
    const double usable = document.pageSize.width() - document.marginLeft - document.marginRight;
    const double columnWidth = (usable - double(columnCount - 1) * document.columnGap) / double(columnCount);
    const double columnHeight = document.pageSize.height() - document.marginTop - document.marginBottom;

    if (columnWidth < document.body.fontSize * 3.0 || columnHeight < document.body.fontSize * 2.0) {
        if (error) {
            *error = i18n("The margins and columns leave no room for text on a page this size.");
        }
        return false;
    }

    QVector<Group> groups;
    groups.reserve(blocks.size());
    for (const Block &block : blocks) {
        Group group;
        switch (block.kind) {
        case BlockKind::Code:
            group = layoutCode(block, columnWidth, notes);
            break;
        case BlockKind::Rule:
            group = layoutRule(block, columnWidth);
            break;
        case BlockKind::Table:
            group = layoutTable(block, columnWidth, document, notes);
            break;
        default:
            group = layoutParagraph(block, columnWidth, document, notes);
            break;
        }
        if (group.items.isEmpty()) {
            continue;
        }
        if (block.kind != BlockKind::Rule && notes) {
            ++notes->report.paragraphs;
        }
        groups.append(group);
    }

    if (groups.isEmpty()) {
        if (error) {
            *error = i18n("There is no text to lay out.");
        }
        return false;
    }

    Flow flow;
    flow.document = &document;
    flow.notes = notes;
    flow.columnWidth = columnWidth;
    flow.columnCount = columnCount;
    for (int i = 0; i < groups.size(); ++i) {
        flow.place(groups.at(i), i + 1 < groups.size() ? &groups.at(i + 1) : nullptr);
    }

    addRunningHeads(flow.pages, document, notes);
    if (notes) {
        notes->report.pages = int(flow.pages.size());
    }
    return writePdf(flow.pages, document, outPdf, error);
}

} // namespace

bool Typeset::fromPlainText(const QString &text, const QString &outPdf, const Document &document, Report *report,
                            QString *error)
{
    Notes notes;
    const bool ok = render(blocksFromPlainText(text, document, &notes), outPdf, document, &notes, error);
    if (report) {
        *report = notes.report;
    }
    return ok;
}

bool Typeset::fromMarkdown(const QString &markdown, const QString &outPdf, const Document &document, Report *report,
                           QString *error)
{
    Notes notes;
    const bool ok = render(blocksFromMarkdown(markdown, document, &notes), outPdf, document, &notes, error);
    if (report) {
        *report = notes.report;
    }
    return ok;
}

bool Typeset::fromTextFile(const QString &path, const QString &outPdf, const Document &document, Report *report,
                           QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) {
            *error = i18n("Cannot read “%1”.", QFileInfo(path).fileName());
        }
        return false;
    }
    const QByteArray bytes = file.readAll();
    file.close();

    // Text files from elsewhere are not always UTF-8, and a page of mojibake is
    // a worse answer than an eight-bit reading of eight-bit bytes.
    QStringDecoder decoder(QStringConverter::Utf8);
    QString text = decoder(bytes);
    if (decoder.hasError()) {
        text = QString::fromLatin1(bytes);
    }

    static const QStringList markdownSuffixes
        = { QStringLiteral("md"), QStringLiteral("markdown"), QStringLiteral("mdown"), QStringLiteral("mkd") };
    if (markdownSuffixes.contains(QFileInfo(path).suffix().toLower())) {
        return fromMarkdown(text, outPdf, document, report, error);
    }
    return fromPlainText(text, outPdf, document, report, error);
}

double Typeset::textWidth(const QString &text, const QString &family, bool bold, bool italic, double fontSize)
{
    return advanceOf(text, core14IndexFor(familyFromName(family), bold, italic), fontSize);
}

QStringList Typeset::limitations()
{
    return {
        i18n("Only the fourteen standard fonts are used, so the alphabet is the one WinAnsi covers: "
             "western Europe. Greek, Cyrillic, Hebrew, Arabic and CJK need an embedded font, and are "
             "reported rather than drawn."),
        i18n("Lines are broken one at a time from left to right rather than by balancing a whole "
             "paragraph at once, so the spacing is even but not the best possible."),
        i18n("Hyphenation follows a spelling pattern rather than a dictionary, and stays off unless "
             "it is asked for."),
        i18n("Pictures are not drawn. A Markdown image contributes its description in italics, "
             "because a caption is more use than an empty frame."),
        i18n("Tables are a plain ruled grid: no merged cells, no heading repeated on the following "
             "page, and a row is never split across pages."),
        i18n("Footnotes, cross-references, an index and a table of contents are not built; Markdown "
             "that describes them is set as ordinary text."),
        i18n("Headers and footers are centred and hold one line, so the page number sits in the "
             "middle rather than at the outer edge."),
        i18n("Text runs from top to bottom and left to right; right-to-left scripts are not laid "
             "out."),
    };
}

} // namespace ps
