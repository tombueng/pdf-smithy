/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#include "TestPdf.h"

#include "core/ScanProcessor.h"
#include "render/PopplerBackend.h"

#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <KLocalizedString>

#include <clocale>
#include <cmath>

using namespace ps;

/**
 * These tests take the long way round on purpose: a text PDF is rasterised into
 * a genuine image-only scan, and then the pipeline has to turn it back into
 * something selectable. Anything less would only test that the code runs.
 */
class TestScanProcessor : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();

    void findsInstalledLanguages();
    void namesLanguagesReadably();

    void recognisesTextInAScan();
    void keepsPageCountAndSize();
    void skipsPagesThatAlreadyHaveText();
    void straightensACrookedScan();
    void leavesStraightPagesAlone();
    void reportsProgressForEveryPage();
    void refusesWhenNothingWasAsked();

private:
    /** Extracted text of a page, via the same renderer the application uses. */
    QString textOf(const QString &path, int page);

    QTemporaryDir m_dir;
    QString m_textPdf;
    QString m_scanPdf;
    PopplerBackend m_backend;
};

void TestScanProcessor::initTestCase()
{
    KLocalizedString::setApplicationDomain("pdf-smithy");
    QVERIFY(m_dir.isValid());

    // Run under a comma-decimal locale on purpose. PDF content streams must use
    // a full stop no matter what the user's system is set to, and getting this
    // wrong once already produced text layers that no viewer could read. If the
    // locale is not generated here, the C locale applies and the guard is a
    // no-op rather than a false failure.
    std::setlocale(LC_ALL, "de_DE.UTF-8");

    if (!ScanProcessor::isAvailable()) {
        QSKIP("Tesseract has no language data installed");
    }
    if (!test::haveGhostscript()) {
        QSKIP("Ghostscript is needed to build the scan fixtures");
    }

    m_textPdf = m_dir.filePath(QStringLiteral("text.pdf"));
    m_scanPdf = m_dir.filePath(QStringLiteral("scan.pdf"));

    QVERIFY(test::writeTextHeavyPdf(m_textPdf, 2));
    QVERIFY(test::rasterizePdf(m_textPdf, m_scanPdf, 200));
}

QString TestScanProcessor::textOf(const QString &path, int page)
{
    PopplerBackend reader;
    QString error;
    if (!reader.addDocument(9000, path, &error)) {
        return {};
    }
    const QString text = reader.extractText(9000, page);
    reader.removeDocument(9000);
    return text;
}

void TestScanProcessor::findsInstalledLanguages()
{
    const QStringList languages = ScanProcessor::availableLanguages();
    QVERIFY(!languages.isEmpty());
    // "osd" is orientation data, not something to offer as a language.
    QVERIFY(!languages.contains(QStringLiteral("osd")));
}

void TestScanProcessor::namesLanguagesReadably()
{
    // Asserted without naming the expected words: the label is translated, so
    // checking for "German" would fail on a German system for no good reason.
    const QString known = ScanProcessor::languageName(QStringLiteral("deu"));
    QVERIFY(!known.isEmpty());
    QVERIFY2(known != QStringLiteral("deu"), "a known code should be given a readable name");

    // Unknown codes fall back to themselves rather than to an empty string.
    QCOMPARE(ScanProcessor::languageName(QStringLiteral("zzz")), QStringLiteral("zzz"));
}

void TestScanProcessor::recognisesTextInAScan()
{
    // The rasterised fixture must genuinely have no text, or the test proves
    // nothing about OCR.
    QVERIFY2(textOf(m_scanPdf, 0).trimmed().isEmpty(), "the fixture is not a pure image scan");

    const QString out = m_dir.filePath(QStringLiteral("recognised.pdf"));

    ScanProcessor processor;
    ScanProcessor::Options options;
    options.languages = { QStringLiteral("deu") };
    options.straighten = false;
    options.dpi = 200;

    ScanProcessor::Report report;
    QString error;
    QVERIFY2(processor.process(m_scanPdf, out, &m_backend, options, &report, &error), qPrintable(error));

    QVERIFY2(report.wordsRecognized > 20,
             qPrintable(QStringLiteral("only %1 words recognised").arg(report.wordsRecognized)));

    const QString extracted = textOf(out, 0);
    QVERIFY2(!extracted.trimmed().isEmpty(), "the page still has no selectable text");
    // The fixture repeats "Zeile" on every line, so a working text layer has to
    // contain it. This is the whole point of the feature: copy and paste works.
    QVERIFY2(extracted.contains(QStringLiteral("Zeile"), Qt::CaseInsensitive),
             qPrintable(QStringLiteral("extracted text was: %1").arg(extracted.left(200))));
}

void TestScanProcessor::keepsPageCountAndSize()
{
    const QString out = m_dir.filePath(QStringLiteral("sized.pdf"));

    ScanProcessor processor;
    ScanProcessor::Options options;
    options.languages = { QStringLiteral("deu") };
    options.dpi = 150;

    QString error;
    QVERIFY2(processor.process(m_scanPdf, out, &m_backend, options, nullptr, &error), qPrintable(error));

    QCOMPARE(test::pageCountOf(out), test::pageCountOf(m_scanPdf));
}

void TestScanProcessor::skipsPagesThatAlreadyHaveText()
{
    const QString out = m_dir.filePath(QStringLiteral("skipped.pdf"));

    ScanProcessor processor;
    ScanProcessor::Options options;
    options.languages = { QStringLiteral("deu") };
    options.skipPagesWithText = true;
    options.straighten = false;

    ScanProcessor::Report report;
    QString error;
    // Fed the original text document, not the scan: every page already has text.
    QVERIFY2(processor.process(m_textPdf, out, &m_backend, options, &report, &error), qPrintable(error));

    QCOMPARE(report.pagesSkipped, 2);
    QCOMPARE(report.wordsRecognized, 0);
}

void TestScanProcessor::straightensACrookedScan()
{
    const QString crookedSource = m_dir.filePath(QStringLiteral("crooked-source.pdf"));
    const QString crookedScan = m_dir.filePath(QStringLiteral("crooked-scan.pdf"));
    QVERIFY(test::writeTextHeavyPdf(crookedSource, 1, 3.0));
    QVERIFY(test::rasterizePdf(crookedSource, crookedScan, 200));

    const QString out = m_dir.filePath(QStringLiteral("straightened.pdf"));

    ScanProcessor processor;
    ScanProcessor::Options options;
    options.recognizeText = false;
    options.straighten = true;

    ScanProcessor::Report report;
    QString error;
    QVERIFY2(processor.process(crookedScan, out, &m_backend, options, &report, &error), qPrintable(error));

    QCOMPARE(report.pagesStraightened, 1);
    // The fixture is tilted by three degrees, so that is what must be measured
    // back out, within the tolerance of a detector working on 200 dpi pixels.
    QVERIFY2(std::abs(report.largestSkewAngle - 3.0) < 0.6,
             qPrintable(QStringLiteral("measured %1 degrees, expected about 3").arg(report.largestSkewAngle)));

    // And afterwards the page must read as straight.
    const QString reScan = m_dir.filePath(QStringLiteral("re-scan.pdf"));
    QVERIFY(test::rasterizePdf(out, reScan, 200));

    ScanProcessor checker;
    ScanProcessor::Options checkOptions;
    checkOptions.recognizeText = false;
    checkOptions.straighten = true;
    ScanProcessor::Report checkReport;
    QVERIFY(checker.process(reScan, m_dir.filePath(QStringLiteral("check.pdf")), &m_backend, checkOptions, &checkReport,
                            nullptr));

    QVERIFY2(checkReport.pagesStraightened == 0 || checkReport.largestSkewAngle < 1.0,
             qPrintable(QStringLiteral("page is still crooked by %1 degrees").arg(checkReport.largestSkewAngle)));
}

void TestScanProcessor::leavesStraightPagesAlone()
{
    const QString out = m_dir.filePath(QStringLiteral("untouched.pdf"));

    ScanProcessor processor;
    ScanProcessor::Options options;
    options.recognizeText = false;
    options.straighten = true;

    ScanProcessor::Report report;
    QString error;
    QVERIFY2(processor.process(m_scanPdf, out, &m_backend, options, &report, &error), qPrintable(error));

    // A straight page must not be nudged; correcting noise would slowly rotate
    // documents every time they were processed.
    QCOMPARE(report.pagesStraightened, 0);
}

void TestScanProcessor::reportsProgressForEveryPage()
{
    ScanProcessor processor;
    QSignalSpy spy(&processor, &ScanProcessor::progress);

    ScanProcessor::Options options;
    options.recognizeText = false;
    options.straighten = true;

    QVERIFY(processor.process(m_scanPdf, m_dir.filePath(QStringLiteral("progress.pdf")), &m_backend, options, nullptr,
                              nullptr));
    QCOMPARE(spy.count(), test::pageCountOf(m_scanPdf));
}

void TestScanProcessor::refusesWhenNothingWasAsked()
{
    ScanProcessor processor;
    ScanProcessor::Options options;
    options.recognizeText = false;
    options.straighten = false;

    QString error;
    QVERIFY(!processor.process(m_scanPdf, m_dir.filePath(QStringLiteral("nothing.pdf")), &m_backend, options, nullptr,
                               &error));
    QVERIFY(!error.isEmpty());
}

QTEST_GUILESS_MAIN(TestScanProcessor)

#include "tst_scanprocessor.moc"
