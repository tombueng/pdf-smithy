/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#pragma once

#include <QRectF>
#include <QVector>

namespace ps {

/**
 * Lining things up and spacing them out, on rectangles alone.
 *
 * Two overlays need this (form fields and page objects) and they have almost
 * nothing else in common: one edits a field dictionary, the other rewrites an
 * instruction stream. What they share is the arithmetic, so that is all this
 * is: rectangles in, rectangles out, no notion of what they belong to.
 *
 * Everything here is in PDF points with the origin at the **bottom left**, the
 * same space the overlays speak. "Top" therefore means the larger y, which is
 * the one thing in this header worth reading twice.
 */
namespace Alignment {

enum class Edge {
    Left,
    HorizontalCentre,
    Right,
    Top,
    VerticalMiddle,
    Bottom,
};

enum class Spread {
    Horizontally, //!< equal gaps left to right
    Vertically, //!< equal gaps bottom to top
};

enum class Match {
    Width,
    Height,
    Both,
};

/**
 * Moves each rectangle onto the same @p edge, and returns the new set.
 *
 * The edge is taken from the enclosing box rather than from the first or the
 * last rectangle, because "align left" on a selection made by dragging a band
 * has no first, and whichever the program picked would look arbitrary half the
 * time. Nothing is resized.
 *
 * Fewer than two rectangles come back unchanged: one thing is already aligned
 * with itself, and saying so with a refusal would be noise.
 */
QVector<QRectF> align(const QVector<QRectF> &boxes, Edge edge);

/**
 * Puts equal gaps between the rectangles along @p spread.
 *
 * The two outermost stay where they are and everything between them moves, so
 * that distributing does not quietly change how much of the page the group
 * covers. Measured between edges, not between centres: equal *gaps* is what
 * people mean and what looks right when the things are different sizes.
 *
 * Fewer than three rectangles come back unchanged: with two there is nothing
 * between them to space out.
 */
QVector<QRectF> distribute(const QVector<QRectF> &boxes, Spread spread);

/**
 * Gives every rectangle the size of the largest, keeping each one's top left.
 *
 * The largest rather than the first, for the same reason align() takes the
 * enclosing box: a band selection has no first. Growing rather than shrinking
 * because a field made too small loses its text, while one made too big only
 * looks generous.
 */
QVector<QRectF> matchSize(const QVector<QRectF> &boxes, Match what);

/** The smallest box holding all of them; a null rectangle when empty. */
QRectF enclosing(const QVector<QRectF> &boxes);

} // namespace Alignment

} // namespace ps
