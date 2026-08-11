/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#include "ui/Alignment.h"

#include <QTest>

using namespace ps;

namespace {

/** A rectangle written out the way a failing case needs to read. */
QString said(const QRectF &box)
{
    return QStringLiteral("x %1 y %2, %3 × %4").arg(box.x()).arg(box.y()).arg(box.width()).arg(box.height());
}

bool sameBox(const QRectF &had, const QRectF &wanted)
{
    // Every number in this file is worked out by hand and comes out of halves
    // and quarters, so the arithmetic is exact; the tolerance is here only so
    // that a failure reports the rectangle rather than the last bit of a double.
    constexpr double tolerance = 1e-9;
    return qAbs(had.x() - wanted.x()) < tolerance && qAbs(had.y() - wanted.y()) < tolerance
        && qAbs(had.width() - wanted.width()) < tolerance && qAbs(had.height() - wanted.height()) < tolerance;
}

} // namespace

#define COMPARE_BOX(had, wanted)                                                                                       \
    QVERIFY2(sameBox((had), (wanted)), qPrintable(QStringLiteral("got %1, wanted %2").arg(said(had), said(wanted))))

/**
 * The arithmetic behind the alignment buttons, on rectangles alone.
 *
 * Every expected number here was worked out on paper from the boxes above it,
 * never read back out of the code: a test that records what the function
 * happens to do would have passed just as happily while "top" meant the smaller
 * y, which is the mistake this whole space invites. PDF points, origin bottom
 * left, so the top of a rectangle is y + height.
 */
class TestAlignment : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void findsTheBoxRoundThemAll();
    void alignsOntoEachOfTheSixEdges();
    void knowsWhichWayIsUp();
    void leavesFewerThanTwoAlone();
    void evensTheGapsAndNotTheCentres();
    void spacesUpwardsTheSameWay();
    void leavesFewerThanThreeAlone();
    void growsToTheLargestKeepingTheTopLeft();
};

// Three rectangles that share no edge and no size, so that every one of the six
// alignments moves a different two of them.
//
//   A   x 100…300   y 700…750
//   B   x 150…250   y 500…600
//   C   x 400…460   y 620…650
//
// which encloses x 100…460 and y 500…750.
static const QVector<QRectF> Scattered = {
    QRectF(100, 700, 200, 50),
    QRectF(150, 500, 100, 100),
    QRectF(400, 620, 60, 30),
};

void TestAlignment::findsTheBoxRoundThemAll()
{
    COMPARE_BOX(Alignment::enclosing(Scattered), QRectF(100, 500, 360, 250));

    QVERIFY(Alignment::enclosing({}).isNull());
    COMPARE_BOX(Alignment::enclosing({ QRectF(20, 30, 40, 50) }), QRectF(20, 30, 40, 50));

    // A rectangle of no size at all still counts towards where the group is.
    // Qt calls that rectangle null and QRectF::united() drops it, so the answer
    // would otherwise start at the wrong corner for a selection holding an empty
    // path or a field never dragged out.
    COMPARE_BOX(Alignment::enclosing({ QRectF(200, 400, 10, 10), QRectF(50, 800, 0, 0) }), QRectF(50, 400, 160, 400));
}

void TestAlignment::alignsOntoEachOfTheSixEdges()
{
    const QVector<QRectF> left = Alignment::align(Scattered, Alignment::Edge::Left);
    COMPARE_BOX(left.at(0), QRectF(100, 700, 200, 50)); // already leftmost
    COMPARE_BOX(left.at(1), QRectF(100, 500, 100, 100));
    COMPARE_BOX(left.at(2), QRectF(100, 620, 60, 30));

    // 280 is the middle of 100…460; each box starts half its width short of it.
    const QVector<QRectF> centre = Alignment::align(Scattered, Alignment::Edge::HorizontalCentre);
    COMPARE_BOX(centre.at(0), QRectF(180, 700, 200, 50));
    COMPARE_BOX(centre.at(1), QRectF(230, 500, 100, 100));
    COMPARE_BOX(centre.at(2), QRectF(250, 620, 60, 30));

    const QVector<QRectF> right = Alignment::align(Scattered, Alignment::Edge::Right);
    COMPARE_BOX(right.at(0), QRectF(260, 700, 200, 50));
    COMPARE_BOX(right.at(1), QRectF(360, 500, 100, 100));
    COMPARE_BOX(right.at(2), QRectF(400, 620, 60, 30)); // already rightmost

    // The top of the group is y 750, so each box is placed its own height below
    // it, which is the arithmetic that would be a subtraction the other way round on a
    // screen, where y grows downwards.
    const QVector<QRectF> top = Alignment::align(Scattered, Alignment::Edge::Top);
    COMPARE_BOX(top.at(0), QRectF(100, 700, 200, 50)); // already topmost
    COMPARE_BOX(top.at(1), QRectF(150, 650, 100, 100));
    COMPARE_BOX(top.at(2), QRectF(400, 720, 60, 30));

    // 625 is the middle of 500…750.
    const QVector<QRectF> middle = Alignment::align(Scattered, Alignment::Edge::VerticalMiddle);
    COMPARE_BOX(middle.at(0), QRectF(100, 600, 200, 50));
    COMPARE_BOX(middle.at(1), QRectF(150, 575, 100, 100));
    COMPARE_BOX(middle.at(2), QRectF(400, 610, 60, 30));

    const QVector<QRectF> bottom = Alignment::align(Scattered, Alignment::Edge::Bottom);
    COMPARE_BOX(bottom.at(0), QRectF(100, 500, 200, 50));
    COMPARE_BOX(bottom.at(1), QRectF(150, 500, 100, 100)); // already lowest
    COMPARE_BOX(bottom.at(2), QRectF(400, 500, 60, 30));

    // Nothing is resized by any of them, and nothing is reordered.
    for (const Alignment::Edge edge : { Alignment::Edge::Left, Alignment::Edge::HorizontalCentre,
                                        Alignment::Edge::Right, Alignment::Edge::Top, Alignment::Edge::VerticalMiddle,
                                        Alignment::Edge::Bottom }) {
        const QVector<QRectF> moved = Alignment::align(Scattered, edge);
        QCOMPARE(moved.size(), Scattered.size());
        for (qsizetype at = 0; at < moved.size(); ++at) {
            QCOMPARE(moved.at(at).size(), Scattered.at(at).size());
        }
    }
}

void TestAlignment::knowsWhichWayIsUp()
{
    // Said again as a property, because getting it backwards is the one mistake
    // here that still looks plausible: aligning to the top must put every box
    // against the higher edge of the group, and must not put any of them on the
    // lower one.
    const QVector<QRectF> top = Alignment::align(Scattered, Alignment::Edge::Top);
    for (const QRectF &box : top) {
        QCOMPARE(box.y() + box.height(), 750.0);
    }

    const QVector<QRectF> bottom = Alignment::align(Scattered, Alignment::Edge::Bottom);
    for (const QRectF &box : bottom) {
        QCOMPARE(box.y(), 500.0);
    }
}

void TestAlignment::leavesFewerThanTwoAlone()
{
    const QVector<QRectF> one = { QRectF(11, 22, 33, 44) };
    for (const Alignment::Edge edge : { Alignment::Edge::Left, Alignment::Edge::HorizontalCentre,
                                        Alignment::Edge::Right, Alignment::Edge::Top, Alignment::Edge::VerticalMiddle,
                                        Alignment::Edge::Bottom }) {
        QCOMPARE(Alignment::align(one, edge), one);
        QCOMPARE(Alignment::align(QVector<QRectF>(), edge), QVector<QRectF>());
    }

    // The same rule for matching sizes: the largest of one is itself.
    for (const Alignment::Match what : { Alignment::Match::Width, Alignment::Match::Height, Alignment::Match::Both }) {
        QCOMPARE(Alignment::matchSize(one, what), one);
        QCOMPARE(Alignment::matchSize(QVector<QRectF>(), what), QVector<QRectF>());
    }
}

void TestAlignment::evensTheGapsAndNotTheCentres()
{
    // Three of markedly different widths, handed over out of order so that the
    // answer has to come back in the order it was given.
    //
    //   wide    x 0…100
    //   narrow  x 150…170
    //   middle  x 300…350
    //
    // 350 points of span hold 170 points of rectangle, so the 180 left over is
    // two gaps of 90: 0…100, gap, 190…210, gap, 300…350.
    const QVector<QRectF> given = {
        QRectF(300, 40, 50, 10), // middle
        QRectF(0, 40, 100, 10), // wide
        QRectF(150, 40, 20, 10), // narrow
    };

    const QVector<QRectF> spread = Alignment::distribute(given, Alignment::Spread::Horizontally);
    QCOMPARE(spread.size(), 3);
    COMPARE_BOX(spread.at(0), QRectF(300, 40, 50, 10)); // outermost, stays
    COMPARE_BOX(spread.at(1), QRectF(0, 40, 100, 10)); // outermost, stays
    COMPARE_BOX(spread.at(2), QRectF(190, 40, 20, 10));

    // The gaps are what came out equal…
    QCOMPARE(spread.at(2).x() - (spread.at(1).x() + spread.at(1).width()), 90.0);
    QCOMPARE(spread.at(0).x() - (spread.at(2).x() + spread.at(2).width()), 90.0);

    // …and the centres are what did not, which is the whole difference. Equal
    // centres would have put the narrow one at 175, not 190.
    QCOMPARE(spread.at(2).center().x(), 200.0);
    QVERIFY(qAbs(spread.at(2).center().x() - 175.0) > 1.0);

    // Nothing was resized and nothing crossed the axis it was not spread along.
    for (qsizetype at = 0; at < spread.size(); ++at) {
        QCOMPARE(spread.at(at).size(), given.at(at).size());
        QCOMPARE(spread.at(at).y(), given.at(at).y());
    }
}

void TestAlignment::spacesUpwardsTheSameWay()
{
    // The same numbers stood on end: y 0…100, y 150…170, y 300…350, so the
    // short one belongs at y 190 and the outermost two do not move.
    const QVector<QRectF> given = {
        QRectF(70, 0, 10, 100),
        QRectF(70, 150, 10, 20),
        QRectF(70, 300, 10, 50),
    };

    const QVector<QRectF> spread = Alignment::distribute(given, Alignment::Spread::Vertically);
    COMPARE_BOX(spread.at(0), QRectF(70, 0, 10, 100));
    COMPARE_BOX(spread.at(1), QRectF(70, 190, 10, 20));
    COMPARE_BOX(spread.at(2), QRectF(70, 300, 10, 50));

    QCOMPARE(spread.at(1).y() - (spread.at(0).y() + spread.at(0).height()), 90.0);
    QCOMPARE(spread.at(2).y() - (spread.at(1).y() + spread.at(1).height()), 90.0);
}

void TestAlignment::leavesFewerThanThreeAlone()
{
    const QVector<QRectF> two = { QRectF(0, 0, 10, 10), QRectF(500, 500, 20, 20) };
    const QVector<QRectF> one = { QRectF(11, 22, 33, 44) };

    for (const Alignment::Spread spread : { Alignment::Spread::Horizontally, Alignment::Spread::Vertically }) {
        QCOMPARE(Alignment::distribute(two, spread), two);
        QCOMPARE(Alignment::distribute(one, spread), one);
        QCOMPARE(Alignment::distribute(QVector<QRectF>(), spread), QVector<QRectF>());
    }
}

void TestAlignment::growsToTheLargestKeepingTheTopLeft()
{
    // No single rectangle is the biggest: the widest is 100 and the tallest is
    // 60, and they are not the same one. Tops are y 120, y 360 and y 60.
    const QVector<QRectF> given = {
        QRectF(10, 100, 40, 20),
        QRectF(200, 300, 100, 60),
        QRectF(400, 50, 70, 10),
    };

    const QVector<QRectF> wider = Alignment::matchSize(given, Alignment::Match::Width);
    COMPARE_BOX(wider.at(0), QRectF(10, 100, 100, 20));
    COMPARE_BOX(wider.at(1), QRectF(200, 300, 100, 60));
    COMPARE_BOX(wider.at(2), QRectF(400, 50, 100, 10));

    // Growing taller drops the bottom edge, because it is the top that is held:
    // 120 − 60 = 60 and 60 − 60 = 0.
    const QVector<QRectF> taller = Alignment::matchSize(given, Alignment::Match::Height);
    COMPARE_BOX(taller.at(0), QRectF(10, 60, 40, 60));
    COMPARE_BOX(taller.at(1), QRectF(200, 300, 100, 60));
    COMPARE_BOX(taller.at(2), QRectF(400, 0, 70, 60));

    const QVector<QRectF> both = Alignment::matchSize(given, Alignment::Match::Both);
    COMPARE_BOX(both.at(0), QRectF(10, 60, 100, 60));
    COMPARE_BOX(both.at(1), QRectF(200, 300, 100, 60));
    COMPARE_BOX(both.at(2), QRectF(400, 0, 100, 60));

    // Nothing was made smaller and nothing was moved sideways, in any of them.
    for (const QVector<QRectF> &answer : { wider, taller, both }) {
        for (qsizetype at = 0; at < answer.size(); ++at) {
            QCOMPARE(answer.at(at).x(), given.at(at).x());
            QCOMPARE(answer.at(at).y() + answer.at(at).height(), given.at(at).y() + given.at(at).height());
            QVERIFY(answer.at(at).width() >= given.at(at).width());
            QVERIFY(answer.at(at).height() >= given.at(at).height());
        }
    }
}

QTEST_MAIN(TestAlignment)

#include "tst_alignment.moc"
