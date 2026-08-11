/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "TestPdf.h"

#include "core/Document.h"
#include "core/DocumentWriter.h"
#include "core/PageObjects.h"
#include "render/PopplerBackend.h"
#include "ui/PageRows.h"

#include <KLocalizedString>

#include <qpdf/QPDF.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>
#include <qpdf/QPDFWriter.hh>

#include <QImage>
#include <QTemporaryDir>
#include <QTest>

using namespace ps;

namespace {

/** The mark on the fixture page, in display points on the upright sheet. */
const QRectF MarkPoints(20.0, 20.0, 30.0, 60.0);

/** The upright fixture page, in points. Portrait, so a swap cannot hide. */
const QSizeF SheetPoints(200.0, 400.0);

/**
 * The turn written the other way round.
 *
 * Kept so that the measurement can say which of the two signs the paper agrees
 * with rather than merely that one of them fits. It is also what every overlay
 * in this program used to do, and it passes at a half turn, which is precisely
 * why nobody noticed for so long.
 */
QTransform theOtherWayRound(int rotation, const QSizeF &shown)
{
    const int degrees = normalizeRotation(rotation);
    if (degrees == 0 || shown.isEmpty()) {
        return {};
    }
    const QSizeF source = degrees % 180 == 0 ? shown : QSizeF(shown.height(), shown.width());
    switch (degrees) {
    case 90:
        return QTransform(0, 1, -1, 0, source.height(), 0);
    case 180:
        return QTransform(-1, 0, 0, -1, source.width(), source.height());
    case 270:
        return QTransform(0, -1, 1, 0, 0, source.width());
    default:
        return {};
    }
}

bool sameBox(const QRectF &a, const QRectF &b, double slop)
{
    return qAbs(a.left() - b.left()) < slop && qAbs(a.top() - b.top()) < slop && qAbs(a.width() - b.width()) < slop
        && qAbs(a.height() - b.height()) < slop;
}

QString describe(const QRectF &r)
{
    return QStringLiteral("%1,%2 %3x%4").arg(r.left()).arg(r.top()).arg(r.width()).arg(r.height());
}

} // namespace

/**
 * Which way a page turns, measured on paper rather than argued from matrices.
 *
 * Every overlay in the editor holds work that was measured before the organiser
 * turned the page under it, and has to move that work along with the sheet. The
 * arithmetic for it was written five separate times, and the two versions that
 * resulted disagreed by a sign, a disagreement invisible at a half turn, which
 * is its own inverse, and wrong by a hundred and eighty degrees at a quarter.
 *
 * So nothing here is checked against a transform. A black mark is put in one
 * corner of an upright sheet, the sheet is turned through the real organiser and
 * written out through the real writer, and Poppler (which knows what PDF
 * /Rotate means and knows nothing about this program) is asked to draw the
 * result. Where the ink actually lands decides the sign.
 */
class TestPageTurn : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();

    void inkLandsWhereAClockwiseTurnPutsIt_data();
    void inkLandsWhereAClockwiseTurnPutsIt();

    void theSharedTurnPredictsTheInk_data();
    void theSharedTurnPredictsTheInk();

    void theOppositeSignOnlySurvivesTheHalfTurn_data();
    void theOppositeSignOnlySurvivesTheHalfTurn();

    void theEngineReadsTheMarkWhereTheTurnPutIt_data();
    void theEngineReadsTheMarkWhereTheTurnPutIt();

    void aTurnArrivingWithARemovalStillFollowsThePages();

private:
    /** An upright sheet with one black mark near its bottom left corner. */
    QString markedSheet();

    /** @p turn degrees through Document and DocumentWriter, as a written file. */
    QString turnedFile(int turn);

    /** The mark's box in display points of @p path, found by looking at pixels. */
    QRectF inkOf(const QString &path, const QSizeF &shown);

    QTemporaryDir m_dir;
    QString m_sheet;
};

void TestPageTurn::initTestCase()
{
    KLocalizedString::setApplicationDomain("pdf-smithy");
    QVERIFY(m_dir.isValid());
    m_sheet = markedSheet();
    QVERIFY(!m_sheet.isEmpty());
}

QString TestPageTurn::markedSheet()
{
    const QString path = m_dir.filePath(QStringLiteral("marked.pdf"));
    try {
        QPDF pdf;
        pdf.emptyPDF();

        // Taller than it is wide, so that a turn which swaps the sides wrongly
        // cannot come out looking right.
        const std::string content = "0 g\n20 20 30 60 re\nf\n";

        QPDFObjectHandle page = QPDFObjectHandle::parse("<< /Type /Page /MediaBox [0 0 200 400] /Resources << >> >>");
        page.replaceKey("/Contents", QPDFObjectHandle::newStream(&pdf, content));

        QPDFPageDocumentHelper documents(pdf);
        documents.addPage(QPDFPageObjectHelper(pdf.makeIndirectObject(page)), false);

        QPDFWriter writer(pdf, path.toUtf8().constData());
        writer.write();
        return path;
    } catch (const std::exception &) {
        return {};
    }
}

QString TestPageTurn::turnedFile(int turn)
{
    PopplerBackend backend;
    Document document;
    document.setRenderBackend(&backend);

    QString error;
    if (!document.open(m_sheet, &error)) {
        return {};
    }
    document.applyRotation({ 0 }, turn);

    const QString path = m_dir.filePath(QStringLiteral("turned%1.pdf").arg(turn));
    return DocumentWriter::write(document, path, DocumentWriter::Options(), &error) ? path : QString();
}

QRectF TestPageTurn::inkOf(const QString &path, const QSizeF &shown)
{
    PopplerBackend backend;
    QString error;
    if (!backend.addDocument(0, path, &error)) {
        return {};
    }

    // Two device pixels to the point, so a pixel of rasterising slop is half a
    // point and the tolerance below can stay far tighter than the mark is big.
    const QImage sheet = backend.renderPage(0, 0, int(shown.width() * 2.0));
    if (sheet.isNull()) {
        return {};
    }

    int left = sheet.width();
    int right = -1;
    int top = sheet.height();
    int bottom = -1;
    for (int y = 0; y < sheet.height(); ++y) {
        for (int x = 0; x < sheet.width(); ++x) {
            if (qGray(sheet.pixel(x, y)) < 128) {
                left = qMin(left, x);
                right = qMax(right, x);
                top = qMin(top, y);
                bottom = qMax(bottom, y);
            }
        }
    }
    if (right < 0) {
        return {};
    }

    // Pixels run down the sheet and display points run up it.
    const double perX = shown.width() / sheet.width();
    const double perY = shown.height() / sheet.height();
    return QRectF(QPointF(left * perX, shown.height() - (bottom + 1) * perY),
                  QPointF((right + 1) * perX, shown.height() - top * perY));
}

void TestPageTurn::inkLandsWhereAClockwiseTurnPutsIt_data()
{
    QTest::addColumn<int>("turn");
    QTest::addColumn<QSizeF>("shown");
    QTest::addColumn<QRectF>("expected");

    // Worked out by hand from what /Rotate means (the page is turned clockwise
    // when it is displayed) and from nothing in this program. A mark in the
    // bottom left of an upright sheet goes to the top left of one turned a
    // quarter clockwise, the top right of one turned a half, the bottom right
    // of one turned three quarters. The mark's own sides swap with it.
    QTest::newRow("upright") << 0 << SheetPoints << QRectF(20, 20, 30, 60);
    QTest::newRow("quarter") << 90 << QSizeF(400, 200) << QRectF(20, 150, 60, 30);
    QTest::newRow("half") << 180 << SheetPoints << QRectF(150, 320, 30, 60);
    QTest::newRow("three quarters") << 270 << QSizeF(400, 200) << QRectF(320, 20, 60, 30);
}

void TestPageTurn::inkLandsWhereAClockwiseTurnPutsIt()
{
    QFETCH(int, turn);
    QFETCH(QSizeF, shown);
    QFETCH(QRectF, expected);

    const QString path = turnedFile(turn);
    QVERIFY(!path.isEmpty());

    const QRectF ink = inkOf(path, shown);
    QVERIFY2(!ink.isEmpty(), "the mark was not found on the rendered sheet");
    QVERIFY2(sameBox(ink, expected, 1.0),
             qPrintable(QStringLiteral("ink at %1, expected %2").arg(describe(ink), describe(expected))));
}

void TestPageTurn::theSharedTurnPredictsTheInk_data()
{
    inkLandsWhereAClockwiseTurnPutsIt_data();
}

void TestPageTurn::theSharedTurnPredictsTheInk()
{
    QFETCH(int, turn);
    QFETCH(QSizeF, shown);

    const QString path = turnedFile(turn);
    QVERIFY(!path.isEmpty());

    const QRectF ink = inkOf(path, shown);
    QVERIFY2(!ink.isEmpty(), "the mark was not found on the rendered sheet");

    const QRectF predicted = pageTurn(turn, shown).mapRect(MarkPoints);
    QVERIFY2(sameBox(predicted, ink, 1.0),
             qPrintable(QStringLiteral("pageTurn says %1, the ink is at %2").arg(describe(predicted), describe(ink))));
}

void TestPageTurn::theOppositeSignOnlySurvivesTheHalfTurn_data()
{
    inkLandsWhereAClockwiseTurnPutsIt_data();
}

void TestPageTurn::theOppositeSignOnlySurvivesTheHalfTurn()
{
    QFETCH(int, turn);
    QFETCH(QSizeF, shown);
    QFETCH(QRectF, expected);

    // What makes the sign worth a test of its own: at nothing and at a half
    // turn the two directions are the same transform, so a suite that only ever
    // turned a page upside down would have gone on agreeing with either.
    const QRectF other = theOtherWayRound(turn, shown).mapRect(MarkPoints);
    if (turn % 180 == 0) {
        QVERIFY(sameBox(other, expected, 1.0));
    } else {
        QVERIFY2(!sameBox(other, expected, 1.0),
                 qPrintable(QStringLiteral("the opposite sign also lands at %1").arg(describe(other))));
    }
}

void TestPageTurn::theEngineReadsTheMarkWhereTheTurnPutIt_data()
{
    inkLandsWhereAClockwiseTurnPutsIt_data();
}

void TestPageTurn::theEngineReadsTheMarkWhereTheTurnPutIt()
{
    QFETCH(int, turn);
    QFETCH(QSizeF, shown);
    QFETCH(QRectF, expected);

    // The other half of the claim: the reader that answers in display points
    // has to agree with the renderer about which corner the mark is in, or an
    // overlay drawn from what it says would sit at ninety degrees to the page.
    const QString path = turnedFile(turn);
    QVERIFY(!path.isEmpty());

    QString error;
    const QVector<PageObject> objects = PageObjects::read(path, 0, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(objects.size(), 1);

    Q_UNUSED(shown)
    // A shade looser than the pixel measurement: a path's reported bounds carry
    // half a line width of allowance all round that a filled rectangle never
    // uses. Half a point either way, against a mark that the wrong sign would
    // put three hundred points from here.
    const QRectF bounds = objects.constFirst().bounds;
    QVERIFY2(sameBox(bounds, expected, 1.5),
             qPrintable(QStringLiteral("read at %1, expected %2").arg(describe(bounds), describe(expected))));
}

void TestPageTurn::aTurnArrivingWithARemovalStillFollowsThePages()
{
    // The two halves of PageRows are answered at different speeds: a page list
    // that got shorter is followed from the event loop, because a move is a
    // removal and an insertion and answering the gap between them would lose
    // every page in it, while a page merely turned is answered on the spot. So
    // a turn can reach followInPlace() while the removal before it is still
    // waiting, and then the snapshot it compares against is a page list that
    // no longer exists.
    const QString many = m_dir.filePath(QStringLiteral("nine.pdf"));
    QVERIFY(test::writeSamplePdf(many, 9));

    PopplerBackend backend;
    Document document;
    document.setRenderBackend(&backend);
    QString error;
    QVERIFY2(document.open(many, &error), qPrintable(error));

    PageRows rows;
    rows.setDocument(&document);

    // One turn of the event loop, two changes: the first sheet taken out and
    // the sixth of what is left turned. Nothing runs in between.
    document.removePages({ 0 });
    document.applyRotation({ 4 }, 90);

    const Rebase change = rows.followInPlace();

    // The page that was row five is row four now, and it is the one that turned.
    QCOMPARE(change.nowAt(5), 4);
    QCOMPARE(change.turnOf(5), 90);

    // And nothing else did. A snapshot compared row for row against a shorter
    // list reports a turn on the wrong sheet and calls every row below the
    // removal a different page, which throws away everything read from them.
    for (int was = 1; was < 9; ++was) {
        if (was == 5) {
            continue;
        }
        QVERIFY2(change.turnOf(was) == 0,
                 qPrintable(
                     QStringLiteral("row %1 was turned by %2 and nobody turned it").arg(was).arg(change.turnOf(was))));
        QCOMPARE(change.nowAt(was), was - 1);
        QVERIFY2(!change.rewritten(was), qPrintable(QStringLiteral("row %1 was called a different page").arg(was)));
    }

    // The sheet that was taken out is the one thing that is gone, and saying so
    // is what lets an overlay tell the user rather than lose work quietly.
    QCOMPARE(change.nowAt(0), -1);
}

QTEST_MAIN(TestPageTurn)

#include "tst_pageturn.moc"
