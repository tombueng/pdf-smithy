/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "TestPdf.h"

#include "core/Document.h"
#include "render/PopplerBackend.h"
#include "ui/ToolDialogs.h"
#include "ui/ToolSupport.h"

#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QFile>
#include <QPlainTextEdit>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QTreeWidget>
#include <QWidget>

#include <KLocalizedString>

#include <qpdf/QPDF.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>
#include <qpdf/QPDFWriter.hh>

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

using namespace ps;

namespace {

/** One value per entry point in ToolDialogs.h. */
enum class Tool { Preflight, Colour, Fonts, Images, Layout, FormBuilder, Objects, Typeset };

void openTool(Tool tool, Document *document, RenderBackend *backend, QWidget *parent)
{
    switch (tool) {
    case Tool::Preflight:
        tools::showPreflight(document, parent);
        return;
    case Tool::Colour:
        tools::showColour(document, parent);
        return;
    case Tool::Fonts:
        tools::showFonts(document, parent);
        return;
    case Tool::Images:
        tools::showImages(document, parent);
        return;
    case Tool::Layout:
        tools::showLayout(document, parent);
        return;
    case Tool::FormBuilder:
        tools::showFormBuilder(document, backend, 0, parent);
        return;
    case Tool::Objects:
        tools::showObjects(document, backend, 0, parent);
        return;
    case Tool::Typeset:
        tools::showTypeset(document, parent);
        return;
    }
}

/**
 * Kills the process rather than let a tool wedge the whole suite.
 *
 * The timer in runTool() rescues anything that is still delivering events. What
 * it cannot rescue is a tool that blocks somewhere outside the event loop, and
 * one test that hangs is how a suite stops being run at all, so that case is
 * turned into a crash, which at least names the case it died in.
 */
class Watchdog
{
public:
    Watchdog()
        : m_thread([this] {
            std::unique_lock<std::mutex> lock(m_mutex);
            if (!m_wake.wait_for(lock, std::chrono::seconds(90), [this] { return m_finished; })) {
                qFatal("a tool stopped responding and was still blocking after ninety seconds");
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
    std::mutex m_mutex;
    std::condition_variable m_wake;
    bool m_finished = false;

    // Last, so that everything it touches is built before it starts.
    std::thread m_thread;
};

/** What one opening of a tool left behind to be judged. */
struct Sighting {
    /** The tool put its own window up, and that window was shut again. */
    bool appeared = false;

    /** Message boxes and other windows the tool opened on top of its own. */
    int asides = 0;

    /** Tabs on the window's tab widget, or -1 when it has none. */
    int tabs = -1;

    /** A window with no caption is one nothing can name in a task list. */
    bool titled = false;
};

/**
 * Runs @p show, looks at whatever it opens, and shuts it again.
 *
 * Every entry point in ToolDialogs.h ends in QDialog::exec(), so a test that
 * merely calls one is waiting for a user who will never arrive. The way in is a
 * timer armed beforehand: it fires inside whichever event loop happens to be
 * running and dismisses the top of the modal stack, which unwedges a tool that
 * greets a damaged file with a message box just as surely as one that opens
 * cleanly.
 *
 * The tool's own window is the modal widget whose parent is @p parent; anything
 * else on the stack was put there by the tool and is counted as an aside. That
 * telling-apart only works because @p parent is a bare widget rather than a
 * MainWindow: ToolSupport asks to save first only when it can find a window to
 * save through, so with a bare parent nothing else can appear at that level.
 */
Sighting runTool(QWidget *parent, const std::function<void()> &show, const std::function<void(QDialog *)> &look = {})
{
    Sighting seen;
    Watchdog watchdog;

    // Which window was dismissed on the previous tick, so that a window slow to
    // go away is not counted twice. Comparing pointers is enough because these
    // windows are strictly nested: the next one cannot exist until this one has
    // gone.
    QWidget *previous = nullptr;

    QTimer closer;
    closer.setInterval(5);
    QObject::connect(&closer, &QTimer::timeout, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (!dialog) {
            return;
        }

        if (dialog != previous) {
            previous = dialog;
            if (dialog->parentWidget() == parent && !seen.appeared) {
                seen.appeared = true;
                seen.titled = !dialog->windowTitle().isEmpty();
                const auto *tabs = dialog->findChild<QTabWidget *>();
                seen.tabs = tabs ? tabs->count() : -1;
                if (look) {
                    look(dialog);
                }
            } else if (dialog->parentWidget() != parent) {
                ++seen.asides;
            }
        }
        dialog->reject();
    });
    closer.start();

    show();

    closer.stop();
    // Message boxes are handed to deleteLater, so without this they are still
    // top-level widgets when the caller counts what is left on screen.
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    return seen;
}

/** Windows still on screen once a tool has been dismissed, named by class. */
QStringList strayWindows()
{
    QStringList left;
    const QList<QWidget *> tops = QApplication::topLevelWidgets();
    for (const QWidget *widget : tops) {
        if (widget->isVisible() && qobject_cast<const QDialog *>(widget)) {
            left += QString::fromLatin1(widget->metaObject()->className());
        }
    }
    return left;
}

/**
 * The first table in @p dialog with @p columns columns.
 *
 * Tools carry several tables and their headings are translated, so the shape of
 * a table is the only thing a test may recognise it by.
 */
QTableWidget *tableWith(const QWidget *dialog, int columns)
{
    const QList<QTableWidget *> tables = dialog->findChildren<QTableWidget *>();
    for (QTableWidget *table : tables) {
        if (table->columnCount() == columns) {
            return table;
        }
    }
    return nullptr;
}

/**
 * A one-page document that draws two small pictures.
 *
 * The generated sample documents are text and nothing else, so the picture tool
 * has nothing whatever to say about them. This is the smallest fixture that
 * gives it two placements to list.
 */
bool writePicturePdf(const QString &path)
{
    try {
        QPDF pdf;
        pdf.emptyPDF();

        // Raw samples rather than an encoded picture: the tool is being asked
        // what is on the page, not to decode anything.
        const auto picture = [&pdf](char value) {
            QPDFObjectHandle stream = QPDFObjectHandle::newStream(&pdf, std::string(8 * 8 * 3, value));
            QPDFObjectHandle dict = stream.getDict();
            dict.replaceKey("/Type", QPDFObjectHandle::newName("/XObject"));
            dict.replaceKey("/Subtype", QPDFObjectHandle::newName("/Image"));
            dict.replaceKey("/Width", QPDFObjectHandle::newInteger(8));
            dict.replaceKey("/Height", QPDFObjectHandle::newInteger(8));
            dict.replaceKey("/ColorSpace", QPDFObjectHandle::newName("/DeviceRGB"));
            dict.replaceKey("/BitsPerComponent", QPDFObjectHandle::newInteger(8));
            return stream;
        };

        QPDFObjectHandle xobjects = QPDFObjectHandle::newDictionary();
        xobjects.replaceKey("/ImA", picture('\x40'));
        xobjects.replaceKey("/ImB", picture('\x7f'));
        QPDFObjectHandle resources = QPDFObjectHandle::newDictionary();
        resources.replaceKey("/XObject", xobjects);

        QPDFObjectHandle page = QPDFObjectHandle::parse("<< /Type /Page /MediaBox [0 0 612 792] >>");
        page.replaceKey("/Resources", resources);
        page.replaceKey("/Contents",
                        QPDFObjectHandle::newStream(&pdf,
                                                    std::string("q 96 0 0 96 72 600 cm /ImA Do Q\n"
                                                                "q 48 0 0 48 72 400 cm /ImB Do Q\n")));

        QPDFPageDocumentHelper documents(pdf);
        documents.addPage(QPDFPageObjectHelper(pdf.makeIndirectObject(page)), false);

        QPDFWriter writer(pdf);
        writer.setOutputFilename(QFile::encodeName(path).constData());
        writer.setDeterministicID(true);
        writer.write();
        return true;
    } catch (const std::exception &) {
        return false;
    }
}

/**
 * One data row per tool, with the shape each one promises.
 *
 * The tab counts are asserted rather than the tab names because the names are
 * translated and this test runs twice, once in German. A count going wrong is
 * the failure that matters anyway: it means a tab was dropped or a whole panel
 * silently built nothing.
 */
void addToolRows()
{
    QTest::addColumn<int>("tool");
    QTest::addColumn<int>("tabs");
    QTest::addColumn<bool>("needsAFile");

    QTest::newRow("preflight") << static_cast<int>(Tool::Preflight) << -1 << true;
    QTest::newRow("colour") << static_cast<int>(Tool::Colour) << 6 << true;
    QTest::newRow("fonts") << static_cast<int>(Tool::Fonts) << 5 << true;
    QTest::newRow("pictures") << static_cast<int>(Tool::Images) << 4 << true;
    QTest::newRow("layout") << static_cast<int>(Tool::Layout) << 8 << true;
    QTest::newRow("form-builder") << static_cast<int>(Tool::FormBuilder) << 5 << true;
    QTest::newRow("objects") << static_cast<int>(Tool::Objects) << 2 << true;

    // Setting text reads the words in front of it rather than the file, which
    // is why it is the one tool that opens on a document with nothing in it.
    QTest::newRow("typeset") << static_cast<int>(Tool::Typeset) << 3 << false;
}

} // namespace

/**
 * The eight editing tools, opened for real and shut again.
 *
 * Nine thousand lines of interface behind eight function calls, and every one of
 * them ends in a modal loop, which is exactly why none of it was covered. The
 * three things that can go wrong here are all silent: a tool that builds no
 * widgets and shows an empty shell, a tool that walks past an empty savedPath()
 * and works on nothing, and a tool that meets a file it cannot read and takes
 * the process down with it. Each is asserted for all eight.
 */
class TestTools : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void cleanup();

    void everyToolOpensAndBuildsItsWidgets_data();
    void everyToolOpensAndBuildsItsWidgets();

    void everyToolRefusesToWorkWithoutAFile_data();
    void everyToolRefusesToWorkWithoutAFile();

    void everyToolSurvivesADamagedFile_data();
    void everyToolSurvivesADamagedFile();

    void thePreflightWindowIsBuiltWithoutTabs();
    void theFontTableListsWhatTheDocumentUses();
    void thePictureTableListsWhatThePagesDraw();
    void theFormBuilderListsTheFieldsAlreadyThere();
    void theObjectListShowsWhatThePageDraws();
    void theReportWindowShowsWhatItWasGiven();

private:
    QTemporaryDir m_dir;
    QString m_file;
    QString m_broken;
    QString m_form;
    QString m_pictures;

    /**
     * A bare widget, deliberately not a MainWindow.
     *
     * ToolSupport only offers to save a document when it can find a window to
     * save it through, so hanging the tools off a plain widget is what makes
     * "the user declined to save" reachable without a dialog to answer.
     */
    QWidget m_parent;
};

void TestTools::initTestCase()
{
    KLocalizedString::setApplicationDomain("pdf-smithy");
    // Keep the developer's real configuration out of it.
    QStandardPaths::setTestModeEnabled(true);

    QVERIFY(m_dir.isValid());

    m_file = m_dir.filePath(QStringLiteral("three.pdf"));
    QVERIFY(test::writeSamplePdf(m_file, 3));

    m_broken = m_dir.filePath(QStringLiteral("broken.pdf"));
    QVERIFY(test::writeBrokenPdf(m_broken));

    m_form = m_dir.filePath(QStringLiteral("form.pdf"));
    QVERIFY(test::writeFormPdf(m_form));

    m_pictures = m_dir.filePath(QStringLiteral("pictures.pdf"));
    QVERIFY(writePicturePdf(m_pictures));
}

void TestTools::cleanup()
{
    // A test that leaves a window up poisons the next one, and the failure then
    // shows up in the wrong place entirely.
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    const QStringList left = strayWindows();
    QVERIFY2(left.isEmpty(), qPrintable(QStringLiteral("still on screen: %1").arg(left.join(QStringLiteral(", ")))));
    QVERIFY2(!QApplication::activeModalWidget(), "a modal window outlived the test that opened it");
}

// ── All eight, on a document worth working on ─────────────────────────────

void TestTools::everyToolOpensAndBuildsItsWidgets_data()
{
    addToolRows();
}

void TestTools::everyToolOpensAndBuildsItsWidgets()
{
    QFETCH(int, tool);
    QFETCH(int, tabs);

    // Declared first so that it outlives the document that renders through it.
    PopplerBackend backend;
    Document document;
    document.setRenderBackend(&backend);

    QString error;
    QVERIFY2(document.open(m_file, &error), qPrintable(error));

    const Sighting seen
        = runTool(&m_parent, [&] { openTool(static_cast<Tool>(tool), &document, &backend, &m_parent); });

    QVERIFY2(seen.appeared, "the tool opened no window at all on a document it can work on");
    QVERIFY2(seen.titled, "the tool's window carries no caption, so nothing can name it to the user");
    QVERIFY2(seen.asides == 0,
             "the tool complained about a document that is perfectly sound, which trains the user to click past it");

    // This is the assertion that catches a panel whose widgets were never
    // reached: the window comes up, it is the right size, and it is empty.
    QCOMPARE(seen.tabs, tabs);

    const QStringList left = strayWindows();
    QVERIFY2(left.isEmpty(), qPrintable(QStringLiteral("left behind: %1").arg(left.join(QStringLiteral(", ")))));
}

// ── All eight, with nothing to work on ────────────────────────────────────

void TestTools::everyToolRefusesToWorkWithoutAFile_data()
{
    addToolRows();
}

void TestTools::everyToolRefusesToWorkWithoutAFile()
{
    QFETCH(int, tool);
    QFETCH(bool, needsAFile);

    PopplerBackend backend;

    // Nothing opened at all. savedPath() returns empty for a document with no
    // pages, and a tool that carried on regardless would be reading a PDF at
    // the empty path.
    Document empty;
    empty.setRenderBackend(&backend);
    QCOMPARE(empty.pageCount(), 0);

    const Sighting onNothing
        = runTool(&m_parent, [&] { openTool(static_cast<Tool>(tool), &empty, &backend, &m_parent); });
    QCOMPARE(onNothing.appeared, !needsAFile);
    QVERIFY2(onNothing.asides == 0, "the tool put a message up about a document the user has not started yet");

    // Pages, but never written down. There is no window to save them through,
    // so savedPath() comes back empty again: the same refusal a user gets by
    // answering no, and the same place the tool has to stop.
    Document unsaved;
    unsaved.setRenderBackend(&backend);
    QString error;
    QVERIFY2(unsaved.importFile(m_file, -1, &error), qPrintable(error));
    QCOMPARE(unsaved.pageCount(), 3);
    QVERIFY2(unsaved.filePath().isEmpty(), "the fixture is meant to stand for a document that was never saved");

    const Sighting onUnsaved
        = runTool(&m_parent, [&] { openTool(static_cast<Tool>(tool), &unsaved, &backend, &m_parent); });
    QVERIFY2(onUnsaved.appeared == !needsAFile,
             "a tool that reads the file went on to open its window without one, so it is working on nothing");

    const QStringList left = strayWindows();
    QVERIFY2(left.isEmpty(), qPrintable(QStringLiteral("left behind: %1").arg(left.join(QStringLiteral(", ")))));
}

// ── All eight, on a file that cannot be read ──────────────────────────────

void TestTools::everyToolSurvivesADamagedFile_data()
{
    addToolRows();
}

void TestTools::everyToolSurvivesADamagedFile()
{
    QFETCH(int, tool);
    QFETCH(int, tabs);

    PopplerBackend backend;
    Document document;
    document.setRenderBackend(&backend);

    QString error;
    QVERIFY2(document.open(m_file, &error), qPrintable(error));

    // Only the path the tool will read is pointed at rubbish; the model itself
    // stays sound. That is the situation a tool actually meets, because it works
    // on what is on disk and what is on disk is not always what it was told.
    document.setFilePath(m_broken);
    QVERIFY2(!document.isModified(), "pointing the document elsewhere must not look like an edit");

    const Sighting seen
        = runTool(&m_parent, [&] { openTool(static_cast<Tool>(tool), &document, &backend, &m_parent); });

    QVERIFY2(seen.appeared, "a damaged file left the tool with no window to say so in");
    QVERIFY2(seen.asides <= 2, "the tool met one damaged file and complained about it more than twice");

    // Still fully built. A tool that meets rubbish must come up empty-handed,
    // not half-constructed, or the user is left with a window whose other tabs
    // do nothing.
    QCOMPARE(seen.tabs, tabs);

    const QStringList left = strayWindows();
    QVERIFY2(left.isEmpty(), qPrintable(QStringLiteral("left behind: %1").arg(left.join(QStringLiteral(", ")))));
}

// ── What each one built, where a count says something ─────────────────────

void TestTools::thePreflightWindowIsBuiltWithoutTabs()
{
    PopplerBackend backend;
    Document document;
    document.setRenderBackend(&backend);
    QString error;
    QVERIFY2(document.open(m_file, &error), qPrintable(error));

    // Preflight is the one tool with no tabs, so "did it build anything" has to
    // be asked of its own widgets: the profile chooser and the four-column list
    // the findings are reported in.
    int profiles = -1;
    int columns = -1;
    const Sighting seen = runTool(
        &m_parent, [&] { tools::showPreflight(&document, &m_parent); },
        [&](QDialog *dialog) {
            if (const auto *chooser = dialog->findChild<QComboBox *>()) {
                profiles = chooser->count();
            }
            if (const auto *findings = dialog->findChild<QTreeWidget *>()) {
                columns = findings->columnCount();
            }
        });

    QVERIFY2(seen.appeared, "preflight opened nothing");
    QVERIFY2(profiles > 0, "preflight offers no profile to check against, so it can check nothing");
    QCOMPARE(columns, 4);
}

void TestTools::theFontTableListsWhatTheDocumentUses()
{
    PopplerBackend backend;
    Document document;
    document.setRenderBackend(&backend);
    QString error;
    QVERIFY2(document.open(m_file, &error), qPrintable(error));

    int rows = -1;
    const Sighting seen = runTool(
        &m_parent, [&] { tools::showFonts(&document, &m_parent); },
        [&](QDialog *dialog) {
            if (const QTableWidget *table = tableWith(dialog, 10)) {
                rows = table->rowCount();
            }
        });

    QVERIFY2(seen.appeared, "the font tool opened nothing");
    QVERIFY2(rows >= 1, "the font table is empty for a document whose every page sets its text in Helvetica");
}

void TestTools::thePictureTableListsWhatThePagesDraw()
{
    PopplerBackend backend;
    Document document;
    document.setRenderBackend(&backend);
    QString error;
    QVERIFY2(document.open(m_pictures, &error), qPrintable(error));

    int rows = -1;
    const Sighting seen = runTool(
        &m_parent, [&] { tools::showImages(&document, &m_parent); },
        [&](QDialog *dialog) {
            if (const QTableWidget *table = tableWith(dialog, 10)) {
                rows = table->rowCount();
            }
        });

    QVERIFY2(seen.appeared, "the picture tool opened nothing");
    // One row per placement, and the fixture places two pictures once each.
    QCOMPARE(rows, 2);
}

void TestTools::theFormBuilderListsTheFieldsAlreadyThere()
{
    PopplerBackend backend;
    Document document;
    document.setRenderBackend(&backend);
    QString error;
    QVERIFY2(document.open(m_form, &error), qPrintable(error));

    int rows = -1;
    const Sighting seen = runTool(
        &m_parent, [&] { tools::showFormBuilder(&document, &backend, 0, &m_parent); },
        [&](QDialog *dialog) {
            if (const QTableWidget *inventory = tableWith(dialog, 5)) {
                rows = inventory->rowCount();
            }
        });

    QVERIFY2(seen.appeared, "the form builder opened nothing");
    // A builder that opens on a form it cannot see is a builder that will
    // cheerfully add a second field of the same name.
    QCOMPARE(rows, 5);
}

void TestTools::theObjectListShowsWhatThePageDraws()
{
    PopplerBackend backend;
    Document document;
    document.setRenderBackend(&backend);
    QString error;
    QVERIFY2(document.open(m_file, &error), qPrintable(error));

    int drawn = -1;
    const Sighting seen = runTool(
        &m_parent, [&] { tools::showObjects(&document, &backend, 0, &m_parent); },
        [&](QDialog *dialog) {
            if (const auto *list = dialog->findChild<QTreeWidget *>()) {
                drawn = list->topLevelItemCount();
            }
        });

    QVERIFY2(seen.appeared, "the object tool opened nothing");
    QVERIFY2(drawn >= 1, "the object list is empty for a page that draws a line of text");
}

void TestTools::theReportWindowShowsWhatItWasGiven()
{
    // Every tool hands its results to this one window, so it is worth knowing
    // that the lines reach it rather than being counted and thrown away. The
    // text is deliberately untranslated so the comparison holds in any locale.
    const QStringList lines { QStringLiteral("PSREPORT one"), QStringLiteral("PSREPORT two") };

    QString shown;
    const Sighting seen = runTool(
        &m_parent, [&] { tools::showReport(&m_parent, QStringLiteral("PSREPORT"), lines, true); },
        [&](QDialog *dialog) {
            if (const auto *body = dialog->findChild<QPlainTextEdit *>()) {
                shown = body->toPlainText();
            }
        });

    QVERIFY2(seen.appeared, "the shared report window did not come up");
    QCOMPARE(shown.split(QLatin1Char('\n')), lines);
}

QTEST_MAIN(TestTools)

#include "tst_tools.moc"
