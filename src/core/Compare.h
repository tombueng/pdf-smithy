/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#pragma once

#include <QImage>
#include <QString>
#include <QStringList>
#include <QVector>

namespace ps {

class RenderBackend;

/**
 * What changed between two versions of a document.
 *
 * The question behind this is nearly always "is this still the contract I
 * read?", and an honest answer needs two witnesses. Extracted text finds the
 * changes that matter to a reader (a name, a figure, a negation) and can say
 * which words went and which arrived. Rendered pixels find everything text
 * cannot see: a swapped signature image, a redrawn table, a font substitution
 * that reflowed a page without altering a single word.
 *
 * Neither witness alone is enough. A document whose text matches word for word
 * can still be a different document on paper, and that is exactly the case a
 * reader is least likely to catch unaided, so it gets a verdict of its own:
 * Change::VisualOnly.
 *
 * ## How the words are compared
 *
 * By longest common subsequence over the word lists, never by set difference.
 * The distinction is not academic: "the tenant shall not sublet the property"
 * and "the tenant shall sublet the property not" hold precisely the same words,
 * and any comparison built on sets, sorted lists or word counts declares them
 * equal. A subsequence keeps the words in order, so a reordering is reported as
 * the change it is.
 *
 * ## Where it errs
 *
 * Pages are paired by position. That is right for the ordinary case (one
 * document saved twice) and wrong for a page inserted in the middle, which
 * shifts everything after it and reports the whole tail as changed. Matching
 * pages to each other first would be a larger claim than this makes, and a
 * wrong pairing is a worse answer than an obviously conservative one.
 *
 * Two pages are rendered to the same pixel width so that they can be compared
 * at all, which by itself would hide the same content moved onto different
 * paper. Page size is therefore checked separately, and a page that changed
 * paper counts as a visual change even where the pixels line up exactly.
 *
 * A scan carries no text, so two scans are judged on their pixels alone. That
 * is a property of the documents rather than a defect here: nothing can tell
 * whether a picture of a sentence says something different without reading it.
 */
class Compare
{
public:
    struct Options {
        /**
         * Spacing and line breaks do not count as changes.
         *
         * On by default, because they usually are not changes: re-saving a
         * document through another writer moves line breaks about without
         * anybody editing a word. Switched off, every word is compared together
         * with the whitespace that follows it, so re-wrapping shows up, and
         * the words reported then are the ones whose spacing moved, which is
         * why the same word can appear on both sides of the report.
         */
        bool ignoreWhitespace = true;

        /** Grey values differing by less than this count as equal (0..255). */
        int pixelTolerance = 12;

        /**
         * Resolution the pages are compared at.
         *
         * A hundred is enough to see a changed image or a substituted font
         * while staying quick over a long document; the words are compared from
         * the text itself, so nothing legible depends on this.
         */
        double dpi = 100.0;
    };

    enum class Change {
        Same,
        TextChanged,
        VisualOnly, //!< The words match, the page does not: an image, or a font
        Added,
        Removed,
    };

    struct PageResult {
        int leftPage = -1; //!< -1 when the page exists only on the right
        int rightPage = -1; //!< -1 when the page exists only on the left
        Change change = Change::Same;
        double pixelDifference = 0.0; //!< fraction 0..1
        QStringList removedWords;
        QStringList addedWords;
    };

    struct Report {
        QVector<PageResult> pages;
        int changedPages = 0;
        bool identical = false;
    };

    /**
     * Compares the two documents page by page.
     *
     * Both files are put on @p backend for the duration and taken off it again
     * before this returns, whichever way the comparison goes. An empty report
     * with @p error set means no comparison happened at all; Report::identical
     * is false there because nothing was established, not because something
     * differed.
     */
    static Report run(RenderBackend *backend, const QString &leftPdf, const QString &rightPdf, const Options &options,
                      QString *error);

    /**
     * Left and right side by side with the differences marked. Null on failure.
     *
     * Either index may be -1, for a page that only one side has; the missing
     * side is drawn as blank paper, which marks everything on the page that
     * does exist: an added page is entirely new, and saying so is the point.
     */
    static QImage visualise(RenderBackend *backend, const QString &leftPdf, const QString &rightPdf, int leftPage,
                            int rightPage, const Options &options);

    /** A one-line description of an outcome, for lists and reports. */
    static QString describe(Change change);
};

} // namespace ps
