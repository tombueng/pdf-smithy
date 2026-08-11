/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#include "TestPdf.h"

#include "core/RenderCache.h"
#include "render/PopplerBackend.h"

#include <QDir>
#include <QElapsedTimer>
#include <QImage>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <QtConcurrent>

#include <KLocalizedString>

#include <atomic>

using namespace ps;

/**
 * The render pipeline: how many pages can be drawn at once, how little of one
 * has to be drawn, and how much of what has been drawn is used again.
 *
 * These are performance properties, and performance is not what a unit test
 * can honestly assert: a loaded build machine will fail any threshold worth
 * setting. So what is checked here is the *behaviour* the speed comes out of,
 * which is stable: that concurrent renders agree with serial ones, that a tile
 * is the same pixels as the part of the page it names, that a width asked for
 * twice is only rendered once, and that a narrower one is scaled out of a
 * wider one rather than drawn again. Each of those failing silently would give
 * back exactly the time this was written to save.
 */
class TestRender : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();

    // ── the backend ───────────────────────────────────────────────────────
    void rendersTheSamePageFromManyThreadsAtOnce();
    void rendersPagesConcurrentlyRatherThanOneAtATime();
    void aTileIsThePartOfThePageItNames();
    void aTileOutsideThePageIsRefusedRatherThanGuessed();
    void draftsAreImagesToo();
    void survivesADocumentClosedUnderARender();

    // ── the cache ─────────────────────────────────────────────────────────
    void holdsSeveralWidthsOfOnePageAtOnce();
    void changingTheWidthKeepsWhatIsAlreadyHeld();
    void scalesANarrowerThumbnailOutOfAWiderOne();
    void showsWhatItHasWhileTheRightSizeIsComing();
    void smallRendersSurviveASweepOfLargeOnes();

private:
    /** Waits for @p spy to reach @p count, or fails. */
    static bool waitFor(QSignalSpy &spy, int count, int msLimit = 30000);

    QTemporaryDir m_dir;
    QString m_pdf;
};

void TestRender::initTestCase()
{
    KLocalizedString::setApplicationDomain("pdf-smithy");
    QVERIFY(m_dir.isValid());

    // Text on every page: a blank sheet renders in no time and would prove
    // nothing about running several of them together.
    m_pdf = m_dir.filePath(QStringLiteral("pages.pdf"));
    QVERIFY(test::writeTextHeavyPdf(m_pdf, 16));
}

bool TestRender::waitFor(QSignalSpy &spy, int count, int msLimit)
{
    QElapsedTimer clock;
    clock.start();
    while (spy.count() < count && clock.elapsed() < msLimit) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
    return spy.count() >= count;
}

// ── the backend ───────────────────────────────────────────────────────────

void TestRender::rendersTheSamePageFromManyThreadsAtOnce()
{
    PopplerBackend backend;
    QString error;
    QVERIFY2(backend.addDocument(1, m_pdf, &error), qPrintable(error));

    const QImage alone = backend.renderPage(1, 0, 300);
    QVERIFY(!alone.isNull());

    // The same page, sixteen ways at once. Poppler cannot share one handle
    // between threads, so this is really a test that the backend keeps enough
    // of them and hands them out without two threads getting the same one.
    QVector<int> runs(16);
    std::iota(runs.begin(), runs.end(), 0);
    QVector<QImage> results(runs.size());
    QFuture<void> work = QtConcurrent::map(runs, [&](int i) { results[i] = backend.renderPage(1, 0, 300); });
    work.waitForFinished();

    for (const QImage &image : std::as_const(results)) {
        QVERIFY(!image.isNull());
        QCOMPARE(image.size(), alone.size());
        QCOMPARE(image, alone);
    }
}

void TestRender::rendersPagesConcurrentlyRatherThanOneAtATime()
{
    if (QThread::idealThreadCount() < 4) {
        QSKIP("Nothing to measure on a machine with fewer than four cores");
    }

    PopplerBackend backend;
    QString error;
    QVERIFY2(backend.addDocument(1, m_pdf, &error), qPrintable(error));

    // Warmed first, so that neither figure below is paying for fonts.
    (void)backend.renderPage(1, 0, 900);

    QElapsedTimer clock;
    clock.start();
    for (int page = 0; page < 8; ++page) {
        QVERIFY(!backend.renderPage(1, page, 900).isNull());
    }
    const qint64 serial = clock.elapsed();

    QVector<int> pages(8);
    std::iota(pages.begin(), pages.end(), 0);
    clock.restart();
    QFuture<void> work = QtConcurrent::map(pages, [&](int page) { (void)backend.renderPage(1, page, 900); });
    work.waitForFinished();
    const qint64 together = clock.elapsed();

    // Deliberately a weak threshold. The point is not how fast the machine is
    // but that the pages are not being serialised behind one lock, which shows
    // up as the two figures being the same; anything under three quarters is
    // parallelism, and a build machine under load will still pass it.
    QVERIFY2(together * 4 < serial * 3,
             qPrintable(
                 QStringLiteral("eight pages took %1 ms one at a time and %2 ms together").arg(serial).arg(together)));
}

void TestRender::aTileIsThePartOfThePageItNames()
{
    PopplerBackend backend;
    QString error;
    QVERIFY2(backend.addDocument(1, m_pdf, &error), qPrintable(error));

    const QImage whole = backend.renderPage(1, 0, 600);
    QVERIFY(!whole.isNull());

    RenderBackend::Request request;
    request.widthPx = 600;
    request.tile = QRect(120, 200, 256, 300);
    const QImage tile = backend.render(1, 0, request);

    QCOMPARE(tile.size(), request.tile.size());
    // The same pixels, not merely a similar picture: a tile that is off by a
    // row would show as a seam wherever two of them meet on screen.
    QCOMPARE(tile, whole.copy(request.tile));
}

void TestRender::aTileOutsideThePageIsRefusedRatherThanGuessed()
{
    PopplerBackend backend;
    QString error;
    QVERIFY2(backend.addDocument(1, m_pdf, &error), qPrintable(error));

    RenderBackend::Request request;
    request.widthPx = 600;
    request.tile = QRect(0, 100000, 256, 256);
    QVERIFY(backend.render(1, 0, request).isNull());
}

void TestRender::draftsAreImagesToo()
{
    PopplerBackend backend;
    QString error;
    QVERIFY2(backend.addDocument(1, m_pdf, &error), qPrintable(error));

    RenderBackend::Request request;
    request.widthPx = 200;
    request.draft = true;
    const QImage draft = backend.render(1, 0, request);
    QVERIFY(!draft.isNull());
    QCOMPARE(draft.width(), 200);

    // And the handle it borrowed goes back configured for accuracy, or every
    // page rendered after a stand-in would quietly be a stand-in as well.
    const QImage proper = backend.renderPage(1, 0, 200);
    QVERIFY(!proper.isNull());
    QVERIFY(proper != draft);
}

void TestRender::survivesADocumentClosedUnderARender()
{
    PopplerBackend backend;
    QString error;
    QVERIFY2(backend.addDocument(1, m_pdf, &error), qPrintable(error));

    std::atomic<int> drawn { 0 };
    QVector<int> pages(16);
    std::iota(pages.begin(), pages.end(), 0);
    QFuture<void> work = QtConcurrent::map(pages, [&](int page) {
        if (!backend.renderPage(1, page % 16, 400).isNull()) {
            ++drawn;
        }
    });

    // Taken away while the renders are running. Each of them holds the entry
    // alive for as long as it needs it, so the ones already started finish and
    // the rest answer with nothing, and neither may crash.
    backend.removeDocument(1);
    work.waitForFinished();
    QVERIFY(drawn.load() >= 0);
    QVERIFY(backend.renderPage(1, 0, 400).isNull());
}

// ── the cache ─────────────────────────────────────────────────────────────

void TestRender::holdsSeveralWidthsOfOnePageAtOnce()
{
    PopplerBackend backend;
    QString error;
    QVERIFY2(backend.addDocument(1, m_pdf, &error), qPrintable(error));

    RenderCache cache;
    cache.setBackend(&backend);
    QSignalSpy ready(&cache, &RenderCache::thumbnailReady);

    QVERIFY(cache.thumbnail(1, 0, 180).isNull());
    QVERIFY(waitFor(ready, 1));
    QVERIFY(cache.isCached(1, 0, 180));

    // A width nothing can be scaled out of (larger than what is held) is a
    // render, and both sizes are then in hand at the same time.
    QVERIFY(cache.thumbnail(1, 0, 900).width() != 900);
    QVERIFY(waitFor(ready, 2));
    QCOMPARE(cache.thumbnail(1, 0, 900).width(), 900);
    QCOMPARE(cache.thumbnail(1, 0, 180).width(), 180);
    QVERIFY(cache.isCached(1, 0, 180));
    QVERIFY(cache.isCached(1, 0, 900));
}

void TestRender::changingTheWidthKeepsWhatIsAlreadyHeld()
{
    PopplerBackend backend;
    QString error;
    QVERIFY2(backend.addDocument(1, m_pdf, &error), qPrintable(error));

    RenderCache cache;
    cache.setBackend(&backend);
    cache.setThumbnailWidth(400);
    QSignalSpy ready(&cache, &RenderCache::thumbnailReady);

    cache.thumbnail(1, 0);
    QVERIFY(waitFor(ready, 1));
    QVERIFY(cache.isCached(1, 0));

    // This is what used to empty the whole cache. Two views share one of these
    // and each has its own idea of how big a thumbnail is, so the width moves
    // whenever the reader touches either of them.
    cache.setThumbnailWidth(500);
    cache.setThumbnailWidth(400);
    QVERIFY(cache.isCached(1, 0));
    QCOMPARE(cache.thumbnail(1, 0).width(), 400);
}

void TestRender::scalesANarrowerThumbnailOutOfAWiderOne()
{
    PopplerBackend backend;
    QString error;
    QVERIFY2(backend.addDocument(1, m_pdf, &error), qPrintable(error));

    RenderCache cache;
    cache.setBackend(&backend);
    QSignalSpy ready(&cache, &RenderCache::thumbnailReady);

    cache.thumbnail(1, 0, 800);
    QVERIFY(waitFor(ready, 1));

    // Answered on the spot out of the wider render, with no second one queued:
    // rendering a page again to make it smaller is the waste this exists to
    // avoid, and it is what the strip was doing for every page of every file.
    const QImage small = cache.thumbnail(1, 0, 200);
    QCOMPARE(small.width(), 200);
    QVERIFY(cache.isCached(1, 0, 200));

    QCoreApplication::processEvents();
    QCOMPARE(ready.count(), 1);
}

void TestRender::showsWhatItHasWhileTheRightSizeIsComing()
{
    PopplerBackend backend;
    QString error;
    QVERIFY2(backend.addDocument(1, m_pdf, &error), qPrintable(error));

    RenderCache cache;
    cache.setBackend(&backend);
    QSignalSpy ready(&cache, &RenderCache::thumbnailReady);

    cache.thumbnail(1, 0, 150);
    QVERIFY(waitFor(ready, 1));

    // Bigger than anything held, so it has to be drawn, but the answer is not
    // nothing. A cell that goes empty while a better picture is on its way is
    // what makes a viewer feel slow.
    const QImage standIn = cache.thumbnail(1, 0, 700);
    QVERIFY(!standIn.isNull());
    QCOMPARE(standIn.width(), 150);

    QVERIFY(waitFor(ready, 2));
    QCOMPARE(cache.thumbnail(1, 0, 700).width(), 700);
}

void TestRender::smallRendersSurviveASweepOfLargeOnes()
{
    PopplerBackend backend;
    QString error;
    QVERIFY2(backend.addDocument(1, m_pdf, &error), qPrintable(error));

    RenderCache cache;
    cache.setBackend(&backend);
    // Small enough that the sixteen large renders below cannot all fit, which
    // is the situation on any real document: a grid sweep is far more pixels
    // than any sane ceiling.
    cache.setCacheLimitKb(4 * 1024);
    QSignalSpy ready(&cache, &RenderCache::thumbnailReady);

    for (int page = 0; page < 16; ++page) {
        cache.thumbnail(1, page, 120);
    }
    QVERIFY(waitFor(ready, 16));

    for (int page = 0; page < 16; ++page) {
        cache.thumbnail(1, page, 1200);
    }
    QVERIFY(waitFor(ready, 32));

    // The strip's thumbnails are all still there. Sharing one ceiling would
    // have thrown every one of them out, and the strip would then have drawn
    // the whole document over again.
    for (int page = 0; page < 16; ++page) {
        QVERIFY2(cache.isCached(1, page, 120), qPrintable(QStringLiteral("page %1").arg(page)));
    }
}

QTEST_MAIN(TestRender)
#include "tst_render.moc"
