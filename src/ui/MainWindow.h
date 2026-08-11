/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#pragma once

#include "OperationDialogs.h"
#include "core/DocumentWriter.h"
#include "PageView.h"
#include "print/PrintController.h"

#include <QHash>
#include <QUrl>

#include <KXmlGuiWindow>

#include <memory>

class QLabel;
class KMessageWidget;
class QDockWidget;
class QSlider;
class QSpinBox;
class QStackedWidget;
class KRecentFilesAction;
class KToolBar;
class KToolBarPopupAction;
class QMenu;

namespace ps {

class Document;
class EditorMode;
class AnnotationOverlay;
class FormDesignOverlay;
class FormOverlay;
class InspectorDock;
class ObjectOverlay;
class TextOverlay;
class OutlineDock;
class PageGridView;
class PageView;
class PageModel;
class PopplerBackend;
class RenderCache;
class ThumbnailDock;

/**
 * The application window.
 *
 * Deliberately a single window with one central view: tools appear as dock
 * panels rather than modal dialogs, so the document stays visible while it is
 * being worked on.
 */
class MainWindow : public KXmlGuiWindow
{
    Q_OBJECT

public:
    /**
     * What the user is here to do, which decides everything else.
     *
     * The mode picks the central view, which layers of the page are live, which
     * toolbar is up and what a click means. It is deliberately a small, closed
     * set shown as four buttons rather than a collection of independent
     * switches: "can I break this document by clicking" has to be answerable
     * by looking at the window, and a program with six orthogonal toggles
     * cannot answer it.
     *
     * Reading is where the program starts. A document is something you open to
     * read; changing it is a decision, and a decision should take a click.
     */
    enum class WorkMode {
        Read, //!< turn pages, select and copy, fill in forms
        Page, //!< correct text, move pictures, mark up
        Form, //!< draw and arrange form fields
        Organise, //!< reorder, turn, duplicate and remove whole pages
    };
    Q_ENUM(WorkMode)

    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    /** Opens @p path, prompting about unsaved work first. */
    bool openFile(const QString &path);

    /**
     * Opens @p url, fetching it first when it lives on a network share.
     *
     * Saving later writes back to the same place, so a document opened over
     * sftp behaves like any other, which is what the desktop file has been
     * promising all along.
     */
    bool openUrl(const QUrl &url);

    /**
     * True when anything at all would be lost by throwing the document away.
     *
     * The undo stack is only half the answer. The layers over the page (the
     * comments, the corrections, the fields being drawn, the answers being
     * typed) collect their work and hand it over in one step, so until that
     * step happens the stack is clean and the document looks untouched.
     *
     * Deliberately answered here rather than taught to Document: the layers
     * belong to this window's view of the document and the engine knows nothing
     * about them, and a second question at closing time ("and the page edits?")
     * would ask the user twice about one afternoon's work. Saving commits the
     * layers first, so one Save still answers both halves.
     */
    bool hasUnsavedWork() const;

    /** True while a save is set to leave attachments and link actions behind. */
    bool stripsInteractivity() const { return m_stripInteractivity; }

    /** What Clean Up decided, undoably; see CleanUpCommand. */
    void setStripInteractivity(bool strip);

    /**
     * Writes the document out if it is not already on disk, and says where.
     *
     * The editing tools read a **file**, which is what a PDF is and what a
     * script has to be able to hand them, while the window holds a Document
     * that may be pages gathered from several places and written down nowhere.
     * Without this they would silently work on the version before the user's
     * last edit, which looks like the tool ignoring what was just done.
     *
     * @returns the path, or empty when there is nothing to save or the user
     * declined. Declining is not an error; they were asked and said no.
     */
    QString saveForTools();

    /**
     * Puts the window back as it was saved, and then puts the toolbars right.
     *
     * KMainWindow restores the whole window state (where every bar sits, how
     * wide it is, whether it is up) from what was written the last time. That
     * state is the wrong authority for the mode's toolbar, which follows the
     * mode and nothing else, so this is where the two are reconciled: the saved
     * layout first, this window's own rules on top.
     */
    void applyMainWindowSettings(const KConfigGroup &config) override;

protected:
    void closeEvent(QCloseEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;

protected Q_SLOTS:
    /** Rebuilds the window after the toolbar editor; the mode still decides. */
    void saveNewToolbarConfig() override;

private Q_SLOTS:
    void openDocument();
    void insertDocument();

    /** An empty document, so the program can be started at rather than opened into. */
    void newDocument();

    /** Empty paper added to the document that is open. */
    void insertBlankPages();
    void insertFilesAt(const QStringList &paths, int at);
    bool saveDocument();
    bool saveDocumentAs();
    void printDocument();
    void showPrintPreview();
    void closeDocument();

    void showContextMenu(const QPoint &viewportPos);

    /**
     * The menu for a right-click on the document itself, chosen by mode.
     *
     * A different question from the one the page grid answers: reading asks
     * "copy this, mark this", editing asks "cut this, keep what I have done",
     * and neither of them asks about the order of the pages.
     */
    void showPageContextMenu(const QPoint &globalPos);

    void splitDocument();
    void numberPages();
    void cropPages();
    void redactPages();
    void addComments();
    void fillForm();
    void correctText();
    void saveArchival();
    void compareWith();
    void exportComments();
    void importComments();
    void arrangeOnSheets();
    void editMetadata();
    void sanitizeDocument();
    void changePassword();

    void showPreflightTool();
    void showColourTool();
    void showFontsTool();
    void showImagesTool();
    void showLayoutTool();
    void showFormBuilderTool();
    void showFormBehaviourTool();
    void showObjectListTool();
    void showObjectsTool();
    void showTypesetTool();
    void importImages();
    void exportAsImages();

    /** Writes a linearised copy: page one arrives before the rest does. */
    void saveForTheWeb();

    /** Writes PDF/X, which is what a commercial printer asks for. */
    void saveForPrinting();
    void signDocument();
    void addWatermark();
    void showPage(int row);
    void recognizeText();
    void compressPages();

    void deleteSelectedPages();
    void rotateSelectionLeft();
    void rotateSelectionRight();
    void duplicateSelection();
    void extractSelection();
    void selectAllPages();

    /** Reads the document's form so the view can offer it for filling in. */
    void loadFormForView();

    /** Shows @p row in the document view, in @p mode, and puts focus there. */
    void showDocumentView(int row, PageView::Mode mode);

    /** Puts the window into @p mode: view, layers, toolbar and indicator. */
    void setWorkMode(WorkMode mode);

    /** Shows the one extra toolbar the current mode is about, and hides the rest. */
    void showModeToolBar();

    /** What to call @p mode in front of a person. */
    static QString modeName(WorkMode mode);

    /** True while the document view rather than the page grid is on screen. */
    bool showingDocument() const;

    /**
     * Writes what has been typed and dragged in the document view into the
     * document, as undoable steps.
     *
     * The overlays collect rather than write, so that a page's worth of edits
     * becomes one undo step instead of one per keystroke. This is where that
     * collection is cashed in: before saving, and whenever the user asks.
     *
     * @returns false when something refused, having already said so.
     */
    bool commitViewEdits();

    void updateTitle();
    void updateStatusBar();
    void updateActionState();

    /**
     * Ticks the one of the five fits that describes the view now on screen.
     *
     * The grid and the document each keep a reading size of their own, and the
     * five menu entries speak for whichever is up, so this is asked again
     * whenever either of them changes its mind, and whenever the mode changes
     * which of the two is being looked at.
     */
    void updateFitActions();

    /**
     * Work has appeared in, or left, one of the layers over the page.
     *
     * Both halves matter: the title bar has to admit there is something to
     * lose, and "Keep the Changes on the Page" has to be reachable exactly when
     * there is something to keep.
     */
    void pendingWorkChanged();

    /** Puts back everything this window remembers about one document. */
    void documentReset();

private:
    void setupActions();

    /** True while any layer is holding work that is not in the document yet. */
    bool hasPendingViewEdits() const;

    /**
     * Asks for the password to @p fileName; false when the user gave up.
     *
     * @p wasWrong says so plainly rather than showing the same prompt twice,
     * because "nothing happened" is how a user concludes the program is broken
     * rather than that they mistyped.
     */
    bool askForPassword(const QString &fileName, bool wasWrong, QString *password);

    /** Turns @p rows, or the whole document, saying so when it was the lot. */
    void rotateBy(int degrees);

    /**
     * The formats File ▸ Export as… writes that are not PDF at all.
     *
     * All four are reconstructions rather than extractions, because a PDF holds
     * glyphs at coordinates and says nothing about words, lines or paragraphs.
     * That is why they share one path: the honest sentence about it has to be
     * said once and said every time.
     */
    enum class TextFormat {
        Text,
        Html,
        Markdown,
        Tables, //!< the tables found on the pages, as comma-separated values
    };

    void exportAsText(TextFormat format);

    /**
     * Builds the split buttons: a default action, plus a menu of its relatives.
     *
     * Zoom, the pens, inserting and exporting are each half a dozen commands
     * that mean one thing between them. Given a button apiece they fill a bar
     * with things nobody is looking for; given one button with an arrow they
     * are one glance away and cost one row. @p follow makes the button show
     * whichever of its members was chosen last, which is what a tool chooser
     * has to do to be readable at all.
     */
    KToolBarPopupAction *addGroup(const QString &name, const QString &text, const QString &iconName,
                                  const QList<QAction *> &members, bool follow);

    void setupStatusBar();
    void restoreSession();
    void saveSession();

    /** The toolbar called @p name, or nothing before the GUI has been built. */
    KToolBar *toolBar(const QString &name) const;

    /**
     * Draws @p bar's buttons in @p style, unless the user has said otherwise.
     *
     * The description could ask for this with an iconText attribute, and that
     * is the obvious place for it, but a toolbar built with one then ignores
     * the user's Plasma-wide setting *and* the Text Position entry in its own
     * context menu, permanently. A default nobody can change is not a default,
     * so it is applied here, where their choice can be looked for first.
     */
    void applyToolBarStyle(KToolBar *bar, Qt::ToolButtonStyle style);

    /**
     * Throws away a menu and toolbar layout saved against an older description.
     *
     * XMLGUI keeps the user's own copy of the description in their profile
     * (that is what the toolbar editor writes) and prefers every container in
     * it to ours. It prefers it whatever version number it carries, so raising
     * ours is not on its own enough: a profile that has ever been through the
     * toolbar editor keeps the old bars for good and never sees a new one.
     *
     * Only containers go. Whatever the user has set for individual actions is
     * copied across, because that is a preference rather than a layout.
     *
     * Called before the GUI is built, which is the only moment it can matter.
     */
    void discardStaleLayoutDescription();

    /**
     * Puts the bars on rows of their own, once per layout generation.
     *
     * Every toolbar defaults to the same row, and a row is shared out left to
     * right: the main bar took all of it and the mode's bar was handed the
     * dozen pixels left over at the right-hand edge. It was there, it answered
     * isVisible(), and there was nothing to see, which is exactly the shape of
     * the complaint that the toolbars "are not shown at all".
     *
     * Done once per generation rather than on every start, so that a user who
     * drags a bar somewhere else keeps it there.
     */
    void applyLayoutDefaults();

    /** Ascending selected rows, or every row when nothing is selected. */
    QVector<int> targetRows() const;

    /** Every row, for operations that offer a whole-document scope. */
    QVector<int> allRows() const;

    /** Writer settings that follow from the document's own state. */
    DocumentWriter::Options writerOptions() const;

    /** True when it is safe to throw the current document away. */
    bool confirmDiscard();

    // ── Editing in the window ─────────────────────────────────────────────

    /**
     * Puts @p mode up: its stage where the grid is, its tools in the panel.
     *
     * Takes ownership. Entering a second mode leaves the first one the same
     * way closing it would, so there is never a stage on screen belonging to a
     * mode whose tools are gone.
     */
    void enterEditor(EditorMode *mode);

    /** Takes the mode down, writing its work into the document when @p commit. */
    void leaveEditor(bool commit);

    // ── Choosing files ────────────────────────────────────────────────────
    //
    // All of it through URLs, never through plain paths. The desktop file
    // advertises sftp, smb and webdav, and a program where only *one* of a
    // dozen choosers can reach a share is worse than one where none can: the
    // user learns that network places work here, and then finds them missing
    // in Insert, in Save As, in Compare With, with nothing to explain why.
    //
    // Reading fetches the file first; writing goes to a scratch copy and is
    // delivered afterwards, because QPDF and Poppler both want a file they can
    // seek in. See RemoteFile.

    /** Where a chooser should open: beside the document, not in the cwd. */
    QUrl chooserLocation() const;

    /** One file to read, fetched first if it is not on this disk. */
    QString chooseFileToRead(const QString &title, const QString &filter);

    /** Several files to read; empty unless every one of them arrived. */
    QStringList chooseFilesToRead(const QString &title, const QString &filter);

    /** Somewhere to write, which may not be on this disk. */
    struct Destination {
        QString path; //!< where to write now
        QUrl remote; //!< where it has to end up; empty when that is @c path
        bool chosen = false; //!< false when the user cancelled
    };

    Destination chooseDestination(const QString &title, const QString &suggestedName, const QString &filter);

    /** A directory to write into, likewise possibly remote. */
    Destination chooseDirectory(const QString &title);

    /**
     * Copies what was written to where it was destined for.
     *
     * @p files are paths below Destination::path; empty means the destination
     * is one file rather than a directory of them.
     */
    bool deliver(const Destination &destination, const QStringList &files = {});

    /**
     * Shows a problem without stealing the window.
     *
     * Errors appear as a bar above the document rather than as a modal box: the
     * user can still see what they were working on, the message does not have
     * to be dismissed before anything else can happen, and the window stays
     * drivable by the test suite. Questions such as "save before closing?" are
     * still modal, because those genuinely need an answer.
     */
    void reportError(const QString &title, const QString &detail);

    /** The same banner, in a colour that does not claim something went wrong. */
    void reportNotice(const QString &text);

    Document *m_document;
    PageModel *m_model;
    PageModel *m_stripModel = nullptr;
    PageGridView *m_view;
    RenderCache *m_cache;
    std::unique_ptr<PopplerBackend> m_backend;

    class PageProcessor *m_processor = nullptr;

    // ── What belongs to the document rather than to the window ────────────
    //
    // All of it put back by documentReset(), because a decision taken about one
    // document must never be carried into the next one behind the user's back:
    // the clean-up flag used to strip a later document's attachments with
    // nobody asked, and a password left behind would lock a file its owner
    // never protected.

    /** Set by the clean-up action; consulted when writing. */
    bool m_stripInteractivity = false;

    /**
     * The protection the document is under, carried into every save.
     *
     * Output is assembled from scratch and carries no encryption of its own, so
     * without keeping this a plain Ctrl+S would quietly unlock a file the user
     * had locked. Read back from the source that opened the file, or set by
     * Document ▸ Password….
     */
    QString m_userPassword;
    QString m_ownerPassword;
    Encryption::Permissions m_permissions;

    /** Where the document came from, when that was not this disk. */
    QUrl m_remoteOrigin;

    KRecentFilesAction *m_recentFiles = nullptr;

    /** Carried between the preview and the print dialog. */
    PrintController::Options m_printOptions;

    /** Remembered so a repeated insert workflow stays a single click. */
    InsertPositionDialog::Position m_lastInsertPosition = InsertPositionDialog::Position::AfterSelection;
    KMessageWidget *m_messageBar = nullptr;
    OutlineDock *m_outlineDock = nullptr;

    /** Grid at index 0, the document at index 1, an editing stage after that. */
    QStackedWidget *m_stack = nullptr;

    /** The document at reading size, where the actual editing happens. */
    PageView *m_pageView = nullptr;

    /** The layers over it, and the panel that shows what is selected in them. */
    AnnotationOverlay *m_annotationOverlay = nullptr;
    FormOverlay *m_formOverlay = nullptr;
    FormDesignOverlay *m_formDesign = nullptr;
    TextOverlay *m_textOverlay = nullptr;
    ObjectOverlay *m_objectOverlay = nullptr;
    InspectorDock *m_inspector = nullptr;

    /** Where every editing mode puts its tools. */
    QDockWidget *m_toolDock = nullptr;

    /** The mode currently up, or nothing. */
    EditorMode *m_editor = nullptr;

    /** Stops closing the tool panel during teardown from re-entering it. */
    bool m_leavingEditor = false;
    QLabel *m_pageCountLabel = nullptr;
    QLabel *m_selectionLabel = nullptr;
    QSlider *m_zoomSlider = nullptr;

    /** The fit actions by PageGridView::Fit, so the view can tick them. */
    QHash<int, QAction *> m_fitActions;

    /**
     * True once the user has said what size they want the page grid at.
     *
     * Until they have, arranging pages opens on an overview worked out from the
     * window, which is what somebody who came here to reorder a document wants
     * to see, and which no remembered number of pixels can stand in for,
     * because the window is not the size it was last time.
     */
    bool m_gridSizeChosen = false;

    /** True while the window itself is resizing the grid, so that is not read as a choice. */
    bool m_arrangingGrid = false;

    /** The drawing tools by AnnotationOverlay::Tool, so the layer can tick them. */
    QHash<int, QAction *> m_drawActions;

    /** The form-field tools by FormDesignOverlay::Kind, likewise. */
    QHash<int, QAction *> m_fieldActions;

    /** The four mode buttons, so anything that changes the mode can tick one. */
    QHash<int, QAction *> m_modeActions;

    /** The three dragging tools by PageView::Tool, likewise. */
    QHash<int, QAction *> m_toolActions;

    /** The panel of page thumbnails, so the menu can offer to show it. */
    QAction *m_thumbnailsAction = nullptr;

    /** True once this session has put the side panels back to their defaults. */
    bool m_panelsArranged = false;

    WorkMode m_mode = WorkMode::Read;

    /** Says which mode is in force, in words, in the status bar. */
    QLabel *m_modeLabel = nullptr;

    /** Which page of how many, and the box that jumps to one. */
    QSpinBox *m_pageBox = nullptr;
    QLabel *m_pageTotal = nullptr;

    ThumbnailDock *m_thumbnails = nullptr;
};

} // namespace ps
