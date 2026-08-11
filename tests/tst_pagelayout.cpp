/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#include "TestPdf.h"

#include "core/PageLayout.h"

#include <KLocalizedString>

#include <QFile>
#include <QTemporaryDir>
#include <QTest>
#include <QtEnvironmentVariables>

#include <qpdf/Buffer.hh>
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>
#include <qpdf/QPDFWriter.hh>

#include <cmath>

using namespace ps;

namespace {

// A4 and A5 to the fraction, because the fractions are the point: this project
// has already shipped a release that read them back as 595 and 841.
constexpr double kA4Width = 595.276;
constexpr double kA4Height = 841.89;
constexpr double kA5Width = 419.528;
constexpr double kA5Height = 595.276;

} // namespace

/**
 * The five boxes and the operations that move paper about.
 *
 * Everything here reads its result back out of the produced file. Boxes written
 * and boxes read are two different code paths through the same locale trap, and
 * only checking both catches it.
 */
class TestPagelayout : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();

    void readsTheBoxesOfARealDocument();
    void reportsAbsentBoxesAsAbsent();
    void roundTripsAllFiveBoxesIncludingTheirFractions();
    void refusesATrimBoxOutsideItsMediaBox();
    void refusesABoxThatEscapesTheOneAroundIt();

    void setsTrimAndBleedFromTheCropBox();
    void growsTheSheetToHoldTheBleed();

    void resizesAPageWithoutRewritingItsContent();
    void leavesTheImageOfAScanUntouchedWhenResizing();
    void unifiesEveryPageToTheLargest();

    void splitsAPageIntoTwoHalvesOverTheSameContent();
    void refusesASplitAtTheVeryEdge();

    void laysAnOverlayOverEveryBasePage();

    void addsPrintersMarksAndGrowsTheSheet();
    void refusesToAddNoMarksAtAll();

    void tilesOnePageOntoFourSheets();
    void compressesTheFormItBuildsForATile();

    void writesAndReadsRomanFrontMatter();
    void reportsNoLabelsWhenTheDocumentHasNone();

    void findsABlankPageAmongTextPages();
    void treatsWhitespaceOnlyTextAsBlank();
    void removesPagesButNotAllOfThem();

    void reordersEightPagesIntoPrinterSpreads();
    void takesPrinterSpreadsBackToReaderOrder();
    void padsAnIncompleteBookletToAMultipleOfFour();

    void namesItsLimitations();

private:
    QString path(const QString &name) const { return m_dir.filePath(name); }

    /** A document with a genuinely empty page and a page whose only text is spaces. */
    static bool writeWithBlankPages(const QString &file, int pageCount, int emptyIndex, int spacesIndex);

    /** The decoded contents of every form XObject on a page, glued together. */
    static QString formContentOf(const QString &file, int page);

    /** The filter on every form XObject in the file, so an uncompressed one shows up. */
    static QStringList formFilters(const QString &file);

    /** The *undecoded* bytes of the first image on a page: the scan itself. */
    static QByteArray rawImageOf(const QString &file, int page);

    QTemporaryDir m_dir;
    QString m_letter;
    QString m_a4;
};

void TestPagelayout::initTestCase()
{
    // Without a domain, KLocalizedString warns on every message it builds and
    // buries the test output.
    KLocalizedString::setApplicationDomain("pdf-smithy");

    QVERIFY(m_dir.isValid());
    m_letter = path(QStringLiteral("letter.pdf"));
    QVERIFY(test::writeSamplePdf(m_letter, 3, QSizeF(612, 792)));
    m_a4 = path(QStringLiteral("a4.pdf"));
    QVERIFY(test::writeSamplePdf(m_a4, 2, QSizeF(kA4Width, kA4Height)));
}

bool TestPagelayout::writeWithBlankPages(const QString &file, int pageCount, int emptyIndex, int spacesIndex)
{
    try {
        QPDF pdf;
        pdf.emptyPDF();
        QPDFPageDocumentHelper pages(pdf);

        QPDFObjectHandle font
            = pdf.makeIndirectObject(QPDFObjectHandle::parse("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>"));

        for (int i = 0; i < pageCount; ++i) {
            std::string body;
            if (i == emptyIndex) {
                // A present but empty stream, which is what a separator page put
                // in by a scanner or a mail merge actually looks like.
                body = "q Q\n";
            } else if (i == spacesIndex) {
                body = "BT /F1 12 Tf 72 700 Td (   ) Tj ET\n";
            } else {
                body = "BT /F1 24 Tf 72 700 Td (PSPAGE " + std::to_string(i + 1) + ") Tj ET\n";
            }

            QPDFObjectHandle page = QPDFObjectHandle::parse("<< /Type /Page /MediaBox [0 0 612 792] >>");
            page.replaceKey("/Resources", QPDFObjectHandle::parse("<< /Font << >> >>"));
            page.getKey("/Resources").getKey("/Font").replaceKey("/F1", font);
            page.replaceKey("/Contents", QPDFObjectHandle::newStream(&pdf, body));
            pages.addPage(QPDFPageObjectHelper(pdf.makeIndirectObject(page)), false);
        }

        QPDFWriter writer(pdf);
        writer.setOutputFilename(QFile::encodeName(file).constData());
        writer.setDeterministicID(true);
        writer.write();
    } catch (const std::exception &) {
        return false;
    }
    return true;
}

QString TestPagelayout::formContentOf(const QString &file, int page)
{
    QString combined;
    try {
        QPDF pdf;
        pdf.processFile(QFile::encodeName(file).constData());
        std::vector<QPDFPageObjectHelper> pages = QPDFPageDocumentHelper(pdf).getAllPages();
        if (page < 0 || page >= int(pages.size())) {
            return {};
        }
        QPDFObjectHandle resources = pages[size_t(page)].getAttribute("/Resources", false);
        QPDFObjectHandle xobjects = resources.getKey("/XObject");
        if (!xobjects.isDictionary()) {
            return {};
        }
        for (const std::string &key : xobjects.getKeys()) {
            QPDFObjectHandle object = xobjects.getKey(key);
            if (!object.isStream()) {
                continue;
            }
            const std::shared_ptr<Buffer> buffer = object.getStreamData();
            combined += QString::fromLatin1(reinterpret_cast<const char *>(buffer->getBuffer()),
                                            qsizetype(buffer->getSize()));
        }
    } catch (const std::exception &) {
        return {};
    }
    return combined;
}

QStringList TestPagelayout::formFilters(const QString &file)
{
    QStringList filters;
    try {
        QPDF pdf;
        pdf.processFile(QFile::encodeName(file).constData());
        for (QPDFObjectHandle &object : pdf.getAllObjects()) {
            if (!object.isStream()) {
                continue;
            }
            QPDFObjectHandle subtype = object.getDict().getKey("/Subtype");
            if (!subtype.isName() || subtype.getName() != "/Form") {
                continue;
            }
            QPDFObjectHandle filter = object.getDict().getKey("/Filter");
            if (filter.isName()) {
                filters.append(QString::fromStdString(filter.getName()));
            } else if (filter.isArray() && filter.getArrayNItems() > 0 && filter.getArrayItem(0).isName()) {
                filters.append(QString::fromStdString(filter.getArrayItem(0).getName()));
            } else {
                filters.append(QString());
            }
        }
    } catch (const std::exception &) {
        return {};
    }
    return filters;
}

QByteArray TestPagelayout::rawImageOf(const QString &file, int page)
{
    try {
        QPDF pdf;
        pdf.processFile(QFile::encodeName(file).constData());
        std::vector<QPDFPageObjectHelper> pages = QPDFPageDocumentHelper(pdf).getAllPages();
        if (page < 0 || page >= int(pages.size())) {
            return {};
        }
        QPDFObjectHandle xobjects = pages[size_t(page)].getAttribute("/Resources", false).getKey("/XObject");
        if (!xobjects.isDictionary()) {
            return {};
        }
        for (const std::string &key : xobjects.getKeys()) {
            QPDFObjectHandle object = xobjects.getKey(key);
            if (!object.isStream()) {
                continue;
            }
            QPDFObjectHandle subtype = object.getDict().getKey("/Subtype");
            if (!subtype.isName() || subtype.getName() != "/Image") {
                continue;
            }
            const std::shared_ptr<Buffer> buffer = object.getRawStreamData();
            return QByteArray(reinterpret_cast<const char *>(buffer->getBuffer()), qsizetype(buffer->getSize()));
        }
    } catch (const std::exception &) {
        return {};
    }
    return {};
}

/**
 * The scanned letter, found wherever the build put it.
 *
 * Not a plain relative path: ctest runs a test from the build directory rather
 * than from the source tree, so "testdata/..." resolves to nothing there and the
 * three cases below failed only under ctest, passing when run by hand, which is
 * the most misleading way for a test to break.
 */
static QString scannedLetter()
{
    return QTest::qFindTestData(QStringLiteral("../testdata/brief-1902.pdf"), __FILE__, __LINE__);
}

void TestPagelayout::readsTheBoxesOfARealDocument()
{
    const QString scan = scannedLetter();
    if (scan.isEmpty()) {
        QSKIP("testdata/brief-1902.pdf has not been downloaded");
    }

    QString error;
    const QVector<PageLayout::Boxes> boxes = PageLayout::boxesOf(scan, &error);
    QVERIFY2(!boxes.isEmpty(), qPrintable(error));
    QCOMPARE(boxes.size(), 5);

    for (const PageLayout::Boxes &page : boxes) {
        QVERIFY(page.media.isValid());
        QVERIFY(page.media.width() > 100.0);
        QVERIFY(page.media.height() > 100.0);
    }
    QCOMPARE(boxes.first().media, QRectF(0, 0, 637, 842));
    QCOMPARE(boxes.first().crop, QRectF(0, 0, 637, 842));
}

void TestPagelayout::reportsAbsentBoxesAsAbsent()
{
    const QString scan = scannedLetter();
    if (scan.isEmpty()) {
        QSKIP("testdata/brief-1902.pdf has not been downloaded");
    }

    QString error;
    const QVector<PageLayout::Boxes> boxes = PageLayout::boxesOf(scan, &error);
    QVERIFY2(!boxes.isEmpty(), qPrintable(error));

    // The document has no prepress boxes at all. Reporting the fallback the
    // specification prescribes would make it indistinguishable from a file that
    // has been prepared for a press, which is the one distinction that matters.
    QVERIFY(!boxes.first().bleed.isValid());
    QVERIFY(!boxes.first().trim.isValid());
    QVERIFY(!boxes.first().art.isValid());

    // A real magazine, if it is on this machine, has all five, and the four
    // fractional numbers that used to read back as integers.
    const QString magazine = qEnvironmentVariable("PS_STRESS_PDF");
    if (!QFile::exists(magazine)) {
        QSKIP("set PS_STRESS_PDF to a large real document to run this");
    }
    const QVector<PageLayout::Boxes> real = PageLayout::boxesOf(magazine, &error);
    QVERIFY2(!real.isEmpty(), qPrintable(error));
    QCOMPARE(real.size(), 180);
    for (const QRectF &box :
         { real.first().media, real.first().crop, real.first().bleed, real.first().trim, real.first().art }) {
        QVERIFY(box.isValid());
        QVERIFY(std::abs(box.width() - kA4Width) < 1e-6);
        QVERIFY(std::abs(box.height() - kA4Height) < 1e-6);
    }
}

void TestPagelayout::roundTripsAllFiveBoxesIncludingTheirFractions()
{
    PageLayout::Boxes wanted;
    wanted.media = QRectF(0.0, 0.0, kA4Width, kA4Height);
    wanted.crop = QRectF(2.5, 3.25, 590.276, 835.39);
    wanted.bleed = QRectF(1.5, 2.25, 592.276, 837.39);
    wanted.trim = QRectF(10.125, 11.5, 575.026, 818.89);
    wanted.art = QRectF(12.375, 13.75, 570.526, 814.39);

    const QString out = path(QStringLiteral("boxes.pdf"));
    QString error;
    QVERIFY2(PageLayout::setBoxes(m_a4, out, { 0, 1 }, wanted, &error), qPrintable(error));

    const QVector<PageLayout::Boxes> read = PageLayout::boxesOf(out, &error);
    QVERIFY2(read.size() == 2, qPrintable(error));

    for (const PageLayout::Boxes &page : read) {
        QCOMPARE(page.media, wanted.media);
        QCOMPARE(page.crop, wanted.crop);
        QCOMPARE(page.bleed, wanted.bleed);
        QCOMPARE(page.trim, wanted.trim);
        QCOMPARE(page.art, wanted.art);
    }

    // Spelled out, because the failure this guards against is not a wrong
    // rectangle but a truncated number: under a comma locale every one of these
    // came back as its integer part.
    QVERIFY(std::abs(read.first().media.width() - kA4Width) < 1e-6);
    QVERIFY(std::abs(read.first().media.height() - kA4Height) < 1e-6);
    QVERIFY(read.first().media.width() > 595.2);
    QVERIFY(std::abs(read.first().trim.x() - 10.125) < 1e-6);
    QVERIFY(std::abs(read.first().art.y() - 13.75) < 1e-6);
}

void TestPagelayout::refusesATrimBoxOutsideItsMediaBox()
{
    PageLayout::Boxes wanted;
    wanted.media = QRectF(0, 0, 612, 792);
    wanted.trim = QRectF(-20, -20, 700, 900);

    const QString out = path(QStringLiteral("refused.pdf"));
    QString error;
    QVERIFY(!PageLayout::setBoxes(m_letter, out, { 0 }, wanted, &error));
    QVERIFY(!error.isEmpty());
    // Nothing may reach the destination when the arrangement is refused.
    QVERIFY(!QFile::exists(out));
}

void TestPagelayout::refusesABoxThatEscapesTheOneAroundIt()
{
    PageLayout::Boxes wanted;
    wanted.media = QRectF(0, 0, 612, 792);
    wanted.bleed = QRectF(20, 20, 572, 752);
    wanted.trim = QRectF(10, 10, 592, 772); // larger than its bleed
    wanted.art = QRectF(30, 30, 100, 100);

    const QString out = path(QStringLiteral("refused2.pdf"));
    QString error;
    QVERIFY(!PageLayout::setBoxes(m_letter, out, { 0 }, wanted, &error));
    QVERIFY(!error.isEmpty());
}

void TestPagelayout::setsTrimAndBleedFromTheCropBox()
{
    PageLayout::Boxes start;
    start.media = QRectF(0.0, 0.0, kA4Width, kA4Height);
    start.crop = QRectF(20.0, 20.0, 555.276, 801.89);

    const QString cropped = path(QStringLiteral("cropped.pdf"));
    QString error;
    QVERIFY2(PageLayout::setBoxes(m_a4, cropped, { 0, 1 }, start, &error), qPrintable(error));

    const QString bled = path(QStringLiteral("bled.pdf"));
    QVERIFY2(PageLayout::setBleed(cropped, bled, { 0, 1 }, 9.0, &error), qPrintable(error));

    const QVector<PageLayout::Boxes> read = PageLayout::boxesOf(bled, &error);
    QVERIFY2(read.size() == 2, qPrintable(error));

    QCOMPARE(read.first().trim, start.crop);
    QCOMPARE(read.first().bleed, start.crop.adjusted(-9.0, -9.0, 9.0, 9.0));
    // The bleed still fits the sheet, so the sheet had no reason to grow.
    QCOMPARE(read.first().media, start.media);
    QCOMPARE(read.first().crop, start.crop);
}

void TestPagelayout::growsTheSheetToHoldTheBleed()
{
    const QString bled = path(QStringLiteral("bled-grown.pdf"));
    QString error;
    QVERIFY2(PageLayout::setBleed(m_letter, bled, { 0 }, 9.0, &error), qPrintable(error));

    const QVector<PageLayout::Boxes> read = PageLayout::boxesOf(bled, &error);
    QVERIFY2(read.size() == 3, qPrintable(error));

    // No crop box, so the trim is the whole sheet and the bleed has to run onto
    // paper the page did not have.
    QCOMPARE(read.first().trim, QRectF(0, 0, 612, 792));
    QCOMPARE(read.first().bleed, QRectF(-9, -9, 630, 810));
    QCOMPARE(read.first().media, QRectF(-9, -9, 630, 810));
    QCOMPARE(read.at(1).media, QRectF(0, 0, 612, 792));
}

void TestPagelayout::resizesAPageWithoutRewritingItsContent()
{
    const QString before = test::contentOf(m_letter, 0);
    const QString untouchedBefore = test::contentOf(m_letter, 1);
    QVERIFY(before.contains(QLatin1String("PSPAGE 1")));

    const QString out = path(QStringLiteral("a5.pdf"));
    QString error;
    QVERIFY2(PageLayout::resize(m_letter, out, { 0 }, QSizeF(kA5Width, kA5Height), PageLayout::Fit::Scale, &error),
             qPrintable(error));

    const QVector<PageLayout::Boxes> read = PageLayout::boxesOf(out, &error);
    QVERIFY2(read.size() == 3, qPrintable(error));
    QVERIFY(std::abs(read.first().media.width() - kA5Width) < 1e-6);
    QVERIFY(std::abs(read.first().media.height() - kA5Height) < 1e-6);

    // The page that was asked for keeps its instructions verbatim; only a
    // wrapper appears around them.
    const QString after = test::contentOf(out, 0);
    QVERIFY(after.contains(before));
    QVERIFY(after.startsWith(QLatin1String("q\n")));

    // The pages that were not asked for are not touched at all.
    QCOMPARE(read.at(1).media, QRectF(0, 0, 612, 792));
    QCOMPARE(test::contentOf(out, 1), untouchedBefore);
    QCOMPARE(test::contentOf(out, 2), test::contentOf(m_letter, 2));
}

void TestPagelayout::leavesTheImageOfAScanUntouchedWhenResizing()
{
    const QString source = scannedLetter();
    if (source.isEmpty()) {
        QSKIP("testdata/brief-1902.pdf has not been downloaded");
    }
    const QByteArray before = rawImageOf(source, 0);
    QVERIFY(!before.isEmpty());

    const QString out = path(QStringLiteral("brief-a5.pdf"));
    QString error;
    QVERIFY2(PageLayout::resize(source, out, { 0 }, QSizeF(kA5Width, kA5Height), PageLayout::Fit::Scale, &error),
             qPrintable(error));

    // Byte for byte the same JPEG. Resizing a scan by re-rendering it would be
    // the easy way to do this and would cost the document its only copy of the
    // original pixels.
    QCOMPARE(rawImageOf(out, 0), before);
}

void TestPagelayout::unifiesEveryPageToTheLargest()
{
    const QString mixed = path(QStringLiteral("mixed.pdf"));
    QString error;
    QVERIFY2(PageLayout::resize(m_letter, mixed, { 1 }, QSizeF(kA5Width, kA5Height), PageLayout::Fit::Scale, &error),
             qPrintable(error));

    const QString out = path(QStringLiteral("unified.pdf"));
    QVERIFY2(PageLayout::unify(mixed, out, QSizeF(), PageLayout::Fit::ScaleAndCentre, &error), qPrintable(error));

    const QVector<PageLayout::Boxes> read = PageLayout::boxesOf(out, &error);
    QVERIFY2(read.size() == 3, qPrintable(error));
    for (const PageLayout::Boxes &page : read) {
        QCOMPARE(page.media, QRectF(0, 0, 612, 792));
    }
    // Every page's text survives the move.
    for (int i = 0; i < 3; ++i) {
        QVERIFY(test::contentOf(out, i).contains(QStringLiteral("PSPAGE %1").arg(i + 1)));
    }
}

void TestPagelayout::splitsAPageIntoTwoHalvesOverTheSameContent()
{
    const QString out = path(QStringLiteral("split.pdf"));
    QString error;
    QVERIFY2(PageLayout::splitPages(m_letter, out, { 0 }, true, 0.5, &error), qPrintable(error));

    QCOMPARE(test::pageCountOf(out), 4);

    const QVector<PageLayout::Boxes> read = PageLayout::boxesOf(out, &error);
    QVERIFY2(read.size() == 4, qPrintable(error));
    QCOMPARE(read.at(0).media, QRectF(0, 0, 306, 792));
    QCOMPARE(read.at(0).crop, QRectF(0, 0, 306, 792));
    QCOMPARE(read.at(1).media, QRectF(306, 0, 306, 792));
    QCOMPARE(read.at(2).media, QRectF(0, 0, 612, 792));

    // Both halves are windows onto the one original stream, so the content is
    // not merely preserved: it is the same content.
    const QString original = test::contentOf(m_letter, 0);
    QCOMPARE(test::contentOf(out, 0), original);
    QCOMPARE(test::contentOf(out, 1), original);
    QVERIFY(test::contentOf(out, 2).contains(QLatin1String("PSPAGE 2")));
}

void TestPagelayout::refusesASplitAtTheVeryEdge()
{
    const QString out = path(QStringLiteral("split-edge.pdf"));
    QString error;
    QVERIFY(!PageLayout::splitPages(m_letter, out, { 0 }, true, 0.001, &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!QFile::exists(out));
}

void TestPagelayout::laysAnOverlayOverEveryBasePage()
{
    const QString overlay = path(QStringLiteral("overlay-source.pdf"));
    QVERIFY(test::writeSamplePdf(overlay, 1, QSizeF(612, 792)));

    const QString out = path(QStringLiteral("overlaid.pdf"));
    QString error;
    QVERIFY2(PageLayout::overlayPages(m_letter, overlay, out, true, &error), qPrintable(error));

    QCOMPARE(test::pageCountOf(out), 3);
    for (int i = 0; i < 3; ++i) {
        // The base page's own instructions are still there, unrewritten.
        QVERIFY(test::contentOf(out, i).contains(QStringLiteral("PSPAGE %1").arg(i + 1)));
        // And the overlay's single page has been placed on every one of them.
        QVERIFY2(formContentOf(out, i).contains(QLatin1String("PSPAGE 1")), qPrintable(QString::number(i)));
    }

    // Without repeating, only the pages the overlay reaches are changed.
    const QString once = path(QStringLiteral("overlaid-once.pdf"));
    QVERIFY2(PageLayout::overlayPages(m_letter, overlay, once, false, &error), qPrintable(error));
    QVERIFY(!formContentOf(once, 1).contains(QLatin1String("PSPAGE 1")));
    QCOMPARE(test::contentOf(once, 1), test::contentOf(m_letter, 1));
}

void TestPagelayout::addsPrintersMarksAndGrowsTheSheet()
{
    PageLayout::Marks marks;
    marks.cropMarks = true;
    marks.registrationMarks = true;
    marks.colourBars = true;
    marks.pageInformation = true;

    const QString out = path(QStringLiteral("marked.pdf"));
    QString error;
    QVERIFY2(PageLayout::addMarks(m_letter, out, marks, &error), qPrintable(error));

    const QVector<PageLayout::Boxes> read = PageLayout::boxesOf(out, &error);
    QVERIFY2(read.size() == 3, qPrintable(error));

    // Room for the offset and the mark itself on every side, and a visible area
    // that reaches out to it.
    QCOMPARE(read.first().media, QRectF(-24, -24, 660, 840));
    QCOMPARE(read.first().crop, QRectF(-24, -24, 660, 840));

    const QString content = test::contentOf(out, 0);
    QVERIFY(content.contains(QLatin1String("PSPAGE 1")));
    QVERIFY(content.contains(QLatin1String(" re f"))); // the colour bars
    QVERIFY(content.contains(QLatin1String(" c\n"))); // the registration rings
    QVERIFY(content.contains(QLatin1String(" l S"))); // the crop marks
    QVERIFY(content.contains(QLatin1String("letter.pdf"))); // the page information

    // The marks must be set in a colour that appears on every plate.
    try {
        QPDF pdf;
        pdf.processFile(QFile::encodeName(out).constData());
        QPDFObjectHandle spaces
            = QPDFPageDocumentHelper(pdf).getAllPages().front().getAttribute("/Resources", false).getKey("/ColorSpace");
        QVERIFY(spaces.isDictionary());
        bool foundAllInk = false;
        for (const std::string &key : spaces.getKeys()) {
            QPDFObjectHandle space = spaces.getKey(key);
            if (space.isArray() && space.getArrayNItems() == 4 && space.getArrayItem(0).isName()
                && space.getArrayItem(0).getName() == "/Separation" && space.getArrayItem(1).isName()
                && space.getArrayItem(1).getName() == "/All") {
                foundAllInk = true;
            }
        }
        QVERIFY(foundAllInk);
    } catch (const std::exception &e) {
        QFAIL(e.what());
    }
}

void TestPagelayout::refusesToAddNoMarksAtAll()
{
    const QString out = path(QStringLiteral("unmarked.pdf"));
    QString error;
    QVERIFY(!PageLayout::addMarks(m_letter, out, PageLayout::Marks(), &error));
    QVERIFY(!error.isEmpty());
}

void TestPagelayout::tilesOnePageOntoFourSheets()
{
    const QString out = path(QStringLiteral("poster.pdf"));
    QString error;
    int sheets = 0;
    QVERIFY2(PageLayout::poster(m_a4, out, 0, QSizeF(kA5Width, kA5Height), 0.0, true, &sheets, &error),
             qPrintable(error));

    // An A4 page needs two A5 sheets across and two up, whichever way the
    // arithmetic is rounded.
    QCOMPARE(sheets, 4);
    QCOMPARE(test::pageCountOf(out), 4);

    const QVector<PageLayout::Boxes> read = PageLayout::boxesOf(out, &error);
    QVERIFY2(read.size() == 4, qPrintable(error));
    for (const PageLayout::Boxes &sheet : read) {
        QVERIFY(std::abs(sheet.media.width() - kA5Width) < 1e-6);
        QVERIFY(std::abs(sheet.media.height() - kA5Height) < 1e-6);
    }

    // Every sheet draws the page, and the page's text went with it.
    for (int i = 0; i < 4; ++i) {
        QVERIFY(test::contentOf(out, i).contains(QLatin1String("/PsTile Do")));
        QVERIFY(formContentOf(out, i).contains(QLatin1String("PSPAGE 1")));
    }
}

void TestPagelayout::compressesTheFormItBuildsForATile()
{
    const QString out = path(QStringLiteral("poster-filters.pdf"));
    QString error;
    int sheets = 0;
    QVERIFY2(PageLayout::poster(m_a4, out, 0, QSizeF(kA5Width, kA5Height), 12.0, false, &sheets, &error),
             qPrintable(error));

    // The form built out of a page arrives with no filter on it. Writing it that
    // way turns a five-megabyte scan into a fifty-megabyte poster, and preserving
    // stream data is exactly what would do so.
    const QStringList filters = formFilters(out);
    QVERIFY(!filters.isEmpty());
    for (const QString &filter : filters) {
        QCOMPARE(filter, QStringLiteral("/FlateDecode"));
    }
}

void TestPagelayout::writesAndReadsRomanFrontMatter()
{
    const QString source = path(QStringLiteral("five.pdf"));
    QVERIFY(test::writeSamplePdf(source, 5, QSizeF(612, 792)));

    PageLayout::Labels front;
    front.fromPage = 0;
    front.style = PageLayout::Labels::Style::RomanLower;
    front.startAt = 1;

    PageLayout::Labels body;
    body.fromPage = 3;
    body.style = PageLayout::Labels::Style::Decimal;
    body.startAt = 1;

    const QString out = path(QStringLiteral("labelled.pdf"));
    QString error;
    QVERIFY2(PageLayout::setLabels(source, out, { front, body }, &error), qPrintable(error));

    const QStringList labels = PageLayout::labelsOf(out, &error);
    QVERIFY2(!labels.isEmpty(), qPrintable(error));
    QCOMPARE(labels,
             QStringList({ QStringLiteral("i"), QStringLiteral("ii"), QStringLiteral("iii"), QStringLiteral("1"),
                           QStringLiteral("2") }));

    // A prefix and a starting number, which is how an appendix is numbered.
    PageLayout::Labels appendix;
    appendix.fromPage = 3;
    appendix.style = PageLayout::Labels::Style::LetterUpper;
    appendix.prefix = QStringLiteral("Anhang ");
    appendix.startAt = 1;

    const QString second = path(QStringLiteral("labelled2.pdf"));
    QVERIFY2(PageLayout::setLabels(source, second, { front, appendix }, &error), qPrintable(error));
    const QStringList mixed = PageLayout::labelsOf(second, &error);
    QCOMPARE(mixed.size(), 5);
    QCOMPARE(mixed.at(2), QStringLiteral("iii"));
    QCOMPARE(mixed.at(3), QStringLiteral("Anhang A"));
    QCOMPARE(mixed.at(4), QStringLiteral("Anhang B"));
}

void TestPagelayout::reportsNoLabelsWhenTheDocumentHasNone()
{
    QString error;
    // Not invented numbers: a document without /PageLabels has no labels, and
    // saying so is what lets a caller offer to add them.
    QVERIFY(PageLayout::labelsOf(m_letter, &error).isEmpty());
    QVERIFY(error.isEmpty());
}

void TestPagelayout::findsABlankPageAmongTextPages()
{
    const QString source = path(QStringLiteral("withblank.pdf"));
    QVERIFY(writeWithBlankPages(source, 5, 2, -1));

    QString error;
    const QVector<int> blanks = PageLayout::findBlankPages(source, 0.0, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(blanks, QVector<int>({ 2 }));
}

void TestPagelayout::treatsWhitespaceOnlyTextAsBlank()
{
    const QString source = path(QStringLiteral("withspaces.pdf"));
    QVERIFY(writeWithBlankPages(source, 4, 1, 3));

    QString error;
    const QVector<int> blanks = PageLayout::findBlankPages(source, 0.0, &error);
    QCOMPARE(blanks, QVector<int>({ 1, 3 }));

    const QString out = path(QStringLiteral("withoutblanks.pdf"));
    QVERIFY2(PageLayout::removePages(source, out, blanks, &error), qPrintable(error));
    QCOMPARE(test::pageCountOf(out), 2);
    QVERIFY(test::contentOf(out, 0).contains(QLatin1String("PSPAGE 1")));
    QVERIFY(test::contentOf(out, 1).contains(QLatin1String("PSPAGE 3")));
}

void TestPagelayout::removesPagesButNotAllOfThem()
{
    const QString out = path(QStringLiteral("emptied.pdf"));
    QString error;
    QVERIFY(!PageLayout::removePages(m_letter, out, { 0, 1, 2 }, &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!QFile::exists(out));
}

void TestPagelayout::reordersEightPagesIntoPrinterSpreads()
{
    const QString source = path(QStringLiteral("eight.pdf"));
    QVERIFY(test::writeSamplePdf(source, 8, QSizeF(612, 792)));

    const QString out = path(QStringLiteral("spreads.pdf"));
    QString error;
    QVERIFY2(PageLayout::reorderForBinding(source, out, true, &error), qPrintable(error));

    QCOMPARE(test::pageCountOf(out), 8);
    const QVector<int> expected { 8, 1, 2, 7, 6, 3, 4, 5 };
    for (int slot = 0; slot < expected.size(); ++slot) {
        QVERIFY2(test::contentOf(out, slot).contains(QStringLiteral("PSPAGE %1").arg(expected.at(slot))),
                 qPrintable(QStringLiteral("slot %1 should hold page %2").arg(slot).arg(expected.at(slot))));
    }
}

void TestPagelayout::takesPrinterSpreadsBackToReaderOrder()
{
    const QString source = path(QStringLiteral("eight2.pdf"));
    QVERIFY(test::writeSamplePdf(source, 8, QSizeF(612, 792)));

    const QString spreads = path(QStringLiteral("spreads2.pdf"));
    const QString back = path(QStringLiteral("readerorder.pdf"));
    QString error;
    QVERIFY2(PageLayout::reorderForBinding(source, spreads, true, &error), qPrintable(error));
    QVERIFY2(PageLayout::reorderForBinding(spreads, back, false, &error), qPrintable(error));

    for (int i = 0; i < 8; ++i) {
        QVERIFY(test::contentOf(back, i).contains(QStringLiteral("PSPAGE %1").arg(i + 1)));
    }
}

void TestPagelayout::padsAnIncompleteBookletToAMultipleOfFour()
{
    const QString source = path(QStringLiteral("six.pdf"));
    QVERIFY(test::writeSamplePdf(source, 6, QSizeF(612, 792)));

    const QString out = path(QStringLiteral("sixspreads.pdf"));
    QString error;
    QVERIFY2(PageLayout::reorderForBinding(source, out, true, &error), qPrintable(error));

    // A folded sheet carries four pages, so six pages become eight and the two
    // blanks land inside the last fold rather than at the front.
    QCOMPARE(test::pageCountOf(out), 8);
    const QVector<PageLayout::Boxes> read = PageLayout::boxesOf(out, &error);
    QCOMPARE(read.size(), 8);
    for (const PageLayout::Boxes &page : read) {
        QCOMPARE(page.media, QRectF(0, 0, 612, 792));
    }
    QVERIFY(test::contentOf(out, 1).contains(QLatin1String("PSPAGE 1")));

    const QVector<int> blanks = PageLayout::findBlankPages(out, 0.0, &error);
    QCOMPARE(blanks.size(), 2);
}

void TestPagelayout::namesItsLimitations()
{
    const QStringList notes = PageLayout::limitations();
    QVERIFY(notes.size() >= 5);
    for (const QString &note : notes) {
        QVERIFY(!note.trimmed().isEmpty());
    }
}

QTEST_MAIN(TestPagelayout)

#include "tst_pagelayout.moc"
