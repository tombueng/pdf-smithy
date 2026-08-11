/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#include "TestPdf.h"

#include "core/Compressor.h"

#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>

#include <numeric>

using namespace ps;

class TestCompressor : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();

    void losslessKeepsEveryPage();
    void losslessReportsSizes();
    void neverReturnsABiggerFile();
    void refusesMissingInput();
    void reportsMissingGhostscript();
    void lossyShrinksAScan();
    void levelsHaveSensibleResolutions();
    void leavesNoTemporaryFiles();
    void saysSoWhenThereIsNothingToGain();
    void estimatesFromASample();
    void refusesToEstimateWithoutSamples();

private:
    QTemporaryDir m_dir;
    QString m_source;
};

void TestCompressor::initTestCase()
{
    QVERIFY(m_dir.isValid());
    m_source = m_dir.filePath(QStringLiteral("source.pdf"));
    QVERIFY(test::writeSamplePdf(m_source, 20));
}

void TestCompressor::losslessKeepsEveryPage()
{
    const QString out = m_dir.filePath(QStringLiteral("lossless.pdf"));

    Compressor::Options options;
    options.level = Compressor::Level::Lossless;

    Compressor::Report report;
    QString error;
    QVERIFY2(Compressor::compress(m_source, out, options, &report, &error), qPrintable(error));

    QCOMPARE(test::pageCountOf(out), 20);
    QVERIFY(!report.usedGhostscript);
    // Content must survive untouched, which is the whole promise of "lossless".
    QVERIFY(test::contentOf(out, 0).contains(QStringLiteral("PSPAGE 1")));
    QVERIFY(test::contentOf(out, 19).contains(QStringLiteral("PSPAGE 20")));
}

void TestCompressor::losslessReportsSizes()
{
    const QString out = m_dir.filePath(QStringLiteral("report.pdf"));

    Compressor::Options options;
    options.level = Compressor::Level::Lossless;

    Compressor::Report report;
    QVERIFY(Compressor::compress(m_source, out, options, &report, nullptr));

    QCOMPARE(report.originalBytes, QFileInfo(m_source).size());
    QCOMPARE(report.compressedBytes, QFileInfo(out).size());
    QVERIFY(report.savedPercent() >= 0);
    QVERIFY(report.savedPercent() <= 100);
}

void TestCompressor::neverReturnsABiggerFile()
{
    // A file that is already tightly packed has nothing left to give. The
    // result must still not be larger than what went in.
    const QString packed = m_dir.filePath(QStringLiteral("packed.pdf"));
    Compressor::Options options;
    options.level = Compressor::Level::Lossless;
    QVERIFY(Compressor::compress(m_source, packed, options, nullptr, nullptr));

    const QString again = m_dir.filePath(QStringLiteral("again.pdf"));
    Compressor::Report report;
    QVERIFY(Compressor::compress(packed, again, options, &report, nullptr));

    QVERIFY2(report.compressedBytes <= report.originalBytes, "compressing must never inflate the document");
    QCOMPARE(test::pageCountOf(again), 20);
}

void TestCompressor::refusesMissingInput()
{
    QString error;
    QVERIFY(!Compressor::compress(m_dir.filePath(QStringLiteral("nope.pdf")), m_dir.filePath(QStringLiteral("out.pdf")),
                                  Compressor::Options {}, nullptr, &error));
    QVERIFY(!error.isEmpty());
}

void TestCompressor::reportsMissingGhostscript()
{
    if (Compressor::isGhostscriptAvailable()) {
        QSKIP("Ghostscript is installed, so the missing-tool path cannot be exercised here");
    }

    Compressor::Options options;
    options.level = Compressor::Level::Balanced;

    QString error;
    QVERIFY(!Compressor::compress(m_source, m_dir.filePath(QStringLiteral("x.pdf")), options, nullptr, &error));
    // The message has to name the package, not just say "failed".
    QVERIFY(error.contains(QStringLiteral("ghostscript"), Qt::CaseInsensitive));
}

void TestCompressor::lossyShrinksAScan()
{
    if (!Compressor::isGhostscriptAvailable()) {
        QSKIP("Ghostscript is not installed");
    }

    const QString out = m_dir.filePath(QStringLiteral("lossy.pdf"));
    Compressor::Options options;
    options.level = Compressor::Level::Strong;

    Compressor::Report report;
    QString error;
    QVERIFY2(Compressor::compress(m_source, out, options, &report, &error), qPrintable(error));

    QVERIFY(report.usedGhostscript);
    QCOMPARE(test::pageCountOf(out), 20);
    QVERIFY(report.compressedBytes <= report.originalBytes);
}

void TestCompressor::levelsHaveSensibleResolutions()
{
    // Stronger levels must not somehow ask for more resolution.
    QVERIFY(Compressor::dpiForLevel(Compressor::Level::Balanced) > Compressor::dpiForLevel(Compressor::Level::Strong));
    QVERIFY(Compressor::dpiForLevel(Compressor::Level::Strong) > Compressor::dpiForLevel(Compressor::Level::Extreme));
    QVERIFY(!Compressor::levelName(Compressor::Level::Lossless).isEmpty());
}

void TestCompressor::leavesNoTemporaryFiles()
{
    const QString out = m_dir.filePath(QStringLiteral("clean.pdf"));
    Compressor::Options options;
    options.level = Compressor::Level::Lossless;
    QVERIFY(Compressor::compress(m_source, out, options, nullptr, nullptr));

    const QStringList leftovers
        = QDir(m_dir.path()).entryList({ QStringLiteral(".pdf-smithy-*") }, QDir::Files | QDir::Hidden);
    QVERIFY2(leftovers.isEmpty(), qPrintable(leftovers.join(QLatin1Char(' '))));
}

void TestCompressor::saysSoWhenThereIsNothingToGain()
{
    // Compress once so the result is already tightly packed, then compress the
    // result again. The second run cannot achieve anything, and the report has
    // to say that rather than quietly showing "0 %".
    const QString packed = m_dir.filePath(QStringLiteral("tight.pdf"));
    Compressor::Options options;
    options.level = Compressor::Level::Lossless;
    QVERIFY(Compressor::compress(m_source, packed, options, nullptr, nullptr));

    Compressor::Report report;
    QVERIFY(Compressor::compress(packed, m_dir.filePath(QStringLiteral("tighter.pdf")), options, &report, nullptr));

    QCOMPARE(report.outcome, Compressor::Outcome::AlreadyOptimal);
    QVERIFY(report.savedPercent() < Compressor::MeaningfulSavingPercent);
}

void TestCompressor::estimatesFromASample()
{
    // The estimator asks the caller for individual pages, because only the
    // caller knows how to produce them.
    int extracted = 0;
    const auto sampler = [this, &extracted](int page) -> QString {
        ++extracted;
        const QString path = m_dir.filePath(QStringLiteral("sample-%1.pdf").arg(page));
        return test::writeSamplePdf(path, 1) ? path : QString();
    };

    QVector<int> pages(20);
    std::iota(pages.begin(), pages.end(), 0);

    Compressor::Options options;
    options.level = Compressor::Level::Lossless;

    Compressor::Estimate estimate;
    QString error;
    QVERIFY2(Compressor::estimate(m_source, pages, sampler, options, &estimate, &error), qPrintable(error));

    // A handful of pages, not all twenty: the point is to answer quickly.
    QVERIFY2(extracted > 0 && extracted <= 4,
             qPrintable(QStringLiteral("sampled %1 pages, expected at most four").arg(extracted)));
    QCOMPARE(estimate.originalBytes, QFileInfo(m_source).size());
    QVERIFY(estimate.expectedBytes > 0);
    QVERIFY(estimate.savedPercent() >= 0);
    QVERIFY(estimate.savedPercent() <= 100);
    QVERIFY2(estimate.reliable, "three or more sampled pages should count as reliable");
}

void TestCompressor::refusesToEstimateWithoutSamples()
{
    // A sampler that can produce nothing must fail loudly rather than
    // reporting a saving of zero as though it had measured one.
    const auto broken = [](int) -> QString { return {}; };

    QVector<int> pages { 0, 1, 2 };
    Compressor::Estimate estimate;
    QString error;
    QVERIFY(!Compressor::estimate(m_source, pages, broken, Compressor::Options {}, &estimate, &error));
    QVERIFY(!error.isEmpty());
}

QTEST_GUILESS_MAIN(TestCompressor)

#include "tst_compressor.moc"
