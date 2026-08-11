/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "TestPdf.h"

#include "core/Document.h"
#include "core/DocumentWriter.h"
#include "core/FormBuilder.h"
#include "core/Forms.h"
#include "core/RenderBackend.h"
#include "ui/AnnotationOverlay.h"
#include "ui/FormDesignOverlay.h"
#include "ui/FormOverlay.h"
#include "ui/Inspector.h"
#include "ui/ObjectOverlay.h"
#include "ui/PageProcessor.h"
#include "ui/PageView.h"
#include "ui/TextOverlay.h"

#include <QApplication>
#include <QLabel>
#include <QLineEdit>
#include <QSignalSpy>
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
 * outside the event loop cannot be rescued by a timer. Turning it into a crash
 * that names itself is the difference between a failing test and a suite nobody
 * runs.
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
 * A rasteriser that draws nothing and says which file it was asked about.
 *
 * Two things are wanted from it and neither is a picture. Page sizes, because
 * ps::Document answers none without a backend and half the overlays measure
 * pages. And the words on a page, stamped with the file and page they were
 * asked for, which is the whole of what a guessed form label is read from, and
 * therefore the only way to prove that the right document was asked.
 *
 * No render is ever started against it, because the view is deliberately left
 * without one: a worker thread scribbling in @ref asked while the test reads it
 * would be a race in the test rather than in the program.
 */
class SayingBackend : public RenderBackend
{
public:
    bool addDocument(int sourceId, const QString &path, QString *error) override
    {
        Q_UNUSED(error)
        m_paths.insert(sourceId, path);
        return true;
    }

    void removeDocument(int sourceId) override { m_paths.remove(sourceId); }

    QImage renderPage(int, int, int) override { return {}; }

    QSizeF pageSizePoints(int, int) override { return QSizeF(612, 792); }

    QString extractText(int, int) override { return {}; }

    QStringList wordsInside(int, int, const QRectF &) override { return {}; }

    QVector<Word> words(int sourceId, int page) override
    {
        asked.append(QStringLiteral("S%1P%2").arg(sourceId).arg(page));

        // One word, printed just to the right of where every field in this test
        // is put, saying which file and which page inside it produced it.
        Word word;
        word.text = QStringLiteral("S%1P%2").arg(sourceId).arg(page);
        word.rect = QRectF(430, 400, 60, 14);
        return { word };
    }

    /** Every (file, page) a label has been looked for on, in order. */
    QStringList asked;

private:
    QHash<int, QString> m_paths;
};

/** Where the one form field of the merged fixture sits, in display points. */
constexpr double FieldLeft = 300.0;
constexpr double FieldBottom = 400.0;
constexpr double FieldWidth = 120.0;
constexpr double FieldHeight = 20.0;

QRectF fieldRect()
{
    return QRectF(FieldLeft, FieldBottom, FieldWidth, FieldHeight);
}

QPointF insideField()
{
    return QPointF(FieldLeft + FieldWidth / 2.0, FieldBottom + FieldHeight / 2.0);
}

/** Where writeSamplePdf() stamps "PSPAGE n", so a press lands on that text. */
QPointF onTheStamp()
{
    return QPointF(100.0, 706.0);
}

int indexOfField(const QVector<FormField> &fields, const QString &name)
{
    for (int i = 0; i < fields.size(); ++i) {
        if (fields.at(i).name == name) {
            return i;
        }
    }
    return -1;
}

/** Two boxes are the same box when no edge of them is a tenth of a point out. */
bool sameBox(const QRectF &one, const QRectF &other)
{
    return qAbs(one.left() - other.left()) < 0.1 && qAbs(one.top() - other.top()) < 0.1
        && qAbs(one.width() - other.width()) < 0.1 && qAbs(one.height() - other.height()) < 0.1;
}

QString describe(const QRectF &box)
{
    return QStringLiteral("%1,%2 %3×%4")
        .arg(box.left(), 0, 'f', 1)
        .arg(box.top(), 0, 'f', 1)
        .arg(box.width(), 0, 'f', 1)
        .arg(box.height(), 0, 'f', 1);
}

/** The four turns, as every case about turning is run against all of them. */
void everyTurn()
{
    QTest::addColumn<int>("degrees");
    // Both quarters, because 180 hides a swapped sign and a quarter turn hides
    // a swapped axis, and neither of them alone catches both.
    QTest::newRow("upright") << 0;
    QTest::newRow("a quarter clockwise") << 90;
    QTest::newRow("upside down") << 180;
    QTest::newRow("three quarters clockwise") << 270;
}

/** Every piece of text a properties panel put on screen. */
QStringList labelsOf(QWidget *panel)
{
    QStringList said;
    const QList<QLabel *> labels = panel->findChildren<QLabel *>();
    for (const QLabel *label : labels) {
        said.append(label->text());
    }
    return said;
}

} // namespace

/**
 * A page index that came out of a file is not a view row.
 *
 * ps::Document is a list of PageRefs, each naming a page inside one of the files
 * the session has open, so "page seven" means two different things: the seventh
 * sheet of the document being assembled, and the eighth page object of some
 * particular file. They are the same number for exactly as long as nobody
 * deletes a page, drags one somewhere else or opens a second document, which is
 * why every case here works on a document built from **two** files with pages
 * deleted and reordered. Nothing else catches these: with one file and no edits
 * the two numberings agree and every one of the confusions below looks correct.
 */
class TestOverlayRows : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void init();
    void cleanup();

    void formFieldsFollowThePageTheyArePrintedOn();
    void thePanelNamesThePageTheReaderIsLookingAt();
    void guessedLabelsAreReadOutOfTheFileTheFieldIsIn();
    void aFieldOnATurnedPageIsWhereTheTurnedPageShowsIt_data();
    void aFieldOnATurnedPageIsWhereTheTurnedPageShowsIt();
    void aFieldDrawnOnAPageIsWrittenOntoThatPage();
    void aChangedFieldIsWrittenOntoThePageItIsShownOn();
    void aFieldMovedOnATurnedPageIsWrittenWhereItWasPutDown_data();
    void aFieldMovedOnATurnedPageIsWrittenWhereItWasPutDown();
    void aFieldDrawnOnATurnedPageIsWrittenWhereItWasDrawn_data();
    void aFieldDrawnOnATurnedPageIsWrittenWhereItWasDrawn();
    void turningASheetTurnsTheFieldWaitingOnIt_data();
    void turningASheetTurnsTheFieldWaitingOnIt();
    void objectHandlesComeFromThePageUnderThePointer();
    void turningAPageKeepsTheCommentsMadeOnTheOthers();
    void movingPagesCarriesTheirCommentsAlong();
    void turningAPageKeepsTheCorrectionsTypedOnTheOthers();
    void deletingAPageSaysWhatItTookWithIt();

private:
    /**
     * A document merged from two files, then named after a third.
     *
     * Which is what a merge followed by "Save as" leaves behind: nine rows drawn
     * from two open files, and a file path pointing at neither of them. The form
     * is read out of that third file, so its page numbers are the merged file's
     * and the rows are the document's, and the two agree only at this instant.
     */
    void openMergedDocument();

    /** Takes the first sheet out and carries the next two to the back. */
    void deleteAndReorder();

    /**
     * Where @p box, measured on the upright seventh sheet, is once it is turned.
     *
     * A field is put on that sheet at @p box, the sheet is turned by @p degrees
     * and the document written out (which bakes the turn into the page's
     * `/Rotate`, exactly as saving does and exactly what every overlay's work is
     * eventually handed), and the form is read back out of the result.
     *
     * Every step of it is the engine's own and no part of the view is involved,
     * so this is an answer to hold the overlays to rather than their own
     * arithmetic written out a second time and agreeing with itself.
     */
    QRectF asTheFileTurnsIt(const QRectF &box, int degrees);

    /**
     * Runs the design layer's waiting work through the engine and reads it back.
     *
     * The answer is where the fields have actually landed, measured on the page
     * as it is now drawn, which is what the round trip is for: a box written a
     * quarter turn out is not visible in anything the overlay says about itself.
     */
    QVector<FormField> writeAndReadBack(FormDesignOverlay &design, PageProcessor &processor);

    /**
     * Lets the layers answer a change to the page list.
     *
     * Moving a page is a removal followed by an insertion, so the layers answer
     * both at once from the event loop rather than reacting to the instant in
     * between when the page is nowhere at all. Anything driving the document
     * without an event loop running has to give them one.
     */
    static void settle();

    QTemporaryDir m_dir;

    /** Five stamped pages and four more, which become the two sources. */
    QString m_first;
    QString m_second;

    /** The two of them written out as one file, with no form on it at all. */
    QString m_merged;

    /** The same file with a form field on page 7. */
    QString m_mergedForm;

    /** Names the oracle's working files apart, since a case may ask twice. */
    int m_oracles = 0;

    /** Seven stamped pages, and two pages with nothing drawn on them at all. */
    QString m_stamped;
    QString m_blank;

    std::unique_ptr<Document> m_document;
    std::unique_ptr<PageView> m_view;
    SayingBackend m_backend;
    std::unique_ptr<Watchdog> m_watchdog;
};

// ── The fixtures ──────────────────────────────────────────────────────────

void TestOverlayRows::initTestCase()
{
    KLocalizedString::setApplicationDomain("pdf-smithy");
    QVERIFY(m_dir.isValid());

    m_first = m_dir.filePath(QStringLiteral("first.pdf"));
    m_second = m_dir.filePath(QStringLiteral("second.pdf"));
    m_stamped = m_dir.filePath(QStringLiteral("stamped.pdf"));
    m_blank = m_dir.filePath(QStringLiteral("blank.pdf"));

    QVERIFY(test::writeSamplePdf(m_first, 5));
    QVERIFY(test::writeSamplePdf(m_second, 4));
    QVERIFY(test::writeSamplePdf(m_stamped, 7));
    // Pages with an empty content stream: nothing is drawn on them, so anything
    // that offers a handle on one is offering another page's contents.
    QVERIFY(test::writeLinkedPdf(m_blank, 2, 0));

    // The merged file the form is read from. Written from a real document so
    // that its pages are the two sources' pages in the order the view shows
    // them, which is exactly what saving a merged document produces.
    Document assembled;
    QString error;
    QVERIFY2(assembled.open(m_first, &error), qPrintable(error));
    QVERIFY2(assembled.importFile(m_second, -1, &error), qPrintable(error));
    m_merged = m_dir.filePath(QStringLiteral("merged.pdf"));
    QVERIFY2(DocumentWriter::write(assembled, m_merged, {}, &error), qPrintable(error));
    QCOMPARE(test::pageCountOf(m_merged), 9);

    FormBuilder::Field field;
    field.kind = FormBuilder::Field::Kind::Text;
    field.name = QStringLiteral("Zeta");
    // The seventh sheet, which is the second file's second page, so the row it
    // is shown at and the page the form names are different files' business.
    field.page = 6;
    field.rect = fieldRect();

    m_mergedForm = m_dir.filePath(QStringLiteral("merged-form.pdf"));
    int added = 0;
    QVERIFY2(FormBuilder::addFields(m_merged, m_mergedForm, { field }, &added, &error), qPrintable(error));
    QCOMPARE(added, 1);
}

void TestOverlayRows::init()
{
    m_watchdog = std::make_unique<Watchdog>(QByteArray(QTest::currentTestFunction()));

    m_document = std::make_unique<Document>();
    m_document->setRenderBackend(&m_backend);

    m_view = std::make_unique<PageView>();
    m_view->setDocument(m_document.get());
    m_view->resize(900, 700);

    m_backend.asked.clear();
}

void TestOverlayRows::cleanup()
{
    // The view keeps raw pointers to its overlays and the overlays keep one to
    // the document, so they go in that order or not at all.
    m_view.reset();
    m_document.reset();
    m_watchdog.reset();
}

void TestOverlayRows::openMergedDocument()
{
    QString error;
    QVERIFY2(m_document->open(m_first, &error), qPrintable(error));
    QVERIFY2(m_document->importFile(m_second, -1, &error), qPrintable(error));
    m_document->setFilePath(m_mergedForm);

    QCOMPARE(m_document->pageCount(), 9);
    QCOMPARE(m_document->pageAt(6).sourceId, 1);
    QCOMPARE(m_document->pageAt(6).sourcePage, 1);
}

void TestOverlayRows::settle()
{
    QCoreApplication::sendPostedEvents();
    QCoreApplication::processEvents();
}

void TestOverlayRows::deleteAndReorder()
{
    m_document->removePages({ 0 });
    m_document->movePages({ 0, 1 }, 8);
    settle();
    QCOMPARE(m_document->pageCount(), 8);

    // The field's page (the second file's second page) has travelled from row
    // six to row three without anything about the form changing.
    QCOMPARE(m_document->pageAt(3).sourceId, 1);
    QCOMPARE(m_document->pageAt(3).sourcePage, 1);
}

QRectF TestOverlayRows::asTheFileTurnsIt(const QRectF &box, int degrees)
{
    const int serial = ++m_oracles;

    FormBuilder::Field field;
    field.kind = FormBuilder::Field::Kind::Text;
    field.name = QStringLiteral("Oracle");
    field.page = 6;
    field.rect = box;

    QString error;
    const QString marked = m_dir.filePath(QStringLiteral("oracle-%1.pdf").arg(serial));
    int added = 0;
    if (!FormBuilder::addFields(m_merged, marked, { field }, &added, &error) || added != 1) {
        return {};
    }

    Document turned;
    turned.setRenderBackend(&m_backend);
    if (!turned.open(marked, &error)) {
        return {};
    }
    turned.applyRotation({ 6 }, degrees);

    const QString written = m_dir.filePath(QStringLiteral("oracle-turned-%1.pdf").arg(serial));
    if (!DocumentWriter::write(turned, written, {}, &error)) {
        return {};
    }

    const QVector<FormField> fields = Forms::read(written, nullptr);
    const int at = indexOfField(fields, QStringLiteral("Oracle"));
    return at < 0 ? QRectF() : fields.at(at).rect;
}

QVector<FormField> TestOverlayRows::writeAndReadBack(FormDesignOverlay &design, PageProcessor &processor)
{
    const FormDesignOverlay::Work work = design.pendingWork();
    QVector<QPair<QString, QString>> renamed;
    renamed.reserve(work.renamed.size());
    for (const FormDesignOverlay::Rename &rename : work.renamed) {
        renamed.append({ rename.from, rename.to });
    }

    QString error;
    if (!processor.buildForm(work.removed, renamed, work.changed, work.added, &error)) {
        qWarning("the engine refused the work: %s", qPrintable(error));
        return {};
    }
    settle();

    // The engine wrote a whole new file and the document now shows its pages, so
    // that file is where the fields have actually ended up. Its pages start
    // upright (the turn went into their /Rotate), which is why what comes back
    // is measured in the same space the user was looking at.
    const Source *source = m_document->source(m_document->pageAt(0).sourceId);
    return source ? Forms::read(source->path(), nullptr) : QVector<FormField>();
}

// ── A. The form ───────────────────────────────────────────────────────────

void TestOverlayRows::formFieldsFollowThePageTheyArePrintedOn()
{
    openMergedDocument();

    FormOverlay form(m_view.get());
    form.setDocument(m_document.get());
    m_view->addOverlay(&form);
    form.setFields(Forms::read(m_document->filePath(), nullptr));

    const int zeta = indexOfField(form.fields(), QStringLiteral("Zeta"));
    QVERIFY2(zeta >= 0, "the fixture no longer carries the field every case here is about");
    QCOMPARE(form.rowOfField(zeta), 6);

    // Where it plainly sits, before anything has moved.
    QVERIFY(form.press(6, insideField(), Qt::LeftButton));
    QCOMPARE(form.selectedField(), zeta);
    form.deselect();

    deleteAndReorder();

    QCOMPARE(form.rowOfField(zeta), 3);
    QVERIFY2(form.press(3, insideField(), Qt::LeftButton),
             "the field is no longer where the page it is printed on is drawn, so it cannot be filled in at all");
    QCOMPARE(form.selectedField(), zeta);

    // And it is not on the row its old number named, which is a different sheet
    // out of a different file now.
    form.deselect();
    QVERIFY2(!form.press(6, insideField(), Qt::LeftButton),
             "the field is still being drawn on whatever page has moved into row six");
    QCOMPARE(form.selectedField(), -1);
}

void TestOverlayRows::thePanelNamesThePageTheReaderIsLookingAt()
{
    openMergedDocument();

    FormOverlay form(m_view.get());
    form.setDocument(m_document.get());
    m_view->addOverlay(&form);
    form.setFields(Forms::read(m_document->filePath(), nullptr));

    const int zeta = indexOfField(form.fields(), QStringLiteral("Zeta"));
    QVERIFY(zeta >= 0);

    deleteAndReorder();
    form.selectField(zeta);
    QCOMPARE(form.selectedField(), zeta);

    Inspectable *described = form.inspected();
    QVERIFY2(described, "choosing a field described nothing");
    std::unique_ptr<QWidget> panel(described->buildEditor(nullptr));
    QVERIFY(panel);

    // The field's page is the only number the panel puts on screen, so the two
    // candidates can be told apart by looking for them.
    const QStringList said = labelsOf(panel.get());
    QVERIFY2(said.contains(QStringLiteral("4")),
             "the panel does not name the page the reader is looking at, which is the fourth");
    QVERIFY2(!said.contains(QStringLiteral("7")),
             "the panel is still quoting the page number the form's own file uses");
}

void TestOverlayRows::guessedLabelsAreReadOutOfTheFileTheFieldIsIn()
{
    openMergedDocument();

    FormOverlay form(m_view.get());
    form.setDocument(m_document.get());
    m_view->addOverlay(&form);
    form.setFields(Forms::read(m_document->filePath(), nullptr));
    // Exactly as the window does it, first row's file and all: with a document
    // to ask, that hint is not what decides which file a label comes out of.
    form.setLabelSource(&m_backend, m_document->pageAt(0).sourceId);

    const int zeta = indexOfField(form.fields(), QStringLiteral("Zeta"));
    QVERIFY(zeta >= 0);

    const FormOverlay::FieldLabel label = form.labelOf(zeta);
    QVERIFY2(label.guessed, "the field carries no /TU, so its label can only have been read off the page");
    QCOMPARE(label.text, QStringLiteral("S1P1"));
    QVERIFY2(!m_backend.asked.contains(QStringLiteral("S0P6")),
             "the words beside the field were looked for in the first file, at the row number the second file's page "
             "happens to sit at");
}

void TestOverlayRows::aFieldOnATurnedPageIsWhereTheTurnedPageShowsIt_data()
{
    everyTurn();
}

void TestOverlayRows::aFieldOnATurnedPageIsWhereTheTurnedPageShowsIt()
{
    QFETCH(int, degrees);

    openMergedDocument();

    FormOverlay form(m_view.get());
    form.setDocument(m_document.get());
    m_view->addOverlay(&form);
    form.setFields(Forms::read(m_document->filePath(), nullptr));

    const int zeta = indexOfField(form.fields(), QStringLiteral("Zeta"));
    QVERIFY(zeta >= 0);

    // Moved as well as turned: a turn only ever applied to a page that stayed at
    // its own row number is a turn that cannot be told from the row number.
    deleteAndReorder();
    m_document->applyRotation({ 3 }, degrees);
    settle();

    const QRectF shown = asTheFileTurnsIt(fieldRect(), degrees);
    QVERIFY2(!shown.isEmpty(), "the fixture no longer says where the field is once its sheet has been turned");

    QVERIFY2(
        form.press(3, shown.center(), Qt::LeftButton),
        qPrintable(
            QStringLiteral("nothing to fill in at %1, where the turned sheet draws the field").arg(describe(shown))));
    QCOMPARE(form.selectedField(), zeta);

    form.deselect();
    if (degrees == 0) {
        return; // Nothing was turned, so there is no second place to look.
    }

    // Where the box sat before the sheet was turned, which on a turned sheet is
    // a piece of bare paper somewhere else entirely.
    QVERIFY2(!form.press(3, insideField(), Qt::LeftButton),
             "the box is still where the file measured it, as though the sheet had never been turned");
    QCOMPARE(form.selectedField(), -1);
}

// ── D. Designing the form ─────────────────────────────────────────────────

void TestOverlayRows::aFieldDrawnOnAPageIsWrittenOntoThatPage()
{
    openMergedDocument();
    m_view->setMode(PageView::Mode::Edit);

    FormDesignOverlay design(m_view.get());
    design.setDocument(m_document.get());
    m_view->addOverlay(&design);
    design.setSource(m_document->filePath());
    design.setFields(Forms::read(m_document->filePath(), nullptr));

    // Drawn on the seventh sheet, which is the second file's second page.
    design.setKindToPlace(FormDesignOverlay::Kind::Text);
    QVERIFY(design.press(6, QPointF(100, 300), Qt::LeftButton));
    design.move(6, QPointF(180, 320), Qt::LeftButton);
    design.release(6, QPointF(220, 340));

    FormDesignOverlay::Work work = design.pendingWork();
    QCOMPARE(work.added.size(), 1);
    QCOMPARE(work.added.at(0).page, 6);

    deleteAndReorder();

    work = design.pendingWork();
    QCOMPARE(work.added.size(), 1);
    QVERIFY2(work.added.at(0).page == 3,
             "the field would be written onto whatever page has moved into the row it was drawn at");
}

void TestOverlayRows::aChangedFieldIsWrittenOntoThePageItIsShownOn()
{
    openMergedDocument();
    m_view->setMode(PageView::Mode::Edit);

    FormDesignOverlay design(m_view.get());
    design.setDocument(m_document.get());
    m_view->addOverlay(&design);
    design.setSource(m_document->filePath());
    design.setFields(Forms::read(m_document->filePath(), nullptr));

    // The document's own field, taken hold of where it is drawn and moved.
    QVERIFY2(design.press(6, insideField(), Qt::LeftButton),
             "the form's existing field is not drawn on the row its page is shown at");
    QCOMPARE(design.chosenCount(), 1);
    design.nudge(10.0, 0.0);

    FormDesignOverlay::Work work = design.pendingWork();
    QCOMPARE(work.changed.size(), 1);
    QVERIFY2(work.changed.at(0).page == 6,
             "the engine is handed the document row by row, so a change has to name the row, not the page number the "
             "form's own file uses");

    deleteAndReorder();

    work = design.pendingWork();
    QCOMPARE(work.changed.size(), 1);
    QCOMPARE(work.changed.at(0).page, 3);
}

void TestOverlayRows::aFieldMovedOnATurnedPageIsWrittenWhereItWasPutDown_data()
{
    everyTurn();
}

void TestOverlayRows::aFieldMovedOnATurnedPageIsWrittenWhereItWasPutDown()
{
    QFETCH(int, degrees);

    // Opened from the form's own file rather than assembled from the two
    // sources, unlike everything else here: this case goes all the way through
    // the engine and back, and the engine can only rewrite a field that the
    // document's own pages actually carry.
    QString error;
    QVERIFY2(m_document->open(m_mergedForm, &error), qPrintable(error));
    m_view->setMode(PageView::Mode::Edit);

    FormDesignOverlay design(m_view.get());
    design.setDocument(m_document.get());
    m_view->addOverlay(&design);
    design.setSource(m_document->filePath());
    design.setFields(Forms::read(m_document->filePath(), nullptr));

    // The first sheet taken out, so the field's page in the file and the row it
    // is drawn at have parted company before anything is turned at all.
    m_document->removePages({ 0 });
    settle();
    m_document->applyRotation({ 5 }, degrees);
    settle();

    const QRectF shown = asTheFileTurnsIt(fieldRect(), degrees);
    QVERIFY(!shown.isEmpty());

    design.press(5, shown.center(), Qt::LeftButton);
    QVERIFY2(design.chosenCount() == 1,
             qPrintable(QStringLiteral("the field cannot be taken hold of at %1, where the turned sheet draws it")
                            .arg(describe(shown))));

    // Ten points to the right of where the user can see it, which is the whole
    // of what the gesture means and the only thing that has to survive.
    design.nudge(10.0, 0.0);

    const FormDesignOverlay::Work work = design.pendingWork();
    QCOMPARE(work.changed.size(), 1);
    QCOMPARE(work.changed.at(0).page, 5);

    PageProcessor processor(m_document.get(), &m_backend, m_view.get());
    const QVector<FormField> written = writeAndReadBack(design, processor);
    const int zeta = indexOfField(written, QStringLiteral("Zeta"));
    QVERIFY2(zeta >= 0, "the field did not survive being written back at all");
    QCOMPARE(written.at(zeta).page, 5);

    const QRectF wanted = shown.translated(10.0, 0.0);
    QVERIFY2(sameBox(written.at(zeta).rect, wanted),
             qPrintable(QStringLiteral("the field was written at %1, and the user put it down at %2")
                            .arg(describe(written.at(zeta).rect), describe(wanted))));
}

void TestOverlayRows::aFieldDrawnOnATurnedPageIsWrittenWhereItWasDrawn_data()
{
    everyTurn();
}

void TestOverlayRows::aFieldDrawnOnATurnedPageIsWrittenWhereItWasDrawn()
{
    QFETCH(int, degrees);

    openMergedDocument();
    m_view->setMode(PageView::Mode::Edit);

    FormDesignOverlay design(m_view.get());
    design.setDocument(m_document.get());
    m_view->addOverlay(&design);
    design.setSource(m_document->filePath());
    design.setFields(Forms::read(m_document->filePath(), nullptr));

    m_document->applyRotation({ 6 }, degrees);
    settle();

    // Clear of the sheet's own field at every turn, so that the drag draws a new
    // box rather than taking hold of the one that is already there.
    const QRectF drawn(80.0, 120.0, 140.0, 30.0);
    design.setKindToPlace(FormDesignOverlay::Kind::Text);
    QVERIFY(design.press(6, drawn.bottomLeft(), Qt::LeftButton));
    design.move(6, drawn.center(), Qt::LeftButton);
    design.release(6, drawn.topRight());

    const FormDesignOverlay::Work work = design.pendingWork();
    QCOMPARE(work.added.size(), 1);

    PageProcessor processor(m_document.get(), &m_backend, m_view.get());
    const QVector<FormField> written = writeAndReadBack(design, processor);
    const int at = indexOfField(written, work.added.at(0).name);
    QVERIFY2(at >= 0, "the field that was drawn did not survive being written at all");

    QVERIFY2(sameBox(written.at(at).rect, drawn),
             qPrintable(QStringLiteral("the field was written at %1, and it was drawn at %2")
                            .arg(describe(written.at(at).rect), describe(drawn))));
}

void TestOverlayRows::turningASheetTurnsTheFieldWaitingOnIt_data()
{
    everyTurn();
}

void TestOverlayRows::turningASheetTurnsTheFieldWaitingOnIt()
{
    QFETCH(int, degrees);

    openMergedDocument();
    m_view->setMode(PageView::Mode::Edit);

    FormDesignOverlay design(m_view.get());
    design.setDocument(m_document.get());
    m_view->addOverlay(&design);
    design.setSource(m_document->filePath());
    design.setFields(Forms::read(m_document->filePath(), nullptr));

    // Drawn on the upright sheet, and only then turned: a box is stuck to the
    // piece of paper it was drawn on, not to the corner of the screen.
    const QRectF drawn(80.0, 120.0, 140.0, 30.0);
    design.setKindToPlace(FormDesignOverlay::Kind::Text);
    QVERIFY(design.press(6, drawn.bottomLeft(), Qt::LeftButton));
    design.move(6, drawn.center(), Qt::LeftButton);
    design.release(6, drawn.topRight());
    QCOMPARE(design.pendingWork().added.size(), 1);

    m_document->applyRotation({ 6 }, degrees);
    settle();

    const QRectF wanted = asTheFileTurnsIt(drawn, degrees);
    QVERIFY(!wanted.isEmpty());

    const FormDesignOverlay::Work work = design.pendingWork();
    QCOMPARE(work.added.size(), 1);
    QVERIFY2(sameBox(work.added.at(0).rect, wanted),
             qPrintable(QStringLiteral("the box was left at %1 while its sheet turned under it, and the paper it was "
                                       "drawn on is now at %2")
                            .arg(describe(work.added.at(0).rect), describe(wanted))));
}

// ── E. What the page draws ────────────────────────────────────────────────

void TestOverlayRows::objectHandlesComeFromThePageUnderThePointer()
{
    QString error;
    QVERIFY2(m_document->open(m_stamped, &error), qPrintable(error));
    QVERIFY2(m_document->importFile(m_blank, -1, &error), qPrintable(error));
    QCOMPARE(m_document->pageCount(), 9);
    m_view->setMode(PageView::Mode::Edit);

    ObjectOverlay objects(m_view.get());
    objects.setDocument(m_document.get());
    m_view->addOverlay(&objects);
    objects.setSource(m_document->filePath());

    // The blank pages are the last two, and they correctly offer nothing.
    QVERIFY2(!objects.press(7, onTheStamp(), Qt::LeftButton), "a page with nothing drawn on it offered a handle");
    QVERIFY(objects.chosenObjects().isEmpty());

    m_document->removePages({ 0 });
    settle();
    QCOMPARE(m_document->pageCount(), 8);

    // The same spot on the same sheet, which is now row six.
    QVERIFY2(!objects.press(6, onTheStamp(), Qt::LeftButton),
             "the blank page offered a handle on another page's text, read out of the file at the row number");
    QVERIFY(objects.chosenObjects().isEmpty());

    // And the pages that do carry something still give up the right something:
    // row zero shows what was the second sheet, stamped "PSPAGE 2".
    QVERIFY(objects.press(0, onTheStamp(), Qt::LeftButton));
    QCOMPARE(objects.chosenPage(), 0);
    QCOMPARE(objects.chosenObjects().size(), 1);
    Inspectable *described = objects.inspected();
    QVERIFY(described);
    QVERIFY2(described->description().contains(QStringLiteral("PSPAGE 2")),
             qPrintable(QStringLiteral("the handle is on the wrong page's text: %1").arg(described->description())));
}

// ── F. Work that must survive the pages moving ────────────────────────────

void TestOverlayRows::turningAPageKeepsTheCommentsMadeOnTheOthers()
{
    QString error;
    QVERIFY2(m_document->open(m_stamped, &error), qPrintable(error));
    QVERIFY2(m_document->importFile(m_blank, -1, &error), qPrintable(error));
    m_view->setMode(PageView::Mode::Edit);

    AnnotationOverlay marks(m_view.get(), m_document.get());
    m_view->addOverlay(&marks);
    marks.setTool(AnnotationOverlay::Tool::Rectangle);

    marks.press(0, QPointF(120, 300), Qt::LeftButton);
    marks.move(0, QPointF(300, 400), Qt::LeftButton);
    marks.release(0, QPointF(400, 500));
    QVERIFY2(marks.isModified(), "a box drawn on the first page was not recorded as a comment");
    QCOMPARE(marks.annotationsByRow().value(0).size(), 1);

    QSignalSpy refusals(&marks, &AnnotationOverlay::refused);
    QVERIFY(refusals.isValid());

    // Turning the fourth sheet has nothing to do with the first one.
    m_document->applyRotation({ 3 }, 90);
    settle();

    QVERIFY2(marks.isModified(), "turning another page threw the comment away");
    QCOMPARE(marks.annotationsByRow().value(0).size(), 1);
    QVERIFY2(refusals.isEmpty(), "nothing was lost, so nothing should have been announced");
}

void TestOverlayRows::movingPagesCarriesTheirCommentsAlong()
{
    QString error;
    QVERIFY2(m_document->open(m_first, &error), qPrintable(error));
    QVERIFY2(m_document->importFile(m_second, -1, &error), qPrintable(error));
    m_view->setMode(PageView::Mode::Edit);

    AnnotationOverlay marks(m_view.get(), m_document.get());
    m_view->addOverlay(&marks);
    marks.setTool(AnnotationOverlay::Tool::Rectangle);

    // On the seventh sheet, which belongs to the second file.
    marks.press(6, QPointF(120, 300), Qt::LeftButton);
    marks.move(6, QPointF(300, 400), Qt::LeftButton);
    marks.release(6, QPointF(400, 500));
    QCOMPARE(marks.annotationsByRow().value(6).size(), 1);

    QSignalSpy refusals(&marks, &AnnotationOverlay::refused);
    deleteAndReorder();

    QVERIFY2(marks.isModified(), "moving pages about threw a comment away that was made on one of them");
    QVERIFY2(marks.annotationsByRow().value(6).isEmpty(), "the comment stayed at the row number rather than the page");
    QCOMPARE(marks.annotationsByRow().value(3).size(), 1);
    QCOMPARE(marks.annotationsByRow().value(3).at(0).page, 3);
    QVERIFY(refusals.isEmpty());
}

void TestOverlayRows::turningAPageKeepsTheCorrectionsTypedOnTheOthers()
{
    QString error;
    QVERIFY2(m_document->open(m_stamped, &error), qPrintable(error));
    QVERIFY2(m_document->importFile(m_blank, -1, &error), qPrintable(error));
    m_view->setMode(PageView::Mode::Edit);

    TextOverlay text(m_view.get(), m_document.get());
    m_view->addOverlay(&text);

    QVERIFY2(text.press(0, onTheStamp(), Qt::LeftButton), "clicking the stamped line opened no caret in it");
    QCOMPARE(text.editedRow(), 0);

    auto *editor = m_view->viewport()->findChild<QLineEdit *>();
    QVERIFY2(editor, "the caret opened without anything to type into");
    // Typed rather than set: the overlay listens for what a person did to the
    // box, which is the only thing that counts as a correction.
    QTest::keyClicks(editor, QStringLiteral("PSTYPED"));
    text.finishEditing();

    QVERIFY2(text.hasEdits(), "what was typed was not recorded");
    QCOMPARE(text.replacements().size(), 1);
    QCOMPARE(text.replacements().at(0).page, 0);

    QSignalSpy refusals(&text, &TextOverlay::refused);
    QVERIFY(refusals.isValid());

    m_document->applyRotation({ 3 }, 90);
    settle();

    QVERIFY2(text.hasEdits(), "turning another page threw the correction away");
    QCOMPARE(text.replacements().size(), 1);
    QCOMPARE(text.replacements().at(0).page, 0);
    QVERIFY(refusals.isEmpty());

    // And it travels with its page rather than staying at its row number.
    m_document->movePages({ 0 }, 5);
    settle();
    QCOMPARE(text.replacements().size(), 1);
    QCOMPARE(text.replacements().at(0).page, 4);
}

void TestOverlayRows::deletingAPageSaysWhatItTookWithIt()
{
    QString error;
    QVERIFY2(m_document->open(m_stamped, &error), qPrintable(error));
    m_view->setMode(PageView::Mode::Edit);

    AnnotationOverlay marks(m_view.get(), m_document.get());
    m_view->addOverlay(&marks);
    marks.setTool(AnnotationOverlay::Tool::Rectangle);
    marks.press(2, QPointF(120, 300), Qt::LeftButton);
    marks.move(2, QPointF(300, 400), Qt::LeftButton);
    marks.release(2, QPointF(400, 500));
    QVERIFY(marks.isModified());

    TextOverlay text(m_view.get(), m_document.get());
    m_view->addOverlay(&text);
    QVERIFY(text.press(2, onTheStamp(), Qt::LeftButton));
    auto *editor = m_view->viewport()->findChild<QLineEdit *>();
    QVERIFY(editor);
    QTest::keyClicks(editor, QStringLiteral("PSTYPED"));
    text.finishEditing();
    QVERIFY(text.hasEdits());

    QSignalSpy comments(&marks, &AnnotationOverlay::refused);
    QSignalSpy corrections(&text, &TextOverlay::refused);
    QVERIFY(comments.isValid());
    QVERIFY(corrections.isValid());

    // The page the work was done on, taken out from under it. This is the one
    // case that cannot be followed, and the one the user has to be told about.
    m_document->removePages({ 2 });
    settle();

    QVERIFY2(!marks.isModified(), "the comment survived the page it was made on being deleted");
    QCOMPARE(comments.size(), 1);
    QVERIFY2(!comments.at(0).at(0).toString().isEmpty(), "the comment vanished with an empty explanation");

    QVERIFY2(!text.hasEdits(), "the correction survived the page it was typed on being deleted");
    QCOMPARE(corrections.size(), 1);
    QVERIFY2(!corrections.at(0).at(0).toString().isEmpty(), "the correction vanished with an empty explanation");
}

QTEST_MAIN(TestOverlayRows)

#include "tst_overlayrows.moc"
