/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#pragma once

#include <QColor>
#include <QRectF>
#include <QString>
#include <QStringList>
#include <QVector>

namespace ps {

/**
 * Changing the words on a page.
 *
 * The feature everyone asks for and nobody quite gets. What people almost always
 * mean is "there is a typo in this letter", "the date is wrong", "that is not my
 * name": a short run of text that has to say something else. That is what this
 * does, and it does it properly: the text is replaced in the page's own
 * instruction stream, in the page's own font, and everything else on the page is
 * left byte for byte alone.
 *
 * What it deliberately does not do is reflow. There is no paragraph model in a
 * PDF (a page is a list of glyphs at coordinates), so making a sentence longer
 * cannot push the next line down, because nothing in the file says which line
 * comes next. Rather than guess, a replacement keeps the run inside the space
 * the original occupied, and says what it had to do to fit.
 *
 * ## Where it refuses
 *
 * A font embedded in a PDF usually carries only the characters the document
 * actually used. Asking for a "ü" in a document that never contained one gets a
 * refusal naming the character, not a blank space or a wrong glyph. Knowing
 * which characters a font can show comes from its `/ToUnicode` map where it has
 * one, which is most of the time, and from its encoding and width table
 * otherwise.
 */
class TextEdit
{
public:
    /** One text-showing operation on a page, as found in its content stream. */
    struct Run {
        int page = 0;

        /**
         * Identifies which operation this is, counting from the start of the
         * page's content stream.
         *
         * Stable for as long as the page is not otherwise edited, which is why a
         * replacement carries it rather than carrying coordinates.
         */
        int index = -1;

        QString text;

        /** In display points, origin at the bottom left of the page. */
        QRectF rect;

        /** The resource name of the font, e.g. "/F1". */
        QString fontResource;

        /** The `Tf` operand, which is the size in text space and not on paper. */
        double fontSize = 0.0;

        /**
         * The size the letters actually came out, in display points.
         *
         * A page may set `/F1 1 Tf` and do the sizing in the text matrix, which
         * is what several layout programs emit, so @ref fontSize on its own says
         * nothing about how tall the type is. This is that number with the
         * matrices applied, and it is what anything drawing the run has to use.
         */
        double scaledSize = 0.0;

        /**
         * `/BaseFont` without the `ABCDEF+` subset prefix, e.g. "LyonText-Regular".
         *
         * The page's own name for the face, which is what anything meaning to
         * draw this text the way the page draws it has to start from. A viewer
         * that starts from its own interface font instead shows the words in the
         * right place at the right height and in nothing else that is right.
         */
        QString fontFamily;

        /** 100 to 900, as everything outside PDF counts weight. 400 is regular. */
        int fontWeight = 400;

        bool italic = false;

        /** From the descriptor's flags: what to fall back to when the face is absent. */
        bool serif = false;
        bool fixedPitch = false;

        /** True when the glyphs travel with the document and can be drawn exactly. */
        bool embedded = false;

        /** The colour the page fills this text with. */
        QColor colour = QColor(Qt::black);

        /** Extra space after every glyph, in display points: the `Tc` operator. */
        double charSpacing = 0.0;

        /** The same after every space, which is `Tw`. */
        double wordSpacing = 0.0;

        /** Horizontal scaling, 1.0 being none: `Tz` divided by a hundred. */
        double horizontalScale = 1.0;

        /**
         * The `Tr` mode. 3 draws nothing, which is how a scan carries its
         * recognised text: drawing that text visibly would put black letters over
         * the picture of the page.
         */
        int renderMode = 0;

        /** False when the font cannot show characters the run does not already use. */
        bool editable = true;

        /** Why not, when it is not. */
        QString limitation;
    };

    /**
     * Type as somebody wants it set, where that differs from how the page sets it.
     *
     * Every member has a value that means "leave this as the page has it", so a
     * caller that only wants a different size says so and says nothing else. A
     * change of family is the expensive one: the new face has to be cut down and
     * put into the document, because a page can only draw glyphs it carries.
     */
    struct Format {
        /** Empty keeps the run's own face. */
        QString family;

        bool bold = false;
        bool italic = false;

        /** Zero keeps the run's own size. */
        double size = 0.0;

        /** An invalid colour keeps the run's own. */
        QColor colour;

        bool isEmpty() const { return family.isEmpty() && size <= 0.0 && !colour.isValid(); }
    };

    struct Replacement {
        int page = 0;
        int index = -1;
        QString text;

        /** How it is to be set, where that is not how the page already sets it. */
        Format format;
    };

    struct Options {
        /**
         * Squeeze a replacement that would be wider than the text it replaces.
         *
         * Only wider. A shorter replacement simply makes the line shorter, which
         * is what shortening a line looks like; stretching it back out to the old
         * width would give four-hundred-percent-wide letters that fool nobody.
         * A longer one, though, would run into whatever sits to its right, so it
         * is squeezed, down to @ref minimumSqueeze and no further, because
         * letters at a tenth of their width are not text any more.
         */
        bool fitWidth = true;

        /** How narrow letters may be squeezed, as a fraction. */
        double minimumSqueeze = 0.6;
    };

    struct Report {
        int replaced = 0;

        /** How many of those were also set in a different face, size or colour. */
        int restyled = 0;

        /** Runs that were asked for but could not be changed, and why. */
        QStringList refusals;

        /**
         * What was done that the user should know about but did not ask for.
         *
         * A licence that says the face may not travel, an italic cut that is not
         * installed so the upright one went in instead. Not refusals: the change
         * was made, and this is what it cost.
         */
        QStringList notes;

        /** The largest squeeze applied, as a percentage narrower than normal. */
        double widthAdjustment = 0.0;

        /** Replacements too long for their space even after squeezing. */
        QStringList overflowed;
    };

    /** Every text run on @p page, in the order the page draws them. */
    static QVector<Run> runsOn(const QString &pdf, int page, QString *error);

    /**
     * Replaces the named runs.
     *
     * Anything that cannot be done is reported through @p report rather than
     * failing the whole operation, so that fixing four typos does not fall over
     * because the fifth needs a character the font has never seen.
     */
    static bool apply(const QString &inputPdf, const QString &outputPdf, const QVector<Replacement> &replacements,
                      const Options &options, Report *report, QString *error);

    /** Plain-language notes about what this cannot promise. */
    static QStringList limitations();
};

} // namespace ps
