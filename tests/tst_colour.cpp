/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#include "core/ColourTools.h"
#include "core/PdfFile.h"
#include "core/PdfGeometry.h"
#include "render/PopplerBackend.h"

#include "TestPdf.h"

#include <KLocalizedString>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QSet>
#include <QTemporaryDir>
#include <QTest>
#include <QtEnvironmentVariables>

#include <qpdf/Buffer.hh>
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFWriter.hh>

#include <cmath>
#include <functional>

using namespace ps;
using namespace Qt::Literals::StringLiterals;

/**
 * Whether a colour conversion converted or destroyed.
 *
 * Every case here reads the produced file back: either its content stream, its
 * stored image samples, or a rendering of it. That matters more here than
 * elsewhere: a colour conversion that flattened the page to a grey rectangle
 * would satisfy any test that only asked "is it grey now?". So the greyscale
 * case asks two questions at once (is every pixel neutral, and is the page
 * still as bright as it was), and neither can be passed by destroying the
 * content.
 */
class TestColour : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();

    void readsTheColourOfARealMagazine();
    void greyscaleKeepsTheBrightnessOfAScan();
    void greyscaleWritesRealGreyIntoTheFile();
    void blackAndWhiteLeavesOnlyTwoTones();
    void blackAndWhiteStoresOneBitPerPixel();
    void leavesUnselectedPagesByteIdentical();
    void rewritesAnIndexedPaletteAndNotItsPixels();
    void replacesOneColourAndLeavesItsNeighboursAlone();
    void raisesAHairlineToTheMinimum();
    void countsHairlinesOnlyWhereSomethingIsStroked();
    void turnsASpotColourIntoProcessInk();
    void mergesTwoSeparationsByRenamingThem();
    void insertsOverprintOnlyWhereBlackIsSet();
    void convertsToCmykThroughAnIccProfile();
    void refusesAPartialCmykConversionWithoutAProfile();
    void measuresInkCoverageAgainstAKnownFill();
    void separatesTheBlackPlateOfAKnownFill();
    void reportsItsOwnLimits();

private:
    /** A document of @p contents, one page each, 200 by 200 points. */
    QString writePdf(const QString &name, const QStringList &contents,
                     const std::function<void(QPDF &, QPDFObjectHandle)> &decorate = {});

    /** Rectangles in red, green, blue and half grey, on white, plus a hairline. */
    static QString colourfulPage();

    /** A one-page document painting a separation at half tint. */
    QString writeSpotPdf(const QString &name, const QStringList &inks);

    /** A one-page document showing a four-pixel indexed image over the whole page. */
    QString writeIndexedPdf(const QString &name);

    static QImage render(const QString &path, int page, int widthPx);

    /** The first image XObject of a page, as the produced file stores it. */
    static QPDFObjectHandle firstImageOf(QPDF &pdf, int page);

    QTemporaryDir m_directory;
};

void TestColour::initTestCase()
{
    KLocalizedString::setApplicationDomain("pdf-smithy");
    QVERIFY(m_directory.isValid());
}

QString TestColour::writePdf(const QString &name, const QStringList &contents,
                             const std::function<void(QPDF &, QPDFObjectHandle)> &decorate)
{
    const QString path = m_directory.filePath(name);
    QPDF pdf;
    pdf.emptyPDF();
    QPDFPageDocumentHelper documents(pdf);

    for (const QString &content : contents) {
        QPDFObjectHandle resources = QPDFObjectHandle::newDictionary();
        if (decorate) {
            decorate(pdf, resources);
        }

        QPDFObjectHandle box = QPDFObjectHandle::newArray();
        for (const int edge : { 0, 0, 200, 200 }) {
            box.appendItem(QPDFObjectHandle::newInteger(edge));
        }

        QPDFObjectHandle page = QPDFObjectHandle::newDictionary();
        page.replaceKey("/Type", QPDFObjectHandle::newName("/Page"));
        page.replaceKey("/MediaBox", box);
        page.replaceKey("/Resources", resources);
        page.replaceKey("/Contents", QPDFObjectHandle::newStream(&pdf, content.toStdString()));
        documents.addPage(pdf.makeIndirectObject(page), false);
    }

    QPDFWriter writer(pdf, QFile::encodeName(path).constData());
    writer.write();
    return path;
}

QString TestColour::colourfulPage()
{
    // Chosen so that a luminance threshold of a half sends red and blue and the
    // mid grey to black while green goes to white. The outcome is arithmetic
    // rather than a matter of taste, so the assertion can be exact.
    return u"1 0 0 rg 10 10 40 40 re f\n"
           "0 1 0 rg 60 10 40 40 re f\n"
           "0 0 1 rg 110 10 40 40 re f\n"
           "0.5 g 10 60 40 40 re f\n"
           "0 w 0 0 0 RG 10 150 m 190 150 l S\n"_s;
}

QString TestColour::writeSpotPdf(const QString &name, const QStringList &inks)
{
    QString content;
    for (int i = 0; i < inks.size(); ++i) {
        content += u"/CS%1 cs 0.5 scn %2 10 40 40 re f\n"_s.arg(i).arg(10 + i * 50);
    }

    const auto decorate = [&inks](QPDF &pdf, QPDFObjectHandle resources) {
        QPDFObjectHandle spaces = QPDFObjectHandle::newDictionary();
        for (int i = 0; i < inks.size(); ++i) {
            QPDFObjectHandle zero = QPDFObjectHandle::newArray();
            QPDFObjectHandle one = QPDFObjectHandle::newArray();
            for (int component = 0; component < 4; ++component) {
                zero.appendItem(QPDFObjectHandle::newInteger(0));
            }
            // Cyan-heavy, so the process equivalent at half tint is a number a
            // test can predict: 0.5 and 0.28 of the two inks that are used.
            one.appendItem(QPDFObjectHandle::newInteger(1));
            one.appendItem(QPDFObjectHandle::newReal(PdfGeometry::number(0.56)));
            one.appendItem(QPDFObjectHandle::newInteger(0));
            one.appendItem(QPDFObjectHandle::newInteger(0));

            QPDFObjectHandle domain = QPDFObjectHandle::newArray();
            domain.appendItem(QPDFObjectHandle::newInteger(0));
            domain.appendItem(QPDFObjectHandle::newInteger(1));

            QPDFObjectHandle tint = QPDFObjectHandle::newDictionary();
            tint.replaceKey("/FunctionType", QPDFObjectHandle::newInteger(2));
            tint.replaceKey("/Domain", domain);
            tint.replaceKey("/C0", zero);
            tint.replaceKey("/C1", one);
            tint.replaceKey("/N", QPDFObjectHandle::newInteger(1));

            QPDFObjectHandle separation = QPDFObjectHandle::newArray();
            separation.appendItem(QPDFObjectHandle::newName("/Separation"));
            separation.appendItem(QPDFObjectHandle::newName("/" + inks.at(i).toStdString()));
            separation.appendItem(QPDFObjectHandle::newName("/DeviceCMYK"));
            separation.appendItem(pdf.makeIndirectObject(tint));

            spaces.replaceKey(u"/CS%1"_s.arg(i).toStdString(), separation);
        }
        resources.replaceKey("/ColorSpace", spaces);
    };

    return writePdf(name, { content }, decorate);
}

QString TestColour::writeIndexedPdf(const QString &name)
{
    const auto decorate = [](QPDF &pdf, QPDFObjectHandle resources) {
        // Four pixels, four palette entries: red, green, blue, white.
        const std::string palette("\xff\x00\x00\x00\xff\x00\x00\x00\xff\xff\xff\xff", 12);
        const std::string pixels("\x00\x01\x02\x03", 4);

        QPDFObjectHandle space = QPDFObjectHandle::newArray();
        space.appendItem(QPDFObjectHandle::newName("/Indexed"));
        space.appendItem(QPDFObjectHandle::newName("/DeviceRGB"));
        space.appendItem(QPDFObjectHandle::newInteger(3));
        space.appendItem(QPDFObjectHandle::newString(palette));

        QPDFObjectHandle image = QPDFObjectHandle::newStream(&pdf, pixels);
        QPDFObjectHandle dict = image.getDict();
        dict.replaceKey("/Type", QPDFObjectHandle::newName("/XObject"));
        dict.replaceKey("/Subtype", QPDFObjectHandle::newName("/Image"));
        dict.replaceKey("/Width", QPDFObjectHandle::newInteger(4));
        dict.replaceKey("/Height", QPDFObjectHandle::newInteger(1));
        dict.replaceKey("/BitsPerComponent", QPDFObjectHandle::newInteger(8));
        dict.replaceKey("/ColorSpace", space);

        QPDFObjectHandle table = QPDFObjectHandle::newDictionary();
        table.replaceKey("/Im0", image);
        resources.replaceKey("/XObject", table);
    };

    return writePdf(name, { u"q 200 0 0 200 0 0 cm /Im0 Do Q\n"_s }, decorate);
}

QImage TestColour::render(const QString &path, int page, int widthPx)
{
    PopplerBackend backend;
    QString error;
    if (!backend.addDocument(1, path, &error)) {
        return {};
    }
    return backend.renderPage(1, page, widthPx);
}

QPDFObjectHandle TestColour::firstImageOf(QPDF &pdf, int page)
{
    std::vector<QPDFPageObjectHelper> pages = QPDFPageDocumentHelper(pdf).getAllPages();
    if (page < 0 || size_t(page) >= pages.size()) {
        return QPDFObjectHandle::newNull();
    }
    QPDFObjectHandle resources = pages.at(size_t(page)).getAttribute("/Resources", false);
    QPDFObjectHandle table = resources.isDictionary() ? resources.getKey("/XObject") : QPDFObjectHandle::newNull();
    if (!table.isDictionary()) {
        return QPDFObjectHandle::newNull();
    }
    for (const auto &[name, object] : table.getDictAsMap()) {
        Q_UNUSED(name)
        if (!object.isStream()) {
            continue;
        }
        QPDFObjectHandle subtype = object.getDict().getKey("/Subtype");
        if (subtype.isName() && subtype.getName() == "/Image") {
            return object;
        }
    }
    return QPDFObjectHandle::newNull();
}

// ══ inspection ════════════════════════════════════════════════════════════

void TestColour::readsTheColourOfARealMagazine()
{
    const QString magazine = qEnvironmentVariable("PS_STRESS_PDF");
    if (!QFileInfo::exists(magazine)) {
        QSKIP("set PS_STRESS_PDF to a large real document to run this");
    }

    QString error;
    const ColourTools::Inventory inventory = ColourTools::inspect(magazine, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));

    QVERIFY(inventory.pages > 100);
    // An offset-printed magazine placed as RGB with ICC profiles on the images:
    // if none of that is seen, the walk is not reaching the content.
    QVERIFY2(inventory.pagesWithRgb > inventory.pages / 2, "hardly any page reports RGB colour");
    QVERIFY2(inventory.hasIccProfile, "the ICC profiles on the images were missed");
    QVERIFY2(inventory.usesOverprint, "the overprint in this file was missed");
    QVERIFY(inventory.spaces.contains(u"ICCBased (3 components)"_s));
    // This issue turns out to be cleanly set: its finest rule measures 0.288 pt,
    // just above the quarter point below which a stroke stops printing reliably.
    // So the thing worth asserting is not that hairlines were found, because a
    // well-produced magazine has none, and demanding some would only mean the
    // test passes when the file is bad. It is that the walk reached the strokes
    // at all, which the count and the measured minimum together establish; a
    // stream walk that quietly stops early reports zero hairlines too, and
    // that is the failure this guards against.
    QVERIFY2(inventory.strokes > 1000, "the walk barely reached any stroked art");
    QVERIFY2(
        inventory.thinnestStroke > 0.0 && inventory.thinnestStroke < 1.0,
        qPrintable(u"the thinnest stroke measured %1 pt, which is not a printed rule"_s.arg(inventory.thinnestStroke)));
    QCOMPARE(inventory.hairlines, 0);
}

// ══ greyscale, the load-bearing case ══════════════════════════════════════

void TestColour::greyscaleKeepsTheBrightnessOfAScan()
{
    const QString scan = QTest::qFindTestData(u"../testdata/brief-1902.pdf"_s, __FILE__, __LINE__);
    if (scan.isEmpty()) {
        QSKIP("testdata/brief-1902.pdf has not been downloaded");
    }

    const QString out = m_directory.filePath(u"scan-grey.pdf"_s);
    ColourTools::ConvertOptions options;
    options.target = ColourTools::Target::Grayscale;
    options.pages = { 0 };

    QStringList changes;
    QString error;
    QVERIFY2(ColourTools::convert(scan, out, options, &changes, &error), qPrintable(error));
    QVERIFY(!changes.isEmpty());

    const QImage before = render(scan, 0, 400);
    const QImage after = render(out, 0, 400);
    QVERIFY(!before.isNull());
    QVERIFY(!after.isNull());
    QCOMPARE(after.size(), before.size());

    double sumBefore = 0.0;
    double sumAfter = 0.0;
    qint64 counted = 0;
    int worstSpread = 0;
    for (int y = 0; y < after.height(); ++y) {
        for (int x = 0; x < after.width(); ++x) {
            const QRgb source = before.pixel(x, y);
            const QRgb result = after.pixel(x, y);
            sumBefore += qGray(source);
            sumAfter += qGray(result);
            ++counted;
            worstSpread
                = std::max(worstSpread,
                           std::max(std::abs(qRed(result) - qGreen(result)), std::abs(qGreen(result) - qBlue(result))));
        }
    }
    QVERIFY(counted > 0);

    QVERIFY2(worstSpread <= 2, qPrintable(u"a pixel is still coloured: channels differ by %1"_s.arg(worstSpread)));

    const double meanBefore = sumBefore / double(counted);
    const double meanAfter = sumAfter / double(counted);
    QVERIFY2(meanBefore > 20.0, "the source render is nearly black, so the comparison means nothing");
    const double drift = std::abs(meanAfter - meanBefore) / meanBefore;
    QVERIFY2(drift < 0.05,
             qPrintable(u"brightness moved from %1 to %2, which is %3 per cent"_s.arg(meanBefore)
                            .arg(meanAfter)
                            .arg(drift * 100.0)));
}

void TestColour::greyscaleWritesRealGreyIntoTheFile()
{
    const QString scan = QTest::qFindTestData(u"../testdata/brief-1902.pdf"_s, __FILE__, __LINE__);
    if (scan.isEmpty()) {
        QSKIP("testdata/brief-1902.pdf has not been downloaded");
    }

    const QString out = m_directory.filePath(u"scan-grey-stored.pdf"_s);
    ColourTools::ConvertOptions options;
    options.target = ColourTools::Target::Grayscale;
    options.pages = { 0 };
    QString error;
    QVERIFY2(ColourTools::convert(scan, out, options, nullptr, &error), qPrintable(error));

    QPDF pdf;
    PdfFile::open(pdf, out);

    // The point of asking the file rather than the renderer: three equal
    // channels would look right and still be a colour document.
    QPDFObjectHandle converted = firstImageOf(pdf, 0);
    QVERIFY(converted.isStream());
    QPDFObjectHandle space = converted.getDict().getKey("/ColorSpace");
    QVERIFY(space.isName());
    QCOMPARE(QString::fromStdString(space.getName()), u"/DeviceGray"_s);

    // And a page that was not asked for keeps its colour, rather than the whole
    // document quietly going grey.
    QPDFObjectHandle untouched = firstImageOf(pdf, 1);
    QVERIFY(untouched.isStream());
    QPDFObjectHandle otherSpace = untouched.getDict().getKey("/ColorSpace");
    QVERIFY(otherSpace.isName());
    QCOMPARE(QString::fromStdString(otherSpace.getName()), u"/DeviceRGB"_s);
}

// ══ black and white ═══════════════════════════════════════════════════════

void TestColour::blackAndWhiteLeavesOnlyTwoTones()
{
    const QString in = writePdf(u"bw-in.pdf"_s, { colourfulPage() });
    const QString out = m_directory.filePath(u"bw-out.pdf"_s);

    ColourTools::ConvertOptions options;
    options.target = ColourTools::Target::BlackWhite;
    QString error;
    QVERIFY2(ColourTools::convert(in, out, options, nullptr, &error), qPrintable(error));

    // One pixel per point, so the axis-aligned rectangles land on whole pixels
    // and any extra grey value would come from the conversion rather than from
    // the renderer smoothing an edge.
    const QImage rendered = render(out, 0, 200);
    QVERIFY(!rendered.isNull());

    QSet<int> tones;
    for (int y = 0; y < rendered.height(); ++y) {
        for (int x = 0; x < rendered.width(); ++x) {
            tones.insert(qGray(rendered.pixel(x, y)));
        }
    }
    QVERIFY2(tones.size() <= 4, qPrintable(u"%1 distinct grey values survived"_s.arg(tones.size())));

    // Red at 0.3 luminance and blue at 0.11 fall below the threshold; green at
    // 0.59 rises above it. That is the whole conversion, checked at four points.
    QCOMPARE(qGray(rendered.pixel(30, 200 - 30)), 0);
    QCOMPARE(qGray(rendered.pixel(80, 200 - 30)), 255);
    QCOMPARE(qGray(rendered.pixel(130, 200 - 30)), 0);
    QCOMPARE(qGray(rendered.pixel(30, 200 - 80)), 0);
}

void TestColour::blackAndWhiteStoresOneBitPerPixel()
{
    const QString scan = QTest::qFindTestData(u"../testdata/brief-1902.pdf"_s, __FILE__, __LINE__);
    if (scan.isEmpty()) {
        QSKIP("testdata/brief-1902.pdf has not been downloaded");
    }

    const QString out = m_directory.filePath(u"scan-bw.pdf"_s);
    ColourTools::ConvertOptions options;
    options.target = ColourTools::Target::BlackWhite;
    options.pages = { 0 };
    QString error;
    QVERIFY2(ColourTools::convert(scan, out, options, nullptr, &error), qPrintable(error));

    QPDF pdf;
    PdfFile::open(pdf, out);
    QPDFObjectHandle image = firstImageOf(pdf, 0);
    QVERIFY(image.isStream());
    QPDFObjectHandle bits = image.getDict().getKey("/BitsPerComponent");
    QVERIFY(bits.isInteger());
    QCOMPARE(bits.getIntValueAsInt(), 1);
    QPDFObjectHandle space = image.getDict().getKey("/ColorSpace");
    QVERIFY(space.isName());
    QCOMPARE(QString::fromStdString(space.getName()), u"/DeviceGray"_s);
}

// ══ what must not change ══════════════════════════════════════════════════

void TestColour::leavesUnselectedPagesByteIdentical()
{
    const QString page = colourfulPage();
    const QString in = writePdf(u"three-in.pdf"_s, { page, page, page });
    const QString out = m_directory.filePath(u"three-out.pdf"_s);

    ColourTools::ConvertOptions options;
    options.target = ColourTools::Target::Grayscale;
    options.pages = { 1 };
    QString error;
    QVERIFY2(ColourTools::convert(in, out, options, nullptr, &error), qPrintable(error));

    QCOMPARE(test::pageCountOf(out), 3);
    QCOMPARE(test::contentOf(out, 0), test::contentOf(in, 0));
    QCOMPARE(test::contentOf(out, 2), test::contentOf(in, 2));

    const QString converted = test::contentOf(out, 1);
    QVERIFY(converted != test::contentOf(in, 1));
    QVERIFY2(!converted.contains(u"1 0 0 rg"_s), "the red fill survived on the converted page");
    QVERIFY2(converted.contains(u" g\n"_s), "no grey operator was written");
}

void TestColour::rewritesAnIndexedPaletteAndNotItsPixels()
{
    const QString in = writeIndexedPdf(u"indexed-in.pdf"_s);
    const QString out = m_directory.filePath(u"indexed-out.pdf"_s);

    ColourTools::ConvertOptions options;
    options.target = ColourTools::Target::Grayscale;
    QStringList changes;
    QString error;
    QVERIFY2(ColourTools::convert(in, out, options, &changes, &error), qPrintable(error));

    QPDF source;
    PdfFile::open(source, in);
    QPDF result;
    PdfFile::open(result, out);

    QPDFObjectHandle beforeImage = firstImageOf(source, 0);
    QPDFObjectHandle afterImage = firstImageOf(result, 0);
    QVERIFY(beforeImage.isStream());
    QVERIFY(afterImage.isStream());

    const std::shared_ptr<Buffer> beforeBytes = beforeImage.getStreamData();
    const std::shared_ptr<Buffer> afterBytes = afterImage.getStreamData();
    const QByteArray beforeSamples(reinterpret_cast<const char *>(beforeBytes->getBuffer()),
                                   qsizetype(beforeBytes->getSize()));
    const QByteArray afterSamples(reinterpret_cast<const char *>(afterBytes->getBuffer()),
                                  qsizetype(afterBytes->getSize()));
    QCOMPARE(afterSamples, beforeSamples);

    QPDFObjectHandle space = afterImage.getDict().getKey("/ColorSpace");
    QVERIFY(space.isArray());
    QCOMPARE(space.getArrayNItems(), 4);
    QVERIFY(space.getArrayItem(1).isName());
    QCOMPARE(QString::fromStdString(space.getArrayItem(1).getName()), u"/DeviceGray"_s);
    QVERIFY(space.getArrayItem(3).isString());
    // One byte per entry now, where there were three.
    QCOMPARE(qsizetype(space.getArrayItem(3).getStringValue().size()), qsizetype(4));

    // Red becomes 0.3 of full, green 0.59, blue 0.11, white stays white.
    const std::string lookup = space.getArrayItem(3).getStringValue();
    QCOMPARE(int(static_cast<uchar>(lookup.at(0))), 77);
    QCOMPARE(int(static_cast<uchar>(lookup.at(1))), 150);
    QCOMPARE(int(static_cast<uchar>(lookup.at(2))), 28);
    QCOMPARE(int(static_cast<uchar>(lookup.at(3))), 255);
}

// ══ replacing one colour ══════════════════════════════════════════════════

void TestColour::replacesOneColourAndLeavesItsNeighboursAlone()
{
    const QString in
        = writePdf(u"replace-in.pdf"_s, { u"1 0 0 rg 10 10 80 80 re f\n0 0.6 0 rg 110 110 80 80 re f\n"_s });
    const QString out = m_directory.filePath(u"replace-out.pdf"_s);

    int replaced = 0;
    QString error;
    QVERIFY2(ColourTools::replaceColour(in, out, QColor(Qt::red), QColor(Qt::blue), 0.0, &replaced, &error),
             qPrintable(error));
    QCOMPARE(replaced, 1);

    const QImage rendered = render(out, 0, 200);
    QVERIFY(!rendered.isNull());

    const QColor changedPixel = rendered.pixelColor(50, 200 - 50);
    QVERIFY2(changedPixel.blue() > 200 && changedPixel.red() < 40,
             qPrintable(u"the red rectangle came out %1"_s.arg(changedPixel.name())));

    const QColor neighbour = rendered.pixelColor(150, 200 - 150);
    QVERIFY2(neighbour.green() > 100 && neighbour.red() < 40 && neighbour.blue() < 40,
             qPrintable(u"the green rectangle was disturbed: %1"_s.arg(neighbour.name())));

    // White paper between them is untouched, which rules out a wholesale repaint.
    QCOMPARE(rendered.pixelColor(100, 200 - 100).rgb(), QColor(Qt::white).rgb());
}

// ══ hairlines ═════════════════════════════════════════════════════════════

void TestColour::raisesAHairlineToTheMinimum()
{
    const QString in = writePdf(u"hair-in.pdf"_s, { u"0 w 0 0 0 RG 20 100 m 180 100 l S\n"_s });
    const QString out = m_directory.filePath(u"hair-out.pdf"_s);

    QVERIFY(test::contentOf(in, 0).contains(u"0 w"_s));

    int fixed = 0;
    QString error;
    QVERIFY2(ColourTools::fixHairlines(in, out, 0.5, &fixed, &error), qPrintable(error));
    QCOMPARE(fixed, 1);

    const QString content = test::contentOf(out, 0);
    QVERIFY2(content.contains(u"0.5000 w"_s), qPrintable(u"the width was not raised: %1"_s.arg(content)));
    QVERIFY(content.contains(u"20 100 m"_s));
    QVERIFY(content.contains(u"S"_s));
}

void TestColour::countsHairlinesOnlyWhereSomethingIsStroked()
{
    // A thin width set and then never used is not a hairline; a thin width that
    // strokes twice is two of them. Counting the operator instead of the stroke
    // would get both of those wrong.
    const QString document = writePdf(u"hair-count.pdf"_s,
                                      { u"0.1 w\n0 0 0 RG 10 20 m 190 20 l S\n10 40 m 190 40 l S\n"
                                        "1 w 10 60 m 190 60 l S\n0.05 w 10 80 m 190 80 l\n"_s });

    QString error;
    const ColourTools::Inventory inventory = ColourTools::inspect(document, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(inventory.hairlines, 2);
}

// ══ separations ═══════════════════════════════════════════════════════════

void TestColour::turnsASpotColourIntoProcessInk()
{
    const QString in = writeSpotPdf(u"spot-in.pdf"_s, { u"Pantone 300 C"_s });
    const QString out = m_directory.filePath(u"spot-out.pdf"_s);

    QString error;
    ColourTools::Inventory before = ColourTools::inspect(in, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(before.spotColours, QStringList { u"Pantone 300 C"_s });

    int changed = 0;
    QVERIFY2(ColourTools::mapSpotColours(in, out, {}, { u"Pantone 300 C"_s }, &changed, &error), qPrintable(error));
    QCOMPARE(changed, 1);

    // The tint transform is the document's own, so the result is arithmetic and
    // can be asserted exactly: half of [1 0.56 0 0].
    const QString content = test::contentOf(out, 0);
    QVERIFY2(content.contains(u"0.5000 0.2800 0.0000 0.0000 k"_s),
             qPrintable(u"the separation was not dissolved: %1"_s.arg(content)));
    QVERIFY2(!content.contains(u"scn"_s), "a spot colour operator survived");

    const ColourTools::Inventory after = ColourTools::inspect(out, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY2(after.spotColours.isEmpty(),
             qPrintable(u"the plate is still there: %1"_s.arg(after.spotColours.join(u", "_s))));
}

void TestColour::mergesTwoSeparationsByRenamingThem()
{
    const QString in = writeSpotPdf(u"merge-in.pdf"_s, { u"Spot A"_s, u"Spot B"_s });
    const QString out = m_directory.filePath(u"merge-out.pdf"_s);

    QString error;
    const ColourTools::Inventory before = ColourTools::inspect(in, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(before.spotColours.size(), 2);

    QHash<QString, QString> renames;
    renames.insert(u"Spot A"_s, u"Merged"_s);
    renames.insert(u"Spot B"_s, u"Merged"_s);

    int changed = 0;
    QVERIFY2(ColourTools::mapSpotColours(in, out, renames, {}, &changed, &error), qPrintable(error));
    QCOMPARE(changed, 2);

    const ColourTools::Inventory after = ColourTools::inspect(out, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    // Two colour spaces, but one plate, which is exactly what a merge is.
    QCOMPARE(after.spotColours, QStringList { u"Merged"_s });
    QVERIFY2(test::contentOf(out, 0).contains(u"scn"_s), "the inks stopped being spot colours");
}

// ══ overprint ═════════════════════════════════════════════════════════════

void TestColour::insertsOverprintOnlyWhereBlackIsSet()
{
    const QString in = writePdf(u"op-in.pdf"_s,
                                { u"0 g 10 10 50 50 re f\n1 0 0 rg 100 100 50 50 re f\n"
                                  "0 0 0 1 k 10 100 50 50 re f\n"_s });
    const QString out = m_directory.filePath(u"op-out.pdf"_s);

    int changed = 0;
    QString error;
    QVERIFY2(ColourTools::setBlackOverprint(in, out, true, &changed, &error), qPrintable(error));
    // Twice: once for the grey black, once for the pure-K black. The red in
    // between resets the state, so the second insertion is needed rather than
    // redundant.
    QCOMPARE(changed, 2);

    const QString content = test::contentOf(out, 0);
    QCOMPARE(content.count(u"/PsOverprintOn gs"_s), 2);

    QPDF pdf;
    PdfFile::open(pdf, out);
    std::vector<QPDFPageObjectHelper> pages = QPDFPageDocumentHelper(pdf).getAllPages();
    QPDFObjectHandle resources = pages.at(0).getAttribute("/Resources", false);
    QPDFObjectHandle states = resources.getKey("/ExtGState");
    QVERIFY(states.isDictionary());
    QPDFObjectHandle state = states.getKey("/PsOverprintOn");
    QVERIFY(state.isDictionary());
    QVERIFY(state.getKey("/OP").isBool());
    QVERIFY(state.getKey("/OP").getBoolValue());
    QVERIFY(state.getKey("/op").getBoolValue());
    QCOMPARE(state.getKey("/OPM").getIntValueAsInt(), 1);

    // And the document now says so when it is asked.
    const ColourTools::Inventory inventory = ColourTools::inspect(out, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(inventory.usesOverprint);
}

// ══ CMYK, and the refusal that goes with it ═══════════════════════════════

void TestColour::convertsToCmykThroughAnIccProfile()
{
    if (!ColourTools::hasColourManagement()) {
        QSKIP("this build has no LittleCMS, so there is no in-place CMYK route to test");
    }
    const QString profile = ColourTools::defaultIccProfile(ColourTools::Target::Cmyk);
    if (profile.isEmpty()) {
        QSKIP("no CMYK ICC profile is installed");
    }

    const QString in = writePdf(u"cmyk-in.pdf"_s, { colourfulPage() });
    const QString out = m_directory.filePath(u"cmyk-out.pdf"_s);

    ColourTools::ConvertOptions options;
    options.target = ColourTools::Target::Cmyk;
    options.iccProfilePath = profile;
    QStringList changes;
    QString error;
    QVERIFY2(ColourTools::convert(in, out, options, &changes, &error), qPrintable(error));
    QVERIFY(!changes.isEmpty());

    const QString content = test::contentOf(out, 0);
    QVERIFY2(content.contains(u" k\n"_s), qPrintable(u"nothing became CMYK: %1"_s.arg(content)));
    QVERIFY2(!content.contains(u"1 0 0 rg"_s), "an RGB fill survived");

    // A grey has to stay one ink. Building it from four is the mistake that
    // makes a printer telephone, so it is asserted rather than assumed: the
    // half grey must come out as three zeros and a black.
    QVERIFY2(content.contains(u"0.0000 0.0000 0.0000 0.5000 k"_s),
             qPrintable(u"the grey was not written as black ink alone: %1"_s.arg(content)));

    // And it still looks like the page it was: red stays red-ish once rendered.
    const QImage rendered = render(out, 0, 200);
    QVERIFY(!rendered.isNull());
    const QColor red = rendered.pixelColor(30, 200 - 30);
    QVERIFY2(red.red() > red.green() + 40 && red.red() > red.blue() + 40,
             qPrintable(u"the red rectangle rendered as %1"_s.arg(red.name())));
}

void TestColour::refusesAPartialCmykConversionWithoutAProfile()
{
    const QString in = writePdf(u"cmyk-partial.pdf"_s, { colourfulPage(), colourfulPage() });
    const QString out = m_directory.filePath(u"cmyk-partial-out.pdf"_s);

    ColourTools::ConvertOptions options;
    options.target = ColourTools::Target::Cmyk;
    options.pages = { 0 };
    // A profile that is not there, so the only remaining route is Ghostscript,
    // which rewrites everything and therefore cannot serve a page selection.
    options.iccProfilePath = m_directory.filePath(u"nothing-here.icc"_s);

    QString error;
    QVERIFY2(!ColourTools::convert(in, out, options, nullptr, &error), "a missing profile was accepted");
    QVERIFY(!error.isEmpty());
    QVERIFY2(!QFileInfo::exists(out), "a file was written despite the refusal");
}

// ══ measurement ═══════════════════════════════════════════════════════════

void TestColour::measuresInkCoverageAgainstAKnownFill()
{
    if (!test::haveGhostscript()) {
        QSKIP("Ghostscript is not installed");
    }

    const QString document
        = writePdf(u"ink.pdf"_s, { u"0 0 0 1 k 0 0 200 200 re f\n"_s, QString(), u"0 0 0 1 k 0 0 200 100 re f\n"_s });

    QString error;
    const QVector<double> coverage = ColourTools::inkCoverage(document, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(coverage.size(), 3);

    // A solid of black ink alone is a hundred per cent of one ink and nothing
    // else, so the highest total on the page is a hundred.
    QVERIFY2(coverage.at(0) > 90.0 && coverage.at(0) < 110.0,
             qPrintable(u"a solid black page measured %1 per cent"_s.arg(coverage.at(0))));
    QVERIFY2(coverage.at(1) < 5.0, qPrintable(u"a blank page measured %1 per cent"_s.arg(coverage.at(1))));
    // Half the page covered does not halve the maximum: an ink limit is about
    // the worst place on the sheet, not the average of it.
    QVERIFY2(coverage.at(2) > 90.0, qPrintable(u"a half-covered page measured %1 per cent"_s.arg(coverage.at(2))));
}

void TestColour::separatesTheBlackPlateOfAKnownFill()
{
    if (!test::haveGhostscript()) {
        QSKIP("Ghostscript is not installed");
    }

    const QString document = writePdf(u"plate.pdf"_s, { u"0 0 0 1 k 0 0 200 200 re f\n"_s });

    QString error;
    const QImage black = ColourTools::separation(document, 0, u"Black"_s, 36.0, &error);
    QVERIFY2(!black.isNull(), qPrintable(error));

    const QImage cyan = ColourTools::separation(document, 0, u"Cyan"_s, 36.0, &error);
    QVERIFY2(!cyan.isNull(), qPrintable(error));

    // Full ink reads as black on the plate, bare paper as white.
    QCOMPARE(qGray(black.pixel(black.width() / 2, black.height() / 2)), 0);
    QCOMPARE(qGray(cyan.pixel(cyan.width() / 2, cyan.height() / 2)), 255);

    const QImage missing = ColourTools::separation(document, 0, u"Pantone 300 C"_s, 36.0, &error);
    QVERIFY2(missing.isNull(), "an ink that is not in the document produced a plate");
    QVERIFY(!error.isEmpty());
}

void TestColour::reportsItsOwnLimits()
{
    const QStringList limits = ColourTools::limitations();
    QVERIFY(limits.size() >= 8);
    for (const QString &limit : limits) {
        QVERIFY(!limit.trimmed().isEmpty());
    }
}

QTEST_MAIN(TestColour)

#include "tst_colour.moc"
