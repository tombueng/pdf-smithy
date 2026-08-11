/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "MainWindow.h"

#include "DocumentCommands.h"
#include "DocumentProperties.h"
#include "EditorMode.h"
#include "OutlineDock.h"
#include "ThumbnailDock.h"
#include "PageGridView.h"
#include "DocumentDialogs.h"
#include "OperationDialogs.h"
#include "CropEditor.h"
#include "NumberEditor.h"
#include "WatermarkEditor.h"
#include "PageToolDialogs.h"
#include "FormEditor.h"
#include "CompareDialog.h"
#include "RedactEditor.h"
#include "ToolDialogs.h"
#include "PageModel.h"
#include "PageView.h"
#include "AnnotationOverlay.h"
#include "FormDesignOverlay.h"
#include "FormOverlay.h"
#include "ObjectOverlay.h"
#include "InspectorDock.h"
#include "TextOverlay.h"
#include "PageProcessor.h"
#include "RemoteFile.h"
#include "SignEditor.h"
#include "PrintOptionsWidget.h"
#include "ToolSupport.h"
#include "print/PrintController.h"
#include "core/BlankPdf.h"
#include "core/Document.h"
#include "core/DocumentWriter.h"
#include "core/Encryption.h"
#include "core/Forms.h"
#include "core/Archival.h"
#include "core/Compare.h"
#include "core/Convert.h"
#include "core/TextEdit.h"
#include "core/ImageIO.h"
#include "core/Metadata.h"
#include "core/PageCrop.h"
#include "core/PageNumbering.h"
#include "core/Splitter.h"
#include "core/RenderCache.h"
#include "core/Source.h"
#include "core/commands/PageCommands.h"
#include "render/PopplerBackend.h"

#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QPrintDialog>
#include <QPrintPreviewDialog>
#include <QPrinter>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTextStream>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>
#include <QLabel>
#include <QDialogButtonBox>
#include <QMenu>
#include <QMenuBar>
#include <QMimeData>
#include <QSlider>
#include <QSpinBox>
#include <QStackedWidget>
#include <QDockWidget>
#include <QStatusBar>
#include <QVBoxLayout>
#include <QUndoStack>
#include <QTransform>
#include <QUrl>

#include <KActionCollection>
#include <KHamburgerMenu>
#include <KToolBar>
#include <KIconLoader>
#include <KToolBarPopupAction>
#include <KConfigGroup>
#include <KLocalizedString>
#include <KMessageBox>
#include <KMessageWidget>
#include <KPasswordDialog>
#include <KXMLGUIFactory>
#include <KRecentFilesAction>
#include <KSharedConfig>
#include <KStandardAction>

#include <iterator>

/**
 * Makes the window's own resources reach a program that links ps_gui
 * statically.
 *
 * A static library's resource initialiser is pulled in only when something else
 * in its translation unit is referenced, and nothing was: the menu bar and the
 * toolbars live in a .qrc of their own. Every test binary was therefore driving
 * a window with neither, and nothing failed, because the tests reach commands
 * through the action collection, which is exactly why it went unseen so long.
 *
 * Deliberately outside namespace ps: the function Qt generates is global, and
 * Q_INIT_RESOURCE declares it in whatever namespace the macro is written in.
 */
static void keepResources()
{
    Q_INIT_RESOURCE(resources);
}

namespace ps {

namespace {

QString pdfFilter()
{
    return i18n("PDF documents (*.pdf);;All files (*)");
}

QString imageFilter()
{
    return i18n("Images (%1);;All files (*)", ImageIO::readableImageFilters().join(QLatin1Char(' ')));
}

/**
 * Which toolbar layout this build wants, and whether the user has it yet.
 *
 * Kept in step with @c version in pdf-smithyui.rc, and needed because that
 * number reaches less than it looks. It decides which *description* wins, and
 * even there only against a copy the user has not edited. Everything about how
 * the window came up last time (where each bar sits, which panels were open) is
 * remembered somewhere else entirely and survives untouched.
 *
 * So this is the number that says "the arrangement below has changed since the
 * user was last shown one", and raising it undoes the old one exactly once.
 */
constexpr int LayoutGeneration = 17;

/** A mode's own toolbar, in the order they sit on their row. */
struct ModeToolBar {
    const char *name;
    MainWindow::WorkMode mode;
};

constexpr ModeToolBar modeToolBars[] = {
    { "readToolBar", MainWindow::WorkMode::Read },
    { "pageToolBar", MainWindow::WorkMode::Page },
    { "formToolBar", MainWindow::WorkMode::Form },
    { "organiseToolBar", MainWindow::WorkMode::Organise },
};

constexpr int modeToolBarCount = int(std::size(modeToolBars));

} // namespace

MainWindow::MainWindow(QWidget *parent)
    : KXmlGuiWindow(parent)
    , m_document(new Document(this))
    , m_model(new PageModel(this))
    , m_view(new PageGridView(this))
    , m_cache(new RenderCache(this))
    , m_backend(std::make_unique<PopplerBackend>())
{
    keepResources();

    m_document->setRenderBackend(m_backend.get());
    m_cache->setBackend(m_backend.get());
    m_cache->setThumbnailWidth(m_view->thumbnailWidth());
    m_model->setThumbnailWidth(m_view->thumbnailWidth());

    m_model->setDocument(m_document);
    m_model->setRenderCache(m_cache);
    m_view->setModel(m_model);

    m_messageBar = new KMessageWidget(this);
    m_messageBar->setMessageType(KMessageWidget::Error);
    m_messageBar->setCloseButtonVisible(true);
    m_messageBar->setWordWrap(true);
    m_messageBar->hide();

    // The grid is one page of a stack rather than the central widget itself,
    // because editing happens here now: marking a page up puts the editor where
    // the grid was instead of covering the window with a dialog.
    m_stack = new QStackedWidget(this);
    m_stack->addWidget(m_view);

    // The document itself, at reading size. The grid answers "which pages, in
    // what order"; this answers "what does this say and how do I change it",
    // and it is the one people spend their time in.
    m_pageView = new PageView(this);
    m_pageView->setDocument(m_document);
    m_pageView->setRenderBackend(m_backend.get());
    m_stack->addWidget(m_pageView);

    connect(m_pageView, &PageView::currentPageChanged, this, [this](int row) {
        // The grid is moved to the page being read, and *only* moved: reading
        // used to replace whatever was chosen there with a single row, so four
        // pages picked out for rearranging became one the moment the reader
        // scrolled. Which page is current and which pages are chosen are two
        // different facts, and only the first of them belongs to the reader.
        const QModelIndex index = m_model->index(row, 0);
        if (index.isValid()) {
            m_view->selectionModel()->setCurrentIndex(index, QItemSelectionModel::NoUpdate);
            m_view->scrollTo(index);
        }
        updateStatusBar();
        updateActionState();
    });
    connect(m_pageView, &PageView::selectionChanged, this, &MainWindow::updateActionState);
    connect(m_pageView, &PageView::refused, this, [this](const QString &reason) { reportNotice(reason); });
    // The document view is where people spend their time, and right-clicking in
    // it did nothing at all: the signal was emitted and nobody was listening.
    connect(m_pageView, &PageView::contextMenuWanted, this,
            [this](int row, const QPointF &pagePoint, const QPoint &globalPos) {
                Q_UNUSED(row)
                Q_UNUSED(pagePoint)
                showPageContextMenu(globalPos);
            });

    // The layers that make the page more than a picture of itself. Each is a
    // file of its own and knows nothing of the others; the view asks them in
    // turn and the first one that claims a click keeps it.
    m_annotationOverlay = new AnnotationOverlay(m_pageView, m_document, this);
    m_formOverlay = new FormOverlay(m_pageView, this);
    m_textOverlay = new TextOverlay(m_pageView, m_document, this);
    m_objectOverlay = new ObjectOverlay(m_pageView, this);
    m_formDesign = new FormDesignOverlay(m_pageView, this);
    // Without the document these three fall back to assuming a view row is a
    // page number in a file, which holds only until the user deletes, inserts
    // or reorders anything, and then it silently addresses the wrong page.
    m_formOverlay->setDocument(m_document);
    m_formDesign->setDocument(m_document);
    m_objectOverlay->setDocument(m_document);
    // Comments first: while a drawing tool is up it has to win the press even
    // over a form field or a line of text, because that is what the user has
    // just told the program they are doing.
    m_pageView->addOverlay(m_annotationOverlay);
    m_pageView->addOverlay(m_formOverlay);
    // Text before objects: a line of text is also an object, and clicking one
    // in Edit mode should offer to correct the words rather than to move the
    // block, since moving is what the handles round it are for.
    m_pageView->addOverlay(m_textOverlay);
    // Designing the form comes before moving what is drawn: a field sits on top
    // of the page's own artwork, and someone dragging a box out has said which
    // of the two they mean.
    m_pageView->addOverlay(m_formDesign);
    m_pageView->addOverlay(m_objectOverlay);

    m_inspector = new InspectorDock(this);
    m_inspector->setAllowedAreas(Qt::AllDockWidgetAreas);
    m_inspector->addSource(m_annotationOverlay, m_annotationOverlay);
    m_inspector->addSource(m_formOverlay, m_formOverlay);
    m_inspector->addSource(m_textOverlay, m_textOverlay);
    m_inspector->addSource(m_formDesign, m_formDesign);
    m_inspector->addSource(m_objectOverlay, m_objectOverlay);

    // With nothing on a page chosen the panel used to stand empty, which is the
    // whole of Read mode and most of arranging pages. It registers itself and
    // watches the grid's selection: the document when nothing is chosen, the
    // chosen sheets when something is.
    new DocumentProperties(m_document, m_view, m_inspector, this);
    addDockWidget(Qt::RightDockWidgetArea, m_inspector);
    m_inspector->hide();

    connect(m_formOverlay, &FormOverlay::selectionChanged, this, [this] { m_inspector->refreshFrom(m_formOverlay); });
    connect(m_textOverlay, &TextOverlay::inspectionChanged, this, [this] { m_inspector->refreshFrom(m_textOverlay); });
    connect(m_formOverlay, &FormOverlay::valueChanged, this, &MainWindow::pendingWorkChanged);
    connect(m_textOverlay, &TextOverlay::editsChanged, this, &MainWindow::pendingWorkChanged);
    connect(m_objectOverlay, &ObjectOverlay::choiceChanged, this, [this] {
        m_inspector->refreshFrom(m_objectOverlay);
        // What can be done to a picture changes the moment one is chosen, and
        // the commands have to say so at that moment rather than at the next
        // thing that happens to refresh them.
        updateActionState();
    });
    connect(m_objectOverlay, &ObjectOverlay::editsChanged, this, &MainWindow::pendingWorkChanged);
    connect(m_annotationOverlay, &AnnotationOverlay::selectionChanged, this,
            [this] { m_inspector->refreshFrom(m_annotationOverlay); });
    connect(m_annotationOverlay, &AnnotationOverlay::annotationsChanged, this, &MainWindow::pendingWorkChanged);
    connect(m_formDesign, &FormDesignOverlay::choiceChanged, this, [this] { m_inspector->refreshFrom(m_formDesign); });
    connect(m_formDesign, &FormDesignOverlay::editsChanged, this, &MainWindow::pendingWorkChanged);
    connect(m_formDesign, &FormDesignOverlay::refused, this, [this](const QString &reason) { reportNotice(reason); });
    connect(m_formDesign, &FormDesignOverlay::kindToPlaceChanged, this, [this](FormDesignOverlay::Kind kind) {
        if (QAction *action = m_fieldActions.value(int(kind))) {
            action->setChecked(true);
        }
    });
    connect(m_objectOverlay, &ObjectOverlay::refused, this, [this](const QString &reason) {
        // The engine's refusals reach the user at the moment of the gesture,
        // which is the whole point of checking them there.
        reportNotice(reason);
    });
    // These two say what a page change cost: work anchored to a page that has
    // just been deleted cannot be carried anywhere, and losing it without a
    // word is the one outcome that is not allowed.
    connect(m_annotationOverlay, &AnnotationOverlay::refused, this,
            [this](const QString &reason) { reportNotice(reason); });
    connect(m_textOverlay, &TextOverlay::refused, this, [this](const QString &reason) { reportNotice(reason); });

    // Filling in a form needs the form; reading it costs a pass over the file,
    // so it happens when the document arrives rather than on every mode switch.
    connect(m_document, &Document::filePathChanged, this, &MainWindow::loadFormForView);
    connect(m_document, &Document::wasReset, this, &MainWindow::loadFormForView);
    connect(m_document, &Document::wasReset, this, &MainWindow::documentReset);
    connect(m_pageView, &PageView::modeChanged, this, [this](PageView::Mode mode) {
        if (QAction *action = actionCollection()->action(mode == PageView::Mode::Edit ? QStringLiteral("mode_edit")
                                                                                      : QStringLiteral("mode_view"))) {
            action->setChecked(true);
        }
        updateActionState();
    });

    auto *centre = new QWidget(this);
    auto *centreLayout = new QVBoxLayout(centre);
    centreLayout->setContentsMargins(0, 0, 0, 0);
    centreLayout->setSpacing(0);
    centreLayout->addWidget(m_messageBar);
    centreLayout->addWidget(m_stack, 1);

    setCentralWidget(centre);
    setAcceptDrops(true);

    // Panels may be dragged to any edge, torn off to float, and dropped beside
    // or on top of one another. Nesting is what makes "any edge" true: without
    // it a second panel on the same side can only become a tab.
    setDockNestingEnabled(true);
    setCorner(Qt::TopLeftCorner, Qt::LeftDockWidgetArea);
    setCorner(Qt::BottomLeftCorner, Qt::LeftDockWidgetArea);
    setCorner(Qt::TopRightCorner, Qt::RightDockWidgetArea);
    setCorner(Qt::BottomRightCorner, Qt::RightDockWidgetArea);

    // The contents sidebar. Created before setupGUI so that its own
    // show/hide action is there to be placed in the menus.
    // Navigation before anything else: a reader knows where it is by looking
    // at the page beside the one it is on.
    m_thumbnails = new ThumbnailDock(this);
    // Its own model over the same document, for one reason: a model carries the
    // width its view draws at, and the strip draws at a fraction of the grid's
    // size. Sharing one meant the narrow strip rendered every page at the
    // grid's width and threw most of it away at the moment of drawing.
    m_stripModel = new PageModel(this);
    m_stripModel->setDocument(m_document);
    m_stripModel->setRenderCache(m_cache);
    m_stripModel->setThumbnailWidth(m_thumbnails->thumbnailWidth());
    connect(m_thumbnails, &ThumbnailDock::thumbnailWidthChanged, m_stripModel, &PageModel::setThumbnailWidth);
    m_thumbnails->setModel(m_stripModel);
    addDockWidget(Qt::LeftDockWidgetArea, m_thumbnails);

    // The strip had no entry anywhere: closing it left no way back except the
    // dock's own context menu, which is not somewhere anybody looks.
    m_thumbnailsAction = m_thumbnails->toggleViewAction();
    m_thumbnailsAction->setText(i18nc("@action", "Page Thumbnails"));
    m_thumbnailsAction->setIcon(QIcon::fromTheme(QStringLiteral("view-preview")));
    actionCollection()->addAction(QStringLiteral("show_thumbnails"), m_thumbnailsAction);
    actionCollection()->setDefaultShortcut(m_thumbnailsAction, QKeySequence(Qt::Key_F9));

    connect(m_thumbnails, &ThumbnailDock::pageRequested, this, [this](int row) {
        m_pageView->goToPage(row);
        m_view->selectRows({ row });
    });
    connect(m_pageView, &PageView::currentPageChanged, m_thumbnails, &ThumbnailDock::followPage);
    // Two different things, both needed: which page the reader is on, which
    // only changes at a page boundary, and how far through they are, which
    // changes on every pixel. The strip marks with the first and travels with
    // the second: reading down one tall page produces no page change at all for
    // several seconds, and a panel that stands still that long reads as broken.
    connect(m_pageView, &PageView::viewPositionChanged, m_thumbnails, &ThumbnailDock::followPosition);

    m_outlineDock = new OutlineDock(m_document, this);
    m_outlineDock->setAllowedAreas(Qt::AllDockWidgetAreas);
    m_outlineDock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable
                               | QDockWidget::DockWidgetClosable);
    addDockWidget(Qt::LeftDockWidgetArea, m_outlineDock);
    m_outlineDock->hide();

    QAction *toggleOutline = m_outlineDock->toggleViewAction();
    toggleOutline->setText(i18nc("@action", "Contents Sidebar"));
    toggleOutline->setIcon(QIcon::fromTheme(QStringLiteral("view-table-of-contents-ltr")));
    actionCollection()->addAction(QStringLiteral("show_outline"), toggleOutline);
    actionCollection()->setDefaultShortcut(toggleOutline, QKeySequence(Qt::CTRL | Qt::Key_B));

    // The tool panel. One dock reused by every editing mode rather than one
    // dock per tool: the user arranges it once (floating on the second screen,
    // narrow on the right, wherever) and every tool from then on appears where
    // they put it.
    m_toolDock = new QDockWidget(this);
    m_toolDock->setObjectName(QStringLiteral("tool_dock"));
    m_toolDock->setAllowedAreas(Qt::AllDockWidgetAreas);
    m_toolDock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable
                            | QDockWidget::DockWidgetClosable);
    m_toolDock->setWindowTitle(i18nc("@title:window", "Tools"));
    addDockWidget(Qt::RightDockWidgetArea, m_toolDock);
    m_toolDock->hide();

    // Closing the panel is the same as saying "never mind": leaving the stage
    // up with no way to reach its tools would be a trap.
    connect(m_toolDock, &QDockWidget::visibilityChanged, this, [this](bool visible) {
        if (!visible && m_editor && !m_leavingEditor) {
            leaveEditor(false);
        }
    });

    m_inspector->addSource(m_outlineDock, m_outlineDock);
    connect(m_outlineDock, &OutlineDock::inspectionChanged, this, [this] { m_inspector->refreshFrom(m_outlineDock); });

    connect(m_outlineDock, &OutlineDock::pageRequested, this, [this](int row) {
        // A chapter is something you click to go and read, and reading happens
        // in the document. Picking a row in the grid behind it left someone
        // clicking bookmark after bookmark with the page in front of them never
        // moving at all.
        m_pageView->goToPage(row);
        m_view->selectRows({ row });
        m_view->scrollTo(m_model->index(row, 0));
    });

    setupActions();
    setupStatusBar();

    // Before the description is read, because after it there is nothing left to
    // decide: whatever the user's copy said is already on screen.
    discardStaleLayoutDescription();

    // setupGUI wires the .rc file, standard shortcuts, toolbar editing and
    // window-geometry persistence in one go.
    // Wide enough for a toolbar whose buttons carry their names: at large icons
    // with text underneath the reading bar wants about 1260 pixels, and a
    // narrower default put half its tools behind an overflow arrow before the
    // user had touched anything.
    setupGUI(QSize(1400, 900),
             KXmlGuiWindow::ToolBar | KXmlGuiWindow::Keys | KXmlGuiWindow::StatusBar | KXmlGuiWindow::Save
                 | KXmlGuiWindow::Create,
             QStringLiteral("pdf-smithyui.rc"));

    // Only now do the toolbars exist, and only now has the saved window state
    // been laid over them. Both of those have to have happened before the
    // window can say where its bars go and which of them are up.
    applyLayoutDefaults();

    // The menu bar can be hidden, and a window with neither a menu bar nor a
    // way to reach the menus is a trap. This is the modern KDE answer to it and
    // shows itself only when it is needed.
    if (auto *hamburger
        = qobject_cast<KHamburgerMenu *>(actionCollection()->action(QStringLiteral("hamburger_menu")))) {
        hamburger->setMenuBar(menuBar());
        hamburger->setShowMenuBarAction(actionCollection()->action(QStringLiteral("options_show_menubar")));
        if (KToolBar *main = toolBar(QStringLiteral("mainToolBar"))) {
            hamburger->hideActionsOf(main);
        }
    }

    connect(m_document, &Document::modifiedChanged, this, &MainWindow::updateTitle);
    connect(m_document, &Document::filePathChanged, this, &MainWindow::updateTitle);
    // Several commands are only meaningful once the document is on disk, and
    // opening one emits wasReset() before it has a path, so without this they
    // stay greyed out until something unrelated happens.
    connect(m_document, &Document::filePathChanged, this, &MainWindow::updateActionState);

    const auto refresh = [this] {
        updateStatusBar();
        updateActionState();
    };
    connect(m_model, &QAbstractItemModel::rowsInserted, this, refresh);
    connect(m_model, &QAbstractItemModel::rowsRemoved, this, refresh);
    connect(m_model, &QAbstractItemModel::modelReset, this, refresh);
    connect(m_view->selectionModel(), &QItemSelectionModel::selectionChanged, this, refresh);

    connect(m_view, &PageGridView::pageActivated, this, &MainWindow::showPage);

    connect(m_view->selectionModel(), &QItemSelectionModel::currentChanged, this, [this](const QModelIndex &current) {
        if (current.isValid() && m_outlineDock) {
            m_outlineDock->followPage(current.row());
        }
    });

    // A document with bookmarks in it used to throw the contents panel open by
    // itself. Most documents have some, so most documents lost a third of the
    // window to a panel nobody had asked for, and closing it did no good,
    // because the next file opened it again. The panel is a click away in the
    // View menu and on the toolbar, which is where a thing nobody needs by
    // default belongs.

    connect(m_view, &PageGridView::thumbnailWidthChanged, this, [this](int width) {
        m_cache->setThumbnailWidth(width);
        m_model->setThumbnailWidth(width);
        if (m_zoomSlider && m_zoomSlider->value() != width) {
            QSignalBlocker blocker(m_zoomSlider);
            m_zoomSlider->setValue(width);
        }
        // Every way the grid can be resized that is not this window doing it
        // (the slider, Ctrl and the wheel, a size put back from the last
        // session) is the user saying what size they want the grid at, and
        // after that the window stops opening it on the overview.
        if (!m_arrangingGrid) {
            m_gridSizeChosen = true;
        }
    });

    m_processor = new PageProcessor(m_document, m_backend.get(), this);

    m_view->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_view, &QWidget::customContextMenuRequested, this, &MainWindow::showContextMenu);

    // A document is something you open to read. Anything else is a decision.
    setWorkMode(WorkMode::Read);

    restoreSession();
    updateTitle();
    updateStatusBar();
    updateActionState();
}

MainWindow::~MainWindow()
{
    // Before anything else goes. The panel watches every overlay for
    // destruction and answers by rebuilding itself out of the document, and the
    // document's file is closed while those overlays are still being taken
    // apart, so a rebuild there reads a QPDF that is already gone.
    m_inspector->stop();

    // Taking the window apart hides the tool panel, which is wired to leave the
    // editing mode, which asks the action collection about itself, and by then
    // the KXmlGuiWindow underneath has already gone. Nothing needs doing on the
    // way out, so the wire is cut first.
    m_leavingEditor = true;
    if (m_toolDock) {
        m_toolDock->disconnect(this);
    }

    // Before the backend goes. The cache is a QObject child and is therefore
    // destroyed *after* this class's own members, so a page still being drawn
    // would otherwise render into a freed Poppler backend, which it did, on
    // roughly one exit in three under load.
    if (m_cache) {
        m_cache->stop();
    }
    // The document view renders on a pool of its own for the same reason and
    // has to be stopped for the same reason: it holds the backend this class
    // is about to let go of.
    if (m_pageView) {
        m_pageView->stopRendering();
    }
}

// ── Setup ─────────────────────────────────────────────────────────────────

void MainWindow::setupActions()
{
    KActionCollection *actions = actionCollection();

    // Before Open, where every program on the desktop puts it. Its absence was
    // felt as the program only being usable on files that already existed.
    KStandardAction::openNew(this, &MainWindow::newDocument, actions);
    KStandardAction::open(this, &MainWindow::openDocument, actions);
    KStandardAction::save(this, &MainWindow::saveDocument, actions);
    KStandardAction::saveAs(this, &MainWindow::saveDocumentAs, actions);
    KStandardAction::print(this, &MainWindow::printDocument, actions);
    KStandardAction::printPreview(this, &MainWindow::showPrintPreview, actions);
    KStandardAction::close(this, &MainWindow::closeDocument, actions);
    KStandardAction::quit(qApp, &QApplication::closeAllWindows, actions);
    KStandardAction::selectAll(this, &MainWindow::selectAllPages, actions);

    // Cut, copy and paste mean the document when the document is what is on
    // screen. In the grid they have nothing to act on, and the view says so
    // rather than silently doing nothing.
    KStandardAction::cut(this, [this] { m_pageView->cut(); }, actions);
    KStandardAction::copy(this, [this] { m_pageView->copy(); }, actions);
    KStandardAction::paste(this, [this] { m_pageView->paste(); }, actions);

    m_recentFiles
        = KStandardAction::openRecent(this, [this](const QUrl &url) { openFile(url.toLocalFile()); }, actions);

    // The menu bar is a row of words most people read once. Letting it go is
    // only safe because the button below puts every menu back within a click,
    // so the two are added together and never apart.
    KStandardAction::showMenubar(this, [this](bool shown) { menuBar()->setVisible(shown); }, actions);
    KStandardAction::hamburgerMenu(nullptr, nullptr, actions);

    // Undo and redo come straight off the document's stack, so drag-and-drop
    // in the grid and menu commands share one history.
    QAction *undo = m_document->undoStack()->createUndoAction(this);
    undo->setIcon(QIcon::fromTheme(QStringLiteral("edit-undo")));
    actions->setDefaultShortcuts(undo, KStandardShortcut::undo());
    actions->addAction(QStringLiteral("edit_undo"), undo);

    QAction *redo = m_document->undoStack()->createRedoAction(this);
    redo->setIcon(QIcon::fromTheme(QStringLiteral("edit-redo")));
    actions->setDefaultShortcuts(redo, KStandardShortcut::redo());
    actions->addAction(QStringLiteral("edit_redo"), redo);

    auto addAction = [&](const QString &name, const QString &text, const QString &iconName,
                         const QList<QKeySequence> &shortcuts, auto slot) {
        QAction *action = actions->addAction(name, this, slot);
        action->setText(text);
        if (!iconName.isEmpty()) {
            action->setIcon(QIcon::fromTheme(iconName));
        }
        if (!shortcuts.isEmpty()) {
            actions->setDefaultShortcuts(action, shortcuts);
        }
        return action;
    };

    addAction(QStringLiteral("insert_file"), i18nc("@action", "Insert File…"), QStringLiteral("document-import"),
              { QKeySequence(Qt::CTRL | Qt::Key_I) }, &MainWindow::insertDocument);

    addAction(QStringLiteral("insert_blank"), i18nc("@action", "Add Blank Pages…"), QStringLiteral("document-new"), {},
              &MainWindow::insertBlankPages);

    // Undo cannot reach this. What is waiting on a picture has not been written
    // to the document yet, so it is not on the undo stack, and until now there
    // was no way at all to change one's mind about a single object short of
    // discarding every change on the page.
    addAction(QStringLiteral("object_take_back"), i18nc("@action", "Undo the Change to This"),
              QStringLiteral("edit-undo"), {}, [this] { m_objectOverlay->takeBack(); });

    addAction(QStringLiteral("page_delete"), i18nc("@action", "Delete Pages"), QStringLiteral("edit-delete"),
              { QKeySequence::Delete }, &MainWindow::deleteSelectedPages);

    addAction(QStringLiteral("page_rotate_left"), i18nc("@action", "Rotate Left"), QStringLiteral("object-rotate-left"),
              { QKeySequence(Qt::CTRL | Qt::Key_L) }, &MainWindow::rotateSelectionLeft);

    addAction(QStringLiteral("page_rotate_right"), i18nc("@action", "Rotate Right"),
              QStringLiteral("object-rotate-right"), { QKeySequence(Qt::CTRL | Qt::Key_R) },
              &MainWindow::rotateSelectionRight);

    addAction(QStringLiteral("page_duplicate"), i18nc("@action", "Duplicate Pages"),
              QStringLiteral("document-duplicate"), { QKeySequence(Qt::CTRL | Qt::Key_D) },
              &MainWindow::duplicateSelection);

    addAction(QStringLiteral("page_extract"), i18nc("@action", "Extract Pages to New File…"),
              QStringLiteral("document-export"), { QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_E) },
              &MainWindow::extractSelection);

    // No shortcut on purpose. Ctrl+Shift+S is Save As, and KXmlGui rightly
    // objects to the clash, with a modal dialog at startup, which is how this
    // was found.
    addAction(QStringLiteral("file_split"), i18nc("@action", "Split…"), QStringLiteral("split"), {},
              &MainWindow::splitDocument);

    addAction(QStringLiteral("page_number"), i18nc("@action", "Page Numbers…"), QStringLiteral("draw-number"), {},
              &MainWindow::numberPages);

    addAction(QStringLiteral("page_crop"), i18nc("@action", "Trim Pages…"), QStringLiteral("transform-crop"), {},
              &MainWindow::cropPages);

    addAction(QStringLiteral("page_redact"), i18nc("@action", "Redact…"), QStringLiteral("edit-delete-shred"), {},
              &MainWindow::redactPages);

    addAction(QStringLiteral("page_annotate"), i18nc("@action", "Comments…"), QStringLiteral("pdf-annotations"),
              { QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_M) }, &MainWindow::addComments);

    addAction(QStringLiteral("comments_export"), i18nc("@action", "Save Comments to a File…"),
              QStringLiteral("document-export"), {}, &MainWindow::exportComments);

    addAction(QStringLiteral("comments_import"), i18nc("@action", "Load Comments from a File…"),
              QStringLiteral("document-import"), {}, &MainWindow::importComments);

    // Not Ctrl+T: text recognition already has it, and an ambiguous shortcut
    // makes KXmlGuiWindow open a modal box while the window is still being
    // built, which greets the user at startup and hangs the end-to-end test
    // before it can report why.
    addAction(QStringLiteral("page_text"), i18nc("@action", "Correct Text…"), QStringLiteral("draw-text"),
              { QKeySequence(Qt::CTRL | Qt::Key_E) }, &MainWindow::correctText);

    addAction(QStringLiteral("page_nup"), i18nc("@action", "Arrange on Sheets…"), QStringLiteral("view-grid"), {},
              &MainWindow::arrangeOnSheets);

    // Named for what it adds over Read mode, which fills forms in on the page
    // itself: a list of every field in one place, which is how a long form is
    // worked through and checked without hunting across twelve pages.
    addAction(QStringLiteral("document_form"), i18nc("@action", "Fill In the Form as a List…"),
              QStringLiteral("edit-entry"), {}, &MainWindow::fillForm);

    addAction(QStringLiteral("document_archive"), i18nc("@action", "Save for Archiving…"),
              QStringLiteral("document-save-as"), {}, &MainWindow::saveArchival);

    addAction(QStringLiteral("document_compare"), i18nc("@action", "Compare with…"),
              QStringLiteral("document-multiple"), {}, &MainWindow::compareWith);

    addAction(QStringLiteral("document_metadata"), i18nc("@action", "Document Properties…"),
              QStringLiteral("document-properties"), {}, &MainWindow::editMetadata);

    addAction(QStringLiteral("document_sanitize"), i18nc("@action", "Clean Up Document…"),
              QStringLiteral("edit-clear-all"), {}, &MainWindow::sanitizeDocument);

    addAction(QStringLiteral("document_password"), i18nc("@action", "Password…"), QStringLiteral("document-encrypt"),
              {}, &MainWindow::changePassword);

    // The editing tools. Each opens its own window and each lives in its own
    // file, so this list is the whole of what the window knows about them.
    addAction(QStringLiteral("tool_preflight"), i18nc("@action", "Check the Document…"),
              QStringLiteral("dialog-ok-apply"), { QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_P) },
              &MainWindow::showPreflightTool);

    addAction(QStringLiteral("tool_colour"), i18nc("@action", "Colour…"), QStringLiteral("color-management"), {},
              &MainWindow::showColourTool);

    addAction(QStringLiteral("tool_fonts"), i18nc("@action", "Fonts…"), QStringLiteral("preferences-desktop-font"), {},
              &MainWindow::showFontsTool);

    addAction(QStringLiteral("tool_images"), i18nc("@action", "Pictures…"), QStringLiteral("edit-image"), {},
              &MainWindow::showImagesTool);

    addAction(QStringLiteral("tool_layout"), i18nc("@action", "Page Setup…"), QStringLiteral("page-simple"), {},
              &MainWindow::showLayoutTool);

    addAction(QStringLiteral("tool_form_behaviour"), i18nc("@action", "Form Behaviour and Answers…"),
              QStringLiteral("view-form-action"), {}, &MainWindow::showFormBehaviourTool);

    addAction(QStringLiteral("tool_object_list"), i18nc("@action", "The Page as a List…"),
              QStringLiteral("view-list-details"), {}, &MainWindow::showObjectListTool);

    addAction(QStringLiteral("tool_form_builder"), i18nc("@action", "Build a Form…"), QStringLiteral("view-form"), {},
              &MainWindow::showFormBuilderTool);

    // Not "Edit the Page…", which is what the mode button is called: one menu
    // bar carrying that name twice, for two different commands, is a menu bar
    // that cannot be read. This one is about what the page *draws*.
    addAction(QStringLiteral("tool_objects"), i18nc("@action", "Move What the Page Draws…"),
              QStringLiteral("transform-move"), { QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_O) },
              &MainWindow::showObjectsTool);

    addAction(QStringLiteral("tool_typeset"), i18nc("@action", "Set Text as Pages…"), QStringLiteral("insert-text"), {},
              &MainWindow::showTypesetTool);

    addAction(QStringLiteral("import_images"), i18nc("@action", "Import Images…"), QStringLiteral("insert-image"), {},
              &MainWindow::importImages);

    addAction(QStringLiteral("export_images"), i18nc("@action", "Export as Images…"), QStringLiteral("image-x-generic"),
              {}, &MainWindow::exportAsImages);

    // The rest of what the engine can write out. Every one of these has been
    // available on the command line since the day it was written, and the
    // window offered two of their siblings and none of them, so "the program
    // cannot do that" was the answer a user would reasonably arrive at.
    addAction(QStringLiteral("export_text"), i18nc("@action", "Plain Text…"), QStringLiteral("text-plain"), {},
              [this] { exportAsText(TextFormat::Text); });

    addAction(QStringLiteral("export_html"), i18nc("@action", "HTML…"), QStringLiteral("text-html"), {},
              [this] { exportAsText(TextFormat::Html); });

    addAction(QStringLiteral("export_markdown"), i18nc("@action", "Markdown…"), QStringLiteral("text-markdown"), {},
              [this] { exportAsText(TextFormat::Markdown); });

    addAction(QStringLiteral("export_tables"), i18nc("@action", "Tables as a Spreadsheet…"),
              QStringLiteral("x-office-spreadsheet"), {}, [this] { exportAsText(TextFormat::Tables); });

    addAction(QStringLiteral("export_web"), i18nc("@action", "Save for the Web…"), QStringLiteral("internet-services"),
              {}, &MainWindow::saveForTheWeb);

    addAction(QStringLiteral("export_pdfx"), i18nc("@action", "Save for Commercial Printing…"),
              QStringLiteral("printer"), {}, &MainWindow::saveForPrinting);

    addAction(QStringLiteral("page_sign"), i18nc("@action", "Sign…"), QStringLiteral("document-edit-sign"),
              { QKeySequence(Qt::CTRL | Qt::Key_G) }, &MainWindow::signDocument);

    addAction(QStringLiteral("page_watermark"), i18nc("@action", "Watermark…"), QStringLiteral("edit-opacity"), {},
              &MainWindow::addWatermark);

    addAction(QStringLiteral("page_ocr"), i18nc("@action", "Recognise Text (OCR)…"), QStringLiteral("document-scan"),
              { QKeySequence(Qt::CTRL | Qt::Key_T) }, &MainWindow::recognizeText);

    addAction(QStringLiteral("page_compress"), i18nc("@action", "Make Smaller…"), QStringLiteral("archive-insert"), {},
              &MainWindow::compressPages);

    // Each of the two views keeps a size of its own, and these act on the one
    // being looked at: arranging pages and reading them are different jobs at
    // different sizes, and a single number shared between them would mean every
    // trip to the grid resized the document and back again.
    const auto zoomBy = [this](void (PageView::*document)(), void (PageGridView::*grid)()) {
        if (showingDocument()) {
            (m_pageView->*document)();
            return;
        }
        m_gridSizeChosen = true;
        (m_view->*grid)();
    };

    addAction(QStringLiteral("zoom_in"), i18nc("@action", "Larger Pages"), QStringLiteral("zoom-in"),
              { QKeySequence::ZoomIn }, [zoomBy] { zoomBy(&PageView::zoomIn, &PageGridView::zoomIn); });

    addAction(QStringLiteral("zoom_out"), i18nc("@action", "Smaller Pages"), QStringLiteral("zoom-out"),
              { QKeySequence::ZoomOut }, [zoomBy] { zoomBy(&PageView::zoomOut, &PageGridView::zoomOut); });

    // The fits are one choice, not four switches, so they belong in a group:
    // ticking "two pages" has to untick "fit width" without either of them
    // having to know the other exists.
    auto *fits = new QActionGroup(this);
    fits->setExclusive(true);

    auto addFit = [&](const QString &name, const QString &text, const QString &iconName,
                      const QList<QKeySequence> &shortcuts, PageGridView::Fit mode) {
        QAction *action = addAction(name, text, iconName, shortcuts, [this, mode] {
            // Ticking the fit that is already in force means "do it again"
            // (useful after the page grid has been rearranged) rather than
            // "turn it off", which would leave no fit selected at all.
            if (showingDocument()) {
                // All five mean something on the document too, and each of them
                // means what it says. "Two pages side by side" used to fall
                // through to fitting one whole page, so the one command whose
                // name is a promise about two pages showed exactly one.
                switch (mode) {
                case PageGridView::Fit::Width:
                    m_pageView->setLayout(PageView::Layout::Single);
                    m_pageView->fitWidth();
                    break;
                case PageGridView::Fit::Height:
                    m_pageView->setLayout(PageView::Layout::Single);
                    m_pageView->fitHeight();
                    break;
                case PageGridView::Fit::Page:
                    m_pageView->setLayout(PageView::Layout::Single);
                    m_pageView->fitPage();
                    break;
                case PageGridView::Fit::TwoPages:
                    // The layout first: a spread is what has to fit, and
                    // fitting one page and then doubling the columns would put
                    // half of each of them off the edge of the window.
                    m_pageView->setLayout(PageView::Layout::Facing);
                    m_pageView->fitPage();
                    break;
                case PageGridView::Fit::Free:
                    // The size stays where it is; what ends is the window's
                    // claim on it.
                    m_pageView->setFit(PageView::Fit::Free);
                    break;
                }
                return;
            }
            // A size the user asked for by name, so the grid keeps it rather
            // than opening on the overview again next time.
            m_gridSizeChosen = true;
            m_view->setFit(mode);
        });
        action->setCheckable(true);
        action->setActionGroup(fits);
        m_fitActions.insert(static_cast<int>(mode), action);
        return action;
    };

    addFit(QStringLiteral("zoom_fit_width"), i18nc("@action", "Fit Width"), QStringLiteral("zoom-fit-width"),
           { QKeySequence(Qt::CTRL | Qt::Key_1) }, PageGridView::Fit::Width);

    addFit(QStringLiteral("zoom_fit_height"), i18nc("@action", "Fit Height"), QStringLiteral("zoom-fit-height"),
           { QKeySequence(Qt::CTRL | Qt::Key_2) }, PageGridView::Fit::Height);

    addFit(QStringLiteral("zoom_one_page"), i18nc("@action", "One Whole Page"), QStringLiteral("zoom-fit-page"),
           { QKeySequence(Qt::CTRL | Qt::Key_3) }, PageGridView::Fit::Page);

    addFit(QStringLiteral("zoom_two_pages"), i18nc("@action", "Two Pages Side by Side"),
           QStringLiteral("view-pages-facing"), { QKeySequence(Qt::CTRL | Qt::Key_4) }, PageGridView::Fit::TwoPages);

    QAction *free = addFit(QStringLiteral("zoom_free"), i18nc("@action", "Free Zoom"), QStringLiteral("zoom-original"),
                           { QKeySequence(Qt::CTRL | Qt::Key_0) }, PageGridView::Fit::Free);
    free->setChecked(true);

    // ── what the user is here to do ───────────────────────────────────────
    //
    // Four modes, one exclusive group, always visible at the left of the
    // toolbar. This is the whole steering of the program: the mode decides
    // which layers are live, which toolbar is up, and what a click on the page
    // means. Reading is the one you land in, because a document is something
    // you open to read; changing it is a decision, and a decision should look
    // like one.
    auto *modes = new QActionGroup(this);
    modes->setExclusive(true);

    auto addMode = [&](const QString &name, const QString &text, const QString &iconName,
                       const QList<QKeySequence> &shortcuts, WorkMode mode, const QString &tip) {
        QAction *action = addAction(name, text, iconName, shortcuts, [this, mode] { setWorkMode(mode); });
        action->setCheckable(true);
        action->setActionGroup(modes);
        action->setToolTip(tip);
        m_modeActions.insert(int(mode), action);
        return action;
    };

    addMode(QStringLiteral("mode_view"), i18nc("@action", "Read && Fill In"), QStringLiteral("document-preview"),
            { QKeySequence(Qt::Key_F5) }, WorkMode::Read,
            i18nc("@info:tooltip",
                  "Turn the pages, select and copy text, and fill in forms. Nothing can be changed by accident."))
        ->setChecked(true);

    addMode(QStringLiteral("mode_edit"), i18nc("@action", "Edit the Page"), QStringLiteral("document-edit"),
            { QKeySequence(Qt::Key_F6) }, WorkMode::Page,
            i18nc("@info:tooltip", "Correct the words, move the pictures, mark the page up."));

    addMode(QStringLiteral("mode_form"), i18nc("@action", "Edit the Form"), QStringLiteral("view-form"),
            { QKeySequence(Qt::Key_F7) }, WorkMode::Form,
            i18nc("@info:tooltip", "Draw form fields onto the page, move them and set what they do."));

    addMode(QStringLiteral("mode_organise"), i18nc("@action", "Arrange the Pages"),
            QStringLiteral("view-pages-overview"), { QKeySequence(Qt::Key_F8) }, WorkMode::Organise,
            i18nc("@info:tooltip", "Reorder, turn, duplicate and throw away whole pages."));

    // "Page Overview" and "The Document" used to sit in the View menu beside
    // the four mode buttons as a second way in, and un-checkable, so the menu
    // could show "Arrange the Pages" ticked next to an untitled "Page
    // Overview", which is two names for one state and no way to tell which is
    // in force. The modes are the names now, and they are the only ones.

    // Typing on the page collects rather than writes, so that a page's worth of
    // corrections is one undo step. This is how the user cashes that in without
    // having to save the whole document to see it land.
    QAction *apply = addAction(QStringLiteral("edit_apply"), i18nc("@action", "Keep the Changes on the Page"),
                               QStringLiteral("dialog-ok"), { QKeySequence(Qt::CTRL | Qt::Key_Return) },
                               [this] { commitViewEdits(); });
    apply->setToolTip(i18nc("@info:tooltip",
                            "Writes what you have typed and dragged into the document, as one undo step. "
                            "Saving does this for you."));

    // ── the drawing tools ─────────────────────────────────────────────────
    // A toolbar rather than a menu: choosing a pen is something you do dozens
    // of times in a session and never want to go looking for.
    auto *pens = new QActionGroup(this);
    pens->setExclusive(true);

    auto addPen = [&](const QString &name, const QString &text, const QString &iconName, AnnotationOverlay::Tool tool) {
        QAction *action = addAction(name, text, iconName, {}, [this, tool] {
            showDocumentView(m_pageView->currentPage(), PageView::Mode::Edit);
            m_annotationOverlay->setTool(tool);
        });
        action->setCheckable(true);
        action->setActionGroup(pens);
        m_drawActions.insert(int(tool), action);
        return action;
    };

    addPen(QStringLiteral("draw_select"), i18nc("@action", "Pick"), QStringLiteral("edit-select"),
           AnnotationOverlay::Tool::Select);
    addPen(QStringLiteral("draw_highlight"), i18nc("@action", "Highlight"), QStringLiteral("draw-highlight"),
           AnnotationOverlay::Tool::Highlight);
    addPen(QStringLiteral("draw_underline"), i18nc("@action", "Underline"), QStringLiteral("format-text-underline"),
           AnnotationOverlay::Tool::Underline);
    addPen(QStringLiteral("draw_strikeout"), i18nc("@action", "Strikethrough"),
           QStringLiteral("format-text-strikethrough"), AnnotationOverlay::Tool::StrikeOut);
    addPen(QStringLiteral("draw_rectangle"), i18nc("@action", "Rectangle"), QStringLiteral("draw-rectangle"),
           AnnotationOverlay::Tool::Rectangle);
    addPen(QStringLiteral("draw_ellipse"), i18nc("@action", "Ellipse"), QStringLiteral("draw-ellipse"),
           AnnotationOverlay::Tool::Ellipse);
    addPen(QStringLiteral("draw_line"), i18nc("@action", "Line"), QStringLiteral("draw-line"),
           AnnotationOverlay::Tool::Line);
    addPen(QStringLiteral("draw_freehand"), i18nc("@action", "Freehand"), QStringLiteral("draw-freehand"),
           AnnotationOverlay::Tool::Freehand);
    addPen(QStringLiteral("draw_textbox"), i18nc("@action", "Text Box"), QStringLiteral("insert-text-frame"),
           AnnotationOverlay::Tool::TextBox);
    addPen(QStringLiteral("draw_note"), i18nc("@action", "Note"), QStringLiteral("note"),
           AnnotationOverlay::Tool::Note);

    m_drawActions.value(int(AnnotationOverlay::Tool::Select))->setChecked(true);

    // The overlay is the one that knows which pen is in hand: a keyboard
    // shortcut or a restored session can change it without the toolbar being
    // touched.
    connect(m_annotationOverlay, &AnnotationOverlay::toolChanged, this, [this](AnnotationOverlay::Tool tool) {
        if (QAction *action = m_drawActions.value(int(tool))) {
            action->setChecked(true);
        }
    });

    // ── moving about ──────────────────────────────────────────────────────
    // "Which page am I on and how do I get to 354" was missing entirely, and it
    // is the question a reader asks most often.
    //
    // Deliberately without PageUp and PageDown. A window-wide shortcut is
    // matched before the key reaches any widget, so claiming those two meant
    // the reading motion the document view is built around (a screenful,
    // glided, so the paragraph crossing the top can still be read) was
    // unreachable code. Both keys belong to whichever view has the focus, and
    // these two commands keep their buttons and their menu entries.
    addAction(QStringLiteral("go_previous_page"), i18nc("@action", "Previous Page"), QStringLiteral("go-previous"), {},
              [this] { m_pageView->goToPage(m_pageView->currentPage() - 1); });
    addAction(QStringLiteral("go_next_page"), i18nc("@action", "Next Page"), QStringLiteral("go-next"), {},
              [this] { m_pageView->goToPage(m_pageView->currentPage() + 1); });
    addAction(QStringLiteral("go_first_page"), i18nc("@action", "First Page"), QStringLiteral("go-first"),
              { QKeySequence(Qt::CTRL | Qt::Key_Home) }, [this] { m_pageView->goToPage(0); });
    addAction(QStringLiteral("go_last_page"), i18nc("@action", "Last Page"), QStringLiteral("go-last"),
              { QKeySequence(Qt::CTRL | Qt::Key_End) }, [this] { m_pageView->goToPage(m_document->pageCount() - 1); });

    // ── what dragging does while reading ──────────────────────────────────
    // Three answers to one question, and the question is the one the owner
    // asked: what will this drag do. Ctrl, space and the middle button still
    // reach the other two without leaving the one in hand.
    auto *tools = new QActionGroup(this);
    tools->setExclusive(true);

    auto addTool = [&](const QString &name, const QString &text, const QString &iconName, PageView::Tool tool,
                       const QString &tip) {
        QAction *action = addAction(name, text, iconName, {}, [this, tool] { m_pageView->setTool(tool); });
        action->setCheckable(true);
        action->setActionGroup(tools);
        action->setToolTip(tip);
        m_toolActions.insert(int(tool), action);
        return action;
    };

    addTool(QStringLiteral("tool_select_text"), i18nc("@action", "Select Text"), QStringLiteral("edit-select-text"),
            PageView::Tool::Text, i18nc("@info:tooltip", "Drag across words to select and copy them."))
        ->setChecked(true);
    addTool(QStringLiteral("tool_select_area"), i18nc("@action", "Select an Area"),
            QStringLiteral("select-rectangular"), PageView::Tool::Rectangle,
            i18nc("@info:tooltip", "Drag a box and take the text and the picture inside it."));
    addTool(QStringLiteral("tool_pan"), i18nc("@action", "Move the Page"), QStringLiteral("transform-browse"),
            PageView::Tool::Hand, i18nc("@info:tooltip", "Drag the page about. Holding space does this too."));

    connect(m_pageView, &PageView::toolChanged, this, [this](PageView::Tool tool) {
        if (QAction *action = m_toolActions.value(int(tool))) {
            action->setChecked(true);
        }
    });

    // ── the form-field tools ──────────────────────────────────────────────
    // Same shape as the pens: pick a kind, drag it onto the page where it
    // goes. A form is drawn, not described in a table of coordinates.
    auto *kinds = new QActionGroup(this);
    kinds->setExclusive(true);

    auto addField
        = [&](const QString &name, const QString &text, const QString &iconName, FormDesignOverlay::Kind kind) {
              QAction *action = addAction(name, text, iconName, {}, [this, kind] {
                  setWorkMode(WorkMode::Form);
                  m_formDesign->setKindToPlace(kind);
              });
              action->setCheckable(true);
              action->setActionGroup(kinds);
              m_fieldActions.insert(int(kind), action);
              return action;
          };

    addField(QStringLiteral("field_none"), i18nc("@action", "Pick a Field"), QStringLiteral("edit-select"),
             FormDesignOverlay::Kind::None)
        ->setChecked(true);
    addField(QStringLiteral("field_text"), i18nc("@action", "Text Field"), QStringLiteral("text-field"),
             FormDesignOverlay::Kind::Text);
    addField(QStringLiteral("field_checkbox"), i18nc("@action", "Tick Box"), QStringLiteral("gnumeric-object-checkbox"),
             FormDesignOverlay::Kind::Checkbox);
    addField(QStringLiteral("field_radio"), i18nc("@action", "Radio Group"), QStringLiteral("draw-circle"),
             FormDesignOverlay::Kind::Radio);
    addField(QStringLiteral("field_dropdown"), i18nc("@action", "Drop-Down"), QStringLiteral("gnumeric-object-combo"),
             FormDesignOverlay::Kind::Dropdown);
    addField(QStringLiteral("field_list"), i18nc("@action", "List"), QStringLiteral("gnumeric-object-list"),
             FormDesignOverlay::Kind::ListBox);
    addField(QStringLiteral("field_button"), i18nc("@action", "Push Button"), QStringLiteral("gnumeric-object-button"),
             FormDesignOverlay::Kind::PushButton);
    addField(QStringLiteral("field_signature"), i18nc("@action", "Signature Field"),
             QStringLiteral("document-edit-sign"), FormDesignOverlay::Kind::Signature);

    // Spreads without a word about their size, for a reader who has set one
    // they are happy with. "Two Pages Side by Side" is the same layout with the
    // size worked out to match, and this stays ticked while that is in force.
    QAction *facing
        = addAction(QStringLiteral("view_facing"), i18nc("@action", "Two Pages Side by Side in the Document"),
                    QStringLiteral("view-pages-facing"), {}, [this](bool on) {
                        m_pageView->setLayout(on ? PageView::Layout::Facing : PageView::Layout::Single);
                    });
    facing->setCheckable(true);

    QAction *inspector = m_inspector->toggleViewAction();
    inspector->setText(i18nc("@action", "Properties Sidebar"));
    inspector->setIcon(QIcon::fromTheme(QStringLiteral("documentinfo")));
    actionCollection()->addAction(QStringLiteral("show_inspector"), inspector);
    actionCollection()->setDefaultShortcut(inspector, QKeySequence(Qt::Key_F4));

    // ── the split buttons ─────────────────────────────────────────────────
    //
    // Six commands that mean one thing between them do not want six buttons.
    // Each of these is one button on the bar and one menu behind it, which is
    // how a toolbar stays short enough to read and still holds everything.
    // Built last, because every one of them is made of actions above.
    const auto entry = [actions](const char *name) { return actions->action(QLatin1String(name)); };

    addGroup(QStringLiteral("view_zoom"), i18nc("@action a menu of zoom levels", "Zoom"), QStringLiteral("page-zoom"),
             { entry("zoom_out"), entry("zoom_in"), nullptr, entry("zoom_fit_width"), entry("zoom_fit_height"),
               entry("zoom_one_page"), entry("zoom_two_pages"), entry("zoom_free") },
             false);

    addGroup(QStringLiteral("view_sidebars"), i18nc("@action a menu of side panels", "Sidebars"),
             QStringLiteral("view-sidetree"),
             { entry("show_thumbnails"), entry("show_outline"), entry("show_inspector") }, false);

    addGroup(QStringLiteral("insert_pages"), i18nc("@action a menu of things to add to the document", "Insert"),
             QStringLiteral("document-import"), { entry("insert_file"), entry("import_images"), entry("tool_typeset") },
             false);

    addGroup(QStringLiteral("export_pages"), i18nc("@action a menu of ways to write pages out", "Export"),
             QStringLiteral("document-export"),
             { entry("page_extract"), entry("export_images"), entry("file_split"), nullptr, entry("document_archive") },
             false);

    // These two wear whichever of their members is in hand: with a tool
    // chooser, "what will a drag do" has to be answerable by looking.
    addGroup(QStringLiteral("tool_select"), i18nc("@action a menu of what dragging does", "Drag"),
             QStringLiteral("edit-select-text"),
             { entry("tool_select_text"), entry("tool_select_area"), entry("tool_pan") }, true);

    addGroup(QStringLiteral("draw_shape"), i18nc("@action a menu of shapes to draw", "Shapes"),
             QStringLiteral("draw-rectangle"),
             { entry("draw_rectangle"), entry("draw_ellipse"), entry("draw_line"), entry("draw_freehand") }, true);

    // The views are the ones that know a fit has lapsed (a turn of the wheel is
    // enough), so the menu follows them rather than the other way round. Both
    // of them, because the same five entries speak for whichever is on screen.
    connect(m_view, &PageGridView::fitChanged, this, &MainWindow::updateFitActions);
    connect(m_pageView, &PageView::fitChanged, this, &MainWindow::updateFitActions);
    connect(m_pageView, &PageView::layoutChanged, this, [this](PageView::Layout layout) {
        if (QAction *action = actionCollection()->action(QStringLiteral("view_facing"))) {
            QSignalBlocker blocker(action);
            action->setChecked(layout == PageView::Layout::Facing);
        }
        updateFitActions();
    });
}

void MainWindow::updateFitActions()
{
    // One of the five is always ticked, and it is the one describing the view
    // the user is looking at. Which page of the stack is up decides that: the
    // grid and the document each keep a size of their own, and a menu showing
    // the other one's answer is worse than showing none.
    int wanted = static_cast<int>(PageGridView::Fit::Free);
    if (showingDocument()) {
        switch (m_pageView->fit()) {
        case PageView::Fit::Width:
            wanted = static_cast<int>(PageGridView::Fit::Width);
            break;
        case PageView::Fit::Height:
            wanted = static_cast<int>(PageGridView::Fit::Height);
            break;
        case PageView::Fit::Page:
            // A whole page and a whole spread are one fit in the view and two
            // entries in the menu, and the layout is what tells them apart.
            wanted = static_cast<int>(m_pageView->layout() == PageView::Layout::Facing ? PageGridView::Fit::TwoPages
                                                                                       : PageGridView::Fit::Page);
            break;
        case PageView::Fit::Free:
            break;
        }
    } else {
        wanted = static_cast<int>(m_view->fit());
    }

    if (QAction *action = m_fitActions.value(wanted)) {
        action->setChecked(true);
    }
}

KToolBarPopupAction *MainWindow::addGroup(const QString &name, const QString &text, const QString &iconName,
                                          const QList<QAction *> &members, bool follow)
{
    auto *group = new KToolBarPopupAction(QIcon::fromTheme(iconName), text, this);

    // Following groups are choosers and get the split button: the left half
    // re-applies what is in hand, the arrow offers the rest. The others are
    // lists of commands with no sensible default, and a button that quietly
    // does one of six things when clicked would be a trap.
    group->setPopupMode(follow ? KToolBarPopupAction::MenuButtonPopup : KToolBarPopupAction::InstantPopup);

    for (QAction *member : members) {
        if (member) {
            group->popupMenu()->addAction(member);
        } else {
            group->popupMenu()->addSeparator();
        }
    }

    if (follow) {
        const auto wear = [group](QAction *member) {
            group->setIcon(member->icon());
            group->setToolTip(member->toolTip());
        };
        for (QAction *member : members) {
            if (!member) {
                continue;
            }
            if (member->isChecked()) {
                wear(member);
            }
            connect(member, &QAction::toggled, group, [wear, member](bool on) {
                if (on) {
                    wear(member);
                }
            });
        }

        // Clicking the button itself means "the one I am already holding", and
        // when none of these is held (a highlighter is not a shape) it means
        // the first, which is what the button is showing.
        connect(group, &QAction::triggered, this, [group] {
            const QList<QAction *> entries = group->popupMenu()->actions();
            for (QAction *member : entries) {
                if (member->isChecked()) {
                    member->trigger();
                    return;
                }
            }
            if (!entries.isEmpty()) {
                entries.constFirst()->trigger();
            }
        });
    }

    actionCollection()->addAction(name, group);
    return group;
}

void MainWindow::setupStatusBar()
{
    m_pageCountLabel = new QLabel(this);
    m_selectionLabel = new QLabel(this);

    // Which mode, in words. The four buttons say it too, but a glance at the
    // bottom of the window is how people check without moving their eyes to
    // the top of it.
    // "Which page of how many" belongs where the eye already is when turning
    // pages, and typing 354 into it has to be a way to get to page 354.
    m_pageBox = new QSpinBox(this);
    m_pageBox->setRange(1, 1);
    m_pageBox->setKeyboardTracking(false);
    m_pageBox->setToolTip(i18nc("@info:tooltip", "Which page is showing. Type a number to go there."));
    m_pageTotal = new QLabel(this);
    connect(m_pageBox, &QSpinBox::valueChanged, this, [this](int value) {
        if (value - 1 != m_pageView->currentPage()) {
            m_pageView->goToPage(value - 1);
        }
    });
    connect(m_pageView, &PageView::currentPageChanged, this, [this](int row) {
        if (m_pageBox->value() != row + 1) {
            QSignalBlocker blocker(m_pageBox);
            m_pageBox->setValue(row + 1);
        }
    });

    m_modeLabel = new QLabel(this);
    m_modeLabel->setToolTip(i18nc("@info:tooltip", "What clicking on the page does at the moment"));

    m_zoomSlider = new QSlider(Qt::Horizontal, this);
    m_zoomSlider->setRange(PageGridView::MinimumWidth, PageGridView::MaximumWidth);
    m_zoomSlider->setValue(m_view->thumbnailWidth());
    m_zoomSlider->setFixedWidth(140);
    m_zoomSlider->setToolTip(i18nc("@info:tooltip", "Page size in the grid"));
    connect(m_zoomSlider, &QSlider::valueChanged, m_view, &PageGridView::setThumbnailWidth);

    statusBar()->addWidget(m_modeLabel);
    statusBar()->addWidget(m_pageBox);
    statusBar()->addWidget(m_pageTotal);
    statusBar()->addWidget(m_pageCountLabel);
    statusBar()->addWidget(m_selectionLabel, 1);
    statusBar()->addPermanentWidget(m_zoomSlider);
}

void MainWindow::restoreSession()
{
    KConfigGroup group(KSharedConfig::openConfig(), QStringLiteral("General"));
    if (m_recentFiles) {
        m_recentFiles->loadEntries(KConfigGroup(KSharedConfig::openConfig(), QStringLiteral("RecentFiles")));
    }
    // The grid's own key. It used to be called ThumbnailWidth, which is also
    // what the thumbnail strip beside the document calls its width: one key,
    // one config group, two widgets, and whichever closed last decided the
    // other's size. A grid page can be 3200 pixels wide and the strip stops at
    // 320, so each was reading a number the other had ruined. The old value is
    // still read once, so nobody loses the zoom they had set.
    const int width
        = group.readEntry("GridThumbnailWidth", group.readEntry("ThumbnailWidth", m_view->thumbnailWidth()));
    m_view->setThumbnailWidth(width);
    m_zoomSlider->setValue(m_view->thumbnailWidth());

    // After the width, and only if it is a real fit: setThumbnailWidth has just
    // cancelled whatever fit was in force, which is right when a person turns
    // the wheel and wrong when the window is only being put back as it was.
    const int fit = group.readEntry("ZoomFit", static_cast<int>(PageGridView::Fit::Free));
    if (fit != static_cast<int>(PageGridView::Fit::Free) && fit <= static_cast<int>(PageGridView::Fit::TwoPages)) {
        m_view->setFit(static_cast<PageGridView::Fit>(fit));
    }

    // Last, so that neither of the two above is mistaken for the user reaching
    // for the slider now. A window that has never been told what size the grid
    // should be opens the grid on the overview instead.
    m_gridSizeChosen = group.readEntry("GridSizeChosen", false);
}

void MainWindow::saveSession()
{
    KConfigGroup group(KSharedConfig::openConfig(), QStringLiteral("General"));
    group.writeEntry("GridThumbnailWidth", m_view->thumbnailWidth());
    group.writeEntry("ZoomFit", static_cast<int>(m_view->fit()));
    group.writeEntry("GridSizeChosen", m_gridSizeChosen);
    if (m_recentFiles) {
        KConfigGroup recent(KSharedConfig::openConfig(), QStringLiteral("RecentFiles"));
        m_recentFiles->saveEntries(recent);
    }

    // Where the bars ended up, written down here rather than left to the
    // "something was dragged" heuristic, and only then is this build's
    // arrangement recorded as delivered. The two have to be written together: a
    // generation marked as done without the layout beside it would leave the
    // next start reading last week's arrangement and declining to fix it.
    if (autoSaveSettings()) {
        saveAutoSaveSettings();
        KConfigGroup layout(KSharedConfig::openConfig(), QStringLiteral("Layout"));
        layout.writeEntry("Generation", LayoutGeneration);
    }
    group.sync();
}

// ── Document lifecycle ────────────────────────────────────────────────────

bool MainWindow::askForPassword(const QString &fileName, bool wasWrong, QString *password)
{
    // Modal on purpose, unlike the error banner: this one is a question, and
    // there is nothing to be done with the document until it is answered.
    KPasswordDialog dialog(this);
    dialog.setWindowTitle(i18nc("@title:window", "Password Needed"));
    dialog.setPrompt(
        wasWrong ? i18n("That password does not open “%1”. Try again, or press Cancel to leave it closed.", fileName)
                 : i18n("“%1” is protected. Type the password that opens it.", fileName));
    if (dialog.exec() != QDialog::Accepted) {
        return false;
    }

    *password = dialog.password();
    return true;
}

bool MainWindow::openUrl(const QUrl &url)
{
    if (!confirmDiscard()) {
        return false;
    }

    QString error;
    const QString local = RemoteFile::fetch(url, this, &error);
    if (local.isEmpty()) {
        reportError(i18nc("@title:window", "Cannot Open Document"), error);
        return false;
    }

    if (!openFile(local)) {
        return false;
    }

    // Remembered so that Save goes back where the document came from rather
    // than leaving the edit in a scratch directory nobody will ever look in.
    m_remoteOrigin = RemoteFile::isLocal(url) ? QUrl() : url;
    if (m_recentFiles) {
        m_recentFiles->addUrl(url);
    }
    updateTitle();
    return true;
}

bool MainWindow::openFile(const QString &path)
{
    if (!confirmDiscard()) {
        return false;
    }

    // A locked file is a question, not a failure. Asked for as often as it
    // takes: someone typing a long password from a card gets it wrong, and one
    // refusal with no way back would leave them shut out of a document this
    // very program can lock.
    QString password;
    for (bool again = false;; again = true) {
        QString error;
        bool needsPassword = false;
        if (m_document->open(path, &error, password, &needsPassword)) {
            break;
        }
        if (!needsPassword) {
            reportError(i18nc("@title:window", "Cannot Open Document"), error);
            return false;
        }
        if (!askForPassword(QFileInfo(path).fileName(), again, &password)) {
            return false;
        }
    }

    // Kept so that saving locks the file again with what opened it, rather than
    // writing a plain copy over a document its owner had protected.
    m_userPassword = password;
    m_remoteOrigin.clear();
    if (m_recentFiles) {
        m_recentFiles->addUrl(QUrl::fromLocalFile(path));
    }
    m_view->scrollToTop();

    // A file that needed recovery is one worth saving a clean copy of, so say
    // so: once, quietly, and not as an error.
    for (int id = 0; id < m_document->sourceCount(); ++id) {
        const Source *source = m_document->source(id);
        if (source && !source->openWarning().isEmpty()) {
            reportNotice(source->openWarning());
            break;
        }
    }
    return true;
}

void MainWindow::openDocument()
{
    // Not chooseFileToRead: openUrl has to see the URL itself, so that saving
    // later goes back to the share the document came from.
    const QUrl url
        = QFileDialog::getOpenFileUrl(this, i18nc("@title:window", "Open PDF"), chooserLocation(), pdfFilter());
    if (!url.isEmpty()) {
        openUrl(url);
    }
}

void MainWindow::newDocument()
{
    if (!confirmDiscard()) {
        return;
    }

    // A new document is one blank sheet in a scratch file, opened like any
    // other. Nothing downstream has to learn about a document that has no file:
    // saving, merging, undo and the page list all work as they always did, and
    // the empty path is what makes Save ask where it should go.
    const QString built = QDir(RemoteFile::scratchDirectory()).filePath(QStringLiteral("untitled.pdf"));
    QString error;
    if (!BlankPdf::write(built, 1, BlankPdf::defaultSize(), &error)) {
        reportError(i18nc("@title:window", "Cannot Start a New Document"), error);
        return;
    }

    openFile(built);
    m_document->setFilePath(QString());
    statusBar()->showMessage(i18nc("@info:status", "A new document with one blank page."), 6000);
}

void MainWindow::insertBlankPages()
{
    // Matched to the page the new ones will sit beside rather than to the first
    // page of the document: a book with a landscape plate in the middle should
    // get another landscape sheet there, not an upright one.
    const QVector<int> selection = m_view->selectedRows();
    const int beside = selection.isEmpty() ? m_document->pageCount() - 1 : selection.last();
    const QSizeF match = beside >= 0 ? m_document->pageSizePoints(beside) : QSizeF();

    BlankPagesDialog dialog(match, this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    const QString built = QDir(RemoteFile::scratchDirectory()).filePath(QStringLiteral("blank.pdf"));
    QString error;
    if (!BlankPdf::write(built, dialog.pageCount(), dialog.sizePoints(), &error)) {
        reportError(i18nc("@title:window", "Cannot Add Blank Pages"), error);
        return;
    }

    if (m_document->pageCount() == 0) {
        openFile(built);
        m_document->setFilePath(QString());
        return;
    }
    insertFilesAt({ built }, beside + 1);
}

void MainWindow::insertDocument()
{
    const QStringList paths = chooseFilesToRead(i18nc("@title:window", "Insert PDF"), pdfFilter());
    if (paths.isEmpty()) {
        return;
    }

    // Appending is right about half the time. The other half the user has a
    // page selected exactly because that is where the new material belongs, so
    // ask rather than guess.
    int at = m_document->pageCount();
    if (m_document->pageCount() > 0) {
        const QVector<int> selection = m_view->selectedRows();
        const int anchor = selection.isEmpty() ? 0 : selection.last() + 1;

        InsertPositionDialog dialog(selection.isEmpty() ? 0 : anchor, paths.size(), this);
        dialog.setPosition(m_lastInsertPosition);
        if (dialog.exec() != QDialog::Accepted) {
            return;
        }
        m_lastInsertPosition = dialog.position();

        switch (dialog.position()) {
        case InsertPositionDialog::Position::Start:
            at = 0;
            break;
        case InsertPositionDialog::Position::BeforeSelection:
            at = selection.isEmpty() ? 0 : selection.first();
            break;
        case InsertPositionDialog::Position::AfterSelection:
            at = selection.isEmpty() ? m_document->pageCount() : selection.last() + 1;
            break;
        case InsertPositionDialog::Position::End:
            at = m_document->pageCount();
            break;
        }
    }

    insertFilesAt(paths, at);
}

void MainWindow::insertFilesAt(const QStringList &paths, int at)
{
    // Everything inserted in one go is one undo step, however many files it was.
    auto *macro = new QUndoCommand(i18ncp("@action:inmenu undo step", "Insert file", "Insert %1 files", paths.size()));
    bool any = false;
    int position = std::clamp(at, 0, m_document->pageCount());
    int firstInserted = position;

    for (const QString &path : paths) {
        // The same question as opening asks, for the same reason: a locked file
        // being inserted is one the user has the password to, and refusing it
        // in silence is how pages go missing from a merge.
        int sourceId = -1;
        QString password;
        for (bool again = false;; again = true) {
            QString error;
            bool needsPassword = false;
            sourceId = m_document->addSource(path, &error, password, &needsPassword);
            if (sourceId >= 0) {
                break;
            }
            if (!needsPassword) {
                reportError(i18nc("@title:window", "Cannot Insert Document"), error);
                break;
            }
            if (!askForPassword(QFileInfo(path).fileName(), again, &password)) {
                break;
            }
        }
        if (sourceId < 0) {
            continue;
        }

        Source *source = m_document->source(sourceId);
        QVector<PageRef> refs;
        refs.reserve(source->pageCount());
        for (int i = 0; i < source->pageCount(); ++i) {
            refs.append(PageRef { sourceId, i, 0 });
        }

        new InsertPagesCommand(m_document, position, refs, QString(), macro);
        position += refs.size();
        any = true;
    }

    if (!any) {
        delete macro;
        return;
    }

    m_document->undoStack()->push(macro);

    // Select what just arrived: it is what the user will want to look at, and
    // it makes the insertion visible even far down a long document.
    QVector<int> inserted;
    for (int row = firstInserted; row < position; ++row) {
        inserted.append(row);
    }
    m_view->selectRows(inserted);
}

bool MainWindow::saveDocument()
{
    // Whatever is typed into the page but not yet written down is part of what
    // the user means by "save", and losing it to a Ctrl+S would be the worst
    // bug this program could have.
    if (!commitViewEdits()) {
        return false;
    }
    if (m_document->filePath().isEmpty()) {
        return saveDocumentAs();
    }

    QString error;
    if (!DocumentWriter::write(*m_document, m_document->filePath(), writerOptions(), &error)) {
        reportError(i18nc("@title:window", "Cannot Save Document"), error);
        return false;
    }

    // Written locally first, then pushed back to the share. Doing it this way
    // round means a failed upload leaves a complete file behind rather than a
    // half-written one on the server.
    if (!m_remoteOrigin.isEmpty() && !RemoteFile::store(m_document->filePath(), m_remoteOrigin, this, &error)) {
        reportError(i18nc("@title:window", "Cannot Save Document"), error);
        return false;
    }

    m_document->undoStack()->setClean();
    return true;
}

bool MainWindow::saveDocumentAs()
{
    if (m_document->pageCount() == 0) {
        return false;
    }
    if (!commitViewEdits()) {
        return false;
    }

    const QString suggestion
        = m_document->filePath().isEmpty() ? QString() : QFileInfo(m_document->filePath()).fileName();
    const Destination destination = chooseDestination(i18nc("@title:window", "Save PDF As"), suggestion, pdfFilter());
    if (!destination.chosen) {
        return false;
    }

    QString error;
    if (!DocumentWriter::write(*m_document, destination.path, writerOptions(), &error)) {
        reportError(i18nc("@title:window", "Cannot Save Document"), error);
        return false;
    }
    if (!deliver(destination)) {
        return false;
    }

    m_document->setFilePath(destination.path);
    m_document->undoStack()->setClean();
    // Saved onto a share: that is where it lives now, and where the next plain
    // Save has to put it.
    m_remoteOrigin = destination.remote;
    if (m_recentFiles) {
        m_recentFiles->addUrl(destination.remote.isEmpty() ? QUrl::fromLocalFile(destination.path)
                                                           : destination.remote);
    }
    updateTitle();
    return true;
}

void MainWindow::showPrintPreview()
{
    if (m_document->pageCount() == 0) {
        return;
    }
    // What is waiting on the page is part of the document as far as the user is
    // concerned, and a preview of the version before their last hour is worse
    // than no preview.
    if (!commitViewEdits()) {
        return;
    }

    PrintController controller;
    controller.setDocument(m_document);
    controller.setRenderBackend(m_backend.get());

    QPrinter printer(QPrinter::HighResolution);
    printer.setDocName(m_document->displayName());

    QPrintPreviewDialog preview(&printer, this);
    preview.setWindowTitle(i18nc("@title:window", "Print Preview"));
    preview.resize(900, 1000);

    // The preview draws through exactly the same code as the printer does, so
    // what is on screen is the imposition that will come out, down to booklet
    // order and blank slots. A preview of the source document instead of the
    // print job would be worse than none.
    connect(&preview, &QPrintPreviewDialog::paintRequested, this, [this, &controller](QPrinter *target) {
        QString error;
        if (!controller.print(target, m_printOptions, &error) && !error.isEmpty()) {
            reportError(i18nc("@title:window", "Cannot Print"), error);
        }
    });

    preview.exec();
}

void MainWindow::printDocument()
{
    if (m_document->pageCount() == 0) {
        return;
    }
    if (!commitViewEdits()) {
        return;
    }

    PrintController controller;
    controller.setDocument(m_document);
    controller.setRenderBackend(m_backend.get());

    QPrinter printer(QPrinter::HighResolution);
    printer.setDocName(m_document->displayName());

    QPrintDialog dialog(&printer, this);
    dialog.setWindowTitle(i18nc("@title:window", "Print Document"));
    dialog.setOption(QAbstractPrintDialog::PrintPageRange, false); // we own ranges

    // The layout controls live inside the system dialog rather than in a second
    // window in front of it, so that Cancel really cancels everything.
    auto *optionsWidget = new PrintOptionsWidget;
    dialog.setOptionTabs({ optionsWidget });

    const auto refreshSummary = [&controller, optionsWidget] {
        const PrintController::Options options = optionsWidget->options();
        QString error;
        const int pages = controller.impose(options, &error).size();
        if (pages == 0) {
            optionsWidget->setSummary(error);
            return;
        }
        const int sheets = controller.sheetCount(options);
        optionsWidget->setSummary(
            i18ncp("@info:status print job summary", "%1 sheet of paper", "%1 sheets of paper", sheets));
    };
    connect(optionsWidget, &PrintOptionsWidget::optionsChanged, this, refreshSummary);
    refreshSummary();

    if (dialog.exec() != QDialog::Accepted) {
        // Kept even on cancel: the user has just said what they want, and the
        // preview should show that rather than reverting to defaults.
        m_printOptions = optionsWidget->options();
        return;
    }

    m_printOptions = optionsWidget->options();

    QString error;
    if (!controller.print(&printer, m_printOptions, &error)) {
        reportError(i18nc("@title:window", "Cannot Print"), error);
        return;
    }

    statusBar()->showMessage(i18nc("@info:status", "Sent to %1.", printer.printerName()), 5000);
}

void MainWindow::showContextMenu(const QPoint &viewportPos)
{
    // Built from the .rc file so the entries stay configurable and translated
    // alongside the menu bar, rather than being a second, divergent list.
    auto *menu = qobject_cast<QMenu *>(guiFactory()->container(QStringLiteral("page_context"), this));
    if (!menu) {
        return;
    }

    // Right-clicking a page the user has not selected should act on that page,
    // which means selecting it first: silently operating on a different
    // selection than the one under the cursor is a good way to lose work.
    const QModelIndex under = m_view->indexAt(viewportPos);
    if (under.isValid() && !m_view->selectedRows().contains(under.row())) {
        m_view->selectRows({ under.row() });
    }

    updateActionState();
    menu->popup(m_view->viewport()->mapToGlobal(viewportPos));
}

void MainWindow::showPageContextMenu(const QPoint &globalPos)
{
    // One menu per mode, because the mode is the whole of what a click means
    // here. Offering the page-arranging menu over a page being read would put
    // "Delete Pages" under the pointer in the one mode that promises nothing
    // can be changed by accident.
    const char *name = nullptr;
    switch (m_mode) {
    case WorkMode::Read:
        name = "read_context";
        break;
    case WorkMode::Page:
        name = "page_edit_context";
        break;
    case WorkMode::Form:
        name = "form_context";
        break;
    case WorkMode::Organise:
        // The grid is what is on screen and it puts its own menu up.
        return;
    }

    auto *menu = qobject_cast<QMenu *>(guiFactory()->container(QLatin1String(name), this));
    if (!menu) {
        return;
    }

    updateActionState();
    menu->popup(globalPos);
}

void MainWindow::splitDocument()
{
    if (m_document->pageCount() < 2) {
        return;
    }

    // Splitting reads the outline from a file, so the document has to exist on
    // disk before bookmark mode can offer anything, and it has to be the file
    // the user is looking at, layers and all.
    if (hasUnsavedWork() || m_document->filePath().isEmpty()) {
        if (!saveDocument()) {
            return;
        }
    }

    SplitDialog dialog(m_document, m_document->filePath(), this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    const Splitter::Result result = Splitter::split(*m_document, dialog.options());
    if (!result.success) {
        reportError(i18nc("@title:window", "Cannot Split Document"), result.error);
        return;
    }

    statusBar()->showMessage(i18ncp("@info:status", "Wrote %1 file.", "Wrote %1 files.", result.files.size()), 8000);
}

void MainWindow::numberPages()
{
    if (m_document->pageCount() == 0) {
        return;
    }

    enterEditor(new NumberEditor(m_document, m_backend.get(), m_view->selectedRows(), m_processor));
}

void MainWindow::cropPages()
{
    if (m_document->pageCount() == 0) {
        return;
    }

    enterEditor(new CropEditor(m_document, m_backend.get(), m_view->selectedRows(), m_processor));
}

void MainWindow::redactPages()
{
    if (m_document->pageCount() == 0) {
        return;
    }

    const QVector<int> selection = m_view->selectedRows();
    enterEditor(
        new RedactEditor(m_document, m_backend.get(), selection.isEmpty() ? 0 : selection.first(), m_processor));
}

void MainWindow::arrangeOnSheets()
{
    if (m_document->pageCount() < 2) {
        return;
    }

    const QVector<int> selection = m_view->selectedRows();
    const QSizeF first = m_document->pageSizePoints(0);

    NUpDialog dialog(selection.size(), m_document->pageCount(), first.width() > first.height(), this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    const QVector<int> rows = dialog.appliesToSelectionOnly() ? selection : allRows();
    int sheets = 0;
    QString error;
    if (!m_processor->arrangeOnSheets(rows, dialog.options(), &sheets, &error)) {
        if (!error.isEmpty()) {
            reportError(i18nc("@title:window", "Cannot Arrange"), error);
        }
        return;
    }

    statusBar()->showMessage(
        i18ncp("@info:status", "Arranged %1 page onto sheets.", "Arranged %1 pages onto sheets.", rows.size()), 6000);
}

void MainWindow::addComments()
{
    if (m_document->pageCount() == 0) {
        return;
    }

    // Straight to the page with a highlighter in hand. Marking up is done *to*
    // a document you are looking at, so there is nothing left for a separate
    // window to add.
    const QVector<int> selection = m_view->selectedRows();
    showDocumentView(selection.isEmpty() ? 0 : selection.first(), PageView::Mode::Edit);
    m_annotationOverlay->setAuthor(QString::fromLocal8Bit(qgetenv("USER")));
    m_annotationOverlay->setTool(AnnotationOverlay::Tool::Highlight);
    m_inspector->show();
}

void MainWindow::exportComments()
{
    if (m_document->pageCount() == 0) {
        return;
    }
    // Comments drawn a minute ago are the ones the user means; without this the
    // file written out held everything except them.
    if (!commitViewEdits()) {
        return;
    }

    const QString suggestion = m_document->filePath().isEmpty()
        ? QStringLiteral("comments.xfdf")
        : QFileInfo(m_document->filePath()).completeBaseName() + QStringLiteral(".xfdf");

    const Destination destination = chooseDestination(i18nc("@title:window", "Save Comments"), suggestion,
                                                      i18n("Comment files (*.xfdf);;All files (*)"));
    if (!destination.chosen) {
        return;
    }

    QString error;
    if (!m_processor->exportComments(destination.path, &error)) {
        reportError(i18nc("@title:window", "Cannot Save Comments"),
                    error.isEmpty() ? i18n("This document has no comments to write out.") : error);
        return;
    }
    if (!deliver(destination)) {
        return;
    }

    statusBar()->showMessage(i18nc("@info:status", "Comments written to %1. The document itself is not in it.",
                                   QFileInfo(destination.path).fileName()),
                             8000);
}

void MainWindow::importComments()
{
    if (m_document->pageCount() == 0) {
        return;
    }
    // Bringing comments in rewrites every page that has any, so anything still
    // waiting in the layer would be overwritten by what arrives.
    if (!commitViewEdits()) {
        return;
    }

    const QString path
        = chooseFileToRead(i18nc("@title:window", "Load Comments"), i18n("Comment files (*.xfdf);;All files (*)"));
    if (path.isEmpty()) {
        return;
    }

    int added = 0;
    QStringList warnings;
    QString error;
    if (!m_processor->importComments(path, &added, &warnings, &error)) {
        reportError(i18nc("@title:window", "Cannot Load Comments"), error);
        return;
    }

    for (const QString &warning : std::as_const(warnings)) {
        reportNotice(warning);
    }
    statusBar()->showMessage(i18ncp("@info:status", "Brought in %1 comment.", "Brought in %1 comments.", added), 6000);
}

void MainWindow::fillForm()
{
    if (m_document->pageCount() == 0) {
        return;
    }

    // The form lives in the file, not in the page list, so this works on the
    // document as it stands rather than on a selection.
    QVector<int> rows = allRows();
    QString error;
    const QString extracted = m_processor->writeScratchCopy(rows, &error);
    if (extracted.isEmpty()) {
        reportError(i18nc("@title:window", "Cannot Fill In Form"), error);
        return;
    }

    const QVector<FormField> fields = Forms::read(extracted, &error);
    if (fields.isEmpty()) {
        reportNotice(i18n("This document has no form fields to fill in."));
        return;
    }

    enterEditor(new FormEditor(m_document, m_backend.get(), fields, m_processor));
}

void MainWindow::correctText()
{
    if (m_document->pageCount() == 0) {
        return;
    }

    // Correcting text happens by clicking into the line and typing. There is
    // no longer a list of lines to pick from, because picking a line out of a
    // list and typing the replacement somewhere else was the worst part of
    // this feature.
    const QVector<int> selection = m_view->selectedRows();
    showDocumentView(selection.isEmpty() ? 0 : selection.first(), PageView::Mode::Edit);
    m_annotationOverlay->setTool(AnnotationOverlay::Tool::Select);
}

void MainWindow::saveArchival()
{
    if (m_document->pageCount() == 0) {
        return;
    }
    if (!Archival::isAvailable()) {
        reportError(i18nc("@title:window", "Cannot Save for Archiving"),
                    i18n("Ghostscript is not installed, and it does the conversion. Install the “ghostscript” "
                         "package and try again."));
        return;
    }
    // After the refusal above, which costs nothing, and before the writing,
    // which reads the document's own sources: what is waiting in a layer is
    // part of what the user means by "keep this for the archive".
    if (!commitViewEdits()) {
        return;
    }

    const QString suggestion = m_document->filePath().isEmpty()
        ? QStringLiteral("archival.pdf")
        : QFileInfo(m_document->filePath()).completeBaseName() + QStringLiteral("-pdfa.pdf");
    const Destination destination
        = chooseDestination(i18nc("@title:window", "Save for Archiving"), suggestion, pdfFilter());
    if (!destination.chosen) {
        return;
    }

    Archival::Options options;
    options.title = m_document->metadata().title;
    options.author = m_document->metadata().author;

    Archival::Report report;
    QString error;
    if (!m_processor->writeArchival(destination.path, options, &report, &error)) {
        reportError(i18nc("@title:window", "Cannot Save for Archiving"), error);
        return;
    }
    if (!deliver(destination)) {
        return;
    }

    // What the conversion had to change, and what it could not fix. Both belong
    // in front of someone who is about to hand the file to an archive.
    for (const QString &warning : std::as_const(report.warnings)) {
        reportNotice(warning);
    }
    statusBar()->showMessage(i18nc("@info:status", "Written as %1.", Archival::describe(options.level)), 8000);
}

void MainWindow::compareWith()
{
    if (m_document->pageCount() == 0) {
        return;
    }

    // Comparing reads two **files**, so this one has to be the document as it
    // stands rather than as it was opened; otherwise the report describes an
    // afternoon's work as absent.
    const QString mine = tools::savedPath(m_document, this);
    if (mine.isEmpty()) {
        return;
    }

    const QString other = chooseFileToRead(i18nc("@title:window", "Compare With"), pdfFilter());
    if (other.isEmpty()) {
        return;
    }

    QString error;
    const Compare::Report report = Compare::run(m_backend.get(), mine, other, {}, &error);
    if (!error.isEmpty()) {
        reportError(i18nc("@title:window", "Cannot Compare"), error);
        return;
    }
    if (report.identical) {
        reportNotice(i18n("The two documents are the same."));
        return;
    }

    CompareDialog dialog(m_backend.get(), mine, other, report, this);
    dialog.exec();
}

DocumentWriter::Options MainWindow::writerOptions() const
{
    DocumentWriter::Options options;
    options.stripInteractivity = m_stripInteractivity;

    // Output is assembled from scratch and carries no encryption of its own, so
    // a save without this quietly unlocks a document its owner had locked,
    // including a copy extracted out of it, which is the same content and has
    // no business being the unprotected one.
    options.userPassword = m_userPassword;
    options.ownerPassword = m_ownerPassword;
    options.permissions = m_permissions;
    return options;
}

void MainWindow::editMetadata()
{
    if (m_document->isEmpty()) {
        return;
    }

    MetadataDialog dialog(m_document->metadata(), this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    // A real command rather than a hand-set dirty flag: marking the stack dirty
    // without pushing anything left Ctrl+Z undoing whatever came before, so
    // changing the title un-rotated a page and kept the title.
    m_document->undoStack()->push(new SetMetadataCommand(m_document, dialog.fields()));
    updateTitle();
}

void MainWindow::sanitizeDocument()
{
    if (m_document->isEmpty()) {
        return;
    }

    Metadata::Findings findings;
    if (!m_document->filePath().isEmpty()) {
        Metadata::inspect(m_document->filePath(), &findings, nullptr);
    }

    SanitizeDialog dialog(findings, m_document->metadata(), this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    m_document->undoStack()->push(
        new CleanUpCommand(m_document, this, dialog.removeMetadata(), dialog.removeInteractivity()));

    updateTitle();
    statusBar()->showMessage(i18nc("@info:status", "Cleaned up. Save to write the result."), 8000);
}

void MainWindow::setStripInteractivity(bool strip)
{
    m_stripInteractivity = strip;
}

void MainWindow::documentReset()
{
    // Everything a decision was taken about *this* document. The clean-up flag
    // used to outlive the document it was set for and strip the next one's
    // attachments with nobody asked; a password left behind would be worse
    // still, since it would lock a file whose owner never protected it.
    m_stripInteractivity = false;
    m_userPassword.clear();
    m_ownerPassword.clear();
    m_permissions = Encryption::Permissions {};
}

void MainWindow::changePassword()
{
    if (m_document->isEmpty() || m_document->filePath().isEmpty()) {
        reportError(i18nc("@title:window", "Cannot Set a Password"),
                    i18n("Save the document once before protecting it."));
        return;
    }

    const bool locked = !m_userPassword.isEmpty() || Encryption::isEncrypted(m_document->filePath());
    PasswordDialog dialog(locked, this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    // Written by the writer on the way out rather than laid over the file
    // afterwards. Two reasons: rewriting the file this window is still reading
    // from is how a document ends up half old and half new, and a password the
    // window does not know about is one the next Ctrl+S would write straight
    // over, which is exactly what used to happen.
    const QString userBefore = m_userPassword;
    const QString ownerBefore = m_ownerPassword;
    const Encryption::Permissions permissionsBefore = m_permissions;

    m_userPassword = dialog.removesProtection() ? QString() : dialog.password();
    m_ownerPassword.clear();
    m_permissions = dialog.removesProtection() ? Encryption::Permissions {} : dialog.permissions();

    if (!saveDocument()) {
        m_userPassword = userBefore;
        m_ownerPassword = ownerBefore;
        m_permissions = permissionsBefore;
        return;
    }

    statusBar()->showMessage(dialog.removesProtection() ? i18nc("@info:status", "The password has been removed.")
                                                        : i18nc("@info:status", "Locked with AES-256."),
                             8000);
}

void MainWindow::importImages()
{
    const QStringList paths = chooseFilesToRead(i18nc("@title:window", "Import Images"), imageFilter());
    if (paths.isEmpty()) {
        return;
    }

    const QString built = QDir(RemoteFile::scratchDirectory()).filePath(QStringLiteral("imported.pdf"));
    QString error;
    if (!ImageIO::imagesToPdf(paths, built, ImageIO::ImportOptions {}, &error)) {
        reportError(i18nc("@title:window", "Cannot Import Images"), error);
        return;
    }

    // Inserted rather than opened, so importing a few scans into an existing
    // document is one gesture instead of two.
    if (m_document->pageCount() == 0) {
        openFile(built);
        m_document->setFilePath(QString());
    } else {
        insertFilesAt({ built }, m_document->pageCount());
    }

    statusBar()->showMessage(i18ncp("@info:status", "Imported %1 image.", "Imported %1 images.", paths.size()), 6000);
}

void MainWindow::exportAsImages()
{
    if (m_document->pageCount() == 0) {
        return;
    }
    // The pages are drawn from the document's own sources, so a correction
    // still waiting in a layer would simply not be in the picture.
    if (!commitViewEdits()) {
        return;
    }

    const QVector<int> selection = m_view->selectedRows();
    ExportImagesDialog dialog(selection.size(), m_document->pageCount(), this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    const QVector<int> rows = dialog.appliesToSelectionOnly() ? selection : allRows();
    const ImageIO::ExportOptions options = dialog.options();

    const Destination destination = chooseDirectory(i18nc("@title:window", "Where should the images go?"));
    if (!destination.chosen) {
        return;
    }

    const QString base = m_document->filePath().isEmpty() ? i18nc("@item default file name", "page")
                                                          : QFileInfo(m_document->filePath()).completeBaseName();
    const QString target = QDir(destination.path).filePath(base + QStringLiteral("-%1.") + options.format);

    // Whole page references, each carrying its own file: a document assembled
    // from two sources has a different one per page, and passing a single
    // source with a list of page numbers rendered the wrong file's pages for
    // every row after the first, then reported success, because the pages it
    // could not find were skipped in silence.
    QVector<PageRef> pages;
    pages.reserve(rows.size());
    for (const int row : rows) {
        pages.append(m_document->pageAt(row));
    }

    QStringList written;
    QVector<int> skipped;
    QString error;
    if (!ImageIO::pagesToImages(m_backend.get(), pages, target, options, &written, &skipped, &error)) {
        reportError(i18nc("@title:window", "Cannot Export Images"), error);
        return;
    }
    if (!deliver(destination, written)) {
        return;
    }

    statusBar()->showMessage(i18ncp("@info:status", "Wrote %1 image.", "Wrote %1 images.", written.size()), 8000);
    if (!skipped.isEmpty()) {
        // Said out loud rather than left to be counted: nothing else on screen
        // distinguishes eight images from the two of them that worked. Two
        // sentences rather than one, because a plural form can only be chosen
        // by a single number and these are two different numbers.
        reportNotice(i18ncp("@info", "One page could not be drawn and was left out.",
                            "%1 pages could not be drawn and were left out.", skipped.size()));
    }
}

void MainWindow::exportAsText(TextFormat format)
{
    if (m_document->pageCount() == 0) {
        return;
    }

    // All of these read the **file** (the words are worked out from where the
    // glyphs sit), so it has to be the document as it stands.
    const QString source = tools::savedPath(m_document, this);
    if (source.isEmpty()) {
        return;
    }

    QString title;
    QString suffix;
    QString filter;
    std::function<bool(const QString &output, QString *error)> write;

    const Convert::TextOptions options; // every page, laid out as it reads

    switch (format) {
    case TextFormat::Text:
        title = i18nc("@title:window", "Export as Plain Text");
        suffix = QStringLiteral(".txt");
        filter = i18n("Text files (*.txt);;All files (*)");
        write = [source, options](const QString &output, QString *error) {
            return Convert::toText(source, output, options, error);
        };
        break;
    case TextFormat::Html:
        title = i18nc("@title:window", "Export as HTML");
        suffix = QStringLiteral(".html");
        filter = i18n("Web pages (*.html);;All files (*)");
        write = [source, options](const QString &output, QString *error) {
            return Convert::toHtml(source, output, options, error);
        };
        break;
    case TextFormat::Markdown:
        title = i18nc("@title:window", "Export as Markdown");
        suffix = QStringLiteral(".md");
        filter = i18n("Markdown files (*.md);;All files (*)");
        write = [source, options](const QString &output, QString *error) {
            return Convert::toMarkdown(source, output, options, error);
        };
        break;
    case TextFormat::Tables:
        title = i18nc("@title:window", "Export the Tables");
        suffix = QStringLiteral(".csv");
        filter = i18n("Comma-separated values (*.csv);;All files (*)");
        write = [source](const QString &output, QString *error) {
            return Convert::tablesToCsv(source, output, {}, error);
        };
        break;
    }

    const Destination destination = chooseDestination(title, QFileInfo(source).completeBaseName() + suffix, filter);
    if (!destination.chosen) {
        return;
    }

    QString error;
    if (!write(destination.path, &error)) {
        reportError(title, error);
        return;
    }
    if (!deliver(destination)) {
        return;
    }

    // Said every time, because it is true every time: there is no text in a PDF
    // in the sense this file now holds it, only glyphs at coordinates, and what
    // came out was worked out from where they sit.
    reportNotice(i18n("A PDF holds letters at positions rather than sentences, so this was worked out from the "
                      "layout. Read it over before relying on it."));
    statusBar()->showMessage(i18nc("@info:status", "Written to %1.", QFileInfo(destination.path).fileName()), 8000);
}

void MainWindow::saveForTheWeb()
{
    if (m_document->pageCount() == 0) {
        return;
    }
    if (!commitViewEdits()) {
        return;
    }

    const QString suggestion = m_document->filePath().isEmpty()
        ? QStringLiteral("web.pdf")
        : QFileInfo(m_document->filePath()).completeBaseName() + QStringLiteral("-web.pdf");
    const Destination destination
        = chooseDestination(i18nc("@title:window", "Save for the Web"), suggestion, pdfFilter());
    if (!destination.chosen) {
        return;
    }

    // A separate file rather than a way of saving this one: linearising is
    // about how the bytes are laid out for someone downloading them, and the
    // copy being worked on has no business being reorganised for that.
    DocumentWriter::Options options = writerOptions();
    options.linearize = true;

    QString error;
    if (!DocumentWriter::write(*m_document, destination.path, options, &error)) {
        reportError(i18nc("@title:window", "Cannot Save for the Web"), error);
        return;
    }
    if (!deliver(destination)) {
        return;
    }

    statusBar()->showMessage(
        i18nc("@info:status", "Written so that a reader on a slow connection sees page one first."), 8000);
}

void MainWindow::saveForPrinting()
{
    if (m_document->pageCount() == 0) {
        return;
    }

    const QString title = i18nc("@title:window", "Save for Commercial Printing");
    if (!Convert::isPdfXAvailable()) {
        reportError(title,
                    i18n("Ghostscript is not installed, and it does the conversion. Install the “ghostscript” "
                         "package and try again."));
        return;
    }

    const QString source = tools::savedPath(m_document, this);
    if (source.isEmpty()) {
        return;
    }

    const Destination destination
        = chooseDestination(title, QFileInfo(source).completeBaseName() + QStringLiteral("-pdfx.pdf"), pdfFilter());
    if (!destination.chosen) {
        return;
    }

    // PDF/X-1a, which is what most printers still ask for; the other two levels
    // are a choice for someone who already knows they need one, and the command
    // line is where that someone is.
    QStringList changes;
    QStringList warnings;
    QString error;
    if (!Convert::toPdfX(source, destination.path, Convert::XLevel::X1a, QString(), &changes, &warnings, &error)) {
        reportError(title, error);
        return;
    }
    if (!deliver(destination)) {
        return;
    }

    // What the conversion had to change, and what it could not: both belong in
    // front of someone about to send a file to a press.
    for (const QString &warning : std::as_const(warnings)) {
        reportNotice(warning);
    }
    statusBar()->showMessage(i18nc("@info:status", "Written as PDF/X-1a."), 8000);
}

void MainWindow::showPage(int row)
{
    if (row < 0 || row >= m_document->pageCount()) {
        return;
    }

    // Double-clicking a page means "take me to it", the same as clicking one in
    // the thumbnail strip. It used to render at 1400 pixels on the GUI thread
    // and put a static picture in a modal box: the window froze, the picture
    // showed the file as it was opened rather than as it stands, and there was
    // nothing to do there but close it again.
    showDocumentView(row, PageView::Mode::View);
}

void MainWindow::signDocument()
{
    if (m_document->pageCount() == 0) {
        return;
    }

    const QVector<int> selection = m_view->selectedRows();
    enterEditor(new SignEditor(m_document, m_backend.get(), selection.isEmpty() ? 0 : selection.first(), m_processor));
}

void MainWindow::addWatermark()
{
    if (m_document->pageCount() == 0) {
        return;
    }

    enterEditor(new WatermarkEditor(m_document, m_backend.get(), m_view->selectedRows(), m_processor));
}

void MainWindow::recognizeText()
{
#ifdef PS_WITH_OCR
    if (m_document->pageCount() == 0) {
        return;
    }
    if (!ScanDialog::isUsable()) {
        reportError(i18nc("@title:window", "Text Recognition Unavailable"),
                    i18n("No recognition languages are installed. Install a package such as "
                         "“tesseract-ocr-deu” and try again."));
        return;
    }

    const QVector<int> selection = m_view->selectedRows();
    ScanDialog dialog(selection.size(), m_document->pageCount(), this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    const QVector<int> rows = dialog.appliesToSelectionOnly() ? selection : allRows();
    QString error;
    if (!m_processor->recognizeText(rows, dialog.options(), &error)) {
        if (!error.isEmpty()) {
            reportError(i18nc("@title:window", "Text Recognition Failed"), error);
        }
        return;
    }

    statusBar()->showMessage(
        i18ncp("@info:status", "Text recognised on %1 page.", "Text recognised on %1 pages.", rows.size()), 6000);
#else
    reportError(i18nc("@title:window", "Text Recognition Unavailable"),
                i18n("This build has no text recognition support."));
#endif
}

void MainWindow::compressPages()
{
    if (m_document->pageCount() == 0) {
        return;
    }

    const QVector<int> selection = m_view->selectedRows();
    CompressDialog dialog(selection.size(), m_document->pageCount(), this);
    dialog.setEstimator([this, &dialog, &selection](const Compressor::Options &options, Compressor::Estimate *estimate,
                                                    QString *error) {
        const QVector<int> rows = dialog.appliesToSelectionOnly() ? selection : allRows();
        return m_processor->estimateCompression(rows, options, estimate, error);
    });

    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    const QVector<int> rows = dialog.appliesToSelectionOnly() ? selection : allRows();
    Compressor::Report report;
    QString error;
    if (!m_processor->compress(rows, dialog.options(), &report, &error)) {
        if (!error.isEmpty()) {
            reportError(i18nc("@title:window", "Compression Failed"), error);
        }
        return;
    }

    if (report.outcome == Compressor::Outcome::AlreadyOptimal) {
        // Saying "0 %" and stopping there would look like a failure.
        statusBar()->showMessage(i18nc("@info:status", "Already as small as it gets, so nothing was changed."), 8000);
        return;
    }

    statusBar()->showMessage(i18nc("@info:status", "%1 smaller.", QLocale().formattedDataSize(report.savedBytes())),
                             8000);
}

void MainWindow::closeDocument()
{
    if (confirmDiscard()) {
        m_document->clear();
    }
}

// ── Page operations ───────────────────────────────────────────────────────

QVector<int> MainWindow::allRows() const
{
    QVector<int> rows(m_document->pageCount());
    std::iota(rows.begin(), rows.end(), 0);
    return rows;
}

QVector<int> MainWindow::targetRows() const
{
    const QVector<int> selected = m_view->selectedRows();
    if (!selected.isEmpty()) {
        return selected;
    }

    // Acting on everything when nothing is picked is what users expect from
    // "rotate", but never from "delete", which is guarded separately.
    QVector<int> all(m_document->pageCount());
    std::iota(all.begin(), all.end(), 0);
    return all;
}

void MainWindow::deleteSelectedPages()
{
    const QVector<int> rows = m_view->selectedRows();
    if (rows.isEmpty()) {
        return;
    }
    if (rows.size() == m_document->pageCount()) {
        // Deleting everything leaves an empty window; make sure that was meant.
        const auto answer = KMessageBox::warningContinueCancel(
            this, i18n("This removes every page from the document. The file on disk is not touched until you save."),
            i18nc("@title:window", "Delete All Pages?"));
        if (answer != KMessageBox::Continue) {
            return;
        }
    }

    const int firstRemoved = rows.first();
    m_document->undoStack()->push(new RemovePagesCommand(m_document, rows));

    // Put the cursor where the deleted block was, so a run of deletions does
    // not force the user to re-aim each time.
    if (m_document->pageCount() > 0) {
        m_view->selectRows({ std::min(firstRemoved, m_document->pageCount() - 1) });
    }
}

void MainWindow::rotateBy(int degrees)
{
    const QVector<int> rows = targetRows();
    if (rows.isEmpty()) {
        return;
    }

    m_document->undoStack()->push(new RotatePagesCommand(m_document, rows, degrees));

    // Nothing was picked out, so that turned every page there is. One keystroke
    // on a five-hundred-page scan is worth a sentence: it is undoable, and a
    // person who has just done it by accident needs to know that it happened
    // and that it can be taken back.
    if (m_view->selectedRows().isEmpty() && rows.size() > 1) {
        reportNotice(i18ncp("@info", "Turned the whole document, %1 page. Undo puts it back.",
                            "Turned the whole document, all %1 pages. Undo puts them back.", rows.size()));
    }
}

void MainWindow::rotateSelectionLeft()
{
    rotateBy(-90);
}

void MainWindow::rotateSelectionRight()
{
    rotateBy(90);
}

void MainWindow::duplicateSelection()
{
    const QVector<int> rows = m_view->selectedRows();
    if (!rows.isEmpty()) {
        m_document->undoStack()->push(new DuplicatePagesCommand(m_document, rows));
    }
}

void MainWindow::extractSelection()
{
    const QVector<int> rows = m_view->selectedRows();
    if (rows.isEmpty()) {
        return;
    }
    if (!commitViewEdits()) {
        return;
    }

    const QString suggestion = m_document->filePath().isEmpty()
        ? QString()
        : QFileInfo(m_document->filePath()).completeBaseName() + QStringLiteral("-pages.pdf");
    const Destination destination = chooseDestination(
        i18ncp("@title:window", "Extract Page To…", "Extract %1 Pages To…", rows.size()), suggestion, pdfFilter());
    if (!destination.chosen) {
        return;
    }

    QString error;
    if (!DocumentWriter::writeSelection(*m_document, rows, destination.path, writerOptions(), &error)) {
        reportError(i18nc("@title:window", "Cannot Extract Pages"), error);
        return;
    }
    if (!deliver(destination)) {
        return;
    }

    statusBar()->showMessage(i18ncp("@info:status", "Extracted one page to %2", "Extracted %1 pages to %2", rows.size(),
                                    QFileInfo(destination.path).fileName()),
                             5000);
}

void MainWindow::selectAllPages()
{
    // In the document view the thing to select is the text, not the pages:
    // Ctrl+A over a page of words that highlighted a row of thumbnails would be
    // the wrong answer to an unambiguous question.
    if (showingDocument()) {
        m_pageView->selectAll();
        return;
    }
    m_view->selectAll();
}

// ── State ─────────────────────────────────────────────────────────────────

void MainWindow::updateTitle()
{
    setCaption(m_document->displayName(), hasUnsavedWork());
}

bool MainWindow::hasPendingViewEdits() const
{
    return (m_annotationOverlay && m_annotationOverlay->isModified()) || (m_textOverlay && m_textOverlay->hasEdits())
        || (m_objectOverlay && (m_objectOverlay->hasPendingEdits() || m_objectOverlay->hasPendingInsertions()))
        || (m_formDesign && m_formDesign->hasPendingWork()) || (m_formOverlay && m_formOverlay->isModified());
}

bool MainWindow::hasUnsavedWork() const
{
    return m_document->isModified() || hasPendingViewEdits();
}

void MainWindow::pendingWorkChanged()
{
    updateTitle();
    updateActionState();
}

QString MainWindow::modeName(WorkMode mode)
{
    switch (mode) {
    case WorkMode::Read:
        return i18nc("@info:status which mode the window is in", "Reading and filling in");
    case WorkMode::Page:
        return i18nc("@info:status which mode the window is in", "Editing the page");
    case WorkMode::Form:
        return i18nc("@info:status which mode the window is in", "Editing the form");
    case WorkMode::Organise:
        return i18nc("@info:status which mode the window is in", "Arranging the pages");
    }
    return {};
}

void MainWindow::updateStatusBar()
{
    if (m_modeLabel) {
        m_modeLabel->setText(modeName(m_mode));
    }
    if (m_pageBox) {
        const int pages = m_document->pageCount();
        QSignalBlocker blocker(m_pageBox);
        m_pageBox->setRange(1, qMax(1, pages));
        m_pageBox->setEnabled(pages > 0);
        m_pageBox->setValue(qBound(1, m_pageView->currentPage() + 1, qMax(1, pages)));
        m_pageTotal->setText(i18ncp("@info:status how many pages there are", "of %1 page", "of %1 pages", pages));
    }

    const int pages = m_document->pageCount();
    m_pageCountLabel->setText(i18ncp("@info:status", "%1 page", "%1 pages", pages));
    // One of the two, never both. "Which page, of how many" is the reader's
    // question and belongs to the document view; "how many pages are there" is
    // the organiser's. Showing both put the same number up twice in a row: the
    // bar read "of 15 pages 15 pages".
    const bool reading = showingDocument();
    m_pageBox->setVisible(reading);
    m_pageTotal->setVisible(reading);
    m_pageCountLabel->setVisible(!reading);

    const int selected = m_view->selectedRows().size();
    m_selectionLabel->setText(selected > 0 ? i18ncp("@info:status", "%1 selected", "%1 selected", selected)
                                           : QString());
}

void MainWindow::updateActionState()
{
    // Offered only when there is a chosen object to take a change back from.
    // A command that is always available and usually does nothing is how a user
    // learns to stop trusting the menu.
    if (QAction *takeBack = actionCollection()->action(QStringLiteral("object_take_back"))) {
        takeBack->setEnabled(m_objectOverlay && !m_objectOverlay->chosenObjects().isEmpty());
    }

    const bool hasPages = m_document->pageCount() > 0;
    const bool hasSelection = !m_view->selectedRows().isEmpty();

    // Not "the document view is not up": an editing tool's stage is neither the
    // grid nor the document, and the pages the selection refers to are not on
    // screen there either.
    const bool showingPages = m_stack && m_stack->currentWidget() == m_view;

    const auto enable = [this](const char *name, bool on) {
        if (QAction *action = actionCollection()->action(QLatin1String(name))) {
            action->setEnabled(on);
        }
    };

    // Only where the pages are on show. Delete is bound to the Delete key
    // window-wide, and a shortcut is matched before the key reaches any widget:
    // in the document view that meant one keystroke removed the page being
    // read, in the mode whose own tooltip promises nothing can be changed by
    // accident. Disabled, the shortcut does not fire and the key travels on to
    // the view, where the layers can make it mean the field or the comment
    // under the pointer.
    enable("page_delete", hasSelection && showingPages);
    enable("page_duplicate", hasSelection);
    enable("page_extract", hasSelection);
    enable("page_rotate_left", hasPages);
    enable("page_rotate_right", hasPages);
    enable("file_save", hasPages);
    enable("file_save_as", hasPages);
    enable("file_print", hasPages);
    enable("file_print_preview", hasPages);
    enable("page_ocr", hasPages);
    enable("page_compress", hasPages);
    enable("page_sign", hasPages);
    enable("page_watermark", hasPages);
    enable("page_number", hasPages);
    enable("page_crop", hasPages);
    enable("page_redact", hasPages);
    enable("page_annotate", hasPages);
    enable("document_form", hasPages);
    enable("page_text", hasPages);
    enable("document_archive", hasPages);
    // No longer "once it is on disk": comparing offers to save first, the same
    // way every other tool that reads a file does.
    enable("document_compare", hasPages);
    enable("comments_export", hasPages);
    enable("comments_import", hasPages);
    enable("page_nup", m_document->pageCount() >= 2);
    enable("file_split", m_document->pageCount() > 1);
    enable("document_metadata", hasPages);
    enable("document_sanitize", hasPages);
    enable("document_password", hasPages);
    enable("edit_select_all", hasPages);
    for (const char *name : { "export_images", "export_text", "export_html", "export_markdown", "export_tables",
                              "export_web", "export_pdfx" }) {
        enable(name, hasPages);
    }

    // The tools all read a file, and every one of them returned in silence with
    // nothing open, and a menu of commands that look available and do nothing
    // teaches the user that the whole menu is decoration. Typesetting is the
    // exception on purpose: it *makes* a document out of text and is the one
    // thing here worth reaching for with nothing open.
    for (const char *name : { "tool_preflight", "tool_colour", "tool_fonts", "tool_images", "tool_layout",
                              "tool_object_list", "tool_form_behaviour", "tool_form_builder", "tool_objects" }) {
        enable(name, hasPages);
    }

    // Reachable exactly when there is something waiting to be kept.
    enable("edit_apply", hasPendingViewEdits());
}

// ── Window events ─────────────────────────────────────────────────────────

bool MainWindow::confirmDiscard()
{
    // An editing mode is work in progress too, and it is anchored to rows of
    // the document that is about to go. Asked first, and by the mode itself, so
    // the question names what is at stake ("keep the trimming?") rather than
    // rolling it into a question about the document.
    if (m_editor) {
        leaveEditor(false);
    }

    if (!hasUnsavedWork()) {
        return true;
    }

    const auto answer = KMessageBox::warningTwoActionsCancel(
        this, i18n("“%1” has unsaved changes.", m_document->displayName()), i18nc("@title:window", "Unsaved Changes"),
        KStandardGuiItem::save(), KStandardGuiItem::discard());

    switch (answer) {
    case KMessageBox::PrimaryAction:
        return saveDocument();
    case KMessageBox::SecondaryAction:
        return true;
    default:
        return false;
    }
}

// ── Editing in the window ─────────────────────────────────────────────────

void MainWindow::enterEditor(EditorMode *mode)
{
    if (!mode) {
        return;
    }
    if (m_editor) {
        leaveEditor(false);
    }

    m_editor = mode;
    mode->setParent(this);

    QWidget *stage = mode->stage();
    QWidget *panel = mode->panel();

    m_stack->addWidget(stage);
    m_stack->setCurrentWidget(stage);

    m_toolDock->setWidget(panel);
    m_toolDock->setWindowTitle(mode->title());
    m_toolDock->show();
    m_toolDock->raise();

    connect(mode, &EditorMode::finished, this, &MainWindow::leaveEditor);
    connect(mode, &EditorMode::statusMessage, this,
            [this](const QString &text) { statusBar()->showMessage(text, 8000); });

    updateActionState();
}

void MainWindow::leaveEditor(bool commit)
{
    if (!m_editor || m_leavingEditor) {
        return;
    }

    // Set before anything else: taking the panel out of the dock hides the
    // dock, which is wired to call this again.
    m_leavingEditor = true;
    EditorMode *mode = m_editor;

    bool keep = commit;
    if (!commit && !mode->isUnchanged()) {
        // Only when there is something to lose. A mode opened and closed again
        // must not ask a question the user cannot answer wrongly.
        keep = KMessageBox::questionTwoActions(this, i18n("Keep what you have done here?"), mode->title(),
                                               KGuiItem(i18nc("@action:button", "Keep")),
                                               KGuiItem(i18nc("@action:button", "Throw Away")))
            == KMessageBox::PrimaryAction;
    }

    QString error;
    if (keep && !mode->commit(&error)) {
        reportError(mode->title(), error);
    }

    m_stack->setCurrentWidget(m_view);
    m_stack->removeWidget(mode->stage());
    m_toolDock->setWidget(nullptr);
    m_toolDock->hide();

    // The panel and the stage belong to the mode, and the mode is about to go.
    // Qt would otherwise leave them parented to widgets that outlive it.
    mode->stage()->setParent(nullptr);
    mode->panel()->setParent(nullptr);

    m_editor = nullptr;
    mode->deleteLater();
    m_leavingEditor = false;

    m_view->setFocus();
    updateActionState();
}

// ── Choosing files ────────────────────────────────────────────────────────

QUrl MainWindow::chooserLocation() const
{
    // A document fetched from a share belongs to that share, not to the
    // scratch directory it happens to be sitting in.
    if (!m_remoteOrigin.isEmpty()) {
        return m_remoteOrigin.adjusted(QUrl::RemoveFilename);
    }
    const QString path = m_document ? m_document->filePath() : QString();
    if (!path.isEmpty()) {
        return QUrl::fromLocalFile(QFileInfo(path).absolutePath());
    }
    return {};
}

QString MainWindow::chooseFileToRead(const QString &title, const QString &filter)
{
    const QUrl url = QFileDialog::getOpenFileUrl(this, title, chooserLocation(), filter);
    if (url.isEmpty()) {
        return {};
    }

    QString error;
    const QString local = RemoteFile::fetch(url, this, &error);
    if (local.isEmpty()) {
        reportError(title, error);
    }
    return local;
}

QStringList MainWindow::chooseFilesToRead(const QString &title, const QString &filter)
{
    const QList<QUrl> urls = QFileDialog::getOpenFileUrls(this, title, chooserLocation(), filter);

    QStringList paths;
    paths.reserve(urls.size());
    for (const QUrl &url : urls) {
        QString error;
        const QString local = RemoteFile::fetch(url, this, &error);
        if (local.isEmpty()) {
            // All or nothing. Inserting four of five chosen documents and
            // mentioning the fifth in a banner is how pages go missing.
            reportError(title, error);
            return {};
        }
        paths.append(local);
    }
    return paths;
}

MainWindow::Destination MainWindow::chooseDestination(const QString &title, const QString &suggestedName,
                                                      const QString &filter)
{
    QUrl start = chooserLocation();
    if (!suggestedName.isEmpty()) {
        if (start.isEmpty()) {
            start = QUrl::fromLocalFile(suggestedName);
        } else {
            start = start.adjusted(QUrl::StripTrailingSlash);
            start.setPath(start.path() + u'/' + suggestedName);
        }
    }

    const QUrl url = QFileDialog::getSaveFileUrl(this, title, start, filter);
    if (url.isEmpty()) {
        return {};
    }

    Destination destination;
    destination.chosen = true;
    if (RemoteFile::isLocal(url)) {
        destination.path = url.toLocalFile();
    } else {
        destination.remote = url;
        const QString name = url.fileName().isEmpty() ? QStringLiteral("document.pdf") : url.fileName();
        destination.path = QDir(RemoteFile::scratchDirectory()).filePath(name);
    }
    return destination;
}

MainWindow::Destination MainWindow::chooseDirectory(const QString &title)
{
    const QUrl url = QFileDialog::getExistingDirectoryUrl(this, title, chooserLocation());
    if (url.isEmpty()) {
        return {};
    }

    Destination destination;
    destination.chosen = true;
    if (RemoteFile::isLocal(url)) {
        destination.path = url.toLocalFile();
        return destination;
    }

    // Its own subdirectory, so that two exports in one session do not deliver
    // each other's leftovers.
    static int run = 0;
    const QString folder = QDir(RemoteFile::scratchDirectory()).filePath(QStringLiteral("export-%1").arg(++run));
    if (!QDir().mkpath(folder)) {
        reportError(title, i18n("A working directory for the export could not be made."));
        return {};
    }

    destination.remote = url;
    destination.path = folder;
    return destination;
}

bool MainWindow::deliver(const Destination &destination, const QStringList &files)
{
    if (destination.remote.isEmpty()) {
        return true;
    }

    const QString title = i18nc("@title:window", "Cannot Save");
    QString error;

    if (files.isEmpty()) {
        if (!RemoteFile::store(destination.path, destination.remote, this, &error)) {
            reportError(title, error);
            return false;
        }
        return true;
    }

    for (const QString &file : files) {
        QUrl target = destination.remote.adjusted(QUrl::StripTrailingSlash);
        target.setPath(target.path() + u'/' + QFileInfo(file).fileName());
        if (!RemoteFile::store(file, target, this, &error)) {
            reportError(title, error);
            return false;
        }
    }
    return true;
}

bool MainWindow::showingDocument() const
{
    return m_stack && m_pageView && m_stack->currentWidget() == m_pageView;
}

void MainWindow::setWorkMode(WorkMode mode)
{
    // A mode's stage sits where the central view goes, so changing what the
    // window is for has to take it down first. Left up, it went on living with
    // its panel in the dock and a Keep button still pointing at a selection the
    // user had long since forgotten about.
    if (m_editor) {
        leaveEditor(false);
    }

    m_mode = mode;

    // Every layer is taken off and only the ones this mode is about are put
    // back. Adding them in this order each time is what fixes which layer wins
    // a click: the view honours the order it is given, and that order is the
    // answer to "what does clicking here do".
    for (PageView::Overlay *layer :
         { static_cast<PageView::Overlay *>(m_annotationOverlay), static_cast<PageView::Overlay *>(m_formOverlay),
           static_cast<PageView::Overlay *>(m_textOverlay), static_cast<PageView::Overlay *>(m_formDesign),
           static_cast<PageView::Overlay *>(m_objectOverlay) }) {
        m_pageView->removeOverlay(layer);
    }

    switch (mode) {
    case WorkMode::Read:
        m_stack->setCurrentWidget(m_pageView);
        m_pageView->setMode(PageView::Mode::View);
        // Comments are readable in a reader; fields are fillable in one. That
        // is the whole of what this mode offers, and it cannot alter the page.
        m_pageView->addOverlay(m_annotationOverlay);
        m_pageView->addOverlay(m_formOverlay);
        break;

    case WorkMode::Page:
        m_stack->setCurrentWidget(m_pageView);
        m_pageView->setMode(PageView::Mode::Edit);
        // Comments first so a pen in hand wins the press; then the words,
        // because a click on a line of text means the words rather than the
        // block; the block last.
        m_pageView->addOverlay(m_annotationOverlay);
        m_pageView->addOverlay(m_textOverlay);
        m_pageView->addOverlay(m_objectOverlay);
        break;

    case WorkMode::Form:
        m_stack->setCurrentWidget(m_pageView);
        m_pageView->setMode(PageView::Mode::Edit);
        // Only the fields. Laying a form out over a page whose text also
        // answers to the mouse is how a field ends up moving a paragraph.
        m_pageView->addOverlay(m_formDesign);
        break;

    case WorkMode::Organise:
        m_stack->setCurrentWidget(m_view);

        // Qt hands the grid the middle of the window on the next pass through
        // the event loop, and the overview is worked out from the size it is
        // about to have rather than the size it last had, which, the first time
        // it is asked, is no size at all.
        if (QLayout *layout = m_stack->layout()) {
            layout->activate();
        }

        // Arranging pages is done by eye, over several pages at once, so this
        // is where the window lands the first time it is asked for. A size the
        // user has chosen for the grid themselves is theirs and is left alone,
        // including one restored from the last session.
        if (!m_gridSizeChosen) {
            m_arrangingGrid = true;
            m_view->showOverview();
            m_arrangingGrid = false;
        }
        break;
    }

    // The page grid already shows every page here, so a second strip of the
    // same thing is a third of the window spent twice. A strip the user closed
    // themselves stays closed when the mode gives it back.
    m_thumbnails->setTemporarilyHidden(mode == WorkMode::Organise);

    if (mode != WorkMode::Organise) {
        m_pageView->setFocus();
    } else {
        m_view->setFocus();
    }

    if (QAction *action = m_modeActions.value(int(mode))) {
        action->setChecked(true);
    }
    showModeToolBar();
    updateStatusBar();
    updateActionState();
    // The two views hold two reading sizes, and the menu and the status bar
    // speak for whichever of them is now on screen.
    updateFitActions();
    if (m_zoomSlider) {
        // The slider is the grid's own control and says so in its tooltip.
        // Left on show in the document modes it did nothing anyone could see
        // and quietly resized the grid behind their back.
        m_zoomSlider->setVisible(mode == WorkMode::Organise);
    }
}

void MainWindow::discardStaleLayoutDescription()
{
    // Worked out rather than asked for: KXMLGUIClient only knows where its
    // local copy lives once it has read the description, and by then it is too
    // late for this to make any difference.
    const QString path = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
        + QStringLiteral("/kxmlgui5/") + componentName() + QStringLiteral("/pdf-smithyui.rc");

    QFile file(path);
    if (!file.exists() || !file.open(QIODevice::ReadOnly)) {
        return;
    }
    const QByteArray saved = file.readAll();
    file.close();

    // What the user set for one action at a time (a different shortcut, a
    // different icon) is a preference and is kept. The containers are a layout
    // and this build has a new one.
    QString preferences;
    int version = 0;
    bool hasContainers = false;
    QXmlStreamReader reader(saved);
    while (!reader.atEnd() && !reader.hasError()) {
        if (reader.readNext() != QXmlStreamReader::StartElement) {
            continue;
        }
        if (reader.name() == QLatin1String("gui")) {
            version = reader.attributes().value(QLatin1String("version")).toInt();
        } else if (reader.name() == QLatin1String("MenuBar") || reader.name() == QLatin1String("Menu")
                   || reader.name() == QLatin1String("ToolBar")) {
            hasContainers = true;
        } else if (reader.name() == QLatin1String("ActionProperties")) {
            QXmlStreamWriter writer(&preferences);
            int depth = 0;
            do {
                if (reader.isStartElement()) {
                    ++depth;
                } else if (reader.isEndElement()) {
                    --depth;
                }
                writer.writeCurrentToken(reader);
                if (depth == 0) {
                    break;
                }
                reader.readNext();
            } while (!reader.atEnd());
        }
    }

    if (reader.hasError() || version >= LayoutGeneration || !hasContainers) {
        // Either it cannot be read (in which case leaving it alone is the only
        // safe thing), or it was written against this description and belongs
        // to the user, or there is nothing left in it to take away.
        return;
    }

    // Rewritten rather than deleted, and in one step rather than in place: a
    // second window opening at the same moment is reading this very file, and
    // it would otherwise find half of one or none at all.
    //
    // Written back with the version it came with, not ours. XMLGUI reads every
    // copy it can find and keeps the one with the highest number: raise this
    // one and it would win against the description compiled into the program,
    // leaving a window with no menus and no toolbars at all.
    QSaveFile replacement(path);
    if (replacement.open(QIODevice::WriteOnly)) {
        QTextStream out(&replacement);
        out << QStringLiteral("<!DOCTYPE gui SYSTEM \"kpartgui.dtd\">\n<gui name=\"%1\" version=\"%2\">\n")
                   .arg(componentName())
                   .arg(version)
            << preferences << QStringLiteral("\n</gui>\n");
        out.flush();
        replacement.commit();
    }
}

KToolBar *MainWindow::toolBar(const QString &name) const
{
    // Asked of the window rather than of the factory, because this has to be
    // able to answer "not yet" quietly: it is called while the window is still
    // being built, before the description has been turned into widgets.
    return findChild<KToolBar *>(name);
}

void MainWindow::applyToolBarStyle(KToolBar *bar, Qt::ToolButtonStyle style)
{
    const bool isMain = bar->objectName() == QLatin1String("mainToolBar");
    const QString wanted = QStringLiteral("TextUnderIcon");

    // What they chose for this bar, from its own context menu. Anything other
    // than what this program writes is a decision, and is left alone; what this
    // program wrote is not a decision, so it does not block the next start.
    KConfigGroup saved = autoSaveConfigGroup();
    KConfigGroup own(&saved, QStringLiteral("Toolbar ") + bar->objectName());
    if (own.hasKey("ToolButtonStyle") && own.readEntry("ToolButtonStyle", QString()) != wanted) {
        return;
    }

    // What they chose for every application, in the system settings. KToolBar
    // keeps main toolbars and the rest apart there, and so must this.
    const KConfigGroup plasma(KSharedConfig::openConfig(), QStringLiteral("Toolbar style"));
    if (plasma.hasKey(isMain ? "ToolButtonStyle" : "ToolButtonStyleOtherToolbars")) {
        return;
    }

    bar->setToolButtonStyle(style);
    bar->setIconDimensions(KIconLoader::SizeMedium);

    // Written as well as set. A KToolBar reads its own group and puts the
    // desktop's smaller default back over anything merely set beforehand, so
    // the entry is what makes the size survive the next pass.
    own.writeEntry("ToolButtonStyle", wanted);
    own.writeEntry("IconSize", int(KIconLoader::SizeMedium));
}

void MainWindow::showModeToolBar()
{
    // One extra toolbar at a time, holding what this mode is for. A window that
    // shows every tool at once asks the user to work out which of them apply,
    // and that is the question the mode already answered.
    for (int i = 0; i < modeToolBarCount; ++i) {
        if (KToolBar *bar = toolBar(QLatin1String(modeToolBars[i].name))) {
            bar->setVisible(modeToolBars[i].mode == m_mode);
        }
    }

    // Anything else is a bar this build no longer has a description for, left
    // behind in a profile that predates the rearrangement. It has no owner, so
    // nothing would ever take it down again, and it would go on taking a share
    // of the row from the bar that does belong there.
    const QList<KToolBar *> present = toolBars();
    for (KToolBar *bar : present) {
        if (bar->objectName() == QLatin1String("mainToolBar")) {
            continue;
        }
        bool known = false;
        for (int i = 0; i < modeToolBarCount && !known; ++i) {
            known = bar->objectName() == QLatin1String(modeToolBars[i].name);
        }
        if (!known) {
            bar->hide();
        }
    }
}

void MainWindow::applyLayoutDefaults()
{
    KToolBar *main = toolBar(QStringLiteral("mainToolBar"));

    // The first of the mode bars still in the strip along the top. One dragged
    // to another edge belongs to whoever dragged it and is left alone.
    KToolBar *leading = nullptr;
    for (int i = 0; i < modeToolBarCount && !leading; ++i) {
        KToolBar *bar = toolBar(QLatin1String(modeToolBars[i].name));
        if (bar && toolBarArea(bar) == Qt::TopToolBarArea) {
            leading = bar;
        }
    }

    // Asked of the window rather than of a note left in the settings. Every
    // toolbar starts life on the same row, and a row is shared out left to
    // right: the main bar took all of it and the mode's bar was handed the
    // pixels left over at the right-hand edge, present, up, answering
    // isVisible(), and with nothing on it a person could see. That is the whole
    // of "the view toolbar is not shown at all", and a stored "already fixed"
    // flag that outlived the layout it described would leave it broken for
    // good.
    if (main && leading && toolBarArea(main) == Qt::TopToolBarArea && !toolBarBreak(leading)) {
        // Taken apart and put back rather than nudged: what was saved can have
        // any bar anywhere, and "a row of its own" is a statement about the row
        // rather than about one bar in it.
        const bool mainWasUp = !main->isHidden();
        removeToolBar(main);
        for (int i = 0; i < modeToolBarCount; ++i) {
            if (KToolBar *bar = toolBar(QLatin1String(modeToolBars[i].name))) {
                removeToolBar(bar);
            }
        }

        addToolBar(Qt::TopToolBarArea, main);
        main->setVisible(mainWasUp);

        addToolBarBreak(Qt::TopToolBarArea);
        for (int i = 0; i < modeToolBarCount; ++i) {
            if (KToolBar *bar = toolBar(QLatin1String(modeToolBars[i].name))) {
                addToolBar(Qt::TopToolBarArea, bar);
            }
        }
    }

    // The panels are a preference rather than a layout, so a note in the
    // settings is the right authority for them and this happens once. A profile
    // made before this still has the contents sidebar saved as open, because the
    // window used to throw it open by itself for any document with bookmarks.
    const KConfigGroup layout(KSharedConfig::openConfig(), QStringLiteral("Layout"));
    if (!m_panelsArranged && layout.readEntry("Generation", 0) < LayoutGeneration) {
        m_panelsArranged = true;
        if (m_thumbnails) {
            m_thumbnails->show();
        }
        if (m_outlineDock) {
            m_outlineDock->hide();
        }
        if (m_inspector) {
            m_inspector->hide();
        }
    }

    // Every time, because KToolBar puts the saved settings back over the top on
    // each pass. Large icons with their names underneath on every bar: a button
    // whose picture has to carry the whole meaning is a button people learn by
    // trial, and this program has ninety of them.
    if (main) {
        applyToolBarStyle(main, Qt::ToolButtonTextUnderIcon);
    }
    for (int i = 0; i < modeToolBarCount; ++i) {
        if (KToolBar *bar = toolBar(QLatin1String(modeToolBars[i].name))) {
            applyToolBarStyle(bar, Qt::ToolButtonTextUnderIcon);
        }
    }

    // Every time, whatever was saved: the tool panel belongs to an editing mode
    // and nothing is being edited yet, so one restored from last time would come
    // up empty with no way to fill it.
    if (m_toolDock && !m_editor) {
        m_toolDock->hide();
    }

    showModeToolBar();
}

void MainWindow::applyMainWindowSettings(const KConfigGroup &config)
{
    KXmlGuiWindow::applyMainWindowSettings(config);

    // What was saved says where the bars sit. It does not get to say which of
    // them is up: that follows the mode, and a window coming back with last
    // week's toolbar would be answering a question the user has since answered
    // differently.
    applyLayoutDefaults();

    if (QAction *showMenuBar = actionCollection()->action(QStringLiteral("options_show_menubar"))) {
        showMenuBar->setChecked(!menuBar()->isHidden());
    }
}

void MainWindow::saveNewToolbarConfig()
{
    KXmlGuiWindow::saveNewToolbarConfig();

    // The whole GUI has just been built again from the edited description, so
    // every bar is back on screen and the mode has to say so again.
    showModeToolBar();
}

void MainWindow::showDocumentView(int row, PageView::Mode mode)
{
    // Commands that take the user to the page say which of the two editing
    // modes they mean; reading is the only other thing the view does.
    setWorkMode(mode == PageView::Mode::Edit ? WorkMode::Page : WorkMode::Read);

    // Only where it is a different page. Several of these commands are "work on
    // what I am looking at" and pass the page already showing, and scrolling to
    // the top of it would throw the reader back to the start of a page they
    // were half way down for no reason they asked for.
    if (row != m_pageView->currentPage()) {
        m_pageView->goToPage(row);
    }
}

bool MainWindow::commitViewEdits()
{
    if (!m_processor) {
        return true;
    }

    // In this order on purpose. Correcting the words rewrites the instruction
    // stream and renumbers the drawing operations, so an object edit worked out
    // against the old numbering has to go first; filling in a form touches only
    // the field dictionaries and is independent of both.
    bool ok = true;
    QString error;
    QStringList said;

    if (m_objectOverlay && (m_objectOverlay->hasPendingEdits() || m_objectOverlay->hasPendingInsertions())) {
        PageComposer::Report report;
        if (m_processor->composeObjects(m_objectOverlay->pendingEdits(), m_objectOverlay->pendingInsertions(), &report,
                                        &error)) {
            m_objectOverlay->takeBackAll();
            said += report.refusals;
        } else {
            ok = false;
        }
    }

    if (ok && m_textOverlay && m_textOverlay->hasEdits()) {
        // Two batches, because whether a line grows or is squeezed into the box
        // it had is a per-line decision and the engine takes it per call. A
        // single call would silently squeeze every line the user asked to grow,
        // which is exactly the awkwardness this was meant to end.
        for (const TextOverlay::Fitting fitting : { TextOverlay::Fitting::Grow, TextOverlay::Fitting::KeepBox }) {
            const QVector<TextEdit::Replacement> batch = m_textOverlay->replacements(fitting);
            if (batch.isEmpty()) {
                continue;
            }
            TextEdit::Report report;
            if (!m_processor->correctText(batch, TextOverlay::optionsFor(fitting), &report, &error)) {
                ok = false;
                break;
            }
            said += report.refusals;
            said += report.overflowed;
            // What was done that the user did not ask for and would want to
            // know: a face whose licence says it may not travel with the file,
            // an italic cut that is not installed so the upright one went in
            // instead. These are not refusals, because the change was made, but
            // a document that silently ships without the font it was set in is
            // a document that looks wrong on the next machine it opens on.
            said += report.notes;
        }
        if (ok) {
            m_textOverlay->discardEdits();
        }
    }

    if (ok && m_annotationOverlay && m_annotationOverlay->isModified()) {
        // The empty case is asked about here rather than read out of a false
        // return: the engine refuses a call with no rows in it, and treating
        // every silent refusal as success meant a failed write was reported as
        // a save with the comments quietly missing.
        const QHash<int, QVector<Annotation>> marks = m_annotationOverlay->annotationsByRow();
        if (marks.isEmpty()) {
            m_annotationOverlay->reload();
        } else if (m_processor->annotate(marks, &error)) {
            m_annotationOverlay->reload();
        } else {
            ok = false;
        }
    }

    if (ok && m_formDesign && m_formDesign->hasPendingWork()) {
        const FormDesignOverlay::Work work = m_formDesign->pendingWork();
        QVector<QPair<QString, QString>> renamed;
        renamed.reserve(work.renamed.size());
        for (const FormDesignOverlay::Rename &rename : work.renamed) {
            renamed.append({ rename.from, rename.to });
        }
        if (m_processor->buildForm(work.removed, renamed, work.changed, work.added, &error)) {
            m_formDesign->takeBackAll();
            // The field list the two form layers work from is now out of date:
            // names have moved and boxes have been drawn.
            loadFormForView();
        } else {
            ok = false;
        }
    }

    if (ok && m_formOverlay && m_formOverlay->isModified()) {
        int filled = 0;
        QStringList notes;
        if (m_processor->fillForm(m_formOverlay->values(), false, &filled, &notes, &error)) {
            m_formOverlay->clearChanges();
            said += notes;
        } else {
            ok = false;
        }
    }

    if (!ok) {
        reportError(i18nc("@title:window", "Cannot Keep the Changes"), error);
        return false;
    }
    for (const QString &note : std::as_const(said)) {
        reportNotice(note);
    }
    if (!said.isEmpty() || m_pageView) {
        m_pageView->refresh();
    }
    return true;
}

void MainWindow::loadFormForView()
{
    if (!m_formOverlay) {
        return;
    }
    // From the file rather than from the model: a form lives in the document's
    // structure, not in the page list, so there is nothing to read until the
    // document is on disk.
    const QString path = m_document->filePath();
    if (path.isEmpty()) {
        m_formOverlay->setFields({});
        if (m_objectOverlay) {
            m_objectOverlay->setSource(QString());
        }
        if (m_formDesign) {
            m_formDesign->setSource(QString());
            m_formDesign->setFields({});
        }
        return;
    }
    m_formOverlay->setFields(Forms::read(path, nullptr));
    // Where a form states no /TU, the words printed beside a field are the only
    // label there is, so the layer is given the renderer to read them with.
    // Guessed, and marked as guessed wherever it is shown.
    m_formOverlay->setLabelSource(m_backend.get(), m_document->pageCount() > 0 ? m_document->pageAt(0).sourceId : -1);
    if (m_objectOverlay) {
        m_objectOverlay->setSource(path);
    }
    if (m_formDesign) {
        m_formDesign->setSource(path);
        m_formDesign->setFields(Forms::read(path, nullptr));
    }
}

void MainWindow::showPreflightTool()
{
    tools::showPreflight(m_document, this);
}

void MainWindow::showColourTool()
{
    tools::showColour(m_document, this);
}

void MainWindow::showFontsTool()
{
    tools::showFonts(m_document, this);
}

void MainWindow::showImagesTool()
{
    tools::showImages(m_document, this);
}

void MainWindow::showLayoutTool()
{
    tools::showLayout(m_document, this);
}

void MainWindow::showFormBuilderTool()
{
    // Laying a form out happens on the page: pick a kind from the toolbar and
    // drag it where it goes. There is nothing a window of coordinate boxes can
    // add to that, so this command now takes you there rather than opening one.
    setWorkMode(WorkMode::Form);
    m_formDesign->setKindToPlace(FormDesignOverlay::Kind::Text);
    m_inspector->show();
}

void MainWindow::showFormBehaviourTool()
{
    // What is left in the window: tab order, formatting and validation rules,
    // and reading and writing the answers. None of those is a gesture on a
    // page, so none of them belongs on one.
    const QVector<int> selection = m_view->selectedRows();
    tools::showFormBuilder(m_document, m_backend.get(), selection.isEmpty() ? 0 : selection.first(), this);
}

void MainWindow::showObjectsTool()
{
    // Same reasoning as the form: what a page draws is moved by dragging it,
    // not by typing its coordinates into a table beside a picture of it.
    showDocumentView(m_pageView->currentPage(), PageView::Mode::Edit);
    m_inspector->show();
}

void MainWindow::showObjectListTool()
{
    // The same page as a list. Kept because a list can be read out, sorted and
    // searched, and because an object too small to click is still a row here.
    const QVector<int> selection = m_view->selectedRows();
    tools::showObjects(m_document, m_backend.get(), selection.isEmpty() ? 0 : selection.first(), this);
}

void MainWindow::showTypesetTool()
{
    tools::showTypeset(m_document, this);
}

QString MainWindow::saveForTools()
{
    if (m_document->pageCount() == 0) {
        return {};
    }
    // Before the "is it already on disk" question, not after: a comment drawn a
    // minute ago leaves the undo stack clean, so every tool used to be handed
    // the file as it stood before the user's last hour.
    if (!commitViewEdits()) {
        return {};
    }
    if (!m_document->isModified() && !m_document->filePath().isEmpty()) {
        return m_document->filePath();
    }
    if (!saveDocument()) {
        return {};
    }
    return m_document->filePath();
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (!confirmDiscard()) {
        event->ignore();
        return;
    }

    saveSession();
    KXmlGuiWindow::closeEvent(event);
}

void MainWindow::dragEnterEvent(QDragEnterEvent *event)
{
    // Dropping a PDF anywhere on the window merges it in: the grid handles
    // precise placement, the window handles "just add this".
    if (event->mimeData()->hasUrls()) {
        event->acceptProposedAction();
    }
}

void MainWindow::dropEvent(QDropEvent *event)
{
    QStringList paths;
    for (const QUrl &url : event->mimeData()->urls()) {
        if (url.isLocalFile()) {
            paths.append(url.toLocalFile());
        }
    }
    if (paths.isEmpty()) {
        return;
    }

    if (m_document->pageCount() == 0) {
        openFile(paths.constFirst());
        m_remoteOrigin.clear();
        event->acceptProposedAction();
        return;
    }

    // Dropped on the window rather than between two pages, so it appends. The
    // grid handles precise placement itself.
    insertFilesAt(paths, m_document->pageCount());
    event->acceptProposedAction();
}

void MainWindow::reportNotice(const QString &text)
{
    m_messageBar->setMessageType(KMessageWidget::Information);
    m_messageBar->setText(text);
    m_messageBar->animatedShow();
}

void MainWindow::reportError(const QString &title, const QString &detail)
{
    const QString text = detail.isEmpty() ? i18n("The operation failed.") : detail;
    m_messageBar->setMessageType(KMessageWidget::Error);
    m_messageBar->setText(title.isEmpty() ? text
                                          : i18nc("@info error banner, headline then detail", "%1: %2", title, text));
    m_messageBar->animatedShow();
}

} // namespace ps
