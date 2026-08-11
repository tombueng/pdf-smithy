/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "TestPdf.h"

#include "core/Document.h"
#include "core/Encryption.h"
#include "core/Metadata.h"
#include "core/Source.h"
#include "ui/MainWindow.h"
#include "ui/PageGridView.h"
#include "ui/PageModel.h"
#include "ui/PageView.h"

#include <QAbstractItemModel>
#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QDockWidget>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QHash>
#include <QLineEdit>
#include <QMimeData>
#include <QSignalSpy>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QFile>
#include <QTest>

#include <KActionCollection>
#include <KLocalizedString>
#include <KMessageWidget>
#include <KPasswordDialog>

using namespace ps;

namespace {

/**
 * Types a password into each password dialog that appears, then gives up.
 *
 * The window asks for as long as it is refused, so a fixed answer would keep a
 * wrong one going round for ever. Handing out a list and cancelling once it is
 * empty is both what a person does and what makes "it asked three times" an
 * assertion rather than a guess.
 */
class PasswordTyper
{
public:
    explicit PasswordTyper(const QStringList &answers)
        : m_answers(answers)
    {
        m_timer.setInterval(5);
        QObject::connect(&m_timer, &QTimer::timeout, [this] {
            auto *dialog = qobject_cast<KPasswordDialog *>(QApplication::activeModalWidget());
            if (!dialog) {
                return;
            }
            ++m_asked;
            if (m_answers.isEmpty()) {
                dialog->reject();
                return;
            }
            dialog->setPassword(m_answers.takeFirst());
            dialog->accept();
        });
        m_timer.start();
    }

    /** How many times the window put the question up. */
    int asked() const { return m_asked; }

    PasswordTyper(const PasswordTyper &) = delete;
    PasswordTyper &operator=(const PasswordTyper &) = delete;

private:
    QTimer m_timer;
    QStringList m_answers;
    int m_asked = 0;
};

/** Fills the first line of whatever dialog is up with @p text and accepts it. */
class DialogFiller
{
public:
    explicit DialogFiller(const QString &text)
        : m_text(text)
    {
        m_timer.setInterval(5);
        QObject::connect(&m_timer, &QTimer::timeout, [this] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!dialog) {
                return;
            }
            const QList<QLineEdit *> lines = dialog->findChildren<QLineEdit *>();
            if (!lines.isEmpty()) {
                lines.constFirst()->setText(m_text);
            }
            dialog->accept();
        });
        m_timer.start();
    }

    DialogFiller(const DialogFiller &) = delete;
    DialogFiller &operator=(const DialogFiller &) = delete;

private:
    QTimer m_timer;
    QString m_text;
};

} // namespace

/**
 * Drives the real window through the real actions.
 *
 * The unit tests already prove the engine is right; what this one proves is
 * that the wiring is: that clicking Rotate reaches the undo stack, that
 * deleting updates the grid, and that Save puts the result on disk. It runs
 * headless, so it works in CI exactly as it does here.
 */
class TestMainWindow : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void init();
    void cleanup();

    void opensADocument();
    void survivesABrokenFile();
    void startsANewDocumentWithOneBlankPage();
    void addsBlankPagesBesideThePageChosen();

    void rotatesTheSelection();
    void deletesTheSelection();
    void duplicatesTheSelection();
    void undoesEverything();

    void savesToDisk();
    void marksUnsavedChangesInTheTitle();
    void disablesActionsWithoutASelection();
    void deletingPagesIsRefusedWhileReading();
    void turningTheWholeDocumentSaysThatIsWhatItDid();
    void theReadingKeysAreLeftToTheView();
    void documentPropertiesAreUndoneOnTheirOwn();
    void cleaningUpBelongsToTheDocumentItWasAskedFor();

    void opensALockedDocumentOnceThePasswordIsGiven();
    void aWrongPasswordSaysSoAndCanBeGivenUpOn();
    void savingKeepsTheDocumentLocked();

    void zoomChangesThumbnailSize();
    void fitsPagesToTheWindow();
    void editsCommentsInTheWindow();
    void everyPageEditorOpensInTheWindow_data();
    void everyPageEditorOpensInTheWindow();
    void hasNoAmbiguousShortcuts();
    void offersEveryDocumentToolInTheMenus();

    void dragAndDropReordersPages();
    void dragAndDropAcceptsTheDrag();

private:
    QAction *action(const QString &name) const;
    PageGridView *view() const;
    QAbstractItemModel *model() const;

    QTemporaryDir m_dir;
    QString m_file;
    MainWindow *m_window = nullptr;
};

void TestMainWindow::initTestCase()
{
    KLocalizedString::setApplicationDomain("pdf-smithy");
    // Keep the developer's real recent-files list and window geometry out of it.
    QStandardPaths::setTestModeEnabled(true);

    QVERIFY(m_dir.isValid());
    m_file = m_dir.filePath(QStringLiteral("ten.pdf"));
    QVERIFY(test::writeSamplePdf(m_file, 10));
}

void TestMainWindow::init()
{
    m_window = new MainWindow;
    m_window->show();
    QVERIFY(QTest::qWaitForWindowExposed(m_window));
}

void TestMainWindow::cleanup()
{
    delete m_window;
    m_window = nullptr;
}

QAction *TestMainWindow::action(const QString &name) const
{
    return m_window->actionCollection()->action(name);
}

PageGridView *TestMainWindow::view() const
{
    return m_window->findChild<PageGridView *>();
}

QAbstractItemModel *TestMainWindow::model() const
{
    return view() ? view()->model() : nullptr;
}

// ── Opening ───────────────────────────────────────────────────────────────

void TestMainWindow::opensADocument()
{
    QVERIFY(m_window->openFile(m_file));
    QVERIFY(model());
    QCOMPARE(model()->rowCount(), 10);
}

void TestMainWindow::survivesABrokenFile()
{
    const QString broken = m_dir.filePath(QStringLiteral("broken.pdf"));
    QVERIFY(test::writeBrokenPdf(broken));

    // A damaged document must produce a refusal, not a crash and not a
    // half-loaded window.
    QVERIFY(!m_window->openFile(broken));
    QCOMPARE(model()->rowCount(), 0);
}

// ── Editing through the actions ───────────────────────────────────────────

void TestMainWindow::rotatesTheSelection()
{
    QVERIFY(m_window->openFile(m_file));
    view()->selectRows({ 1, 3 });

    QAction *rotate = action(QStringLiteral("page_rotate_right"));
    QVERIFY(rotate);
    rotate->trigger();

    QCOMPARE(model()->data(model()->index(1, 0), PageModel::RotationRole).toInt(), 90);
    QCOMPARE(model()->data(model()->index(3, 0), PageModel::RotationRole).toInt(), 90);
    QCOMPARE(model()->data(model()->index(2, 0), PageModel::RotationRole).toInt(), 0);
}

void TestMainWindow::deletesTheSelection()
{
    QVERIFY(m_window->openFile(m_file));
    // Whole pages are removed where whole pages are on show; see
    // deletingPagesIsRefusedWhileReading for why that is not incidental.
    action(QStringLiteral("mode_organise"))->trigger();
    view()->selectRows({ 0, 1, 2 });

    action(QStringLiteral("page_delete"))->trigger();
    QCOMPARE(model()->rowCount(), 7);

    // The remaining first page must be the old page 4.
    QCOMPARE(model()->data(model()->index(0, 0), PageModel::SourcePageRole).toInt(), 3);
}

void TestMainWindow::duplicatesTheSelection()
{
    QVERIFY(m_window->openFile(m_file));
    view()->selectRows({ 2 });

    action(QStringLiteral("page_duplicate"))->trigger();
    QCOMPARE(model()->rowCount(), 11);
    QCOMPARE(model()->data(model()->index(3, 0), PageModel::SourcePageRole).toInt(), 2);
}

void TestMainWindow::undoesEverything()
{
    QVERIFY(m_window->openFile(m_file));
    action(QStringLiteral("mode_organise"))->trigger();

    view()->selectRows({ 0 });
    action(QStringLiteral("page_rotate_right"))->trigger();
    view()->selectRows({ 5, 6 });
    action(QStringLiteral("page_delete"))->trigger();
    QCOMPARE(model()->rowCount(), 8);

    QAction *undo = action(QStringLiteral("edit_undo"));
    QVERIFY(undo);
    undo->trigger();
    QCOMPARE(model()->rowCount(), 10);
    undo->trigger();
    QCOMPARE(model()->data(model()->index(0, 0), PageModel::RotationRole).toInt(), 0);
    QVERIFY(!undo->isEnabled());
}

// ── Saving ────────────────────────────────────────────────────────────────

void TestMainWindow::savesToDisk()
{
    const QString working = m_dir.filePath(QStringLiteral("working.pdf"));
    QVERIFY(test::writeSamplePdf(working, 6));
    QVERIFY(m_window->openFile(working));
    action(QStringLiteral("mode_organise"))->trigger();

    view()->selectRows({ 0, 1 });
    action(QStringLiteral("page_delete"))->trigger();
    action(QStringLiteral("page_rotate_right"))->trigger();

    QVERIFY(action(QStringLiteral("file_save")));
    action(QStringLiteral("file_save"))->trigger();

    // The result has to be right on disk, not merely in the model.
    QCOMPARE(test::pageCountOf(working), 4);
    QVERIFY(test::contentOf(working, 0).contains(QStringLiteral("PSPAGE 3")));
    QCOMPARE(test::rotationOf(working, 0), 90);
}

void TestMainWindow::marksUnsavedChangesInTheTitle()
{
    const QString working = m_dir.filePath(QStringLiteral("titled.pdf"));
    QVERIFY(test::writeSamplePdf(working, 3));
    QVERIFY(m_window->openFile(working));

    // KMainWindow signals unsaved work through the window-modified flag and the
    // "[*]" placeholder rather than by rewriting the caption, so that is what
    // has to be asserted; the window manager decorates it from there.
    QVERIFY(!m_window->isWindowModified());
    QVERIFY(m_window->windowTitle().contains(QStringLiteral("titled.pdf")));

    view()->selectRows({ 0 });
    action(QStringLiteral("page_rotate_right"))->trigger();
    QVERIFY2(m_window->isWindowModified(), "an edited document must not look saved");

    action(QStringLiteral("file_save"))->trigger();
    QVERIFY2(!m_window->isWindowModified(), "a saved document must not look dirty");
}

// ── Action state ──────────────────────────────────────────────────────────

void TestMainWindow::disablesActionsWithoutASelection()
{
    QVERIFY(m_window->openFile(m_file));
    view()->clearSelection();
    QTest::qWait(1);

    // Deleting nothing is meaningless, so the action must be unavailable rather
    // than silently doing something surprising.
    QVERIFY(!action(QStringLiteral("page_delete"))->isEnabled());
    QVERIFY(!action(QStringLiteral("page_duplicate"))->isEnabled());
    QVERIFY(!action(QStringLiteral("page_extract"))->isEnabled());

    // Rotation without a selection turns the whole document, which is useful.
    QVERIFY(action(QStringLiteral("page_rotate_right"))->isEnabled());
}

void TestMainWindow::deletingPagesIsRefusedWhileReading()
{
    QVERIFY(m_window->openFile(m_file));
    view()->selectRows({ 4 });

    QAction *remove = action(QStringLiteral("page_delete"));
    QVERIFY(remove);
    // The point of the whole thing: this command answers to the bare Delete
    // key, and a shortcut is matched before the key reaches any widget. Left
    // available while the document is on screen, one keystroke removed the page
    // being read, in the mode whose tooltip promises nothing can be changed by
    // accident.
    QVERIFY2(remove->shortcuts().contains(QKeySequence(QKeySequence::Delete)),
             "Delete no longer removes pages, so this case is guarding nothing");

    QVERIFY2(!remove->isEnabled(), "Delete removes pages while the document is being read");
    QCOMPARE(model()->rowCount(), 10);

    action(QStringLiteral("mode_edit"))->trigger();
    QVERIFY2(!remove->isEnabled(), "Delete removes whole pages while the page is being edited");

    action(QStringLiteral("mode_organise"))->trigger();
    view()->selectRows({ 4 });
    QVERIFY2(remove->isEnabled(), "pages cannot be removed even where the pages are");
    remove->trigger();
    QCOMPARE(model()->rowCount(), 9);
}

void TestMainWindow::turningTheWholeDocumentSaysThatIsWhatItDid()
{
    QVERIFY(m_window->openFile(m_file));
    action(QStringLiteral("mode_organise"))->trigger();
    view()->clearSelection();
    QTest::qWait(1);

    auto *bar = m_window->findChild<KMessageWidget *>();
    QVERIFY2(bar, "the window has no message bar to say anything in");
    QVERIFY(!bar->isVisible());

    // Deleting asks before it takes the whole document; rotating simply took
    // it. It is undoable, so the fix is a sentence rather than a question, but
    // a keystroke that turns five hundred pages must not pass in silence.
    action(QStringLiteral("page_rotate_right"))->trigger();
    QTest::qWait(50);

    QCOMPARE(model()->data(model()->index(0, 0), PageModel::RotationRole).toInt(), 90);
    QCOMPARE(model()->data(model()->index(9, 0), PageModel::RotationRole).toInt(), 90);
    QVERIFY2(bar->isVisible() && !bar->text().isEmpty(),
             "the whole document was turned and the window said nothing about it");
}

void TestMainWindow::theReadingKeysAreLeftToTheView()
{
    // PageUp and PageDown are how a document is read, and the view answers them
    // with a screenful of glide. A window-wide shortcut is matched before the
    // key reaches any widget, so while two commands claimed these the reading
    // motion was code nobody could reach.
    const QList<QAction *> actions = m_window->actionCollection()->actions();
    for (const QKeySequence &key : { QKeySequence(Qt::Key_PageUp), QKeySequence(Qt::Key_PageDown) }) {
        for (const QAction *entry : actions) {
            QVERIFY2(!entry->shortcuts().contains(key),
                     qPrintable(QStringLiteral("%1 claims %2 window-wide, so the view never sees it")
                                    .arg(entry->objectName(), key.toString())));
        }
    }

    // The commands themselves are still there; it is only the keys they gave up.
    QVERIFY(action(QStringLiteral("go_next_page")));
    QVERIFY(action(QStringLiteral("go_previous_page")));
}

void TestMainWindow::documentPropertiesAreUndoneOnTheirOwn()
{
    const QString working = m_dir.filePath(QStringLiteral("properties.pdf"));
    QVERIFY(test::writeSamplePdf(working, 3));
    QVERIFY(m_window->openFile(working));
    action(QStringLiteral("mode_organise"))->trigger();

    view()->selectRows({ 0 });
    action(QStringLiteral("page_rotate_right"))->trigger();
    QCOMPARE(model()->data(model()->index(0, 0), PageModel::RotationRole).toInt(), 90);

    {
        const DialogFiller filler(QStringLiteral("PSTITLE"));
        action(QStringLiteral("document_metadata"))->trigger();
    }
    QVERIFY2(m_window->isWindowModified(), "changing the properties left the document looking saved");

    // The one that used to fail: with nothing pushed, Ctrl+Z undid the *page*
    // and left the new title in place, so a rotation vanished because someone
    // typed a title.
    action(QStringLiteral("edit_undo"))->trigger();
    QCOMPARE(model()->data(model()->index(0, 0), PageModel::RotationRole).toInt(), 90);

    action(QStringLiteral("edit_undo"))->trigger();
    QCOMPARE(model()->data(model()->index(0, 0), PageModel::RotationRole).toInt(), 0);
}

void TestMainWindow::cleaningUpBelongsToTheDocumentItWasAskedFor()
{
    QVERIFY(m_window->openFile(m_file));

    // What Clean Up decides is a decision about *this* document. It used to sit
    // in the window for the rest of the session, so the next file opened had
    // its attachments and its link actions stripped with nobody asked.
    m_window->setStripInteractivity(true);
    QVERIFY(m_window->stripsInteractivity());

    const QString other = m_dir.filePath(QStringLiteral("another.pdf"));
    QVERIFY(test::writeSamplePdf(other, 2));
    QVERIFY(m_window->openFile(other));

    QVERIFY2(!m_window->stripsInteractivity(),
             "a later document is still being cleaned up on a decision taken about an earlier one");
}

// ── Locked documents ──────────────────────────────────────────────────────

void TestMainWindow::opensALockedDocumentOnceThePasswordIsGiven()
{
    const QString plain = m_dir.filePath(QStringLiteral("unlocked.pdf"));
    const QString locked = m_dir.filePath(QStringLiteral("locked.pdf"));
    QVERIFY(test::writeSamplePdf(plain, 4));
    QString error;
    QVERIFY2(Encryption::encrypt(plain, locked, QStringLiteral("geheim"), QString(), Encryption::Permissions {},
                                 QString(), &error),
             qPrintable(error));

    // Nothing ever asked: the window said "invalid password" and the document
    // stayed shut for good, while this same program can put the password on.
    const PasswordTyper typer({ QStringLiteral("geheim") });
    QVERIFY2(m_window->openFile(locked), "a locked document could not be opened even with its password");
    QCOMPARE(typer.asked(), 1);
    QCOMPARE(model()->rowCount(), 4);
}

void TestMainWindow::aWrongPasswordSaysSoAndCanBeGivenUpOn()
{
    const QString plain = m_dir.filePath(QStringLiteral("unlocked2.pdf"));
    const QString locked = m_dir.filePath(QStringLiteral("locked2.pdf"));
    QVERIFY(test::writeSamplePdf(plain, 4));
    QString error;
    QVERIFY(Encryption::encrypt(plain, locked, QStringLiteral("geheim"), QString(), Encryption::Permissions {},
                                QString(), &error));

    // Two wrong answers, then the cancel that ends it. Asking again is the
    // whole point: a single refusal for a mistyped password would be worse
    // than the bug it replaces.
    const PasswordTyper typer({ QStringLiteral("nein"), QStringLiteral("auch-nicht") });
    QVERIFY2(!m_window->openFile(locked), "a document opened with the wrong password");
    QCOMPARE(typer.asked(), 3);
    QCOMPARE(model()->rowCount(), 0);
}

void TestMainWindow::savingKeepsTheDocumentLocked()
{
    const QString plain = m_dir.filePath(QStringLiteral("unlocked3.pdf"));
    const QString locked = m_dir.filePath(QStringLiteral("locked3.pdf"));
    QVERIFY(test::writeSamplePdf(plain, 4));
    QString error;
    QVERIFY(Encryption::encrypt(plain, locked, QStringLiteral("geheim"), QString(), Encryption::Permissions {},
                                QString(), &error));

    {
        const PasswordTyper typer({ QStringLiteral("geheim") });
        QVERIFY(m_window->openFile(locked));
    }

    action(QStringLiteral("mode_organise"))->trigger();
    view()->selectRows({ 0 });
    action(QStringLiteral("page_rotate_right"))->trigger();
    action(QStringLiteral("file_save"))->trigger();

    // Output is assembled from scratch, so without carrying the password across
    // every save quietly handed back an unlocked copy of a document its owner
    // had locked, and said nothing at all about it.
    QVERIFY2(Encryption::isEncrypted(locked), "saving unlocked a document that was opened locked");
    QVERIFY2(Encryption::canOpen(locked, QStringLiteral("geheim")),
             "the saved document is locked with something other than its own password");

    // And the edit is in there, so this is protection over the new document
    // rather than the old file left untouched.
    const QString opened = m_dir.filePath(QStringLiteral("opened3.pdf"));
    QVERIFY2(Encryption::decrypt(locked, opened, QStringLiteral("geheim"), &error), qPrintable(error));
    QCOMPARE(test::rotationOf(opened, 0), 90);
}

void TestMainWindow::zoomChangesThumbnailSize()
{
    QVERIFY(m_window->openFile(m_file));
    // The zoom commands act on whichever view is showing, and the window now
    // starts in the document. This case is about the grid's thumbnails.
    action(QStringLiteral("mode_organise"))->trigger();
    const int before = view()->thumbnailWidth();

    action(QStringLiteral("zoom_in"))->trigger();
    QVERIFY(view()->thumbnailWidth() > before);

    action(QStringLiteral("zoom_out"))->trigger();
    QCOMPARE(view()->thumbnailWidth(), before);
}

void TestMainWindow::fitsPagesToTheWindow()
{
    QVERIFY(m_window->openFile(m_file));
    action(QStringLiteral("mode_organise"))->trigger();
    m_window->resize(1200, 900);
    QVERIFY(QTest::qWaitForWindowExposed(m_window));

    action(QStringLiteral("zoom_fit_width"))->trigger();
    QCOMPARE(view()->fit(), PageGridView::Fit::Width);

    // One page per row and as wide as the viewport: the cell has to be within
    // a scrollbar's width of the viewport, or "fit width" does not fit.
    const int cell = view()->gridSize().width();
    const int viewport = view()->viewport()->width();
    QVERIFY2(cell <= viewport && cell > viewport - 60,
             qPrintable(QStringLiteral("cell %1 against a viewport of %2").arg(cell).arg(viewport)));

    // Two pages side by side must genuinely leave room for two.
    action(QStringLiteral("zoom_two_pages"))->trigger();
    QCOMPARE(view()->fit(), PageGridView::Fit::TwoPages);
    QVERIFY2(2 * view()->gridSize().width() <= view()->viewport()->width(),
             "two pages do not fit side by side in the width the fit chose");
    QVERIFY2(view()->gridSize().height() <= view()->viewport()->height(),
             "a whole page was promised but it is taller than the window");

    // Turning the wheel is the user overruling the fit, and it has to stick:
    // re-imposing it on the next resize would feel like the window arguing.
    const int chosen = view()->thumbnailWidth();
    view()->setThumbnailWidth(chosen - 40);
    QCOMPARE(view()->fit(), PageGridView::Fit::Free);
    m_window->resize(1000, 800);
    QTest::qWait(50);
    QCOMPARE(view()->thumbnailWidth(), chosen - 40);
}

void TestMainWindow::editsCommentsInTheWindow()
{
    QVERIFY(m_window->openFile(m_file));

    auto *stack = m_window->findChild<QStackedWidget *>();
    QVERIFY(stack);
    // A document is something you open to read, so the window starts in the
    // document rather than in the page grid.
    auto *pages = m_window->findChild<PageView *>();
    QVERIFY(pages);
    QVERIFY2(stack->currentWidget() == pages, "the window did not start in the document");

    // Commenting and correcting text are done on the page itself now. What has
    // to happen is that the command takes you there and puts the document into
    // the mode that can change it, not that a panel appears.
    for (const QString &name : { QStringLiteral("page_annotate"), QStringLiteral("page_text") }) {
        action(QStringLiteral("mode_organise"))->trigger();
        QCOMPARE(stack->currentWidget(), view());

        action(name)->trigger();

        QVERIFY2(stack->currentWidget() == pages,
                 qPrintable(QStringLiteral("%1 left the page grid in the centre").arg(name)));
        QVERIFY2(pages->mode() == PageView::Mode::Edit,
                 qPrintable(QStringLiteral("%1 did not put the document into an editable mode").arg(name)));
    }
}

void TestMainWindow::everyPageEditorOpensInTheWindow_data()
{
    QTest::addColumn<QString>("actionName");

    // Every action that puts an editing mode up rather than a dialog. The
    // failure this guards against is silent: a mode whose constructor throws or
    // whose widgets never reach the window leaves the menu entry doing nothing
    // at all, and nothing in the build says so.
    QTest::newRow("redact") << QStringLiteral("page_redact");
    QTest::newRow("trim") << QStringLiteral("page_crop");
    QTest::newRow("watermark") << QStringLiteral("page_watermark");
    QTest::newRow("sign") << QStringLiteral("page_sign");
    QTest::newRow("page numbers") << QStringLiteral("page_number");
}

void TestMainWindow::everyPageEditorOpensInTheWindow()
{
    QFETCH(QString, actionName);
    QVERIFY(m_window->openFile(m_file));

    auto *dock = m_window->findChild<QDockWidget *>(QStringLiteral("tool_dock"));
    auto *stack = m_window->findChild<QStackedWidget *>();
    QVERIFY(dock);
    QVERIFY(stack);

    QAction *entry = action(actionName);
    QVERIFY2(entry, qPrintable(QStringLiteral("no action called %1").arg(actionName)));
    entry->trigger();

    QVERIFY2(dock->isVisible(), qPrintable(QStringLiteral("%1 opened no tool panel").arg(actionName)));
    QVERIFY2(dock->widget(), qPrintable(QStringLiteral("%1 put an empty panel up").arg(actionName)));
    QVERIFY2(!dock->windowTitle().isEmpty(), qPrintable(QStringLiteral("%1 left the panel unnamed").arg(actionName)));
    QVERIFY2(stack->currentWidget() != view(),
             qPrintable(QStringLiteral("%1 left the page grid in the centre").arg(actionName)));

    dock->close();
    QVERIFY2(stack->currentWidget() == view(),
             qPrintable(QStringLiteral("%1 did not put the grid back").arg(actionName)));
}

// ── Drag and drop ─────────────────────────────────────────────────────────

void TestMainWindow::dragAndDropAcceptsTheDrag()
{
    QVERIFY(m_window->openFile(m_file));
    view()->selectRows({ 0 });

    // The payload the view itself would produce when a drag starts.
    const std::unique_ptr<QMimeData> mime(model()->mimeData({ model()->index(0, 0) }));
    QVERIFY2(mime, "the model refuses to describe a dragged page");
    // The viewport is the drop site, not the view. This was false once and the
    // whole feature was dead with nothing to show for it, so it is asserted.
    QVERIFY2(view()->viewport()->acceptDrops(), "the grid is not registered as a drop target");
    QVERIFY(mime->hasFormat(PageModel::pageMimeType()));

    const QPoint target = view()->visualRect(model()->index(4, 0)).center();

    QDragEnterEvent enter(target, Qt::MoveAction, mime.get(), Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(view()->viewport(), &enter);
    QVERIFY2(enter.isAccepted(), "the grid refuses the drag as it arrives");

    QDragMoveEvent move(target, Qt::MoveAction, mime.get(), Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(view()->viewport(), &move);
    QVERIFY2(move.isAccepted(), "the grid shows the forbidden cursor over a page");
}

void TestMainWindow::dragAndDropReordersPages()
{
    QVERIFY(m_window->openFile(m_file));
    view()->selectRows({ 0 });

    const std::unique_ptr<QMimeData> mime(model()->mimeData({ model()->index(0, 0) }));
    QVERIFY(mime);

    // Drop on the right-hand half of page 5, which means "put it after page 5".
    const QRect fifth = view()->visualRect(model()->index(4, 0));
    const QPoint target(fifth.right() - 4, fifth.center().y());

    QDragEnterEvent enter(target, Qt::MoveAction, mime.get(), Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(view()->viewport(), &enter);
    QDragMoveEvent move(target, Qt::MoveAction, mime.get(), Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(view()->viewport(), &move);
    QDropEvent drop(QPointF(target), Qt::MoveAction, mime.get(), Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(view()->viewport(), &drop);

    QVERIFY2(drop.isAccepted(), "the drop was refused");

    // Page 1 has to have travelled; the pages it passed shift down by one.
    QCOMPARE(model()->rowCount(), 10);
    QCOMPARE(model()->data(model()->index(0, 0), PageModel::SourcePageRole).toInt(), 1);
    QCOMPARE(model()->data(model()->index(4, 0), PageModel::SourcePageRole).toInt(), 0);

    // And it has to be undoable like every other edit.
    QVERIFY(action(QStringLiteral("edit_undo"))->isEnabled());
    action(QStringLiteral("edit_undo"))->trigger();
    QCOMPARE(model()->data(model()->index(0, 0), PageModel::SourcePageRole).toInt(), 0);
}

void TestMainWindow::hasNoAmbiguousShortcuts()
{
    // Two actions on one key makes KXmlGui open a modal complaint while the
    // window is still being built, which hangs anything driving it and greets
    // the user with a dialog at startup. It happened once; this stops it
    // happening quietly again.
    QHash<QKeySequence, QString> seen;
    const QList<QAction *> actions = m_window->actionCollection()->actions();

    for (QAction *action : actions) {
        for (const QKeySequence &shortcut : action->shortcuts()) {
            if (shortcut.isEmpty()) {
                continue;
            }
            const QString owner = action->objectName();
            // The message is built only when there is something to report:
            // QVERIFY2 evaluates its arguments either way, and describing a
            // clash that is not there means reading an end iterator.
            if (seen.contains(shortcut)) {
                QFAIL(qPrintable(QStringLiteral("%1 is on both “%2” and “%3”")
                                     .arg(shortcut.toString(), seen.value(shortcut), owner)));
            }
            seen.insert(shortcut, owner);
        }
    }

    QVERIFY2(seen.size() > 8, "the shortcuts seem to have gone missing entirely");
}

void TestMainWindow::offersEveryDocumentToolInTheMenus()
{
    QVERIFY(m_window->openFile(m_file));
    // In the mode where every one of these applies: removing whole pages is
    // offered where whole pages are.
    action(QStringLiteral("mode_organise"))->trigger();
    view()->selectRows({ 0, 1 });

    // Features that exist but cannot be reached are features the user does not
    // have. Anything added to the engine belongs on this list.
    const QStringList expected {
        QStringLiteral("page_delete"), QStringLiteral("page_rotate_left"), QStringLiteral("page_duplicate"),
        QStringLiteral("page_extract"), QStringLiteral("page_ocr"), QStringLiteral("page_compress"),
        QStringLiteral("page_sign"), QStringLiteral("page_watermark"), QStringLiteral("page_number"),
        QStringLiteral("page_crop"), QStringLiteral("page_redact"), QStringLiteral("page_nup"),
        QStringLiteral("page_annotate"), QStringLiteral("comments_export"), QStringLiteral("comments_import"),
        QStringLiteral("document_form"), QStringLiteral("page_text"), QStringLiteral("document_archive"),
        QStringLiteral("document_compare"), QStringLiteral("insert_file"), QStringLiteral("import_images"),
        QStringLiteral("export_images"), QStringLiteral("file_split"), QStringLiteral("document_metadata"),
        QStringLiteral("document_sanitize"), QStringLiteral("document_password"),
        // The family the command line has had all along and the window offered
        // no way into: writing the document out as something other than a PDF,
        // and the two ways of writing a PDF for somebody else's machine.
        QStringLiteral("export_text"), QStringLiteral("export_html"), QStringLiteral("export_markdown"),
        QStringLiteral("export_tables"), QStringLiteral("export_web"), QStringLiteral("export_pdfx")
    };

    for (const QString &name : expected) {
        QAction *found = action(name);
        QVERIFY2(found, qPrintable(QStringLiteral("no action named %1").arg(name)));
        QVERIFY2(found->isEnabled(), qPrintable(QStringLiteral("%1 is unavailable with a document open").arg(name)));
    }

    // And every one of them is placed in a menu or toolbar. An action that
    // exists but appears nowhere is how "the engine can do it" turns into "the
    // user cannot", which has happened here before.
    QFile rc(QStringLiteral(QT_TESTCASE_SOURCEDIR) + QStringLiteral("/../src/pdf-smithyui.rc"));
    QVERIFY2(rc.open(QIODevice::ReadOnly), qPrintable(rc.fileName()));
    const QString markup = QString::fromUtf8(rc.readAll());

    for (const QString &name : expected) {
        QVERIFY2(markup.contains(QStringLiteral("\"%1\"").arg(name)),
                 qPrintable(QStringLiteral("%1 is in no menu or toolbar").arg(name)));
    }
}

void TestMainWindow::startsANewDocumentWithOneBlankPage()
{
    // The program was only usable on files that already existed: there was no
    // way to begin one. A new document is a real blank page in a scratch file,
    // so everything downstream (saving, merging, undo) works unchanged.
    QAction *action = m_window->actionCollection()->action(QStringLiteral("file_new"));
    QVERIFY2(action, "there is no command for starting a new document");
    action->trigger();

    QVERIFY(model());
    QCOMPARE(model()->rowCount(), 1);
    // The scratch file the page came out of must not become the document's
    // name, or Save would quietly write into a temporary directory. The title
    // is where a user would see it if it had.
    QVERIFY2(!m_window->windowTitle().contains(QLatin1String("untitled.pdf")),
             "a new document claims the scratch file it was built from");
}

void TestMainWindow::addsBlankPagesBesideThePageChosen()
{
    QVERIFY(m_window->openFile(m_file));
    QCOMPARE(model()->rowCount(), 10);

    QAction *action = m_window->actionCollection()->action(QStringLiteral("insert_blank"));
    QVERIFY2(action, "there is no command for adding a blank page");

    // The dialog is modal, so it is answered from a timer the way the tool
    // tests answer theirs: accept whatever it offers and check where the paper
    // landed.
    QTimer::singleShot(0, [] {
        for (QWidget *widget : QApplication::topLevelWidgets()) {
            if (auto *dialog = qobject_cast<QDialog *>(widget); dialog && dialog->isVisible()) {
                dialog->accept();
                return;
            }
        }
    });
    action->trigger();
    QTRY_COMPARE_WITH_TIMEOUT(model()->rowCount(), 11, 5000);
}

QTEST_MAIN(TestMainWindow)

#include "tst_mainwindow.moc"
