/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "TestPdf.h"

#include "core/Document.h"
#include "core/PageObjects.h"
#include "core/RenderBackend.h"
#include "ui/ObjectOverlay.h"
#include "ui/PageView.h"

#include <QApplication>
#include <QPainter>
#include <QTemporaryDir>
#include <QTest>

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
 * Everything here waits on work done off the GUI thread, so a case that blocks
 * cannot be rescued by a timer inside the event loop it is not running.
 */
class Watchdog
{
public:
    explicit Watchdog(QByteArray what, int seconds = 120)
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
 * A rasteriser that draws nothing and only ever says how big a page is.
 *
 * Deliberately blind: the whole point of the lift is that it does its own
 * rendering off the GUI thread, out of files it composes itself, so a case that
 * sees ink travel here has seen it come from there and from nowhere else.
 */
class SizingBackend : public RenderBackend
{
public:
    bool addDocument(int, const QString &, QString *) override { return true; }

    void removeDocument(int) override { }

    QImage renderPage(int, int, int) override { return {}; }

    QSizeF pageSizePoints(int, int) override { return QSizeF(612, 792); }

    QString extractText(int, int) override { return {}; }

    QStringList wordsInside(int, int, const QRectF &) override { return {}; }

    QVector<Word> words(int, int) override { return {}; }
};

/** Where the fixture puts its red rectangle, in points. */
QRectF redBoxAt()
{
    return QRectF(120.0, 380.0, 150.0, 100.0);
}

/** And its half-transparent one, well clear of everything else. */
QRectF paleBoxAt()
{
    return QRectF(320.0, 600.0, 150.0, 100.0);
}

const QColor RedInk(220, 40, 40);
const QColor BlueInk(30, 60, 210);

/** The ground the layer is painted over: a colour no page would ever draw. */
const QColor Ground(0, 200, 0);

bool nearlyWhite(const QColor &colour)
{
    return colour.red() > 235 && colour.green() > 235 && colour.blue() > 235;
}

/** Paper, allowing for the wash the layer lays over whatever is chosen. */
bool washedPaper(const QColor &colour)
{
    return colour.red() > 200 && colour.green() > 200 && colour.blue() > 200;
}

bool isGround(const QColor &colour)
{
    return colour == Ground;
}

} // namespace

/**
 * What a drag looks like while it is being made.
 *
 * tst_pastedobjects settles where an object *ends up*: it writes the changes out
 * and reads the file back. This settles the other half, which was wrong for
 * longer: what the reader sees between pressing the mouse and letting it go. The
 * boundary used to travel alone while the ink stayed printed where it was, so
 * every drag was a promise the page only kept after being saved.
 *
 * So every case here paints the layer over a ground no page draws, and asks
 * about colours. Green is ground the layer left alone, white is paper it put
 * back over a hole, and anything else is ink it carried.
 *
 * The rasteriser the view holds draws nothing at all, which is deliberate: ink
 * that turns up in these pictures came out of the lift's own renders, made off
 * the GUI thread from pages it composed itself, and could not have come from
 * anywhere else.
 */
class TestLiftedObjects : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void init();
    void cleanup();

    void aShapeTravelsWhileItIsDragged();
    void aLineOfTextTravelsWhileItIsDragged();
    void theInkKeepsWhatShowsThroughIt();
    void nothingIsPaintedWhileEverythingStandsWhereTheFileDrawsIt();
    void twoObjectsUnderDifferentChangesTravelToTheirOwnPlaces();
    void theInkIsMadeAgainForANewZoom();
    void aTurnSettlesIntoInkOfItsOwn();
    void takingAnObjectOffThePageTakesItsInkWithIt();
    void whatWasMovedStaysPutAfterItIsLetGoOf();

private:
    /** The object whose boundary holds @p where, or -1. */
    int objectAt(const QPointF &where) const;

    /** Chooses whatever is at @p where and waits for its pixels. */
    bool grab(ObjectOverlay &objects, const QPointF &where);

    /** Waits until the layer has the chosen objects in hand as ink. */
    static bool liftReady(ObjectOverlay &objects, int row);

    /** Turns whatever is chosen by @p degrees, by its own turn handle. */
    void turnBy(ObjectOverlay &objects, const QRectF &box, int degrees);

    /** The layer drawn over @ref Ground, and nothing else. */
    QImage painted(ObjectOverlay &objects, int row);

    QColor colourAt(const QImage &canvas, const QPointF &where);

    /**
     * How many pixels inside @p box are bare paper, which only a hole draws.
     *
     * Inside a box rather than over the whole picture, and inside it by a good
     * margin, because the handles round a chosen object are drawn in the
     * palette's base colour, which on most desktops is paper white too.
     */
    int paperPixels(const QImage &canvas, const QRectF &box);

    /** How many pixels inside @p box are darker than the paper under them. */
    int inkPixels(const QImage &canvas, const QRectF &box);

    QTemporaryDir m_dir;
    QString m_file;
    QVector<PageObject> m_objects;

    std::unique_ptr<Document> m_document;
    std::unique_ptr<PageView> m_view;
    SizingBackend m_backend;
    std::unique_ptr<Watchdog> m_watchdog;
};

// ── The fixture ───────────────────────────────────────────────────────────

void TestLiftedObjects::initTestCase()
{
    KLocalizedString::setApplicationDomain("pdf-smithy");
    QVERIFY(m_dir.isValid());

    // One page carrying a stamp of its own, a solid red rectangle, a line of
    // blue words and a rectangle that is half see-through. Written into the file
    // rather than pasted: everything here is about content the page itself draws
    // and the sheet underneath therefore already shows.
    const QString base = m_dir.filePath(QStringLiteral("base.pdf"));
    QVERIFY(test::writeSamplePdf(base, 1));

    QVector<NewContent> items;

    NewContent box;
    box.kind = NewContent::Kind::Rectangle;
    box.page = 0;
    box.rect = redBoxAt();
    box.fill = RedInk;
    box.stroke = QColor();
    items.append(box);

    NewContent words;
    words.kind = NewContent::Kind::Text;
    words.page = 0;
    words.rect = QRectF(120.0, 250.0, 300.0, 40.0);
    words.text = QStringLiteral("Getragen");
    words.fontSize = 34.0;
    words.fill = BlueInk;
    items.append(words);

    NewContent pale;
    pale.kind = NewContent::Kind::Rectangle;
    pale.page = 0;
    pale.rect = paleBoxAt();
    pale.fill = QColor(Qt::black);
    pale.stroke = QColor();
    pale.opacity = 0.5;
    items.append(pale);

    m_file = m_dir.filePath(QStringLiteral("page.pdf"));
    PageComposer::Report report;
    QString error;
    QVERIFY2(PageComposer::apply(base, m_file, {}, items, &report, &error), qPrintable(error));
    QCOMPARE(report.inserted, 3);

    m_objects = PageObjects::read(m_file, 0, &error);
    QVERIFY2(!m_objects.isEmpty(), qPrintable(error));
}

void TestLiftedObjects::init()
{
    m_watchdog = std::make_unique<Watchdog>(QByteArray(QTest::currentTestFunction()));

    m_document = std::make_unique<Document>();
    m_document->setRenderBackend(&m_backend);
    QString error;
    QVERIFY2(m_document->open(m_file, &error), qPrintable(error));

    m_view = std::make_unique<PageView>();
    m_view->setDocument(m_document.get());
    m_view->resize(900, 700);
    m_view->setMode(PageView::Mode::Edit);
    m_view->show();
    QCoreApplication::processEvents();

    QVERIFY2(!m_view->fromPoints(0, redBoxAt()).isEmpty(),
             "the view has no layout, so nothing measured in pixels means anything");
}

void TestLiftedObjects::cleanup()
{
    // The view keeps raw pointers to its overlays and the overlays keep one to
    // the document, so they go in that order or not at all.
    m_view.reset();
    m_document.reset();
    m_watchdog.reset();
}

// ── Asking the picture ────────────────────────────────────────────────────

int TestLiftedObjects::objectAt(const QPointF &where) const
{
    return PageObjects::hitTest(m_objects, where);
}

bool TestLiftedObjects::liftReady(ObjectOverlay &objects, int row)
{
    // Both halves: pixels in hand, and none better on the way. A lift that still
    // says something true stays up while it is being replaced, so waiting on the
    // first alone would go on with the answer to the question before this one.
    const auto settled = [&] { return objects.isLifted(row) && !objects.isLifting(); };
    for (int waited = 0; waited < 30000 && !settled(); waited += 20) {
        QTest::qWait(20);
    }
    return settled();
}

bool TestLiftedObjects::grab(ObjectOverlay &objects, const QPointF &where)
{
    if (!objects.press(0, where, Qt::LeftButton)) {
        return false;
    }
    objects.release(0, where);
    return objects.chosenObjects().size() == 1 && liftReady(objects, 0);
}

void TestLiftedObjects::turnBy(ObjectOverlay &objects, const QRectF &box, int degrees)
{
    // The handle is hunted for rather than worked out: it floats a fixed number
    // of pixels above the box, and a case that knew that number would go on
    // passing after the handle had moved. The pointer saying "turn" is what a
    // hand goes by.
    const QRectF pixels = m_view->fromPoints(0, box.normalized());
    QPointF handle;
    for (double above = 1.0; above <= 60.0; above += 1.0) {
        const QPointF where = m_view->toPoints(0, QPointF(pixels.center().x(), pixels.top() - above));
        if (objects.cursor(0, where) == Qt::CrossCursor) {
            handle = where;
            break;
        }
    }
    if (handle.isNull()) {
        return;
    }

    QTransform rotation;
    rotation.rotate(degrees);
    const QPointF middle = box.normalized().center();
    const QPointF to = middle + rotation.map(handle - middle);
    objects.press(0, handle, Qt::LeftButton);
    objects.move(0, (handle + to) / 2.0, Qt::LeftButton);
    objects.move(0, to, Qt::LeftButton);
    objects.release(0, to);
}

QImage TestLiftedObjects::painted(ObjectOverlay &objects, int row)
{
    QImage canvas(m_view->viewport()->size(), QImage::Format_ARGB32);
    canvas.fill(Ground);
    QPainter painter(&canvas);
    objects.paint(painter, row, m_view->pageRect(row));
    painter.end();
    return canvas;
}

QColor TestLiftedObjects::colourAt(const QImage &canvas, const QPointF &where)
{
    const QPoint pixel = m_view->fromPoints(0, where).toPoint();
    return canvas.rect().contains(pixel) ? canvas.pixelColor(pixel) : QColor();
}

int TestLiftedObjects::paperPixels(const QImage &canvas, const QRectF &box)
{
    const QRect pixels
        = m_view->fromPoints(0, box.normalized()).toRect().adjusted(12, 12, -12, -12).intersected(canvas.rect());
    int paper = 0;
    for (int y = pixels.top(); y <= pixels.bottom(); ++y) {
        for (int x = pixels.left(); x <= pixels.right(); ++x) {
            paper += nearlyWhite(canvas.pixelColor(x, y)) ? 1 : 0;
        }
    }
    return paper;
}

int TestLiftedObjects::inkPixels(const QImage &canvas, const QRectF &box)
{
    const QRect pixels = m_view->fromPoints(0, box.normalized()).toRect().intersected(canvas.rect());
    int dark = 0;
    for (int y = pixels.top(); y <= pixels.bottom(); ++y) {
        for (int x = pixels.left(); x <= pixels.right(); ++x) {
            const QColor colour = canvas.pixelColor(x, y);
            dark += colour.blue() > 120 && colour.red() < 120 && colour.green() < 120 ? 1 : 0;
        }
    }
    return dark;
}

// ── The cases ─────────────────────────────────────────────────────────────

void TestLiftedObjects::aShapeTravelsWhileItIsDragged()
{
    ObjectOverlay objects(m_view.get());
    objects.setDocument(m_document.get());
    objects.setSource(m_file);

    const QRectF box = redBoxAt();
    QVERIFY2(grab(objects, box.center()), "the chosen shape never came off the page as pixels");

    // Held halfway through the drag rather than after it. What was wrong was
    // never where an object ended up; it was that nothing moved until the file
    // had been written and drawn again.
    const QPointF shift(180.0, -140.0);
    objects.press(0, box.center(), Qt::LeftButton);
    objects.move(0, box.center() + shift / 2.0, Qt::LeftButton);
    objects.move(0, box.center() + shift, Qt::LeftButton);

    const QImage canvas = painted(objects, 0);
    const QColor arrived = colourAt(canvas, box.center() + shift);
    QVERIFY2(arrived.red() > 170 && arrived.green() < 90 && arrived.blue() < 90,
             qPrintable(QStringLiteral("the shape was dragged across the page and %1 is what is drawn where it went")
                            .arg(arrived.name())));

    const QColor left = colourAt(canvas, box.center());
    QVERIFY2(nearlyWhite(left),
             qPrintable(QStringLiteral("where the shape was dragged from shows %1 rather than the page under it")
                            .arg(left.name())));

    // And it stays where it was put once the mouse is let go: the file still
    // says nothing about any of this until the document is saved.
    objects.release(0, box.center() + shift);
    const QColor settled = colourAt(painted(objects, 0), box.center() + shift);
    QVERIFY2(settled.red() > 170 && settled.blue() < 90,
             qPrintable(QStringLiteral("after the drag the shape shows %1 where it was left").arg(settled.name())));
}

void TestLiftedObjects::aLineOfTextTravelsWhileItIsDragged()
{
    ObjectOverlay objects(m_view.get());
    objects.setDocument(m_document.get());
    objects.setSource(m_file);

    const int index = objectAt(QPointF(140.0, 262.0));
    QVERIFY2(index >= 0, "the fixture's line of words is not where the case expects it");
    QRectF box;
    for (const PageObject &object : std::as_const(m_objects)) {
        if (object.index == index) {
            box = object.bounds.normalized();
        }
    }
    QCOMPARE(objects.chosenObjects().size(), 0);
    QVERIFY2(grab(objects, box.center()), "the chosen words never came off the page as pixels");

    // Words are thin, so this counts ink over a box rather than asking one
    // pixel: whether a particular pixel of a particular glyph is covered is a
    // question about the font, and the question here is whether the line moved.
    const QPointF shift(0.0, 220.0);
    objects.press(0, box.center(), Qt::LeftButton);
    objects.move(0, box.center() + shift, Qt::LeftButton);
    const QImage canvas = painted(objects, 0);

    QVERIFY2(inkPixels(canvas, box.translated(shift)) > 40,
             "the line of words was dragged up the page and nothing was drawn where it went");
    QVERIFY2(inkPixels(canvas, box) == 0, "the line of words is still printed where it was dragged from");
    QVERIFY2(nearlyWhite(colourAt(canvas, box.center())),
             "the hole the words left shows something other than the page under them");
}

void TestLiftedObjects::theInkKeepsWhatShowsThroughIt()
{
    ObjectOverlay objects(m_view.get());
    objects.setDocument(m_document.get());
    objects.setSource(m_file);

    const QRectF box = paleBoxAt();
    QVERIFY2(grab(objects, box.center()), "the chosen shape never came off the page as pixels");

    // The rectangle is black at half opacity. Carried onto ground no page draws,
    // it has to go on being half see-through: an object lifted as though it were
    // solid would black the ground out, and one whose coverage was guessed at
    // from how far it stands out against paper would come out some other grey.
    //
    // That is the whole of why the ink is made from the same objects drawn twice,
    // over white and over black.
    const QPointF shift(-200.0, -300.0);
    objects.press(0, box.center(), Qt::LeftButton);
    objects.move(0, box.center() + shift, Qt::LeftButton);

    // The wash that says "chosen" is over the ink as well, which is why this
    // asks for a range rather than for half of the ground exactly. A shape
    // lifted as though it were solid comes out at about a quarter of the
    // ground's green; one whose coverage was guessed at from how far it stands
    // out against paper comes out darker still.
    const QColor arrived = colourAt(painted(objects, 0), box.center() + shift);
    QVERIFY2(arrived.green() > 60 && arrived.green() < 150 && arrived.red() < 40 && arrived.blue() < 60,
             qPrintable(QStringLiteral("a half see-through shape carried onto %1 came out %2 rather than half of it")
                            .arg(Ground.name(), arrived.name())));
}

void TestLiftedObjects::nothingIsPaintedWhileEverythingStandsWhereTheFileDrawsIt()
{
    ObjectOverlay objects(m_view.get());
    objects.setDocument(m_document.get());
    objects.setSource(m_file);

    const QRectF box = redBoxAt();
    QVERIFY2(grab(objects, box.center()), "the chosen shape never came off the page as pixels");

    // Paper is the one thing only a hole can draw: the ground is green, the
    // boundary is the palette's highlight and the ink is the object's own
    // colours. So a page with no hole in it has no paper on it either, and that
    // is what says the sheet underneath has been left to speak for itself.
    QCOMPARE(paperPixels(painted(objects, 0), box), 0);

    objects.nudge(0.0, 40.0);
    QVERIFY2(paperPixels(painted(objects, 0), box) > 0,
             "the shape was moved and nothing was put back where it had been");
}

void TestLiftedObjects::twoObjectsUnderDifferentChangesTravelToTheirOwnPlaces()
{
    ObjectOverlay objects(m_view.get());
    objects.setDocument(m_document.get());
    objects.setSource(m_file);

    // The rectangle is moved on its own first, so that when both are chosen the
    // two are under different waiting changes. One picture cannot be carried by
    // two matrices, so this is the case that says the lift is split by them.
    const QRectF box = redBoxAt();
    const QPointF apart(150.0, 0.0);
    QVERIFY(grab(objects, box.center()));
    objects.nudge(apart.x(), apart.y());

    const int words = objectAt(QPointF(140.0, 262.0));
    QVERIFY(words >= 0);
    QRectF said;
    for (const PageObject &object : std::as_const(m_objects)) {
        if (object.index == words) {
            said = object.bounds.normalized();
        }
    }

    // A band round the two of them, which is how a choice of several is made
    // without a keyboard: the interface hands the layer the buttons but not the
    // modifiers, so a Ctrl-click cannot be posted to it from here.
    objects.clearChoice();
    objects.press(0, QPointF(100.0, 200.0), Qt::LeftButton);
    objects.move(0, QPointF(300.0, 400.0), Qt::LeftButton);
    objects.move(0, QPointF(480.0, 520.0), Qt::LeftButton);
    objects.release(0, QPointF(480.0, 520.0));
    QCOMPARE(objects.chosenObjects().size(), 2);
    QVERIFY2(liftReady(objects, 0), "two objects under two changes never came off the page");

    const QPointF shift(140.0, 0.0);
    objects.press(0, said.center(), Qt::LeftButton);
    objects.move(0, said.center() + shift, Qt::LeftButton);
    const QImage canvas = painted(objects, 0);

    const QColor shape = colourAt(canvas, box.center() + apart + shift);
    QVERIFY2(shape.red() > 170 && shape.blue() < 90,
             qPrintable(QStringLiteral("the shape had been moved once already and %1 is what is drawn where the two "
                                       "changes together put it")
                            .arg(shape.name())));
    QVERIFY2(inkPixels(canvas, said.translated(shift)) > 40,
             "the words were dragged with the shape and nothing was drawn where they went");
}

void TestLiftedObjects::theInkIsMadeAgainForANewZoom()
{
    ObjectOverlay objects(m_view.get());
    objects.setDocument(m_document.get());
    objects.setSource(m_file);

    const QRectF box = redBoxAt();
    QVERIFY(grab(objects, box.center()));

    // A lift is pixels at one size. Changing the zoom leaves them standing,
    // stretched, so that nothing flickers, and asks for sharp ones; what this
    // settles is that the sharp ones land in the right place, which is where a
    // second copy of the points-to-pixels arithmetic would show up.
    m_view->setZoom(m_view->zoom() * 1.3);
    QCoreApplication::processEvents();
    QVERIFY2(liftReady(objects, 0), "the lift never came back after the zoom");
    QTest::qWait(400);

    const QPointF shift(120.0, -90.0);
    objects.press(0, box.center(), Qt::LeftButton);
    objects.move(0, box.center() + shift, Qt::LeftButton);
    const QImage canvas = painted(objects, 0);
    QVERIFY2(canvas.rect().contains(m_view->fromPoints(0, box.center() + shift).toPoint()),
             "the zoom carried the place this asks about off the screen");

    const QColor arrived = colourAt(canvas, box.center() + shift);
    QVERIFY2(
        arrived.red() > 170 && arrived.blue() < 90,
        qPrintable(QStringLiteral("at a larger zoom the shape shows %1 where it was dragged").arg(arrived.name())));
    QVERIFY2(nearlyWhite(colourAt(canvas, box.center())), "at a larger zoom the hole is not where the shape was");
}

void TestLiftedObjects::aTurnSettlesIntoInkOfItsOwn()
{
    ObjectOverlay objects(m_view.get());
    objects.setDocument(m_document.get());
    objects.setSource(m_file);

    const QRectF box = redBoxAt();
    QVERIFY(grab(objects, box.center()));

    // A quarter turn of a rectangle half again as wide as it is tall lands its
    // ink where the file never drew any, which is what makes this worth asking:
    // a bitmap put through a turn is soft, so once the gesture has settled the
    // pixels are made again with the turn written into the page they come from.
    turnBy(objects, box, 90);
    QVERIFY(objects.hasPendingEdits());
    QVERIFY2(liftReady(objects, 0), "the turned shape never came off the page as pixels");
    QTest::qWait(600);

    const QImage canvas = painted(objects, 0);
    const QPointF middle = box.center();
    for (const QPointF &inside : { middle - QPointF(0.0, 60.0), middle + QPointF(0.0, 60.0) }) {
        const QColor there = colourAt(canvas, inside);
        QVERIFY2(
            there.red() > 150 && there.blue() < 110,
            qPrintable(
                QStringLiteral("the turned shape shows %1 where its own long side now reaches").arg(there.name())));
    }
    for (const QPointF &outside : { middle - QPointF(63.0, 0.0), middle + QPointF(63.0, 0.0) }) {
        const QColor there = colourAt(canvas, outside);
        QVERIFY2(!(there.red() > 150 && there.blue() < 110),
                 qPrintable(QStringLiteral("the turned shape still shows %1 where only its old long side reached")
                                .arg(there.name())));
    }

    // And what is drawn is what would be written: the same matrix goes into the
    // waiting change and into the page the ink was made from.
    const QVector<PageEdit> waiting = objects.pendingEdits();
    QCOMPARE(waiting.size(), 1);
    const QRectF turned = waiting.constFirst().transform.mapRect(box);
    QVERIFY2(std::abs(turned.width() - box.height()) < 1.0 && std::abs(turned.height() - box.width()) < 1.0,
             "a quarter turn did not swap the boundary's sides");
}

void TestLiftedObjects::takingAnObjectOffThePageTakesItsInkWithIt()
{
    ObjectOverlay objects(m_view.get());
    objects.setDocument(m_document.get());
    objects.setSource(m_file);

    const QRectF box = redBoxAt();
    QVERIFY(grab(objects, box.center()));

    // A removal is only waiting, and until now the ink went on showing through
    // the crossed-out boundary until the document had been written. The hole is
    // already cut, so this costs nothing: the ink simply is not put back.
    objects.removeChosen();
    const QImage canvas = painted(objects, 0);

    // Off the middle: a boundary round something waiting to be removed is drawn
    // crossed through, and the two lines meet exactly at the centre.
    const QColor gone = colourAt(canvas, box.center() + QPointF(28.0, 11.0));
    QVERIFY2(washedPaper(gone),
             qPrintable(QStringLiteral("a shape marked for removal still shows %1 where it was").arg(gone.name())));

    objects.takeBack();
    QVERIFY2(liftReady(objects, 0), "the lift never came back after the removal was taken back");
    QVERIFY2(isGround(colourAt(painted(objects, 0), QPointF(560.0, 240.0))),
             "taking the removal back left paper over a part of the page nothing had touched");
    QCOMPARE(paperPixels(painted(objects, 0), box), 0);
}

void TestLiftedObjects::whatWasMovedStaysPutAfterItIsLetGoOf()
{
    ObjectOverlay objects(m_view.get());
    objects.setDocument(m_document.get());
    objects.setSource(m_file);

    const QRectF box = redBoxAt();
    QVERIFY(grab(objects, box.center()));

    const QPointF away(0.0, -200.0);
    objects.nudge(away.x(), away.y());
    objects.clearChoice();
    QCOMPARE(objects.chosenObjects().size(), 0);

    // The point of the case: a change that is still only waiting outlives the
    // choice that made it, and the sheet underneath goes on drawing the object
    // where the file has it until the document is saved. So the page as it now
    // stands has to be put right whether or not anybody is holding the thing.
    QVERIFY2(liftReady(objects, 0), "the page was never put right after the choice was let go of");

    const QImage canvas = painted(objects, 0);
    const QColor moved = colourAt(canvas, box.center() + away);
    QVERIFY2(moved.red() > 170 && moved.blue() < 90,
             qPrintable(QStringLiteral("the shape was moved and then let go of, and %1 is what is drawn where it was "
                                       "put")
                            .arg(moved.name())));

    const QColor left = colourAt(canvas, box.center());
    QVERIFY2(nearlyWhite(left),
             qPrintable(QStringLiteral("the shape was moved and then let go of, and %1 is still drawn where it was")
                            .arg(left.name())));
}

QTEST_MAIN(TestLiftedObjects)

#include "tst_liftedobjects.moc"
