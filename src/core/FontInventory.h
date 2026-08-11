/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#pragma once

#include <QByteArray>
#include <QHash>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

namespace ps {

/** One font a document uses, and everything worth knowing about it. */
struct FontUse {
    /** `/BaseFont`, subset prefix included, as the document spells it. */
    QString baseFont;

    /** The same without the `ABCDEF+` prefix, which is what a person wants to read. */
    QString family;

    /** `/Type1`, `/TrueType`, `/Type0`, `/Type3` or `/MMType1`. */
    QString subtype;

    /** True when the glyphs travel with the document. */
    bool embedded = false;

    /** True when only part of the font is here, as the `ABCDEF+` prefix says. */
    bool subset = false;

    /** Without it text cannot be copied out, nor searched, nor read aloud. */
    bool hasToUnicode = false;

    /** The name the page refers to it by, e.g. "/F1". */
    QString resourceName;

    /** Zero-based page indexes where it is used, in order. */
    QVector<int> pages;

    /**
     * Which characters it can show.
     *
     * From the `/ToUnicode` map where there is one and from the encoding
     * otherwise, so a font may well be able to draw more than this says. This is
     * what the document lets anyone *address*, which is the question that
     * matters when the text is to be changed.
     */
    QSet<QChar> coverage;

    /** The embedded programme's size once decompressed, or zero when there is none. */
    qint64 fileSize = 0;

    /** As the file spells it: "/WinAnsiEncoding", "/Identity-H", "/Differences". */
    QString encoding;

    /** From the OS/2 fsType bits of an embedded TrueType: may it be embedded onward? */
    QString licence;
};

/** What cutting one embedded font down to size came to. */
struct SubsetReduction {
    /** `/BaseFont` before the reduction, as the document spelled it. */
    QString baseFont;

    /** The `/Fx` names the pages call it by; unchanged by the reduction. */
    QStringList resourceNames;

    int glyphsBefore = 0;
    int glyphsAfter = 0;

    /** The embedded programme's size, decompressed, before and after. */
    qint64 bytesBefore = 0;
    qint64 bytesAfter = 0;
};

/**
 * What fonts a document uses, and what can be done about them.
 *
 * Almost every awkward question about a PDF turns out to be a question about its
 * fonts. Why is this file eleven megabytes. Why can nobody copy text out of it.
 * Why does the same typeface appear four times in the font list. Why will this
 * text not let itself be edited. None of those can be answered without looking
 * at what the file actually carries, and no viewer shows it in a form anyone can
 * act on.
 *
 * So this reads it out: every font, where it is used, whether its glyphs travel
 * with the document or are merely hoped for at the far end, whether it is a
 * subset, whether its text can be copied, how much of the file it accounts for,
 * and what its licence says. Then the three repairs that follow from knowing:
 * @ref removeEmbedding to get the size back, @ref mergeSubsets to undo what
 * merging documents did, and @ref addToUnicode to make a file's text readable
 * again.
 */
class FontInventory
{
public:
    /** Every font the pages use, one entry per font object however often it appears. */
    static QVector<FontUse> read(const QString &pdf, QString *error);

    /**
     * The glyphs one page carries, keyed by the `/Fx` name the page calls them by.
     *
     * For drawing the page's own words somewhere the page is not: an editor
     * that has to show a line in the face it is actually set in, rather than in
     * whatever the desktop uses for menus. The bytes are the font programme as
     * the document stores it, decompressed: TrueType from `/FontFile2`, a
     * compact or OpenType programme from `/FontFile3`, Type 1 from `/FontFile`.
     *
     * A font whose glyphs do not travel with the document is simply absent, and
     * so is one this build cannot decompress. Both mean the same thing to a
     * caller (there is nothing here to draw with, find something that looks
     * like it), so neither is an error.
     */
    static QHash<QString, QByteArray> embeddedProgrammes(const QString &pdf, int page, QString *error);

    /**
     * Throws the glyphs away and keeps the reference.
     *
     * For the case where a document has to shrink and the fonts in it are ones
     * every machine has anyway. The text stays where it is and stays selectable;
     * what goes is the guarantee that it will look the same elsewhere, so this is
     * a trade rather than an improvement. An empty @p baseFonts means every
     * embedded font in the file.
     */
    static bool removeEmbedding(const QString &in, const QString &out, const QStringList &baseFonts, int *removed,
                                QString *error);

    /**
     * Merges several subsets of the same family into one font object.
     *
     * What produces them is merging documents: three chapters, each carrying its
     * own subset of the same typeface, become one file carrying three. Where two
     * subsets are the same font programme with the same encoding (the usual
     * case, because the chapters came out of the same program), they are reduced
     * to one object and every page is pointed at it.
     *
     * Subsets that genuinely hold different glyphs are left alone. Combining
     * those means rewriting the glyph programmes and every character code on
     * every page that refers to them, and a wrong answer there is a page of
     * wrong letters.
     */
    static bool mergeSubsets(const QString &in, const QString &out, int *merged, QString *error);

    /**
     * Writes a `/ToUnicode` map for fonts that lack one, so their text becomes
     * copyable.
     *
     * The map is worked out from the font's own encoding: a `/Differences` array
     * saying `/eacute` means the code beside it draws an é, whatever the font
     * chose to call its glyphs. That is the same reading @ref read does, written
     * back into the file so that every other program can do it too.
     */
    static bool addToUnicode(const QString &in, const QString &out, int *added, QString *error);

    /**
     * Cuts embedded fonts down to the glyphs the pages actually draw.
     *
     * The other half of @ref mergeSubsets, and the commoner complaint: a
     * two-page letter carrying a whole 700 kB typeface because whatever produced
     * it embedded the font rather than a subset of it. What is used is measured
     * from the content streams (every page, every form XObject, every
     * annotation's appearance), and everything else is left behind.
     *
     * Nothing on any page changes. The `/Encoding`, the `/Widths` and the
     * character codes are untouched; only the glyph programme is rewritten, with
     * a `cmap` that still answers for every code the document can address. A
     * font whose codes cannot be resolved to glyphs (a symbolic font, a
     * composite one, a compact or Type 1 programme) is left exactly as it was
     * and named in @p notes, because a font nobody can read is not one to
     * rewrite on a guess.
     */
    static bool subsetEmbedded(const QString &in, const QString &out, QVector<SubsetReduction> *reduced,
                               QStringList *notes, QString *error);

    /** Plain-language notes about what this cannot promise. */
    static QStringList limitations();
};

} // namespace ps
