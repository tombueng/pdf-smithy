/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "ui/PageModel.h"
#include "ui/ThumbnailDock.h"

#include <QListView>
#include <QMainWindow>
#include <QScrollBar>
#include <QStandardItemModel>
#include <QStandardPaths>
#include <QTest>
#include <QTimer>

#include <KLocalizedString>

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

using namespace ps;

namespace {

/**
 * Kills the process rather than let one case wedge the whole suite.
 *
 * The dock is a real widget in a real window, and a layout that will not settle
 * shows up as a case that never returns. A suite with one hanging case in it is
 * a suite nobody runs, so that case is turned into a crash that names itself.
 */
class Watchdog
{
public:
    explicit Watchdog(QByteArray what, int seconds = 60)
        : m_what(std::move(what))
        , m_thread([this, seconds] {
            std::unique_lock<std::mutex> lock(m_mutex);
            if (!m_wake.wait_for(lock, std::chrono::seconds(seconds), [this] { return m_finished; })) {
                qFatal("%s stopped responding and was still blocking after %d seconds", m_what.constData(), seconds);
            }
        })
    {
    }

    ~Watchdog()
    {
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            m_finished = true;
        }
        m_wake.notify_all();
        m_thread.join();
    }

    Watchdog(const Watchdog &) = delete;
    Watchdog &operator=(const Watchdog &) = delete;

private:
    QByteArray m_what;
    std::mutex m_mutex;
    std::condition_variable m_wake;
    bool m_finished = false;

    // Last, so that everything it reads is built before it starts.
    std::thread m_thread;
};

/**
 * A stand-in for the window's page model, carrying only what the strip reads.
 *
 * Deliberately not a PageModel over a real document: what is under test is how
 * the panel lays its cells out and where it travels to, and none of that has an
 * opinion about PDF. A fixture document would add a render backend, its lock and
 * its cache to every one of these cases and answer no question they ask.
 */
QStandardItemModel *pages(int count, QObject *parent)
{
    auto *model = new QStandardItemModel(count, 1, parent);
    for (int row = 0; row < count; ++row) {
        const QModelIndex index = model->index(row, 0);
        model->setData(index, 210.0 / 297.0, PageModel::AspectRatioRole);
        model->setData(index, row + 1, PageModel::PageNumberRole);
        model->setData(index, 0, PageModel::SourceIdRole);
        model->setData(index, row, PageModel::SourcePageRole);
    }
    return model;
}

} // namespace

/**
 * The page strip, and the two things about it that depend on where it is put.
 *
 * A strip docked down the side of the window is read by running an eye down it,
 * and one docked along the top by running an eye across it. Everything follows
 * from that: which way the cells flow, which scrollbar carries the travel, and
 * which way round the arithmetic that keeps the panel in step with the document
 * has to be done. None of it is read out of the dock's own bookkeeping: the
 * questions are asked of the list the user actually sees, because a member
 * variable saying "horizontal" over a view that still scrolls downwards is
 * exactly the bug these cases exist to catch.
 */
class TestThumbnailDock : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void init();
    void cleanup();

    void downTheSideItTravelsDownwards();
    void alongTheTopItTravelsSideways();
    void aWideStripPutsSeveralPagesAbreast();
    void turningTheStripKeepsTheMarkOnItsPage();
    void alongTheTopItStillFollowsTheReader();
    void afloatItGoesByItsOwnShape();
    void arrangingPagesTakesTheStripDownAndPutsItBack();

private:
    /** The list inside the panel, which is what the user is actually looking at. */
    QListView *strip() const;

    /** Puts the panel in @p area and lets the window lay itself out again. */
    void dockAt(Qt::DockWidgetArea area);

    /** Lets the layout run and the queued catching-up finish. */
    void settle();

    QMainWindow *m_window = nullptr;
    ThumbnailDock *m_dock = nullptr;
    QStandardItemModel *m_pages = nullptr;
    std::unique_ptr<Watchdog> m_watchdog;
};

void TestThumbnailDock::initTestCase()
{
    KLocalizedString::setApplicationDomain("pdf-smithy");
    // The panel remembers its thumbnail size in the user's own configuration,
    // and a test run must not be able to change what the user sees next time.
    QStandardPaths::setTestModeEnabled(true);
}

void TestThumbnailDock::init()
{
    m_watchdog = std::make_unique<Watchdog>(QByteArray(QTest::currentTestFunction()));

    m_window = new QMainWindow;
    m_window->setCentralWidget(new QWidget(m_window));
    m_dock = new ThumbnailDock(m_window);
    m_pages = pages(40, m_dock);
    m_dock->setModel(m_pages);
    m_window->addDockWidget(Qt::LeftDockWidgetArea, m_dock);

    // Named rather than left to whatever the last case wrote into the panel's
    // settings, because the panel remembers this number between one of its own
    // lifetimes and the next, and a case that inherited another case's zoom
    // would be asserting about a strip nobody had described.
    m_dock->setThumbnailWidth(140);

    // A known size, so that what fits across the strip and what does not is the
    // same answer here as it is in CI.
    m_window->resize(1100, 800);
    m_window->show();
    QVERIFY(QTest::qWaitForWindowExposed(m_window));
    settle();
}

void TestThumbnailDock::cleanup()
{
    delete m_window;
    m_window = nullptr;
    m_dock = nullptr;
    m_pages = nullptr;
    m_watchdog.reset();
}

QListView *TestThumbnailDock::strip() const
{
    return m_dock->findChild<QListView *>();
}

void TestThumbnailDock::dockAt(Qt::DockWidgetArea area)
{
    m_window->addDockWidget(area, m_dock);
    m_dock->show();
    settle();
}

void TestThumbnailDock::settle()
{
    QCoreApplication::processEvents();
    QTest::qWait(30);
    QCoreApplication::processEvents();
}

// ── Which way the strip runs ──────────────────────────────────────────────

void TestThumbnailDock::downTheSideItTravelsDownwards()
{
    dockAt(Qt::LeftDockWidgetArea);
    QVERIFY2(strip(), "the panel holds no list at all");

    QVERIFY2(strip()->verticalScrollBar()->maximum() > 0,
             "forty pages down the side of the window and the strip says there is nothing to scroll through");
    QVERIFY2(strip()->horizontalScrollBarPolicy() == Qt::ScrollBarAlwaysOff,
             "a strip down the side of the window offers a sideways scrollbar, which is travel in the direction it "
             "does not run");
    QCOMPARE(strip()->horizontalScrollBar()->maximum(), 0);
}

void TestThumbnailDock::alongTheTopItTravelsSideways()
{
    dockAt(Qt::TopDockWidgetArea);
    QVERIFY2(strip(), "the panel holds no list at all");

    QVERIFY2(strip()->horizontalScrollBar()->maximum() > 0,
             "docked along the top of the window, the strip still cannot be scrolled sideways, so the pages past the "
             "edge of the window cannot be reached at all");
    QVERIFY2(strip()->verticalScrollBarPolicy() == Qt::ScrollBarAlwaysOff,
             "a strip along the top of the window still offers a downwards scrollbar, so it scrolls the way it is not "
             "laid out");
    QCOMPARE(strip()->verticalScrollBar()->maximum(), 0);
}

void TestThumbnailDock::aWideStripPutsSeveralPagesAbreast()
{
    dockAt(Qt::LeftDockWidgetArea);

    // Room for seven cells across at the smallest thumbnail size, which is the
    // shape a user makes when they want an overview rather than a column.
    m_window->resizeDocks({ m_dock }, { 600 }, Qt::Horizontal);
    settle();
    m_dock->setThumbnailWidth(ThumbnailDock::MinimumThumbnailWidth);
    settle();

    QVERIFY2(m_dock->width() >= 500,
             qPrintable(QStringLiteral("shrinking the thumbnails dragged the panel back to %1 pixels, so the room the "
                                       "user made for more pages was taken away again")
                            .arg(m_dock->width())));

    const QRect first = strip()->visualRect(m_pages->index(0, 0));
    const QRect second = strip()->visualRect(m_pages->index(1, 0));
    QVERIFY2(!first.isEmpty() && !second.isEmpty(), "the first two pages have no place in the strip at all");
    QVERIFY2(second.top() == first.top() && second.left() > first.left(),
             qPrintable(QStringLiteral("a panel %1 pixels wide showing %2-pixel thumbnails still puts one page per "
                                       "line: page 2 sits at (%3, %4) and page 1 at (%5, %6)")
                            .arg(m_dock->width())
                            .arg(m_dock->thumbnailWidth())
                            .arg(second.left())
                            .arg(second.top())
                            .arg(first.left())
                            .arg(first.top())));
}

// ── Staying in step, whichever way it runs ────────────────────────────────

void TestThumbnailDock::turningTheStripKeepsTheMarkOnItsPage()
{
    dockAt(Qt::LeftDockWidgetArea);

    m_dock->followPage(30);
    settle();
    QCOMPARE(strip()->currentIndex().row(), 30);

    dockAt(Qt::TopDockWidgetArea);

    QVERIFY2(strip()->currentIndex().row() == 30,
             qPrintable(QStringLiteral("moving the panel to the top of the window moved the mark from page 31 to "
                                       "page %1")
                            .arg(strip()->currentIndex().row() + 1)));

    const QRect cell = strip()->visualRect(m_pages->index(30, 0));
    QVERIFY2(!cell.isEmpty() && strip()->viewport()->rect().intersects(cell),
             "after the panel was moved to the top of the window the page being read is nowhere on the strip, so the "
             "mark is drawn somewhere nobody can see it");
}

void TestThumbnailDock::alongTheTopItStillFollowsTheReader()
{
    dockAt(Qt::TopDockWidgetArea);

    const QScrollBar *across = strip()->horizontalScrollBar();
    QVERIFY2(across->maximum() > 0, "the strip has no sideways travel to follow the reader along");

    // Both signals, because that is how the window drives the panel: which page
    // is being read and how far through the document the reader stands arrive
    // together. A position on its own would prove nothing here, since the strip
    // refuses to travel away from the page it has marked, and the mark starts on
    // the first one.
    m_dock->followPage(0);
    m_dock->followPosition(0.0);
    settle();
    QCOMPARE(across->value(), 0);

    m_dock->followPage(20);
    m_dock->followPosition(0.5);
    settle();

    const int middle = across->value();
    QVERIFY2(middle > 0,
             "the reader is halfway through the document and a strip laid out across the window has not moved at all");
    QVERIFY2(strip()->verticalScrollBar()->value() == 0,
             "a strip laid out across the window travelled downwards to follow the reader");

    // On through the same page, without turning it. This is the whole point of
    // tracking a position rather than a page number, and it is the piece that
    // had to be turned on its side along with the layout: a reader going slowly
    // down a tall page turns nothing for many seconds, and a strip driven by the
    // page number alone would stand perfectly still throughout.
    m_dock->followPosition(0.62);
    settle();
    QVERIFY2(across->value() > middle,
             qPrintable(QStringLiteral("the reader scrolled on without turning a page and the strip stayed at %1 of %2")
                            .arg(across->value())
                            .arg(across->maximum())));
}

void TestThumbnailDock::afloatItGoesByItsOwnShape()
{
    // A floating panel is in no dock area at all, so there is nothing to ask
    // where it is; its own shape is the only thing left that says how it is read.
    m_dock->setFloating(true);
    settle();

    m_dock->resize(900, 320);
    settle();
    QVERIFY2(strip()->verticalScrollBarPolicy() == Qt::ScrollBarAlwaysOff,
             "a panel floated into a wide, shallow window is still read downwards, which is the one direction it has "
             "no room in");

    m_dock->resize(320, 760);
    settle();
    QVERIFY2(strip()->horizontalScrollBarPolicy() == Qt::ScrollBarAlwaysOff,
             "a panel floated into a tall, narrow window is still read sideways");
}

// ── The mode that already shows every page ────────────────────────────────

void TestThumbnailDock::arrangingPagesTakesTheStripDownAndPutsItBack()
{
    dockAt(Qt::LeftDockWidgetArea);
    QVERIFY(m_dock->isVisible());

    m_dock->setTemporarilyHidden(true);
    settle();
    QVERIFY2(!m_dock->isVisible(),
             "the page grid shows every page in the document and the strip beside it shows them a second time");

    m_dock->setTemporarilyHidden(false);
    settle();
    QVERIFY2(m_dock->isVisible(), "leaving the arranging mode did not put the strip back");

    // And a strip the user closed themselves stays closed, which is the whole
    // difference between putting a panel back and throwing one open.
    m_dock->close();
    settle();
    QVERIFY(!m_dock->isVisible());

    m_dock->setTemporarilyHidden(true);
    settle();
    m_dock->setTemporarilyHidden(false);
    settle();
    QVERIFY2(!m_dock->isVisible(),
             "leaving the arranging mode threw open a panel the user had closed, so the panel comes back every time "
             "they visit that mode");
}

QTEST_MAIN(TestThumbnailDock)

#include "tst_thumbnaildock.moc"
