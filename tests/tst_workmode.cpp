/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "TestPdf.h"

#include "core/Forms.h"
#include "ui/AnnotationOverlay.h"
#include "ui/FormDesignOverlay.h"
#include "ui/FormOverlay.h"
#include "ui/MainWindow.h"
#include "ui/ObjectOverlay.h"
#include "ui/PageGridView.h"
#include "ui/PageView.h"
#include "ui/TextOverlay.h"

#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QDockWidget>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QScrollBar>
#include <QSet>
#include <QSlider>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <KActionCollection>
#include <KLocalizedString>
#include <KToolBar>

#include <chrono>
#include <cmath>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

using namespace ps;

namespace {

/**
 * Kills the process rather than let one case wedge the whole suite.
 *
 * Everything here drives a real window, and a window is free to open a modal
 * loop nobody in a test is going to answer. The timer in ModalCloser rescues
 * anything still delivering events; what it cannot rescue is code blocking
 * outside the event loop, and a suite with one hanging case in it is a suite
 * nobody runs. So that case is turned into a crash that names itself instead.
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
 * Answers whatever modal window is on top, for as long as it is alive.
 *
 * A command that asks a question is waiting for a user who will never arrive, so
 * the answer is armed beforehand and fires inside whichever event loop happens
 * to be running. Refusing is the safe answer: it is the one that leaves the
 * document as it was.
 */
class ModalCloser
{
public:
    ModalCloser()
    {
        m_timer.setInterval(5);
        QObject::connect(&m_timer, &QTimer::timeout, [] {
            if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget())) {
                dialog->reject();
            }
        });
        m_timer.start();
    }

    ~ModalCloser() { m_timer.stop(); }

    ModalCloser(const ModalCloser &) = delete;
    ModalCloser &operator=(const ModalCloser &) = delete;

private:
    QTimer m_timer;
};

/** Shuts every dialog still on screen, so none of them outlives its case. */
void closeStrayDialogs()
{
    const QList<QWidget *> tops = QApplication::topLevelWidgets();
    for (QWidget *widget : tops) {
        if (widget->isVisible() && qobject_cast<QDialog *>(widget)) {
            widget->close();
        }
    }
    // Dialogs are handed to deleteLater, so without this they are still
    // top-level widgets the next time anyone counts.
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

/**
 * Every mode there is, in one place.
 *
 * A fifth mode added to the window and not added here would be covered by
 * nothing at all, and the tests would still pass, so the list is written once
 * and every case walks it rather than naming modes one at a time.
 */
QVector<MainWindow::WorkMode> everyMode()
{
    return { MainWindow::WorkMode::Read, MainWindow::WorkMode::Page, MainWindow::WorkMode::Form,
             MainWindow::WorkMode::Organise };
}

/** The button that puts the window into @p mode. */
QString buttonFor(MainWindow::WorkMode mode)
{
    switch (mode) {
    case MainWindow::WorkMode::Read:
        return QStringLiteral("mode_view");
    case MainWindow::WorkMode::Page:
        return QStringLiteral("mode_edit");
    case MainWindow::WorkMode::Form:
        return QStringLiteral("mode_form");
    case MainWindow::WorkMode::Organise:
        return QStringLiteral("mode_organise");
    }
    return {};
}

/** The one extra toolbar @p mode is about; the other three must be away. */
QString toolBarFor(MainWindow::WorkMode mode)
{
    switch (mode) {
    case MainWindow::WorkMode::Read:
        return QStringLiteral("readToolBar");
    case MainWindow::WorkMode::Page:
        return QStringLiteral("pageToolBar");
    case MainWindow::WorkMode::Form:
        return QStringLiteral("formToolBar");
    case MainWindow::WorkMode::Organise:
        return QStringLiteral("organiseToolBar");
    }
    return {};
}

/** Where @p name sits in @p fields, or -1. Field names are the document's own. */
int indexOfField(const QVector<FormField> &fields, const QString &name)
{
    for (int i = 0; i < fields.size(); ++i) {
        if (fields.at(i).name == name) {
            return i;
        }
    }
    return -1;
}

} // namespace

/**
 * The four work modes, and everything that is supposed to follow from one.
 *
 * The mode is the whole steering of this window: it decides which view is in
 * the middle, which layers of the page answer a click and in what order, which
 * toolbar is up and what the status bar says. Those four are set in four
 * different places in the window, which is exactly why they drift apart: a
 * toolbar left visible after the mode changed, or a layer still registered and
 * quietly taking presses, is invisible in a build and invisible in a unit test.
 *
 * So none of it is read out of the window's own bookkeeping. Which layer is
 * live is proved by clicking on the page and seeing which one moved; which
 * toolbar is up is proved by asking the toolbars; and the status bar is only
 * ever asked whether it says something *different*, because it says it in the
 * user's language and this runs in two of them.
 */
class TestWorkMode : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void init();
    void cleanup();

    void theWindowOpensReadyToRead();

    void eachModeShowsItsOwnView_data();
    void eachModeShowsItsOwnView();

    void eachModeShowsOneToolBarOfItsOwn_data();
    void eachModeShowsOneToolBarOfItsOwn();

    void readingFillsInFormsWithoutTouchingTheWords();
    void editingThePageTypesIntoTheWords();
    void editingTheFormDrawsFieldsAndNothingElse();

    void openingADocumentLeavesTheModeAlone_data();
    void openingADocumentLeavesTheModeAlone();

    void theCommandsThatWorkOnThePageChooseTheModeForYou_data();
    void theCommandsThatWorkOnThePageChooseTheModeForYou();

    void changingModeTakesTheEditingToolDown();
    void rightClickingTheDocumentOffersTheModesOwnMenu_data();
    void rightClickingTheDocumentOffersTheModesOwnMenu();

    void theStatusBarSaysWhichModeIsInForce();

    void theDraggingToolsAreExclusive_data();
    void theDraggingToolsAreExclusive();

    void thePageBoxSaysWhichPageOfHowMany();
    void everyToolBarNamesItsButtons();

    void theReadingSizeSurvivesEveryChangeOfMode_data();
    void theReadingSizeSurvivesEveryChangeOfMode();
    void aFitSurvivesAWindowResizedInAnotherMode();
    void arrangingThePagesOpensOnAWindowfulOfThem();
    void arrangingThePagesKeepsTheSizeChosenThere();
    void twoPagesSideBySideShowsTwoPages();
    void fittingTheHeightFitsAWholePage();

private:
    QAction *action(const QString &name) const;
    QStackedWidget *stack() const;
    PageGridView *grid() const;
    PageView *pageView() const;
    AnnotationOverlay *comments() const;
    FormOverlay *formFields() const;
    FormDesignOverlay *formDesign() const;
    TextOverlay *textRuns() const;
    ObjectOverlay *pageObjects() const;
    KToolBar *toolBar(const QString &name) const;

    /** The box that says which page, which is the status bar's only spin box. */
    QSpinBox *pageBox() const;

    /** Everything the status bar spells out in words, in the order it shows it. */
    QList<QLabel *> statusLabels() const;

    /** Lets the view take the size it has been given and lay its pages out in it. */
    void settle();

    /** Everything the window should be saying about @p mode, having got there by @p how. */
    void expectMode(MainWindow::WorkMode mode, const QString &how);

    /** One extra toolbar up, and the one @p mode is about. */
    void expectOneToolBar(MainWindow::WorkMode mode, const QString &how);

    /** One mouse event at @p pagePoint, in points on @p row's page. */
    void sendMouse(QEvent::Type type, int row, const QPointF &pagePoint, Qt::MouseButton button,
                   Qt::MouseButtons buttons);

    /** Press, two moves and a release, all in points on @p row's page. */
    void dragOnPage(int row, const QPointF &from, const QPointF &to);

    QTemporaryDir m_dir;

    /** Seven pages, each stamped "PSPAGE n" in 24pt at 72,700 on a 612 x 792 sheet. */
    QString m_file;

    /** The awkward form: required, multi-line, tick box, drop-down and locked. */
    QString m_form;

    MainWindow *m_window = nullptr;
    std::unique_ptr<Watchdog> m_watchdog;
};

void TestWorkMode::initTestCase()
{
    KLocalizedString::setApplicationDomain("pdf-smithy");
    // Keep the developer's real recent-files list, window geometry, toolbar
    // layout and shortcut overrides out of it.
    QStandardPaths::setTestModeEnabled(true);

    // The window's toolbars are described in the .rc file rather than built in
    // code, and XMLGUI looks that file up under the running program's own name.
    // A test binary is called something else, so without this it would find no
    // description and come up with no toolbars, and "which toolbar is up"
    // would have nothing to be asked of. The file itself is compiled into
    // ps_gui, so what is tested is the one in this checkout, not an installed
    // copy that may be older.
    QCoreApplication::setApplicationName(QStringLiteral("pdf-smithy"));

    QVERIFY(m_dir.isValid());
    m_file = m_dir.filePath(QStringLiteral("seven.pdf"));
    QVERIFY(test::writeSamplePdf(m_file, 7));
    m_form = m_dir.filePath(QStringLiteral("form.pdf"));
    QVERIFY(test::writeFormPdf(m_form));
}

void TestWorkMode::init()
{
    m_watchdog = std::make_unique<Watchdog>(QByteArray(QTest::currentTestFunction()) + ' '
                                            + QByteArray(QTest::currentDataTag() ? QTest::currentDataTag() : ""));

    m_window = new MainWindow;
    // A known size, so that what fits and what does not is the same answer here
    // as it is in CI.
    // Wide enough for the toolbars as they are now drawn: large icons with
    // their names underneath need about 1260 pixels for the reading bar, and
    // the window's own default was widened to match. A test window narrower
    // than the program's default would be asserting about a window nobody has.
    m_window->resize(1400, 900);
    m_window->show();
    QVERIFY(QTest::qWaitForWindowExposed(m_window));
}

void TestWorkMode::cleanup()
{
    // A window destroyed with an editing mode still up runs that mode's teardown
    // against a half-destroyed window. Closing the panel is how a user leaves one.
    if (auto *dock = m_window->findChild<QDockWidget *>(QStringLiteral("tool_dock"))) {
        if (dock->isVisible()) {
            dock->close();
        }
    }

    closeStrayDialogs();
    delete m_window;
    m_window = nullptr;
    m_watchdog.reset();
}

// ── Reaching the window ───────────────────────────────────────────────────

QAction *TestWorkMode::action(const QString &name) const
{
    return m_window->actionCollection()->action(name);
}

QStackedWidget *TestWorkMode::stack() const
{
    return m_window->findChild<QStackedWidget *>();
}

PageGridView *TestWorkMode::grid() const
{
    return m_window->findChild<PageGridView *>();
}

PageView *TestWorkMode::pageView() const
{
    return m_window->findChild<PageView *>();
}

AnnotationOverlay *TestWorkMode::comments() const
{
    return m_window->findChild<AnnotationOverlay *>();
}

FormOverlay *TestWorkMode::formFields() const
{
    return m_window->findChild<FormOverlay *>();
}

FormDesignOverlay *TestWorkMode::formDesign() const
{
    return m_window->findChild<FormDesignOverlay *>();
}

TextOverlay *TestWorkMode::textRuns() const
{
    return m_window->findChild<TextOverlay *>();
}

ObjectOverlay *TestWorkMode::pageObjects() const
{
    return m_window->findChild<ObjectOverlay *>();
}

KToolBar *TestWorkMode::toolBar(const QString &name) const
{
    return m_window->findChild<KToolBar *>(name);
}

QSpinBox *TestWorkMode::pageBox() const
{
    // From the status bar rather than from the whole window: other parts of the
    // program put spin boxes on screen too, and the first one findChild happens
    // upon is not a promise anybody made.
    return m_window->statusBar()->findChild<QSpinBox *>();
}

QList<QLabel *> TestWorkMode::statusLabels() const
{
    return m_window->statusBar()->findChildren<QLabel *>();
}

void TestWorkMode::settle()
{
    QCoreApplication::processEvents();
    QTest::qWait(20);
}

void TestWorkMode::expectMode(MainWindow::WorkMode mode, const QString &how)
{
    QVERIFY2(stack(), "the window has no central view at all");

    // Exactly one button ticked. Two ticked, or none, and "can I break this
    // document by clicking on it" has no answer the user can read off the
    // window, which is the whole reason the modes are a closed set.
    for (const MainWindow::WorkMode other : everyMode()) {
        QAction *button = action(buttonFor(other));
        QVERIFY2(button,
                 qPrintable(QStringLiteral("%1 is no longer registered with the window").arg(buttonFor(other))));
        const bool wanted = other == mode;
        QVERIFY2(button->isChecked() == wanted,
                 qPrintable(QStringLiteral("after %1 the window cannot say which mode it is in: %2 is %3")
                                .arg(how, buttonFor(other),
                                     button->isChecked() ? QStringLiteral("ticked and should not be")
                                                         : QStringLiteral("clear and should be ticked"))));
    }

    // Arranging pages is done in the grid; the other three are done on the
    // document itself, and asking for one of them has to bring it up.
    const bool wantsGrid = mode == MainWindow::WorkMode::Organise;
    QWidget *wanted = wantsGrid ? static_cast<QWidget *>(grid()) : static_cast<QWidget *>(pageView());
    QVERIFY2(wanted, "the window did not build the view this mode works in");
    QVERIFY2(stack()->currentWidget() == wanted,
             qPrintable(QStringLiteral("after %1 the window is showing the %2 rather than the %3")
                            .arg(how, wantsGrid ? QStringLiteral("document") : QStringLiteral("page grid"),
                                 wantsGrid ? QStringLiteral("page grid") : QStringLiteral("document"))));

    if (wantsGrid) {
        return; // The document view is not on screen, so what it would allow does not arise.
    }

    // Reading must not be able to alter the page; the other two must.
    const PageView::Mode expected = mode == MainWindow::WorkMode::Read ? PageView::Mode::View : PageView::Mode::Edit;
    QVERIFY2(
        pageView()->mode() == expected,
        qPrintable(QStringLiteral("after %1 the document %2, which is the opposite of what this mode promises")
                       .arg(how,
                            expected == PageView::Mode::View ? QStringLiteral("can still be changed by clicking on it")
                                                             : QStringLiteral("refuses to be changed"))));
}

void TestWorkMode::expectOneToolBar(MainWindow::WorkMode mode, const QString &how)
{
    QStringList up;
    for (const MainWindow::WorkMode other : everyMode()) {
        KToolBar *bar = toolBar(toolBarFor(other));
        QVERIFY2(bar,
                 qPrintable(QStringLiteral("the window has no %1, so this mode has nowhere to put its tools")
                                .arg(toolBarFor(other))));
        if (bar->isVisible()) {
            up += toolBarFor(other);
        }
    }

    QVERIFY2(up.size() == 1,
             qPrintable(QStringLiteral("after %1 the window is offering %2 sets of tools at once (%3), so the user "
                                       "has to work out which of them apply")
                            .arg(how)
                            .arg(up.size())
                            .arg(up.isEmpty() ? QStringLiteral("none") : up.join(QStringLiteral(", ")))));
    QVERIFY2(up.first() == toolBarFor(mode),
             qPrintable(QStringLiteral("after %1 the toolbar on screen is %2, which belongs to another mode")
                            .arg(how, up.first())));

    // Up is not the same as on screen, and this is the difference that shipped.
    // Every toolbar starts life on the same row, and a row is shared out left
    // to right: the main bar took all of it and the mode's bar was handed the
    // dozen pixels left over at the right-hand edge. It was there, it answered
    // isVisible(), and there was nothing on it a person could see, which is
    // exactly what the assertions above cannot tell apart.
    KToolBar *bar = toolBar(toolBarFor(mode));
    KToolBar *main = toolBar(QStringLiteral("mainToolBar"));
    QVERIFY2(main, "the window has no main toolbar at all");
    QVERIFY2(main->isVisible(), "the main toolbar is not on screen, so nothing that every mode needs is reachable");
    QVERIFY2(bar->y() != main->y(),
             qPrintable(QStringLiteral("after %1 the %2 shares a row with the main toolbar, which leaves it whatever "
                                       "width the main one did not want")
                            .arg(how, toolBarFor(mode))));
    QVERIFY2(bar->width() >= bar->sizeHint().width(),
             qPrintable(QStringLiteral("after %1 the %2 is %3 pixels wide and needs %4, so some of its tools are "
                                       "hidden behind an overflow arrow before the user has touched anything")
                            .arg(how, toolBarFor(mode))
                            .arg(bar->width())
                            .arg(bar->sizeHint().width())));
}

void TestWorkMode::sendMouse(QEvent::Type type, int row, const QPointF &pagePoint, Qt::MouseButton button,
                             Qt::MouseButtons buttons)
{
    const QPointF at = pageView()->fromPoints(row, pagePoint);
    // Sent straight to the viewport rather than posted to the window: the
    // scroll area routes viewport events itself, and hit-testing a widget that
    // is only notionally on screen is not what is being tested here.
    QMouseEvent event(type, at, at, pageView()->viewport()->mapToGlobal(at), button, buttons, Qt::NoModifier);
    QApplication::sendEvent(pageView()->viewport(), &event);
}

void TestWorkMode::dragOnPage(int row, const QPointF &from, const QPointF &to)
{
    sendMouse(QEvent::MouseButtonPress, row, from, Qt::LeftButton, Qt::LeftButton);
    // Two moves rather than one: an overlay that only starts a gesture on the
    // second move would otherwise never be asked.
    sendMouse(QEvent::MouseMove, row, (from + to) / 2.0, Qt::NoButton, Qt::LeftButton);
    sendMouse(QEvent::MouseMove, row, to, Qt::NoButton, Qt::LeftButton);
    sendMouse(QEvent::MouseButtonRelease, row, to, Qt::LeftButton, Qt::NoButton);
}

// ── Where the window starts ───────────────────────────────────────────────

void TestWorkMode::theWindowOpensReadyToRead()
{
    // A document is something you open to read. Landing in a mode that can
    // rewrite the page is how someone ruins a file they only meant to look at,
    // so this is the one starting state worth pinning down.
    expectMode(MainWindow::WorkMode::Read, QStringLiteral("opening the window"));
    expectOneToolBar(MainWindow::WorkMode::Read, QStringLiteral("opening the window"));

    QVERIFY(m_window->openFile(m_file));
    settle();
    expectMode(MainWindow::WorkMode::Read, QStringLiteral("opening a document into a fresh window"));

    // The keys are half of what makes four modes bearable, and they are asserted
    // as key codes because the labels beside them are translated.
    const QVector<Qt::Key> keys { Qt::Key_F5, Qt::Key_F6, Qt::Key_F7, Qt::Key_F8 };
    const QVector<MainWindow::WorkMode> modes = everyMode();
    for (int i = 0; i < modes.size(); ++i) {
        QVERIFY2(
            action(buttonFor(modes.at(i)))->shortcuts().contains(QKeySequence(keys.at(i))),
            qPrintable(QStringLiteral("%1 can no longer be reached from the keyboard").arg(buttonFor(modes.at(i)))));
    }
}

// ── What each mode puts on screen ─────────────────────────────────────────

void TestWorkMode::eachModeShowsItsOwnView_data()
{
    QTest::addColumn<int>("mode");
    for (const MainWindow::WorkMode mode : everyMode()) {
        QTest::newRow(qPrintable(buttonFor(mode))) << int(mode);
    }
}

void TestWorkMode::eachModeShowsItsOwnView()
{
    QFETCH(int, mode);
    const auto wanted = MainWindow::WorkMode(mode);

    QVERIFY(m_window->openFile(m_file));
    settle();

    action(buttonFor(wanted))->trigger();
    settle();
    expectMode(wanted, QStringLiteral("pressing %1").arg(buttonFor(wanted)));

    // And it holds on the way back out, so that no mode is a place the window
    // can only be got into.
    action(buttonFor(MainWindow::WorkMode::Read))->trigger();
    settle();
    expectMode(MainWindow::WorkMode::Read, QStringLiteral("leaving %1 again").arg(buttonFor(wanted)));
}

void TestWorkMode::eachModeShowsOneToolBarOfItsOwn_data()
{
    eachModeShowsItsOwnView_data();
}

void TestWorkMode::eachModeShowsOneToolBarOfItsOwn()
{
    QFETCH(int, mode);
    const auto wanted = MainWindow::WorkMode(mode);

    // This is the assertion that catches the toolbars drifting apart from the
    // modes: each is switched somewhere different in the window, and a toolbar
    // left up after its mode has gone is a row of buttons that do nothing to
    // the document now on screen.
    QVERIFY(m_window->openFile(m_file));
    settle();

    action(buttonFor(wanted))->trigger();
    settle();
    expectOneToolBar(wanted, QStringLiteral("pressing %1").arg(buttonFor(wanted)));

    // Through every other mode and back, because the failure that matters is a
    // toolbar that is never hidden rather than one that is never shown.
    for (const MainWindow::WorkMode other : everyMode()) {
        action(buttonFor(other))->trigger();
        settle();
        expectOneToolBar(other, QStringLiteral("going from %1 to %2").arg(buttonFor(wanted), buttonFor(other)));
    }
}

// ── Which layer takes the click ───────────────────────────────────────────

void TestWorkMode::readingFillsInFormsWithoutTouchingTheWords()
{
    // Which overlays are registered cannot be read back off the view, so it is
    // proved the only way it matters: a press on the page reaches the layer that
    // owns what is under it, and reaches no other.
    QVERIFY(m_window->openFile(m_form));
    settle();
    pageView()->fitPage();
    expectMode(MainWindow::WorkMode::Read, QStringLiteral("opening a form"));

    QVERIFY2(!formFields()->fields().isEmpty(), "the document's form never reached the view");
    const int name = indexOfField(formFields()->fields(), QStringLiteral("Name"));
    QVERIFY2(name >= 0, "the fixture's own field is not among the ones the view read");
    const QRectF box = formFields()->fields().at(name).rect;

    sendMouse(QEvent::MouseButtonPress, 0, box.center(), Qt::LeftButton, Qt::LeftButton);
    sendMouse(QEvent::MouseButtonRelease, 0, box.center(), Qt::LeftButton, Qt::NoButton);
    QVERIFY2(formFields()->selectedField() == name,
             "clicking a form field while reading did not offer it to be filled in");

    // The layer that *builds* forms is not in this mode, so the same click must
    // not have taken the field to be moved or resized instead.
    QVERIFY2(formDesign()->chosenPage() == -1,
             "clicking a form field while reading picked it up for redesigning, which is not what filling in a form "
             "means");

    // And a line of text on a page is something to select and copy here, never
    // something to retype.
    QVERIFY(m_window->openFile(m_file));
    settle();
    pageView()->fitPage();
    expectMode(MainWindow::WorkMode::Read, QStringLiteral("opening a document to read"));

    sendMouse(QEvent::MouseButtonPress, 0, QPointF(90, 706), Qt::LeftButton, Qt::LeftButton);
    sendMouse(QEvent::MouseButtonRelease, 0, QPointF(90, 706), Qt::LeftButton, Qt::NoButton);
    QVERIFY2(textRuns()->editedRow() == -1,
             "clicking a line of text while reading opened a caret in it, so a document opened to be read can be "
             "rewritten by accident");
    QVERIFY2(!textRuns()->hasEdits(), "reading a document left words waiting to be replaced in it");
    QVERIFY2(pageObjects()->chosenObjects().isEmpty(), "clicking while reading took hold of what the page draws");
    QVERIFY2(!m_window->isWindowModified(), "reading a document is not an edit and must not look like one");
}

void TestWorkMode::editingThePageTypesIntoTheWords()
{
    QVERIFY(m_window->openFile(m_file));
    settle();
    pageView()->fitPage();

    action(buttonFor(MainWindow::WorkMode::Page))->trigger();
    settle();
    expectMode(MainWindow::WorkMode::Page, QStringLiteral("pressing mode_edit"));

    sendMouse(QEvent::MouseButtonPress, 0, QPointF(90, 706), Qt::LeftButton, Qt::LeftButton);
    sendMouse(QEvent::MouseButtonRelease, 0, QPointF(90, 706), Qt::LeftButton, Qt::NoButton);

    QVERIFY2(textRuns()->editedRow() == 0,
             "clicking a line of text on the page opened no caret in it, so there is no way to correct a typo");

    // A line of text is also something the page draws, so both layers can answer
    // for it and only the order they were registered in decides which does.
    // Words first, or correcting a typo picks the whole block up instead.
    QVERIFY2(pageObjects()->chosenObjects().isEmpty(),
             "the layer that moves things about took a press on a line of text, so it is being asked before the "
             "layer that types into it");

    // Filling forms in belongs to reading, and designing them to the form mode;
    // neither may be listening here.
    QVERIFY(m_window->openFile(m_form));
    settle();
    pageView()->fitPage();
    expectMode(MainWindow::WorkMode::Page, QStringLiteral("opening a form while editing the page"));

    const int name = indexOfField(formFields()->fields(), QStringLiteral("Name"));
    QVERIFY(name >= 0);
    const QRectF box = formFields()->fields().at(name).rect;
    sendMouse(QEvent::MouseButtonPress, 0, box.center(), Qt::LeftButton, Qt::LeftButton);
    sendMouse(QEvent::MouseButtonRelease, 0, box.center(), Qt::LeftButton, Qt::NoButton);

    QVERIFY2(formFields()->selectedField() == -1,
             "a form field could be filled in while editing the page, so an answer typed there would be lost the "
             "moment the page is rewritten");
    QVERIFY2(formDesign()->chosenPage() == -1, "editing the page picked a form field up for redesigning");
}

void TestWorkMode::editingTheFormDrawsFieldsAndNothingElse()
{
    // Deliberately on a page with words on it: laying a form out over text whose
    // own layer still answers to the mouse is how a new field ends up moving a
    // paragraph instead.
    QVERIFY(m_window->openFile(m_file));
    settle();
    pageView()->fitPage();

    action(buttonFor(MainWindow::WorkMode::Form))->trigger();
    settle();
    expectMode(MainWindow::WorkMode::Form, QStringLiteral("pressing mode_form"));

    formDesign()->setKindToPlace(FormDesignOverlay::Kind::Text);
    QCOMPARE(formDesign()->kindToPlace(), FormDesignOverlay::Kind::Text);
    QVERIFY2(!formDesign()->hasPendingWork(), "the form layer had work waiting before anything was drawn");

    // Straight across the headline, which is the one place on this page where
    // every layer has something it could claim.
    dragOnPage(0, QPointF(80, 696), QPointF(320, 724));

    QVERIFY2(formDesign()->hasPendingWork(),
             "dragging with a field kind chosen drew no field, so a form cannot be laid out by hand at all");
    QVERIFY2(textRuns()->editedRow() == -1 && !textRuns()->hasEdits(),
             "drawing a field over a line of text opened that line for retyping as well");
    QVERIFY2(pageObjects()->chosenObjects().isEmpty(), "drawing a field also took hold of what the page draws");
    QVERIFY2(!comments()->isModified(), "drawing a field left a comment on the page as well");
    QVERIFY2(formFields()->selectedField() == -1, "drawing a field also offered one to be filled in");

    // Leaving the mode must not throw the work away, or every accidental press
    // of F5 costs the user the form they were drawing.
    action(buttonFor(MainWindow::WorkMode::Read))->trigger();
    settle();
    QVERIFY2(formDesign()->hasPendingWork(), "leaving the form mode discarded the field that had just been drawn");
}

// ── What may and may not change the mode ──────────────────────────────────

void TestWorkMode::openingADocumentLeavesTheModeAlone_data()
{
    eachModeShowsItsOwnView_data();
}

void TestWorkMode::openingADocumentLeavesTheModeAlone()
{
    QFETCH(int, mode);
    const auto wanted = MainWindow::WorkMode(mode);

    // Opening the next file in a stack of them is not a change of intent: a user
    // who has said they are here to arrange pages is still here to arrange pages
    // when the second document arrives.
    QVERIFY(m_window->openFile(m_file));
    settle();
    action(buttonFor(wanted))->trigger();
    settle();

    QVERIFY(m_window->openFile(m_form));
    settle();
    expectMode(wanted, QStringLiteral("opening another document"));
    expectOneToolBar(wanted, QStringLiteral("opening another document"));
}

void TestWorkMode::theCommandsThatWorkOnThePageChooseTheModeForYou_data()
{
    QTest::addColumn<QString>("command");
    QTest::addColumn<int>("mode");

    // Each of these used to open a window of its own and now takes the user to
    // the page instead, so the mode it lands in *is* the command.
    QTest::newRow("page_annotate") << QStringLiteral("page_annotate") << int(MainWindow::WorkMode::Page);
    QTest::newRow("page_text") << QStringLiteral("page_text") << int(MainWindow::WorkMode::Page);
    QTest::newRow("tool_form_builder") << QStringLiteral("tool_form_builder") << int(MainWindow::WorkMode::Form);
    QTest::newRow("field_text") << QStringLiteral("field_text") << int(MainWindow::WorkMode::Form);
    QTest::newRow("field_checkbox") << QStringLiteral("field_checkbox") << int(MainWindow::WorkMode::Form);
    QTest::newRow("field_signature") << QStringLiteral("field_signature") << int(MainWindow::WorkMode::Form);
}

void TestWorkMode::theCommandsThatWorkOnThePageChooseTheModeForYou()
{
    QFETCH(QString, command);
    QFETCH(int, mode);
    const auto wanted = MainWindow::WorkMode(mode);

    QVERIFY(m_window->openFile(m_file));
    settle();

    // From the far end of the window, so that arriving in the right mode cannot
    // be an accident of where it already was.
    action(buttonFor(MainWindow::WorkMode::Organise))->trigger();
    settle();

    QAction *entry = action(command);
    QVERIFY2(entry, qPrintable(QStringLiteral("%1 is no longer registered with the window").arg(command)));
    QVERIFY2(entry->isEnabled(), qPrintable(QStringLiteral("%1 is greyed out on an open document").arg(command)));

    {
        const ModalCloser closer;
        entry->trigger();
    }
    settle();

    expectMode(wanted, QStringLiteral("choosing %1").arg(command));
    expectOneToolBar(wanted, QStringLiteral("choosing %1").arg(command));
}

// ── An editing tool and a change of mode ──────────────────────────────────

void TestWorkMode::changingModeTakesTheEditingToolDown()
{
    QVERIFY(m_window->openFile(m_file));
    settle();

    auto *dock = m_window->findChild<QDockWidget *>(QStringLiteral("tool_dock"));
    QVERIFY(dock);

    action(QStringLiteral("page_crop"))->trigger();
    settle();
    QVERIFY2(dock->isVisible(), "trimming opened no tool panel, so this case is guarding nothing");
    QVERIFY2(stack()->currentWidget() != grid(), "the trimming stage never reached the middle of the window");

    // Changing what the window is for takes the stage with it. It used to leave
    // the mode alive with its panel in the dock and its Keep button pointing at
    // a selection in a document the user had stopped looking at.
    action(buttonFor(MainWindow::WorkMode::Read))->trigger();
    settle();

    QVERIFY2(!dock->isVisible(), "the trimming tools are still in the panel after the mode changed");
    QVERIFY2(!dock->widget(), "the panel still holds a tool belonging to a mode that has gone");
    expectMode(MainWindow::WorkMode::Read, QStringLiteral("pressing mode_view while trimming"));
}

// ── The menu under the right button ───────────────────────────────────────

void TestWorkMode::rightClickingTheDocumentOffersTheModesOwnMenu_data()
{
    QTest::addColumn<int>("mode");
    QTest::addColumn<QString>("expected");

    // What the menu has to hold is what the mode is about. Reading offers to
    // take a copy; editing the page offers to keep what has been done to it;
    // laying a form out offers the fields. None of them offers to delete the
    // page, which is what the one menu there used to be would have done.
    QTest::newRow("reading") << int(MainWindow::WorkMode::Read) << QStringLiteral("edit_copy");
    QTest::newRow("editing the page") << int(MainWindow::WorkMode::Page) << QStringLiteral("edit_apply");
    QTest::newRow("editing the form") << int(MainWindow::WorkMode::Form) << QStringLiteral("field_text");
}

void TestWorkMode::rightClickingTheDocumentOffersTheModesOwnMenu()
{
    QFETCH(int, mode);
    QFETCH(QString, expected);

    QVERIFY(m_window->openFile(m_file));
    settle();
    pageView()->fitPage();

    action(buttonFor(MainWindow::WorkMode(mode)))->trigger();
    settle();

    sendMouse(QEvent::MouseButtonPress, 0, QPointF(200, 500), Qt::RightButton, Qt::RightButton);
    settle();

    auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
    QVERIFY2(menu, "right-clicking the document put no menu up at all");

    QSet<QString> offered;
    const QList<QAction *> entries = menu->actions();
    for (const QAction *entry : entries) {
        offered.insert(entry->objectName());
    }
    QVERIFY2(offered.contains(expected),
             qPrintable(QStringLiteral("the menu for this mode does not offer %1").arg(expected)));
    QVERIFY2(!offered.contains(QStringLiteral("page_delete")),
             "right-clicking the document offers to delete the page, which is the page grid's question");

    menu->close();
    settle();
}

// ── Saying so ─────────────────────────────────────────────────────────────

void TestWorkMode::theStatusBarSaysWhichModeIsInForce()
{
    QVERIFY(m_window->openFile(m_file));
    settle();

    const QList<QLabel *> labels = statusLabels();
    QVERIFY2(!labels.isEmpty(), "the status bar spells nothing out at all");

    QVector<QStringList> said;
    for (const MainWindow::WorkMode mode : everyMode()) {
        action(buttonFor(mode))->trigger();
        settle();
        QStringList row;
        for (const QLabel *label : labels) {
            row += label->text();
        }
        said += row;
    }

    // Never compared against a particular wording: the status bar speaks the
    // user's language and this runs in two of them. What has to hold is that one
    // of its labels always says something, and says something *different* in
    // each of the four modes, which is the whole of "it tells you where you are".
    int speaks = -1;
    for (int column = 0; column < labels.size(); ++column) {
        QSet<QString> distinct;
        bool blank = false;
        for (const QStringList &row : std::as_const(said)) {
            distinct.insert(row.at(column));
            blank = blank || row.at(column).isEmpty();
        }
        if (!blank && distinct.size() == everyMode().size()) {
            speaks = column;
            break;
        }
    }

    QVERIFY2(speaks >= 0,
             "nothing in the status bar names the mode the window is in, so a glance at the bottom of the window "
             "cannot answer what clicking on the page will do");
}

// ── The three ways of dragging ────────────────────────────────────────────

void TestWorkMode::theDraggingToolsAreExclusive_data()
{
    QTest::addColumn<QString>("command");
    QTest::addColumn<int>("tool");

    QTest::newRow("tool_select_text") << QStringLiteral("tool_select_text") << int(PageView::Tool::Text);
    QTest::newRow("tool_select_area") << QStringLiteral("tool_select_area") << int(PageView::Tool::Rectangle);
    QTest::newRow("tool_pan") << QStringLiteral("tool_pan") << int(PageView::Tool::Hand);
}

void TestWorkMode::theDraggingToolsAreExclusive()
{
    QFETCH(QString, command);
    QFETCH(int, tool);

    QVERIFY(m_window->openFile(m_file));
    settle();

    const QStringList all { QStringLiteral("tool_select_text"), QStringLiteral("tool_select_area"),
                            QStringLiteral("tool_pan") };

    QAction *entry = action(command);
    QVERIFY2(entry, qPrintable(QStringLiteral("%1 is no longer registered with the window").arg(command)));
    entry->trigger();

    QVERIFY2(
        pageView()->tool() == PageView::Tool(tool),
        qPrintable(QStringLiteral("%1 was chosen and dragging on the page still does something else").arg(command)));

    for (const QString &other : all) {
        const bool wanted = other == command;
        QVERIFY2(action(other)->isChecked() == wanted,
                 qPrintable(
                     QStringLiteral("with %1 in hand the toolbar shows %2 as %3, so the user cannot tell what "
                                    "a drag will do")
                         .arg(command, other,
                              action(other)->isChecked() ? QStringLiteral("chosen too") : QStringLiteral("unchosen"))));
    }

    // Picking up a different way of dragging is not a change of mode: the whole
    // point of these three is that they live inside reading.
    expectMode(MainWindow::WorkMode::Read, QStringLiteral("choosing %1").arg(command));
}

// ── Which page of how many ────────────────────────────────────────────────

void TestWorkMode::thePageBoxSaysWhichPageOfHowMany()
{
    QVERIFY(m_window->openFile(m_file));
    settle();

    QSpinBox *box = pageBox();
    QVERIFY2(box, "the status bar has no box saying which page is showing");

    // Identified by what it is for rather than by name: the box that jumps to a
    // page is the one that can reach every page of this document and no more.
    QCOMPARE(box->minimum(), 1);
    QVERIFY2(box->maximum() == 7,
             qPrintable(QStringLiteral("the page box stops at %1 in a document of 7 pages, so the last pages cannot "
                                       "be reached through it")
                            .arg(box->maximum())));
    QCOMPARE(box->value(), 1);

    // Turning pages moves it, or it is a box that has to be read twice.
    pageView()->goToPage(4);
    settle();
    QCOMPARE(pageView()->currentPage(), 4);
    QVERIFY2(box->value() == 5,
             qPrintable(QStringLiteral("the reader is on page 5 and the box says %1").arg(box->value())));

    // And typing a number into it goes there, which is the point of a box rather
    // than a label.
    box->setValue(2);
    settle();
    QVERIFY2(pageView()->currentPage() == 1,
             qPrintable(QStringLiteral("page 2 was typed into the box and the document went to page %1")
                            .arg(pageView()->currentPage() + 1)));

    // How many there are, beside it. The count is a numeral in any language, so
    // this is the one thing about the status bar worth reading literally.
    QString spelled;
    const QList<QLabel *> labels = statusLabels();
    for (const QLabel *label : labels) {
        spelled += label->text() + QLatin1Char(' ');
    }
    QVERIFY2(spelled.contains(QString::number(7)),
             qPrintable(QStringLiteral("the status bar never says how long the document is; it reads \"%1\"")
                            .arg(spelled.trimmed())));
}

// ── The reading size, across a change of mode ─────────────────────────────

void TestWorkMode::theReadingSizeSurvivesEveryChangeOfMode_data()
{
    QTest::addColumn<int>("from");
    QTest::addColumn<int>("to");

    for (const MainWindow::WorkMode from : everyMode()) {
        for (const MainWindow::WorkMode to : everyMode()) {
            if (from != to) {
                QTest::newRow(qPrintable(QStringLiteral("%1 to %2").arg(buttonFor(from), buttonFor(to))))
                    << int(from) << int(to);
            }
        }
    }
}

void TestWorkMode::theReadingSizeSurvivesEveryChangeOfMode()
{
    QFETCH(int, from);
    QFETCH(int, to);

    QVERIFY(m_window->openFile(m_file));
    settle();

    // Saying what the user is here to do is not asking for anything to change
    // about the document in front of them. A page that resizes itself on the
    // way is the window taking a decision nobody asked it to take, and the
    // reader loses both the size they had set and the line they were on.
    pageView()->setZoom(1.4);
    pageView()->setLayout(PageView::Layout::Facing);
    pageView()->goToPage(3);
    pageView()->verticalScrollBar()->setValue(pageView()->verticalScrollBar()->value() + 40);
    settle();

    action(buttonFor(MainWindow::WorkMode(from)))->trigger();
    settle();

    const double zoom = pageView()->zoom();
    const PageView::Layout layout = pageView()->layout();
    const int scroll = pageView()->verticalScrollBar()->value();
    const int page = pageView()->currentPage();
    QVERIFY2(scroll > 0, "the case set the document down at the top of it, so it is proving nothing about scrolling");

    const QString how = QStringLiteral("going from %1 to %2 and back")
                            .arg(buttonFor(MainWindow::WorkMode(from)), buttonFor(MainWindow::WorkMode(to)));

    action(buttonFor(MainWindow::WorkMode(to)))->trigger();
    settle();

    // Already on the way there, not only once it is back: three of the four
    // modes show the very same document, so the page must not so much as
    // flinch on the way through them.
    if (MainWindow::WorkMode(to) != MainWindow::WorkMode::Organise) {
        QVERIFY2(std::abs(pageView()->zoom() - zoom) < 1e-6,
                 qPrintable(QStringLiteral("%1 resized the page on arrival, from %2 to %3")
                                .arg(how)
                                .arg(zoom)
                                .arg(pageView()->zoom())));
        QCOMPARE(pageView()->verticalScrollBar()->value(), scroll);
    }

    action(buttonFor(MainWindow::WorkMode(from)))->trigger();
    settle();

    QVERIFY2(std::abs(pageView()->zoom() - zoom) < 1e-6,
             qPrintable(
                 QStringLiteral("%1 left the page at %2 where it was %3").arg(how).arg(pageView()->zoom()).arg(zoom)));
    QVERIFY2(
        pageView()->layout() == layout,
        qPrintable(QStringLiteral("%1 put the pages back in one column, and the reader had asked for two").arg(how)));
    QVERIFY2(pageView()->verticalScrollBar()->value() == scroll,
             qPrintable(QStringLiteral("%1 moved the document from %2 to %3, so the reader has lost their place")
                            .arg(how)
                            .arg(scroll)
                            .arg(pageView()->verticalScrollBar()->value())));
    QCOMPARE(pageView()->currentPage(), page);
}

void TestWorkMode::aFitSurvivesAWindowResizedInAnotherMode()
{
    QVERIFY(m_window->openFile(m_file));
    settle();

    action(QStringLiteral("zoom_fit_width"))->trigger();
    settle();
    QVERIFY2(pageView()->pageRect(0).width() <= pageView()->viewport()->width(),
             "fit width does not fit the width even before anything has been asked of it");

    // The document view is not laid out at all while the grid is up, so the
    // window it comes back to can be a different one from the window it left.
    // A fit that lapsed there is the worst of both: still ticked in the menu,
    // and the page hanging off the right-hand edge of the window.
    action(buttonFor(MainWindow::WorkMode::Organise))->trigger();
    settle();
    m_window->resize(700, 800);
    settle();
    action(buttonFor(MainWindow::WorkMode::Page))->trigger();
    settle();

    const int available = pageView()->viewport()->width();
    const int drawn = pageView()->pageRect(0).width();
    QVERIFY2(drawn <= available,
             qPrintable(QStringLiteral("the window was made narrower elsewhere and the page came back %1 wide in %2")
                            .arg(drawn)
                            .arg(available)));
    QVERIFY2(drawn > available - 80,
             qPrintable(QStringLiteral("fit width left %1 of %2 unused").arg(available - drawn).arg(available)));
    QVERIFY2(pageView()->horizontalScrollBar()->maximum() == 0,
             "the page fits the window and there is still a sideways scrollbar under it");
}

// ── Arranging pages is a different size of job ────────────────────────────

void TestWorkMode::arrangingThePagesOpensOnAWindowfulOfThem()
{
    QVERIFY(m_window->openFile(m_file));
    settle();

    action(buttonFor(MainWindow::WorkMode::Organise))->trigger();
    settle();

    // Coming here to reorder a document and finding four pages filling the
    // window is arriving at the wrong tool: the question being answered is
    // "which of these belongs where", and it cannot be answered without a
    // page's neighbours on screen beside it.
    const int cell = grid()->gridSize().width();
    QVERIFY2(cell > 0, "the grid has no cell size at all");
    const int across = grid()->viewport()->width() / cell;
    QVERIFY2(across >= 6,
             qPrintable(QStringLiteral("arranging the pages opened on %1 pages across a window of %2, at %3 apiece")
                            .arg(across)
                            .arg(grid()->viewport()->width())
                            .arg(cell)));

    // The slider beside it belongs to the grid and says so in its tooltip. In
    // the document modes it did nothing anybody could see and quietly resized
    // the grid behind their back, which is the same crossed wire seen from the
    // other end.
    auto *slider = m_window->statusBar()->findChild<QSlider *>();
    QVERIFY2(slider, "the status bar has no slider for the size of the pages in the grid");
    QVERIFY2(slider->isVisible(), "arranging the pages offers no slider for how big they are");

    // And leaving it puts the document back the size it was, rather than
    // carrying the overview into the mode where a page is read.
    const double reading = pageView()->zoom();
    action(buttonFor(MainWindow::WorkMode::Read))->trigger();
    settle();
    QVERIFY2(std::abs(pageView()->zoom() - reading) < 1e-6,
             "the size the grid was left at followed the reader back into the document");
    QVERIFY2(!slider->isVisible(),
             "the grid's own size slider is still on show while a document is being read, where moving it changes "
             "nothing the reader can see");
}

void TestWorkMode::arrangingThePagesKeepsTheSizeChosenThere()
{
    QVERIFY(m_window->openFile(m_file));
    settle();

    action(buttonFor(MainWindow::WorkMode::Organise))->trigger();
    settle();

    // A size the user set by hand is theirs. Opening on the overview again the
    // next time would be the window insisting it knows better, once per visit.
    const int chosen = grid()->thumbnailWidth() + 120;
    grid()->setThumbnailWidth(chosen);
    settle();
    const int held = grid()->thumbnailWidth();
    QVERIFY2(held != 0, "the grid refused a size outright");

    action(buttonFor(MainWindow::WorkMode::Read))->trigger();
    settle();
    action(buttonFor(MainWindow::WorkMode::Organise))->trigger();
    settle();

    QVERIFY2(
        grid()->thumbnailWidth() == held,
        qPrintable(
            QStringLiteral("the grid was left at %1 and came back at %2").arg(held).arg(grid()->thumbnailWidth())));
}

// ── What the five fits mean on the document ───────────────────────────────

void TestWorkMode::twoPagesSideBySideShowsTwoPages()
{
    QVERIFY(m_window->openFile(m_file));
    settle();

    action(QStringLiteral("zoom_two_pages"))->trigger();
    settle();

    QVERIFY2(pageView()->layout() == PageView::Layout::Facing,
             "two pages side by side were asked for and the pages are still in one column");

    const QRect left = pageView()->pageRect(0);
    const QRect right = pageView()->pageRect(1);
    QVERIFY2(left.width() > 1 && right.width() > 1, "one of the two pages is not being drawn at all");
    QVERIFY2(right.left() >= left.right(), "the second page is not beside the first");
    QVERIFY2(left.left() >= 0 && right.right() <= pageView()->viewport()->width(),
             qPrintable(QStringLiteral("the spread runs from %1 to %2 in a window %3 wide, so one of the two pages "
                                       "the command promised is off the edge")
                            .arg(left.left())
                            .arg(right.right())
                            .arg(pageView()->viewport()->width())));
    QVERIFY2(left.height() <= pageView()->viewport()->height(),
             "a whole page was promised and it is taller than the window");

    // And the switch that says the same thing agrees with it, or the window is
    // showing a spread with "two pages side by side" left unticked.
    QVERIFY2(action(QStringLiteral("view_facing"))->isChecked(),
             "the pages are facing and the menu entry that turns that on is clear");
}

void TestWorkMode::fittingTheHeightFitsAWholePage()
{
    QVERIFY(m_window->openFile(m_file));
    settle();

    // Narrow on purpose. In a wide window fitting the height and fitting the
    // whole page work out to the same number, which is exactly why fit height
    // could quietly be neither for as long as it was.
    m_window->resize(620, 820);
    settle();

    action(QStringLiteral("zoom_fit_height"))->trigger();
    settle();

    const int available = pageView()->viewport()->height();
    const int drawn = pageView()->pageRect(0).height();
    QVERIFY2(
        drawn <= available,
        qPrintable(QStringLiteral("fit height drew a page %1 tall into a window of %2").arg(drawn).arg(available)));
    QVERIFY2(drawn > available - 60,
             qPrintable(QStringLiteral("fit height left %1 of %2 unused, so it is fitting something else")
                            .arg(available - drawn)
                            .arg(available)));
}

void TestWorkMode::everyToolBarNamesItsButtons()
{
    // A button whose picture has to carry the whole meaning is a button people
    // learn by trial, and this program has ninety of them. The mode bars used
    // to be icons alone, on the theory that someone who chose a mode already
    // knows what it holds. They do not.
    const QList<KToolBar *> bars = m_window->toolBars();
    QVERIFY2(!bars.isEmpty(), "the window came up with no toolbars at all");

    for (KToolBar *bar : bars) {
        QVERIFY2(bar->toolButtonStyle() == Qt::ToolButtonTextUnderIcon,
                 qPrintable(QStringLiteral("“%1” hides what its buttons are called").arg(bar->objectName())));
        QVERIFY2(bar->iconSize().width() >= 32,
                 qPrintable(QStringLiteral("“%1” draws its icons at %2 pixels")
                                .arg(bar->objectName())
                                .arg(bar->iconSize().width())));
    }
}

QTEST_MAIN(TestWorkMode)

#include "tst_workmode.moc"
