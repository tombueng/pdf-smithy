/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#include "TestPdf.h"

#include "core/Overlay.h"
#include "render/PopplerBackend.h"

#include <QPainter>
#include <QTemporaryDir>
#include <QTest>

#include <KLocalizedString>

using namespace ps;

/**
 * The overlay engine is checked by looking at the result, not at the code that
 * produced it: the output is rendered and the pixels are sampled where the
 * stamp was asked to go. A content stream that parses but draws the signature
 * off the edge of the page would pass any structural test.
 */
class TestOverlay : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();

    void anchorsRectangles_data();
    void anchorsRectangles();

    void putsAStampWhereAsked();
    void keepsTheOriginalContent();
    void leavesOtherPagesUntouched();
    void honoursTransparency();
    void placesCorrectlyOnARotatedPage();

    void writesATextWatermark();
    void rotatedWatermarkIsVisible();
    void refusesEmptyWatermarkText();

private:
    /** Renders one page of @p path and returns it. */
    QImage render(const QString &path, int page, int widthPx = 600);

    /** Solid square of @p colour, used as a stamp that is easy to find again. */
    static QImage marker(const QColor &colour, int size = 64);

    QTemporaryDir m_dir;
    QString m_source;
};

void TestOverlay::initTestCase()
{
    KLocalizedString::setApplicationDomain("pdf-smithy");
    QVERIFY(m_dir.isValid());
    m_source = m_dir.filePath(QStringLiteral("source.pdf"));
    QVERIFY(test::writeSamplePdf(m_source, 3));
}

QImage TestOverlay::render(const QString &path, int page, int widthPx)
{
    PopplerBackend backend;
    QString error;
    if (!backend.addDocument(1, path, &error)) {
        return {};
    }
    const QImage image = backend.renderPage(1, page, widthPx);
    backend.removeDocument(1);
    return image;
}

QImage TestOverlay::marker(const QColor &colour, int size)
{
    QImage image(size, size, QImage::Format_ARGB32);
    image.fill(colour);
    return image;
}

// ── Anchoring ─────────────────────────────────────────────────────────────

void TestOverlay::anchorsRectangles_data()
{
    QTest::addColumn<Overlay::Anchor>("anchor");
    QTest::addColumn<QPointF>("expectedTopLeft");

    // A 600x800 page, a 100x50 item, 10 points of margin.
    QTest::newRow("bottom left") << Overlay::Anchor::BottomLeft << QPointF(10, 10);
    QTest::newRow("bottom right") << Overlay::Anchor::BottomRight << QPointF(490, 10);
    QTest::newRow("top left") << Overlay::Anchor::TopLeft << QPointF(10, 740);
    QTest::newRow("top right") << Overlay::Anchor::TopRight << QPointF(490, 740);
    QTest::newRow("centre") << Overlay::Anchor::Centre << QPointF(250, 375);
    QTest::newRow("bottom centre") << Overlay::Anchor::BottomCentre << QPointF(250, 10);
}

void TestOverlay::anchorsRectangles()
{
    QFETCH(Overlay::Anchor, anchor);
    QFETCH(QPointF, expectedTopLeft);

    const QRectF rect = Overlay::anchoredRect(anchor, QSizeF(600, 800), QSizeF(100, 50), 10);
    QCOMPARE(rect.left(), expectedTopLeft.x());
    QCOMPARE(rect.bottom() - rect.height(), expectedTopLeft.y());
    QCOMPARE(rect.size(), QSizeF(100, 50));
}

// ── Stamping ──────────────────────────────────────────────────────────────

void TestOverlay::putsAStampWhereAsked()
{
    const QString out = m_dir.filePath(QStringLiteral("stamped.pdf"));

    // Bottom-right corner of a 612x792 page, where a signature usually goes.
    Overlay::ImageStamp stamp;
    stamp.image = marker(Qt::red);
    stamp.rect = QRectF(400, 80, 150, 100);

    QString error;
    QVERIFY2(Overlay::stampImages(m_source, out, { { 0, { stamp } } }, &error), qPrintable(error));

    const QImage page = render(out, 0);
    QVERIFY(!page.isNull());

    // PDF counts upwards from the bottom, images downwards from the top.
    const double scale = page.width() / 612.0;
    const QPoint centre(static_cast<int>(stamp.rect.center().x() * scale),
                        static_cast<int>((792.0 - stamp.rect.center().y()) * scale));

    const QColor found = page.pixelColor(centre);
    QVERIFY2(
        found.red() > 200 && found.green() < 80 && found.blue() < 80,
        qPrintable(
            QStringLiteral("expected red at %1,%2 but found %3").arg(centre.x()).arg(centre.y()).arg(found.name())));

    // And nowhere else: a corner well away from the stamp stays white.
    const QColor elsewhere = page.pixelColor(QPoint(page.width() / 20, page.height() / 20));
    QVERIFY(elsewhere.red() > 200 && elsewhere.green() > 200);
}

void TestOverlay::keepsTheOriginalContent()
{
    const QString out = m_dir.filePath(QStringLiteral("kept.pdf"));

    Overlay::ImageStamp stamp;
    stamp.image = marker(Qt::blue);
    stamp.rect = QRectF(50, 50, 80, 40);

    QVERIFY(Overlay::stampImages(m_source, out, { { 0, { stamp } } }, nullptr));

    // Stamping must not disturb what was already on the page.
    QVERIFY2(test::contentOf(out, 0).contains(QStringLiteral("PSPAGE 1")),
             "the original page content did not survive stamping");
    QCOMPARE(test::pageCountOf(out), 3);
}

void TestOverlay::leavesOtherPagesUntouched()
{
    const QString out = m_dir.filePath(QStringLiteral("selective.pdf"));

    Overlay::ImageStamp stamp;
    stamp.image = marker(Qt::red);
    stamp.rect = QRectF(200, 300, 200, 200);

    QVERIFY(Overlay::stampImages(m_source, out, { { 1, { stamp } } }, nullptr));

    const QImage stamped = render(out, 1);
    const QImage untouched = render(out, 0);
    QVERIFY(!stamped.isNull() && !untouched.isNull());

    const double scale = stamped.width() / 612.0;
    const QPoint probe(static_cast<int>(300 * scale), static_cast<int>((792.0 - 400) * scale));

    QVERIFY2(stamped.pixelColor(probe).red() > 200 && stamped.pixelColor(probe).green() < 80,
             "the stamp is missing from the page it was asked for");
    QVERIFY2(untouched.pixelColor(probe).green() > 200, "a page that was not listed got stamped anyway");
}

void TestOverlay::honoursTransparency()
{
    const QString solid = m_dir.filePath(QStringLiteral("solid.pdf"));
    const QString faint = m_dir.filePath(QStringLiteral("faint.pdf"));

    Overlay::ImageStamp stamp;
    stamp.image = marker(Qt::black);
    stamp.rect = QRectF(200, 300, 200, 200);
    QVERIFY(Overlay::stampImages(m_source, solid, { { 0, { stamp } } }, nullptr));

    stamp.opacity = 0.2;
    QVERIFY(Overlay::stampImages(m_source, faint, { { 0, { stamp } } }, nullptr));

    const double scale = 600 / 612.0;
    const QPoint probe(static_cast<int>(300 * scale), static_cast<int>((792.0 - 400) * scale));

    const int solidLightness = render(solid, 0).pixelColor(probe).lightness();
    const int faintLightness = render(faint, 0).pixelColor(probe).lightness();

    QVERIFY2(solidLightness < 60, "the opaque stamp is not opaque");
    QVERIFY2(
        faintLightness > solidLightness + 60,
        qPrintable(
            QStringLiteral("opacity had no effect: solid %1 vs faint %2").arg(solidLightness).arg(faintLightness)));
}

void TestOverlay::placesCorrectlyOnARotatedPage()
{
    // The trap: word boxes and stamp rectangles are given in display space,
    // content streams are written in page space. Get it wrong and the
    // signature lands off the paper.
    const QString rotated = m_dir.filePath(QStringLiteral("rotated-source.pdf"));
    QVERIFY(test::writeRotatedPdf(rotated, 1, 90));

    const QString out = m_dir.filePath(QStringLiteral("rotated-stamped.pdf"));

    Overlay::ImageStamp stamp;
    stamp.image = marker(Qt::red);
    // A 90-degree page shows as 792 wide by 612 tall.
    stamp.rect = QRectF(600, 60, 120, 90);

    QString error;
    QVERIFY2(Overlay::stampImages(rotated, out, { { 0, { stamp } } }, &error), qPrintable(error));

    const QImage page = render(out, 0, 792);
    QVERIFY(!page.isNull());
    QCOMPARE(page.width(), 792);

    const QPoint centre(static_cast<int>(stamp.rect.center().x()), static_cast<int>(612.0 - stamp.rect.center().y()));
    const QColor found = page.pixelColor(centre);
    QVERIFY2(found.red() > 200 && found.green() < 80,
             qPrintable(QStringLiteral("stamp missing on the rotated page at %1,%2, found %3")
                            .arg(centre.x())
                            .arg(centre.y())
                            .arg(found.name())));
}

// ── Watermarks ────────────────────────────────────────────────────────────

void TestOverlay::writesATextWatermark()
{
    const QString out = m_dir.filePath(QStringLiteral("watermarked.pdf"));

    Overlay::TextStamp stamp;
    stamp.text = QStringLiteral("VERTRAULICH");
    stamp.opacity = 0.3;
    // Level, so that extraction returns one contiguous word. A turned
    // watermark is still real text (see rotatedWatermarkIsVisible), but
    // Poppler splits diagonal runs into fragments while assembling lines, and
    // that is a property of the reader rather than of the document.
    stamp.rotation = 0.0;

    QString error;
    QVERIFY2(Overlay::stampText(m_source, out, { 0, 1, 2 }, stamp, &error), qPrintable(error));

    PopplerBackend backend;
    QVERIFY(backend.addDocument(1, out, nullptr));
    for (int page = 0; page < 3; ++page) {
        QVERIFY2(backend.extractText(1, page).contains(QStringLiteral("VERTRAULICH")),
                 qPrintable(QStringLiteral("page %1 has no watermark").arg(page + 1)));
    }
    // The pages underneath still say what they said.
    QVERIFY(test::contentOf(out, 2).contains(QStringLiteral("PSPAGE 3")));
}

void TestOverlay::rotatedWatermarkIsVisible()
{
    const QString out = m_dir.filePath(QStringLiteral("diagonal.pdf"));

    Overlay::TextStamp stamp;
    stamp.text = QStringLiteral("ENTWURF");
    stamp.rotation = 45.0;
    stamp.opacity = 1.0;
    stamp.colour = Qt::red;
    stamp.fontSize = 72;

    QVERIFY(Overlay::stampText(m_source, out, { 0 }, stamp, nullptr));

    // Checked by looking at the page: somewhere across the middle band there
    // now has to be red where the plain document was white.
    const QImage plain = render(m_source, 0);
    const QImage marked = render(out, 0);
    QVERIFY(!plain.isNull() && !marked.isNull());

    int reddened = 0;
    for (int y = marked.height() / 3; y < 2 * marked.height() / 3; ++y) {
        for (int x = 0; x < marked.width(); ++x) {
            const QColor before = plain.pixelColor(x, y);
            const QColor after = marked.pixelColor(x, y);
            if (before.green() > 200 && after.red() > 120 && after.green() < 150) {
                ++reddened;
            }
        }
    }
    QVERIFY2(reddened > 500,
             qPrintable(QStringLiteral("only %1 pixels changed, so the watermark is not drawn").arg(reddened)));

    // And the characters really are text, not a picture of text.
    PopplerBackend backend;
    QVERIFY(backend.addDocument(1, out, nullptr));
    QVERIFY2(backend.extractText(1, 0).contains(QLatin1Char('E')), "the diagonal watermark left no text behind at all");
}

void TestOverlay::refusesEmptyWatermarkText()
{
    Overlay::TextStamp stamp;
    stamp.text = QStringLiteral("   ");

    QString error;
    QVERIFY(!Overlay::stampText(m_source, m_dir.filePath(QStringLiteral("empty.pdf")), { 0 }, stamp, &error));
    QVERIFY(!error.isEmpty());
}

QTEST_MAIN(TestOverlay)

#include "tst_overlay.moc"
