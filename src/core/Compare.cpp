/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#include "Compare.h"

#include "PdfFile.h"
#include "RenderBackend.h"

#include <KLocalizedString>

#include <QPainter>
#include <QSizeF>

#include <qpdf/QPDF.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>

#include <algorithm>
#include <cmath>

namespace ps {

namespace {

// Well clear of the ids a document hands out for its own sources, and of the
// scratch id the page processor renders through.
constexpr int leftSourceId = 900'001;
constexpr int rightSourceId = 900'002;

/**
 * The most words per page the subsequence match will look at.
 *
 * The table is quadratic in the word count, so a cap is what keeps a
 * machine-generated page of a hundred thousand words from asking for gigabytes.
 * Words beyond it are still compared, but position by position rather than as a
 * subsequence: an edit out there is still reported, only with more words listed
 * as removed and added than a person would have written down, because a single
 * inserted word shifts everything after it. A typeset page holds well under a
 * thousand words, so this is a ceiling rather than a limit.
 */
constexpr qsizetype maxSubsequenceWords = 2000;

/**
 * Fraction of the page below which pixel differences are not called a change.
 *
 * Not there for renderer noise: the same content rendered twice is identical to
 * the pixel. It is there for the aftermath of a rewrite: a writer that emits
 * "612" where the original had "612.00000" can shift a hairline or a glyph edge
 * by one pixel, and a document is not "changed" because two of its edges moved.
 * An altered image or a substituted font touches thousands of pixels and clears
 * this by orders of magnitude.
 */
constexpr double visualFloor = 0.0002;

/** Page count according to QPDF, or -1 with @p error set. */
int pageCountOf(const QString &path, QString *error)
{
    try {
        QPDF pdf;
        PdfFile::open(pdf, path);
        return int(QPDFPageDocumentHelper(pdf).getAllPages().size());
    } catch (const std::exception &e) {
        if (error) {
            *error = QString::fromUtf8(e.what());
        }
        return -1;
    }
}

/**
 * The page's words, one per entry.
 *
 * With whitespace ignored these are the bare words. With whitespace significant
 * each word keeps the run of spaces or newlines that follows it, so that a word
 * which merely swapped a space for a line break no longer matches its twin. The
 * separator is trimmed off again before anything is reported: "Alpha\n" is not
 * a word anybody wants to find in a list of changes.
 */
QStringList wordsOf(const QString &text, bool ignoreWhitespace)
{
    QStringList words;
    qsizetype at = 0;

    while (at < text.size()) {
        while (at < text.size() && text.at(at).isSpace()) {
            ++at;
        }
        const qsizetype start = at;
        while (at < text.size() && !text.at(at).isSpace()) {
            ++at;
        }
        if (at == start) {
            break;
        }

        qsizetype end = at;
        if (!ignoreWhitespace) {
            while (end < text.size() && text.at(end).isSpace()) {
                ++end;
            }
        }
        words.append(text.mid(start, end - start));
        at = end;
    }

    return words;
}

struct WordDiff {
    QStringList removed;
    QStringList added;

    bool changed() const { return !removed.isEmpty() || !added.isEmpty(); }
};

/** The words of @p left that @p right does not have, and the other way round. */
WordDiff diffWords(const QStringList &left, const QStringList &right)
{
    const qsizetype leftHead = std::min<qsizetype>(left.size(), maxSubsequenceWords);
    const qsizetype rightHead = std::min<qsizetype>(right.size(), maxSubsequenceWords);
    const qsizetype stride = rightHead + 1;

    // table[i][j] is the length of the longest common subsequence of the first
    // i left words and the first j right ones. A length can never exceed the
    // cap, so sixteen bits per cell is plenty and halves what the table costs.
    QVector<quint16> table(size_t((leftHead + 1) * stride), 0);
    const auto cell = [stride](qsizetype i, qsizetype j) { return i * stride + j; };

    for (qsizetype i = 1; i <= leftHead; ++i) {
        for (qsizetype j = 1; j <= rightHead; ++j) {
            table[cell(i, j)] = left.at(i - 1) == right.at(j - 1)
                ? quint16(table.at(cell(i - 1, j - 1)) + 1)
                : std::max(table.at(cell(i - 1, j)), table.at(cell(i, j - 1)));
        }
    }

    WordDiff diff;
    qsizetype i = leftHead;
    qsizetype j = rightHead;
    while (i > 0 || j > 0) {
        if (i > 0 && j > 0 && left.at(i - 1) == right.at(j - 1)) {
            --i;
            --j;
        } else if (j > 0 && (i == 0 || table.at(cell(i, j - 1)) >= table.at(cell(i - 1, j)))) {
            diff.added.append(right.at(j - 1).trimmed());
            --j;
        } else {
            diff.removed.append(left.at(i - 1).trimmed());
            --i;
        }
    }

    // The table is walked from the end, so both lists come out backwards.
    std::reverse(diff.removed.begin(), diff.removed.end());
    std::reverse(diff.added.begin(), diff.added.end());

    // Whatever sits past the cap, lined up where it stands.
    for (qsizetype offset = 0; leftHead + offset < left.size() || rightHead + offset < right.size(); ++offset) {
        const QString here = leftHead + offset < left.size() ? left.at(leftHead + offset) : QString();
        const QString there = rightHead + offset < right.size() ? right.at(rightHead + offset) : QString();
        if (here == there) {
            continue;
        }
        if (!here.isEmpty()) {
            diff.removed.append(here.trimmed());
        }
        if (!there.isEmpty()) {
            diff.added.append(there.trimmed());
        }
    }

    return diff;
}

struct PixelComparison {
    double fraction = 0.0;

    /** Grey, 255 where the two differ. Null unless it was asked for. */
    QImage mask;
};

/**
 * Grey-value comparison across the union of the two images.
 *
 * Where one image is shorter or narrower than the other the missing part counts
 * as blank paper rather than being skipped, which is what makes the two cases
 * come out differently: a page that grew a footer differs down there, while a
 * page that merely grew taller with nothing on the new strip does not.
 */
PixelComparison comparePixels(const QImage &left, const QImage &right, int tolerance, bool wantMask)
{
    PixelComparison result;

    const QImage leftGrey = left.isNull() ? QImage() : left.convertToFormat(QImage::Format_Grayscale8);
    const QImage rightGrey = right.isNull() ? QImage() : right.convertToFormat(QImage::Format_Grayscale8);

    const int width = std::max(leftGrey.width(), rightGrey.width());
    const int height = std::max(leftGrey.height(), rightGrey.height());
    if (width <= 0 || height <= 0) {
        return result;
    }

    if (wantMask) {
        result.mask = QImage(width, height, QImage::Format_Grayscale8);
        if (result.mask.isNull()) {
            return result;
        }
        result.mask.fill(0);
    }

    // A tolerance of zero has to mean "exactly equal", not "everything differs",
    // so the comparison is against at least one grey level.
    const int limit = std::max(1, tolerance);
    constexpr int paper = 255;

    qint64 differing = 0;
    for (int y = 0; y < height; ++y) {
        const uchar *leftLine = y < leftGrey.height() ? leftGrey.constScanLine(y) : nullptr;
        const uchar *rightLine = y < rightGrey.height() ? rightGrey.constScanLine(y) : nullptr;
        uchar *maskLine = wantMask ? result.mask.scanLine(y) : nullptr;

        for (int x = 0; x < width; ++x) {
            const int here = leftLine && x < leftGrey.width() ? leftLine[x] : paper;
            const int there = rightLine && x < rightGrey.width() ? rightLine[x] : paper;
            if (std::abs(here - there) >= limit) {
                ++differing;
                if (maskLine) {
                    maskLine[x] = 255;
                }
            }
        }
    }

    result.fraction = double(differing) / (double(width) * double(height));
    return result;
}

/** Pixels across for a page at @p dpi, or 0 when the page cannot be measured. */
int renderWidthFor(RenderBackend *backend, int sourceId, int page, double dpi)
{
    const QSizeF size = backend->pageSizePoints(sourceId, page);
    if (size.width() <= 0.0) {
        return 0;
    }
    return std::max(1, qRound(size.width() * dpi / 72.0));
}

/** Absurd resolutions are somebody's typo, not a request. */
double sensibleDpi(double dpi)
{
    return qBound(10.0, dpi, 600.0);
}

} // namespace

Compare::Report Compare::run(RenderBackend *backend, const QString &leftPdf, const QString &rightPdf,
                             const Options &options, QString *error)
{
    Report report;

    if (!backend) {
        if (error) {
            *error = i18n("Comparing documents needs a renderer.");
        }
        return report;
    }

    // Page counts come from QPDF rather than from the renderer, which has no
    // notion of how many pages a document has, and asking it to render one
    // past the end to find out would be a guess dressed up as a measurement.
    const int leftCount = pageCountOf(leftPdf, error);
    if (leftCount < 0) {
        return report;
    }
    const int rightCount = pageCountOf(rightPdf, error);
    if (rightCount < 0) {
        return report;
    }
    if (leftCount == 0 || rightCount == 0) {
        if (error) {
            *error = i18n("A document with no pages cannot be compared.");
        }
        return report;
    }

    if (!backend->addDocument(leftSourceId, leftPdf, error)) {
        return report;
    }
    if (!backend->addDocument(rightSourceId, rightPdf, error)) {
        backend->removeDocument(leftSourceId);
        return report;
    }

    const double dpi = sensibleDpi(options.dpi);
    const int tolerance = qBound(0, options.pixelTolerance, 255);

    for (int index = 0; index < std::max(leftCount, rightCount); ++index) {
        PageResult page;
        page.leftPage = index < leftCount ? index : -1;
        page.rightPage = index < rightCount ? index : -1;

        if (page.leftPage < 0 || page.rightPage < 0) {
            const bool onlyOnTheRight = page.leftPage < 0;
            const int sourceId = onlyOnTheRight ? rightSourceId : leftSourceId;
            const int number = onlyOnTheRight ? page.rightPage : page.leftPage;

            page.change = onlyOnTheRight ? Change::Added : Change::Removed;

            // Measured against blank paper, so the report can tell a chapter
            // appended to a document from a blank sheet appended to it.
            const QImage only = backend->renderPage(sourceId, number, renderWidthFor(backend, sourceId, number, dpi));
            page.pixelDifference = comparePixels(only, QImage(), tolerance, false).fraction;

            const QStringList words = wordsOf(backend->extractText(sourceId, number), options.ignoreWhitespace);
            const WordDiff diff = onlyOnTheRight ? diffWords({}, words) : diffWords(words, {});
            page.removedWords = diff.removed;
            page.addedWords = diff.added;

            ++report.changedPages;
            report.pages.append(page);
            continue;
        }

        const WordDiff diff = diffWords(wordsOf(backend->extractText(leftSourceId, index), options.ignoreWhitespace),
                                        wordsOf(backend->extractText(rightSourceId, index), options.ignoreWhitespace));
        page.removedWords = diff.removed;
        page.addedWords = diff.added;

        // The left document is the "before", so it sets the scale both pages are
        // drawn at; that way an unchanged pair lines up pixel for pixel instead
        // of being resampled twice and differing everywhere.
        int width = renderWidthFor(backend, leftSourceId, index, dpi);
        if (width <= 0) {
            width = renderWidthFor(backend, rightSourceId, index, dpi);
        }
        page.pixelDifference = comparePixels(backend->renderPage(leftSourceId, index, width),
                                             backend->renderPage(rightSourceId, index, width), tolerance, false)
                                   .fraction;

        const QSizeF leftSize = backend->pageSizePoints(leftSourceId, index);
        const QSizeF rightSize = backend->pageSizePoints(rightSourceId, index);
        // Both pages arrive the same number of pixels wide, which is what makes
        // them comparable and also what would hide a page moved from A4 to A3:
        // the same content at a different scale renders identically. A point of
        // slack absorbs the rounding that writers do to media boxes.
        const bool resized = std::abs(leftSize.width() - rightSize.width()) > 1.0
            || std::abs(leftSize.height() - rightSize.height()) > 1.0;

        if (diff.changed()) {
            page.change = Change::TextChanged;
        } else if (resized || page.pixelDifference >= visualFloor) {
            page.change = Change::VisualOnly;
        } else {
            page.change = Change::Same;
        }

        if (page.change != Change::Same) {
            ++report.changedPages;
        }
        report.pages.append(page);
    }

    backend->removeDocument(leftSourceId);
    backend->removeDocument(rightSourceId);

    report.identical = report.changedPages == 0 && leftCount == rightCount;
    return report;
}

QImage Compare::visualise(RenderBackend *backend, const QString &leftPdf, const QString &rightPdf, int leftPage,
                          int rightPage, const Options &options)
{
    if (!backend || (leftPage < 0 && rightPage < 0)) {
        return {};
    }

    const bool wantLeft = leftPage >= 0;
    const bool wantRight = rightPage >= 0;

    if (wantLeft && !backend->addDocument(leftSourceId, leftPdf, nullptr)) {
        return {};
    }
    if (wantRight && !backend->addDocument(rightSourceId, rightPdf, nullptr)) {
        if (wantLeft) {
            backend->removeDocument(leftSourceId);
        }
        return {};
    }

    const double dpi = sensibleDpi(options.dpi);
    const int tolerance = qBound(0, options.pixelTolerance, 255);

    int width = wantLeft ? renderWidthFor(backend, leftSourceId, leftPage, dpi) : 0;
    if (width <= 0 && wantRight) {
        width = renderWidthFor(backend, rightSourceId, rightPage, dpi);
    }

    QImage leftImage = wantLeft && width > 0 ? backend->renderPage(leftSourceId, leftPage, width) : QImage();
    QImage rightImage = wantRight && width > 0 ? backend->renderPage(rightSourceId, rightPage, width) : QImage();

    QImage result;
    // A page that was asked for and did not arrive is a failure, not a blank
    // sheet: drawing it empty would say the page is blank, which is a lie.
    const bool renderable = (!wantLeft || !leftImage.isNull()) && (!wantRight || !rightImage.isNull());

    if (renderable) {
        const QSize span(std::max(leftImage.width(), rightImage.width()),
                         std::max(leftImage.height(), rightImage.height()));
        if (leftImage.isNull()) {
            leftImage = QImage(span, QImage::Format_RGB32);
            leftImage.fill(Qt::white);
        }
        if (rightImage.isNull()) {
            rightImage = QImage(span, QImage::Format_RGB32);
            rightImage.fill(Qt::white);
        }

        const PixelComparison comparison = comparePixels(leftImage, rightImage, tolerance, true);

        constexpr int gap = 14;
        result = QImage(leftImage.width() + gap + rightImage.width(), std::max(leftImage.height(), rightImage.height()),
                        QImage::Format_RGB32);
        if (!result.isNull()) {
            // Grey behind the pages rather than white, so that the edge of a
            // page and the edge of the picture are two different things.
            result.fill(QColor(210, 210, 210));

            QPainter painter(&result);
            painter.drawImage(0, 0, leftImage);
            const int rightX = leftImage.width() + gap;
            painter.drawImage(rightX, 0, rightImage);

            // Marked in small blocks rather than pixel by pixel: a comma that
            // changed is three dark pixels, and three red pixels on a
            // page-sized picture is not something anybody spots. Marking stops
            // at the right page's own edge, because a difference below a shorter right
            // page has nothing there to tint.
            constexpr int block = 6;
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(220, 30, 30, 90));

            for (int y = 0; y < rightImage.height(); y += block) {
                for (int x = 0; x < rightImage.width(); x += block) {
                    bool differs = false;
                    for (int row = y; row < std::min(y + block, comparison.mask.height()) && !differs; ++row) {
                        const uchar *line = comparison.mask.constScanLine(row);
                        for (int column = x; column < std::min(x + block, comparison.mask.width()); ++column) {
                            if (line[column] != 0) {
                                differs = true;
                                break;
                            }
                        }
                    }
                    if (differs) {
                        painter.drawRect(QRect(rightX + x, y, std::min(block, rightImage.width() - x),
                                               std::min(block, rightImage.height() - y)));
                    }
                }
            }
        }
    }

    if (wantLeft) {
        backend->removeDocument(leftSourceId);
    }
    if (wantRight) {
        backend->removeDocument(rightSourceId);
    }
    return result;
}

QString Compare::describe(Change change)
{
    switch (change) {
    case Change::Same:
        return i18nc("@item document comparison result", "Unchanged");
    case Change::TextChanged:
        return i18nc("@item document comparison result", "Text changed");
    case Change::VisualOnly:
        return i18nc("@item document comparison result", "Same text, different appearance");
    case Change::Added:
        return i18nc("@item document comparison result", "Added");
    case Change::Removed:
        return i18nc("@item document comparison result", "Removed");
    }
    return i18nc("@item document comparison result", "Unchanged");
}

} // namespace ps
