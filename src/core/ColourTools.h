/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#pragma once

#include <QColor>
#include <QHash>
#include <QImage>
#include <QString>
#include <QStringList>
#include <QVector>

#include <limits>

namespace ps {

/**
 * Colour, as a print shop cares about it.
 *
 * Everything here exists because the answer people are given on Linux today is
 * a Ghostscript incantation copied off a forum, which converts the whole
 * document, throws the text away into a rasterised approximation often enough
 * to be frightening, and never says what it did. This class takes the opposite
 * position: say which route was taken, touch only what was asked for, and
 * refuse rather than guess.
 *
 * ## Two routes, and the difference is the point
 *
 * Text and vector art carry their colour in the content stream, as operators:
 * `g`, `rg`, `k`, `scn` and their stroking twins. Those can be rewritten by
 * walking the stream and replacing the numbers, which is *exact*: the glyphs,
 * the paths, the fonts, the structure and every byte that is not a colour come
 * out untouched. That is the route this class prefers, and it is why a page not
 * listed in ConvertOptions::pages comes out with its content stream unchanged.
 *
 * Images carry their colour in their pixels, so those have to be decoded,
 * converted and re-embedded. Indexed images are the happy exception: only the
 * palette is rewritten, so the pixel data and its compression survive intact.
 *
 * ## What "converted" is allowed to mean
 *
 * Some conversions are defined by the PDF specification itself and are therefore
 * exact arithmetic, not an approximation: DeviceRGB to DeviceGray is
 * `0.3 R + 0.59 G + 0.11 B`, DeviceCMYK to DeviceRGB is `1 - min(1, C + K)` per
 * channel, and DeviceGray goes to either without loss. Those are done in place
 * and reported as spec-defined.
 *
 * Conversion *into* CMYK is not defined by the specification and cannot be
 * honestly faked. With LittleCMS and an ICC profile it is done properly, in
 * place, and the report names the profile. Without one, Ghostscript is asked to
 * convert the whole document and the report says so plainly, including that
 * the page selection could not be honoured on that route. What this will never
 * do is apply `C = 1 - R` and call the result a CMYK conversion.
 *
 * A Separation is the third case and the nicest one: the file itself carries the
 * tint transform that defines what the ink means in process colour, so
 * converting a spot colour needs no profile at all, only the arithmetic the
 * document already specified.
 */
class ColourTools
{
public:
    struct Inventory {
        /** Which colour spaces the document actually uses. */
        QStringList spaces;

        /** Named separations found, e.g. "Pantone 300 C". */
        QStringList spotColours;

        bool hasIccProfile = false;
        bool hasOutputIntent = false;
        bool usesTransparency = false;
        bool usesOverprint = false;
        int pagesWithRgb = 0;
        int pagesWithCmyk = 0;

        /** Strokes thinner than a quarter point, which vanish in print. */
        int hairlines = 0;

        /**
         * The thinnest stroke on the paper, in points, after the page's own scaling.
         *
         * Reported separately from the hairline count because a file can be clean
         * and still be close to the edge: a magazine whose finest rule is 0.288 pt
         * has no hairlines, but a press that holds a quarter point badly will lose
         * it anyway. Infinite when the document strokes nothing.
         */
        double thinnestStroke = std::numeric_limits<double>::infinity();

        /** How many strokes were seen, which says whether the walk got anywhere. */
        int strokes = 0;

        /** How many pages were read; the counts above are out of this. */
        int pages = 0;

        /** Anything found that this class cannot convert in place. */
        QStringList notes;
    };

    /**
     * Reads what the document does with colour, without changing anything.
     *
     * Measured from the content streams and the resource dictionaries rather
     * than from a rendering, so it sees a spot colour that is defined but never
     * used and it does not see colour that only a shading pattern produces.
     */
    static Inventory inspect(const QString &pdf, QString *error);

    enum class Target { Grayscale, BlackWhite, Rgb, Cmyk };

    struct ConvertOptions {
        Target target = Target::Grayscale;

        /** Pages to touch; empty means all of them. */
        QVector<int> pages;

        /** An ICC profile for the destination. Empty looks for one on the system. */
        QString iccProfilePath;

        /** For BlackWhite: above this luminance a colour becomes white. */
        double threshold = 0.5;

        /**
         * Re-encode converted photographs as JPEG rather than storing pixels.
         *
         * On by default because a greyscaled 300 dpi scan stored raw is roughly
         * eight times the size of the JPEG it came from, which turns a colour
         * conversion into a filesize complaint.
         */
        bool recompressPhotographs = true;

        int jpegQuality = 92;
    };

    /**
     * Converts the colours of a document.
     *
     * Two ways in, and the difference is honest rather than incidental: text and
     * vector colour operators are rewritten in place, which is exact and lossless;
     * images have their pixels converted. Where a whole-document colour-managed
     * conversion is asked for, Ghostscript does it and the report says so.
     *
     * @p changes receives one sentence per thing that happened, including which
     * route was taken and what could not be reached. Read it and show it: a
     * conversion the user cannot inspect is a conversion they cannot trust.
     */
    static bool convert(const QString &in, const QString &out, const ConvertOptions &options, QStringList *changes,
                        QString *error);

    /**
     * Replaces every use of one colour with another, exactly.
     *
     * @p tolerance is a distance in unit RGB, so 0 matches only the colour
     * itself and 0.05 forgives the rounding a design tool leaves behind.
     * Replacements are written in DeviceRGB, or DeviceGray when the replacement
     * is a grey and the original was one, because writing an arbitrary colour
     * back into CMYK or into a spot ink would need a conversion this function
     * has not been given a profile for.
     */
    static bool replaceColour(const QString &in, const QString &out, const QColor &from, const QColor &to,
                              double tolerance, int *replaced, QString *error);

    /**
     * Renames, merges or converts named separations to process colour.
     *
     * @p renames maps an existing ink name to a new one; pointing two inks at
     * the same new name is how they are merged, because a printer plates by
     * name. @p toProcess names inks to dissolve into process colour, which is
     * done through the document's own tint transform and so needs no profile.
     */
    static bool mapSpotColours(const QString &in, const QString &out, const QHash<QString, QString> &renames,
                               const QStringList &toProcess, int *changed, QString *error);

    /**
     * Sets strokes thinner than @p minimumWidth to exactly that, so they print.
     *
     * The comparison is made in device space, after the current transformation
     * matrix, because a `0.5 w` inside a `0.1 0 0 0.1 0 0 cm` is a twentieth of
     * a point on paper whatever the operator says. The replacement width is
     * scaled back so that the stroke lands at @p minimumWidth on the page.
     */
    static bool fixHairlines(const QString &in, const QString &out, double minimumWidth, int *fixed, QString *error);

    /**
     * Turns overprint on or off for black text, the commonest prepress request.
     *
     * Wherever the content stream selects black (`0 g`, `0 0 0 rg`, or CMYK
     * with only the black channel raised), a graphics state that sets `/OP` and
     * `/op` is inserted straight after it, so the change is scoped to black and
     * nothing else on the page acquires or loses overprint by accident.
     */
    static bool setBlackOverprint(const QString &in, const QString &out, bool on, int *changed, QString *error);

    /**
     * Total ink coverage per page, as a percentage; over 300 is trouble.
     *
     * Measured, not estimated: Ghostscript renders each page to a CMYK raster
     * and the highest C+M+Y+K found on the page is reported. The maximum is the
     * number a print shop's ink limit is about, because an average would hide the one
     * dark panel that will not dry. It follows that resolution matters a little,
     * and that a document with no CMYK in it is still measured through
     * Ghostscript's own conversion, which is not colour-managed unless the file
     * carries an output intent.
     *
     * Returns an empty vector and sets @p error when Ghostscript is missing.
     */
    static QVector<double> inkCoverage(const QString &pdf, QString *error);

    /**
     * One separation on its own, rendered, for a separations preview.
     *
     * Genuinely separated by Ghostscript rather than approximated from a colour
     * rendering: process inks come from its CMYK device, and a named spot ink
     * comes from its `tiffsep` device, which keeps the ink as its own plate. The
     * result is an ink density map (black is full ink, white is bare paper),
     * so it looks like the plate rather than like the page.
     *
     * @p inkName is "Cyan", "Magenta", "Yellow", "Black", or a separation name
     * as inspect() reports it.
     */
    static QImage separation(const QString &pdf, int page, const QString &inkName, double dpi, QString *error);

    /**
     * True when this build can convert into CMYK without Ghostscript.
     *
     * False means convert() will fall back to a whole-document Ghostscript run
     * for Target::Cmyk, which cannot honour a page selection. Worth asking
     * before offering the option.
     */
    static bool hasColourManagement();

    /**
     * An ICC profile on this system suitable for @p target, or empty.
     *
     * Only ever a guess at what the user meant; a print job has a profile named
     * in its specification and that one should be passed explicitly.
     */
    static QString defaultIccProfile(Target target);

    /** Plain-language notes about what this cannot promise. */
    static QStringList limitations();
};

} // namespace ps
