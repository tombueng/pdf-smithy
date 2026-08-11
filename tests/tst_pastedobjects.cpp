/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "TestPdf.h"

#include "core/Document.h"
#include "core/PageObjects.h"
#include "core/PageRef.h"
#include "core/PdfGeometry.h"
#include "core/RenderBackend.h"
#include "core/Source.h"
#include "ui/Alignment.h"
#include "ui/Inspector.h"
#include "ui/ObjectOverlay.h"
#include "ui/PageClipboard.h"
#include "ui/PageProcessor.h"
#include "ui/PageRows.h"
#include "ui/PageView.h"

#include <QApplication>
#include <QClipboard>
#include <QMimeData>
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
 * The same guard the other end-to-end tests carry, and for the same reason:
 * everything here builds a real view over real files, and a case that blocks
 * outside the event loop cannot be rescued by a timer.
 */
class Watchdog
{
public:
    explicit Watchdog(QByteArray what, int seconds = 90)
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
 * ps::Document answers no page size without a backend and the view needs one for
 * every conversion between points and pixels. Nothing here looks at ink on a
 * rendered sheet, so there is no reason to produce any.
 */
class SizingBackend : public RenderBackend
{
public:
    bool addDocument(int sourceId, const QString &path, QString *error) override
    {
        Q_UNUSED(sourceId)
        Q_UNUSED(path)
        Q_UNUSED(error)
        return true;
    }

    void removeDocument(int) override { }

    QImage renderPage(int, int, int) override { return {}; }

    QSizeF pageSizePoints(int, int) override { return QSizeF(612, 792); }

    QString extractText(int, int) override { return {}; }

    QStringList wordsInside(int, int, const QRectF &) override { return {}; }

    QVector<Word> words(int, int) override { return {}; }
};

/** Where writeSamplePdf() stamps "PSPAGE n", well clear of anything pasted. */
QPointF onTheStamp()
{
    return QPointF(100.0, 706.0);
}

/** Where every case here puts its picture down, before the paste steps it on. */
QRectF pastedAt()
{
    return QRectF(200.0, 200.0, 120.0, 90.0);
}

QImage swatch(const QColor &colour = QColor(200, 60, 60), int across = 40, int down = 30)
{
    QImage image(across, down, QImage::Format_RGB32);
    image.fill(colour);
    return image;
}

/**
 * A picture with a red left half and a blue right half.
 *
 * For the cases that ask which way round something is: a picture of one colour
 * looks the same after every turn, so it can only ever say where a thing is and
 * never how it went round.
 */
QImage twoTone(int across = 40, int down = 30)
{
    QImage image(across, down, QImage::Format_RGB32);
    image.fill(QColor(200, 60, 60));
    QPainter painter(&image);
    painter.fillRect(QRect(across / 2, 0, across - across / 2, down), QColor(50, 70, 200));
    return image;
}

/** One picture, packed exactly as a copy taken off a page packs it. */
QMimeData *pictureParcel(const QRectF &where, const QImage &picture = swatch())
{
    PageClipboard::Parcel parcel;
    NewContent item;
    item.kind = NewContent::Kind::Image;
    item.rect = where;
    item.image = picture;
    parcel.items.append(item);
    return PageClipboard::pack(parcel);
}

/** One line of text, the other thing a page copy carries. */
QMimeData *textParcel(const QRectF &where)
{
    PageClipboard::Parcel parcel;
    NewContent item;
    item.kind = NewContent::Kind::Text;
    item.rect = where;
    item.text = QStringLiteral("Pasted");
    item.fontSize = 12.0;
    item.fill = QColor(Qt::black);
    parcel.items.append(item);
    return PageClipboard::pack(parcel);
}

bool nearly(double one, double other, double slack = 0.6)
{
    return qAbs(one - other) <= slack;
}

/** Two boxes are the same box when no edge of them is more than @p slack out. */
bool sameBox(const QRectF &one, const QRectF &other, double slack = 0.6)
{
    return nearly(one.left(), other.left(), slack) && nearly(one.top(), other.top(), slack)
        && nearly(one.width(), other.width(), slack) && nearly(one.height(), other.height(), slack);
}

QString describe(const QRectF &box)
{
    return QStringLiteral("%1,%2 %3x%4")
        .arg(box.left(), 0, 'f', 1)
        .arg(box.top(), 0, 'f', 1)
        .arg(box.width(), 0, 'f', 1)
        .arg(box.height(), 0, 'f', 1);
}

/** The size writeSamplePdf() gives every sheet, before the organiser turns any of them. */
QSizeF sheet()
{
    return QSizeF(612.0, 792.0);
}

/**
 * The four corners of @p box, in the order the composer maps the unit square on.
 *
 * A box is not enough to tell a turned picture from an upright one: turn a
 * picture a quarter and the box round it is the same size in the same place, so
 * everything here that asks "which way round is it" asks about the corners.
 */
QPolygonF cornersOf(const QRectF &box)
{
    const QRectF area = box.normalized();
    return QPolygonF() << area.topLeft() << area.topRight() << area.bottomRight() << area.bottomLeft();
}

/** Where the four corners of something waiting to be written now are, in points. */
QPolygonF quadOf(const NewContent &item)
{
    return item.placement.map(cornersOf(item.rect));
}

/**
 * The same for a picture the file holds, measured as the screen measures it.
 *
 * A picture is drawn in the unit square, so the matrix in force when it was
 * drawn says where all four of its corners went. That matrix is in the page's
 * own space; the screen was looking at the sheet as @p sheetTurn had turned it,
 * and the second half of this puts the turn back on.
 */
QPolygonF quadOf(const PageObject &picture, int sheetTurn)
{
    const QTransform toDisplay
        = PdfGeometry::displayToPageTransform(sheetTurn, sheet().width(), sheet().height()).inverted();
    return (picture.placement * toDisplay).map(cornersOf(QRectF(0.0, 0.0, 1.0, 1.0)));
}

/** @p box turned @p degrees about its own middle, corner by corner. */
QPolygonF turnedAbout(const QRectF &box, int degrees)
{
    const QPointF middle = box.normalized().center();
    QTransform rotation;
    rotation.rotate(degrees);
    return (QTransform::fromTranslate(-middle.x(), -middle.y()) * rotation
            * QTransform::fromTranslate(middle.x(), middle.y()))
        .map(cornersOf(box));
}

bool sameQuad(const QPolygonF &one, const QPolygonF &other, double slack = 1.0)
{
    if (one.size() != other.size()) {
        return false;
    }
    for (int corner = 0; corner < one.size(); ++corner) {
        if (!nearly(one.at(corner).x(), other.at(corner).x(), slack)
            || !nearly(one.at(corner).y(), other.at(corner).y(), slack)) {
            return false;
        }
    }
    return true;
}

QString describe(const QPolygonF &quad)
{
    QStringList corners;
    for (const QPointF &corner : quad) {
        corners << QStringLiteral("%1,%2").arg(corner.x(), 0, 'f', 1).arg(corner.y(), 0, 'f', 1);
    }
    return corners.join(u' ');
}

/**
 * The page point that lands on the bottom right handle of @p box, in pixels.
 *
 * Handles are placed in the viewport's own coordinates, where y runs down the
 * screen; the page's y runs up. So the handle a hand would reach for at the
 * bottom right of the box on screen is at the box's larger x and *smaller* y in
 * points, which is what QRectF calls its top right corner.
 */
QPointF bottomRightHandle(const QRectF &box)
{
    return box.normalized().topRight();
}

/**
 * The same for the handle in the middle of the box's lower edge on screen.
 *
 * The one to reach for when a box is to be made a different shape rather than a
 * different size: a corner holds the proportions and an edge does not.
 */
QPointF bottomEdgeHandle(const QRectF &box)
{
    const QRectF area = box.normalized();
    return QPointF(area.center().x(), area.top());
}

/** The four turns, because a paste has to survive every one of them. */
void everyTurn()
{
    QTest::addColumn<int>("degrees");
    // Both quarters, because 180 hides a swapped sign and a quarter turn hides a
    // swapped axis, and neither of them alone catches both.
    QTest::newRow("upright") << 0;
    QTest::newRow("a quarter clockwise") << 90;
    QTest::newRow("upside down") << 180;
    QTest::newRow("three quarters clockwise") << 270;
}

} // namespace

/**
 * A picture pasted onto a page is an object like any other.
 *
 * The complaint these were written against: a picture pasted in Edit mode lands
 * on the page and is then stuck: no handles, no drag, no resize, no properties,
 * nothing until the document has been saved and read back. Every case here asks
 * the same question in a different way: does what has just been pasted answer to
 * the gestures that everything already on the page answers to?
 */
class TestPastedObjects : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void init();
    void cleanup();

    // ── the complaint ─────────────────────────────────────────────────────
    void aPastedPictureCanBeChosenByClickingIt();
    void aPastedPictureIsDraggedByDraggingIt();
    void aPastedPictureIsScaledByItsHandles();
    void aCornerHandleKeepsAPastedPictureInProportion();
    void shiftFreesTheCornerToChangeTheShape();
    void aPastedPictureOnABarePageCanStillBeTakenHoldOf();
    void oneGestureMeansTheSameToBothKinds();

    // ── one kind of thing rather than two ─────────────────────────────────
    void aBandRoundBothTakesBoth();
    void aPastedPictureLinesUpWithWhatWasAlreadyThere();
    void thePanelDescribesAPastedPicture();
    void aPastedPictureIsCopiedAndDeletedOnTheSameKeys();
    void takingBackARemovalBringsThePasteBack();
    void twoPastesAreTwoObjectsThatMoveApart();

    // ── the turn handle ───────────────────────────────────────────────────
    void aPastedPictureIsTurnedByItsHandle();
    void oneTurnMeansTheSameToBothKinds();
    void turningAPastedLineOfTextLeavesItsTypeSizeAlone();
    void aTurnedPastedPictureIsWrittenTurned_data();
    void aTurnedPastedPictureIsWrittenTurned();
    void aPictureTheFileHoldsIsTurnedByTheSameHandle_data();
    void aPictureTheFileHoldsIsTurnedByTheSameHandle();

    // ── the picture travels, not only the frame round it ──────────────────
    void aPictureTheFileHoldsTravelsWhileItIsDragged();
    void aPictureTheFileHoldsIsSeenToTurn();
    void theBoundaryGoesRoundWithTheObjectRatherThanGrowing();

    // ── what is written is what was seen ──────────────────────────────────
    void aPastedPictureLandsWhereItWasDragged();
    void aPastedPictureOnATurnedPageLandsWhereItWasDragged_data();
    void aPastedPictureOnATurnedPageLandsWhereItWasDragged();
    void turningTheSheetCarriesThePasteRoundWithIt_data();
    void turningTheSheetCarriesThePasteRoundWithIt();
    void aPastedLineOfTextGrowsWithItsBox();

private:
    void openDocument(int rotation = 0);

    /** Paste @p data onto @p row and hand back where it landed, in points. */
    static QRectF pasteOnto(ObjectOverlay &objects, int row, QMimeData *data);

    /** The one thing waiting to be written, in the shape the engine takes it. */
    static NewContent onlyInsertion(const ObjectOverlay &objects);

    /**
     * The box round where that one thing now stands.
     *
     * Its own rectangle put through its placement, which is what "where it
     * stands" means once a thing can be turned: the rectangle says how big the
     * content is and the matrix says which way round it went.
     */
    static QRectF onlyInsertionBox(const ObjectOverlay &objects);

    /** Press, move twice and release, exactly as a hand on a mouse would. */
    static void dragOn(ObjectOverlay &objects, int row, const QPointF &from, const QPointF &to);

    /** Where the turn handle of @p box floats, found the way a hand finds it. */
    QPointF turnHandleOf(ObjectOverlay &objects, int row, const QRectF &box);

    /** Takes that handle and carries it @p degrees round the middle of @p box. */
    void turnBy(ObjectOverlay &objects, int row, const QRectF &box, int degrees);

    /**
     * Runs everything waiting through the engine and reads the page back.
     *
     * The answer is where the objects have actually landed, measured on the page
     * as it is now drawn. A picture written a quarter turn out is not visible in
     * anything the overlay says about itself, which is the whole point of going
     * all the way to the file and back.
     */
    QVector<PageObject> writeAndReadBack(ObjectOverlay &objects, PageProcessor &processor, int row);

    /** The one picture on a page that has been written out. */
    static PageObject onlyPicture(const QVector<PageObject> &objects);

    /**
     * What the layer draws over @p row, on a ground no page would draw.
     *
     * Green underneath, so that three answers can be told apart in one picture:
     * green is paper the layer left alone, white is paper it covered over, and
     * anything else is something it drew.
     */
    QImage painted(ObjectOverlay &objects, int row);

    /** The colour @p painted has at a place on the page, in points. */
    QColor colourAt(const QImage &canvas, int row, const QPointF &where);

    /**
     * Waits until the layer has the chosen objects in hand as ink.
     *
     * The preparation is a render, and it happens off the GUI thread precisely
     * so that nothing is composed or rasterised while a gesture is being made.
     * So a case about what a drag looks like has to let it arrive first, exactly
     * as the hand does: the click chooses, and the pixels follow.
     */
    static bool liftReady(ObjectOverlay &objects, int row);

    static void settle();

    QTemporaryDir m_dir;

    /** Three pages, each stamped with its own number. */
    QString m_file;

    std::unique_ptr<Document> m_document;
    std::unique_ptr<PageView> m_view;
    SizingBackend m_backend;
    std::unique_ptr<Watchdog> m_watchdog;
};

// ── The fixtures ──────────────────────────────────────────────────────────

void TestPastedObjects::initTestCase()
{
    KLocalizedString::setApplicationDomain("pdf-smithy");
    QVERIFY(m_dir.isValid());

    m_file = m_dir.filePath(QStringLiteral("pages.pdf"));
    QVERIFY(test::writeSamplePdf(m_file, 3));
}

void TestPastedObjects::init()
{
    m_watchdog = std::make_unique<Watchdog>(QByteArray(QTest::currentTestFunction()));

    m_document = std::make_unique<Document>();
    m_document->setRenderBackend(&m_backend);

    m_view = std::make_unique<PageView>();
    m_view->setDocument(m_document.get());
    m_view->resize(900, 700);
}

void TestPastedObjects::cleanup()
{
    // The view keeps raw pointers to its overlays and the overlays keep one to
    // the document, so they go in that order or not at all.
    m_view.reset();
    m_document.reset();
    m_watchdog.reset();
}

void TestPastedObjects::openDocument(int rotation)
{
    QString error;
    QVERIFY2(m_document->open(m_file, &error), qPrintable(error));
    if (rotation != 0) {
        m_document->applyRotation({ 0 }, rotation);
    }
    m_view->setMode(PageView::Mode::Edit);
    m_view->show();
    settle();

    // Handles are placed in pixels, so a view with no layout of its own would
    // put every one of them at the same spot and make every press a resize.
    QVERIFY2(!m_view->fromPoints(0, pastedAt()).isEmpty(),
             "the view has no layout, so nothing measured in pixels means anything");
}

void TestPastedObjects::settle()
{
    QCoreApplication::sendPostedEvents();
    QCoreApplication::processEvents();
}

QRectF TestPastedObjects::pasteOnto(ObjectOverlay &objects, int row, QMimeData *data)
{
    const std::unique_ptr<QMimeData> holder(data);
    if (!objects.paste(holder.get(), row)) {
        return {};
    }
    const QVector<NewContent> waiting = objects.pendingInsertions();
    return waiting.isEmpty() ? QRectF() : waiting.constLast().rect.normalized();
}

NewContent TestPastedObjects::onlyInsertion(const ObjectOverlay &objects)
{
    const QVector<NewContent> waiting = objects.pendingInsertions();
    return waiting.size() == 1 ? waiting.constFirst() : NewContent();
}

QRectF TestPastedObjects::onlyInsertionBox(const ObjectOverlay &objects)
{
    const NewContent item = onlyInsertion(objects);
    return item.placement.mapRect(item.rect.normalized()).normalized();
}

QPointF TestPastedObjects::turnHandleOf(ObjectOverlay &objects, int row, const QRectF &box)
{
    // Hunted for rather than worked out: the handle floats a fixed number of
    // pixels above the box, and a test that knew that number would go on passing
    // if the handle moved. The pointer saying "turn" is what a hand goes by.
    const QRectF pixels = m_view->fromPoints(row, box.normalized());
    for (double above = 1.0; above <= 60.0; above += 1.0) {
        const QPointF where = m_view->toPoints(row, QPointF(pixels.center().x(), pixels.top() - above));
        if (objects.cursor(row, where) == Qt::CrossCursor) {
            return where;
        }
    }
    return {};
}

void TestPastedObjects::turnBy(ObjectOverlay &objects, int row, const QRectF &box, int degrees)
{
    const QPointF handle = turnHandleOf(objects, row, box);
    QVERIFY2(!handle.isNull(), "there is no turn handle above the chosen box at all");

    // Carried round the middle of the box, which is what the handle turns about.
    const QPointF middle = box.normalized().center();
    QTransform rotation;
    rotation.rotate(degrees);
    dragOn(objects, row, handle, middle + rotation.map(handle - middle));
}

void TestPastedObjects::dragOn(ObjectOverlay &objects, int row, const QPointF &from, const QPointF &to)
{
    objects.press(row, from, Qt::LeftButton);
    // Two moves rather than one: a layer that only starts a gesture on the
    // second move would otherwise never be asked.
    objects.move(row, (from + to) / 2.0, Qt::LeftButton);
    objects.move(row, to, Qt::LeftButton);
    objects.release(row, to);
}

QVector<PageObject> TestPastedObjects::writeAndReadBack(ObjectOverlay &objects, PageProcessor &processor, int row)
{
    // Nothing waiting is not a failure to write: it is a page that already says
    // what it is going to say, and reading it back is still the honest answer.
    if (objects.hasPendingEdits() || objects.hasPendingInsertions()) {
        PageComposer::Report report;
        QString error;
        if (!processor.composeObjects(objects.pendingEdits(), objects.pendingInsertions(), &report, &error)) {
            qWarning("the engine refused the work: %s", qPrintable(error));
            return {};
        }
        objects.takeBackAll();
        settle();
    }

    // The engine wrote a whole new file and the document now shows its pages, so
    // that file is where the content has actually ended up. Its pages carry the
    // turn in their own /Rotate, which is why what comes back is measured in the
    // same space the user was looking at.
    const PageRef ref = m_document->pageAt(row);
    const Source *source = m_document->source(ref.sourceId);
    if (!source) {
        return {};
    }
    QString why;
    const QVector<PageObject> found = PageObjects::read(source->path(), ref.sourcePage, &why);
    if (!why.isEmpty()) {
        qWarning("the written page could not be read back: %s", qPrintable(why));
    }
    return found;
}

QImage TestPastedObjects::painted(ObjectOverlay &objects, int row)
{
    QImage canvas(m_view->viewport()->size(), QImage::Format_ARGB32);
    canvas.fill(QColor(0, 200, 0));
    QPainter painter(&canvas);
    objects.paint(painter, row, m_view->pageRect(row));
    painter.end();
    return canvas;
}

bool TestPastedObjects::liftReady(ObjectOverlay &objects, int row)
{
    for (int waited = 0; waited < 20000 && !objects.isLifted(row); waited += 20) {
        QTest::qWait(20);
    }
    return objects.isLifted(row);
}

QColor TestPastedObjects::colourAt(const QImage &canvas, int row, const QPointF &where)
{
    const QPoint pixel = m_view->fromPoints(row, where).toPoint();
    if (!canvas.rect().contains(pixel)) {
        return {};
    }
    return canvas.pixelColor(pixel);
}

PageObject TestPastedObjects::onlyPicture(const QVector<PageObject> &objects)
{
    for (const PageObject &object : objects) {
        if (object.kind == PageObject::Kind::Image) {
            return object;
        }
    }
    return {};
}

// ── A. The complaint ──────────────────────────────────────────────────────

void TestPastedObjects::aPastedPictureCanBeChosenByClickingIt()
{
    openDocument();

    ObjectOverlay objects(m_view.get());
    objects.setDocument(m_document.get());
    objects.setSource(m_document->filePath());

    const QRectF landed = pasteOnto(objects, 0, pictureParcel(pastedAt()));
    QVERIFY2(!landed.isNull(), "the picture was not pasted at all");

    // Chosen the moment it lands: what has just been put on the page is what the
    // hand is reaching for, and having to hunt for it is half the complaint.
    QVERIFY2(!objects.chosenObjects().isEmpty(),
             "nothing is chosen straight after a paste, so the picture arrives without handles on it");
    QCOMPARE(objects.chosenPage(), 0);

    // And chosen again by clicking it, like anything else on the page.
    objects.clearChoice();
    QVERIFY(objects.chosenObjects().isEmpty());

    QVERIFY2(objects.press(0, landed.center(), Qt::LeftButton),
             "the press on the pasted picture was not taken by the object layer at all");
    QVERIFY2(!objects.chosenObjects().isEmpty(),
             "clicking the picture that was just pasted chose nothing: it is on the page and cannot be taken hold of");
    QCOMPARE(objects.chosenPage(), 0);
    objects.release(0, landed.center());

    // The pointer says so too, which is how a user learns it can be moved.
    QCOMPARE(objects.cursor(0, landed.center()), Qt::SizeAllCursor);
    QCOMPARE(objects.cursor(0, bottomRightHandle(landed)), Qt::SizeFDiagCursor);
}

void TestPastedObjects::aPastedPictureIsDraggedByDraggingIt()
{
    openDocument();

    ObjectOverlay objects(m_view.get());
    objects.setDocument(m_document.get());
    objects.setSource(m_document->filePath());

    const QRectF landed = pasteOnto(objects, 0, pictureParcel(pastedAt()));
    QVERIFY(!landed.isNull());

    const QPointF shift(80.0, -60.0);
    dragOn(objects, 0, landed.center(), landed.center() + shift);

    const QRectF now = onlyInsertionBox(objects);
    QVERIFY2(sameBox(now, landed.translated(shift)),
             qPrintable(QStringLiteral("dragging the pasted picture left it at %1; it was dragged to %2")
                            .arg(describe(now), describe(landed.translated(shift)))));
}

void TestPastedObjects::aPastedPictureIsScaledByItsHandles()
{
    openDocument();

    ObjectOverlay objects(m_view.get());
    objects.setDocument(m_document.get());
    objects.setSource(m_document->filePath());

    const QRectF landed = pasteOnto(objects, 0, pictureParcel(pastedAt()));
    QVERIFY(!landed.isNull());

    // The bottom right handle on screen, pulled further out and further down.
    // Along the box's own diagonal, so that what comes back says the same thing
    // whether or not the corner is holding the proportions.
    const QPointF grip = bottomRightHandle(landed);
    dragOn(objects, 0, grip, grip + QPointF(60.0, -45.0));

    const QRectF now = onlyInsertionBox(objects);
    const QRectF wanted(landed.left(), landed.top() - 45.0, landed.width() + 60.0, landed.height() + 45.0);
    QVERIFY2(sameBox(now, wanted),
             qPrintable(QStringLiteral("pulling the corner handle made the pasted picture %1; it was pulled to %2")
                            .arg(describe(now), describe(wanted))));
}

void TestPastedObjects::aCornerHandleKeepsAPastedPictureInProportion()
{
    openDocument();

    ObjectOverlay objects(m_view.get());
    objects.setDocument(m_document.get());
    objects.setSource(m_document->filePath());

    const QRectF landed = pasteOnto(objects, 0, pictureParcel(pastedAt()));
    QVERIFY(!landed.isNull());

    // Pulled far more across than down, so a corner that did not hold the
    // proportions would come out obviously oblong. No modifier: this is what a
    // corner does when nothing is being asked of it, which is what a hand that
    // has used any other drawing program expects of it.
    const QPointF grip = bottomRightHandle(landed);
    dragOn(objects, 0, grip, grip + QPointF(120.0, -20.0));

    const QRectF now = onlyInsertionBox(objects);
    const double across = now.width() / landed.width();
    const double down = now.height() / landed.height();
    QVERIFY2(nearly(across, down, 0.01),
             qPrintable(QStringLiteral("the corner left the pasted picture %1 wider and %2 taller")
                            .arg(across, 0, 'f', 3)
                            .arg(down, 0, 'f', 3)));
    QVERIFY2(across > 1.05, "the corner drag did nothing at all");
}

void TestPastedObjects::shiftFreesTheCornerToChangeTheShape()
{
    openDocument();

    ObjectOverlay objects(m_view.get());
    objects.setDocument(m_document.get());
    objects.setSource(m_document->filePath());

    const QRectF landed = pasteOnto(objects, 0, pictureParcel(pastedAt()));
    QVERIFY(!landed.isNull());

    // Through the window system, because that is what sets the modifier state
    // the gesture reads; a key event sent straight to the widget would not.
    QVERIFY(m_view->windowHandle());
    QTest::keyPress(m_view->windowHandle(), Qt::Key_Shift, Qt::ShiftModifier);
    QCOMPARE(QGuiApplication::keyboardModifiers(), Qt::ShiftModifier);

    const QPointF grip = bottomRightHandle(landed);
    dragOn(objects, 0, grip, grip + QPointF(120.0, -20.0));
    QTest::keyRelease(m_view->windowHandle(), Qt::Key_Shift, Qt::NoModifier);

    // The proportions are what Shift gives up: each edge follows the pointer on
    // its own, so the picture really does come out a different shape.
    const QRectF now = onlyInsertionBox(objects);
    QVERIFY2(nearly(now.width(), landed.width() + 120.0),
             qPrintable(QStringLiteral("the freed corner made it %1 wide rather than %2")
                            .arg(now.width(), 0, 'f', 1)
                            .arg(landed.width() + 120.0, 0, 'f', 1)));
    QVERIFY2(nearly(now.height(), landed.height() + 20.0),
             qPrintable(QStringLiteral("the freed corner made it %1 tall rather than %2")
                            .arg(now.height(), 0, 'f', 1)
                            .arg(landed.height() + 20.0, 0, 'f', 1)));
}

void TestPastedObjects::aPastedPictureOnABarePageCanStillBeTakenHoldOf()
{
    openDocument();

    ObjectOverlay objects(m_view.get());
    objects.setDocument(m_document.get());
    objects.setSource(m_document->filePath());

    // Nothing of the page's own is under the pointer here, and a layer that only
    // answers a press when the file already drew something would let this one
    // fall straight through to whatever is underneath.
    const QRectF landed = pasteOnto(objects, 0, pictureParcel(QRectF(300.0, 300.0, 100.0, 80.0)));
    QVERIFY(!landed.isNull());
    objects.clearChoice();

    QVERIFY2(objects.press(0, landed.center(), Qt::LeftButton), "the press was not taken by the object layer");
    QVERIFY2(!objects.chosenObjects().isEmpty(), "the pasted picture is the only thing there and still chose nothing");
    objects.release(0, landed.center());
}

void TestPastedObjects::oneGestureMeansTheSameToBothKinds()
{
    openDocument();

    ObjectOverlay objects(m_view.get());
    objects.setDocument(m_document.get());
    objects.setSource(m_document->filePath());

    // The same drag, made on the page's own stamp and on a pasted picture. The
    // point is not that either one moves: it is that one gesture becomes one
    // record of the same shape whichever kind of thing it was made on.
    const QPointF shift(30.0, -20.0);
    QVERIFY(objects.press(0, onTheStamp(), Qt::LeftButton));
    objects.release(0, onTheStamp());
    dragOn(objects, 0, onTheStamp(), onTheStamp() + shift);

    const QVector<PageEdit> edits = objects.pendingEdits();
    QCOMPARE(edits.size(), 1);
    QCOMPARE(edits.constFirst().kind, PageEdit::Kind::Transform);
    QVERIFY2(nearly(edits.constFirst().transform.dx(), shift.x())
                 && nearly(edits.constFirst().transform.dy(), shift.y()),
             "the page's own object did not answer the drag, so there is nothing to compare against");

    const QRectF landed = pasteOnto(objects, 0, pictureParcel(pastedAt()));
    QVERIFY(!landed.isNull());
    dragOn(objects, 0, landed.center(), landed.center() + shift);

    // And the pasted one is not among the edits: it names no operator the page
    // has, so what it carries goes into the insertion instead.
    QCOMPARE(objects.pendingEdits().size(), 1);
    QVERIFY2(sameBox(onlyInsertionBox(objects), landed.translated(shift)),
             "the same drag moved the page's own object and left the pasted one behind");
}

// ── B. One kind of thing rather than two ──────────────────────────────────

void TestPastedObjects::aBandRoundBothTakesBoth()
{
    openDocument();

    ObjectOverlay objects(m_view.get());
    objects.setDocument(m_document.get());
    objects.setSource(m_document->filePath());

    // The page's own stamp, so that a band drawn round both has one of each in
    // it. Chosen first to prove the page really does draw something there.
    QVERIFY(objects.press(0, onTheStamp(), Qt::LeftButton));
    objects.release(0, onTheStamp());
    const QVector<int> read = objects.chosenObjects();
    QCOMPARE(read.size(), 1);
    objects.clearChoice();

    const QRectF landed = pasteOnto(objects, 0, pictureParcel(pastedAt()));
    QVERIFY(!landed.isNull());
    objects.clearChoice();

    // A band from above the stamp down past the picture.
    dragOn(objects, 0, QPointF(40.0, 760.0), QPointF(480.0, 120.0));
    const QVector<int> both = objects.chosenObjects();
    QVERIFY2(
        both.size() >= 2,
        qPrintable(QStringLiteral("a band round the stamp and the pasted picture took %1 of them").arg(both.size())));
    QVERIFY2(both.contains(read.constFirst()), "the band missed the object the page itself draws");
    QVERIFY2(both.size() > read.size(), "the band took nothing that had been pasted");
}

void TestPastedObjects::aPastedPictureLinesUpWithWhatWasAlreadyThere()
{
    openDocument();

    ObjectOverlay objects(m_view.get());
    objects.setDocument(m_document.get());
    objects.setSource(m_document->filePath());

    QVERIFY(objects.press(0, onTheStamp(), Qt::LeftButton));
    objects.release(0, onTheStamp());
    QCOMPARE(objects.chosenObjects().size(), 1);
    objects.clearChoice();

    const QRectF landed = pasteOnto(objects, 0, pictureParcel(pastedAt()));
    QVERIFY(!landed.isNull());

    // Both of them, then put on the same left edge.
    dragOn(objects, 0, QPointF(40.0, 760.0), QPointF(480.0, 120.0));
    QVERIFY(objects.chosenObjects().size() >= 2);

    objects.alignChosen(Alignment::Edge::Left);
    const QRectF now = onlyInsertionBox(objects);
    QVERIFY2(!sameBox(now, landed),
             "lining the selection up left the pasted picture exactly where it was, so it took no part in it");

    // Where the stamp's left edge actually is, taken out of the file rather than
    // guessed: the arithmetic under test must not also supply its own answer.
    QString why;
    const QVector<PageObject> onPage = PageObjects::read(m_file, 0, &why);
    QVERIFY2(!onPage.isEmpty(), qPrintable(why));
    double leftmost = onPage.constFirst().bounds.normalized().left();
    for (const PageObject &object : onPage) {
        leftmost = std::min(leftmost, object.bounds.normalized().left());
    }
    QVERIFY2(nearly(now.left(), leftmost, 0.5),
             qPrintable(QStringLiteral("the pasted picture came to rest at x=%1 rather than on the %2 the page's own "
                                       "object stands at")
                            .arg(now.left(), 0, 'f', 1)
                            .arg(leftmost, 0, 'f', 1)));
}

void TestPastedObjects::thePanelDescribesAPastedPicture()
{
    openDocument();

    ObjectOverlay objects(m_view.get());
    objects.setDocument(m_document.get());
    objects.setSource(m_document->filePath());

    const QRectF landed = pasteOnto(objects, 0, pictureParcel(pastedAt()));
    QVERIFY(!landed.isNull());

    Inspectable *panel = objects.inspected();
    QVERIFY2(panel, "a pasted picture has no properties at all, so the sidebar has nothing to show");
    QCOMPARE(panel->kindName(), PageObjects::describe(PageObject::Kind::Image));
    QVERIFY2(!panel->description().isEmpty(), "the panel has no name for what was pasted");
}

void TestPastedObjects::aPastedPictureIsCopiedAndDeletedOnTheSameKeys()
{
    openDocument();

    ObjectOverlay objects(m_view.get());
    objects.setDocument(m_document.get());
    objects.setSource(m_document->filePath());

    const QRectF landed = pasteOnto(objects, 0, pictureParcel(pastedAt()));
    QVERIFY(!landed.isNull());

    // Copied straight back out again: what was pasted is content the program
    // already holds, so this needs nothing out of the file.
    QVERIFY2(objects.copy(), "a pasted picture cannot be copied");
    const PageClipboard::Parcel parcel = PageClipboard::unpack(QGuiApplication::clipboard()->mimeData());
    QCOMPARE(parcel.items.size(), 1);
    QCOMPARE(parcel.items.constFirst().kind, NewContent::Kind::Image);
    QVERIFY2(parcel.missing.isEmpty(),
             qPrintable(QStringLiteral("copying a pasted picture complained: %1").arg(parcel.missing.join(u' '))));
    QVERIFY(!parcel.items.constFirst().image.isNull());

    // Cut is copy and delete on one key, and it has to reach a pasted picture
    // like anything else.
    QVERIFY2(objects.cut(), "a pasted picture cannot be cut");
    QVERIFY2(objects.pendingInsertions().isEmpty(), "the pasted picture was cut and is still on its way into the file");
    QVERIFY2(!objects.hasPendingInsertions(), "the cut paste still counts as work waiting to be written");
    QVERIFY2(!objects.hasPendingEdits(), "removing something that was never written left work waiting on the page");

    // And it is off the page rather than left crossed out: there is no operator
    // to cross out, and a mark no save could clear would sit there for good.
    objects.clearChoice();
    QVERIFY2(!objects.press(0, landed.center(), Qt::LeftButton) || objects.chosenObjects().isEmpty(),
             "the picture that was cut can still be picked up where it used to be");
    objects.release(0, landed.center());
}

void TestPastedObjects::takingBackARemovalBringsThePasteBack()
{
    openDocument();

    ObjectOverlay objects(m_view.get());
    objects.setDocument(m_document.get());
    objects.setSource(m_document->filePath());

    const QRectF landed = pasteOnto(objects, 0, pictureParcel(pastedAt()));
    QVERIFY(!landed.isNull());

    dragOn(objects, 0, landed.center(), landed.center() + QPointF(50.0, 0.0));
    QVERIFY(sameBox(onlyInsertionBox(objects), landed.translated(50.0, 0.0)));

    objects.removeChosen();
    QVERIFY(objects.pendingInsertions().isEmpty());

    objects.takeBack();
    QCOMPARE(objects.pendingInsertions().size(), 1);
    QVERIFY2(sameBox(onlyInsertionBox(objects), landed),
             "taking the removal back put the picture somewhere other than where it was pasted");
}

void TestPastedObjects::twoPastesAreTwoObjectsThatMoveApart()
{
    openDocument();

    ObjectOverlay objects(m_view.get());
    objects.setDocument(m_document.get());
    objects.setSource(m_document->filePath());

    const QRectF first = pasteOnto(objects, 0, pictureParcel(pastedAt()));
    const QRectF second = pasteOnto(objects, 0, pictureParcel(pastedAt()));
    QCOMPARE(objects.pendingInsertions().size(), 2);
    QVERIFY2(!sameBox(first, second), "the second paste landed exactly on the first, so neither can be picked out");

    // The second one is the one chosen, and dragging it must leave the first
    // where it is: two copies of one picture are two objects, not one drawn
    // twice, and a number shared between them would move both.
    dragOn(objects, 0, second.center(), second.center() + QPointF(100.0, 0.0));

    const QVector<NewContent> waiting = objects.pendingInsertions();
    QCOMPARE(waiting.size(), 2);
    QVERIFY2(sameBox(waiting.constFirst().rect.normalized(), first),
             qPrintable(QStringLiteral("dragging the second copy carried the first one to %1 as well")
                            .arg(describe(waiting.constFirst().rect.normalized()))));
    QVERIFY2(sameBox(waiting.constLast().rect.normalized(), second.translated(100.0, 0.0)),
             qPrintable(QStringLiteral("the copy that was dragged ended at %1")
                            .arg(describe(waiting.constLast().rect.normalized()))));
}

// ── C. The turn handle ────────────────────────────────────────────────────

void TestPastedObjects::aPastedPictureIsTurnedByItsHandle()
{
    openDocument();

    ObjectOverlay objects(m_view.get());
    objects.setDocument(m_document.get());
    objects.setSource(m_document->filePath());

    const QRectF landed = pasteOnto(objects, 0, pictureParcel(pastedAt()));
    QVERIFY(!landed.isNull());

    QStringList refusals;
    connect(&objects, &ObjectOverlay::refused, this, [&refusals](const QString &why) { refusals.append(why); });

    turnBy(objects, 0, landed, 90);
    QVERIFY2(refusals.isEmpty(),
             qPrintable(QStringLiteral("turning the pasted picture was refused: %1").arg(refusals.join(u' '))));

    // The corners rather than the box: a quarter turn leaves a box of the same
    // size in the same place, so a boundary alone cannot tell a picture that was
    // turned from one that was quietly squared up again.
    const QPolygonF wanted = turnedAbout(landed, 90);
    const QPolygonF now = quadOf(onlyInsertion(objects));
    QVERIFY2(
        sameQuad(now, wanted),
        qPrintable(
            QStringLiteral("the turned picture stands at %1 rather than at %2").arg(describe(now), describe(wanted))));

    // And it is still the same picture: a turn that grew it is a turn that was
    // mapped onto the box instead of being written down.
    QCOMPARE(onlyInsertion(objects).rect.normalized().size(), landed.size());
}

void TestPastedObjects::oneTurnMeansTheSameToBothKinds()
{
    openDocument();

    ObjectOverlay objects(m_view.get());
    objects.setDocument(m_document.get());
    objects.setSource(m_document->filePath());

    // The page's own stamp first, so that there is something to compare against.
    QVERIFY(objects.press(0, onTheStamp(), Qt::LeftButton));
    objects.release(0, onTheStamp());
    QCOMPARE(objects.chosenObjects().size(), 1);

    // Its boundary out of the file rather than off the overlay: the box the turn
    // handle floats above is the box the file says the stamp fills.
    QString why;
    const QVector<PageObject> onPage = PageObjects::read(m_file, 0, &why);
    QVERIFY2(!onPage.isEmpty(), qPrintable(why));
    QRectF stamp;
    for (const PageObject &object : onPage) {
        if (object.index == objects.chosenObjects().constFirst()) {
            stamp = object.bounds.normalized();
        }
    }
    QVERIFY(!stamp.isEmpty());
    turnBy(objects, 0, stamp, 90);

    const QVector<PageEdit> edits = objects.pendingEdits();
    QCOMPARE(edits.size(), 1);
    QCOMPARE(edits.constFirst().kind, PageEdit::Kind::Transform);
    QVERIFY2(nearly(edits.constFirst().transform.m11(), 0.0, 0.01)
                 && nearly(edits.constFirst().transform.m12(), 1.0, 0.01),
             "the page's own object did not answer the turn, so there is nothing to compare against");
    objects.takeBack();
    objects.clearChoice();

    // The same gesture on a pasted picture. What is being compared is not where
    // either of them ends up but that one gesture becomes one turn of the same
    // amount whichever kind of thing it was made on.
    const QRectF landed = pasteOnto(objects, 0, pictureParcel(pastedAt()));
    QVERIFY(!landed.isNull());
    turnBy(objects, 0, landed, 90);

    QVERIFY2(sameQuad(quadOf(onlyInsertion(objects)), turnedAbout(landed, 90)),
             "the same turn went round the page's own object and left the pasted one alone");
    QVERIFY2(objects.pendingEdits().isEmpty(), "the turn made on the pasted picture was addressed to an operator");
}

void TestPastedObjects::turningAPastedLineOfTextLeavesItsTypeSizeAlone()
{
    openDocument();

    ObjectOverlay objects(m_view.get());
    objects.setDocument(m_document.get());
    objects.setSource(m_document->filePath());

    const QRectF landed = pasteOnto(objects, 0, textParcel(QRectF(200.0, 400.0, 120.0, 16.0)));
    QVERIFY(!landed.isNull());
    const double was = onlyInsertion(objects).fontSize;

    // A wide, low box turned on its side: everything that measures type by the
    // height of the box round it comes out seven times too big here.
    turnBy(objects, 0, landed, 90);
    QVERIFY2(nearly(onlyInsertion(objects).fontSize, was, 0.01),
             qPrintable(QStringLiteral("turning the line changed its type size from %1 pt to %2 pt")
                            .arg(was, 0, 'f', 1)
                            .arg(onlyInsertion(objects).fontSize, 0, 'f', 1)));
    QVERIFY2(sameQuad(quadOf(onlyInsertion(objects)), turnedAbout(landed, 90)), "the line of text did not turn");
}

void TestPastedObjects::aTurnedPastedPictureIsWrittenTurned_data()
{
    QTest::addColumn<int>("sheet");
    QTest::addColumn<int>("degrees");

    // Every turn of the picture against every turn of the sheet, because the two
    // compose: a sign the wrong way round on either of them agrees with the right
    // one at a half turn and disagrees at both quarters.
    for (const int sheet : { 0, 90, 180, 270 }) {
        for (const int degrees : { 0, 90, 180, 270 }) {
            QTest::addRow("sheet %d, picture %d", sheet, degrees) << sheet << degrees;
        }
    }
}

void TestPastedObjects::aTurnedPastedPictureIsWrittenTurned()
{
    QFETCH(int, sheet);
    QFETCH(int, degrees);
    openDocument(sheet);

    ObjectOverlay objects(m_view.get());
    objects.setDocument(m_document.get());
    objects.setSource(m_document->filePath());
    PageProcessor processor(m_document.get(), &m_backend, m_view.get());

    const QRectF landed = pasteOnto(objects, 0, pictureParcel(pastedAt()));
    QVERIFY(!landed.isNull());

    turnBy(objects, 0, landed, degrees);
    const QPolygonF wanted = turnedAbout(landed, degrees);
    QVERIFY2(sameQuad(quadOf(onlyInsertion(objects)), wanted), "the picture did not turn on screen");

    const PageObject written = onlyPicture(writeAndReadBack(objects, processor, 0));
    QCOMPARE(written.kind, PageObject::Kind::Image);

    const QPolygonF inFile = quadOf(written, sheet);
    QVERIFY2(sameQuad(inFile, wanted),
             qPrintable(QStringLiteral("on a sheet turned %1 degrees a picture turned %2 stood at %3 and was written "
                                       "at %4")
                            .arg(sheet)
                            .arg(degrees)
                            .arg(describe(wanted), describe(inFile))));
}

void TestPastedObjects::aPictureTheFileHoldsIsTurnedByTheSameHandle_data()
{
    QTest::addColumn<int>("sheet");
    QTest::addColumn<int>("degrees");

    for (const int sheet : { 0, 90, 180, 270 }) {
        for (const int degrees : { 0, 90, 180, 270 }) {
            QTest::addRow("sheet %d, picture %d", sheet, degrees) << sheet << degrees;
        }
    }
}

void TestPastedObjects::aPictureTheFileHoldsIsTurnedByTheSameHandle()
{
    QFETCH(int, sheet);
    QFETCH(int, degrees);
    openDocument(sheet);

    ObjectOverlay objects(m_view.get());
    objects.setDocument(m_document.get());
    objects.setSource(m_document->filePath());
    PageProcessor processor(m_document.get(), &m_backend, m_view.get());

    // Written out first, so that what is turned below is a picture the page
    // itself draws rather than a paste. The gesture under test is the same one;
    // the point of the case is that the kind of thing it is made on is not.
    QVERIFY(!pasteOnto(objects, 0, pictureParcel(pastedAt())).isNull());
    const PageObject read = onlyPicture(writeAndReadBack(objects, processor, 0));
    QCOMPARE(read.kind, PageObject::Kind::Image);

    const QRectF box = read.bounds.normalized();
    QVERIFY(objects.press(0, box.center(), Qt::LeftButton));
    objects.release(0, box.center());
    QCOMPARE(objects.chosenObjects().size(), 1);
    QVERIFY2(objects.chosenObjects().constFirst() >= 0,
             "the picture is in the file now and is still being counted as something pasted");

    turnBy(objects, 0, box, degrees);

    const PageObject written = onlyPicture(writeAndReadBack(objects, processor, 0));
    QCOMPARE(written.kind, PageObject::Kind::Image);

    const QPolygonF wanted = turnedAbout(box, degrees);
    const QPolygonF inFile = quadOf(written, sheet);
    QVERIFY2(sameQuad(inFile, wanted),
             qPrintable(QStringLiteral("on a sheet turned %1, a picture the page draws and the reader turned %2 was "
                                       "written at %3 rather than at %4")
                            .arg(sheet)
                            .arg(degrees)
                            .arg(describe(inFile), describe(wanted))));

    // And it is the same picture afterwards: a turn that made it bigger is a
    // turn that went into the box round it instead of into the picture.
    QVERIFY2(nearly(QLineF(inFile.at(0), inFile.at(1)).length(), box.width())
                 && nearly(QLineF(inFile.at(1), inFile.at(2)).length(), box.height()),
             "turning the picture changed its size");
}

void TestPastedObjects::aPictureTheFileHoldsTravelsWhileItIsDragged()
{
    openDocument();

    ObjectOverlay objects(m_view.get());
    objects.setDocument(m_document.get());
    objects.setSource(m_document->filePath());
    PageProcessor processor(m_document.get(), &m_backend, m_view.get());

    // Into the file first: the whole complaint is about a picture the page draws
    // for itself, which is baked into the sheet under the layer and cannot move
    // by itself however far the boundary round it is dragged.
    QVERIFY(!pasteOnto(objects, 0, pictureParcel(pastedAt())).isNull());
    const QRectF box = onlyPicture(writeAndReadBack(objects, processor, 0)).bounds.normalized();
    QVERIFY(!box.isEmpty());

    QVERIFY(objects.press(0, box.center(), Qt::LeftButton));
    objects.release(0, box.center());
    QCOMPARE(objects.chosenObjects().size(), 1);
    QVERIFY2(liftReady(objects, 0), "the chosen picture never came off the page as pixels");

    // Held halfway through the drag rather than after it: "the images do not
    // travel at all" is about what the hand sees while it is still moving.
    const QPointF shift(150.0, -120.0);
    objects.press(0, box.center(), Qt::LeftButton);
    objects.move(0, box.center() + shift / 2.0, Qt::LeftButton);
    objects.move(0, box.center() + shift, Qt::LeftButton);

    const QImage midDrag = painted(objects, 0);
    const QColor arrived = colourAt(midDrag, 0, box.center() + shift);
    QVERIFY2(arrived.red() > 120 && arrived.blue() < 110,
             qPrintable(QStringLiteral("the picture was dragged half a page and %1 is what is drawn where it went")
                            .arg(arrived.name())));

    const QColor left = colourAt(midDrag, 0, box.center());
    QVERIFY2(
        left.red() > 200 && left.green() > 200 && left.blue() > 200,
        qPrintable(
            QStringLiteral("where the picture was dragged from shows %1 rather than bare paper").arg(left.name())));

    // And it stays where it was put once the mouse is let go, because the file
    // still says nothing about any of this until the document is saved.
    objects.release(0, box.center() + shift);
    const QColor settled = colourAt(painted(objects, 0), 0, box.center() + shift);
    QVERIFY2(settled.red() > 120 && settled.blue() < 110,
             qPrintable(QStringLiteral("after the drag the picture shows %1 where it was left").arg(settled.name())));
}

void TestPastedObjects::aPictureTheFileHoldsIsSeenToTurn()
{
    openDocument();

    ObjectOverlay objects(m_view.get());
    objects.setDocument(m_document.get());
    objects.setSource(m_document->filePath());
    PageProcessor processor(m_document.get(), &m_backend, m_view.get());

    // Red on its left half and blue on its right, so that a quarter turn is
    // something both the eye and this case can actually see.
    QVERIFY(!pasteOnto(objects, 0, pictureParcel(pastedAt(), twoTone())).isNull());
    const QRectF box = onlyPicture(writeAndReadBack(objects, processor, 0)).bounds.normalized();
    QVERIFY(!box.isEmpty());

    QVERIFY(objects.press(0, box.center(), Qt::LeftButton));
    objects.release(0, box.center());
    QCOMPARE(objects.chosenObjects().size(), 1);
    QVERIFY2(liftReady(objects, 0), "the chosen picture never came off the page as pixels");

    turnBy(objects, 0, box, 90);

    // A quarter turn the way the page's own points run takes what was on the
    // left down to the bottom. Quartered rather than halved so that each point
    // is well inside its own half of the turned box.
    const QImage canvas = painted(objects, 0);
    const QColor low = colourAt(canvas, 0, box.center() - QPointF(0.0, box.width() / 4.0));
    const QColor high = colourAt(canvas, 0, box.center() + QPointF(0.0, box.width() / 4.0));

    QVERIFY2(
        low.red() > 120 && low.blue() < 110,
        qPrintable(
            QStringLiteral("the picture's left half came out %1 at the bottom of the turned box").arg(low.name())));
    QVERIFY2(high.blue() > 120 && high.red() < 110,
             qPrintable(
                 QStringLiteral("the picture's right half came out %1 at the top of the turned box").arg(high.name())));
}

void TestPastedObjects::theBoundaryGoesRoundWithTheObjectRatherThanGrowing()
{
    openDocument();

    ObjectOverlay objects(m_view.get());
    objects.setDocument(m_document.get());
    objects.setSource(m_document->filePath());

    const QRectF landed = pasteOnto(objects, 0, pictureParcel(pastedAt()));
    QVERIFY(!landed.isNull());

    // Half a right angle, because that is the turn at which the box round a
    // turned rectangle is at its most unlike the rectangle: a boundary drawn as
    // that box instead reaches into all four corners the rectangle has left.
    turnBy(objects, 0, landed, 45);

    // Well inside the box round the turned picture and well outside the picture
    // itself, and clear of the handles, which do stand on the upright box.
    const QPointF corner = landed.center() + QPointF(64.0, 64.0);
    const QColor there = colourAt(painted(objects, 0), 0, corner);
    QVERIFY2(there == QColor(0, 200, 0),
             qPrintable(QStringLiteral("the boundary reached a corner the turned picture has left, showing %1 there")
                            .arg(there.name())));
}

// ── D. What is written is what was seen ───────────────────────────────────

void TestPastedObjects::aPastedPictureLandsWhereItWasDragged()
{
    openDocument();

    ObjectOverlay objects(m_view.get());
    objects.setDocument(m_document.get());
    objects.setSource(m_document->filePath());
    PageProcessor processor(m_document.get(), &m_backend, m_view.get());

    // High on the sheet, so that half a page down is still on the paper.
    const QRectF landed = pasteOnto(objects, 0, pictureParcel(QRectF(200.0, 600.0, 120.0, 90.0)));
    QVERIFY(!landed.isNull());

    // Half a page down and a good way across, then made bigger by its corner.
    const QPointF shift(120.0, -300.0);
    dragOn(objects, 0, landed.center(), landed.center() + shift);
    const QRectF moved = onlyInsertionBox(objects);

    const QPointF grip = bottomRightHandle(moved);
    dragOn(objects, 0, grip, grip + QPointF(40.0, -30.0));

    const QRectF wanted = onlyInsertionBox(objects);
    const PageObject written = onlyPicture(writeAndReadBack(objects, processor, 0));
    QCOMPARE(written.kind, PageObject::Kind::Image);
    QVERIFY2(sameBox(written.bounds.normalized(), wanted, 1.0),
             qPrintable(QStringLiteral("the picture was left at %1 on screen and written at %2")
                            .arg(describe(wanted), describe(written.bounds.normalized()))));
}

void TestPastedObjects::aPastedPictureOnATurnedPageLandsWhereItWasDragged_data()
{
    everyTurn();
}

void TestPastedObjects::aPastedPictureOnATurnedPageLandsWhereItWasDragged()
{
    QFETCH(int, degrees);
    openDocument(degrees);

    ObjectOverlay objects(m_view.get());
    objects.setDocument(m_document.get());
    objects.setSource(m_document->filePath());
    PageProcessor processor(m_document.get(), &m_backend, m_view.get());

    const QRectF landed = pasteOnto(objects, 0, pictureParcel(pastedAt()));
    QVERIFY(!landed.isNull());

    dragOn(objects, 0, landed.center(), landed.center() + QPointF(60.0, -80.0));
    const QRectF wanted = onlyInsertionBox(objects);

    const PageObject written = onlyPicture(writeAndReadBack(objects, processor, 0));
    QCOMPARE(written.kind, PageObject::Kind::Image);
    QVERIFY2(sameBox(written.bounds.normalized(), wanted, 1.0),
             qPrintable(QStringLiteral("on a sheet turned %1 degrees the picture was left at %2 and written at %3")
                            .arg(degrees)
                            .arg(describe(wanted), describe(written.bounds.normalized()))));
}

void TestPastedObjects::turningTheSheetCarriesThePasteRoundWithIt_data()
{
    everyTurn();
}

void TestPastedObjects::turningTheSheetCarriesThePasteRoundWithIt()
{
    QFETCH(int, degrees);
    openDocument();

    ObjectOverlay objects(m_view.get());
    objects.setDocument(m_document.get());
    objects.setSource(m_document->filePath());
    PageProcessor processor(m_document.get(), &m_backend, m_view.get());

    const QRectF landed = pasteOnto(objects, 0, pictureParcel(pastedAt()));
    QVERIFY(!landed.isNull());

    // Turned after the paste, which is the awkward order: the copy was measured
    // against a sheet that has since gone round.
    m_document->applyRotation({ 0 }, degrees);
    settle();

    const QRectF wanted = onlyInsertionBox(objects);
    const QRectF asTheViewTurnsIt = pageTurn(degrees, m_view->pageSizePoints(0)).mapRect(landed);
    QVERIFY2(sameBox(wanted, asTheViewTurnsIt, 1.0),
             qPrintable(QStringLiteral("turning the sheet %1 degrees left the paste at %2 rather than at %3")
                            .arg(degrees)
                            .arg(describe(wanted), describe(asTheViewTurnsIt))));

    const PageObject written = onlyPicture(writeAndReadBack(objects, processor, 0));
    QCOMPARE(written.kind, PageObject::Kind::Image);
    QVERIFY2(sameBox(written.bounds.normalized(), wanted, 1.0),
             qPrintable(QStringLiteral("after the sheet was turned %1 degrees the picture showed at %2 and was "
                                       "written at %3")
                            .arg(degrees)
                            .arg(describe(wanted), describe(written.bounds.normalized()))));
}

void TestPastedObjects::aPastedLineOfTextGrowsWithItsBox()
{
    openDocument();

    ObjectOverlay objects(m_view.get());
    objects.setDocument(m_document.get());
    objects.setSource(m_document->filePath());

    const QRectF landed = pasteOnto(objects, 0, textParcel(QRectF(200.0, 400.0, 120.0, 16.0)));
    QVERIFY(!landed.isNull());
    QCOMPARE(onlyInsertion(objects).kind, NewContent::Kind::Text);
    const double was = onlyInsertion(objects).fontSize;

    // Twice as tall, so the letters have to come out twice the size: a read line
    // of text is wrapped in the scale and really does grow, and a pasted one that
    // kept its type size would be the two kinds drifting apart again. By the
    // edge rather than the corner, because changing one measurement and not the
    // other is exactly what an edge handle is for.
    const QPointF grip = bottomEdgeHandle(landed);
    dragOn(objects, 0, grip, grip + QPointF(0.0, -landed.height()));

    const NewContent now = onlyInsertion(objects);
    QVERIFY2(nearly(now.rect.normalized().height(), landed.height() * 2.0, 1.0),
             qPrintable(QStringLiteral("the box came out %1 points tall rather than %2")
                            .arg(now.rect.normalized().height(), 0, 'f', 1)
                            .arg(landed.height() * 2.0, 0, 'f', 1)));
    QVERIFY2(nearly(now.fontSize, was * 2.0, 0.5),
             qPrintable(QStringLiteral("the box was pulled to twice the height and the letters stayed at %1 pt")
                            .arg(now.fontSize, 0, 'f', 1)));
}

QTEST_MAIN(TestPastedObjects)

#include "tst_pastedobjects.moc"
