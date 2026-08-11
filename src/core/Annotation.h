/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#pragma once

#include <QColor>
#include <QDateTime>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QVector>

namespace ps {

/**
 * A remark laid over a page without altering it.
 *
 * Annotations are the one kind of markup PDF has a proper place for: they live
 * beside the page rather than in its content stream, so adding, editing and
 * removing one leaves everything underneath untouched. Nothing here rewrites a
 * page.
 *
 * Coordinates are in display points with the origin at the bottom left, the
 * same convention as redaction and stamping use, and are mapped through
 * `/Rotate` on the way in.
 */
struct Annotation {
    enum class Type {
        Highlight, //!< Coloured wash over text, drawn so the words stay readable
        Underline,
        StrikeOut,
        Squiggly,
        Ink, //!< Freehand
        Square,
        Circle,
        Line,
        FreeText, //!< A box of text on the page itself
        Note, //!< A collapsed marker holding a comment
    };

    Type type = Type::Highlight;

    /** Zero-based page index. */
    int page = 0;

    /**
     * The area the annotation occupies.
     *
     * For text markup this is the union of @ref quads; it is filled in
     * automatically when left empty and quads are given.
     */
    QRectF rect;

    /** One rectangle per run of marked text. Used by the four markup types. */
    QVector<QRectF> quads;

    /** One entry per stroke of freehand drawing; also the two ends of a Line. */
    QVector<QVector<QPointF>> strokes;

    QColor colour = QColor(255, 212, 0);

    /** An invalid colour means an outline with nothing inside it. */
    QColor interior;

    double opacity = 1.0;
    double lineWidth = 2.0;
    double fontSize = 11.0;

    QString author;
    QString contents;

    QDateTime created;

    /** Read back from a file; ignored when adding. */
    QString identifier;
};

/**
 * Reading, writing and removing the annotations of a document.
 *
 * Every annotation is written with its own appearance stream. The alternative,
 * leaving readers to draw it from the properties, is allowed by the
 * specification and looks different in every viewer, which for a highlight over
 * evidence is not a cosmetic matter.
 */
class Annotations
{
public:
    /** Appends @p annotations to whatever the document already carries. */
    static bool add(const QString &inputPdf, const QString &outputPdf, const QVector<Annotation> &annotations,
                    QString *error);

    /** Everything already on the given pages, or on all of them when empty. */
    static QVector<Annotation> read(const QString &pdf, const QVector<int> &pages, QString *error);

    /**
     * Removes annotations from @p pages, or from all pages when empty.
     *
     * @p identifiers narrows it further to particular ones; empty means all of
     * them. Widgets, the fields of a form, are never touched, because losing
     * a form to a "remove comments" command would be a surprise of the worst
     * kind.
     */
    static bool remove(const QString &inputPdf, const QString &outputPdf, const QVector<int> &pages,
                       const QStringList &identifiers, int *removed, QString *error);

    /**
     * Writes the comments out as XFDF, the interchange format readers agree on.
     *
     * Sending back a marked-up copy of a contract means sending the contract;
     * sending an XFDF file means sending only the remarks. It is also the way
     * comments move between this and Acrobat without either having to
     * understand the other.
     */
    static bool exportXfdf(const QString &pdf, const QString &xfdfPath, const QVector<int> &pages,
                           const QString &sourceName, QString *error);

    /**
     * Reads an XFDF file and adds what it holds to @p inputPdf.
     *
     * Anything referring to a page the document does not have is skipped rather
     * than clamped onto the last one, because a comment in the wrong place is
     * worse than a comment that is missing and said to be.
     */
    static bool importXfdf(const QString &inputPdf, const QString &xfdfPath, const QString &outputPdf, int *added,
                           QStringList *warnings, QString *error);

    /**
     * Draws the annotations into the pages and then deletes them.
     *
     * What a comment looks like otherwise is up to the reader, and some print
     * it, some do not. Flattening ends the argument at the cost of being
     * final.
     */
    static bool flatten(const QString &inputPdf, const QString &outputPdf, const QVector<int> &pages, int *flattened,
                        QString *error);
};

} // namespace ps
