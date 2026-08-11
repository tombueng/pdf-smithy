/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "TestPdf.h"

#include "core/Annotation.h"
#include "core/Forms.h"
#include "ui/AnnotationOverlay.h"
#include "ui/FormOverlay.h"
#include "ui/InspectorDock.h"
#include "ui/MainWindow.h"
#include "ui/ObjectOverlay.h"
#include "ui/OutlineDock.h"
#include "ui/PageGridView.h"
#include "ui/PageView.h"
#include "ui/TextOverlay.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QDialog>
#include <QDockWidget>
#include <QLineEdit>
#include <QMouseEvent>
#include <QScrollArea>
#include <QSignalSpy>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QWhatsThis>

#include <KActionCollection>
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
 * Everything here drives a real window through real commands, and several of
 * those commands end in a modal loop. The timer in ModalCloser rescues anything
 * still delivering events; what it cannot rescue is code blocking outside the
 * event loop, and a suite with one hanging case in it is a suite nobody runs.
 * So that case is turned into a crash that names itself instead.
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
        QObject::connect(&m_timer, &QTimer::timeout, [this] {
            if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget())) {
                ++m_answered;
                dialog->reject();
            }
        });
        m_timer.start();
    }

    ~ModalCloser() { m_timer.stop(); }

    ModalCloser(const ModalCloser &) = delete;
    ModalCloser &operator=(const ModalCloser &) = delete;

    int answered() const { return m_answered; }

private:
    QTimer m_timer;
    int m_answered = 0;
};

/** Shuts every dialog still on screen and says what they were, by class. */
QStringList closeStrayDialogs()
{
    QStringList left;
    const QList<QWidget *> tops = QApplication::topLevelWidgets();
    for (QWidget *widget : tops) {
        if (widget->isVisible() && qobject_cast<QDialog *>(widget)) {
            left += QString::fromLatin1(widget->metaObject()->className());
            widget->close();
        }
    }
    // Dialogs are handed to deleteLater, so without this they are still
    // top-level widgets the next time anyone counts.
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    return left;
}

/**
 * The actions the sweep does not trigger, and why each one is on the list.
 *
 * Nothing is here for being awkward: an action a test may not press is an
 * action nobody can say works. These three reach outside the process.
 */
QStringList actionsLeftAlone()
{
    return {
        // Closes every window there is, and KMainWindow destroys itself when it
        // is closed, so the window this test is holding would be freed under it.
        QStringLiteral("file_quit"),

        // The one action that can genuinely put paper through a printer. Its
        // dialog belongs to the platform's spooler rather than to this process,
        // so there is no promise it can be answered from a timer in here.
        QStringLiteral("file_print"),

        // Hands a help URL to the desktop, which starts a separate program
        // (khelpcenter, observed doing exactly that) outliving the test that
        // asked for it.
        QStringLiteral("help_contents"),
    };
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
 * The document view, and every command that can be reached from the window.
 *
 * The page grid answers "which pages, in what order" and is covered elsewhere.
 * This is the other half: the document at reading size, the two modes it can be
 * in, the layers drawn over it, and the panel that describes whatever is
 * selected in them. Everything is driven through the window's own actions and
 * through synthesised mouse events, because the failures worth catching here (a
 * menu entry wired to nothing, an overlay the view never asks, typing that
 * never reaches the file) are all invisible to a unit test and all silent in a
 * build.
 */
class TestDocumentView : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void init();
    void cleanup();

    void everyActionSurvivesBeingTriggered_data();
    void everyActionSurvivesBeingTriggered();

    void switchesBetweenTheGridAndTheDocument();
    void switchesBetweenReadingAndEditing();
    void zoomingChangesHowBigThePageIsDrawn();
    void laysThePagesOutSinglyAndFacing();
    void goesToAPageAndSaysWhichOneItIsOn();

    void selectsAllTheTextAndCopiesIt();
    void draggingAcrossAWordSelectsIt();
    void aParagraphDraggedOverKeepsItsLineBreaks();

    void everyOverlayClaimsTheModeItWorksIn_data();
    void everyOverlayClaimsTheModeItWorksIn();
    void everyOverlayStartsWithNothingWaiting();
    void theViewPutsThePressThroughItsOverlays();
    void formAnswersAreCollectedAndThenApplied();
    void pageObjectsAreChosenMovedAndThenApplied();
    void aCommentDrawnOnThePageIsAppliedToTheDocument();

    void workWaitingOnThePageCountsAsUnsaved();
    void everythingThatReadsTheFileSeesWhatIsWaitingOnThePage();
    void theToolCommandsAreOfferedOnlyWithADocument();
    void readingKeepsThePagesChosenForArranging();
    void aBookmarkTurnsToItsPage();
    void doubleClickingAPageTurnsToItRatherThanShowingAPicture();

    void thePropertiesPanelIsAPanel();
    void thePropertiesPanelDescribesWhatWasSelected();

    void savingKeepsWhatWasTyped();

private:
    QAction *action(const QString &name) const;
    PageGridView *grid() const;
    PageView *pageView() const;
    QStackedWidget *stack() const;
    AnnotationOverlay *comments() const;
    FormOverlay *formFields() const;
    TextOverlay *textRuns() const;
    ObjectOverlay *pageObjects() const;
    InspectorDock *inspector() const;

    /** Brings the document view up and lets it lay its pages out at full size. */
    void showDocumentView();

    /** One mouse event at @p pagePoint, in points on @p row's page. */
    void sendMouse(QEvent::Type type, int row, const QPointF &pagePoint, Qt::MouseButton button,
                   Qt::MouseButtons buttons);

    /** Press, two moves and a release, all in points on @p row's page. */
    void dragOnPage(int row, const QPointF &from, const QPointF &to);

    QTemporaryDir m_dir;

    /** Six pages, each stamped "PSPAGE n", which is what selections are read by. */
    QString m_file;

    /** The awkward form: required, multi-line, tick box, drop-down and locked. */
    QString m_form;

    /** Every action the window registers, read once from a throwaway window. */
    QStringList m_actionNames;

    MainWindow *m_window = nullptr;
    std::unique_ptr<Watchdog> m_watchdog;
};

void TestDocumentView::initTestCase()
{
    KLocalizedString::setApplicationDomain("pdf-smithy");
    // Keep the developer's real recent-files list, window geometry and shortcut
    // overrides out of it.
    QStandardPaths::setTestModeEnabled(true);

    QVERIFY(m_dir.isValid());
    m_file = m_dir.filePath(QStringLiteral("six.pdf"));
    QVERIFY(test::writeSamplePdf(m_file, 6));
    m_form = m_dir.filePath(QStringLiteral("form.pdf"));
    QVERIFY(test::writeFormPdf(m_form));

    // The sweep needs the list of actions before any test window exists, and a
    // data function runs before init() does. One throwaway window here is
    // cheaper than building one per data row twice over.
    MainWindow census;
    const QList<QAction *> actions = census.actionCollection()->actions();
    for (const QAction *entry : actions) {
        m_actionNames += entry->objectName();
    }
    QVERIFY2(m_actionNames.size() > 40, "the window registered almost no actions, so the sweep would prove nothing");
}

void TestDocumentView::init()
{
    m_watchdog = std::make_unique<Watchdog>(QByteArray(QTest::currentTestFunction()) + ' '
                                            + QByteArray(QTest::currentDataTag() ? QTest::currentDataTag() : ""));

    m_window = new MainWindow;
    // A known size, so that what fits and what does not is the same answer here
    // as it is in CI.
    m_window->resize(1100, 800);
    m_window->show();
    QVERIFY(QTest::qWaitForWindowExposed(m_window));
}

void TestDocumentView::cleanup()
{
    // A window destroyed with an editing mode still up runs that mode's teardown
    // against a half-destroyed window, and leaving one up would carry it into the
    // next case besides. Closing the panel is how a user leaves a tool.
    if (auto *dock = m_window->findChild<QDockWidget *>(QStringLiteral("tool_dock"))) {
        if (dock->isVisible()) {
            dock->close();
        }
    }

    // What's This is application-wide: entering it and not leaving would eat the
    // next case's mouse events wherever they were aimed.
    if (QWhatsThis::inWhatsThisMode()) {
        QWhatsThis::leaveWhatsThisMode();
    }

    closeStrayDialogs();
    delete m_window;
    m_window = nullptr;
    m_watchdog.reset();
}

// ── Reaching the window ───────────────────────────────────────────────────

QAction *TestDocumentView::action(const QString &name) const
{
    return m_window->actionCollection()->action(name);
}

PageGridView *TestDocumentView::grid() const
{
    return m_window->findChild<PageGridView *>();
}

PageView *TestDocumentView::pageView() const
{
    return m_window->findChild<PageView *>();
}

QStackedWidget *TestDocumentView::stack() const
{
    return m_window->findChild<QStackedWidget *>();
}

AnnotationOverlay *TestDocumentView::comments() const
{
    return m_window->findChild<AnnotationOverlay *>();
}

FormOverlay *TestDocumentView::formFields() const
{
    return m_window->findChild<FormOverlay *>();
}

TextOverlay *TestDocumentView::textRuns() const
{
    return m_window->findChild<TextOverlay *>();
}

ObjectOverlay *TestDocumentView::pageObjects() const
{
    return m_window->findChild<ObjectOverlay *>();
}

InspectorDock *TestDocumentView::inspector() const
{
    return m_window->findChild<InspectorDock *>(QStringLiteral("inspector_dock"));
}

void TestDocumentView::showDocumentView()
{
    action(QStringLiteral("mode_view"))->trigger();
    // The view lays its pages out against the size it has been given, and it is
    // only given a real one once the stack has actually put it on screen.
    QCoreApplication::processEvents();
    QTest::qWait(20);
}

void TestDocumentView::sendMouse(QEvent::Type type, int row, const QPointF &pagePoint, Qt::MouseButton button,
                                 Qt::MouseButtons buttons)
{
    const QPointF at = pageView()->fromPoints(row, pagePoint);
    // Sent straight to the viewport rather than posted to the window: the
    // scroll area routes viewport events itself, and hit-testing a widget that
    // is only notionally on screen is not what is being tested here.
    QMouseEvent event(type, at, at, pageView()->viewport()->mapToGlobal(at), button, buttons, Qt::NoModifier);
    QApplication::sendEvent(pageView()->viewport(), &event);
}

void TestDocumentView::dragOnPage(int row, const QPointF &from, const QPointF &to)
{
    sendMouse(QEvent::MouseButtonPress, row, from, Qt::LeftButton, Qt::LeftButton);
    // Two moves rather than one: an overlay that only starts a gesture on the
    // second move would otherwise never be asked.
    sendMouse(QEvent::MouseMove, row, (from + to) / 2.0, Qt::NoButton, Qt::LeftButton);
    sendMouse(QEvent::MouseMove, row, to, Qt::NoButton, Qt::LeftButton);
    sendMouse(QEvent::MouseButtonRelease, row, to, Qt::LeftButton, Qt::NoButton);
}

// ── Every command in the window ───────────────────────────────────────────

void TestDocumentView::everyActionSurvivesBeingTriggered_data()
{
    QTest::addColumn<QString>("actionName");

    const QStringList skipped = actionsLeftAlone();
    for (const QString &name : std::as_const(m_actionNames)) {
        if (skipped.contains(name)) {
            continue;
        }
        QTest::newRow(qPrintable(name)) << name;
    }
}

void TestDocumentView::everyActionSurvivesBeingTriggered()
{
    QFETCH(QString, actionName);

    QVERIFY(m_window->openFile(m_file));
    // With pages chosen, since a command that works on a selection is disabled
    // without one and would otherwise never be pressed at all.
    grid()->selectRows({ 0, 1 });

    QAction *entry = action(actionName);
    QVERIFY2(entry, qPrintable(QStringLiteral("%1 is no longer registered with the window").arg(actionName)));
    if (!entry->isEnabled()) {
        return; // Unavailable here is a state, not a failure; the state itself is asserted elsewhere.
    }

    {
        const ModalCloser closer;
        entry->trigger();
    }
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

    // The window is still a window: this is what fails when a command reaches
    // something that is not there any more.
    QVERIFY2(m_window->isVisible(), qPrintable(QStringLiteral("%1 took the window off screen").arg(actionName)));
    QVERIFY2(m_window->centralWidget(), qPrintable(QStringLiteral("%1 emptied the window").arg(actionName)));
    QVERIFY2(pageView(), qPrintable(QStringLiteral("%1 took the document view with it").arg(actionName)));
    QVERIFY2(m_window->actionCollection()->action(actionName) == entry,
             qPrintable(QStringLiteral("%1 rebuilt the action collection under itself").arg(actionName)));

    // A question nobody answered leaves the program unusable and the next case
    // deadlocked, which is worse than whatever the command was going to do.
    QVERIFY2(!QApplication::activeModalWidget(),
             qPrintable(QStringLiteral("%1 left a modal window nothing had answered").arg(actionName)));
}

// ── Which view, and what may be done in it ────────────────────────────────

void TestDocumentView::switchesBetweenTheGridAndTheDocument()
{
    QVERIFY(m_window->openFile(m_file));
    QVERIFY(stack());
    QVERIFY(grid());
    QVERIFY(pageView());

    // A document is something you open to read, so the window starts in it.
    QCOMPARE(stack()->currentWidget(), static_cast<QWidget *>(pageView()));
    QVERIFY2(action(QStringLiteral("mode_view"))->isChecked(), "the window opens in no mode according to the toolbar");

    action(QStringLiteral("mode_organise"))->trigger();
    QCOMPARE(stack()->currentWidget(), static_cast<QWidget *>(grid()));
    QVERIFY(action(QStringLiteral("mode_organise"))->isChecked());
    QVERIFY2(!action(QStringLiteral("mode_view"))->isChecked(),
             "two modes are ticked at once, so the window cannot say which one is in force");

    action(QStringLiteral("mode_view"))->trigger();
    QCOMPARE(stack()->currentWidget(), static_cast<QWidget *>(pageView()));
    QVERIFY(action(QStringLiteral("mode_view"))->isChecked());

    // The keys are half of what makes four modes bearable; asserted as key
    // codes because the labels are translated and this runs twice.
    QVERIFY2(action(QStringLiteral("mode_view"))->shortcuts().contains(QKeySequence(Qt::Key_F5)),
             "reading has lost its key");
    QVERIFY2(action(QStringLiteral("mode_organise"))->shortcuts().contains(QKeySequence(Qt::Key_F8)),
             "arranging the pages has lost its key");
}

void TestDocumentView::switchesBetweenReadingAndEditing()
{
    QVERIFY(m_window->openFile(m_file));
    QCOMPARE(pageView()->mode(), PageView::Mode::View);
    QVERIFY2(action(QStringLiteral("mode_view"))->isChecked(),
             "the window starts in a mode the menu does not admit to");

    QSignalSpy modes(pageView(), &PageView::modeChanged);
    QVERIFY(modes.isValid());

    action(QStringLiteral("mode_edit"))->trigger();
    QCOMPARE(pageView()->mode(), PageView::Mode::Edit);
    QCOMPARE(modes.size(), 1);
    // There is nothing to edit in a grid of thumbnails, so asking to edit has to
    // bring the document up as well.
    QCOMPARE(stack()->currentWidget(), static_cast<QWidget *>(pageView()));
    QVERIFY(action(QStringLiteral("mode_edit"))->isChecked());
    QVERIFY2(!action(QStringLiteral("mode_view"))->isChecked(),
             "both modes are ticked, so \"can I break this by clicking\" has no answer");

    action(QStringLiteral("mode_view"))->trigger();
    QCOMPARE(pageView()->mode(), PageView::Mode::View);
    QCOMPARE(modes.size(), 2);
    QVERIFY(action(QStringLiteral("mode_view"))->isChecked());
}

void TestDocumentView::zoomingChangesHowBigThePageIsDrawn()
{
    QVERIFY(m_window->openFile(m_file));
    showDocumentView();

    const QRect first = pageView()->pageRect(0);
    QVERIFY2(first.isValid() && first.width() > 1, "the first page is not being drawn at any size at all");

    QSignalSpy zooms(pageView(), &PageView::zoomChanged);
    QVERIFY(zooms.isValid());

    const double before = pageView()->zoom();
    pageView()->zoomIn();
    const double larger = pageView()->zoom();
    QVERIFY2(larger > before, "zooming in did not change the scale");
    QVERIFY2(pageView()->pageRect(0).width() > first.width(), "the scale changed but the page is drawn the same size");
    QCOMPARE(zooms.size(), 1);

    pageView()->zoomOut();
    QVERIFY2(pageView()->zoom() < larger, "zooming out did not come back down");
    QCOMPARE(pageView()->pageRect(0).size(), first.size());

    const int viewportWidth = pageView()->viewport()->width();
    pageView()->fitWidth();
    const QRect wide = pageView()->pageRect(0);
    // As wide as the window less its gaps and a scrollbar: any narrower and the
    // fit is not fitting, any wider and it has overshot into a scrollbar.
    QVERIFY2(
        wide.width() <= viewportWidth,
        qPrintable(QStringLiteral("fit width drew %1 into a viewport of %2").arg(wide.width()).arg(viewportWidth)));
    QVERIFY2(
        wide.width() > viewportWidth - 80,
        qPrintable(
            QStringLiteral("fit width left %1 of %2 unused").arg(viewportWidth - wide.width()).arg(viewportWidth)));

    pageView()->fitPage();
    const QRect whole = pageView()->pageRect(0);
    QVERIFY2(whole.height() <= pageView()->viewport()->height(),
             "a whole page was promised and it is drawn taller than the window");
    QVERIFY2(whole.width() <= pageView()->viewport()->width(), "a whole page was promised and it is drawn too wide");
    QVERIFY2(whole.height() > pageView()->viewport()->height() / 2,
             "the whole page fits with room for another one beside it, which is not what was asked for");
}

void TestDocumentView::laysThePagesOutSinglyAndFacing()
{
    QVERIFY(m_window->openFile(m_file));
    showDocumentView();
    pageView()->fitPage();

    QCOMPARE(pageView()->layout(), PageView::Layout::Single);
    QVERIFY2(pageView()->pageRect(1).top() >= pageView()->pageRect(0).bottom(),
             "one column was asked for and two pages share a line");

    QSignalSpy layouts(pageView(), &PageView::layoutChanged);
    QVERIFY(layouts.isValid());

    pageView()->setLayout(PageView::Layout::Facing);
    QCOMPARE(pageView()->layout(), PageView::Layout::Facing);
    QCOMPARE(layouts.size(), 1);

    const QRect left = pageView()->pageRect(0);
    const QRect right = pageView()->pageRect(1);
    QVERIFY2(right.left() >= left.right(), "a spread was asked for and the second page is not beside the first");
    QVERIFY2(right.bottom() == left.bottom(), "the two halves of the spread do not stand on the same line");
    QVERIFY2(pageView()->pageRect(2).top() >= left.bottom(), "the third page did not start a new spread");

    pageView()->setLayout(PageView::Layout::Single);
    QCOMPARE(pageView()->layout(), PageView::Layout::Single);
    QCOMPARE(layouts.size(), 2);
}

void TestDocumentView::goesToAPageAndSaysWhichOneItIsOn()
{
    QVERIFY(m_window->openFile(m_file));
    showDocumentView();

    QSignalSpy pages(pageView(), &PageView::currentPageChanged);
    QVERIFY(pages.isValid());

    pageView()->goToPage(4);
    QCOMPARE(pageView()->currentPage(), 4);
    QVERIFY2(!pages.isEmpty(), "the view moved to another page without telling the window");
    QCOMPARE(pages.last().first().toInt(), 4);

    // The page has to be at the top of the window, or "go to page five" showed
    // the end of page four.
    QVERIFY2(qAbs(pageView()->pageRect(4).top()) < 40,
             "the page asked for is not at the top of the window after going to it");

    const int announcements = pages.size();
    pageView()->goToPage(99);
    QCOMPARE(pageView()->currentPage(), 4);
    QCOMPARE(pages.size(), announcements);
}

// ── Reading and copying ───────────────────────────────────────────────────

void TestDocumentView::selectsAllTheTextAndCopiesIt()
{
    QVERIFY(m_window->openFile(m_file));
    showDocumentView();
    QCOMPARE(pageView()->mode(), PageView::Mode::View);

    QSignalSpy selections(pageView(), &PageView::selectionChanged);
    QVERIFY(selections.isValid());

    pageView()->selectAll();
    QVERIFY2(pageView()->hasSelection(), "select all selected nothing in a document with text on every page");

    const QString selected = pageView()->selectedText();
    QVERIFY2(!selected.isEmpty(), "there is a selection but no text came out of it");
    // The marker the fixture stamps on every page, so this holds in any locale.
    QVERIFY2(selected.contains(QStringLiteral("PSPAGE")),
             qPrintable(QStringLiteral("the selection reads \"%1\", which is not what is on the pages").arg(selected)));

    QApplication::clipboard()->clear();
    pageView()->copySelection();
    QCOMPARE(QApplication::clipboard()->text(), selected);

    pageView()->clearSelection();
    QVERIFY2(!pageView()->hasSelection(), "clearing the selection left it selected");
    QVERIFY2(pageView()->selectedText().isEmpty(), "nothing is selected and there is still text to copy");
    QVERIFY2(selections.size() >= 2, "the window was never told the selection had changed");
}

void TestDocumentView::draggingAcrossAWordSelectsIt()
{
    QVERIFY(m_window->openFile(m_file));
    showDocumentView();
    pageView()->fitPage();
    pageView()->goToPage(0);

    // The fixture sets "PSPAGE 1" in 24pt at 72,700 on a 612 x 792 page, so a
    // line drawn through 706 points up crosses both words of it.
    QVERIFY2(pageView()->pageAt(pageView()->fromPoints(0, QPointF(76, 706)).toPoint()) == 0,
             "the headline is not where the view says the first page is");

    dragOnPage(0, QPointF(76, 706), QPointF(300, 706));

    QVERIFY2(pageView()->hasSelection(), "dragging the mouse across the headline selected nothing");
    const QString selected = pageView()->selectedText();
    QVERIFY2(
        selected.contains(QStringLiteral("PSPAGE")),
        qPrintable(QStringLiteral("the drag selected \"%1\" rather than the words it was dragged over").arg(selected)));

    // And a press on bare paper puts it down again, which is what makes a
    // selection something the user is holding rather than something stuck to it.
    sendMouse(QEvent::MouseButtonPress, 0, QPointF(300, 200), Qt::LeftButton, Qt::LeftButton);
    sendMouse(QEvent::MouseButtonRelease, 0, QPointF(300, 200), Qt::LeftButton, Qt::NoButton);
    QVERIFY2(!pageView()->hasSelection(), "clicking away from the words kept them selected");
}

void TestDocumentView::aParagraphDraggedOverKeepsItsLineBreaks()
{
    // The complaint: cut and paste loses the line breaks. Dragging over a
    // paragraph used to put a newline in at the page boundary and nowhere else,
    // so four lines of a letter pasted as one unbroken run of words, while the
    // very same four lines taken with a rectangle came out as four lines,
    // because that path had been taught to compare the boxes and this one had
    // not.
    const QString lines = m_dir.filePath(QStringLiteral("paragraph.pdf"));
    QVERIFY(test::writeTextHeavyPdf(lines, 2));
    QVERIFY(m_window->openFile(lines));
    showDocumentView();
    pageView()->fitPage();
    pageView()->goToPage(0);

    // The fixture sets forty lines of 13pt at 72 points in, on baselines
    // 720, 704, 688 … so a drag from just above the first baseline to just above
    // the fourth crosses four lines of type.
    dragOnPage(0, QPointF(74, 722), QPointF(300, 674));
    QVERIFY2(pageView()->hasSelection(), "dragging down four lines selected nothing");

    const QString selected = pageView()->selectedText();
    const QStringList parts = selected.split(QLatin1Char('\n'));
    QVERIFY2(parts.size() == 4,
             qPrintable(QStringLiteral("four lines came out as %1: \"%2\"").arg(parts.size()).arg(selected)));
    for (int i = 0; i < parts.size(); ++i) {
        QVERIFY2(parts.at(i).contains(QStringLiteral("Zeile %1 ").arg(i + 1)),
                 qPrintable(QStringLiteral("line %1 reads \"%2\"").arg(i + 1).arg(parts.at(i))));
    }

    // The middle lines are whole, and the words inside one are joined by spaces
    // rather than by anything the boxes were guessed at.
    QCOMPARE(parts.at(1), QStringLiteral("Zeile 2 auf Seite 1 mit genug Text fuer die Messung"));
    QCOMPARE(parts.at(2), QStringLiteral("Zeile 3 auf Seite 1 mit genug Text fuer die Messung"));

    // And across the whole document: every line of every page is a line, and the
    // break between two sheets is one newline rather than two.
    pageView()->selectAll();
    const QStringList all = pageView()->selectedText().split(QLatin1Char('\n'));
    QCOMPARE(all.size(), 80);
    QVERIFY2(!all.contains(QString()), "a blank line crept in where two pages meet");
    QCOMPARE(all.at(39), QStringLiteral("Zeile 40 auf Seite 1 mit genug Text fuer die Messung"));
    QCOMPARE(all.at(40), QStringLiteral("Zeile 1 auf Seite 2 mit genug Text fuer die Messung"));
}

// ── The layers over the page ──────────────────────────────────────────────

void TestDocumentView::everyOverlayClaimsTheModeItWorksIn_data()
{
    QTest::addColumn<int>("which");
    QTest::addColumn<bool>("inReading");
    QTest::addColumn<bool>("inEditing");

    // Which mode each layer works in is a promise its own documentation makes,
    // and getting it wrong is silent: the layer simply never draws and never
    // sees a click.
    QTest::newRow("comments") << 0 << true << true;
    QTest::newRow("form fields") << 1 << true << false;
    QTest::newRow("text runs") << 2 << false << true;
    QTest::newRow("page objects") << 3 << false << true;
}

void TestDocumentView::everyOverlayClaimsTheModeItWorksIn()
{
    QFETCH(int, which);
    QFETCH(bool, inReading);
    QFETCH(bool, inEditing);

    QVERIFY(m_window->openFile(m_file));

    PageView::Overlay *overlay = nullptr;
    switch (which) {
    case 0:
        overlay = comments();
        break;
    case 1:
        overlay = formFields();
        break;
    case 2:
        overlay = textRuns();
        break;
    default:
        overlay = pageObjects();
        break;
    }
    QVERIFY2(overlay, "the window did not build this layer at all");

    QCOMPARE(overlay->appliesTo(PageView::Mode::View), inReading);
    QCOMPARE(overlay->appliesTo(PageView::Mode::Edit), inEditing);
    QVERIFY2(inReading || inEditing, "a layer that applies in neither mode can never be reached");
}

void TestDocumentView::everyOverlayStartsWithNothingWaiting()
{
    QVERIFY(m_window->openFile(m_file));
    showDocumentView();

    QVERIFY(comments() && formFields() && textRuns() && pageObjects());

    // A document that has just been opened has nothing unsaved in it. Anything
    // waiting here would be written into the file at the next save, and the user
    // never asked for it.
    QVERIFY2(!comments()->isModified(), "a freshly opened document already has an uncommitted comment on it");
    QVERIFY2(!formFields()->isModified(), "a freshly opened document already has an answer typed into its form");
    QVERIFY2(!textRuns()->hasEdits(), "a freshly opened document already has words waiting to be replaced");
    QVERIFY2(!pageObjects()->hasPendingEdits(), "a freshly opened document already has a change waiting on its pages");

    // And nothing is selected in any of them, so the properties panel has
    // nothing to describe.
    QCOMPARE(comments()->selectedRow(), -1);
    QCOMPARE(formFields()->selectedField(), -1);
    QCOMPARE(textRuns()->editedRow(), -1);
    QCOMPARE(pageObjects()->chosenPage(), -1);
    QVERIFY(pageObjects()->chosenObjects().isEmpty());

    QVERIFY2(!m_window->isWindowModified(), "opening a document is not an edit and must not look like one");
}

void TestDocumentView::theViewPutsThePressThroughItsOverlays()
{
    // Registration cannot be read off the view, so it is proved the only way it
    // matters: a press on the page has to reach the layer that owns what is
    // under it. A layer that is built but never added looks exactly like one
    // that is added and never used, until a user clicks.
    QVERIFY(m_window->openFile(m_form));
    showDocumentView();
    pageView()->fitPage();

    QVERIFY2(!formFields()->fields().isEmpty(), "the document's form never reached the view");
    const int name = indexOfField(formFields()->fields(), QStringLiteral("Name"));
    QVERIFY2(name >= 0, "the fixture's own field is not among the ones the view read");
    const QRectF box = formFields()->fields().at(name).rect;

    sendMouse(QEvent::MouseButtonPress, 0, box.center(), Qt::LeftButton, Qt::LeftButton);
    sendMouse(QEvent::MouseButtonRelease, 0, box.center(), Qt::LeftButton, Qt::NoButton);
    QCOMPARE(formFields()->selectedField(), name);

    // The same press in the other mode belongs to whoever edits the page, so the
    // form must not take it.
    action(QStringLiteral("mode_edit"))->trigger();
    QCOMPARE(formFields()->selectedField(), -1);

    // Text runs, on a page that has some.
    QVERIFY(m_window->openFile(m_file));
    showDocumentView();
    pageView()->fitPage();
    action(QStringLiteral("mode_edit"))->trigger();
    QCOMPARE(pageView()->mode(), PageView::Mode::Edit);

    sendMouse(QEvent::MouseButtonPress, 0, QPointF(90, 706), Qt::LeftButton, Qt::LeftButton);
    sendMouse(QEvent::MouseButtonRelease, 0, QPointF(90, 706), Qt::LeftButton, Qt::NoButton);

    // A line of text is also something drawn on the page, so both layers can
    // answer for it and only the order they are asked in decides which does.
    // Text has to be asked first, or correcting a typo picks the block up
    // instead, and moving is what the handles round it are for.
    QVERIFY2(pageObjects()->chosenObjects().isEmpty(),
             "the layer that moves things about took a press on a line of text, so it is being asked before the "
             "layer that types into it");
    QVERIFY2(textRuns()->editedRow() == 0, "clicking a line of text in Edit mode opened no caret in it");
}

void TestDocumentView::formAnswersAreCollectedAndThenApplied()
{
    QVERIFY(m_window->openFile(m_form));
    showDocumentView();

    FormOverlay *form = formFields();
    QVERIFY(form);
    const int name = indexOfField(form->fields(), QStringLiteral("Name"));
    QVERIFY(name >= 0);

    // Chosen by index rather than by clicking, because that is the way in the
    // overlay offers that does not depend on where the field happens to be.
    form->selectField(name);
    QCOMPARE(form->selectedField(), name);

    auto *editor = pageView()->viewport()->findChild<QLineEdit *>();
    QVERIFY2(editor, "choosing a text field put nothing on the page to type into");
    editor->setText(QStringLiteral("PSANSWER"));

    QVERIFY2(form->isModified(), "an answer was typed and the overlay has nothing waiting");
    QCOMPARE(form->valueOf(name), QStringLiteral("PSANSWER"));
    QCOMPARE(form->values().value(QStringLiteral("Name")), QStringLiteral("PSANSWER"));

    QAction *apply = action(QStringLiteral("edit_apply"));
    QVERIFY(apply);
    {
        const ModalCloser closer;
        apply->trigger();
    }

    QVERIFY2(!form->isModified(), "keeping the changes left them waiting as well as applied");
    QVERIFY2(m_window->isWindowModified(), "the answer went into the document without the document noticing");
}

void TestDocumentView::pageObjectsAreChosenMovedAndThenApplied()
{
    QVERIFY(m_window->openFile(m_file));
    showDocumentView();
    pageView()->fitPage();
    action(QStringLiteral("mode_edit"))->trigger();

    ObjectOverlay *objects = pageObjects();
    QVERIFY(objects);
    QVERIFY2(!objects->source().isEmpty(), "the layer that edits the page was never told which file to read");

    // Banded from bare paper across the whole sheet, which is the gesture that
    // takes hold of everything drawn on it.
    dragOnPage(0, QPointF(20, 20), QPointF(590, 770));
    QCOMPARE(objects->chosenPage(), 0);
    QVERIFY2(!objects->chosenObjects().isEmpty(), "banding the whole page took hold of nothing drawn on it");

    objects->nudge(12.0, 0.0);
    QVERIFY2(objects->hasPendingEdits(), "moving what was chosen left nothing waiting to be written");

    {
        const ModalCloser closer;
        action(QStringLiteral("edit_apply"))->trigger();
    }

    QVERIFY2(!objects->hasPendingEdits(), "keeping the changes left them waiting as well as applied");
    QVERIFY2(m_window->isWindowModified(), "the page was rewritten without the document noticing");
}

void TestDocumentView::aCommentDrawnOnThePageIsAppliedToTheDocument()
{
    QVERIFY(m_window->openFile(m_file));
    showDocumentView();
    pageView()->fitPage();
    action(QStringLiteral("mode_edit"))->trigger();

    AnnotationOverlay *marks = comments();
    QVERIFY(marks);
    marks->setTool(AnnotationOverlay::Tool::Rectangle);
    QCOMPARE(marks->tool(), AnnotationOverlay::Tool::Rectangle);

    dragOnPage(0, QPointF(120, 300), QPointF(400, 500));
    QVERIFY2(marks->isModified(), "a box drawn on the page was not recorded as a comment");
    QCOMPARE(marks->annotationsByRow().value(0).size(), 1);

    {
        const ModalCloser closer;
        action(QStringLiteral("edit_apply"))->trigger();
    }

    QVERIFY2(!marks->isModified(), "keeping the changes left the comment waiting as well as applied");
    QVERIFY2(m_window->isWindowModified(), "the comment went into the document without the document noticing");
}

// ── Work that has not been written down yet ───────────────────────────────

void TestDocumentView::workWaitingOnThePageCountsAsUnsaved()
{
    QVERIFY(m_window->openFile(m_file));
    showDocumentView();
    pageView()->fitPage();
    action(QStringLiteral("mode_edit"))->trigger();

    QVERIFY2(!action(QStringLiteral("edit_apply"))->isEnabled(),
             "there is something to keep before anything has been done");

    comments()->setTool(AnnotationOverlay::Tool::Rectangle);
    dragOnPage(0, QPointF(120, 300), QPointF(400, 500));
    QVERIFY2(comments()->isModified(), "a box drawn on the page was not recorded as a comment");

    // The undo stack is clean (the layers collect and hand over in one step),
    // and yet an hour's marking up is sitting there to be lost. The window used
    // to answer "unsaved changes?" from the stack alone, so File ▸ Open threw
    // the lot away without a word.
    QVERIFY2(!action(QStringLiteral("edit_undo"))->isEnabled(),
             "the comment reached the undo stack by itself, so this case proves nothing");
    QVERIFY2(m_window->hasUnsavedWork(), "a page full of comments does not count as unsaved work");
    QVERIFY2(m_window->isWindowModified(), "the title bar says the document is saved while a comment waits on it");
    QVERIFY2(action(QStringLiteral("edit_apply"))->isEnabled(),
             "there is work waiting and no way to ask for it to be kept");
}

void TestDocumentView::everythingThatReadsTheFileSeesWhatIsWaitingOnThePage()
{
    const QString working = m_dir.filePath(QStringLiteral("commented.pdf"));
    QVERIFY(test::writeSamplePdf(working, 6));
    QVERIFY(m_window->openFile(working));
    showDocumentView();
    pageView()->fitPage();
    action(QStringLiteral("mode_edit"))->trigger();

    comments()->setTool(AnnotationOverlay::Tool::Rectangle);
    dragOnPage(0, QPointF(120, 300), QPointF(400, 500));
    QVERIFY(comments()->isModified());

    // Every tool, every export and every print reads a **file**, and this is
    // the one call that stands between them and the document. It used to hand
    // back the path unchanged whenever the undo stack was clean, which is
    // exactly when a page full of comments is waiting.
    QString path;
    {
        const ModalCloser closer;
        path = m_window->saveForTools();
    }
    QVERIFY2(!path.isEmpty(), "the document could not be put on disk for the tools to read");

    QString error;
    const QVector<Annotation> written = Annotations::read(path, {}, &error);
    QVERIFY2(!written.isEmpty(), "the file handed to the tools holds none of the comments that were drawn on the page");
    QVERIFY2(!comments()->isModified(), "the comment was written out and is still waiting to be written out");
}

void TestDocumentView::theToolCommandsAreOfferedOnlyWithADocument()
{
    // Seven commands that read a file, all of them offered with nothing open
    // and all of them returning in silence. A menu whose entries look available
    // and do nothing teaches the user that the menu is decoration.
    const QStringList tools { QStringLiteral("tool_preflight"),     QStringLiteral("tool_colour"),
                              QStringLiteral("tool_fonts"),         QStringLiteral("tool_images"),
                              QStringLiteral("tool_layout"),        QStringLiteral("tool_object_list"),
                              QStringLiteral("tool_form_behaviour") };

    for (const QString &name : tools) {
        QAction *entry = action(name);
        QVERIFY2(entry, qPrintable(QStringLiteral("no action named %1").arg(name)));
        QVERIFY2(!entry->isEnabled(), qPrintable(QStringLiteral("%1 is offered with no document open").arg(name)));
    }

    QVERIFY(m_window->openFile(m_file));
    for (const QString &name : tools) {
        QVERIFY2(action(name)->isEnabled(),
                 qPrintable(QStringLiteral("%1 is unavailable with a document open").arg(name)));
    }
}

// ── Moving about the document ─────────────────────────────────────────────

void TestDocumentView::readingKeepsThePagesChosenForArranging()
{
    QVERIFY(m_window->openFile(m_file));
    grid()->selectRows({ 0, 1, 2, 3 });
    QCOMPARE(grid()->selectedRows().size(), 4);

    showDocumentView();
    pageView()->goToPage(4);
    QCoreApplication::processEvents();

    // Turning to a page is not a decision about which pages are chosen. The
    // window used to rewrite the grid's selection to the single row being read,
    // silently and inside a signal blocker, so four pages picked out for
    // rearranging became one, and nothing was told that they had.
    QCOMPARE(pageView()->currentPage(), 4);
    QCOMPARE(grid()->selectedRows().size(), 4);
    QCOMPARE(grid()->selectedRows().constFirst(), 0);
}

void TestDocumentView::aBookmarkTurnsToItsPage()
{
    const QString booked = m_dir.filePath(QStringLiteral("chapters.pdf"));
    QVERIFY(test::writeBookmarkedPdf(booked, 6, { { 0, QStringLiteral("Front") }, { 3, QStringLiteral("Middle") } }));
    QVERIFY(m_window->openFile(booked));
    showDocumentView();

    auto *outline = m_window->findChild<OutlineDock *>();
    QVERIFY2(outline, "the window has no contents sidebar");

    // What clicking a chapter does. It used to pick a row in the page grid,
    // which is not on screen while reading, so bookmark after bookmark could
    // be clicked with the page in front of the reader never moving.
    Q_EMIT outline->pageRequested(3);
    QCoreApplication::processEvents();

    QCOMPARE(pageView()->currentPage(), 3);
}

void TestDocumentView::doubleClickingAPageTurnsToItRatherThanShowingAPicture()
{
    QVERIFY(m_window->openFile(m_file));
    action(QStringLiteral("mode_organise"))->trigger();
    QCOMPARE(stack()->currentWidget(), static_cast<QWidget *>(grid()));

    // It used to render at 1400 pixels on the GUI thread and put the result in
    // a modal box: the window froze, the picture showed the file as opened
    // rather than as it stands, and there was nothing to do there but close it.
    Q_EMIT grid()->pageActivated(3);
    QCoreApplication::processEvents();

    QVERIFY2(!QApplication::activeModalWidget(), "double-clicking a page still opens a window of its own");
    QCOMPARE(stack()->currentWidget(), static_cast<QWidget *>(pageView()));
    QCOMPARE(pageView()->currentPage(), 3);
}

// ── The properties panel ──────────────────────────────────────────────────

void TestDocumentView::thePropertiesPanelIsAPanel()
{
    QVERIFY(m_window->openFile(m_file));

    InspectorDock *panel = inspector();
    QVERIFY2(panel, "the window has no properties panel, or it is not the one named inspector_dock");
    QCOMPARE(panel->objectName(), QStringLiteral("inspector_dock"));

    // Dragged to any edge, torn off, and put away again, which is the whole
    // reason it is a panel rather than a column bolted to one side.
    QCOMPARE(panel->allowedAreas(), Qt::AllDockWidgetAreas);
    QVERIFY(panel->features().testFlag(QDockWidget::DockWidgetFloatable));
    QVERIFY(panel->features().testFlag(QDockWidget::DockWidgetMovable));
    QVERIFY(panel->features().testFlag(QDockWidget::DockWidgetClosable));
    QVERIFY2(!panel->windowTitle().isEmpty(), "the panel carries no caption, so nothing can name it to the user");

    // And it is reachable from the menus rather than only from the mouse.
    QAction *toggle = action(QStringLiteral("show_inspector"));
    QVERIFY2(toggle, "there is no menu entry for the properties panel");
    QVERIFY(toggle->isCheckable());
    QVERIFY(toggle->shortcuts().contains(QKeySequence(Qt::Key_F4)));

    toggle->trigger();
    QVERIFY2(panel->isVisible(), "the menu entry did not bring the panel up");
    toggle->trigger();
    QVERIFY2(panel->isHidden(), "the menu entry did not put the panel away again");
}

void TestDocumentView::thePropertiesPanelDescribesWhatWasSelected()
{
    QVERIFY(m_window->openFile(m_form));
    showDocumentView();
    action(QStringLiteral("show_inspector"))->trigger();

    InspectorDock *panel = inspector();
    QVERIFY(panel);
    auto *body = panel->findChild<QScrollArea *>();
    QVERIFY2(body, "the panel has nothing to put an editor in");
    // With nothing on the page chosen the panel describes the document itself.
    // It used to stand empty here, which is the whole of reading and most of
    // arranging pages spent looking at a blank sidebar.
    QVERIFY2(body->widget(), "nothing is selected and the panel says nothing about the document either");
    const QWidget *aboutDocument = body->widget();

    const int name = indexOfField(formFields()->fields(), QStringLiteral("Name"));
    QVERIFY(name >= 0);
    formFields()->selectField(name);
    QCoreApplication::processEvents();

    QWidget *editor = body->widget();
    QVERIFY2(editor, "a form field was selected and the panel stayed empty");
    QVERIFY2(editor != aboutDocument, "the panel still describes the document although a field was selected");
    QVERIFY2(!editor->findChildren<QWidget *>().isEmpty(),
             "the panel put an empty sheet up, so the field's attributes were never built");
}

// ── The one that matters most ─────────────────────────────────────────────

void TestDocumentView::savingKeepsWhatWasTyped()
{
    // Everything else here is about the window behaving; this is about the user
    // not losing an afternoon's work. What was typed on the page has to be in
    // the file afterwards, read back from disk rather than from the overlay that
    // put it there.
    const QString working = m_dir.filePath(QStringLiteral("filled.pdf"));
    QVERIFY(test::writeFormPdf(working));
    QVERIFY(m_window->openFile(working));
    showDocumentView();

    FormOverlay *form = formFields();
    QVERIFY(form);
    const int name = indexOfField(form->fields(), QStringLiteral("Name"));
    QVERIFY2(name >= 0, "the fixture's own field is not among the ones the view read");

    form->selectField(name);
    auto *editor = pageView()->viewport()->findChild<QLineEdit *>();
    QVERIFY2(editor, "choosing a text field put nothing on the page to type into");
    editor->setText(QStringLiteral("PSANSWER"));
    QVERIFY(form->isModified());

    {
        const ModalCloser closer;
        action(QStringLiteral("file_save"))->trigger();
    }
    QVERIFY2(!m_window->isWindowModified(), "the document still looks unsaved after saving it");

    const QVector<FormField> written = Forms::read(working, nullptr);
    QVERIFY2(!written.isEmpty(), "the saved file carries no form at all any more");
    const int saved = indexOfField(written, QStringLiteral("Name"));
    QVERIFY2(saved >= 0, "the field that was filled in is missing from the saved file");
    QCOMPARE(written.at(saved).value, QStringLiteral("PSANSWER"));

    // The fields nobody touched are still the form's own, or filling in one
    // answer quietly rewrote the rest of the document.
    QCOMPARE(written.size(), form->fields().size());
    const int locked = indexOfField(written, QStringLiteral("Aktenzeichen"));
    QVERIFY(locked >= 0);
    QCOMPARE(written.at(locked).value, QStringLiteral("AZ-2026-001"));
}

QTEST_MAIN(TestDocumentView)

#include "tst_documentview.moc"
