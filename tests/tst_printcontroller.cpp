/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "TestPdf.h"

#include "core/Document.h"
#include "print/PrintController.h"

#include <QTemporaryDir>
#include <QTest>

#include <KLocalizedString>

using namespace ps;

/**
 * Imposition is the part of printing that is easy to get subtly wrong and
 * expensive to discover on paper, so it is tested without a printer anywhere
 * in sight.
 */
class TestPrintController : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void init();

    void countsPagesPerSheet_data();
    void countsPagesPerSheet();

    void normalLayoutKeepsOrder();
    void honoursPageRange();
    void reversesOrder();
    void splitsOddAndEvenForManualDuplex();

    void padsIncompleteGrids();
    void countsSheets_data();
    void countsSheets();

    void ordersAFourPageBooklet();
    void ordersAnEightPageBooklet();
    void padsBookletToMultipleOfFour();

    void refusesEmptyDocument();
    void reportsBadRange();

private:
    QTemporaryDir m_dir;
    QString m_file;
    Document m_document;
    PrintController m_controller;
};

void TestPrintController::initTestCase()
{
    KLocalizedString::setApplicationDomain("pdf-smithy");
    QVERIFY(m_dir.isValid());
    m_file = m_dir.filePath(QStringLiteral("ten.pdf"));
    QVERIFY(test::writeSamplePdf(m_file, 10));
}

void TestPrintController::init()
{
    QVERIFY(m_document.open(m_file, nullptr));
    m_controller.setDocument(&m_document);
}

void TestPrintController::countsPagesPerSheet_data()
{
    QTest::addColumn<PrintController::Layout>("layout");
    QTest::addColumn<int>("expected");

    QTest::newRow("normal") << PrintController::Layout::Normal << 1;
    QTest::newRow("two up") << PrintController::Layout::TwoUp << 2;
    QTest::newRow("four up") << PrintController::Layout::FourUp << 4;
    QTest::newRow("six up") << PrintController::Layout::SixUp << 6;
    QTest::newRow("nine up") << PrintController::Layout::NineUp << 9;
    QTest::newRow("sixteen up") << PrintController::Layout::SixteenUp << 16;
    QTest::newRow("booklet") << PrintController::Layout::Booklet << 2;
}

void TestPrintController::countsPagesPerSheet()
{
    QFETCH(PrintController::Layout, layout);
    QFETCH(int, expected);
    QCOMPARE(PrintController::pagesPerSheet(layout), expected);
    QVERIFY(!PrintController::layoutName(layout).isEmpty());
}

void TestPrintController::normalLayoutKeepsOrder()
{
    PrintController::Options options;
    const QVector<int> imposed = m_controller.impose(options, nullptr);
    QCOMPARE(imposed, (QVector<int> { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 }));
}

void TestPrintController::honoursPageRange()
{
    PrintController::Options options;
    options.range = QStringLiteral("2-4,9");
    QCOMPARE(m_controller.impose(options, nullptr), (QVector<int> { 1, 2, 3, 8 }));
}

void TestPrintController::reversesOrder()
{
    PrintController::Options options;
    options.range = QStringLiteral("1-4");
    options.reverseOrder = true;
    // Printers that stack face-up need the job back to front.
    QCOMPARE(m_controller.impose(options, nullptr), (QVector<int> { 3, 2, 1, 0 }));
}

void TestPrintController::splitsOddAndEvenForManualDuplex()
{
    PrintController::Options odd;
    odd.range = QStringLiteral("1-6");
    odd.subset = PrintController::Subset::OddOnly;

    PrintController::Options even = odd;
    even.subset = PrintController::Subset::EvenOnly;

    const QVector<int> first = m_controller.impose(odd, nullptr);
    const QVector<int> second = m_controller.impose(even, nullptr);

    QCOMPARE(first, (QVector<int> { 0, 2, 4 }));
    QCOMPARE(second, (QVector<int> { 1, 3, 5 }));

    // Together the two passes must cover the job exactly once: a page printed
    // twice or not at all is the whole failure mode of manual duplex.
    QVector<int> combined = first + second;
    std::sort(combined.begin(), combined.end());
    QCOMPARE(combined, (QVector<int> { 0, 1, 2, 3, 4, 5 }));
}

void TestPrintController::padsIncompleteGrids()
{
    PrintController::Options options;
    options.layout = PrintController::Layout::FourUp;
    options.range = QStringLiteral("1-6");

    const QVector<int> imposed = m_controller.impose(options, nullptr);
    QCOMPARE(imposed.size(), 8);
    // The two trailing slots are blank rather than wrapping the next sheet's
    // pages onto this one.
    QCOMPARE(imposed.at(6), -1);
    QCOMPARE(imposed.at(7), -1);
}

void TestPrintController::countsSheets_data()
{
    QTest::addColumn<PrintController::Layout>("layout");
    QTest::addColumn<QString>("range");
    QTest::addColumn<int>("expected");

    QTest::newRow("ten pages, one up") << PrintController::Layout::Normal << QString() << 10;
    QTest::newRow("ten pages, two up") << PrintController::Layout::TwoUp << QString() << 5;
    QTest::newRow("ten pages, four up") << PrintController::Layout::FourUp << QString() << 3;
    QTest::newRow("six pages, four up") << PrintController::Layout::FourUp << QStringLiteral("1-6") << 2;
    QTest::newRow("ten pages, booklet") << PrintController::Layout::Booklet << QString() << 6;
}

void TestPrintController::countsSheets()
{
    QFETCH(PrintController::Layout, layout);
    QFETCH(QString, range);
    QFETCH(int, expected);

    PrintController::Options options;
    options.layout = layout;
    options.range = range;
    QCOMPARE(m_controller.sheetCount(options), expected);
}

void TestPrintController::ordersAFourPageBooklet()
{
    PrintController::Options options;
    options.layout = PrintController::Layout::Booklet;
    options.range = QStringLiteral("1-4");

    // Folded once: the outside sheet carries pages 4 and 1, the inside 2 and 3.
    QCOMPARE(m_controller.impose(options, nullptr), (QVector<int> { 3, 0, 1, 2 }));
}

void TestPrintController::ordersAnEightPageBooklet()
{
    PrintController::Options options;
    options.layout = PrintController::Layout::Booklet;
    options.range = QStringLiteral("1-8");

    // Outer sheet: 8|1 then 2|7. Inner sheet: 6|3 then 4|5.
    QCOMPARE(m_controller.impose(options, nullptr), (QVector<int> { 7, 0, 1, 6, 5, 2, 3, 4 }));
}

void TestPrintController::padsBookletToMultipleOfFour()
{
    PrintController::Options options;
    options.layout = PrintController::Layout::Booklet;
    options.range = QStringLiteral("1-5");

    const QVector<int> imposed = m_controller.impose(options, nullptr);
    // Five pages need two folded sheets, so eight slots with three blanks.
    QCOMPARE(imposed.size(), 8);
    QCOMPARE(imposed.count(-1), 3);
    // Every real page still appears exactly once.
    for (int page = 0; page < 5; ++page) {
        QCOMPARE(imposed.count(page), 1);
    }
}

void TestPrintController::refusesEmptyDocument()
{
    Document empty;
    PrintController controller;
    controller.setDocument(&empty);

    QString error;
    QVERIFY(controller.impose(PrintController::Options {}, &error).isEmpty());
    QVERIFY(!error.isEmpty());
    QCOMPARE(controller.sheetCount(PrintController::Options {}), 0);
}

void TestPrintController::reportsBadRange()
{
    PrintController::Options options;
    options.range = QStringLiteral("50-60");

    QString error;
    QVERIFY(m_controller.impose(options, &error).isEmpty());
    QVERIFY(!error.isEmpty());
}

QTEST_MAIN(TestPrintController)

#include "tst_printcontroller.moc"
