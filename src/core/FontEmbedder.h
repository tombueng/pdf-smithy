/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#pragma once

#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

class QPDF;
class QPDFObjectHandle;

namespace ps {

/**
 * Putting a font into a document that never had it.
 *
 * This is what lifts the one real limit on editing text. A font embedded in a
 * PDF carries only the glyphs the document happened to use, so replacing
 * "Muller" with "Müller" in a document that never contained an umlaut has to be
 * refused: there is no "ü" in the file to draw. The way out is to stop asking
 * the document's font and bring one along instead: take a font off this system,
 * cut it down to the handful of characters actually wanted, and add it to the
 * page as a new resource.
 *
 * ## Why a subset and not the font
 *
 * DejaVu Sans is three quarters of a megabyte. A letter that needs nine
 * characters of it should not grow by that much, and a document that gained a
 * whole font per correction would be absurd within a week. So the font file is
 * rewritten: its tables are taken apart, the wanted glyphs are copied into a
 * new one, and everything else is left behind. Two or three kilobytes is the
 * usual result.
 *
 * The one thing that has to be got right in that rewrite is composite glyphs.
 * An "ä" in almost every font is not a shape of its own; it is a reference to an
 * "a" and a reference to a diaeresis. Copy the "ä" alone and it draws nothing:
 * a blank where the letter should be, in a file that looks perfectly valid. So
 * the wanted set is closed over the glyphs its members refer to before anything
 * is copied, and the references inside the copied outlines are renumbered to
 * match their new positions.
 *
 * ## What the document ends up with
 *
 * A `/TrueType` simple font with an `/Encoding` that names every glyph it
 * carries, a `/FontDescriptor` whose `/FontFile2` holds the subset, and a
 * `/ToUnicode` map so the text can still be copied out of the finished
 * document. The base font gets the customary six-capital prefix, which is how
 * every other producer marks a subset and how @ref FontInventory recognises
 * one.
 *
 * ## Repairing what a document already refers to
 *
 * Adding a font is only half of it. The other half is the complaint every
 * prepress desk makes ("this document is missing Helvetica"), where the page
 * already says `/F1 12 Tf` and no glyphs travel with the file. @ref embedMissing
 * and @ref replaceFont answer that by working on the font object the pages
 * *already* refer to, rather than adding a second one beside it: the `/Fx` name
 * stays, the content streams are never touched, and every byte between the
 * brackets of a `Tj` still means the letter it always meant.
 *
 * Which is only true if three things are preserved exactly. The `/Encoding`,
 * because that is what says code 233 is an é. The `/Differences`, because that
 * is what says it in the files that come out of serious layout programs. And the
 * `/Widths`, because a simple font's widths live in the document rather than in
 * the font programme, so keeping the array keeps every glyph in the position the
 * page put it in, whatever the new face's own metrics say. A substitution that
 * rewrote any of the three would reflow the page, and a reflowed page is a
 * ruined document that still opens.
 *
 * Where the document states no widths at all (the standard fourteen may leave
 * them out, because every reader is required to know them), they are written
 * from the standard metrics, so the spacing survives being made explicit. Where
 * even that is not possible, the substitution says so rather than quietly
 * letting the lines move.
 *
 * ## Licences
 *
 * A TrueType font states in its `OS/2` table whether it may be embedded. That
 * is read and reported, never enforced: the person doing the embedding may hold
 * a licence the bits know nothing about, and a program that refuses to use a
 * font its owner paid for is not being careful. But it does say so, because the
 * person who does *not* hold that licence deserves to know before sending the
 * file out.
 */
class FontEmbedder
{
public:
    struct Request {
        QString family = QStringLiteral("DejaVu Sans");
        bool bold = false;
        bool italic = false;

        /**
         * Only these characters go in.
         *
         * A subset of what a document uses, not a whole font. Characters outside
         * the basic multilingual plane cannot be asked for at all, because a
         * QChar cannot hold one.
         */
        QSet<QChar> characters;
    };

    struct Result {
        /** The `/Fx` name it was added under, ready to be put in front of a `Tf`. */
        QString resourceName;

        /** Glyphs in the embedded programme, `.notdef` and composite parts included. */
        int glyphCount = 0;

        qint64 embeddedBytes = 0;

        /** What the user should know: a licence, a missing character, a substituted cut. */
        QStringList warnings;
    };

    /**
     * What became of one font the document refers to.
     *
     * One of these per font looked at, not per font changed: an empty @ref
     * family means nothing was done to it, and @ref warnings says why. A call
     * that quietly returned only its successes would let a Type 0 font it
     * cannot touch pass for one it fixed.
     */
    struct Substitution {
        /** The `/BaseFont` the document named, spelled as the document spells it. */
        QString baseFont;

        /** The face taken off this system, or empty when the font was left alone. */
        QString family;

        /** The `/Fx` names the pages call it by. Unchanged; they are listed to be shown. */
        QStringList resourceNames;

        /** Zero-based pages it appears on, in order. */
        QVector<int> pages;

        /** Character codes carried across with their meaning intact. */
        int codes = 0;

        int glyphCount = 0;
        qint64 embeddedBytes = 0;

        /**
         * The widest gap between the advance the page assumes and the one the
         * new face actually has, in thousandths of the em.
         *
         * Zero means the substitute is metrically the same font by another name,
         * which is what Liberation Sans is to Helvetica. Anything larger is not
         * a reflow (the `/Widths` array still decides where each glyph sits),
         * but it is a letter that does not quite fill the room reserved for it,
         * and at some size somebody will notice.
         */
        int widthGap = 0;

        /** What could not be done, and what the reader should know before sending the file on. */
        QStringList warnings;
    };

    /** True when a usable font file for @p request can be found on this system. */
    static bool isAvailable(const Request &request);

    /**
     * Adds a subsetted, embedded font to @p page's resources and reports its name.
     *
     * The character codes to write are deliberately not returned. They are in
     * the font's own `/Encoding` and `/ToUnicode`, so whatever reads the font
     * back (this program or any other) agrees with what wrote it. One source
     * of truth, and it is in the file.
     */
    static bool embed(QPDF &pdf, QPDFObjectHandle resources, const Request &request, Result *result, QString *error);

    /**
     * The same, from one file to another, for callers holding no document open.
     *
     * @p pages is zero-based and empty means all of them. Every page gets its own
     * copy of the font, and its own entry in @p results in page order, because a
     * page's resources are its own: sharing one dictionary between pages is
     * common enough in real files that adding a font to it would put the font on
     * pages nobody asked about.
     */
    static bool embed(const QString &in, const QString &out, const Request &request, const QVector<int> &pages,
                      QVector<Result> *results, QString *error);

    /**
     * Embeds a face for every font the document refers to but does not carry,
     * and points the existing text at it.
     *
     * @p family names the face to substitute; empty asks for the closest thing
     * on this system, which is chosen from what the document says: the family
     * name it asks for if that is installed, then a metrically compatible stand
     * -in (Liberation Sans for Helvetica and Arial, Liberation Serif for Times,
     * Liberation Mono for Courier), then whatever answers to the role.
     *
     * The pages are not rewritten and do not need to be: the font object they
     * already point at is the one that gains the glyphs. See the class
     * documentation for what has to be preserved for that to be true, and read
     * @p substitutions before sending the file anywhere: a font that could not
     * be dealt with is in there with an empty family and a reason.
     */
    static bool embedMissing(const QString &in, const QString &out, const QString &family,
                             QVector<Substitution> *substitutions, QString *error);

    /**
     * Replaces one font with another throughout a document, codes and all.
     *
     * @p oldBaseFont names the font to replace, with or without its subset tag
     * and with or without the spaces a PostScript name does not have, so
     * "Liberation Serif" reaches `ABCDEF+LiberationSerif`. @p newFamily is a
     * family installed on this system.
     *
     * The same promise as @ref embedMissing and for the same reason: the codes
     * on the page keep their meaning, the `/Widths` the document states are kept
     * so nothing moves, and what the new face cannot draw is reported rather
     * than left as a blank somebody finds later.
     */
    static bool replaceFont(const QString &in, const QString &out, const QString &oldBaseFont, const QString &newFamily,
                            QVector<Substitution> *substitutions, QString *error);

    /**
     * Cuts a TrueType programme down to the glyphs @p characters need.
     *
     * The sfnt surgery on its own, without a document around it, because
     * FontInventory::subsetEmbedded needs exactly this and reimplementing it
     * there is how the two would come to disagree about composite glyphs.
     *
     * Characters are found through the programme's own `cmap`, and failing that
     * through the glyph names in its `post` table, because subsetters routinely throw
     * the `cmap` away, and a font that can only be addressed by name is still a
     * font that can be cut down. Returns empty and sets @p error when neither
     * answers, which is the honest result for a symbolic font whose codes mean
     * nothing outside itself.
     */
    static QByteArray subsetProgramme(const QByteArray &programme, const QSet<QChar> &characters, int *glyphsBefore,
                                      int *glyphsAfter, QString *error);

    /** The path Fontconfig gives for a request, for reporting. */
    static QString locate(const Request &request);

    /** Plain-language notes about what this cannot promise. */
    static QStringList limitations();
};

} // namespace ps
