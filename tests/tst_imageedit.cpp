/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#include "core/ImageEdit.h"
#include "core/PdfFile.h"
#include "core/PdfGeometry.h"
#include "render/PopplerBackend.h"

#include <KLocalizedString>

#include <QBuffer>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QPainter>
#include <QTemporaryDir>
#include <QTest>
#include <QtEnvironmentVariables>

#include <qpdf/QPDF.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>
#include <qpdf/QPDFWriter.hh>

#include <set>

using namespace ps;

/**
 * What separates a picture editor from a picture destroyer.
 *
 * Two of these cases matter more than the rest. One asks whether a JPEG that
 * needed nothing done to it came out with the bytes it went in with, because a
 * tool that quietly re-encodes a scan every time it is opened will have ruined
 * an archive within a year, and nothing on screen would ever show it. The other
 * asks whether a picture shared between pages is sized for the biggest place it
 * appears, since sizing it for the smallest is a change nobody asked for on
 * every other page.
 *
 * Everything is checked by reading the produced file back, never by trusting
 * what the operation said it did.
 */
class TestImageedit : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();

    void readsAFullPageScanAtItsRealResolution();
    void readsARealMagazine();
    void reportsTheEffectiveResolutionOfAPlacement();
    void followsPageRotation();
    void countsHowOftenOnePictureIsDrawn();
    void recompressesAScanAndKeepsItReadable();
    void leavesAJpegAloneWhenNothingNeedsDoing();
    void recompressesOnlyWhatWasNamed();
    void recompressesLosslesslyAndToGreyWhenAsked();
    void sizesASharedPictureForItsLargestPlacement();
    void recompressesASharedPictureOnlyOnce();
    void adjustToGrayscaleStoresOneComponent();
    void adjustToBlackWhiteStoresOneBit();
    void adjustBrightensWithoutMovingThePicture();
    void cropTakesThePixelsOutAndLeavesTheRestInPlace();
    void cropCarriesTransparencyWithIt();
    void replacePutsANewPictureInTheSamePlace();
    void extractWritesTheStoredJpegUntouched();
    void extractDecodesToPngWithItsTransparency();
    void turnsAPictureWithoutTouchingOneStoredByte();
    void turnsAnAwkwardAngleAndSaysWhatThatCosts();
    void reencodesAtTheSameSizeOnlyWhenTold();
    void removeTakesThePictureOffThePage();
    void deduplicateMergesIdenticalPictures();
    void refusesPicturesItCannotOpen();
    void reportsItsOwnLimits();

private:
    /** The stored bytes of the first image on a page, exactly as the file holds them. */
    static QByteArray storedImageBytes(const QString &pdf, int page);

    /**
     * A document whose pages each draw @p picture once.
     *
     * @p distinctObjects gives every page its own copy of the same bytes, which
     * is what deduplication has to find; sharing one object instead is what
     * tells a shared picture from a duplicated one, and the two must not be
     * confused. @p withAlpha adds a soft mask, because transparency is the part
     * every pixel operation is most likely to lose.
     */
    QString writeImagePdf(const QString &name, const QImage &picture, const QSizeF &pageSize,
                          const QVector<QRectF> &placements, bool distinctObjects = false, bool withAlpha = false,
                          int rotate = 0);

    /** A page holding a picture in a format nothing here can open. */
    QString writeUnreadablePdf(const QString &name);

    QTemporaryDir m_dir;
};

namespace {

/** A picture with four tellable quarters, so a crop cannot fake having worked. */
QImage quarteredPicture(int size)
{
    QImage picture(size, size, QImage::Format_RGB32);
    QPainter painter(&picture);
    painter.fillRect(0, 0, size / 2, size / 2, QColor(220, 20, 20));
    painter.fillRect(size / 2, 0, size - size / 2, size / 2, QColor(20, 180, 20));
    painter.fillRect(0, size / 2, size / 2, size - size / 2, QColor(20, 20, 220));
    painter.fillRect(size / 2, size / 2, size - size / 2, size - size / 2, QColor(230, 210, 30));
    painter.end();
    return picture;
}

QByteArray asJpeg(const QImage &picture, int quality)
{
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    picture.convertToFormat(QImage::Format_RGB888).save(&buffer, "JPEG", quality);
    buffer.close();
    return bytes;
}

/** What one picture really looks like in the file, read straight from the stream. */
struct StoredImage {
    bool valid = false;
    QString name;
    QByteArray raw;
    int width = 0;
    int height = 0;
    int bits = 0;
    QString colourSpace;
    QString filter;
    int softMaskWidth = 0;
};

/** The one picture page @p page draws, whatever the operation renamed it to. */
StoredImage storedImageOf(const QString &path, int page)
{
    StoredImage stored;
    QPDF pdf;
    PdfFile::open(pdf, path);
    QPDFPageDocumentHelper documents(pdf);
    std::vector<QPDFPageObjectHelper> pages = documents.getAllPages();
    if (page < 0 || page >= int(pages.size())) {
        return stored;
    }
    QPDFObjectHandle resources = pages.at(size_t(page)).getAttribute("/Resources", false);
    QPDFObjectHandle xobjects = resources.isDictionary() ? resources.getKey("/XObject") : QPDFObjectHandle::newNull();
    if (!xobjects.isDictionary()) {
        return stored;
    }

    std::map<std::string, QPDFObjectHandle> entries = xobjects.getDictAsMap();
    for (auto &entry : entries) {
        QPDFObjectHandle object = entry.second;
        if (!object.isStream()) {
            continue;
        }
        QPDFObjectHandle dict = object.getDict();
        if (!dict.getKey("/Subtype").isName() || dict.getKey("/Subtype").getName() != "/Image") {
            continue;
        }
        stored.valid = true;
        stored.name = QString::fromStdString(entry.first);
        const auto raw = object.getRawStreamData();
        stored.raw = QByteArray(reinterpret_cast<const char *>(raw->getBuffer()), qsizetype(raw->getSize()));
        stored.width = dict.getKey("/Width").getIntValueAsInt();
        stored.height = dict.getKey("/Height").getIntValueAsInt();
        stored.bits
            = dict.getKey("/BitsPerComponent").isInteger() ? dict.getKey("/BitsPerComponent").getIntValueAsInt() : 8;
        stored.colourSpace = dict.getKey("/ColorSpace").isName()
            ? QString::fromStdString(dict.getKey("/ColorSpace").getName())
            : QString::fromStdString(dict.getKey("/ColorSpace").unparse());
        stored.filter
            = dict.getKey("/Filter").isName() ? QString::fromStdString(dict.getKey("/Filter").getName()) : QString();
        QPDFObjectHandle mask = dict.getKey("/SMask");
        if (mask.isStream()) {
            stored.softMaskWidth = mask.getDict().getKey("/Width").getIntValueAsInt();
        }
        break;
    }
    return stored;
}

/** How many separate image objects the whole file still holds. */
int distinctImageObjects(const QString &path)
{
    QPDF pdf;
    PdfFile::open(pdf, path);
    QPDFPageDocumentHelper documents(pdf);
    std::set<QPDFObjGen> found;
    for (QPDFPageObjectHelper &page : documents.getAllPages()) {
        QPDFObjectHandle resources = page.getAttribute("/Resources", false);
        QPDFObjectHandle xobjects
            = resources.isDictionary() ? resources.getKey("/XObject") : QPDFObjectHandle::newNull();
        if (!xobjects.isDictionary()) {
            continue;
        }
        std::map<std::string, QPDFObjectHandle> entries = xobjects.getDictAsMap();
        for (auto &entry : entries) {
            QPDFObjectHandle object = entry.second;
            if (object.isStream() && object.getDict().getKey("/Subtype").isName()
                && object.getDict().getKey("/Subtype").getName() == "/Image") {
                found.insert(object.getObjGen());
            }
        }
    }
    return int(found.size());
}

QImage renderedPage(const QString &path, int page, int widthPx)
{
    PopplerBackend backend;
    if (!backend.addDocument(1, path, nullptr)) {
        return {};
    }
    return backend.renderPage(1, page, widthPx);
}

double meanBrightness(const QImage &image)
{
    if (image.isNull()) {
        return -1.0;
    }
    const QImage rgb = image.convertToFormat(QImage::Format_RGB32);
    double total = 0.0;
    for (int y = 0; y < rgb.height(); ++y) {
        const auto *line = reinterpret_cast<const QRgb *>(rgb.constScanLine(y));
        for (int x = 0; x < rgb.width(); ++x) {
            total += qGray(line[x]);
        }
    }
    return total / (double(rgb.width()) * rgb.height());
}

} // namespace

void TestImageedit::initTestCase()
{
    KLocalizedString::setApplicationDomain("pdf-smithy");
    QVERIFY(m_dir.isValid());
}

QString TestImageedit::writeImagePdf(const QString &name, const QImage &picture, const QSizeF &pageSize,
                                     const QVector<QRectF> &placements, bool distinctObjects, bool withAlpha,
                                     int rotate)
{
    const QString path = m_dir.filePath(name);
    const QByteArray jpeg = asJpeg(picture, 90);

    QPDF pdf;
    pdf.emptyPDF();

    const auto makeImage = [&] {
        QPDFObjectHandle stream = QPDFObjectHandle::newStream(&pdf, std::string(jpeg.constData(), size_t(jpeg.size())));
        QPDFObjectHandle dict = stream.getDict();
        dict.replaceKey("/Type", QPDFObjectHandle::newName("/XObject"));
        dict.replaceKey("/Subtype", QPDFObjectHandle::newName("/Image"));
        dict.replaceKey("/Width", QPDFObjectHandle::newInteger(picture.width()));
        dict.replaceKey("/Height", QPDFObjectHandle::newInteger(picture.height()));
        dict.replaceKey("/ColorSpace", QPDFObjectHandle::newName("/DeviceRGB"));
        dict.replaceKey("/BitsPerComponent", QPDFObjectHandle::newInteger(8));
        dict.replaceKey("/Filter", QPDFObjectHandle::newName("/DCTDecode"));

        if (withAlpha) {
            // A left-to-right ramp, so a crop that keeps the wrong half is
            // obvious in the mask's own numbers.
            std::string alpha;
            alpha.reserve(size_t(picture.width()) * picture.height());
            for (int y = 0; y < picture.height(); ++y) {
                for (int x = 0; x < picture.width(); ++x) {
                    alpha.push_back(char(x * 255 / std::max(1, picture.width() - 1)));
                }
            }
            QPDFObjectHandle mask = QPDFObjectHandle::newStream(&pdf, alpha);
            QPDFObjectHandle maskDict = mask.getDict();
            maskDict.replaceKey("/Type", QPDFObjectHandle::newName("/XObject"));
            maskDict.replaceKey("/Subtype", QPDFObjectHandle::newName("/Image"));
            maskDict.replaceKey("/Width", QPDFObjectHandle::newInteger(picture.width()));
            maskDict.replaceKey("/Height", QPDFObjectHandle::newInteger(picture.height()));
            maskDict.replaceKey("/ColorSpace", QPDFObjectHandle::newName("/DeviceGray"));
            maskDict.replaceKey("/BitsPerComponent", QPDFObjectHandle::newInteger(8));
            dict.replaceKey("/SMask", mask);
        }
        return stream;
    };

    QPDFObjectHandle shared = makeImage();

    QPDFPageDocumentHelper documents(pdf);
    for (const QRectF &placement : placements) {
        QPDFObjectHandle xobjects = QPDFObjectHandle::newDictionary();
        xobjects.replaceKey("/Im0", distinctObjects ? makeImage() : shared);
        QPDFObjectHandle resources = QPDFObjectHandle::newDictionary();
        resources.replaceKey("/XObject", xobjects);

        // Two nested matrices deliberately: real generators write the placement
        // as a scale on top of a unit conversion, and anything that reads only
        // the last `cm` gets the resolution spectacularly wrong.
        std::string content = "q 0.5 0 0 0.5 0 0 cm\n";
        content += PdfGeometry::number(placement.width() * 2) + " 0 0 " + PdfGeometry::number(placement.height() * 2)
            + " " + PdfGeometry::number(placement.x() * 2) + " " + PdfGeometry::number(placement.y() * 2) + " cm\n";
        content += "/Im0 Do\nQ\n";

        QPDFObjectHandle page = QPDFObjectHandle::parse("<< /Type /Page >>");
        page.replaceKey("/MediaBox",
                        QPDFObjectHandle::parse("[ 0 0 " + PdfGeometry::number(pageSize.width()) + " "
                                                + PdfGeometry::number(pageSize.height()) + " ]"));
        page.replaceKey("/Resources", resources);
        page.replaceKey("/Contents", QPDFObjectHandle::newStream(&pdf, content));
        if (rotate != 0) {
            page.replaceKey("/Rotate", QPDFObjectHandle::newInteger(rotate));
        }
        documents.addPage(QPDFPageObjectHelper(pdf.makeIndirectObject(page)), false);
    }

    QPDFWriter writer(pdf, path.toUtf8().constData());
    writer.setStreamDataMode(qpdf_s_preserve);
    writer.write();
    return path;
}

QString TestImageedit::writeUnreadablePdf(const QString &name)
{
    const QString path = m_dir.filePath(name);

    QPDF pdf;
    pdf.emptyPDF();

    QPDFObjectHandle stream = QPDFObjectHandle::newStream(&pdf, std::string(2048, '\x7f'));
    QPDFObjectHandle dict = stream.getDict();
    dict.replaceKey("/Type", QPDFObjectHandle::newName("/XObject"));
    dict.replaceKey("/Subtype", QPDFObjectHandle::newName("/Image"));
    dict.replaceKey("/Width", QPDFObjectHandle::newInteger(1000));
    dict.replaceKey("/Height", QPDFObjectHandle::newInteger(1000));
    dict.replaceKey("/ColorSpace", QPDFObjectHandle::newName("/DeviceRGB"));
    dict.replaceKey("/BitsPerComponent", QPDFObjectHandle::newInteger(8));
    dict.replaceKey("/Filter", QPDFObjectHandle::newName("/JPXDecode"));

    QPDFObjectHandle xobjects = QPDFObjectHandle::newDictionary();
    xobjects.replaceKey("/Im0", stream);
    QPDFObjectHandle resources = QPDFObjectHandle::newDictionary();
    resources.replaceKey("/XObject", xobjects);

    QPDFObjectHandle page = QPDFObjectHandle::parse("<< /Type /Page /MediaBox [0 0 200 200] >>");
    page.replaceKey("/Resources", resources);
    page.replaceKey("/Contents", QPDFObjectHandle::newStream(&pdf, std::string("q 72 0 0 72 20 20 cm /Im0 Do Q\n")));

    QPDFPageDocumentHelper documents(pdf);
    documents.addPage(QPDFPageObjectHelper(pdf.makeIndirectObject(page)), false);

    QPDFWriter writer(pdf, path.toUtf8().constData());
    writer.setStreamDataMode(qpdf_s_preserve);
    writer.write();
    return path;
}

void TestImageedit::readsAFullPageScanAtItsRealResolution()
{
    const QString scan = QTest::qFindTestData(QStringLiteral("../testdata/brief-1902.pdf"), __FILE__, __LINE__);
    if (scan.isEmpty()) {
        QSKIP("testdata/brief-1902.pdf is not available");
    }

    QString error;
    const QVector<ImageEdit::ImageUse> uses = ImageEdit::read(scan, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(uses.size(), 5);

    const ImageEdit::ImageUse &first = uses.constFirst();
    QCOMPARE(first.page, 0);
    QCOMPARE(first.resourceName, QStringLiteral("/R7"));
    QCOMPARE(first.pixelSize, QSize(1283, 1695));
    QCOMPARE(first.encoding, QStringLiteral("DCTDecode"));
    QCOMPARE(first.colourSpace, QStringLiteral("DeviceRGB"));
    QCOMPARE(first.bitsPerComponent, 8);
    QVERIFY(first.decodable);
    QVERIFY(!first.hasAlpha);
    QVERIFY(!first.isMask);
    QCOMPARE(first.drawnTimes, 1);
    QVERIFY(first.storedBytes > 100000);

    // 1283 pixels across a 637-point page is 145 dpi, and the whole point of
    // reading the placement matrix rather than /Width is getting that number.
    QVERIFY2(first.effectiveDpiX > 135.0 && first.effectiveDpiX < 160.0,
             qPrintable(QStringLiteral("dpi was %1").arg(first.effectiveDpiX)));
    QVERIFY(first.effectiveDpiY > 135.0 && first.effectiveDpiY < 160.0);

    // A full-page scan covers the page.
    QVERIFY(first.placement.width() > 600.0);
    QVERIFY(first.placement.height() > 800.0);
    QVERIFY(qAbs(first.placement.x()) < 1.0);
}

void TestImageedit::readsARealMagazine()
{
    const QString magazine = qEnvironmentVariable("PS_STRESS_PDF");
    if (!QFileInfo::exists(magazine)) {
        QSKIP("set PS_STRESS_PDF to a large real document to run this");
    }

    QString error;
    const QVector<ImageEdit::ImageUse> uses = ImageEdit::read(magazine, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY2(uses.size() > 100, qPrintable(QStringLiteral("only %1 pictures found").arg(uses.size())));

    int shared = 0;
    int large = 0;
    for (const ImageEdit::ImageUse &use : uses) {
        QCOMPARE(use.encoding, QStringLiteral("DCTDecode"));
        QVERIFY(use.colourSpace.startsWith(QStringLiteral("ICCBased")));
        QVERIFY(use.decodable);
        QVERIFY(use.storedBytes > 0);
        QVERIFY(use.drawnTimes >= 1);
        if (use.drawnTimes > 1) {
            ++shared;
        }
        // Small badges and rules can sit at any resolution; anything big enough
        // to be a photograph should be in the range a magazine is made at.
        if (use.pixelSize.width() * use.pixelSize.height() > 40000) {
            ++large;
            QVERIFY2(use.effectiveDpiX > 100.0 && use.effectiveDpiX < 400.0,
                     qPrintable(QStringLiteral("page %1 %2 is at %3 dpi")
                                    .arg(use.page + 1)
                                    .arg(use.resourceName)
                                    .arg(use.effectiveDpiX)));
        }
    }
    QVERIFY(large > 50);
    // The same logo and the same rules recur throughout; a reader that missed
    // this would recompress them once per page.
    QVERIFY2(shared > 0, "nothing was recognised as drawn more than once");
}

void TestImageedit::reportsTheEffectiveResolutionOfAPlacement()
{
    // 400 pixels across 100 points is 288 dpi, and the fixture writes the
    // placement as two nested matrices to make sure both are read.
    const QString path = writeImagePdf(QStringLiteral("placed.pdf"), quarteredPicture(400), QSizeF(200, 200),
                                       { QRectF(50, 50, 100, 100) });

    QString error;
    const QVector<ImageEdit::ImageUse> uses = ImageEdit::read(path, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(uses.size(), 1);
    QCOMPARE(uses.constFirst().pixelSize, QSize(400, 400));
    QCOMPARE(qRound(uses.constFirst().effectiveDpiX), 288);
    QCOMPARE(qRound(uses.constFirst().effectiveDpiY), 288);
    QCOMPARE(uses.constFirst().placement, QRectF(50, 50, 100, 100));
}

void TestImageedit::followsPageRotation()
{
    // A viewer shows this page turned a quarter turn, so a placement measured in
    // page space is not where the user sees it. Getting this wrong would put a
    // picture inventory's coordinates at right angles to the page on screen.
    const QString path = writeImagePdf(QStringLiteral("rotated.pdf"), quarteredPicture(400), QSizeF(300, 200),
                                       { QRectF(10, 20, 100, 50) }, false, false, 90);

    QString error;
    const QVector<ImageEdit::ImageUse> uses = ImageEdit::read(path, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(uses.size(), 1);

    // Page rect (10,20)-(110,70) turned for display on a 200-by-300 page.
    QCOMPARE(uses.constFirst().placement, QRectF(20, 190, 50, 100));

    // The resolution belongs to the picture's own axes, so turning the page
    // must not swap the two numbers: 400 pixels over 100 points is 288 dpi,
    // and over 50 points is 576.
    QCOMPARE(qRound(uses.constFirst().effectiveDpiX), 288);
    QCOMPARE(qRound(uses.constFirst().effectiveDpiY), 576);
}

void TestImageedit::recompressesOnlyWhatWasNamed()
{
    const QString scan = QTest::qFindTestData(QStringLiteral("../testdata/brief-1902.pdf"), __FILE__, __LINE__);
    if (scan.isEmpty()) {
        QSKIP("testdata/brief-1902.pdf is not available");
    }
    const QString out = m_dir.filePath(QStringLiteral("scan-page3-only.pdf"));

    QVector<QByteArray> before;
    for (int page = 0; page < 5; ++page) {
        before.append(storedImageOf(scan, page).raw);
        QVERIFY(!before.constLast().isEmpty());
    }

    ImageEdit::Recompress options;
    options.targetDpi = 100.0;

    qint64 saved = 0;
    QString error;
    QVERIFY2(ImageEdit::recompress(scan, out, { 2 }, { QStringLiteral("/R7") }, options, &saved, &error),
             qPrintable(error));
    QVERIFY(saved > 0);

    // Every page in this document calls its picture /R7, but they are five
    // different objects, and only the one that was named may have moved.
    for (int page = 0; page < 5; ++page) {
        const QByteArray after = storedImageOf(out, page).raw;
        if (page == 2) {
            QVERIFY2(after.size() < before.at(page).size(), "the named picture was not recompressed");
        } else {
            QCOMPARE(after, before.at(page));
        }
    }
}

void TestImageedit::recompressesLosslesslyAndToGreyWhenAsked()
{
    const QString path = writeImagePdf(QStringLiteral("lossless-in.pdf"), quarteredPicture(400), QSizeF(200, 200),
                                       { QRectF(50, 50, 100, 100) });

    ImageEdit::Recompress lossless;
    lossless.targetDpi = 100.0;
    lossless.lossless = true;

    const QString flat = m_dir.filePath(QStringLiteral("lossless-out.pdf"));
    qint64 saved = 0;
    QString error;
    QVERIFY2(ImageEdit::recompress(path, flat, {}, {}, lossless, &saved, &error), qPrintable(error));
    StoredImage stored = storedImageOf(flat, 0);
    QVERIFY(stored.valid);
    // Deflated samples rather than a second pass of JPEG: fewer pixels, but no
    // new generation loss on the ones that survive.
    QCOMPARE(stored.filter, QStringLiteral("/FlateDecode"));
    QCOMPARE(stored.colourSpace, QStringLiteral("/DeviceRGB"));
    QCOMPARE(stored.width, 139);

    ImageEdit::Recompress grey;
    grey.targetDpi = 0.0;
    grey.toGrayscale = true;

    const QString greyOut = m_dir.filePath(QStringLiteral("grey-recompressed.pdf"));
    QVERIFY2(ImageEdit::recompress(path, greyOut, {}, {}, grey, &saved, &error), qPrintable(error));
    stored = storedImageOf(greyOut, 0);
    QVERIFY(stored.valid);
    QCOMPARE(stored.colourSpace, QStringLiteral("/DeviceGray"));
    // A zero target keeps every pixel; only the colour went.
    QCOMPARE(stored.width, 400);
    QCOMPARE(stored.height, 400);
    QVERIFY(!QImage::fromData(stored.raw).isNull());
    QCOMPARE(QImage::fromData(stored.raw).format(), QImage::Format_Grayscale8);
}

void TestImageedit::countsHowOftenOnePictureIsDrawn()
{
    const QString shared = writeImagePdf(QStringLiteral("shared.pdf"), quarteredPicture(200), QSizeF(200, 200),
                                         { QRectF(20, 20, 80, 80), QRectF(20, 20, 80, 80), QRectF(20, 20, 80, 80) });
    const QString copies
        = writeImagePdf(QStringLiteral("copies.pdf"), quarteredPicture(200), QSizeF(200, 200),
                        { QRectF(20, 20, 80, 80), QRectF(20, 20, 80, 80), QRectF(20, 20, 80, 80) }, true);

    QString error;
    const QVector<ImageEdit::ImageUse> sharedUses = ImageEdit::read(shared, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(sharedUses.size(), 3);
    for (const ImageEdit::ImageUse &use : sharedUses) {
        QCOMPARE(use.drawnTimes, 3);
    }
    QCOMPARE(distinctImageObjects(shared), 1);

    // Three separate objects that happen to look the same are drawn once each.
    const QVector<ImageEdit::ImageUse> copyUses = ImageEdit::read(copies, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(copyUses.size(), 3);
    for (const ImageEdit::ImageUse &use : copyUses) {
        QCOMPARE(use.drawnTimes, 1);
    }
    QCOMPARE(distinctImageObjects(copies), 3);
}

void TestImageedit::recompressesAScanAndKeepsItReadable()
{
    const QString scan = QTest::qFindTestData(QStringLiteral("../testdata/brief-1902.pdf"), __FILE__, __LINE__);
    if (scan.isEmpty()) {
        QSKIP("testdata/brief-1902.pdf is not available");
    }
    const QString out = m_dir.filePath(QStringLiteral("scan-100dpi.pdf"));

    const double brightnessBefore = meanBrightness(renderedPage(scan, 0, 500));
    QVERIFY(brightnessBefore > 0.0);

    ImageEdit::Recompress options;
    options.targetDpi = 100.0;
    options.jpegQuality = 80;

    qint64 saved = 0;
    QString error;
    QVERIFY2(ImageEdit::recompress(scan, out, {}, {}, options, &saved, &error), qPrintable(error));
    QVERIFY2(saved > 0, "nothing was saved on a 145 dpi scan asked for 100 dpi");

    const qint64 before = QFileInfo(scan).size();
    const qint64 after = QFileInfo(out).size();
    QVERIFY2(after < before * 3 / 4, qPrintable(QStringLiteral("%1 bytes became %2").arg(before).arg(after)));

    // 637 points at 100 dpi is 885 pixels, and that is what should be stored.
    const StoredImage stored = storedImageOf(out, 0);
    QVERIFY(stored.valid);
    QCOMPARE(stored.filter, QStringLiteral("/DCTDecode"));
    QVERIFY2(stored.width > 860 && stored.width < 910, qPrintable(QStringLiteral("width %1").arg(stored.width)));

    // Smaller is only worth having if the page still says the same thing.
    const double brightnessAfter = meanBrightness(renderedPage(out, 0, 500));
    QVERIFY2(qAbs(brightnessAfter - brightnessBefore) < 6.0,
             qPrintable(QStringLiteral("brightness went from %1 to %2").arg(brightnessBefore).arg(brightnessAfter)));

    // And read() should now agree that the document sits at the asked-for
    // resolution rather than above it.
    const QVector<ImageEdit::ImageUse> uses = ImageEdit::read(out, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(uses.size(), 5);
    QVERIFY(uses.constFirst().effectiveDpiX < 105.0);
}

void TestImageedit::leavesAJpegAloneWhenNothingNeedsDoing()
{
    const QString scan = QTest::qFindTestData(QStringLiteral("../testdata/brief-1902.pdf"), __FILE__, __LINE__);
    if (scan.isEmpty()) {
        QSKIP("testdata/brief-1902.pdf is not available");
    }
    const QString out = m_dir.filePath(QStringLiteral("scan-noop.pdf"));

    const StoredImage before = storedImageOf(scan, 0);
    QVERIFY(before.valid);

    // The scan is at 145 dpi; asking for 150 asks for nothing.
    ImageEdit::Recompress options;
    options.targetDpi = 150.0;

    qint64 saved = -1;
    QString error;
    QVERIFY2(ImageEdit::recompress(scan, out, {}, {}, options, &saved, &error), qPrintable(error));
    QCOMPARE(saved, 0);

    const StoredImage after = storedImageOf(out, 0);
    QVERIFY(after.valid);
    QCOMPARE(after.width, before.width);
    QCOMPARE(after.height, before.height);
    // The whole promise: not merely the same size, the same bytes. A tool that
    // re-encodes a scan for nothing destroys an archive one open at a time.
    QCOMPARE(QCryptographicHash::hash(after.raw, QCryptographicHash::Sha256),
             QCryptographicHash::hash(before.raw, QCryptographicHash::Sha256));
}

void TestImageedit::sizesASharedPictureForItsLargestPlacement()
{
    // One object, drawn small on the first page and large on the second. Sizing
    // it from the placement that was named would wreck the other one.
    const QString path = writeImagePdf(QStringLiteral("two-sizes.pdf"), quarteredPicture(400), QSizeF(300, 300),
                                       { QRectF(10, 10, 50, 50), QRectF(10, 10, 200, 200) });

    ImageEdit::Recompress options;
    options.targetDpi = 100.0;

    qint64 saved = 0;
    QString error;
    QVERIFY2(ImageEdit::recompress(path, m_dir.filePath(QStringLiteral("two-sizes-out.pdf")), { 0 },
                                   { QStringLiteral("/Im0") }, options, &saved, &error),
             qPrintable(error));

    // 200 points at 100 dpi needs 278 pixels. Sizing from the 50-point
    // placement would have left 70, and the big page would be a mosaic.
    const StoredImage stored = storedImageOf(m_dir.filePath(QStringLiteral("two-sizes-out.pdf")), 1);
    QVERIFY(stored.valid);
    QVERIFY2(stored.width > 260 && stored.width < 290, qPrintable(QStringLiteral("width %1").arg(stored.width)));
    QCOMPARE(distinctImageObjects(m_dir.filePath(QStringLiteral("two-sizes-out.pdf"))), 1);
}

void TestImageedit::recompressesASharedPictureOnlyOnce()
{
    const QString path = writeImagePdf(QStringLiteral("shared-recompress.pdf"), quarteredPicture(600), QSizeF(200, 200),
                                       { QRectF(20, 20, 72, 72), QRectF(20, 20, 72, 72), QRectF(20, 20, 72, 72) });
    const QString out = m_dir.filePath(QStringLiteral("shared-recompress-out.pdf"));

    ImageEdit::Recompress options;
    options.targetDpi = 150.0;

    qint64 saved = 0;
    QString error;
    QVERIFY2(ImageEdit::recompress(path, out, {}, {}, options, &saved, &error), qPrintable(error));
    QVERIFY(saved > 0);

    // Still one object: three pages asking for the same work must not produce
    // three copies of it, and must not have re-encoded it three times over.
    QCOMPARE(distinctImageObjects(out), 1);
    const StoredImage stored = storedImageOf(out, 2);
    QVERIFY(stored.valid);
    QCOMPARE(stored.width, 150);

    for (int page = 0; page < 3; ++page) {
        QVERIFY(meanBrightness(renderedPage(out, page, 200)) < 250.0);
    }
}

void TestImageedit::adjustToGrayscaleStoresOneComponent()
{
    const QString path = writeImagePdf(QStringLiteral("grey-in.pdf"), quarteredPicture(200), QSizeF(200, 200),
                                       { QRectF(50, 50, 100, 100) });
    const QString out = m_dir.filePath(QStringLiteral("grey-out.pdf"));

    ImageEdit::Adjust options;
    options.toGrayscale = true;

    QString error;
    QVERIFY2(ImageEdit::adjust(path, out, 0, QStringLiteral("/Im0"), options, &error), qPrintable(error));

    const StoredImage stored = storedImageOf(out, 0);
    QVERIFY(stored.valid);
    QCOMPARE(stored.colourSpace, QStringLiteral("/DeviceGray"));
    QCOMPARE(stored.width, 200);
    QCOMPARE(stored.height, 200);

    // The dictionary saying one component is worth nothing unless the codestream
    // agrees, so the stored bytes are decoded and counted.
    QCOMPARE(stored.filter, QStringLiteral("/DCTDecode"));
    const QImage decoded = QImage::fromData(stored.raw);
    QVERIFY(!decoded.isNull());
    QCOMPARE(decoded.format(), QImage::Format_Grayscale8);

    const QVector<ImageEdit::ImageUse> uses = ImageEdit::read(out, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(uses.size(), 1);
    QCOMPARE(uses.constFirst().colourSpace, QStringLiteral("DeviceGray"));
    QCOMPARE(uses.constFirst().placement, QRectF(50, 50, 100, 100));
}

void TestImageedit::adjustToBlackWhiteStoresOneBit()
{
    const QString path = writeImagePdf(QStringLiteral("bw-in.pdf"), quarteredPicture(200), QSizeF(200, 200),
                                       { QRectF(50, 50, 100, 100) });
    const QString out = m_dir.filePath(QStringLiteral("bw-out.pdf"));

    ImageEdit::Adjust options;
    options.toBlackWhite = true;
    options.threshold = 128;

    QString error;
    QVERIFY2(ImageEdit::adjust(path, out, 0, QStringLiteral("/Im0"), options, &error), qPrintable(error));

    const StoredImage stored = storedImageOf(out, 0);
    QVERIFY(stored.valid);
    QCOMPARE(stored.bits, 1);
    QCOMPARE(stored.colourSpace, QStringLiteral("/DeviceGray"));
    QCOMPARE(stored.filter, QStringLiteral("/FlateDecode"));
    // One bit per pixel deflated has to be a fraction of the JPEG it replaced.
    QVERIFY(stored.raw.size() < 4000);

    const QVector<ImageEdit::ImageUse> uses = ImageEdit::read(out, &error);
    QCOMPARE(uses.size(), 1);
    QCOMPARE(uses.constFirst().bitsPerComponent, 1);
}

void TestImageedit::adjustBrightensWithoutMovingThePicture()
{
    const QString path = writeImagePdf(QStringLiteral("bright-in.pdf"), quarteredPicture(200), QSizeF(200, 200),
                                       { QRectF(50, 50, 100, 100) });
    const QString out = m_dir.filePath(QStringLiteral("bright-out.pdf"));

    const double before = meanBrightness(renderedPage(path, 0, 200));
    QVERIFY(before > 0.0);

    ImageEdit::Adjust options;
    options.brightness = 0.3;

    QString error;
    QVERIFY2(ImageEdit::adjust(path, out, 0, QStringLiteral("/Im0"), options, &error), qPrintable(error));

    const double after = meanBrightness(renderedPage(out, 0, 200));
    QVERIFY2(after > before + 5.0, qPrintable(QStringLiteral("brightness went from %1 to %2").arg(before).arg(after)));

    // Brightening must not move or resize anything.
    const QVector<ImageEdit::ImageUse> uses = ImageEdit::read(out, &error);
    QCOMPARE(uses.size(), 1);
    QCOMPARE(uses.constFirst().pixelSize, QSize(200, 200));
    QCOMPARE(uses.constFirst().placement, QRectF(50, 50, 100, 100));
}

void TestImageedit::cropTakesThePixelsOutAndLeavesTheRestInPlace()
{
    const QString path = writeImagePdf(QStringLiteral("crop-in.pdf"), quarteredPicture(400), QSizeF(200, 200),
                                       { QRectF(50, 50, 100, 100) });
    const QString out = m_dir.filePath(QStringLiteral("crop-out.pdf"));

    QString error;
    QVERIFY2(ImageEdit::crop(path, out, 0, QStringLiteral("/Im0"), QRectF(0, 0, 0.5, 0.5), &error), qPrintable(error));

    // The pixels are gone from the file, not merely hidden.
    const StoredImage stored = storedImageOf(out, 0);
    QVERIFY(stored.valid);
    QCOMPARE(stored.width, 200);
    QCOMPARE(stored.height, 200);
    QVERIFY(stored.raw.size() < QByteArray(asJpeg(quarteredPicture(400), 90)).size());

    // And what is left stays exactly where it was: the top-left quarter of the
    // picture still covers the top-left quarter of the old placement.
    const QVector<ImageEdit::ImageUse> uses = ImageEdit::read(out, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(uses.size(), 1);
    QCOMPARE(uses.constFirst().placement, QRectF(50, 100, 50, 50));

    const QImage rendered = renderedPage(out, 0, 200);
    QVERIFY(!rendered.isNull());
    QCOMPARE(rendered.size(), QSize(200, 200));
    const QColor kept = rendered.pixelColor(75, 75);
    QVERIFY2(kept.red() > 170 && kept.green() < 90 && kept.blue() < 90, qPrintable(kept.name()));
    // The three quarters that were cropped away show the page, not a stretched
    // remnant of the picture.
    QVERIFY(qGray(rendered.pixelColor(125, 75).rgb()) > 240);
    QVERIFY(qGray(rendered.pixelColor(75, 125).rgb()) > 240);
    QVERIFY(qGray(rendered.pixelColor(125, 125).rgb()) > 240);
}

void TestImageedit::cropCarriesTransparencyWithIt()
{
    const QString path = writeImagePdf(QStringLiteral("alpha-in.pdf"), quarteredPicture(400), QSizeF(200, 200),
                                       { QRectF(50, 50, 100, 100) }, false, true);

    QString error;
    const QVector<ImageEdit::ImageUse> before = ImageEdit::read(path, &error);
    QCOMPARE(before.size(), 1);
    QVERIFY(before.constFirst().hasAlpha);

    const QString out = m_dir.filePath(QStringLiteral("alpha-out.pdf"));
    QVERIFY2(ImageEdit::crop(path, out, 0, QStringLiteral("/Im0"), QRectF(0.5, 0, 0.5, 1.0), &error),
             qPrintable(error));

    const StoredImage stored = storedImageOf(out, 0);
    QVERIFY(stored.valid);
    QCOMPARE(stored.width, 200);
    QCOMPARE(stored.height, 400);
    // A soft mask left at its old size would put the transparency over the
    // wrong pixels, so it has to have been cropped as well.
    QCOMPARE(stored.softMaskWidth, 200);

    const QVector<ImageEdit::ImageUse> after = ImageEdit::read(out, &error);
    QCOMPARE(after.size(), 1);
    QVERIFY(after.constFirst().hasAlpha);
}

void TestImageedit::replacePutsANewPictureInTheSamePlace()
{
    const QString path = writeImagePdf(QStringLiteral("replace-in.pdf"), quarteredPicture(400), QSizeF(200, 200),
                                       { QRectF(50, 50, 100, 100) });
    const QString out = m_dir.filePath(QStringLiteral("replace-out.pdf"));

    QImage fresh(120, 60, QImage::Format_RGB32);
    fresh.fill(QColor(200, 30, 190));

    QString error;
    QVERIFY2(ImageEdit::replace(path, out, 0, QStringLiteral("/Im0"), fresh, false, &error), qPrintable(error));

    const QVector<ImageEdit::ImageUse> uses = ImageEdit::read(out, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(uses.size(), 1);
    QCOMPARE(uses.constFirst().pixelSize, QSize(120, 60));
    // Stretched to the old area, because that is what was asked for.
    QCOMPARE(uses.constFirst().placement, QRectF(50, 50, 100, 100));
    QCOMPARE(distinctImageObjects(out), 1);

    const QImage rendered = renderedPage(out, 0, 200);
    const QColor centre = rendered.pixelColor(100, 100);
    QVERIFY2(centre.red() > 170 && centre.green() < 80 && centre.blue() > 160, qPrintable(centre.name()));

    // Fitting instead of stretching keeps the shape and centres it, so a
    // two-to-one picture only covers the middle band of a square placement.
    const QString fitted = m_dir.filePath(QStringLiteral("replace-fitted.pdf"));
    QVERIFY2(ImageEdit::replace(path, fitted, 0, QStringLiteral("/Im0"), fresh, true, &error), qPrintable(error));
    const QVector<ImageEdit::ImageUse> fittedUses = ImageEdit::read(fitted, &error);
    QCOMPARE(fittedUses.size(), 1);
    QCOMPARE(qRound(fittedUses.constFirst().placement.width()), 100);
    QCOMPARE(qRound(fittedUses.constFirst().placement.height()), 50);
    QCOMPARE(qRound(fittedUses.constFirst().placement.center().y()), 100);
}

void TestImageedit::extractWritesTheStoredJpegUntouched()
{
    const QString scan = QTest::qFindTestData(QStringLiteral("../testdata/brief-1902.pdf"), __FILE__, __LINE__);
    if (scan.isEmpty()) {
        QSKIP("testdata/brief-1902.pdf is not available");
    }
    const QString jpeg = m_dir.filePath(QStringLiteral("page1.jpg"));

    QString error;
    QVERIFY2(ImageEdit::extract(scan, 0, QStringLiteral("/R7"), jpeg, &error), qPrintable(error));

    QFile file(jpeg);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QByteArray written = file.readAll();
    file.close();

    // Byte for byte the stream that was in the document: decoding and
    // re-encoding it would cost a generation for nothing.
    QCOMPARE(written, storedImageOf(scan, 0).raw);

    const QImage reopened(jpeg);
    QVERIFY(!reopened.isNull());
    QCOMPARE(reopened.size(), QSize(1283, 1695));
}

void TestImageedit::extractDecodesToPngWithItsTransparency()
{
    const QString path = writeImagePdf(QStringLiteral("extract-alpha.pdf"), quarteredPicture(200), QSizeF(200, 200),
                                       { QRectF(50, 50, 100, 100) }, false, true);
    const QString png = m_dir.filePath(QStringLiteral("extracted.png"));

    QString error;
    QVERIFY2(ImageEdit::extract(path, 0, QStringLiteral("/Im0"), png, &error), qPrintable(error));

    const QImage reopened(png);
    QVERIFY(!reopened.isNull());
    QCOMPARE(reopened.size(), QSize(200, 200));
    QVERIFY(reopened.hasAlphaChannel());
    // The fixture's mask ramps from left to right, and a logo extracted as an
    // opaque rectangle is not the logo that was in the file.
    QVERIFY(qAlpha(reopened.pixel(2, 100)) < 20);
    QVERIFY(qAlpha(reopened.pixel(197, 100)) > 235);
}

QByteArray TestImageedit::storedImageBytes(const QString &pdf, int page)
{
    QPDF document;
    document.processFile(QFile::encodeName(pdf).constData());
    std::vector<QPDFPageObjectHelper> pages = QPDFPageDocumentHelper(document).getAllPages();
    if (page < 0 || size_t(page) >= pages.size()) {
        return {};
    }
    QPDFObjectHandle resources = pages.at(size_t(page)).getAttribute("/Resources", false);
    QPDFObjectHandle table = resources.isDictionary() ? resources.getKey("/XObject") : QPDFObjectHandle::newNull();
    if (!table.isDictionary()) {
        return {};
    }
    for (const auto &entry : table.getDictAsMap()) {
        QPDFObjectHandle object = entry.second;
        if (!object.isStream()) {
            continue;
        }
        // The raw stream, not the decoded pixels: the claim being tested is
        // about the bytes in the file, and decoding them would hide a
        // re-encode that produced the same picture.
        const auto data = object.getRawStreamData();
        return QByteArray(reinterpret_cast<const char *>(data->getBuffer()), qsizetype(data->getSize()));
    }
    return {};
}

void TestImageedit::turnsAPictureWithoutTouchingOneStoredByte()
{
    const QString path = writeImagePdf(QStringLiteral("turn-in.pdf"), quarteredPicture(400), QSizeF(300, 300),
                                       { QRectF(50, 100, 200, 100) });
    const QByteArray before = storedImageBytes(path, 0);
    QVERIFY(before.size() > 1000);

    const QString out = m_dir.filePath(QStringLiteral("turn-out.pdf"));
    ImageEdit::RotateReport report;
    QString error;
    QVERIFY2(ImageEdit::rotate(path, out, 0, QStringLiteral("/Im0"), 90.0, &report, &error), qPrintable(error));

    QCOMPARE(report.placementsTurned, 1);
    QCOMPARE(report.placementsRefused, 0);
    QVERIFY(report.quarterTurn);

    // The whole claim of this operation: a turn is a matrix, so the JPEG comes
    // out with the bytes it went in with and gains no generation of loss.
    QVERIFY(report.pixelsUntouched);
    QCOMPARE(storedImageBytes(out, 0), before);

    // And it really did move. A 200 by 100 placement centred at (150, 150)
    // becomes 100 by 200 about the same centre: the size is kept and the area
    // covered is not, which is what the header promises.
    QVERIFY2(qAbs(report.placement.width() - 100.0) < 0.01, qPrintable(QString::number(report.placement.width())));
    QVERIFY2(qAbs(report.placement.height() - 200.0) < 0.01, qPrintable(QString::number(report.placement.height())));
    QVERIFY2(qAbs(report.placement.center().x() - 150.0) < 0.01,
             qPrintable(QString::number(report.placement.center().x())));
    QVERIFY2(qAbs(report.placement.center().y() - 150.0) < 0.01,
             qPrintable(QString::number(report.placement.center().y())));

    // Four quarter turns are the identity, so the content stream should come
    // back to a placement of the original shape. This is what catches a
    // conjugation done the wrong way round: that scales the movement by the
    // placement's own scaling, so the fourth turn lands somewhere else entirely.
    QString step = path;
    for (int i = 0; i < 4; ++i) {
        const QString next = m_dir.filePath(QStringLiteral("turn-%1.pdf").arg(i));
        ImageEdit::RotateReport each;
        QVERIFY2(ImageEdit::rotate(step, next, 0, QStringLiteral("/Im0"), 90.0, &each, &error), qPrintable(error));
        step = next;
    }
    const QVector<ImageEdit::ImageUse> after = ImageEdit::read(step, &error);
    QVERIFY2(!after.isEmpty(), qPrintable(error));
    QVERIFY2(qAbs(after.constFirst().placement.width() - 200.0) < 0.01,
             qPrintable(QString::number(after.constFirst().placement.width())));
    QVERIFY2(qAbs(after.constFirst().placement.height() - 100.0) < 0.01,
             qPrintable(QString::number(after.constFirst().placement.height())));
}

void TestImageedit::turnsAnAwkwardAngleAndSaysWhatThatCosts()
{
    const QString path = writeImagePdf(QStringLiteral("skew-in.pdf"), quarteredPicture(200), QSizeF(300, 300),
                                       { QRectF(100, 100, 100, 100) });
    const QByteArray before = storedImageBytes(path, 0);

    const QString out = m_dir.filePath(QStringLiteral("skew-out.pdf"));
    ImageEdit::RotateReport report;
    QString error;
    QVERIFY2(ImageEdit::rotate(path, out, 0, QStringLiteral("/Im0"), 30.0, &report, &error), qPrintable(error));

    // Any angle works, because nothing is resampled to a pixel grid.
    QCOMPARE(report.placementsTurned, 1);
    QVERIFY(report.pixelsUntouched);
    QCOMPARE(storedImageBytes(out, 0), before);

    // But it is no longer square to the page, and saying so is the difference
    // between a tool that is trusted and one that is not.
    QVERIFY(!report.quarterTurn);
    QVERIFY2(!report.notes.isEmpty(), "a turn off the square said nothing about it");

    // A 100-point square turned 30 degrees needs about 136 points of room.
    QVERIFY2(report.placement.width() > 130.0 && report.placement.width() < 142.0,
             qPrintable(QString::number(report.placement.width())));

    QString ignored;
    QVERIFY(!ImageEdit::rotate(path, out, 0, QStringLiteral("/Im0"), 360.0, nullptr, &ignored));
    QVERIFY(!ignored.isEmpty());
}

void TestImageedit::reencodesAtTheSameSizeOnlyWhenTold()
{
    const QString path = writeImagePdf(QStringLiteral("force-in.pdf"), quarteredPicture(600), QSizeF(200, 200),
                                       { QRectF(0, 0, 200, 200) });
    const qint64 before = QFileInfo(path).size();

    // The picture is already at the resolution being asked for, so without
    // being told otherwise this must cost nothing at all.
    ImageEdit::Recompress gentle;
    gentle.targetDpi = 216; // 600 px over 200 pt
    gentle.jpegQuality = 20;

    const QString same = m_dir.filePath(QStringLiteral("force-same.pdf"));
    qint64 saved = -1;
    QString error;
    QVERIFY2(ImageEdit::recompress(path, same, {}, {}, gentle, &saved, &error), qPrintable(error));
    QCOMPARE(saved, 0);
    QCOMPARE(storedImageBytes(same, 0), storedImageBytes(path, 0));

    // Asked for it explicitly, the same request has to bite.
    ImageEdit::Recompress forced = gentle;
    forced.forceReencode = true;

    const QString smaller = m_dir.filePath(QStringLiteral("force-small.pdf"));
    QVERIFY2(ImageEdit::recompress(path, smaller, {}, {}, forced, &saved, &error), qPrintable(error));
    QVERIFY2(saved > 0, qPrintable(QStringLiteral("saved %1 bytes").arg(saved)));
    QVERIFY(QFileInfo(smaller).size() < before);

    // Smaller in bytes, unchanged in pixels, which is the whole point of the
    // flag, and a downsample would satisfy the size check on its own.
    const QVector<ImageEdit::ImageUse> after = ImageEdit::read(smaller, &error);
    QVERIFY2(!after.isEmpty(), qPrintable(error));
    QCOMPARE(after.constFirst().pixelSize, QSize(600, 600));
}

void TestImageedit::removeTakesThePictureOffThePage()
{
    const QString path = writeImagePdf(QStringLiteral("remove-in.pdf"), quarteredPicture(200), QSizeF(200, 200),
                                       { QRectF(50, 50, 100, 100), QRectF(50, 50, 100, 100) }, true);
    const QString out = m_dir.filePath(QStringLiteral("remove-out.pdf"));

    QString error;
    QVERIFY2(ImageEdit::remove(path, out, 0, QStringLiteral("/Im0"), &error), qPrintable(error));

    const QVector<ImageEdit::ImageUse> uses = ImageEdit::read(out, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(uses.size(), 1);
    QCOMPARE(uses.constFirst().page, 1);

    // Gone from the page, and gone from the file: nothing else pointed at it.
    QCOMPARE(distinctImageObjects(out), 1);
    QCOMPARE(qRound(meanBrightness(renderedPage(out, 0, 200))), 255);
    QVERIFY(meanBrightness(renderedPage(out, 1, 200)) < 250.0);
}

void TestImageedit::deduplicateMergesIdenticalPictures()
{
    const QString path
        = writeImagePdf(QStringLiteral("dupes-in.pdf"), quarteredPicture(300), QSizeF(200, 200),
                        { QRectF(20, 20, 100, 100), QRectF(20, 20, 100, 100), QRectF(20, 20, 100, 100) }, true);
    const QString out = m_dir.filePath(QStringLiteral("dupes-out.pdf"));

    QCOMPARE(distinctImageObjects(path), 3);
    const qint64 storedEach = storedImageOf(path, 0).raw.size();
    QVERIFY(storedEach > 0);

    int merged = -1;
    qint64 saved = -1;
    QString error;
    QVERIFY2(ImageEdit::deduplicate(path, out, &merged, &saved, &error), qPrintable(error));
    QCOMPARE(merged, 2);
    QVERIFY2(saved > storedEach, qPrintable(QStringLiteral("saved only %1").arg(saved)));

    QCOMPARE(distinctImageObjects(out), 1);
    QVERIFY(QFileInfo(out).size() < QFileInfo(path).size());

    // Merging must not have cost any page its picture.
    const QVector<ImageEdit::ImageUse> uses = ImageEdit::read(out, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(uses.size(), 3);
    for (const ImageEdit::ImageUse &use : uses) {
        QCOMPARE(use.pixelSize, QSize(300, 300));
        QCOMPARE(use.drawnTimes, 3);
        QVERIFY(meanBrightness(renderedPage(out, use.page, 200)) < 250.0);
    }

    // A document with nothing to merge must come away saying so rather than
    // finding duplicates that are not there.
    const QString shared = writeImagePdf(QStringLiteral("dupes-none.pdf"), quarteredPicture(120), QSizeF(200, 200),
                                         { QRectF(20, 20, 60, 60), QRectF(20, 20, 60, 60) });
    merged = -1;
    saved = -1;
    QVERIFY2(
        ImageEdit::deduplicate(shared, m_dir.filePath(QStringLiteral("dupes-none-out.pdf")), &merged, &saved, &error),
        qPrintable(error));
    QCOMPARE(merged, 0);
    QCOMPARE(saved, 0);
}

void TestImageedit::refusesPicturesItCannotOpen()
{
    const QString path = writeUnreadablePdf(QStringLiteral("jpeg2000.pdf"));

    QString error;
    const QVector<ImageEdit::ImageUse> uses = ImageEdit::read(path, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(uses.size(), 1);
    QCOMPARE(uses.constFirst().encoding, QStringLiteral("JPXDecode"));
    QVERIFY2(!uses.constFirst().decodable, "a JPEG 2000 image was claimed to be readable");
    QCOMPARE(qRound(uses.constFirst().effectiveDpiX), 1000);

    // Every operation that needs pixels says no, by name, rather than dropping
    // the picture or putting something grey in its place.
    ImageEdit::Recompress recompress;
    recompress.targetDpi = 50.0;
    qint64 saved = 0;
    error.clear();
    QVERIFY(!ImageEdit::recompress(path, m_dir.filePath(QStringLiteral("jpx-recompressed.pdf")), { 0 },
                                   { QStringLiteral("/Im0") }, recompress, &saved, &error));
    QVERIFY(!error.isEmpty());

    error.clear();
    QVERIFY(!ImageEdit::adjust(path, m_dir.filePath(QStringLiteral("jpx-adjusted.pdf")), 0, QStringLiteral("/Im0"), {},
                               &error));
    QVERIFY(!error.isEmpty());

    error.clear();
    QVERIFY(!ImageEdit::crop(path, m_dir.filePath(QStringLiteral("jpx-cropped.pdf")), 0, QStringLiteral("/Im0"),
                             QRectF(0, 0, 0.5, 0.5), &error));
    QVERIFY(!error.isEmpty());

    error.clear();
    QVERIFY(!ImageEdit::extract(path, 0, QStringLiteral("/Im0"), m_dir.filePath(QStringLiteral("jpx.png")), &error));
    QVERIFY(!error.isEmpty());

    // A whole-document sweep steps over it instead, because one picture it
    // cannot open is no reason to refuse to compress the other two hundred.
    error.clear();
    saved = -1;
    QVERIFY2(ImageEdit::recompress(path, m_dir.filePath(QStringLiteral("jpx-swept.pdf")), {}, {}, recompress, &saved,
                                   &error),
             qPrintable(error));
    QCOMPARE(saved, 0);
    QCOMPARE(storedImageOf(m_dir.filePath(QStringLiteral("jpx-swept.pdf")), 0).raw.size(), qsizetype(2048));

    // Removing it needs no pixels at all, so that must still work.
    error.clear();
    QVERIFY2(
        ImageEdit::remove(path, m_dir.filePath(QStringLiteral("jpx-removed.pdf")), 0, QStringLiteral("/Im0"), &error),
        qPrintable(error));
    QCOMPARE(ImageEdit::read(m_dir.filePath(QStringLiteral("jpx-removed.pdf")), &error).size(), 0);

    // And a name that is not there is a refusal, not a crash.
    error.clear();
    QVERIFY(!ImageEdit::crop(path, m_dir.filePath(QStringLiteral("nothing.pdf")), 0, QStringLiteral("/NotThere"),
                             QRectF(0, 0, 1, 1), &error));
    QVERIFY(!error.isEmpty());
    error.clear();
    QVERIFY(!ImageEdit::remove(path, m_dir.filePath(QStringLiteral("nothing.pdf")), 9, QStringLiteral("/Im0"), &error));
    QVERIFY(!error.isEmpty());
}

void TestImageedit::reportsItsOwnLimits()
{
    const QStringList limits = ImageEdit::limitations();
    QVERIFY(limits.size() >= 5);
    for (const QString &limit : limits) {
        QVERIFY(!limit.trimmed().isEmpty());
    }
}

QTEST_MAIN(TestImageedit)

#include "tst_imageedit.moc"
