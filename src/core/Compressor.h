/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#pragma once

#include <QObject>
#include <QString>
#include <QVector>

#include <functional>

namespace ps {

/**
 * Makes PDFs smaller, and says honestly what it cost.
 *
 * Two engines, picked by level:
 *
 * - **Lossless** goes through QPDF alone: object streams, re-compressed
 *   streams, duplicate objects merged. Every pixel survives. Typical saving on
 *   a text document is 5 to 30%, on a scan close to nothing.
 * - **Everything else** shells out to Ghostscript to downsample images, which
 *   is where the real savings on scans live.
 *
 * Ghostscript is invoked as a separate process, never linked. That keeps its
 * AGPL licence off this codebase and makes it an optional dependency: without
 * it installed, the lossless path still works and the UI says why the rest is
 * unavailable.
 */
class Compressor
{
public:
    enum class Level {
        Lossless, //!< structural only, no image touched
        Balanced, //!< 150 dpi images: screen reading, still prints acceptably
        Strong, //!< 110 dpi: email attachments
        Extreme, //!< 72 dpi: smallest useful, visibly softer
    };

    struct Options {
        Level level = Level::Balanced;

        /** Overrides the level's image resolution when greater than zero. */
        int imageDpi = 0;

        int jpegQuality = 75;

        /** Convert everything to grayscale, which often halves a colour scan. */
        bool grayscale = false;

        /** Drop document metadata (author, producer, timestamps). */
        bool removeMetadata = false;

        /** Web-optimised page order. */
        bool linearize = false;
    };

    enum class Outcome {
        Shrunk, //!< the result is meaningfully smaller
        AlreadyOptimal, //!< the saving was negligible or the original was kept
    };

    /** Below this, the saving is not worth calling a saving. */
    static constexpr int MeaningfulSavingPercent = 1;

    struct Report {
        qint64 originalBytes = 0;
        qint64 compressedBytes = 0;
        bool usedGhostscript = false;

        /**
         * Whether anything was actually achieved.
         *
         * Reporting "0 % smaller" and stopping there tells the user nothing.
         * Scans are very often already stored as JPEG or JPEG 2000 below the
         * target resolution, in which case no level will help and saying so is
         * more useful than a number.
         */
        Outcome outcome = Outcome::Shrunk;

        qint64 savedBytes() const { return originalBytes - compressedBytes; }

        /** Percent saved, 0 when nothing was gained. */
        int savedPercent() const
        {
            if (originalBytes <= 0 || compressedBytes >= originalBytes) {
                return 0;
            }
            return static_cast<int>((originalBytes - compressedBytes) * 100 / originalBytes);
        }
    };

    struct Estimate {
        qint64 originalBytes = 0;
        qint64 expectedBytes = 0;
        int pagesSampled = 0;
        bool reliable = false;

        int savedPercent() const
        {
            if (originalBytes <= 0 || expectedBytes >= originalBytes) {
                return 0;
            }
            return static_cast<int>((originalBytes - expectedBytes) * 100 / originalBytes);
        }
    };

    /**
     * Works out roughly what compression would achieve, without doing it.
     *
     * A handful of pages are compressed for real and the ratio is applied to
     * the whole document. Compressing a four-hundred-page scan to find out
     * whether it is worth compressing is not a reasonable thing to ask of
     * anyone, and an honest estimate beats an exact answer nobody waits for.
     *
     * @p sampleExtractor is handed a page index and must write that single
     * page somewhere, returning its path; the caller owns the page list, so
     * only it can do that.
     */
    static bool estimate(const QString &inputPath, const QVector<int> &pageIndexes,
                         const std::function<QString(int)> &sampleExtractor, const Options &options, Estimate *estimate,
                         QString *error);

    /** True when the lossy levels can be offered at all. */
    static bool isGhostscriptAvailable();

    /** Path to the Ghostscript binary, empty when not installed. */
    static QString ghostscriptPath();

    static QString levelName(Level level);
    static int dpiForLevel(Level level);

    /**
     * Compresses @p inputPath into @p outputPath.
     *
     * The result is only kept when it is actually smaller, because a "compression"
     * that inflates the file is a bug from the user's point of view, so in that
     * case the original is copied through unchanged and the report says so.
     */
    static bool compress(const QString &inputPath, const QString &outputPath, const Options &options, Report *report,
                         QString *error);
};

} // namespace ps
