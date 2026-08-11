/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#include "TestPdf.h"

#include "core/Document.h"
#include "core/Splitter.h"

#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>

using namespace ps;

class TestSplitter : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();

    void splitsIntoSinglePages();
    void splitsIntoChunks();
    void handlesUnevenLastChunk();
    void splitsByRanges();
    void splitsBySize();
    void readsChapters();
    void splitsOnBookmarks();
    void namesFilesAfterBookmarks();
    void refusesBookmarkSplitWithoutBookmarks();

    void refusesEmptyDocument();
    void refusesMissingTemplate();

    void padsNumbersForSorting();
    void appendsNumberWhenTemplateHasNoPlaceholder();

private:
    QTemporaryDir m_dir;
    QString m_tenPages;
};

void TestSplitter::initTestCase()
{
    QVERIFY(m_dir.isValid());
    m_tenPages = m_dir.filePath(QStringLiteral("ten.pdf"));
    QVERIFY(test::writeSamplePdf(m_tenPages, 10));
}

void TestSplitter::splitsIntoSinglePages()
{
    Document document;
    QVERIFY(document.open(m_tenPages, nullptr));

    Splitter::Options options;
    options.mode = Splitter::Mode::EveryNPages;
    options.pagesPerFile = 1;
    options.outputTemplate = m_dir.filePath(QStringLiteral("single-%1.pdf"));

    const Splitter::Result result = Splitter::split(document, options);
    QVERIFY2(result.success, qPrintable(result.error));
    QCOMPARE(result.files.size(), 10);

    for (int i = 0; i < result.files.size(); ++i) {
        QCOMPARE(test::pageCountOf(result.files.at(i)), 1);
        QVERIFY(test::contentOf(result.files.at(i), 0).contains(QStringLiteral("PSPAGE %1").arg(i + 1)));
    }
}

void TestSplitter::splitsIntoChunks()
{
    Document document;
    QVERIFY(document.open(m_tenPages, nullptr));

    Splitter::Options options;
    options.pagesPerFile = 5;
    options.outputTemplate = m_dir.filePath(QStringLiteral("half-%1.pdf"));

    const Splitter::Result result = Splitter::split(document, options);
    QVERIFY2(result.success, qPrintable(result.error));
    QCOMPARE(result.files.size(), 2);
    QCOMPARE(test::pageCountOf(result.files.at(0)), 5);
    QCOMPARE(test::pageCountOf(result.files.at(1)), 5);
    QVERIFY(test::contentOf(result.files.at(1), 0).contains(QStringLiteral("PSPAGE 6")));
}

void TestSplitter::handlesUnevenLastChunk()
{
    Document document;
    QVERIFY(document.open(m_tenPages, nullptr));

    Splitter::Options options;
    options.pagesPerFile = 3;
    options.outputTemplate = m_dir.filePath(QStringLiteral("third-%1.pdf"));

    const Splitter::Result result = Splitter::split(document, options);
    QVERIFY2(result.success, qPrintable(result.error));
    // 10 pages in threes: 3, 3, 3 and a remainder of 1.
    QCOMPARE(result.files.size(), 4);
    QCOMPARE(test::pageCountOf(result.files.at(3)), 1);
}

void TestSplitter::splitsByRanges()
{
    Document document;
    QVERIFY(document.open(m_tenPages, nullptr));

    Splitter::Options options;
    options.mode = Splitter::Mode::Ranges;
    options.ranges = QStringLiteral("1-3, 8-10");
    options.outputTemplate = m_dir.filePath(QStringLiteral("part-%1.pdf"));

    const Splitter::Result result = Splitter::split(document, options);
    QVERIFY2(result.success, qPrintable(result.error));
    // Each comma group becomes its own document.
    QCOMPARE(result.files.size(), 2);
    QCOMPARE(test::pageCountOf(result.files.at(0)), 3);
    QCOMPARE(test::pageCountOf(result.files.at(1)), 3);
    QVERIFY(test::contentOf(result.files.at(1), 0).contains(QStringLiteral("PSPAGE 8")));
}

void TestSplitter::splitsBySize()
{
    Document document;
    QVERIFY(document.open(m_tenPages, nullptr));

    // Small enough that the ten-page document cannot fit in one piece.
    const qint64 whole = QFileInfo(m_tenPages).size();

    Splitter::Options options;
    options.mode = Splitter::Mode::MaxFileSize;
    options.maxBytes = whole / 3;
    options.outputTemplate = m_dir.filePath(QStringLiteral("chunk-%1.pdf"));

    const Splitter::Result result = Splitter::split(document, options);
    QVERIFY2(result.success, qPrintable(result.error));
    QVERIFY(result.files.size() > 1);

    int total = 0;
    for (const QString &file : result.files) {
        total += test::pageCountOf(file);
    }
    // Nothing may be lost or duplicated, whatever the sizes worked out to.
    QCOMPARE(total, 10);

    // No probe files left behind.
    const QStringList leftovers = QDir(m_dir.path()).entryList({ QStringLiteral("*.probe") }, QDir::Files);
    QVERIFY(leftovers.isEmpty());
}

void TestSplitter::refusesEmptyDocument()
{
    Document empty;
    Splitter::Options options;
    options.outputTemplate = m_dir.filePath(QStringLiteral("nothing-%1.pdf"));

    const Splitter::Result result = Splitter::split(empty, options);
    QVERIFY(!result.success);
    QVERIFY(!result.error.isEmpty());
}

void TestSplitter::refusesMissingTemplate()
{
    Document document;
    QVERIFY(document.open(m_tenPages, nullptr));

    const Splitter::Result result = Splitter::split(document, Splitter::Options {});
    QVERIFY(!result.success);
    QVERIFY(!result.error.isEmpty());
}

void TestSplitter::padsNumbersForSorting()
{
    // Twelve files must sort correctly in a file manager, so 1 becomes "01".
    QCOMPARE(Splitter::expandTemplate(QStringLiteral("out-%1.pdf"), 1, 12), QStringLiteral("out-01.pdf"));
    QCOMPARE(Splitter::expandTemplate(QStringLiteral("out-%1.pdf"), 12, 12), QStringLiteral("out-12.pdf"));
    QCOMPARE(Splitter::expandTemplate(QStringLiteral("out-%1.pdf"), 7, 9), QStringLiteral("out-7.pdf"));
    QCOMPARE(Splitter::expandTemplate(QStringLiteral("out-%1.pdf"), 5, 100), QStringLiteral("out-005.pdf"));
}

void TestSplitter::appendsNumberWhenTemplateHasNoPlaceholder()
{
    // Without this, every chunk would overwrite the previous one.
    QCOMPARE(Splitter::expandTemplate(QStringLiteral("/tmp/report.pdf"), 2, 5), QStringLiteral("/tmp/report-2.pdf"));
}

void TestSplitter::readsChapters()
{
    const QString book = m_dir.filePath(QStringLiteral("book.pdf"));
    QVERIFY(test::writeBookmarkedPdf(
        book, 10,
        { { 0, QStringLiteral("Vorwort") }, { 3, QStringLiteral("Hauptteil") }, { 7, QStringLiteral("Anhang") } }));

    const QVector<Splitter::Chapter> chapters = Splitter::chaptersOf(book);
    QCOMPARE(chapters.size(), 3);
    QCOMPARE(chapters.at(0).firstPage, 0);
    QCOMPARE(chapters.at(0).title, QStringLiteral("Vorwort"));
    QCOMPARE(chapters.at(1).firstPage, 3);
    QCOMPARE(chapters.at(2).title, QStringLiteral("Anhang"));

    // A document without an outline reports none rather than failing.
    QVERIFY(Splitter::chaptersOf(m_tenPages).isEmpty());
}

void TestSplitter::splitsOnBookmarks()
{
    const QString book = m_dir.filePath(QStringLiteral("chaptered.pdf"));
    QVERIFY(test::writeBookmarkedPdf(book, 10, { { 0, QStringLiteral("Eins") }, { 4, QStringLiteral("Zwei") } }));

    Document document;
    QVERIFY(document.open(book, nullptr));

    Splitter::Options options;
    options.mode = Splitter::Mode::Bookmarks;
    options.bookmarkSource = book;
    options.nameFilesAfterBookmarks = false;
    options.outputTemplate = m_dir.filePath(QStringLiteral("chapter-%1.pdf"));

    const Splitter::Result result = Splitter::split(document, options);
    QVERIFY2(result.success, qPrintable(result.error));
    QCOMPARE(result.files.size(), 2);

    // Pages 1-4 and 5-10: the split runs from one bookmark to the next.
    QCOMPARE(test::pageCountOf(result.files.at(0)), 4);
    QCOMPARE(test::pageCountOf(result.files.at(1)), 6);
    QVERIFY(test::contentOf(result.files.at(1), 0).contains(QStringLiteral("PSPAGE 5")));
}

void TestSplitter::namesFilesAfterBookmarks()
{
    const QString book = m_dir.filePath(QStringLiteral("named.pdf"));
    QVERIFY(
        test::writeBookmarkedPdf(book, 6, { { 0, QStringLiteral("Kündigung") }, { 3, QStringLiteral("Anlage A/B") } }));

    Document document;
    QVERIFY(document.open(book, nullptr));

    Splitter::Options options;
    options.mode = Splitter::Mode::Bookmarks;
    options.bookmarkSource = book;
    options.outputTemplate = m_dir.filePath(QStringLiteral("out.pdf"));

    const Splitter::Result result = Splitter::split(document, options);
    QVERIFY2(result.success, qPrintable(result.error));
    QCOMPARE(result.files.size(), 2);

    QVERIFY2(result.files.at(0).contains(QStringLiteral("Kündigung")), qPrintable(result.files.at(0)));
    // A slash in a bookmark would otherwise be read as a directory.
    QVERIFY2(!QFileInfo(result.files.at(1)).fileName().contains(QLatin1Char('/')), qPrintable(result.files.at(1)));
    QVERIFY(QFileInfo::exists(result.files.at(1)));
}

void TestSplitter::refusesBookmarkSplitWithoutBookmarks()
{
    Document document;
    QVERIFY(document.open(m_tenPages, nullptr));

    Splitter::Options options;
    options.mode = Splitter::Mode::Bookmarks;
    options.bookmarkSource = m_tenPages;
    options.outputTemplate = m_dir.filePath(QStringLiteral("none-%1.pdf"));

    const Splitter::Result result = Splitter::split(document, options);
    QVERIFY(!result.success);
    QVERIFY2(!result.error.isEmpty(), "a refusal has to say why");
}

QTEST_GUILESS_MAIN(TestSplitter)

#include "tst_splitter.moc"
