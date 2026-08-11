/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#include "TestPdf.h"

#include "core/NUp.h"
#include "render/PopplerBackend.h"

#include <QTemporaryDir>
#include <QTest>

using namespace ps;

/**
 * N-up as a document operation.
 *
 * The point of doing this on the object structure rather than by rendering is
 * that the text survives, so that is what these check: the right number of
 * sheets, the right size, and (the one that matters) that every page's text
 * is still selectable afterwards.
 */
class TestNUp : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();

    void picksASensibleGrid();
    void putsFourPagesOnOneSheet();
    void keepsTheTextSelectable();
    void turnsTheSheetToSuitTheGrid();
    void fillsThePartialLastSheet();
    void refusesAGridOfOne();
    void refusesMarginsThatLeaveNoRoom();

private:
    QTemporaryDir m_dir;
    QString m_source;
};

void TestNUp::initTestCase()
{
    QVERIFY(m_dir.isValid());
    m_source = m_dir.filePath(QStringLiteral("source.pdf"));
    QVERIFY(test::writeTextHeavyPdf(m_source, 7));
}

void TestNUp::picksASensibleGrid()
{
    // Near-square, so the slots stay as large as they can be.
    QCOMPARE(NUp::gridFor(4, false), QSize(2, 2));
    QCOMPARE(NUp::gridFor(9, false), QSize(3, 3));

    // The long side of the grid follows the long side of the sheet.
    QCOMPARE(NUp::gridFor(2, true), QSize(2, 1));
    QCOMPARE(NUp::gridFor(2, false), QSize(1, 2));
    QCOMPARE(NUp::gridFor(6, true), QSize(3, 2));
    QCOMPARE(NUp::gridFor(6, false), QSize(2, 3));
}

void TestNUp::putsFourPagesOnOneSheet()
{
    const QString out = m_dir.filePath(QStringLiteral("four-up.pdf"));

    NUp::Options options;
    options.columns = 2;
    options.rows = 2;

    QString error;
    QVERIFY2(NUp::apply(m_source, out, options, &error), qPrintable(error));

    // Seven pages, four to a sheet, is two sheets.
    QCOMPARE(test::pageCountOf(out), 2);
}

void TestNUp::keepsTheTextSelectable()
{
    const QString out = m_dir.filePath(QStringLiteral("selectable.pdf"));

    NUp::Options options;
    options.columns = 2;
    options.rows = 2;

    QString error;
    QVERIFY2(NUp::apply(m_source, out, options, &error), qPrintable(error));

    // The whole reason for placing form XObjects instead of rendering: every
    // one of the four pages on the first sheet is still text.
    PopplerBackend backend;
    QVERIFY(backend.addDocument(1, out, nullptr));
    const QString first = backend.extractText(1, 0);
    for (int page = 1; page <= 4; ++page) {
        QVERIFY2(first.contains(QStringLiteral("auf Seite %1").arg(page)),
                 qPrintable(QStringLiteral("page %1 is not on the first sheet as text").arg(page)));
    }

    const QString second = backend.extractText(1, 1);
    QVERIFY2(second.contains(QStringLiteral("auf Seite 7")), qPrintable(second));
    QVERIFY2(!second.contains(QStringLiteral("auf Seite 1 ")), "a page landed on two sheets");
}

void TestNUp::turnsTheSheetToSuitTheGrid()
{
    const QString out = m_dir.filePath(QStringLiteral("landscape.pdf"));

    // Two portrait pages side by side belong on a landscape sheet; saying so
    // should not be necessary.
    NUp::Options options;
    options.columns = 2;
    options.rows = 1;

    QString error;
    QVERIFY2(NUp::apply(m_source, out, options, &error), qPrintable(error));

    PopplerBackend backend;
    QVERIFY(backend.addDocument(1, out, nullptr));
    const QSizeF sheet = backend.pageSizePoints(1, 0);
    QVERIFY2(sheet.width() > sheet.height(),
             qPrintable(QStringLiteral("sheet is %1 × %2").arg(sheet.width()).arg(sheet.height())));
}

void TestNUp::fillsThePartialLastSheet()
{
    const QString out = m_dir.filePath(QStringLiteral("partial.pdf"));

    NUp::Options options;
    options.columns = 3;
    options.rows = 1;

    QString error;
    QVERIFY2(NUp::apply(m_source, out, options, &error), qPrintable(error));
    QCOMPARE(test::pageCountOf(out), 3); // 3 + 3 + 1

    // The last sheet holds only the seventh page and must not repeat anything.
    PopplerBackend backend;
    QVERIFY(backend.addDocument(1, out, nullptr));
    const QString last = backend.extractText(1, 2);
    QVERIFY2(last.contains(QStringLiteral("auf Seite 7")), qPrintable(last));
    QVERIFY2(!last.contains(QStringLiteral("auf Seite 6")), "the short sheet was padded with a repeat");
}

void TestNUp::refusesAGridOfOne()
{
    NUp::Options options;
    options.columns = 1;
    options.rows = 1;

    QString error;
    QVERIFY(!NUp::apply(m_source, m_dir.filePath(QStringLiteral("one.pdf")), options, &error));
    QVERIFY(!error.isEmpty());
}

void TestNUp::refusesMarginsThatLeaveNoRoom()
{
    NUp::Options options;
    options.columns = 2;
    options.rows = 2;
    options.marginPoints = 500.0;

    QString error;
    QVERIFY(!NUp::apply(m_source, m_dir.filePath(QStringLiteral("cramped.pdf")), options, &error));
    QVERIFY(!error.isEmpty());
}

QTEST_MAIN(TestNUp)

#include "tst_nup.moc"
