/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "TestPdf.h"

#include "core/Document.h"
#include "core/PageRange.h"
#include "core/RenderBackend.h"
#include "core/commands/PageCommands.h"
#include "ui/DocumentCommands.h"
#include "ui/DocumentProperties.h"
#include "ui/Inspector.h"
#include "ui/InspectorDock.h"

#include <QApplication>
#include <QDateTimeEdit>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QLocale>
#include <QPushButton>
#include <QSignalSpy>
#include <QStringListModel>
#include <QTemporaryDir>
#include <QTest>
#include <QUndoStack>

#include <KLocalizedString>

using namespace ps;

namespace {

/**
 * A rasteriser that draws nothing and says which file and page it was asked for.
 *
 * The panel counts words by asking the backend for a page's text, and the whole
 * question worth testing is *which* page it asks about, so the text it hands
 * back is the (file, page) pair spelled out. A document merged from two files
 * and then shuffled reports the wrong file's page from anything that mistakes a
 * view row for a page number, and with one unedited file the two agree and the
 * mistake is invisible.
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

    QString extractText(int sourceId, int page) override
    {
        return QStringLiteral("source %1 page %2").arg(sourceId).arg(page);
    }

    QStringList wordsInside(int, int, const QRectF &) override { return {}; }

    QVector<Word> words(int, int) override { return {}; }

private:
    QHash<int, QString> m_paths;
};

/** Every piece of text the panel put on screen. */
QStringList said(QWidget *panel)
{
    QStringList lines;
    const QList<QLabel *> labels = panel->findChildren<QLabel *>();
    for (const QLabel *label : labels) {
        lines.append(label->text());
    }
    return lines;
}

/** True when some label on the panel contains @p fragment. */
bool mentions(QWidget *panel, const QString &fragment)
{
    const QStringList lines = said(panel);
    for (const QString &line : lines) {
        if (line.contains(fragment)) {
            return true;
        }
    }
    return false;
}

/** The line edit whose current text is @p value, or nothing. */
QLineEdit *editHolding(QWidget *panel, const QString &value)
{
    const QList<QLineEdit *> edits = panel->findChildren<QLineEdit *>();
    for (QLineEdit *edit : edits) {
        if (edit->text() == value) {
            return edit;
        }
    }
    return nullptr;
}

/**
 * A width and height as this locale writes them, which is what the panel shows.
 *
 * Follows the same rule as the panel rather than assuming millimetres: the suite
 * runs under C and de_DE, both metric, but a developer's own shell may not be,
 * and a test that fails only on an American desktop is a test nobody trusts.
 */
QString paperSize(double widthPoints, double heightPoints)
{
    if (QLocale().measurementSystem() != QLocale::MetricSystem) {
        return QStringLiteral("%1 × %2 in")
            .arg(QLocale().toString(widthPoints / 72.0, 'f', 2), QLocale().toString(heightPoints / 72.0, 'f', 2));
    }
    const double perMillimetre = 72.0 / 25.4;
    return QStringLiteral("%1 × %2 mm")
        .arg(QLocale().toString(widthPoints / perMillimetre, 'f', 1),
             QLocale().toString(heightPoints / perMillimetre, 'f', 1));
}

} // namespace

/**
 * The properties panel when nothing on the page is selected.
 *
 * Two answers are under test and one distinction runs through both. The panel
 * describes either the document or the pages picked out in the organiser, and in
 * the second case every number it shows has to have come from the page of the
 * file the row actually points at, which is a different page as soon as the
 * document is two files with something deleted between them, and the same page
 * for as long as it is one file untouched. So the merged, shuffled fixture is
 * the one the page cases run against; with a plain document they would all pass
 * while addressing the wrong sheet.
 */
class TestDocumentPropertiesPanel : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void init();
    void cleanup();

    // ── the document ──────────────────────────────────────────────────────
    void describesTheDocumentWhenNothingIsChosen();
    void editingATitleIsOneUndoableStep();
    void aDateThatWasNeverRecordedStaysUnrecorded();
    void fillsThePanelItWasGiven();
    void undoPutsTheOldTitleBackInItsBox();

    // ── the pages ─────────────────────────────────────────────────────────
    void describesTheChosenPages();
    void namesThePageInsideItsOwnFile();
    void saysWhenTheChosenPagesDisagree();
    void saysWhenABoxIsAbsentRatherThanGuessingIt();
    void countsAcrossTheWholeSelection();
    void aTurnedPageIsMeasuredAsItIsShown();

    // ── choosing between the two ──────────────────────────────────────────
    void rowsLeftBehindStopCountingWhenTheGridGoesAway();

    // ── the dear half ─────────────────────────────────────────────────────
    void countsWordsOffTheGuiThread();
    void namesTheTypefacesThatAreNotEmbedded();

private:
    /** One file, four pages, US Letter: the fixture for everything about the document. */
    void openPlain();

    /**
     * Two files with pages deleted and reordered: the fixture for everything about pages.
     *
     * Row 0 shows page 5 of the four-page Letter file, row 1 page 0 of the A4
     * one, and so on: no row is its own page number in either file.
     */
    void openMerged();

    QTemporaryDir m_dir;
    QString m_letter;
    QString m_a4;
    QString m_turned;

    SayingBackend m_backend;
    Document *m_document = nullptr;
    QStringListModel *m_model = nullptr;
    QListView *m_grid = nullptr;
    DocumentProperties *m_properties = nullptr;
};

void TestDocumentPropertiesPanel::initTestCase()
{
    KLocalizedString::setApplicationDomain("pdf-smithy");
    QVERIFY(m_dir.isValid());

    m_letter = m_dir.filePath(QStringLiteral("letter.pdf"));
    m_a4 = m_dir.filePath(QStringLiteral("a4.pdf"));
    m_turned = m_dir.filePath(QStringLiteral("turned.pdf"));
    QVERIFY(test::writeSamplePdf(m_letter, 8));
    QVERIFY(test::writeSamplePdf(m_a4, 4, QSizeF(595.276, 841.89)));
    QVERIFY(test::writeRotatedPdf(m_turned, 2, 90));
}

void TestDocumentPropertiesPanel::init()
{
    m_document = new Document(this);
    m_document->setRenderBackend(&m_backend);

    m_model = new QStringListModel(this);
    m_grid = new QListView;
    m_grid->setModel(m_model);
    m_grid->setSelectionMode(QAbstractItemView::ExtendedSelection);
}

void TestDocumentPropertiesPanel::cleanup()
{
    delete m_properties;
    m_properties = nullptr;
    delete m_grid;
    m_grid = nullptr;
    delete m_document;
    m_document = nullptr;
}

void TestDocumentPropertiesPanel::openPlain()
{
    QString error;
    QVERIFY2(m_document->open(m_letter, &error), qPrintable(error));

    QStringList rows;
    for (int i = 0; i < m_document->pageCount(); ++i) {
        rows << QString::number(i);
    }
    m_model->setStringList(rows);

    m_grid->show();
    QVERIFY(QTest::qWaitForWindowExposed(m_grid));
    m_properties = new DocumentProperties(m_document, m_grid, nullptr, this);
}

void TestDocumentPropertiesPanel::openMerged()
{
    QString error;
    QVERIFY2(m_document->open(m_letter, &error), qPrintable(error));
    QVERIFY2(m_document->importFile(m_a4, -1, &error), qPrintable(error));

    // Away with the first five Letter pages, so that no row anywhere in what is
    // left is also its own page number inside the file it came from.
    m_document->removePages({ 0, 1, 2, 3, 4 });

    QStringList rows;
    for (int i = 0; i < m_document->pageCount(); ++i) {
        rows << QString::number(i);
    }
    m_model->setStringList(rows);

    m_grid->show();
    QVERIFY(QTest::qWaitForWindowExposed(m_grid));
    m_properties = new DocumentProperties(m_document, m_grid, nullptr, this);

    // Row 0 is page 5 of the Letter file and row 3 is page 0 of the A4 one.
    QCOMPARE(m_document->pageCount(), 7);
    QCOMPARE(m_document->pageAt(0).sourceId, 0);
    QCOMPARE(m_document->pageAt(0).sourcePage, 5);
    QCOMPARE(m_document->pageAt(3).sourceId, 1);
    QCOMPARE(m_document->pageAt(3).sourcePage, 0);
}

// ── The document ──────────────────────────────────────────────────────────

void TestDocumentPropertiesPanel::describesTheDocumentWhenNothingIsChosen()
{
    openPlain();

    Inspectable *thing = m_properties->inspected();
    QVERIFY(thing);
    QVERIFY(m_properties->chosenRows().isEmpty());
    QCOMPARE(thing->description(), QStringLiteral("letter.pdf"));

    std::unique_ptr<QWidget> panel(thing->buildEditor(nullptr));
    QVERIFY(panel);

    // The page count is the thing somebody opens this panel for first, and it
    // has to be the document's own count rather than any one file's.
    QVERIFY(mentions(panel.get(), QLocale().toString(8)));
    QVERIFY(mentions(panel.get(), QStringLiteral("1.")));
}

void TestDocumentPropertiesPanel::editingATitleIsOneUndoableStep()
{
    openPlain();
    m_document->undoStack()->clear();

    std::unique_ptr<QWidget> panel(m_properties->inspected()->buildEditor(nullptr));
    QLineEdit *title = editHolding(panel.get(), QString());
    QVERIFY(title);

    title->setText(QStringLiteral("Mietvertrag"));
    Q_EMIT title->editingFinished();

    QCOMPARE(m_document->metadata().title, QStringLiteral("Mietvertrag"));

    // Through the undo stack rather than around it, and as *one* step: the
    // dialog and this panel write the same thing, and two paths to one field is
    // how one of them quietly stops being undoable.
    QCOMPARE(m_document->undoStack()->count(), 1);
    m_document->undoStack()->undo();
    QCOMPARE(m_document->metadata().title, QString());
}

void TestDocumentPropertiesPanel::aDateThatWasNeverRecordedStaysUnrecorded()
{
    openPlain();
    QVERIFY(!m_document->metadata().created.isValid());
    m_document->undoStack()->clear();

    std::unique_ptr<QWidget> panel(m_properties->inspected()->buildEditor(nullptr));
    const QList<QDateTimeEdit *> stamps = panel->findChildren<QDateTimeEdit *>();
    QCOMPARE(stamps.size(), 2);

    // Merely looking at a document must not date it. Every date editor has a
    // value whether or not the document has one, so without the "not recorded"
    // floor the panel would stamp a creation time on everything it is opened on.
    for (QDateTimeEdit *stamp : stamps) {
        Q_EMIT stamp->editingFinished();
    }
    QVERIFY(!m_document->metadata().created.isValid());
    QVERIFY(!m_document->metadata().modified.isValid());
    QCOMPARE(m_document->undoStack()->count(), 0);
}

void TestDocumentPropertiesPanel::fillsThePanelItWasGiven()
{
    QString error;
    QVERIFY2(m_document->open(m_letter, &error), qPrintable(error));
    m_model->setStringList({ QStringLiteral("0") });

    auto *dock = new InspectorDock;
    m_properties = new DocumentProperties(m_document, m_grid, dock, this);

    // The whole complaint was an empty panel, so the case worth having is that
    // registering the source is all the window has to do.
    QVERIFY(dock->findChild<QLabel *>());
    QVERIFY(mentions(dock, QStringLiteral("letter.pdf")));

    delete dock;
}

void TestDocumentPropertiesPanel::undoPutsTheOldTitleBackInItsBox()
{
    QString error;
    QVERIFY2(m_document->open(m_letter, &error), qPrintable(error));
    m_model->setStringList({ QStringLiteral("0") });

    auto *dock = new InspectorDock;
    m_properties = new DocumentProperties(m_document, m_grid, dock, this);

    Metadata::Fields fields = m_document->metadata();
    fields.title = QStringLiteral("Mietvertrag");
    m_document->undoStack()->push(new SetMetadataCommand(m_document, fields));
    QVERIFY(editHolding(dock, QStringLiteral("Mietvertrag")));

    // The panel is a view of the document, not a second copy of it. Walking the
    // change back has to walk the box back with it, or the next thing typed
    // anywhere else on the panel writes the undone title in again.
    m_document->undoStack()->undo();
    QVERIFY(!editHolding(dock, QStringLiteral("Mietvertrag")));
    QCOMPARE(m_document->metadata().title, QString());

    delete dock;
}

// ── The pages ─────────────────────────────────────────────────────────────

void TestDocumentPropertiesPanel::describesTheChosenPages()
{
    openMerged();

    m_grid->selectionModel()->select(m_model->index(0, 0), QItemSelectionModel::Select);
    QTRY_COMPARE(m_properties->chosenRows(), QVector<int>({ 0 }));

    Inspectable *thing = m_properties->inspected();
    QVERIFY(thing);
    QCOMPARE(thing->description(), PageRange::format({ 0 }));

    std::unique_ptr<QWidget> panel(thing->buildEditor(nullptr));
    QVERIFY(mentions(panel.get(), paperSize(612.0, 792.0)));
    QVERIFY(mentions(panel.get(), QStringLiteral("Letter")));

    // A range rather than a list of eight numbers, and the same notation the
    // print dialog and the command line already use.
    m_grid->selectionModel()->select(QItemSelection(m_model->index(1, 0), m_model->index(3, 0)),
                                     QItemSelectionModel::Select);
    QTRY_COMPARE(m_properties->chosenRows().size(), 4);
    QCOMPARE(m_properties->inspected()->description(), PageRange::format({ 0, 1, 2, 3 }));
}

void TestDocumentPropertiesPanel::namesThePageInsideItsOwnFile()
{
    openMerged();

    m_grid->selectionModel()->select(m_model->index(3, 0), QItemSelectionModel::Select);
    QTRY_COMPARE(m_properties->chosenRows(), QVector<int>({ 3 }));

    const QVector<PageFacts> &facts = m_properties->pageFacts();
    QCOMPARE(facts.size(), 1);

    // Row 3 of the document is page 0 of the second file. Anything that took the
    // row for a page number would read the sixth page of the first file here and
    // report US Letter for an A4 sheet.
    QCOMPARE(facts.first().row, 3);
    QCOMPARE(facts.first().page.sourceId(), 1);
    QCOMPARE(facts.first().page.pageInFile(), 0);
    QCOMPARE(facts.first().file, m_a4);
    QCOMPARE(qRound(facts.first().sizePoints.width()), 595);

    std::unique_ptr<QWidget> panel(m_properties->inspected()->buildEditor(nullptr));
    QVERIFY(mentions(panel.get(), QStringLiteral("A4")));
    // And it says which page of which file, because a merged document has two
    // answers to "which page is this" and only one of them is on the screen.
    QVERIFY(mentions(panel.get(), QStringLiteral("a4.pdf")));
    QVERIFY(mentions(panel.get(), QLocale().toString(1)));
}

void TestDocumentPropertiesPanel::saysWhenTheChosenPagesDisagree()
{
    openMerged();

    // Row 0 is a Letter page and row 3 an A4 one.
    m_grid->selectionModel()->select(m_model->index(0, 0), QItemSelectionModel::Select);
    m_grid->selectionModel()->select(m_model->index(3, 0), QItemSelectionModel::Select);
    QTRY_COMPARE(m_properties->chosenRows(), QVector<int>({ 0, 3 }));

    std::unique_ptr<QWidget> panel(m_properties->inspected()->buildEditor(nullptr));

    // Neither of the two sizes may be shown as though it were everyone's.
    QVERIFY(!mentions(panel.get(), paperSize(612.0, 792.0)));
    QVERIFY(!mentions(panel.get(), paperSize(595.276, 841.89)));
    QVERIFY(
        mentions(panel.get(),
                 i18nc("@info a property whose value is not the same on every chosen page", "differs between pages")));

    // And the weight is a total, so it is more than either page alone.
    const QVector<PageFacts> &facts = m_properties->pageFacts();
    QCOMPARE(facts.size(), 2);
    QVERIFY(facts.at(0).bytes > 0);
    QVERIFY(facts.at(1).bytes > 0);
}

void TestDocumentPropertiesPanel::saysWhenABoxIsAbsentRatherThanGuessingIt()
{
    openPlain();

    m_grid->selectionModel()->select(m_model->index(2, 0), QItemSelectionModel::Select);
    QTRY_COMPARE(m_properties->chosenRows(), QVector<int>({ 2 }));

    const PageFacts &facts = m_properties->pageFacts().first();
    QVERIFY(facts.boxes.media.isValid());

    // The fixture carries a /MediaBox and nothing else. A panel that filled the
    // other four in from what the specification says they fall back to would
    // make every document look ready for a press.
    QVERIFY(!facts.boxes.crop.isValid());
    QVERIFY(!facts.boxes.trim.isValid());
    QVERIFY(!facts.boxes.bleed.isValid());
    QVERIFY(!facts.boxes.art.isValid());

    std::unique_ptr<QWidget> panel(m_properties->inspected()->buildEditor(nullptr));
    QVERIFY(mentions(panel.get(), i18nc("@info the page does not carry this box at all", "Not set")));

    // And several pages that all lack the same box agree about it. Pages with
    // nothing to say answer the same nothing, and reading that as disagreement
    // reported five boxes as differing across a document whose pages were
    // identical.
    m_grid->selectionModel()->select(m_model->index(3, 0), QItemSelectionModel::Select);
    QTRY_COMPARE(m_properties->chosenRows(), QVector<int>({ 2, 3 }));

    std::unique_ptr<QWidget> both(m_properties->inspected()->buildEditor(nullptr));
    QVERIFY(mentions(both.get(), i18nc("@info the page does not carry this box at all", "Not set")));
    QVERIFY(
        !mentions(both.get(),
                  i18nc("@info a property whose value is not the same on every chosen page", "differs between pages")));
}

void TestDocumentPropertiesPanel::countsAcrossTheWholeSelection()
{
    openMerged();

    m_grid->selectAll();
    QTRY_COMPARE(m_properties->chosenRows().size(), 7);

    qint64 total = 0;
    for (const PageFacts &page : m_properties->pageFacts()) {
        QVERIFY(page.isValid());
        total += page.bytes;
    }
    QVERIFY(total > 0);

    // Every row is described, in row order, and each from its own file.
    QCOMPARE(m_properties->pageFacts().first().row, 0);
    QCOMPARE(m_properties->pageFacts().last().row, 6);
    QCOMPARE(m_properties->pageFacts().last().page.sourceId(), 1);
}

void TestDocumentPropertiesPanel::aTurnedPageIsMeasuredAsItIsShown()
{
    QString error;
    QVERIFY2(m_document->open(m_turned, &error), qPrintable(error));
    m_model->setStringList({ QStringLiteral("0"), QStringLiteral("1") });
    m_grid->show();
    QVERIFY(QTest::qWaitForWindowExposed(m_grid));
    m_properties = new DocumentProperties(m_document, m_grid, nullptr, this);

    m_grid->selectionModel()->select(m_model->index(0, 0), QItemSelectionModel::Select);
    QTRY_COMPARE(m_properties->chosenRows(), QVector<int>({ 0 }));

    // The file already turns the page a quarter, and the organiser adds another
    // quarter on top. Both belong in the answer, because what the reader is
    // looking at is the sum of the two, and at half a turn the page is upright
    // again and 612 wide, which is the case that catches a panel adding only one
    // of them.
    m_document->applyRotation({ 0 }, 90);
    QTRY_VERIFY(m_properties->pageFacts().size() == 1 && m_properties->pageFacts().first().rotation == 180);
    QCOMPARE(qRound(m_properties->pageFacts().first().sizePoints.width()), 612);

    m_document->applyRotation({ 0 }, 90);
    QTRY_VERIFY(m_properties->pageFacts().size() == 1 && m_properties->pageFacts().first().rotation == 270);
    QCOMPARE(qRound(m_properties->pageFacts().first().sizePoints.width()), 792);
}

// ── Choosing between the two ──────────────────────────────────────────────

void TestDocumentPropertiesPanel::rowsLeftBehindStopCountingWhenTheGridGoesAway()
{
    openPlain();

    m_grid->selectionModel()->select(m_model->index(1, 0), QItemSelectionModel::Select);
    QTRY_COMPARE(m_properties->chosenRows(), QVector<int>({ 1 }));

    // Going back to reading leaves the organiser's selection exactly where it
    // was. The panel must go back to describing the document all the same: rows
    // nobody can see are not what the reader is looking at, and that was the
    // whole of the complaint about Read mode.
    m_grid->hide();
    QTRY_VERIFY(m_properties->chosenRows().isEmpty());
    QCOMPARE(m_properties->inspected()->kindName(),
             i18nc("@title the kind of thing the properties panel is showing", "Document"));

    m_grid->show();
    QVERIFY(QTest::qWaitForWindowExposed(m_grid));
    QTRY_COMPARE(m_properties->chosenRows(), QVector<int>({ 1 }));
}

// ── The dear half ─────────────────────────────────────────────────────────

void TestDocumentPropertiesPanel::countsWordsOffTheGuiThread()
{
    openMerged();

    m_grid->selectionModel()->select(m_model->index(3, 0), QItemSelectionModel::Select);
    QTRY_COMPARE(m_properties->chosenRows(), QVector<int>({ 3 }));

    QSignalSpy counted(m_properties, &DocumentProperties::countingChanged);
    m_properties->startCounting();
    QVERIFY(counted.wait(30000));

    const DeepFacts &deep = m_properties->deepFacts();
    QVERIFY(deep.done);

    // "source 1 page 0": four words, from the second file's first page. A panel
    // that handed the row number to the backend would have asked for page 3.
    QCOMPARE(deep.words, 4);
    QCOMPARE(deep.characters, QStringLiteral("source1page0").size());

    // And the content stream really was read, not guessed at: the fixture draws
    // one run of text and nothing else.
    QCOMPARE(deep.textRuns, 1);
    QCOMPARE(deep.pictures, 0);
}

void TestDocumentPropertiesPanel::namesTheTypefacesThatAreNotEmbedded()
{
    openPlain();

    // Nothing chosen, so this is the document's own count: one read of the font
    // inventory per file, however many pages the document has.
    QSignalSpy counted(m_properties, &DocumentProperties::countingChanged);
    QVERIFY(counted.wait(30000));

    const DeepFacts &deep = m_properties->deepFacts();
    QVERIFY(deep.done);
    QCOMPARE(deep.fontFamilies, 1);
    QCOMPARE(deep.notEmbedded, QStringList { QStringLiteral("Helvetica") });

    // Named on the panel, because a count alone leaves somebody opening the font
    // tool to find out which typeface will be substituted at the far end.
    std::unique_ptr<QWidget> panel(m_properties->inspected()->buildEditor(nullptr));
    QVERIFY(mentions(panel.get(), QStringLiteral("Helvetica")));
}

QTEST_MAIN(TestDocumentPropertiesPanel)

#include "tst_documentproperties_panel.moc"
