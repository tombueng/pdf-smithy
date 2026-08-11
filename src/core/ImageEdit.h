/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#pragma once

#include <QImage>
#include <QRectF>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QVector>

namespace ps {

/**
 * The pictures in a document, as objects you can open rather than pixels you
 * must accept.
 *
 * A PDF does not hold "the image on page three". It holds an image XObject in
 * some resource dictionary and, somewhere in a content stream, a matrix and a
 * `Do` that puts it on a page. Those two halves are what this class works with,
 * and keeping them apart is what makes the difference between a toy and a tool:
 * the same picture can be drawn twenty times at twenty sizes, and a change that
 * forgets this either does its work twenty times over or quietly alters the
 * nineteen placements nobody asked about.
 *
 * ## Effective resolution is the number that matters
 *
 * Not `/Width`. A 4000-pixel photograph placed five centimetres wide is two
 * thousand pixels per inch: it costs megabytes and shows nothing a reader can
 * see. ImageUse::effectiveDpiX and ImageUse::effectiveDpiY come from the matrix
 * that placed the picture, which is the only place that answer lives, and they
 * are why recompress() can promise a saving without guessing.
 *
 * ## What it refuses to do
 *
 * Fax (CCITT, JBIG2) and JPEG 2000 pixels cannot be opened here, and
 * ImageUse::decodable says so before anything is attempted. The operations that
 * need pixels then refuse by name instead of dropping the picture or replacing
 * it with something grey: a scan silently emptied is far worse than an
 * operation that would not run.
 *
 * The other refusal is subtler and matters just as much: **an image that does
 * not need re-encoding is left byte for byte alone.** Ask recompress() for 150
 * dpi on a document already at 150 dpi and it reports zero saved and writes the
 * original JPEG straight through. Generation loss inflicted for nothing is
 * exactly what a professional tool must never do.
 *
 * ## Which placement an operation means
 *
 * Everything is addressed by page index plus resource name, e.g. page 0 and
 * `/Im0`, because that pair is what a content stream actually says. read()
 * therefore reports one entry per name per page. Where one name is drawn
 * several times on the same page, the entry describes the largest of those
 * placements (the one that decides how many pixels are really needed), and
 * an operation on that name applies to every place the page draws it.
 */
class ImageEdit
{
public:
    /** One picture as one page draws it. */
    struct ImageUse {
        /** Zero-based page index. */
        int page = 0;

        QString resourceName; //!< the /Im0 name

        QSize pixelSize;

        /** Where it sits, in display points, origin bottom-left. */
        QRectF placement;

        /** The number that actually matters: pixels per inch as placed. */
        double effectiveDpiX = 0.0;
        double effectiveDpiY = 0.0;

        QString encoding; //!< DCTDecode, FlateDecode, CCITTFaxDecode, JPXDecode, none

        QString colourSpace; //!< DeviceRGB, DeviceGray, DeviceCMYK, ICCBased(3), Indexed, …

        int bitsPerComponent = 8;

        bool hasAlpha = false;

        /**
         * A stencil rather than a picture: its one-bit samples choose where the
         * page's current fill colour lands. Its pixels carry no colour of their
         * own, so changing them is not the same kind of operation at all.
         */
        bool isMask = false;

        qint64 storedBytes = 0;

        bool decodable = false; //!< whether this tool can open its pixels

        /** How often this same XObject is placed, across the whole document. */
        int drawnTimes = 1;
    };

    /**
     * Every picture every page draws, in the order the pages draw them.
     *
     * Cheap enough to call on a two-hundred-page magazine: the content streams
     * are walked but no pixels are decoded.
     */
    static QVector<ImageUse> read(const QString &pdf, QString *error);

    struct Recompress {
        double targetDpi = 150.0; //!< 0 keeps the pixel size
        int jpegQuality = 82;
        bool toGrayscale = false;
        bool lossless = false; //!< re-deflate rather than re-encode as JPEG

        /**
         * Re-encode even when the pixel count and the colour are already right.
         *
         * A new field rather than a new meaning for an old one, and a field
         * rather than a second entry point: "same size, quality 40" differs from
         * an ordinary recompress in one bit and nothing else, so a whole
         * parallel function would have to repeat the sharing, sizing and refusal
         * rules that make this one worth having.
         *
         * Off by default, and it has to stay that way. Skipping a picture that
         * needs nothing is the promise in the class comment above: ask for 150
         * dpi on a document already at 150 dpi and the JPEG comes out with the
         * bytes it went in with. Turn this on and it does not: quality is
         * applied whatever the resolution says, which is the right answer to
         * "make this smaller without losing resolution" and the wrong answer to
         * anything else. It costs a generation of JPEG loss every time it runs.
         *
         * The one thing it will still not do is grow a picture: if the new bytes
         * are no smaller than the old, the original is kept and @p savedBytes
         * reports zero.
         */
        bool forceReencode = false;
    };

    /**
     * Recompresses just the named images, leaving the rest of the document alone.
     *
     * @p pages and @p resourceNames are read in step: the first name belongs to
     * the first page. Both empty means every picture in the document, which is
     * the whole-file mode; a name that cannot be opened is then skipped rather
     * than failing the run, whereas one asked for by name earns a refusal.
     *
     * A picture drawn on several pages is recompressed once, and sized for the
     * largest placement anywhere in the document, because downsampling for a thumbnail
     * would ruin the full-page copy of the same object.
     *
     * @p savedBytes counts the stored bytes that left the image streams, not the
     * change in file size, so a run that had nothing to do reports exactly zero.
     */
    static bool recompress(const QString &in, const QString &out, const QVector<int> &pages,
                           const QStringList &resourceNames, const Recompress &options, qint64 *savedBytes,
                           QString *error);

    struct Adjust {
        double brightness = 0.0; //!< -1..1
        double contrast = 0.0; //!< -1..1

        /** Above one brightens the midtones, below one darkens them. */
        double gamma = 1.0;

        bool sharpen = false;
        bool despeckle = false;
        bool autoLevels = false;
        bool toGrayscale = false;
        bool toBlackWhite = false;
        int threshold = 128; //!< for toBlackWhite
    };

    /**
     * Works over the pixels of one placement.
     *
     * The picture keeps its size on the page and its number of pixels; only the
     * values change. A picture shared with another page is copied first, so the
     * other page keeps what it had.
     */
    static bool adjust(const QString &in, const QString &out, int page, const QString &resourceName,
                       const Adjust &options, QString *error);

    /**
     * Crops the pixels themselves, so the cropped-away part leaves the file.
     *
     * @p keepFractional is the part to keep as fractions of the stored picture,
     * measured from its top-left corner the way the pixels are stored: x to the
     * right, y downwards. The kept part stays exactly where it was on the page:
     * the placement matrix is narrowed to match, rather than the remaining
     * pixels being stretched back over the old area.
     */
    static bool crop(const QString &in, const QString &out, int page, const QString &resourceName,
                     const QRectF &keepFractional, QString *error);

    /** What a turn actually did, because "it was turned" is not the whole answer. */
    struct RotateReport {
        /** How many places on the page were turned; one name can be drawn several times. */
        int placementsTurned = 0;

        /** Placements whose own matrix cannot be reversed arithmetically, so no turn could be worked out. */
        int placementsRefused = 0;

        /**
         * True when the stored pixels came through byte for byte.
         *
         * The normal case, and the whole point: a turn is a matrix in the
         * content stream, so a JPEG keeps its exact compressed bytes and does
         * not gain a generation. Nothing in rotate() ever decodes anything, so
         * this only goes false if a placement had to be left alone.
         */
        bool pixelsUntouched = true;

        /** A quarter, a half or three quarters, so the pixel grid lands back on itself. */
        bool quarterTurn = false;

        /** Where the picture ends up, in display points, origin bottom-left. */
        QRectF placement;

        /** What the caller should know, in words a person can act on. */
        QStringList notes;
    };

    /**
     * Turns one placed picture by @p degrees, clockwise as a reader sees it,
     * about the middle of where it sits.
     *
     * **No pixel is touched and nothing is re-encoded.** A PDF image is put on a
     * page by a matrix, so a turn belongs in that matrix: the stored stream comes
     * out of the far end with the bytes it went in with, whatever it is. That
     * also means any angle works, not just quarter turns, and that a fax or a
     * JPEG 2000 picture, which nothing here can decode, turns just as well as
     * a JPEG.
     *
     * The picture keeps its size and its centre, so a turn that is not a
     * half-circle leaves a non-square picture covering a different part of the
     * page. @p report says where it ended up and warns when that is off the page.
     *
     * A placement whose matrix is degenerate cannot be turned, because the sum
     * that expresses a page-space movement inside the stream needs to reverse it.
     * Those are counted in RotateReport::placementsRefused rather than rasterised
     * behind the caller's back. @p report may be null.
     */
    static bool rotate(const QString &in, const QString &out, int page, const QString &resourceName, double degrees,
                       RotateReport *report, QString *error);

    /**
     * Puts a different picture in the same place.
     *
     * With @p keepAspect the replacement is fitted inside the old placement and
     * centred, which is almost always what someone swapping a photograph wants;
     * without it, it is stretched to fill exactly the area the old one covered.
     */
    static bool replace(const QString &in, const QString &out, int page, const QString &resourceName,
                        const QImage &replacement, bool keepAspect, QString *error);

    /**
     * Writes one picture out to @p toFile, guessing the format from its suffix.
     *
     * A JPEG asked for as `.jpg` is copied out compressed byte for byte: the
     * one extraction that loses nothing at all. Anything else is decoded and
     * saved by Qt, with a soft mask folded back in as alpha where the format can
     * carry it.
     */
    static bool extract(const QString &pdf, int page, const QString &resourceName, const QString &toFile,
                        QString *error);

    /** Stops the page drawing it. The bytes go when nothing else points at them. */
    static bool remove(const QString &in, const QString &out, int page, const QString &resourceName, QString *error);

    /**
     * Identical pictures stored more than once, merged into one object.
     *
     * @p merged counts the objects that disappeared and @p saved their stored
     * bytes.
     */
    static bool deduplicate(const QString &in, const QString &out, int *merged, qint64 *saved, QString *error);

    /** Plain-language notes about what this cannot promise. */
    static QStringList limitations();
};

} // namespace ps
