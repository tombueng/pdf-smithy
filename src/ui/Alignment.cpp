/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "Alignment.h"

#include <algorithm>
#include <numeric>

namespace ps {

namespace {

/**
 * How far along the axis a rectangle starts, and how much of it it covers.
 *
 * Along y, "starts" is the bottom: the page's origin is its bottom left, so the
 * smaller y comes first in exactly the way the smaller x does. Naming them
 * start and extent rather than borrowing QRectF::top() keeps that straight:
 * QRectF calls the smaller y the top, which is the opposite of what a page
 * means by the word.
 */
double startOf(const QRectF &box, bool horizontally)
{
    return horizontally ? box.x() : box.y();
}

double extentOf(const QRectF &box, bool horizontally)
{
    return horizontally ? box.width() : box.height();
}

} // namespace

QRectF Alignment::enclosing(const QVector<QRectF> &boxes)
{
    if (boxes.isEmpty()) {
        return {};
    }

    // Written out rather than folded with QRectF::united(): a rectangle of no
    // width and no height is null as far as Qt is concerned, and united()
    // quietly drops a null operand. A page object can genuinely have no size
    // (an empty path, a form field never dragged out) and dropping it would put
    // the enclosing box somewhere the user can see is wrong.
    const QRectF first = boxes.first().normalized();
    double left = first.x();
    double right = first.x() + first.width();
    double bottom = first.y();
    double top = first.y() + first.height();
    for (const QRectF &box : boxes) {
        const QRectF normal = box.normalized();
        left = std::min(left, normal.x());
        right = std::max(right, normal.x() + normal.width());
        bottom = std::min(bottom, normal.y());
        top = std::max(top, normal.y() + normal.height());
    }
    return QRectF(left, bottom, right - left, top - bottom);
}

QVector<QRectF> Alignment::align(const QVector<QRectF> &boxes, Edge edge)
{
    if (boxes.size() < 2) {
        return boxes;
    }

    const QRectF bounds = enclosing(boxes);

    QVector<QRectF> result;
    result.reserve(boxes.size());
    for (const QRectF &box : boxes) {
        // Normalised first, because a rectangle dragged out from its far corner
        // arrives with a negative width, and every line below reads width() and
        // height() as the distances they claim to be.
        const QRectF normal = box.normalized();
        double x = normal.x();
        double y = normal.y();

        switch (edge) {
        case Edge::Left:
            x = bounds.x();
            break;
        case Edge::HorizontalCentre:
            x = bounds.x() + (bounds.width() - normal.width()) / 2.0;
            break;
        case Edge::Right:
            x = bounds.x() + bounds.width() - normal.width();
            break;
        case Edge::Top:
            // y is the bottom edge and up is the direction y grows, so landing
            // on the top of the enclosing box means starting one height below
            // it. That makes Top the mirror of Right rather than of Left, which
            // is the one place this file would be wrong if it were written from
            // habit.
            y = bounds.y() + bounds.height() - normal.height();
            break;
        case Edge::VerticalMiddle:
            y = bounds.y() + (bounds.height() - normal.height()) / 2.0;
            break;
        case Edge::Bottom:
            y = bounds.y();
            break;
        }

        result.append(QRectF(x, y, normal.width(), normal.height()));
    }
    return result;
}

QVector<QRectF> Alignment::distribute(const QVector<QRectF> &boxes, Spread spread)
{
    if (boxes.size() < 3) {
        return boxes;
    }

    const bool horizontally = spread == Spread::Horizontally;

    QVector<QRectF> normal;
    normal.reserve(boxes.size());
    for (const QRectF &box : boxes) {
        normal.append(box.normalized());
    }

    // Worked through in the order they sit on the page, not the order they were
    // picked: a selection made by clicking right to left would otherwise come
    // back reversed, which reads as the program having rearranged the page.
    // Stable, so that two rectangles starting at the same place keep the order
    // they arrived in instead of swapping on every press of the button.
    QVector<qsizetype> order(boxes.size());
    std::iota(order.begin(), order.end(), qsizetype(0));
    std::stable_sort(order.begin(), order.end(), [&normal, horizontally](qsizetype left, qsizetype right) {
        return startOf(normal.at(left), horizontally) < startOf(normal.at(right), horizontally);
    });

    double occupied = 0.0;
    for (const QRectF &box : normal) {
        occupied += extentOf(box, horizontally);
    }

    const QRectF bounds = enclosing(normal);
    const double span = extentOf(bounds, horizontally);

    // What is left over once the rectangles themselves are accounted for, shared
    // between the openings. Measured between edges, so rectangles of different
    // sizes end up with gaps that look equal instead of centres that merely
    // are. Negative when they overlap more than the span can hold, and that is
    // still the answer worth giving: an even overlap is a readable one.
    const double gap = (span - occupied) / static_cast<double>(boxes.size() - 1);

    QVector<QRectF> result = normal;
    double cursor = startOf(normal.at(order.first()), horizontally);
    for (const qsizetype at : order) {
        const QRectF box = normal.at(at);
        // The first lands exactly where it already was, and the arithmetic puts
        // the last against the far side of the same span, so the group covers
        // the same part of the page afterwards as before.
        result[at] = horizontally ? QRectF(cursor, box.y(), box.width(), box.height())
                                  : QRectF(box.x(), cursor, box.width(), box.height());
        cursor += extentOf(box, horizontally) + gap;
    }
    return result;
}

QVector<QRectF> Alignment::matchSize(const QVector<QRectF> &boxes, Match what)
{
    if (boxes.size() < 2) {
        return boxes;
    }

    // The widest and the tallest are looked for separately rather than taken
    // from whichever single rectangle is biggest overall. One tall column and
    // one wide banner have no biggest between them, and picking either would
    // shrink the other, which is the one outcome the header rules out, because
    // a field made too small loses the text inside it.
    double widest = 0.0;
    double tallest = 0.0;
    for (const QRectF &box : boxes) {
        const QRectF normal = box.normalized();
        widest = std::max(widest, normal.width());
        tallest = std::max(tallest, normal.height());
    }

    QVector<QRectF> result;
    result.reserve(boxes.size());
    for (const QRectF &box : boxes) {
        const QRectF normal = box.normalized();
        const double width = what == Match::Height ? normal.width() : widest;
        const double height = what == Match::Width ? normal.height() : tallest;

        // The corner that stays is the top left, and the top is the larger y, so
        // the extra height comes off the bottom. Holding the stored corner
        // instead would lift a row of matched fields off the line they were
        // already sitting on.
        const double y = normal.y() + normal.height() - height;
        result.append(QRectF(normal.x(), y, width, height));
    }
    return result;
}

} // namespace ps
