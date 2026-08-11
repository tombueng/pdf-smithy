/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "TestPdf.h"

#include "core/Document.h"
#include "core/DocumentWriter.h"
#include "render/PopplerBackend.h"
#include "core/Annotation.h"
#include "core/Redaction.h"
#include "ui/AnnotationOverlay.h"
#include "ui/PageView.h"
#include "ui/PageProcessor.h"

#include <QTemporaryDir>
#include <QTest>
#include <QUndoStack>
#include <QWidget>

#include <QColor>
#include <QImage>

#include <KLocalizedString>

using namespace ps;

/**
 * "How do I OCR a single page?" This is the answer, checked end to end.
 *
 * Operating on part of a document is the whole point of the processor: it
 * writes the chosen pages out, works on them, and puts the results back in the
 * same slots. What matters is that the untouched pages really are untouched
 * and that the whole thing is one undo step.
 */
class TestPageProcessor : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();

    void recognisesTextOnOneChosenPage();
    void leavesTheOtherPagesAlone();
    void isASingleUndoStep();
    void compressesOnlyTheChosenPages();
    void stampsASignatureOnChosenPages();
    void redactsOneLineAndPutsItBackOnUndo();
    void writesCommentsAndReplacesThemWholesale();
    void theCommentLayerFindsWhatIsAlreadyThere();

private:
    QTemporaryDir m_dir;
    QString m_scan;
    bool m_haveScan = false;
    QWidget m_parent;
};

void TestPageProcessor::initTestCase()
{
    KLocalizedString::setApplicationDomain("pdf-smithy");
    QVERIFY(m_dir.isValid());

    const QString text = m_dir.filePath(QStringLiteral("text.pdf"));
    m_scan = m_dir.filePath(QStringLiteral("scan.pdf"));
    QVERIFY(test::writeTextHeavyPdf(text, 3));

    // Only the scan fixture needs Ghostscript; the redaction case works on
    // text and must not be skipped along with it.
    m_haveScan = test::haveGhostscript() && test::rasterizePdf(text, m_scan, 200);
}

void TestPageProcessor::recognisesTextOnOneChosenPage()
{
#ifndef PS_WITH_OCR
    QSKIP("built without OCR support");
#else
    if (!ScanProcessor::isAvailable()) {
        QSKIP("Tesseract has no language data installed");
    }

    PopplerBackend backend;
    Document document;
    document.setRenderBackend(&backend);
    if (!m_haveScan) {
        QSKIP("Ghostscript is needed to build an image-only scan fixture");
    }
    QVERIFY(document.open(m_scan, nullptr));

    PageProcessor processor(&document, &backend, &m_parent);
    ScanProcessor::Options options;
    options.languages = { QStringLiteral("deu") };
    options.dpi = 200;
    options.straighten = false;

    QString error;
    // Only the middle page, exactly as picking one thumbnail would do.
    QVERIFY2(processor.recognizeText({ 1 }, options, &error), qPrintable(error));

    const QString out = m_dir.filePath(QStringLiteral("one-page.pdf"));
    QVERIFY2(DocumentWriter::write(document, out, {}, &error), qPrintable(error));

    PopplerBackend reader;
    QVERIFY(reader.addDocument(1, out, &error));
    const QString recognised = reader.extractText(1, 1);
    QVERIFY2(recognised.contains(QStringLiteral("Zeile"), Qt::CaseInsensitive),
             qPrintable(QStringLiteral("page 2 is still not searchable: %1").arg(recognised.left(120))));
#endif
}

void TestPageProcessor::leavesTheOtherPagesAlone()
{
#ifndef PS_WITH_OCR
    QSKIP("built without OCR support");
#else
    if (!ScanProcessor::isAvailable()) {
        QSKIP("Tesseract has no language data installed");
    }

    PopplerBackend backend;
    Document document;
    document.setRenderBackend(&backend);
    if (!m_haveScan) {
        QSKIP("Ghostscript is needed to build an image-only scan fixture");
    }
    QVERIFY(document.open(m_scan, nullptr));

    PageProcessor processor(&document, &backend, &m_parent);
    ScanProcessor::Options options;
    options.languages = { QStringLiteral("deu") };
    options.dpi = 200;
    options.straighten = false;

    QVERIFY(processor.recognizeText({ 1 }, options, nullptr));

    const QString out = m_dir.filePath(QStringLiteral("untouched.pdf"));
    QVERIFY(DocumentWriter::write(document, out, {}, nullptr));

    PopplerBackend reader;
    QVERIFY(reader.addDocument(1, out, nullptr));
    QCOMPARE(test::pageCountOf(out), 3);
    // Asking for one page must not quietly process the whole document.
    QVERIFY2(reader.extractText(1, 0).trimmed().isEmpty(), "page 1 was processed without being asked");
    QVERIFY2(reader.extractText(1, 2).trimmed().isEmpty(), "page 3 was processed without being asked");
#endif
}

void TestPageProcessor::isASingleUndoStep()
{
#ifndef PS_WITH_OCR
    QSKIP("built without OCR support");
#else
    if (!ScanProcessor::isAvailable()) {
        QSKIP("Tesseract has no language data installed");
    }

    PopplerBackend backend;
    Document document;
    document.setRenderBackend(&backend);
    if (!m_haveScan) {
        QSKIP("Ghostscript is needed to build an image-only scan fixture");
    }
    QVERIFY(document.open(m_scan, nullptr));

    const PageRef before = document.pageAt(1);

    PageProcessor processor(&document, &backend, &m_parent);
    ScanProcessor::Options options;
    options.languages = { QStringLiteral("deu") };
    options.dpi = 200;
    QVERIFY(processor.recognizeText({ 1 }, options, nullptr));

    QCOMPARE(document.undoStack()->count(), 1);
    QVERIFY(document.pageAt(1).sourceId != before.sourceId);

    document.undoStack()->undo();
    QCOMPARE(document.pageAt(1).sourceId, before.sourceId);
    QCOMPARE(document.pageAt(1).sourcePage, before.sourcePage);
    QCOMPARE(document.pageCount(), 3);
#endif
}

void TestPageProcessor::compressesOnlyTheChosenPages()
{
    PopplerBackend backend;
    Document document;
    document.setRenderBackend(&backend);
    if (!m_haveScan) {
        QSKIP("Ghostscript is needed to build an image-only scan fixture");
    }
    QVERIFY(document.open(m_scan, nullptr));

    PageProcessor processor(&document, &backend, &m_parent);
    Compressor::Options options;
    options.level = Compressor::Level::Lossless;

    Compressor::Report report;
    QString error;
    QVERIFY2(processor.compress({ 0 }, options, &report, &error), qPrintable(error));

    // Either it shrank the page and swapped it in, or there was nothing to gain
    // and it left the document alone. Never a pointless undo step either way.
    if (report.outcome == Compressor::Outcome::AlreadyOptimal) {
        QCOMPARE(document.undoStack()->count(), 0);
    } else {
        QCOMPARE(document.undoStack()->count(), 1);
    }
    QCOMPARE(document.pageCount(), 3);
}

void TestPageProcessor::stampsASignatureOnChosenPages()
{
    PopplerBackend backend;
    Document document;
    document.setRenderBackend(&backend);
    if (!m_haveScan) {
        QSKIP("Ghostscript is needed to build an image-only scan fixture");
    }
    QVERIFY(document.open(m_scan, nullptr));

    QImage signature(80, 30, QImage::Format_ARGB32);
    signature.fill(QColor(220, 20, 20));

    PageProcessor processor(&document, &backend, &m_parent);
    QString error;
    // Pages 1 and 3, deliberately skipping the one in between.
    QVERIFY2(processor.stamp({ 0, 2 }, signature, QRectF(380, 90, 150, 56), &error), qPrintable(error));
    QCOMPARE(document.undoStack()->count(), 1);

    const QString out = m_dir.filePath(QStringLiteral("signed.pdf"));
    QVERIFY2(DocumentWriter::write(document, out, {}, &error), qPrintable(error));

    PopplerBackend reader;
    QVERIFY(reader.addDocument(1, out, nullptr));

    const auto isRedAtSignature = [&reader](int page) {
        const QImage image = reader.renderPage(1, page, 612);
        if (image.isNull()) {
            return false;
        }
        const QColor pixel = image.pixelColor(455, static_cast<int>(792 - 118));
        return pixel.red() > 180 && pixel.green() < 90;
    };

    QVERIFY2(isRedAtSignature(0), "page 1 was not signed");
    QVERIFY2(!isRedAtSignature(1), "page 2 was signed although it was not selected");
    QVERIFY2(isRedAtSignature(2), "page 3 was not signed");

    // And it comes off again in one step.
    document.undoStack()->undo();
    const QString reverted = m_dir.filePath(QStringLiteral("reverted.pdf"));
    QVERIFY(DocumentWriter::write(document, reverted, {}, nullptr));
    PopplerBackend after;
    QVERIFY(after.addDocument(2, reverted, nullptr));
    const QImage plain = after.renderPage(2, 0, 612);
    QVERIFY(!plain.isNull());
    // Both brackets matter. Without them C++ reads this as
    // `red() > (180 == (green() > 180))`, which is `red() > 0` and true of
    // very nearly every pixel, so the assertion passed for two years without
    // ever looking at the green channel.
    const QColor spot = plain.pixelColor(455, static_cast<int>(792 - 118));
    QVERIFY2((spot.red() > 180) == (spot.green() > 180), "undo did not remove the signature");
}

void TestPageProcessor::redactsOneLineAndPutsItBackOnUndo()
{
    // A page of text rather than the scan fixture, because what matters here is
    // whether the characters leave the file.
    const QString text = m_dir.filePath(QStringLiteral("redact-source.pdf"));
    QVERIFY(test::writeTextHeavyPdf(text, 3));

    PopplerBackend backend;
    Document document;
    document.setRenderBackend(&backend);
    QVERIFY(document.open(text, nullptr));

    // The fifth line of the fixture sits on baseline 720 - 4 * 16, and the
    // lines are 16 apart with a 13-point font, so there is barely two points
    // of clearance either side. Anything looser catches the neighbours, which
    // is the correct behaviour, and makes this a real measurement.
    QHash<int, QVector<QRectF>> areas;
    areas.insert(1, { QRectF(70, 654, 440, 14) });

    PageProcessor processor(&document, &backend, &m_parent);
    Redaction::Report report;
    QString error;
    QVERIFY2(processor.redact(areas, &report, &error), qPrintable(error));
    QVERIFY(report.glyphsRemoved > 20);
    QCOMPARE(document.undoStack()->count(), 1);

    const QString out = m_dir.filePath(QStringLiteral("redacted.pdf"));
    QVERIFY2(DocumentWriter::write(document, out, {}, &error), qPrintable(error));

    PopplerBackend reader;
    QVERIFY(reader.addDocument(1, out, nullptr));
    const QString second = reader.extractText(1, 1);
    QVERIFY2(!second.contains(QStringLiteral("Zeile 5 auf Seite 2")), qPrintable(second));
    QVERIFY2(second.contains(QStringLiteral("Zeile 6 auf Seite 2")), "the line below went too");
    QVERIFY2(second.contains(QStringLiteral("Zeile 4 auf Seite 2")), "the line above went too");
    QVERIFY2(reader.extractText(1, 0).contains(QStringLiteral("Zeile 5 auf Seite 1")), "another page lost a line");

    // Undo restores the page in the organiser. What has already been written
    // out stays redacted, which is the honest behaviour: the file was the
    // point.
    document.undoStack()->undo();
    const QString restored = m_dir.filePath(QStringLiteral("restored.pdf"));
    QVERIFY2(DocumentWriter::write(document, restored, {}, &error), qPrintable(error));

    PopplerBackend afterUndo;
    QVERIFY(afterUndo.addDocument(2, restored, nullptr));
    QVERIFY(afterUndo.extractText(2, 1).contains(QStringLiteral("Zeile 5 auf Seite 2")));
}

void TestPageProcessor::writesCommentsAndReplacesThemWholesale()
{
    const QString text = m_dir.filePath(QStringLiteral("comment-source.pdf"));
    QVERIFY(test::writeTextHeavyPdf(text, 3));

    PopplerBackend backend;
    Document document;
    document.setRenderBackend(&backend);
    QVERIFY(document.open(text, nullptr));

    Annotation first;
    first.type = Annotation::Type::Highlight;
    first.quads = { QRectF(70, 654, 200, 14) };
    first.contents = QStringLiteral("Erste Anmerkung");

    Annotation second;
    second.type = Annotation::Type::Note;
    second.rect = QRectF(400, 600, 20, 20);
    second.contents = QStringLiteral("Zweite Anmerkung");

    PageProcessor processor(&document, &backend, &m_parent);
    QString error;
    QVERIFY2(processor.annotate({ { 1, { first, second } } }, &error), qPrintable(error));
    QCOMPARE(document.undoStack()->count(), 1);

    const QString out = m_dir.filePath(QStringLiteral("commented.pdf"));
    QVERIFY2(DocumentWriter::write(document, out, {}, &error), qPrintable(error));

    QVector<Annotation> written = Annotations::read(out, {}, &error);
    QCOMPARE(written.size(), 2);
    QCOMPARE(written.constFirst().page, 1);

    // Applying again with one comment must leave one, not three: the dialog
    // hands over the whole page, so the processor replaces rather than appends.
    QVERIFY2(processor.annotate({ { 1, { second } } }, &error), qPrintable(error));
    const QString again = m_dir.filePath(QStringLiteral("commented-again.pdf"));
    QVERIFY2(DocumentWriter::write(document, again, {}, &error), qPrintable(error));

    written = Annotations::read(again, {}, &error);
    QCOMPARE(written.size(), 1);
    QCOMPARE(written.constFirst().type, Annotation::Type::Note);
    QCOMPARE(written.constFirst().contents, QStringLiteral("Zweite Anmerkung"));

    // And an empty list clears the page rather than failing.
    QVERIFY2(processor.annotate({ { 1, {} } }, &error), qPrintable(error));
    const QString cleared = m_dir.filePath(QStringLiteral("comments-cleared.pdf"));
    QVERIFY2(DocumentWriter::write(document, cleared, {}, &error), qPrintable(error));
    QCOMPARE(Annotations::read(cleared, {}, &error).size(), 0);
}

void TestPageProcessor::theCommentLayerFindsWhatIsAlreadyThere()
{
    // Opening a document that someone else has already commented on has to show
    // their comments, or the first thing this tool does is throw them away.
    const QString plain = m_dir.filePath(QStringLiteral("dialog-source.pdf"));
    QVERIFY(test::writeTextHeavyPdf(plain, 2));

    Annotation existing;
    existing.type = Annotation::Type::Highlight;
    existing.page = 1;
    existing.quads = { QRectF(70, 654, 200, 14) };
    existing.contents = QStringLiteral("Von jemand anderem");
    existing.author = QStringLiteral("Kollegin");

    const QString marked = m_dir.filePath(QStringLiteral("dialog-marked.pdf"));
    QString error;
    QVERIFY2(Annotations::add(plain, marked, { existing }, &error), qPrintable(error));

    PopplerBackend backend;
    Document document;
    document.setRenderBackend(&backend);
    QVERIFY(document.open(marked, nullptr));
    // Turn the page in the organiser, so the coordinates have to be mapped.
    document.applyRotation({ 1 }, 90);

    // Through the layer the document view uses, which is where this reading
    // lives now. Picking a comment is what makes it look at that page at all;
    // the layer reads lazily, since a four-hundred-page document should not
    // parse every annotation to show page one.
    PageView view;
    view.setDocument(&document);
    view.setRenderBackend(&backend);
    AnnotationOverlay overlay(&view, &document);
    overlay.selectComment(1, 0);

    const QHash<int, QVector<Annotation>> found = overlay.annotationsByRow();

    QCOMPARE(found.value(1).size(), 1);
    QCOMPARE(found.value(1).constFirst().contents, QStringLiteral("Von jemand anderem"));
    QCOMPARE(found.value(1).constFirst().author, QStringLiteral("Kollegin"));
    QVERIFY2(!overlay.isModified(), "merely looking at a page counted as an edit");

    // On a page turned by 90 degrees the mark is no longer where it was in the
    // file, and the width and height have swapped.
    const QRectF shown = found.value(1).constFirst().quads.constFirst();
    QVERIFY2(shown.height() > shown.width(),
             qPrintable(QStringLiteral("expected a tall mark on a turned page, got %1 x %2")
                            .arg(shown.width())
                            .arg(shown.height())));
}

QTEST_MAIN(TestPageProcessor)

#include "tst_pageprocessor.moc"
