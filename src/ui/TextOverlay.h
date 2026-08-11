/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#pragma once

#include "Inspector.h"
#include "PageRows.h"
#include "PageView.h"
#include "core/FontInventory.h"
#include "core/TextEdit.h"

#include <QBasicTimer>
#include <QFont>
#include <QFutureWatcher>
#include <QHash>
#include <QImage>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QTemporaryDir>
#include <QVector>

#include <memory>

class QLabel;
class QLineEdit;
class QToolButton;

namespace ps {

class Document;

/**
 * Typing on the page.
 *
 * Correcting a typo is the single commonest thing anyone wants from a PDF
 * editor, and the way it is usually offered (pick a line from a list, type the
 * new wording into a box somewhere else, press a button) asks the user to hold
 * the page in their head while they work on it. This puts the caret where the
 * letters are: click into a line, type, and the page says the new thing while
 * it is being typed.
 *
 * ## What a "run" is, and why it is visible
 *
 * A PDF has no paragraphs. A page is a list of instructions, and one of them
 * draws a run of glyphs at a coordinate; that run is the unit that can be
 * replaced, and it is often a single line or even half of one. Every editable
 * run is therefore outlined faintly before anything is clicked, so the shape of
 * what can be changed is visible rather than discovered by surprise.
 *
 * Runs set in a font that cannot be written into are outlined dashed and refuse
 * the click with the reason. That refusal, and the one for a character the font
 * does not hold, arrive while the user is typing rather than after they have
 * saved, which is the whole difference between a feature and a trap.
 *
 * ## The box grows; the letters do not shrink
 *
 * Adding a word to a line means the line gets longer. Narrowing the type to
 * keep the old width instead is what a layout program does when it has nowhere
 * to put the extra letters, and doing it by default makes every correction
 * quietly disfigure the page. So a run's box follows what is typed into it at
 * the size the page sets, and the exception, @ref Fitting::KeepBox for the
 * table cell or the boxed figure where the space really is fixed, is offered
 * per run, next to the caret.
 *
 * Growing costs something, and it costs it immediately: a PDF page has no
 * paragraphs, so nothing beside a run moves aside to make room for it. A run
 * that has grown over its neighbour, or out past where the rest of the page's
 * text stops, is shown doing so while it is being typed, because the
 * alternative is finding out in the saved file.
 *
 * ## What it does not do
 *
 * It does not write the file. Edits accumulate here as TextEdit::Replacement
 * values against the document's own page rows and are handed to whoever owns
 * saving and undo, because a text change has to be undoable next to every other
 * change rather than beside it.
 *
 * ## The rows move under it
 *
 * A row is a position in a list the organiser rearranges, not a page. A
 * correction held against row four belongs to a different sheet as soon as a
 * page in front of it is deleted, so every change to the page list is followed
 * rather than treated as a reason to throw the typing away; see PageRows.h. A
 * correction whose page has genuinely been deleted has nowhere to go, and that
 * is reported through @ref refused() instead of happening quietly.
 *
 * ## The clipboard, while a line is being typed into
 *
 * A caret in a line is a text field, and the clipboard has to behave like one:
 * copy takes the selected letters, not the object the letters belong to. So the
 * three clipboard commands are claimed for as long as the editor is up and
 * handed straight to it, and let go the moment it is not.
 *
 * ## The page's own type, not the desktop's
 *
 * Everything drawn here is drawn in the face the page itself is set in. Where
 * the document carries its glyphs, which is nearly always in anything
 * professionally produced, that very font programme is handed to Qt and used,
 * so the letters under the caret are the letters the page draws, at the size
 * and in the colour the page draws them. Where it does not, the closest face on
 * this system is chosen from what the document says about its own: the family
 * name, the weight, the slant, whether it is serif and whether it is fixed
 * pitch.
 *
 * The same font settles all three of the things that have to agree: what the
 * editor shows, what the preview paints, and which character a click lands in
 * front of. They were three different answers before, and a caret that lands
 * two letters from where it was clicked is what that feels like.
 *
 * ## What is shown is the file, once the typing stops
 *
 * A painted preview is a drawing of what the file will say, and however
 * faithful it is it remains a drawing. So a third of a second after the last
 * keystroke the edit is written to a scratch copy of that one page and the page
 * is rendered from it, and what is on the screen from then on *is* the file. It
 * costs about a tenth of a second on a dense magazine page, which is why it
 * waits for the typing to stop rather than following it.
 */
class TextOverlay : public QObject, public PageView::Overlay, public InspectionSource
{
    Q_OBJECT

public:
    /** What happens to a run's box when what is typed no longer fits inside it. */
    enum class Fitting {
        /** The box follows the text at the page's own type size. The default. */
        Grow,

        /** The box stays the size it is and the type is narrowed into it. */
        KeepBox,
    };

    /** Neither @p view nor @p document is owned; both must outlive this. */
    explicit TextOverlay(PageView *view, Document *document, QObject *parent = nullptr);
    ~TextOverlay() override;

    // ── PageView::Overlay ─────────────────────────────────────────────────

    bool appliesTo(PageView::Mode mode) const override;
    void paint(QPainter &painter, int row, const QRect &pageRect) override;
    bool press(int row, const QPointF &pagePoint, Qt::MouseButtons buttons) override;
    Qt::CursorShape cursor(int row, const QPointF &pagePoint) override;
    bool copy() override;
    bool cut() override;
    bool paste(const QMimeData *data, int row) override;

    // ── InspectionSource ──────────────────────────────────────────────────

    Inspectable *inspected() override;

    /**
     * Everything typed so far, addressed by the document's own page rows.
     *
     * Both behaviours in one list, which suits a caller that means to write them
     * all the same way. Anything that means to save what the page actually
     * showed wants the replacements(Fitting) pair instead: applied with the
     * default TextEdit::Options, every run in here is narrowed to the width of
     * the text it replaces, including the ones the page showed growing.
     */
    QVector<TextEdit::Replacement> replacements() const;

    /**
     * Only what was typed into runs whose box behaves as @p fitting says.
     *
     * TextEdit::apply() takes one TextEdit::Options for a whole batch, and the
     * two behaviours are opposite settings of TextEdit::Options::fitWidth, so
     * they cannot travel together: whoever saves has to apply each of these
     * with the matching optionsFor(). Handing the lot to a single call writes
     * one of the two groups the way the page never showed it.
     */
    QVector<TextEdit::Replacement> replacements(Fitting fitting) const;

    /** What the engine has to be told, so that the file says what the page showed. */
    static TextEdit::Options optionsFor(Fitting fitting);

    bool hasEdits() const;

    /** The page whose text is being typed into, or -1 when none is. */
    int editedRow() const { return m_editRow; }

    /**
     * True when @p row is showing a render of the file rather than a drawing of it.
     *
     * The distinction the whole settle-and-render arrangement exists for, so it
     * is a fact the class states rather than one a caller has to infer.
     */
    bool showsTheFile(int row) const { return row >= 0 && row == m_liveRow && !m_live.isNull(); }

    /**
     * The font a run is set in, as this machine can draw it.
     *
     * Public because it is the answer three separate things need (the editor,
     * the preview and the caret arithmetic) and because a test that means to
     * check the caret has to be able to ask the same question the caret asks.
     */
    QFont fontOf(int row, int index);

    /**
     * Which character a click at @p pageX falls in front of, in the run's own font.
     *
     * Measured from the run's left edge in the page's own points rather than
     * from the editor widget's corner: where the widget sits and how its style
     * insets its text are not things the answer should depend on.
     */
    int caretAt(int row, int index, double pageX);

    /** How a run is to be set, where that differs from how the page sets it. */
    TextEdit::Format formatOf(int row, int index) const;

    /** The run as the page itself has it, with nothing the user asked for on top. */
    TextEdit::Run pageRunOf(int row, int index);

    /**
     * Sets a run in a different face, size, weight or colour.
     *
     * The whole run, because a PDF's unit of type is the run: one text-showing
     * operation carries one font at one size, and a page cannot express half a
     * run in something else without being cut into two operations. Selecting
     * part of a line and restyling only that is therefore refused rather than
     * quietly applied to the lot.
     *
     * A face whose glyphs cannot be found on this system is refused by name
     * through @ref refused(), because the alternative, a page of empty boxes
     * where the letters were, is discovered far too late.
     */
    void setFormat(int row, int index, const TextEdit::Format &format);

public Q_SLOTS:
    /** Leaves the line being edited, keeping what was typed into it. */
    void finishEditing();

    /** Leaves the line and throws every typed change on every page away. */
    void discardEdits();

    /** Reads the pages again, after the document behind them has been written. */
    void reload();

    /**
     * Carries what was typed across a change to the page list.
     *
     * A correction names a line by where it falls in one page's list of
     * instructions and names the page by its row. The first half survives
     * anything the organiser does; the second is followed here, so that only a
     * page actually deleted takes its corrections with it.
     */
    void rebaseRows();

    /** The same for a row that kept its number and changed underneath. */
    void refreshRows();

Q_SIGNALS:
    /** Text was typed, taken back or thrown away. */
    void editsChanged();

    /** The caret moved to a different line, so the properties panel is stale. */
    void inspectionChanged();

    /**
     * What was lost and why, in words a person can act on.
     *
     * Same shape as PageView::refused() and ObjectOverlay::refused(), so that
     * all of them can go to the same place in the window.
     */
    void refused(const QString &reason);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void timerEvent(QTimerEvent *event) override;

private:
    /** What a run that has outgrown its box now lies over. */
    struct Encroachment {
        /** The part of the page it covers that was never its own, in points. */
        QRectF over;

        /** What it ran into, in words, or empty when it ran into nothing. */
        QString message;
    };

    /** The runs on @p row, read from the file once and kept. */
    const QVector<TextEdit::Run> &runsOf(int row);

    /**
     * The run as it is to be set: the page's own type with what was asked for over it.
     *
     * One place where the page's answer and the user's are put together, so that
     * the editor, the preview and the caret cannot disagree about which face a
     * line is in.
     */
    TextEdit::Run styledRun(int row, int index);

    /** One body for both re-basing slots: carries the typing through @p change. */
    void carryRows(const Rebase &change);

    /** Which characters the page's @p fontResource can be asked for. */
    QSet<QChar> coverageFor(int row, const QString &fontResource);

    /** The topmost run under @p pagePoint, or -1. */
    int runAt(int row, const QPointF &pagePoint);

    void beginEdit(int row, int index, const QPointF &pagePoint);
    void endEdit(bool keepTyping);

    /** Moves the caret to the next editable run on the page; -1 goes back. */
    void stepToNeighbour(int direction);

    void recordTyped(const QString &text);

    /** How the box of the run at @p index on @p row behaves as it is typed into. */
    Fitting fittingOf(int row, int index) const;

    /** Changes that, for the run the caret is in. */
    void setFitting(Fitting fitting);

    /** Where @p text really ends up, in points, which is past the box when it grew. */
    QRectF reachOf(int row, int index, const QString &text);

    /** What @p reach has taken from the rest of the page, and how to say it. */
    Encroachment encroachmentOf(int row, int index, const QRectF &reach);

    /** The characters this run's font cannot be asked for, said out loud, or empty. */
    QString characterRefusal(const TextEdit::Run &run, const QString &text);

    /** Puts the one thing worth knowing about what is being typed under the caret. */
    void updateAdvice();

    void paintEncroachment(QPainter &painter, int row, int index, const QString &text);

    void ensureWidgets();
    void placeEditor();
    void updateBoxButton();
    void showNote(const QString &text, const QRect &anchor);
    void hideNote();

    /**
     * The page's own face for @p run at the zoom the view is at.
     *
     * The document's embedded glyphs where it carries them, and the closest
     * thing on this system chosen from what the document says about the face
     * where it does not. Size, horizontal scaling and letter spacing come from
     * the page too, so that a line measured here is as wide as the line on the
     * paper.
     */
    QFont fontFor(int row, const TextEdit::Run &run);

    /**
     * That font set the way the engine will set it for @p text.
     *
     * A growing box leaves it alone; a kept box narrows it into @p available,
     * measured in the same pixels QFontMetricsF answers in, no further than the
     * engine will narrow it.
     */
    QFont typeFor(int row, const TextEdit::Run &run, const QString &text, double available, Fitting fitting);

    /** The size the letters came out on the page, in points, however it was set. */
    static double sizeOf(const TextEdit::Run &run);

    /** Where the letters sit, in the page's own points. */
    static double baselineOf(const TextEdit::Run &run);

    /**
     * Registers the embedded faces of @p row's page with Qt, once per page.
     *
     * Two subsets of one family carry the same family name and Qt has no way to
     * tell them apart, so the first one seen keeps the name and the second is
     * drawn with it. They are subsets of the same typeface, so the shapes agree;
     * what the second may be missing is a character the first never used, and Qt
     * fills that from elsewhere rather than drawing a box.
     */
    void readFaces(int row);

    /** The Qt family for @p run's embedded glyphs, or empty when there are none. */
    QString faceFor(int row, const TextEdit::Run &run);

    /**
     * How the page's paper looks where @p run sits, so the editor can wear it.
     *
     * Sampled from the page itself, because half a magazine's text sits on a
     * tinted panel and covering one of those with white is precisely the "an
     * input box appeared" this exists to stop. Only ever asked when the caret
     * arrives: it grabs the viewport, and grabbing a widget paints it.
     */
    QColor paperUnder(int row, const TextEdit::Run &run) const;

    // ── What is shown is the file ─────────────────────────────────────────

    /** Starts the clock that ends in a render of the page as edited. */
    void scheduleLiveRender();

    /** Writes the edited page to a scratch file and has it drawn off this thread. */
    void startLiveRender();

    void liveRenderArrived();

    /** Throws away a render that no longer stands for what is on the screen. */
    void dropLiveRender();

    /**
     * Draws the settled render over @p index's box. True when it could.
     *
     * @p reach is in the viewport's own pixels, as everything the paint sees is.
     */
    bool paintLive(QPainter &painter, int row, int index, const QRectF &reach);

    QPointer<PageView> m_view;
    Document *m_document = nullptr;

    /** Which page each row shows, so that a change to the list can be followed. */
    PageRows m_rows;

    QHash<int, QVector<TextEdit::Run>> m_runs;

    /** Fonts of one source file, keyed by its path, for the character check. */
    QHash<QString, QVector<FontUse>> m_fonts;

    /** Page row to run position to what it should say. */
    QHash<int, QHash<int, QString>> m_edits;

    /**
     * The runs somebody has asked to be set differently, by page row.
     *
     * Apart from the words, because the two are asked for separately and a line
     * may want either without the other: a heading that only has to be bigger is
     * not a correction, and a corrected line usually keeps its type.
     */
    QHash<int, QHash<int, TextEdit::Format>> m_formats;

    /**
     * The runs whose box is to be kept, by page row.
     *
     * Absence means Fitting::Grow, so the common case costs nothing to store
     * and a run nobody has said anything about behaves the way people expect.
     */
    QHash<int, QSet<int>> m_keptBoxes;

    /**
     * The Qt family each page font was registered under, by page and `/Fx`.
     *
     * Empty for a font whose glyphs the document does not carry, or that Qt
     * cannot read; both mean the same thing to whoever asked, which is that a
     * substitute has to be chosen.
     */
    QHash<QString, QString> m_faces;

    /** Pages whose fonts have been through @ref readFaces, so it happens once. */
    QSet<QString> m_facesRead;

    /** Families already registered, so two subsets do not fight over one name. */
    QSet<QString> m_claimed;

    int m_editRow = -1;
    int m_editRun = -1;

    /** Set while the editor is being taken down, so losing focus does not recurse. */
    bool m_leaving = false;

    /** Whether the line being typed has grown over something, as of the last keystroke. */
    bool m_crowded = false;

    QPointer<QLineEdit> m_editor;
    QPointer<QLabel> m_note;

    /** The padlock beside the caret that says whether the box may grow. */
    QPointer<QToolButton> m_boxButton;

    std::unique_ptr<Inspectable> m_inspected;

    // ── The page as edited, drawn rather than painted ─────────────────────

    /** Where the one-page copies the settled render is made from are written. */
    QTemporaryDir m_scratch;

    /** Runs after the last keystroke, so a render answers typing rather than keys. */
    QBasicTimer m_settle;

    /** Which row that render is for, which outlives the caret being in it. */
    int m_settleRow = -1;

    /**
     * The edited page as the rasteriser draws it, and what it is a picture of.
     *
     * Kept rather than painted over: while it holds, what the user is looking at
     * is the file and not a drawing of the file, which is the only way the
     * question "will it really look like this" has a truthful answer.
     */
    QImage m_live;
    int m_liveRow = -1;
    double m_liveZoom = 0.0;
    QSize m_livePage;

    /** Counts the requests, so that each one writes to a scratch file of its own. */
    quint64 m_liveWanted = 0;

    QFutureWatcher<QImage> *m_liveWatcher = nullptr;

    /** The paper colour under the line being typed, sampled from the page. */
    QColor m_paper;

    /** The same for every line typed into so far, since the preview outlives the caret. */
    QHash<int, QHash<int, QColor>> m_papers;
};

} // namespace ps
