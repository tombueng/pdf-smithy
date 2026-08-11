/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#include "TestPdf.h"

#include "core/ImageIO.h"
#include "render/PopplerBackend.h"

#include <QDir>
#include <QImage>
#include <QTemporaryDir>
#include <QTest>

#include <cmath>
#include <numeric>

#include <KLocalizedString>

using namespace ps;

/**
 * Scans in, pictures out.
 *
 * The size arithmetic is what these tests are really for: an image imported
 * without regard for its resolution produces a page measured in pixels, which
 * prints as a poster. Getting that wrong is invisible until someone tries to
 * put the result on paper.
 */
class TestImageIO : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();

    void buildsAPdfFromImages();
    void sizesPagesFromTheImageResolution();
    void fitsImagesIntoAFixedPageSize();
    void refusesAnEmptyList();
    void refusesFilesThatAreNotImages();

    void exportsPagesAsImages();
    void exportsAtTheRequestedResolution();
    void numbersExportsSoTheySort();
    void exportsEveryPageOfADocumentAssembledFromTwoFiles();
    void saysWhichPagesItCouldNotRender();

private:
    QStringList makeImages(int count, const QSize &size, int dpi);

    QTemporaryDir m_dir;
};

void TestImageIO::initTestCase()
{
    KLocalizedString::setApplicationDomain("pdf-smithy");
    QVERIFY(m_dir.isValid());
}

QStringList TestImageIO::makeImages(int count, const QSize &size, int dpi)
{
    QStringList paths;
    for (int i = 0; i < count; ++i) {
        QImage image(size, QImage::Format_RGB888);
        image.fill(i % 2 == 0 ? Qt::white : QColor(230, 235, 245));
        if (dpi > 0) {
            const int dotsPerMeter = static_cast<int>(std::lround(dpi / 0.0254));
            image.setDotsPerMeterX(dotsPerMeter);
            image.setDotsPerMeterY(dotsPerMeter);
        }
        const QString path = m_dir.filePath(QStringLiteral("in-%1.png").arg(i + 1));
        if (image.save(path)) {
            paths.append(path);
        }
    }
    return paths;
}

// ── Images to PDF ─────────────────────────────────────────────────────────

void TestImageIO::buildsAPdfFromImages()
{
    const QStringList images = makeImages(3, QSize(600, 800), 300);
    QCOMPARE(images.size(), 3);

    const QString out = m_dir.filePath(QStringLiteral("built.pdf"));
    QString error;
    QVERIFY2(ImageIO::imagesToPdf(images, out, ImageIO::ImportOptions {}, &error), qPrintable(error));

    QCOMPARE(test::pageCountOf(out), 3);
}

void TestImageIO::sizesPagesFromTheImageResolution()
{
    // 600 x 800 pixels at 300 dpi is two inches by two and two thirds, so the
    // page has to come out at 144 x 192 points, not at 600 x 800.
    const QStringList images = makeImages(1, QSize(600, 800), 300);
    const QString out = m_dir.filePath(QStringLiteral("sized.pdf"));
    QVERIFY(ImageIO::imagesToPdf(images, out, ImageIO::ImportOptions {}, nullptr));

    PopplerBackend backend;
    QVERIFY(backend.addDocument(1, out, nullptr));
    const QSizeF size = backend.pageSizePoints(1, 0);

    QVERIFY2(std::abs(size.width() - 144.0) < 1.0,
             qPrintable(QStringLiteral("page is %1 points wide, expected about 144").arg(size.width())));
    QVERIFY2(std::abs(size.height() - 192.0) < 1.0,
             qPrintable(QStringLiteral("page is %1 points tall, expected about 192").arg(size.height())));
}

void TestImageIO::fitsImagesIntoAFixedPageSize()
{
    const QStringList images = makeImages(2, QSize(1000, 500), 300);
    const QString out = m_dir.filePath(QStringLiteral("a4.pdf"));

    ImageIO::ImportOptions options;
    options.pageSizePoints = QSizeF(595, 842); // A4
    options.marginPoints = 20;

    QString error;
    QVERIFY2(ImageIO::imagesToPdf(images, out, options, &error), qPrintable(error));

    PopplerBackend backend;
    QVERIFY(backend.addDocument(1, out, nullptr));
    QCOMPARE(test::pageCountOf(out), 2);

    const QSizeF size = backend.pageSizePoints(1, 0);
    QVERIFY2(std::abs(size.width() - 595.0) < 1.0, "a fixed page size was ignored");
    QVERIFY2(std::abs(size.height() - 842.0) < 1.0, "a fixed page size was ignored");
}

void TestImageIO::refusesAnEmptyList()
{
    QString error;
    QVERIFY(!ImageIO::imagesToPdf({}, m_dir.filePath(QStringLiteral("none.pdf")), ImageIO::ImportOptions {}, &error));
    QVERIFY(!error.isEmpty());
}

void TestImageIO::refusesFilesThatAreNotImages()
{
    const QString notAnImage = m_dir.filePath(QStringLiteral("notes.txt"));
    QFile file(notAnImage);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("this is not a picture");
    file.close();

    QString error;
    QVERIFY(!ImageIO::imagesToPdf({ notAnImage }, m_dir.filePath(QStringLiteral("bad.pdf")), ImageIO::ImportOptions {},
                                  &error));
    QVERIFY2(!error.isEmpty(), "a refusal has to explain itself");
}

// ── PDF to images ─────────────────────────────────────────────────────────

void TestImageIO::exportsPagesAsImages()
{
    const QString source = m_dir.filePath(QStringLiteral("source.pdf"));
    QVERIFY(test::writeSamplePdf(source, 3));

    PopplerBackend backend;
    QVERIFY(backend.addDocument(1, source, nullptr));

    QStringList written;
    QString error;
    QVERIFY2(ImageIO::pagesToImages(&backend, 1, { 0, 1, 2 }, {}, m_dir.filePath(QStringLiteral("page-%1.png")),
                                    ImageIO::ExportOptions {}, &written, &error),
             qPrintable(error));

    QCOMPARE(written.size(), 3);
    for (const QString &path : written) {
        QVERIFY2(QFileInfo::exists(path), qPrintable(path));
        QImage image(path);
        QVERIFY2(!image.isNull(), qPrintable(QStringLiteral("%1 is not a readable image").arg(path)));
    }
}

void TestImageIO::exportsAtTheRequestedResolution()
{
    const QString source = m_dir.filePath(QStringLiteral("res.pdf"));
    QVERIFY(test::writeSamplePdf(source, 1)); // 612 x 792 points

    PopplerBackend backend;
    QVERIFY(backend.addDocument(1, source, nullptr));

    ImageIO::ExportOptions options;
    options.dpi = 300;

    QStringList written;
    QVERIFY(ImageIO::pagesToImages(&backend, 1, { 0 }, {}, m_dir.filePath(QStringLiteral("hi-%1.png")), options,
                                   &written, nullptr));
    QCOMPARE(written.size(), 1);

    // 612 points is 8.5 inches, so 300 dpi means 2550 pixels across.
    const QImage image(written.constFirst());
    QVERIFY2(std::abs(image.width() - 2550) < 4,
             qPrintable(QStringLiteral("exported %1 pixels wide, expected about 2550").arg(image.width())));
}

void TestImageIO::numbersExportsSoTheySort()
{
    const QString source = m_dir.filePath(QStringLiteral("many.pdf"));
    QVERIFY(test::writeSamplePdf(source, 12));

    PopplerBackend backend;
    QVERIFY(backend.addDocument(1, source, nullptr));

    QVector<int> pages(12);
    std::iota(pages.begin(), pages.end(), 0);

    ImageIO::ExportOptions options;
    options.dpi = 24; // small and quick; this test is about the names

    QStringList written;
    QVERIFY(ImageIO::pagesToImages(&backend, 1, pages, {}, m_dir.filePath(QStringLiteral("sorted-%1.png")), options,
                                   &written, nullptr));
    QCOMPARE(written.size(), 12);

    // Twelve files means two digits, so page one is "01" and sorts before ten.
    QVERIFY2(written.constFirst().endsWith(QLatin1String("sorted-01.png")), qPrintable(written.constFirst()));
    QVERIFY2(written.constLast().endsWith(QLatin1String("sorted-12.png")), qPrintable(written.constLast()));

    QStringList sorted = written;
    sorted.sort();
    QCOMPARE(sorted, written);
}

void TestImageIO::exportsEveryPageOfADocumentAssembledFromTwoFiles()
{
    // The case the old interface could not express: it took one source id and a
    // list of page numbers, so a document merged from two files exported the
    // first file's pages under the second file's numbers, and said it had
    // succeeded, because the pages it could not find were skipped in silence.
    const QString first = m_dir.filePath(QStringLiteral("first.pdf"));
    const QString second = m_dir.filePath(QStringLiteral("second.pdf"));
    QVERIFY(test::writeSamplePdf(first, 2));
    QVERIFY(test::writeSamplePdf(second, 3));

    PopplerBackend backend;
    QString error;
    QVERIFY2(backend.addDocument(0, first, &error), qPrintable(error));
    QVERIFY2(backend.addDocument(1, second, &error), qPrintable(error));

    const QVector<PageRef> pages {
        PageRef { 0, 0, 0 }, PageRef { 1, 0, 0 }, PageRef { 1, 1, 0 }, PageRef { 0, 1, 0 }, PageRef { 1, 2, 0 },
    };

    const QString target = m_dir.filePath(QStringLiteral("merged-%1.png"));
    QStringList written;
    QVector<int> skipped;
    ImageIO::ExportOptions options;
    options.dpi = 36; // small: this is about which page, not about how it looks

    QVERIFY2(ImageIO::pagesToImages(&backend, pages, target, options, &written, &skipped, &error), qPrintable(error));

    QVERIFY2(skipped.isEmpty(), "a page of a merged document was skipped, which is how the old bug hid itself");
    QCOMPARE(written.size(), pages.size());
    for (const QString &path : std::as_const(written)) {
        const QImage image(path);
        QVERIFY2(!image.isNull(), qPrintable(QStringLiteral("“%1” was reported written but cannot be read").arg(path)));
    }
}

void TestImageIO::saysWhichPagesItCouldNotRender()
{
    const QString file = m_dir.filePath(QStringLiteral("three.pdf"));
    QVERIFY(test::writeSamplePdf(file, 3));

    PopplerBackend backend;
    QString error;
    QVERIFY2(backend.addDocument(0, file, &error), qPrintable(error));

    // The middle one cannot exist. Silently writing two files and calling that
    // success is what this guards: a partial export has to be able to say so.
    const QVector<PageRef> pages { PageRef { 0, 0, 0 }, PageRef { 0, 99, 0 }, PageRef { 0, 2, 0 } };

    const QString target = m_dir.filePath(QStringLiteral("gappy-%1.png"));
    QStringList written;
    QVector<int> skipped;
    QVERIFY(ImageIO::pagesToImages(&backend, pages, target, ImageIO::ExportOptions {}, &written, &skipped, &error));

    QCOMPARE(written.size(), 2);
    QCOMPARE(skipped, QVector<int> { 2 });
}

QTEST_MAIN(TestImageIO)

#include "tst_imageio.moc"
