/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#include "core/PageRange.h"

#include <KLocalizedString>
#include <QTest>

using namespace ps;

class TestPageRange : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        // Without a domain, KLocalizedString warns on every message it builds
        // and floods the test output.
        KLocalizedString::setApplicationDomain("pdf-smithy");
    }

    void parses_data();
    void parses();

    void rejects_data();
    void rejects();

    void emptyMeansEverything();
    void keepsOrderAndRepetition();
    void reversesDescendingRanges();
    void takesTheWordsItOffers();

    void formats_data();
    void formats();
};

void TestPageRange::parses_data()
{
    QTest::addColumn<QString>("input");
    QTest::addColumn<int>("pageCount");
    QTest::addColumn<QVector<int>>("expected");

    QTest::newRow("single") << QStringLiteral("3") << 10 << QVector<int> { 2 };
    QTest::newRow("list") << QStringLiteral("1,3,5") << 10 << QVector<int> { 0, 2, 4 };
    QTest::newRow("range") << QStringLiteral("2-4") << 10 << QVector<int> { 1, 2, 3 };
    QTest::newRow("mixed") << QStringLiteral("1-3, 7, 9-10") << 10 << QVector<int> { 0, 1, 2, 6, 8, 9 };
    QTest::newRow("spaces tolerated") << QStringLiteral("  1 - 3 ,  7 ") << 10 << QVector<int> { 0, 1, 2, 6 };
    QTest::newRow("open end") << QStringLiteral("8-") << 10 << QVector<int> { 7, 8, 9 };
    QTest::newRow("open start") << QStringLiteral("-3") << 10 << QVector<int> { 0, 1, 2 };
    QTest::newRow("last keyword") << QStringLiteral("last") << 5 << QVector<int> { 4 };
    QTest::newRow("to last") << QStringLiteral("4-last") << 5 << QVector<int> { 3, 4 };
    QTest::newRow("odd") << QStringLiteral("odd") << 6 << QVector<int> { 0, 2, 4 };
    QTest::newRow("even") << QStringLiteral("even") << 6 << QVector<int> { 1, 3, 5 };
    QTest::newRow("german odd") << QStringLiteral("ungerade") << 5 << QVector<int> { 0, 2, 4 };
    QTest::newRow("whole document") << QStringLiteral("1-last") << 3 << QVector<int> { 0, 1, 2 };
}

void TestPageRange::parses()
{
    QFETCH(QString, input);
    QFETCH(int, pageCount);
    QFETCH(QVector<int>, expected);

    QString error;
    const QVector<int> actual = PageRange::parse(input, pageCount, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(actual, expected);
}

void TestPageRange::rejects_data()
{
    QTest::addColumn<QString>("input");
    QTest::addColumn<int>("pageCount");

    QTest::newRow("beyond the end") << QStringLiteral("11") << 10;
    QTest::newRow("range beyond the end") << QStringLiteral("5-99") << 10;
    QTest::newRow("zero is not a page") << QStringLiteral("0") << 10;
    QTest::newRow("not a number") << QStringLiteral("abc") << 10;
    QTest::newRow("garbage in a list") << QStringLiteral("1,xyz,3") << 10;
}

void TestPageRange::rejects()
{
    QFETCH(QString, input);
    QFETCH(int, pageCount);

    QString error;
    const QVector<int> actual = PageRange::parse(input, pageCount, &error);
    QVERIFY(actual.isEmpty());
    QVERIFY2(!error.isEmpty(), "a rejection must come with an explanation");
}

void TestPageRange::emptyMeansEverything()
{
    QString error;
    const QVector<int> actual = PageRange::parse(QString(), 4, &error);
    QVERIFY(error.isEmpty());
    QCOMPARE(actual, (QVector<int> { 0, 1, 2, 3 }));
}

void TestPageRange::keepsOrderAndRepetition()
{
    // "Extract pages 3, 1 and 3 again" is a real request, not a mistake to be
    // tidied away.
    QCOMPARE(PageRange::parse(QStringLiteral("3,1,3"), 5), (QVector<int> { 2, 0, 2 }));
}

void TestPageRange::reversesDescendingRanges()
{
    QCOMPARE(PageRange::parse(QStringLiteral("5-1"), 5), (QVector<int> { 4, 3, 2, 1, 0 }));
}

void TestPageRange::takesTheWordsItOffers()
{
    // The boxes in the interface print these words as the ones to type, and a
    // translation that renamed them without the parser hearing of it would put
    // a word on screen that the parser then refused. Whatever language this
    // runs in, the offered word has to come back through parse().
    QCOMPARE(PageRange::parse(PageRange::oddWord(), 6), (QVector<int> { 0, 2, 4 }));
    QCOMPARE(PageRange::parse(PageRange::evenWord(), 6), (QVector<int> { 1, 3, 5 }));
    QCOMPARE(PageRange::parse(PageRange::lastWord(), 6), (QVector<int> { 5 }));
    QCOMPARE(PageRange::parse(QStringLiteral("2-") + PageRange::lastWord(), 4), (QVector<int> { 1, 2, 3 }));

    // And the English words stay, because the handbook and people's scripts
    // are written in them.
    QCOMPARE(PageRange::parse(QStringLiteral("odd"), 6), (QVector<int> { 0, 2, 4 }));
    QCOMPARE(PageRange::parse(QStringLiteral("even"), 6), (QVector<int> { 1, 3, 5 }));
    QCOMPARE(PageRange::parse(QStringLiteral("3-last"), 4), (QVector<int> { 2, 3 }));
}

void TestPageRange::formats_data()
{
    QTest::addColumn<QVector<int>>("input");
    QTest::addColumn<QString>("expected");

    QTest::newRow("empty") << QVector<int> {} << QString();
    QTest::newRow("single") << QVector<int> { 0 } << QStringLiteral("1");
    QTest::newRow("run") << QVector<int> { 0, 1, 2 } << QStringLiteral("1-3");
    QTest::newRow("pair stays a list") << QVector<int> { 0, 1 } << QStringLiteral("1, 2");
    QTest::newRow("gaps") << QVector<int> { 0, 1, 2, 5 } << QStringLiteral("1-3, 6");
    QTest::newRow("unsorted input") << QVector<int> { 5, 1, 0, 2 } << QStringLiteral("1-3, 6");
}

void TestPageRange::formats()
{
    QFETCH(QVector<int>, input);
    QFETCH(QString, expected);
    QCOMPARE(PageRange::format(input), expected);
}

QTEST_GUILESS_MAIN(TestPageRange)

#include "tst_pagerange.moc"
