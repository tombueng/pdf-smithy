/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#pragma once

#include <QColor>
#include <QImage>
#include <QPainterPath>
#include <QRectF>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QTransform>
#include <QVector>

namespace ps {

/**
 * One thing on a page that a person would point at.
 *
 * A PDF page is a list of drawing instructions, not a list of objects, so this is
 * the bridge: every painting operator becomes something with a boundary, a place
 * in the stacking order, and a handle to grab it by. That is what makes "click the
 * picture and drag it" possible at all.
 */
struct PageObject {
    enum class Kind {
        Text, //!< One text-showing operation
        Image,
        Path, //!< Filled or stroked vector art
        Drawing, //!< A form XObject: a group drawn as one unit
        Shading,
        InlineImage,
    };

    /**
     * Which painting operation this is, counting from the start of the page.
     *
     * The handle. Stable for as long as the page is not otherwise edited, which is
     * why an edit carries it rather than carrying coordinates.
     */
    int index = -1;

    int page = 0;
    Kind kind = Kind::Path;

    /** In display points, origin at the bottom left, clipping taken into account. */
    QRectF bounds;

    /** The matrix in force when it was drawn, mapping its own space to the page. */
    QTransform placement;

    /** Paint order. What comes later lies on top. */
    int layer = 0;

    // ── Text ──────────────────────────────────────────────────────────────
    QString text;
    QString fontResource;
    double fontSize = 0.0;

    // ── Image and drawing ─────────────────────────────────────────────────
    QString resourceName;
    QSize pixelSize;

    // ── Path ──────────────────────────────────────────────────────────────
    /** Approximate, in display points. Good enough to click on; drawing stays the renderer's job. */
    QPainterPath outline;
    QColor fill;
    QColor stroke;
    double lineWidth = 1.0;
};

/** One change to one object, as data rather than as a side effect. */
struct PageEdit {
    enum class Kind {
        Transform, //!< Move, scale, rotate, shear, flip: all one matrix
        Delete,
        Restyle,
    };

    Kind kind = Kind::Transform;
    int page = 0;
    int object = -1;

    /**
     * For Transform, in display points: what should happen to the object.
     *
     * The same space @ref PageObject::bounds is measured in, so a matrix built
     * out of boundaries this handed out means what it looks like it means. On a
     * sheet the file turns with /Rotate that is not the page's own space, and
     * the composer carries it across.
     */
    QTransform transform;

    QColor fill; //!< Restyle; an invalid colour leaves it alone
    QColor stroke;
    double lineWidth = -1.0; //!< negative leaves it alone
};

/** Something new to put on a page. */
struct NewContent {
    enum class Kind { Text, Image, Rectangle, Ellipse, Line };

    Kind kind = Kind::Rectangle;
    int page = 0;

    /** Where it goes, in display points. */
    QRectF rect;

    /**
     * Which way round it goes, in display points, applied after @ref rect.
     *
     * A rectangle says where a thing is but not how it is turned, and a turn
     * folded into a rectangle instead is no turn at all: the box round a picture
     * on the slant is upright and larger than the picture, so mapping one onto
     * the other squares the picture up and grows it. The two together say it
     * properly: the content is laid out in @ref rect and then put through this.
     *
     * The identity, which is what an upright box leaves here, is the ordinary
     * case and writes nothing extra into the file.
     */
    QTransform placement;

    QString text;
    QString fontFamily = QStringLiteral("Helvetica");
    double fontSize = 11.0;

    QImage image;
    int jpegQuality = -1; //!< negative embeds losslessly

    QColor fill;
    QColor stroke = QColor(Qt::black);
    double lineWidth = 1.0;
    double opacity = 1.0;
};

/**
 * Reading a page as objects, and writing the changes back.
 *
 * The rule that shapes everything here: **editing rewrites the instruction
 * stream, it does not rebuild the document.** A moved picture becomes
 * `q <matrix> … Q` around the operators that already drew it. A deleted object is
 * operators left out. Anything untouched keeps its bytes, which is why moving one
 * caption on a scanned page does not re-encode the scan.
 *
 * A transform is given in **display space**, which is how a person thinks:
 * "twenty points to the right", of a sheet as they are looking at it. Two things
 * are then done to it on the way in, and both have to be right. It is carried
 * into the page's own space, which is a quarter turn away on anything the file
 * turns with /Rotate; and inside the stream it has to be applied before the
 * matrix already in force, so it is conjugated by that matrix too. Getting the
 * second the wrong way round moves things by an amount that depends on their own
 * scaling, which looks like a mystery rather than a bug; getting the first wrong
 * is invisible until somebody edits a turned scan.
 */
class PageObjects
{
public:
    /** Everything on @p page, in the order the page draws it. */
    static QVector<PageObject> read(const QString &pdf, int page, QString *error);

    /** Every page, for a document-wide inventory. */
    static QVector<PageObject> readAll(const QString &pdf, QString *error);

    /** The object at @p where, topmost first, or -1. */
    static int hitTest(const QVector<PageObject> &objects, const QPointF &where);

    /** Every object whose bounds meet @p area. */
    static QVector<int> within(const QVector<PageObject> &objects, const QRectF &area);

    static QString describe(PageObject::Kind kind);

    static QStringList limitations();
};

/** Applies edits and insertions to a document. */
class PageComposer
{
public:
    struct Report {
        int transformed = 0;
        int deleted = 0;
        int restyled = 0;
        int inserted = 0;

        /** What could not be done, and why, in words a person can act on. */
        QStringList refusals;
    };

    /**
     * What the composed page is for, beyond the changes themselves.
     *
     * Both settings exist for one caller: a layer that wants to know what a
     * single object contributes to the page, so that it can draw that object
     * somewhere else while the reader is dragging it. A renderer hands out an
     * opaque sheet and never says which pixels came from which operator, so the
     * only way to ask is to compose a page that holds nothing but that object
     * and to render it twice over two known colours. What is left over from the
     * two answers is the object's own ink, coverage and all.
     */
    struct Options {
        /**
         * A colour laid behind everything the page draws.
         *
         * Written before the page's own content rather than after it, which is
         * the whole point: an insertion goes on top and would hide the page.
         * An invalid colour leaves the page's own background alone, which is
         * what every ordinary edit wants.
         */
        QColor backdrop;

        /**
         * Whether the pages keep their annotations.
         *
         * A comment or a form field is not part of the content stream, so
         * leaving out every painting operator does not leave it out. A page
         * composed to hold one object would still arrive carrying the sheet's
         * markup, and that markup would then be lifted along with the object.
         */
        bool annotations = true;
    };

    static bool apply(const QString &inputPdf, const QString &outputPdf, const QVector<PageEdit> &edits,
                      const QVector<NewContent> &insertions, Report *report, QString *error);

    static bool apply(const QString &inputPdf, const QString &outputPdf, const QVector<PageEdit> &edits,
                      const QVector<NewContent> &insertions, const Options &options, Report *report, QString *error);

    static QStringList limitations();
};

} // namespace ps
