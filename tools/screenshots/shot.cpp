/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/

/**
 * @file
 * Takes a picture of the application's own window, for the handbook.
 *
 * The window is the real ps::MainWindow, driven into the state that is wanted
 * by triggering the very actions a user would click, and then copied out with
 * QWidget::grab(). Nothing here scrapes the screen or talks to a compositor,
 * so it produces the same picture on a developer's desktop, over SSH and in
 * CI, and it works under QT_QPA_PLATFORM=offscreen where there is no screen at
 * all.
 *
 * Every step is named on the command line and every step that cannot be
 * carried out ends the run with a message saying which one it was. A
 * screenshot that quietly shows the wrong thing is worse than no screenshot:
 * it goes into the handbook and stays there.
 */

#include "config.h"

#include "ui/FormOverlay.h"
#include "ui/MainWindow.h"
#include "ui/PageGridView.h"
#include "ui/PageView.h"

#include <QAbstractItemModel>
#include <QAction>
#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDialog>
#include <QDir>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QIcon>
#include <QMenu>
#include <QMenuBar>
#include <QPainter>
#include <QPixmap>
#include <QStackedWidget>
#include <QTemporaryDir>
#include <QTextStream>
#include <QTimer>
#include <QWindow>

#include <KAboutData>
#include <KActionCollection>
#include <KLocalizedString>

#include <chrono>
#include <clocale>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

using namespace ps;

namespace {

QTextStream &out()
{
    static QTextStream stream(stdout);
    return stream;
}

QTextStream &err()
{
    static QTextStream stream(stderr);
    return stream;
}

/** Says what went wrong and gives the exit code the caller should return. */
int fail(const QString &what)
{
    err() << QStringLiteral("pdf-smithy-shot: ") << what << Qt::endl;
    err().flush();
    return 1;
}

/**
 * Kills the process rather than let one run hang for ever.
 *
 * This drives a real window, and a window is free to open a modal loop nobody
 * is going to answer. A screenshot script runs dozens of these in a row, so a
 * single wedged run has to end by itself and say which file it was taking.
 * Copied in spirit from the test suite, which needs it for the same reason.
 */
/**
 * Answers a modal window the moment one appears, so a run cannot hang on it.
 *
 * A shot that drives a command may open a dialog it never meant to: a question
 * about unsaved work, a chooser, a warning. Nobody is there to press a button,
 * so the run would sit until the watchdog killed it and the shot would be lost.
 * Rejecting is the safe answer, because it is the one that changes nothing
 * about the document being photographed. A shot that genuinely wants a dialog
 * in the picture simply does not ask for one of these.
 */
class ModalCloser : public QObject
{
public:
    ModalCloser()
    {
        // On a timer rather than on a signal: Qt has no "a modal window went
        // up" notification, and the dialogs here are opened by exec(), which
        // does not return until the window is answered.
        m_timer.setInterval(100);
        QObject::connect(&m_timer, &QTimer::timeout, this, [] {
            if (QWidget *modal = QApplication::activeModalWidget()) {
                if (auto *dialog = qobject_cast<QDialog *>(modal)) {
                    dialog->reject();
                } else {
                    modal->close();
                }
            }
        });
        m_timer.start();
    }

private:
    QTimer m_timer;
};

class Watchdog
{
public:
    explicit Watchdog(QByteArray what, int seconds)
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
 * Deals with whatever modal dialog turns up while the recipe is running.
 *
 * A dialog is a nested event loop that does not return until somebody answers
 * it, and nobody here is going to, so something has to be armed beforehand and
 * fire from inside that loop. What it should do is the one thing only the
 * person asking for the picture knows:
 *
 *  - reject: refuse it, which is what the test suite does and the safe answer,
 *    because it is the one that leaves the document as it was. Use it when the
 *    dialog is in the way of the picture rather than the subject of it.
 *  - leave: the dialog is what the picture is of. It is photographed where it
 *    stands, and only then refused, because a run that never answers is a run
 *    the watchdog has to kill and no picture comes out of that.
 *
 * The shutter is handed in rather than known here: taking the picture is the
 * caller's business, and this only decides the moment.
 */
class ModalWatcher
{
public:
    ModalWatcher(bool photograph, std::function<void()> shoot)
        : m_photograph(photograph)
        , m_shoot(std::move(shoot))
    {
        m_timer.setInterval(25);
        QObject::connect(&m_timer, &QTimer::timeout, [this] {
            // The shutter runs an event loop of its own, so without this the
            // timer would fire again in the middle of taking the picture.
            if (m_busy) {
                return;
            }
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!dialog) {
                return;
            }
            m_busy = true;
            if (m_photograph) {
                m_shoot();
            }
            dialog->reject();
            m_busy = false;
        });
        m_timer.start();
    }

    ~ModalWatcher() { m_timer.stop(); }

    ModalWatcher(const ModalWatcher &) = delete;
    ModalWatcher &operator=(const ModalWatcher &) = delete;

private:
    bool m_photograph;
    std::function<void()> m_shoot;
    bool m_busy = false;
    QTimer m_timer;
};

/**
 * Lets the window get on with its work for @p milliseconds.
 *
 * Pages are rasterised on other threads and arrive through the render cache, so
 * a grab taken the instant after a command was triggered photographs a grey
 * rectangle where the page will be. There is no signal that means "everything
 * anybody asked for has been drawn", so this is time rather than a condition,
 * and the caller is given both a per-step wait and a longer one before the
 * shutter.
 */
void settle(int milliseconds)
{
    if (milliseconds <= 0) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        return;
    }

    QEventLoop loop;
    QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
    loop.exec(QEventLoop::AllEvents);
}

/** Waits until @p widget has a window the platform has actually shown. */
bool waitForExposed(QWidget *widget, int milliseconds = 5000)
{
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < milliseconds) {
        if (widget->windowHandle() && widget->windowHandle()->isExposed()) {
            return true;
        }
        settle(20);
    }
    return widget->windowHandle() && widget->windowHandle()->isExposed();
}

/** "1280x800" as a size, or an invalid size when it is not that. */
QSize parseSize(const QString &text)
{
    const qsizetype cross = text.indexOf(QLatin1Char('x'), 0, Qt::CaseInsensitive);
    if (cross <= 0) {
        return {};
    }
    bool wide = false;
    bool tall = false;
    const int width = text.left(cross).toInt(&wide);
    const int height = text.mid(cross + 1).toInt(&tall);
    if (!wide || !tall || width <= 0 || height <= 0) {
        return {};
    }
    return { width, height };
}

/** The menu of the menu bar called @p name, as the .rc file names it. */
QMenu *menuBarMenu(MainWindow *window, const QString &name)
{
    // The description gives every menu a name and KXMLGUI puts that name on the
    // QMenu it builds, so this is the same string the .rc file uses: file,
    // edit, pages, forms, tools, document, view.
    const QList<QMenu *> menus = window->menuBar()->findChildren<QMenu *>(QString(), Qt::FindDirectChildrenOnly);
    for (QMenu *menu : menus) {
        if (menu->objectName().compare(name, Qt::CaseInsensitive) == 0) {
            return menu;
        }
    }

    // A menu whose name was never set can still be found by what it says, with
    // the keyboard accelerator taken back out of the title.
    for (QMenu *menu : menus) {
        if (menu->title().remove(QLatin1Char('&')).compare(name, Qt::CaseInsensitive) == 0) {
            return menu;
        }
    }
    return nullptr;
}

/** Every menu the menu bar offers, for a message that lists the choices. */
QStringList menuBarMenuNames(MainWindow *window)
{
    QStringList names;
    const QList<QMenu *> menus = window->menuBar()->findChildren<QMenu *>(QString(), Qt::FindDirectChildrenOnly);
    for (const QMenu *menu : menus) {
        names += menu->objectName().isEmpty() ? menu->title().remove(QLatin1Char('&')) : menu->objectName();
    }
    return names;
}

/** Shuts anything that has been popped up, so one run does not affect the next. */
void closePopups()
{
    while (QWidget *popup = QApplication::activePopupWidget()) {
        popup->close();
        settle(10);
    }
}

/**
 * Takes the window apart in the order it can survive.
 *
 * Never through close(). KMainWindow gives itself Qt::WA_DeleteOnClose, so
 * closing it posts a deleteLater against the window, and any window this
 * program did not put on the heap is then freed from the middle of the event
 * loop. It is also why an editing tool is taken down first: a window destroyed
 * with a mode still up runs that mode's teardown against a half-destroyed
 * window, which is the same order the test suite's own cleanup uses.
 */
void dismantle(std::unique_ptr<MainWindow> &window)
{
    if (!window) {
        return;
    }
    closePopups();
    if (auto *dock = window->findChild<QDockWidget *>(QStringLiteral("tool_dock"))) {
        if (dock->isVisible()) {
            dock->close();
        }
    }
    window.reset();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

/**
 * Carries out one --step, or says why it could not.
 *
 * @returns an empty string when the step was carried out, and the complaint to
 * print otherwise.
 */
QString applyStep(MainWindow *window, const QString &spec, int settleMilliseconds)
{
    const qsizetype colon = spec.indexOf(QLatin1Char(':'));
    if (colon <= 0) {
        return QStringLiteral("step \"%1\" is not of the form verb:argument").arg(spec);
    }
    const QString verb = spec.left(colon);
    const QString argument = spec.mid(colon + 1);

    if (verb == QLatin1String("action") || verb == QLatin1String("on") || verb == QLatin1String("off")) {
        QAction *action = window->actionCollection()->action(argument);
        if (!action) {
            return QStringLiteral("step \"%1\": the window has no action called %2").arg(spec, argument);
        }
        if (!action->isEnabled()) {
            return QStringLiteral("step \"%1\": %2 is greyed out in the state the window is in").arg(spec, argument);
        }

        // on: and off: say what the window should look like; action: says what
        // to press. They matter for the switches, where the two are not the
        // same thing: the thumbnails panel is up when the window opens, so a
        // recipe that presses show_thumbnails to show it in fact hides it, and
        // the picture is of the panel missing. Anything that is not a switch
        // has only one behaviour and takes action:.
        if (verb != QLatin1String("action")) {
            if (!action->isCheckable()) {
                return QStringLiteral("step \"%1\": %2 is not a switch, so it can only be pressed; use action:%2")
                    .arg(spec, argument);
            }
            const bool wanted = verb == QLatin1String("on");
            if (action->isChecked() == wanted) {
                return {};
            }
        }

        action->trigger();
        settle(settleMilliseconds);
        return {};
    }

    if (verb == QLatin1String("page")) {
        bool number = false;
        const int page = argument.toInt(&number);
        if (!number || page < 1) {
            return QStringLiteral("step \"%1\": pages are counted from 1").arg(spec);
        }

        auto *view = window->findChild<PageView *>();
        if (!view) {
            return QStringLiteral("step \"%1\": the window has no document view to turn").arg(spec);
        }
        view->goToPage(page - 1);

        // Arranging pages shows the grid rather than the document, and there
        // "which page" means which cell is picked out. Only when the grid is
        // the view on screen, though: selecting a page in a grid nobody is
        // looking at still puts "1 selected" in the status bar of the picture.
        auto *grid = window->findChild<PageGridView *>();
        auto *stack = window->findChild<QStackedWidget *>();
        if (grid && stack && stack->currentWidget() == grid && grid->model()
            && page - 1 < grid->model()->rowCount()) {
            grid->selectRows({ page - 1 });
            grid->scrollTo(grid->model()->index(page - 1, 0));
        }
        settle(settleMilliseconds);
        return {};
    }

    if (verb == QLatin1String("wait")) {
        bool number = false;
        const int milliseconds = argument.toInt(&number);
        if (!number || milliseconds < 0) {
            return QStringLiteral("step \"%1\": wait takes a number of milliseconds").arg(spec);
        }
        settle(milliseconds);
        return {};
    }

    if (verb == QLatin1String("resize")) {
        const QSize size = parseSize(argument);
        if (!size.isValid()) {
            return QStringLiteral("step \"%1\": a size is written WIDTHxHEIGHT, as in 1280x800").arg(spec);
        }
        window->resize(size);
        settle(settleMilliseconds);
        return {};
    }

    if (verb == QLatin1String("menu")) {
        QMenu *menu = menuBarMenu(window, argument);
        if (!menu) {
            return QStringLiteral("step \"%1\": the menu bar has no %2; it offers %3")
                .arg(spec, argument, menuBarMenuNames(window).join(QStringLiteral(", ")));
        }

        QAction *owner = nullptr;
        const QList<QAction *> entries = window->menuBar()->actions();
        for (QAction *entry : entries) {
            if (entry->menu() == menu) {
                owner = entry;
                break;
            }
        }
        if (!owner) {
            return QStringLiteral("step \"%1\": %2 is not on the menu bar").arg(spec, argument);
        }

        // popup() rather than exec(): exec() runs an event loop of its own and
        // does not return until the menu is dismissed, so the shutter would
        // never be reached. popup() puts the menu on screen and comes straight
        // back, which is exactly what a picture of an open menu needs.
        const QRect where = window->menuBar()->actionGeometry(owner);
        menu->popup(window->menuBar()->mapToGlobal(where.bottomLeft()));
        settle(settleMilliseconds);
        if (!menu->isVisible()) {
            return QStringLiteral("step \"%1\": the %2 menu was asked to open and did not").arg(spec, argument);
        }
        return {};
    }

    if (verb == QLatin1String("select-field")) {
        auto *fields = window->findChild<FormOverlay *>();
        if (!fields) {
            return QStringLiteral("step \"%1\": the window has no form layer").arg(spec);
        }
        const QVector<FormField> &known = fields->fields();
        for (int i = 0; i < known.size(); ++i) {
            if (known.at(i).name == argument) {
                fields->selectField(i);
                settle(settleMilliseconds);
                return {};
            }
        }

        QStringList names;
        for (const FormField &field : known) {
            names += field.name;
        }
        return QStringLiteral("step \"%1\": this document has no field called %2; it has %3")
            .arg(spec, argument,
                 names.isEmpty() ? QStringLiteral("none at all") : names.join(QStringLiteral(", ")));
    }

    return QStringLiteral("step \"%1\": there is no such step as %2").arg(spec, verb);
}

/**
 * The picture itself.
 *
 * Three ways of framing it, because three different things get photographed:
 *
 *  - window: the whole application window, and nothing that floats over it. A
 *    menu opened with menu: is a window of its own and will be missing.
 *  - active: whatever is on top, which is the popped-up menu or the dialog. Its
 *    own geometry, with no drop shadow and no desktop behind it.
 *  - both: the window, with anything floating over it drawn in where it
 *    actually sits. This is the one that makes a picture of an open menu look
 *    like a picture of the program rather than of a floating rectangle, so it
 *    is what every menu: shot should use, and it is right for a dialog too.
 */
QPixmap takeShot(MainWindow *window, const QString &what)
{
    QWidget *floating = QApplication::activePopupWidget();
    if (!floating) {
        floating = QApplication::activeModalWidget();
    }

    if (what == QLatin1String("active")) {
        QWidget *top = floating;
        if (!top) {
            top = QApplication::activeWindow();
        }
        if (!top) {
            top = window;
        }
        return top->grab();
    }

    QPixmap shot = window->grab();

    if (what == QLatin1String("both") && floating && floating != window) {
        const QPixmap over = floating->grab();
        const QPoint at = floating->mapToGlobal(QPoint(0, 0)) - window->mapToGlobal(QPoint(0, 0));
        QPainter painter(&shot);
        painter.drawPixmap(at, over);
    }

    return shot;
}

} // namespace

int main(int argc, char **argv)
{
    // PDF is written in the C locale and QPDF parses it with strtod, so on a
    // German or French system every fractional number in every document would
    // read as zero. The same line, and the same reason, as in src/main.cpp.
    std::setlocale(LC_NUMERIC, "C");

    // A profile of its own, made now and thrown away at the end.
    //
    // The developer's own has no business in a handbook picture: their recent
    // files would be in the File menu, their toolbar rearrangements on the
    // toolbars, their colour scheme everywhere. What matters as much is that
    // the same command run twice has to give the same picture, and any profile
    // that is kept drifts, because the window writes its state back into it.
    //
    // Set through the environment rather than with QStandardPaths test mode
    // because that mode points at one fixed directory which the test suite
    // uses too, so runs of the tests and runs of this would show up in each
    // other's pictures. Before QApplication, so that nothing has resolved a
    // path yet.
    QTemporaryDir profile;
    if (!profile.isValid()) {
        err() << QStringLiteral("pdf-smithy-shot: no temporary directory to keep a clean profile in") << Qt::endl;
        err().flush();
        return 1;
    }
    for (const auto &[variable, folder] : { std::pair { "XDG_CONFIG_HOME", "config" },
                                            std::pair { "XDG_DATA_HOME", "data" },
                                            std::pair { "XDG_STATE_HOME", "state" },
                                            std::pair { "XDG_CACHE_HOME", "cache" } }) {
        const QString path = profile.filePath(QLatin1String(folder));
        QDir().mkpath(path);
        qputenv(variable, QFile::encodeName(path));
    }

    QApplication application(argc, argv);

    KLocalizedString::setApplicationDomain(PS_NAME);

    // The same identity the application gives itself. The window asks for it
    // while it is being built, and XMLGUI looks the menu and toolbar
    // description up under the running program's name, so without this the
    // window would come up with no menus and no toolbars at all.
    KAboutData about(QStringLiteral(PS_NAME), QStringLiteral(PS_DISPLAY), QStringLiteral(PS_VERSION),
                     i18n("Edit, merge, split, sign and OCR PDF documents, entirely on your own machine."),
                     KAboutLicense::GPL_V3, i18n("© 2026 Tom Bueng"), QString(),
                     QStringLiteral("https://github.com/tombueng/pdf-smithy"));
    about.addAuthor(QStringLiteral("Tom Bueng"), i18nc("@info:credit", "Author and maintainer"),
                    QStringLiteral("tombueng@gmail.com"));
    about.setDesktopFileName(QStringLiteral(PS_APPID));
    about.setOrganizationDomain(QByteArrayLiteral("github.io"));
    KAboutData::setApplicationData(about);

    // The offscreen platform has no desktop to ask, so nothing tells Qt which
    // icon theme is in force and every toolbar button comes out blank. Breeze
    // is what the application is drawn for and what the handbook shows.
    if (QIcon::themeName().isEmpty() || QIcon::themeName() == QLatin1String("hicolor")) {
        QIcon::setThemeName(QStringLiteral("breeze"));
    }
    QApplication::setWindowIcon(QIcon::fromTheme(QStringLiteral(PS_APPID)));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Drives the PDF Smithy window into a named state and photographs it."));
    parser.addHelpOption();

    const QCommandLineOption openOption(QStringLiteral("open"),
                                        QStringLiteral("The document to open. Without it the empty window is taken."),
                                        QStringLiteral("file"));
    const QCommandLineOption outOption(QStringLiteral("out"), QStringLiteral("Where to write the PNG."),
                                       QStringLiteral("png"));
    const QCommandLineOption sizeOption(QStringLiteral("size"), QStringLiteral("Window size, as WIDTHxHEIGHT."),
                                        QStringLiteral("WxH"), QStringLiteral("1280x800"));
    const QCommandLineOption stepOption(
        QStringLiteral("step"),
        QStringLiteral("A step to carry out, repeatable, in the order given. One of "
                       "action:NAME, on:NAME, off:NAME, page:N, menu:NAME, select-field:NAME, "
                       "resize:WxH, wait:MS."),
        QStringLiteral("spec"));
    const QCommandLineOption grabOption(QStringLiteral("grab"),
                                        QStringLiteral("What to photograph: window, active or both."),
                                        QStringLiteral("what"), QStringLiteral("window"));
    const QCommandLineOption settleOption(
        QStringLiteral("settle"), QStringLiteral("Milliseconds to let the window work after each step."),
        QStringLiteral("ms"), QStringLiteral("250"));
    const QCommandLineOption finalSettleOption(
        QStringLiteral("final-settle"),
        QStringLiteral("Milliseconds to wait before the shutter, for pages still being drawn."),
        QStringLiteral("ms"), QStringLiteral("600"));
    const QCommandLineOption dialogOption(
        QStringLiteral("answer-dialogs"),
        QStringLiteral("What a modal dialog is: leave means it is the subject and gets photographed "
                       "where it stands, reject means it is in the way and gets refused."),
        QStringLiteral("leave|reject"), QStringLiteral("leave"));
    const QCommandLineOption timeoutOption(QStringLiteral("timeout"),
                                           QStringLiteral("Seconds before a wedged run kills itself."),
                                           QStringLiteral("seconds"), QStringLiteral("120"));

    parser.addOption(openOption);
    parser.addOption(outOption);
    parser.addOption(sizeOption);
    parser.addOption(stepOption);
    parser.addOption(grabOption);
    parser.addOption(settleOption);
    parser.addOption(finalSettleOption);
    parser.addOption(dialogOption);
    parser.addOption(timeoutOption);

    if (!parser.parse(QApplication::arguments())) {
        err() << parser.errorText() << Qt::endl << Qt::endl << parser.helpText();
        err().flush();
        return 1;
    }
    if (parser.isSet(QStringLiteral("help"))) {
        out() << parser.helpText();
        out().flush();
        return 0;
    }

    if (!parser.isSet(outOption)) {
        return fail(QStringLiteral("--out is required: there is nowhere to put the picture"));
    }
    const QString outPath = parser.value(outOption);

    const QSize size = parseSize(parser.value(sizeOption));
    if (!size.isValid()) {
        return fail(QStringLiteral("--size %1 is not a size; write it as WIDTHxHEIGHT").arg(parser.value(sizeOption)));
    }

    const QString grabWhat = parser.value(grabOption);
    if (grabWhat != QLatin1String("window") && grabWhat != QLatin1String("active")
        && grabWhat != QLatin1String("both")) {
        return fail(QStringLiteral("--grab %1 is not one of window, active or both").arg(grabWhat));
    }

    const QString dialogs = parser.value(dialogOption);
    if (dialogs != QLatin1String("leave") && dialogs != QLatin1String("reject")) {
        return fail(QStringLiteral("--answer-dialogs %1 is not one of leave or reject").arg(dialogs));
    }

    bool number = false;
    const int settleMs = parser.value(settleOption).toInt(&number);
    if (!number || settleMs < 0) {
        return fail(QStringLiteral("--settle takes a number of milliseconds"));
    }
    const int finalSettleMs = parser.value(finalSettleOption).toInt(&number);
    if (!number || finalSettleMs < 0) {
        return fail(QStringLiteral("--final-settle takes a number of milliseconds"));
    }
    const int timeoutSeconds = parser.value(timeoutOption).toInt(&number);
    if (!number || timeoutSeconds <= 0) {
        return fail(QStringLiteral("--timeout takes a number of seconds"));
    }

    const Watchdog watchdog(QFileInfo(outPath).fileName().toUtf8(), timeoutSeconds);

    // Only when it is asked for. A picture of a dialog needs the dialog to
    // still be there when the shutter goes.
    std::unique_ptr<ModalCloser> closer;
    if (dialogs == QLatin1String("reject")) {
        closer = std::make_unique<ModalCloser>();
    }

    // ps_gui is a static archive, and the description of the menus and
    // toolbars reaches it through a Qt resource. A resource in an archive is
    // linked in only when something outside it is referenced, so this is what
    // makes the window come up with menus rather than bare.
    Q_INIT_RESOURCE(resources);

    auto window = std::make_unique<MainWindow>();

    // Every way out of here goes through this, so that a run that gives up
    // halfway leaves the window exactly as tidily as a run that succeeds.
    const auto giveUp = [&window](const QString &what) {
        dismantle(window);
        return fail(what);
    };

    window->resize(size);
    window->show();
    if (!waitForExposed(window.get())) {
        return giveUp(QStringLiteral("the window never appeared, so there is nothing to photograph"));
    }
    settle(settleMs);

    if (parser.isSet(openOption)) {
        const QString file = parser.value(openOption);
        if (!QFileInfo::exists(file)) {
            return giveUp(QStringLiteral("there is no file at %1").arg(file));
        }
        if (!window->openFile(file)) {
            return giveUp(QStringLiteral("%1 could not be opened").arg(file));
        }
        settle(settleMs);
    }

    const QStringList steps = parser.values(stepOption);
    for (const QString &step : steps) {
        const QString complaint = applyStep(window.get(), step, settleMs);
        if (!complaint.isEmpty()) {
            return giveUp(complaint);
        }
    }

    settle(finalSettleMs);

    const QPixmap shot = takeShot(window.get(), grabWhat);
    if (shot.isNull()) {
        return giveUp(QStringLiteral("the grab came back empty"));
    }

    const QFileInfo target(outPath);
    if (!target.absoluteDir().mkpath(QStringLiteral("."))) {
        return giveUp(QStringLiteral("%1 could not be created").arg(target.absolutePath()));
    }
    if (!shot.save(outPath, "PNG")) {
        return giveUp(QStringLiteral("%1 could not be written").arg(outPath));
    }

    dismantle(window);

    out() << outPath << QStringLiteral(" %1x%2").arg(shot.width()).arg(shot.height()) << Qt::endl;
    out().flush();
    return 0;
}
