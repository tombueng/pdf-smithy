/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#include "TestPdf.h"

#include "core/Document.h"
#include "core/Source.h"

#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using namespace ps;

class TestDocument : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();

    void opensFile();
    void refusesBrokenFile();
    void refusesMissingFile();

    void insertsPages();
    void removesContiguousPages();
    void removesScatteredPages();
    void removeReturnsAscendingOrder();
    void ignoresOutOfRangeRemoval();

    void movesPagesForward();
    void movesPagesBackward();
    void movesScatteredSelection();

    void rotatesPages();
    void normalisesRotation_data();
    void normalisesRotation();

    void mergesSecondFile();
    void clearsEverything();

private:
    QTemporaryDir m_dir;
    QString m_tenPages;
    QString m_threePages;
    QString m_broken;
};

void TestDocument::initTestCase()
{
    QVERIFY(m_dir.isValid());
    m_tenPages = m_dir.filePath(QStringLiteral("ten.pdf"));
    m_threePages = m_dir.filePath(QStringLiteral("three.pdf"));
    m_broken = m_dir.filePath(QStringLiteral("broken.pdf"));

    QVERIFY(test::writeSamplePdf(m_tenPages, 10));
    QVERIFY(test::writeSamplePdf(m_threePages, 3));
    QVERIFY(test::writeBrokenPdf(m_broken));
}

// ── Loading ───────────────────────────────────────────────────────────────

void TestDocument::opensFile()
{
    Document document;
    QString error;
    QVERIFY2(document.open(m_tenPages, &error), qPrintable(error));

    QCOMPARE(document.pageCount(), 10);
    QCOMPARE(document.sourceCount(), 1);
    QCOMPARE(document.filePath(), m_tenPages);
    QVERIFY(!document.isModified());

    // Pages start out pointing at their source one-to-one, unrotated.
    for (int i = 0; i < 10; ++i) {
        QCOMPARE(document.pageAt(i).sourceId, 0);
        QCOMPARE(document.pageAt(i).sourcePage, i);
        QCOMPARE(document.pageAt(i).rotation, 0);
    }
}

void TestDocument::refusesBrokenFile()
{
    Document document;
    QString error;
    QVERIFY(!document.open(m_broken, &error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(document.pageCount(), 0);
}

void TestDocument::refusesMissingFile()
{
    Document document;
    QString error;
    QVERIFY(!document.open(m_dir.filePath(QStringLiteral("nope.pdf")), &error));
    QVERIFY(!error.isEmpty());
}

// ── Insertion ─────────────────────────────────────────────────────────────

void TestDocument::insertsPages()
{
    Document document;
    QVERIFY(document.open(m_tenPages, nullptr));

    QSignalSpy about(&document, &Document::pagesAboutToBeInserted);
    QSignalSpy done(&document, &Document::pagesInserted);

    document.insertPages(3, { PageRef { 0, 7, 90 }, PageRef { 0, 8, 0 } });

    QCOMPARE(document.pageCount(), 12);
    QCOMPARE(about.count(), 1);
    QCOMPARE(done.count(), 1);
    QCOMPARE(done.first().at(0).toInt(), 3);
    QCOMPARE(done.first().at(1).toInt(), 2);

    // Inserted in order, at the requested position, pushing the rest along.
    QCOMPARE(document.pageAt(3).sourcePage, 7);
    QCOMPARE(document.pageAt(3).rotation, 90);
    QCOMPARE(document.pageAt(4).sourcePage, 8);
    QCOMPARE(document.pageAt(5).sourcePage, 3);
}

// ── Removal ───────────────────────────────────────────────────────────────

void TestDocument::removesContiguousPages()
{
    Document document;
    QVERIFY(document.open(m_tenPages, nullptr));

    QSignalSpy removed(&document, &Document::pagesRemoved);
    document.removePages({ 2, 3, 4 });

    QCOMPARE(document.pageCount(), 7);
    // One run means one signal, not three.
    QCOMPARE(removed.count(), 1);
    QCOMPARE(removed.first().at(0).toInt(), 2);
    QCOMPARE(removed.first().at(1).toInt(), 3);
    QCOMPARE(document.pageAt(2).sourcePage, 5);
}

void TestDocument::removesScatteredPages()
{
    Document document;
    QVERIFY(document.open(m_tenPages, nullptr));

    QSignalSpy removed(&document, &Document::pagesRemoved);
    document.removePages({ 1, 2, 5, 8 });

    QCOMPARE(document.pageCount(), 6);
    // Three runs: {1,2}, {5}, {8}.
    QCOMPARE(removed.count(), 3);

    const QVector<int> expected { 0, 3, 4, 6, 7, 9 };
    for (int i = 0; i < expected.size(); ++i) {
        QCOMPARE(document.pageAt(i).sourcePage, expected.at(i));
    }
}

void TestDocument::removeReturnsAscendingOrder()
{
    Document document;
    QVERIFY(document.open(m_tenPages, nullptr));

    // Deliberately unsorted with a duplicate: commands rely on the tidy-up.
    const QVector<PageRef> taken = document.removePages({ 5, 1, 5, 3 });

    QCOMPARE(taken.size(), 3);
    QCOMPARE(taken.at(0).sourcePage, 1);
    QCOMPARE(taken.at(1).sourcePage, 3);
    QCOMPARE(taken.at(2).sourcePage, 5);
    QCOMPARE(document.pageCount(), 7);
}

void TestDocument::ignoresOutOfRangeRemoval()
{
    Document document;
    QVERIFY(document.open(m_threePages, nullptr));

    const QVector<PageRef> taken = document.removePages({ -4, 1, 99 });
    QCOMPARE(taken.size(), 1);
    QCOMPARE(document.pageCount(), 2);
}

// ── Moving ────────────────────────────────────────────────────────────────

void TestDocument::movesPagesForward()
{
    Document document;
    QVERIFY(document.open(m_tenPages, nullptr));

    // Page 0 dropped in front of what is currently page 5.
    document.movePages({ 0 }, 5);

    QCOMPARE(document.pageCount(), 10);
    const QVector<int> expected { 1, 2, 3, 4, 0, 5, 6, 7, 8, 9 };
    for (int i = 0; i < expected.size(); ++i) {
        QCOMPARE(document.pageAt(i).sourcePage, expected.at(i));
    }
}

void TestDocument::movesPagesBackward()
{
    Document document;
    QVERIFY(document.open(m_tenPages, nullptr));

    document.movePages({ 7 }, 2);

    const QVector<int> expected { 0, 1, 7, 2, 3, 4, 5, 6, 8, 9 };
    for (int i = 0; i < expected.size(); ++i) {
        QCOMPARE(document.pageAt(i).sourcePage, expected.at(i));
    }
}

void TestDocument::movesScatteredSelection()
{
    Document document;
    QVERIFY(document.open(m_tenPages, nullptr));

    // Pages 1, 3 and 8 gathered up and dropped before page 6.
    document.movePages({ 1, 3, 8 }, 6);

    QCOMPARE(document.pageCount(), 10);
    const QVector<int> expected { 0, 2, 4, 5, 1, 3, 8, 6, 7, 9 };
    for (int i = 0; i < expected.size(); ++i) {
        QCOMPARE(document.pageAt(i).sourcePage, expected.at(i));
    }
}

// ── Rotation ──────────────────────────────────────────────────────────────

void TestDocument::rotatesPages()
{
    Document document;
    QVERIFY(document.open(m_tenPages, nullptr));

    QSignalSpy changed(&document, &Document::pagesChanged);
    document.applyRotation({ 2, 4 }, 90);

    QCOMPARE(changed.count(), 1);
    QCOMPARE(changed.first().at(0).toInt(), 2);
    QCOMPARE(changed.first().at(1).toInt(), 4);
    QCOMPARE(document.pageAt(2).rotation, 90);
    QCOMPARE(document.pageAt(3).rotation, 0);
    QCOMPARE(document.pageAt(4).rotation, 90);

    // Rotation accumulates and wraps rather than piling up.
    document.applyRotation({ 2 }, 300);
    QCOMPARE(document.pageAt(2).rotation, 0);
}

void TestDocument::normalisesRotation_data()
{
    QTest::addColumn<int>("input");
    QTest::addColumn<int>("expected");

    QTest::newRow("zero") << 0 << 0;
    QTest::newRow("quarter") << 90 << 90;
    QTest::newRow("full turn") << 360 << 0;
    QTest::newRow("beyond full") << 450 << 90;
    QTest::newRow("negative") << -90 << 270;
    QTest::newRow("far negative") << -450 << 270;
    QTest::newRow("off grid rounds down") << 100 << 90;
}

void TestDocument::normalisesRotation()
{
    QFETCH(int, input);
    QFETCH(int, expected);
    QCOMPARE(normalizeRotation(input), expected);
}

// ── Merging ───────────────────────────────────────────────────────────────

void TestDocument::mergesSecondFile()
{
    Document document;
    QVERIFY(document.open(m_tenPages, nullptr));

    QString error;
    QVERIFY2(document.importFile(m_threePages, -1, &error), qPrintable(error));

    QCOMPARE(document.sourceCount(), 2);
    QCOMPARE(document.pageCount(), 13);

    // The appended pages remember that they came from the second file.
    QCOMPARE(document.pageAt(9).sourceId, 0);
    QCOMPARE(document.pageAt(10).sourceId, 1);
    QCOMPARE(document.pageAt(10).sourcePage, 0);
    QCOMPARE(document.pageAt(12).sourcePage, 2);
}

void TestDocument::clearsEverything()
{
    Document document;
    QVERIFY(document.open(m_tenPages, nullptr));
    QVERIFY(!document.isEmpty());

    document.clear();

    QCOMPARE(document.pageCount(), 0);
    QCOMPARE(document.sourceCount(), 0);
    QVERIFY(document.isEmpty());
    QVERIFY(document.filePath().isEmpty());
}

QTEST_GUILESS_MAIN(TestDocument)

#include "tst_document.moc"
