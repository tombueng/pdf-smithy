/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#include "TestPdf.h"

#include "core/PageCrop.h"
#include "core/PageNumbering.h"
#include "render/PopplerBackend.h"

#include <QTemporaryDir>
#include <QFile>
#include <QTest>

#include <KLocalizedString>

#include <qpdf/QPDF.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>

using namespace ps;

/**
 * Numbering and cropping.
 *
 * Both are the sort of thing that looks right in a dialog and is wrong on
 * paper, so the checks read the result back out of the file rather than
 * trusting the call that produced it.
 */
class TestPageTools : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();

    void fillsPlaceholders_data();
    void fillsPlaceholders();
    void padsBatesNumbers();
    void stampsNumbersOntoPages();
    void numbersFollowTheOrderGiven();
    void refusesEmptyText();

    void setsACropBox();
    void mapsMarginsOnARotatedPage();
    void refusesToCropAwayEverything();
    void removesACrop();
    void detectsWhiteMargins();
    void commonMarginsAreTheSafestTrim();

private:
    /** The /CropBox of one page, or an invalid rect when there is none. */
    static QRectF cropBoxOf(const QString &path, int page);

    QTemporaryDir m_dir;
    QString m_plain;
};

void TestPageTools::initTestCase()
{
    KLocalizedString::setApplicationDomain("pdf-smithy");
    QVERIFY(m_dir.isValid());
    m_plain = m_dir.filePath(QStringLiteral("plain.pdf"));
    QVERIFY(test::writeSamplePdf(m_plain, 5));
}

QRectF TestPageTools::cropBoxOf(const QString &path, int page)
{
    try {
        QPDF pdf;
        pdf.processFile(QFile::encodeName(path).constData());
        auto pages = QPDFPageDocumentHelper(pdf).getAllPages();
        if (page < 0 || page >= static_cast<int>(pages.size())) {
            return {};
        }
        QPDFObjectHandle box = pages[static_cast<size_t>(page)].getObjectHandle().getKey("/CropBox");
        if (!box.isArray() || box.getArrayNItems() != 4) {
            return {};
        }
        const double left = box.getArrayItem(0).getNumericValue();
        const double bottom = box.getArrayItem(1).getNumericValue();
        const double right = box.getArrayItem(2).getNumericValue();
        const double top = box.getArrayItem(3).getNumericValue();
        return QRectF(left, bottom, right - left, top - bottom);
    } catch (const std::exception &) {
        return {};
    }
}

// ── Numbering ─────────────────────────────────────────────────────────────

void TestPageTools::fillsPlaceholders_data()
{
    QTest::addColumn<QString>("templateText");
    QTest::addColumn<int>("index");
    QTest::addColumn<int>("total");
    QTest::addColumn<QString>("expected");

    QTest::newRow("page of pages") << QStringLiteral("{page} / {pages}") << 0 << 5 << QStringLiteral("1 / 5");
    QTest::newRow("later page") << QStringLiteral("{page} / {pages}") << 3 << 5 << QStringLiteral("4 / 5");
    QTest::newRow("words around it") << QStringLiteral("Seite {page} von {pages}") << 1 << 9
                                     << QStringLiteral("Seite 2 von 9");
    QTest::newRow("file name") << QStringLiteral("{file}") << 0 << 1 << QStringLiteral("vertrag.pdf");
    QTest::newRow("no placeholder") << QStringLiteral("Entwurf") << 2 << 4 << QStringLiteral("Entwurf");
}

void TestPageTools::fillsPlaceholders()
{
    QFETCH(QString, templateText);
    QFETCH(int, index);
    QFETCH(int, total);
    QFETCH(QString, expected);

    PageNumbering::Options options;
    options.text = templateText;
    options.fileName = QStringLiteral("vertrag.pdf");
    QCOMPARE(PageNumbering::render(options, index, total), expected);
}

void TestPageTools::padsBatesNumbers()
{
    PageNumbering::Options options;
    options.text = QStringLiteral("{bates}");
    options.batesPrefix = QStringLiteral("ACME-");
    options.batesDigits = 6;
    options.startNumber = 1;

    // The whole point of a Bates number is that it sorts and quotes cleanly.
    QCOMPARE(PageNumbering::render(options, 0, 3), QStringLiteral("ACME-000001"));
    QCOMPARE(PageNumbering::render(options, 41, 3), QStringLiteral("ACME-000042"));

    options.startNumber = 500;
    QCOMPARE(PageNumbering::render(options, 0, 3), QStringLiteral("ACME-000500"));

    options.batesDigits = 0;
    QCOMPARE(PageNumbering::render(options, 0, 3), QStringLiteral("ACME-500"));
}

void TestPageTools::stampsNumbersOntoPages()
{
    const QString out = m_dir.filePath(QStringLiteral("numbered.pdf"));

    PageNumbering::Options options;
    options.text = QStringLiteral("Seite {page} von {pages}");

    QString error;
    QVERIFY2(PageNumbering::apply(m_plain, out, { 0, 1, 2, 3, 4 }, options, &error), qPrintable(error));

    PopplerBackend backend;
    QVERIFY(backend.addDocument(1, out, nullptr));
    QVERIFY2(backend.extractText(1, 0).contains(QStringLiteral("Seite 1 von 5")),
             qPrintable(backend.extractText(1, 0).left(120)));
    QVERIFY(backend.extractText(1, 4).contains(QStringLiteral("Seite 5 von 5")));

    // And the pages underneath are untouched.
    QVERIFY(test::contentOf(out, 2).contains(QStringLiteral("PSPAGE 3")));
}

void TestPageTools::numbersFollowTheOrderGiven()
{
    const QString out = m_dir.filePath(QStringLiteral("subset.pdf"));

    PageNumbering::Options options;
    options.text = QStringLiteral("[{page}]");

    // Only three of the five pages, so they are numbered one to three: the
    // number is the position in the run, not the page's index in the file.
    QVERIFY(PageNumbering::apply(m_plain, out, { 1, 3, 4 }, options, nullptr));

    PopplerBackend backend;
    QVERIFY(backend.addDocument(1, out, nullptr));
    QVERIFY(backend.extractText(1, 1).contains(QStringLiteral("[1]")));
    QVERIFY(backend.extractText(1, 3).contains(QStringLiteral("[2]")));
    QVERIFY(backend.extractText(1, 4).contains(QStringLiteral("[3]")));
    QVERIFY2(!backend.extractText(1, 0).contains(QLatin1Char('[')), "a page that was not listed got numbered");
}

void TestPageTools::refusesEmptyText()
{
    PageNumbering::Options options;
    options.text = QStringLiteral("   ");

    QString error;
    QVERIFY(!PageNumbering::apply(m_plain, m_dir.filePath(QStringLiteral("x.pdf")), { 0 }, options, &error));
    QVERIFY(!error.isEmpty());
}

// ── Cropping ──────────────────────────────────────────────────────────────

void TestPageTools::setsACropBox()
{
    const QString out = m_dir.filePath(QStringLiteral("cropped.pdf"));

    PageCrop::Margins margins;
    margins.left = 20;
    margins.right = 30;
    margins.top = 40;
    margins.bottom = 10;

    QString error;
    QVERIFY2(PageCrop::crop(m_plain, out, { 0 }, margins, &error), qPrintable(error));

    // The source is 612 x 792 at the origin.
    const QRectF box = cropBoxOf(out, 0);
    QVERIFY(box.isValid());
    QCOMPARE(box.left(), 20.0);
    QCOMPARE(box.width(), 612.0 - 20.0 - 30.0);
    QCOMPARE(box.height(), 792.0 - 40.0 - 10.0);

    // Untouched pages keep no crop at all.
    QVERIFY2(!cropBoxOf(out, 1).isValid(), "a page that was not listed got cropped");
}

void TestPageTools::mapsMarginsOnARotatedPage()
{
    const QString rotated = m_dir.filePath(QStringLiteral("rotated.pdf"));
    QVERIFY(test::writeRotatedPdf(rotated, 1, 90));

    const QString out = m_dir.filePath(QStringLiteral("rotated-cropped.pdf"));

    // The user trims fifty points off what they see as the left. On a page
    // turned by ninety degrees that is the bottom of the page's own
    // coordinates, and trimming the actual left would cut the wrong edge.
    PageCrop::Margins margins;
    margins.left = 50;

    QVERIFY(PageCrop::crop(rotated, out, { 0 }, margins, nullptr));

    const QRectF box = cropBoxOf(out, 0);
    QVERIFY(box.isValid());
    QCOMPARE(box.left(), 0.0);
    QCOMPARE(box.bottom() - box.height(), 50.0);
    QCOMPARE(box.width(), 612.0);
}

void TestPageTools::refusesToCropAwayEverything()
{
    const QString out = m_dir.filePath(QStringLiteral("silly.pdf"));

    PageCrop::Margins margins;
    margins.left = 400;
    margins.right = 400;

    QVERIFY(PageCrop::crop(m_plain, out, { 0 }, margins, nullptr));
    // Asked to leave a negative-width page, it leaves the page alone instead.
    QVERIFY2(!cropBoxOf(out, 0).isValid(), "a nonsensical crop was applied anyway");
}

void TestPageTools::removesACrop()
{
    const QString cropped = m_dir.filePath(QStringLiteral("to-reset.pdf"));
    PageCrop::Margins margins;
    margins.left = 20;
    QVERIFY(PageCrop::crop(m_plain, cropped, { 0 }, margins, nullptr));
    QVERIFY(cropBoxOf(cropped, 0).isValid());

    const QString reset = m_dir.filePath(QStringLiteral("reset.pdf"));
    QVERIFY(PageCrop::reset(cropped, reset, { 0 }, nullptr));
    QVERIFY2(!cropBoxOf(reset, 0).isValid(), "the crop survived being reset");
}

void TestPageTools::detectsWhiteMargins()
{
    PopplerBackend backend;
    QVERIFY(backend.addDocument(1, m_plain, nullptr));

    // The fixture writes one short line near the top left, so there should be
    // a lot of empty paper to the right of it and below.
    const PageCrop::Margins margins = PageCrop::detect(&backend, 1, 0);

    QVERIFY2(margins.right > 100, qPrintable(QStringLiteral("right margin measured %1").arg(margins.right)));
    QVERIFY2(margins.bottom > 100, qPrintable(QStringLiteral("bottom margin measured %1").arg(margins.bottom)));
    // And it must not eat into the text.
    QVERIFY2(margins.left < 80, qPrintable(QStringLiteral("left margin measured %1").arg(margins.left)));
}

void TestPageTools::commonMarginsAreTheSafestTrim()
{
    PopplerBackend backend;
    QVERIFY(backend.addDocument(1, m_plain, nullptr));

    const PageCrop::Margins single = PageCrop::detect(&backend, 1, 0);
    const PageCrop::Margins common = PageCrop::detectCommon(&backend, 1, { 0, 1, 2, 3, 4 });

    // Across pages the trim can only ever shrink, never grow, or some page
    // would lose content.
    QVERIFY(common.left <= single.left + 0.01);
    QVERIFY(common.right <= single.right + 0.01);
    QVERIFY(common.top <= single.top + 0.01);
    QVERIFY(common.bottom <= single.bottom + 0.01);
}

QTEST_MAIN(TestPageTools)

#include "tst_pagetools.moc"
