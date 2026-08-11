/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#pragma once

#include "Alignment.h"
#include "Inspector.h"
#include "PageRows.h"
#include "PageView.h"
#include "core/FormBuilder.h"
#include "core/Forms.h"

#include <QBasicTimer>
#include <QFutureWatcher>
#include <QHash>
#include <QImage>
#include <QObject>
#include <QPointF>
#include <QPointer>
#include <QRectF>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QTransform>
#include <QVector>

#include <functional>
#include <memory>

namespace ps {

/**
 * Making the form, on the page, where the form is.
 *
 * FormOverlay is the other half of this: it fills in someone else's form in
 * View mode. This one authors the form in Edit mode: pick a kind of field from
 * a toolbar, drag out the box it is to occupy, then move it, resize it by its
 * corners and set its properties in the panel beside the document. A form is a
 * layout before it is a data structure, and a layout is designed by looking at
 * it; a dialog full of coordinate spin boxes asks the author to hold the page
 * in their head while typing numbers about it.
 *
 * ## Nothing is written here
 *
 * Every gesture is collected as pending work and handed to the window through
 * @ref pendingWork(), which owns saving and undo. The engine rewrites the whole
 * document for each of its calls, so the work is collected into four buckets
 * rather than a stream of individual operations, and the window applies them in
 * this order (each step names fields by the name the file has when it runs):
 *
 *  1. `FormBuilder::removeFields()` with @ref Work::removed. Those are the
 *     names the file has now, because nothing has renamed anything yet.
 *  2. `FormBuilder::renameField()` once per @ref Work::renamed entry, in the
 *     order they come, `from` then `to`.
 *  3. `FormBuilder::updateFields()` with @ref Work::changed. Each entry is a
 *     complete description, as that call demands, and carries the name the
 *     field has *after* step 2.
 *  4. `FormBuilder::addFields()` with @ref Work::added. Last, so that a name
 *     freed by a removal in step 1 is available to a new field.
 *
 * Each of those reads one file and writes another, so the window chains them
 * through temporaries and only the last output becomes the document.
 *
 * ## What the engine will not do, said at the gesture
 *
 * A document carrying an XFA form is refused outright: `FormBuilder` will not
 * add to one, because Acrobat honours the XFA and everything else honours the
 * fields, so the result behaves differently in every reader. That is checked
 * once per document and reported through @ref refused() the moment a field is
 * armed or drawn, not at saving time when the work is already done.
 *
 * The same applies to a field with more than one widget (a radio group is
 * always one), which `FormBuilder::updateFields()` deliberately leaves where
 * its author put it. Dragging one is refused while the mouse is still on it.
 *
 * ## Where it goes in the order
 *
 * Before ObjectOverlay: a form field lies over the text and the rules that
 * drew it, and in Edit mode on a form the field is the thing being reached
 * for. A drag on bare paper bands a selection here only when the page actually
 * carries fields, so a document with no form never takes a band away from the
 * layer underneath.
 *
 * ## Pages in the file, rows in the view
 *
 * The engine is handed pages by their **row**, because the window writes the
 * document out row by row before calling it. `Forms::read()`, on the other hand,
 * numbers an existing field by its page **in the file** it was read from. Those
 * two numbers agree only until a page is deleted or moved, so an existing field
 * is anchored to a ps::SourcePage when the form arrives and a field drawn here
 * is anchored to one the moment it is drawn; see PageRows.h. A field whose page
 * is deleted outright cannot be re-anchored, and what was waiting on it is
 * dropped and said out loud through @ref refused() rather than written onto
 * whichever page has moved into that slot.
 *
 * ## Turned pages
 *
 * Everything from @ref specOf() onwards (what is drawn, what a click is matched
 * against, what the panel's numbers say and what @ref pendingWork() hands over)
 * is in the space the view draws the row in, so that a gesture made on a turned
 * sheet means on the page what it looked like it meant on screen. The document's
 * own fields arrive measured on the page before the organiser turned it and are
 * put through @ref pageTurn() on the way in; a field drawn here is already in
 * that space, and one waiting when the sheet is turned is turned with it by
 * @ref carryRows(). Nothing is turned on the way out, because the window writes
 * the document to a file before the engine sees it and that file carries the
 * turn in its `/Rotate`, which is exactly the space these rectangles are in.
 *
 * ## Only one of each field, and it is the one being dragged
 *
 * A field's appearance is painted into the page bitmap. Drag one and there used
 * to be two: the one under the pointer, drawn here, and the original, still
 * standing where the file has it and knowing nothing about the gesture. Covering
 * the original with a patch of paper is what the text layer used to do and it
 * never quite matched.
 *
 * So the page underneath is asked for without its fields (see
 * PageView::setOmittedFromRenders(), which takes off the widgets and leaves
 * every comment where it is), and this layer draws all of them itself, from the
 * very description the engine will be handed. Nothing can be left behind,
 * because nothing was ever there.
 *
 * ## And then it is the file, not a drawing of the file
 *
 * What is drawn here is a reconstruction from `/MK`, `/BS`, `/DA` and `/Q`, and
 * it is exact for a field this program made, because `FormBuilder` draws its
 * appearance from those same four things. It cannot be exact for a field whose
 * author drew their own appearance stream (a logo on a button, a bevel with a
 * gradient in it), because an appearance stream is a program and this is not
 * running it.
 *
 * That is why the gesture is not the end of it. A third of a second after the
 * last change, the page and the work waiting on it are written to a one-page
 * scratch copy, the copy is rendered off the GUI thread *with* its fields, and
 * the result is put back over the boxes. From then on what is on the screen is
 * the file. The same arrangement TextOverlay uses for typing, for the same
 * reason and at the same price.
 */
class FormDesignOverlay : public QObject, public PageView::Overlay, public InspectionSource
{
    Q_OBJECT
    Q_PROPERTY(Kind kindToPlace READ kindToPlace WRITE setKindToPlace NOTIFY kindToPlaceChanged)

public:
    /**
     * What the next drag on bare paper makes.
     *
     * None is a kind of its own rather than a separate "armed" flag, because a
     * toolbar's field buttons and its pointer button are one exclusive group
     * and this is the one thing they all set.
     */
    enum class Kind {
        None, //!< dragging bands a selection instead
        Text,
        Checkbox,
        Radio, //!< one button of a group; consecutive drags join the same group
        Dropdown,
        ListBox,
        PushButton,
        Signature,
    };
    Q_ENUM(Kind)

    /** One field to be renamed, in the order `FormBuilder::renameField()` takes. */
    struct Rename {
        QString from;
        QString to;
    };

    /** Everything waiting, in the four shapes the engine's calls take. */
    struct Work {
        /** For `FormBuilder::addFields()`: fields the document does not have yet. */
        QVector<FormBuilder::Field> added;

        /** For `FormBuilder::renameField()`, one call each, in this order. */
        QVector<Rename> renamed;

        /** For `FormBuilder::updateFields()`: complete descriptions, renamed names. */
        QVector<FormBuilder::Field> changed;

        /** For `FormBuilder::removeFields()`: the names the file has now. */
        QStringList removed;

        bool isEmpty() const { return added.isEmpty() && renamed.isEmpty() && changed.isEmpty() && removed.isEmpty(); }
    };

    /** Adds itself to @p view and takes itself off again when it dies. */
    explicit FormDesignOverlay(PageView *view, QObject *parent = nullptr);
    ~FormDesignOverlay() override;

    /**
     * The document the form is designed on. Not owned; must outlive this.
     *
     * What turns a page number in the form's own file into the row the view
     * draws it at, and back again for the engine. Give it before setFields().
     */
    void setDocument(Document *document);

    /**
     * The file the fields are read from and the work is addressed to.
     *
     * Only used to ask whether the document carries an XFA form, which decides
     * whether anything can be authored at all. A new file drops everything that
     * was waiting: the work names fields in one particular document.
     */
    void setSource(const QString &pdfPath);

    QString source() const { return m_source; }

    /** The document's fields, as `Forms::read()` gave them. Drops pending work. */
    void setFields(const QVector<FormField> &fields);

    const QVector<FormField> &fields() const { return m_fields; }

    Kind kindToPlace() const { return m_kind; }

    /** The name of a kind, for a toolbar's buttons and the panel's list. */
    static QString describe(Kind kind);

    // ── PageView::Overlay ─────────────────────────────────────────────────
    bool appliesTo(PageView::Mode mode) const override;
    void paint(QPainter &painter, int row, const QRect &pageRect) override;
    bool press(int row, const QPointF &pagePoint, Qt::MouseButtons buttons) override;
    bool move(int row, const QPointF &pagePoint, Qt::MouseButtons buttons) override;
    bool release(int row, const QPointF &pagePoint) override;
    Qt::CursorShape cursor(int row, const QPointF &pagePoint) override;

    // ── InspectionSource ──────────────────────────────────────────────────
    Inspectable *inspected() override;

    /** Everything waiting, in the shape the window applies. */
    Work pendingWork() const;

    /**
     * Only the work that falls on @p row, addressed to a file holding that page alone.
     *
     * What the settled render is made from, and public because a test that means
     * to compare the screen against the file has to be able to build the same
     * file the screen is claiming to show. Every `page` in the answer is zero:
     * the one-page copy it is meant for has no other.
     */
    Work workOn(int row) const;

    bool hasPendingWork() const;

    /** The page the choice lives on, or -1. A choice never spans two pages. */
    int chosenPage() const { return m_page; }

    int chosenCount() const { return int(m_chosen.size()); }

    /**
     * True when @p row's fields are a render of the file rather than a drawing.
     *
     * The distinction the whole settle-and-render arrangement exists for, so it
     * is a fact the class states rather than one a caller has to infer.
     */
    bool showsTheFile(int row) const { return row >= 0 && row == m_liveRow && !m_live.isNull(); }

public Q_SLOTS:
    /**
     * Arms the next drag, or disarms it with Kind::None.
     *
     * Stays armed after a field is placed, because a form is laid out several
     * boxes at a time; the toolbar's own pointer button is what puts it back.
     */
    void setKindToPlace(Kind kind);

    /**
     * Ends the radio group the next button would have joined.
     *
     * Consecutive radio drags belong together: that is what "yes, no, maybe"
     * is, so a second group has to be asked for. Changing the armed kind away
     * from Radio and back again does the same thing.
     */
    void startNewRadioGroup();

    void clearChoice();

    /** Takes the chosen fields off the form, as waiting work. */
    void removeChosen();

    /** Drops every waiting change and every field placed but not yet written. */
    void takeBackAll();

    /** Moves what is chosen, in points; up is positive, as on the page. */
    void nudge(double acrossPoints, double upPoints);

    /**
     * Puts every chosen field on the same @p edge of the box round them all.
     *
     * The same command the panel's buttons give, so a menu entry or a shortcut
     * reaches it without the properties panel having to be open. Fields the
     * engine cannot move are left alone and named through @ref refused().
     */
    void alignChosen(Alignment::Edge edge);

    /** Leaves equal gaps between the chosen fields along @p spread. */
    void distributeChosen(Alignment::Spread spread);

    /** Gives every chosen field the size of the largest of them. */
    void matchChosenSize(Alignment::Match what);

Q_SIGNALS:
    /** The toolbar's buttons follow this, including when a refusal disarms one. */
    void kindToPlaceChanged(Kind kind);

    /** Something is waiting that was not waiting before, or is no longer. */
    void editsChanged();

    /** A different thing is chosen: the properties panel should be rebuilt. */
    void choiceChanged();

    /** What could not be done, in words the user can act on, as it is asked. */
    void refused(const QString &reason);

protected:
    /** The Overlay interface has no key hook; see the definition for why this does. */
    bool eventFilter(QObject *watched, QEvent *event) override;

    void timerEvent(QTimerEvent *event) override;

private:
    /** The attributes of what is chosen, for the properties panel. */
    class Details;

    /** Which of the eight marks on the chosen box is being held. */
    enum class Grip { None, TopLeft, Top, TopRight, Right, BottomRight, Bottom, BottomLeft, Left };

    /** What the mouse is doing between a press and its release. */
    enum class Drag { None, Band, Move, Resize, Create };

    /**
     * A field placed but not yet written, with a number of its own.
     *
     * Numbered rather than addressed by position, so that deleting one of five
     * does not silently move the choice onto its neighbour.
     */
    struct Draft {
        int id = 0;

        /**
         * @ref FormBuilder::Field::page is the **row** it was drawn at.
         *
         * That is what the engine is given, because the window writes the
         * document out row by row before calling it. It is also exactly what
         * stops being true the moment the organiser deletes a sheet in front of
         * it, which is why @ref rebaseRows() moves it.
         */
        FormBuilder::Field spec;
    };

    /** Whether the document refuses to be authored, worked out once and kept. */
    enum class Xfa { Unknown, Absent, Present };

    /**
     * One number for both kinds of field: the document's and the drafts'.
     *
     * A document field is its index plus one and a draft is minus its number,
     * which leaves zero meaning "none", so the choice, the drag and the
     * alignment arithmetic can all carry "nothing" without a second flag.
     */
    static bool isDraft(int handle) { return handle < 0; }

    static int handleOfField(int index) { return index + 1; }

    static int fieldIndex(int handle) { return handle - 1; }

    /** Every field drawn on @p row, back to front: the document's, then drafts. */
    QVector<int> handlesOn(int row) const;

    /**
     * One box of a field that is not the box the field is dragged by.
     *
     * A field and a box are the same thing only for a text field. A group of
     * radio buttons is one field with one widget per choice, and a reference
     * number repeated at the head of every sheet is one field with a widget per
     * page. Those extra boxes are drawn on the page like any other, so with the
     * page asked for without its fields they have to be drawn here or they
     * simply vanish, which is how a three-button group came to show one button.
     *
     * They are not dragged and not chosen: the engine leaves a field with
     * several widgets exactly where its author put it, so there is nothing here
     * to offer a handle for.
     */
    struct Extra {
        int field = -1; //!< index into m_fields
        int widget = 0; //!< index into that field's widgets
    };

    /** The extra boxes on @p row, in the order the document lists them. */
    QVector<Extra> extrasOn(int row) const;

    /** The row @p handle is drawn on now, or -1 when its page has gone. */
    int rowOf(int handle) const;

    /**
     * One body for @ref pendingWork() and @ref workOn().
     *
     * @p onlyRow of -1 collects the lot and keeps each field's row number, which
     * is what the window hands the engine after writing the document out row by
     * row. Any other value keeps that one row and renumbers its pages to zero,
     * for the one-page copy the settled render is made from.
     */
    Work collect(int onlyRow) const;

    /** Which row shows which of the form file's pages, and what sits on it. */
    void rebuildRows();

    /**
     * Follows the rows when the organiser moves them, and says what is lost.
     *
     * A field whose page merely moved goes with it; one whose page was deleted
     * has nowhere left to be written, so the work waiting on it is dropped and
     * named through @ref refused() rather than put on another page.
     */
    void rebaseRows();

    /** The same for a row that kept its number and changed underneath. */
    void refreshRows();

    /** One body for both. */
    void carryRows(const Rebase &change);

    /**
     * The document's field @p index, in the space the view draws its row in.
     *
     * `Forms::read()` measured it on the page as its file has it. The one place
     * that becomes the space everything else here works in, so that the turn is
     * asked about once rather than at every gesture.
     */
    FormBuilder::Field documentSpec(int index) const;

    /** What to put field @p index's box through to draw it where its row is now. */
    QTransform turnOf(int index) const;

    /** The same for a box measured on @p page of the form's file, drawn at @p row. */
    QTransform turnAt(int row, int page) const;

    /** @p handle as it now stands, with everything waiting already applied. */
    FormBuilder::Field specOf(int handle) const;

    /** The same, to be written into; makes the waiting change if there is none. */
    FormBuilder::Field *editableSpec(int handle);

    QRectF rectOf(int handle) const;
    QString nameOf(int handle) const;

    /** True while a waiting removal is on it: still drawn, crossed through. */
    bool isRemoved(int handle) const;

    /**
     * Whether the engine can put this field's box somewhere else.
     *
     * A field with several widgets, which every radio group has, is left
     * where its author put it, so its box is not the overlay's to drag.
     */
    bool isMovable(int handle) const;

    /** The chosen handles the engine can actually move, without saying anything. */
    QVector<int> movableChosen() const;

    /** The boxes alignment works on: the movable chosen ones, in handle order. */
    QVector<QRectF> alignableBoxes() const;

    /** Puts the movable chosen fields at @p boxes, in the order they went out. */
    void placeChosen(const QVector<QRectF> &boxes);

    QVector<int> chosenHandles() const;

    /** The box round everything chosen, in points, without the drag in progress. */
    QRectF choiceBounds() const;

    Grip gripAt(int row, const QPointF &pagePoint) const;

    /** Where a grip is drawn, in viewport pixels, given the chosen box in pixels. */
    static QPointF gripCentre(const QRectF &box, Grip grip);
    static Qt::CursorShape cursorForGrip(Grip grip);

    /** What the drag in progress does to a box, in page space. */
    QTransform liveTransform() const;

    /** The field under @p pagePoint on @p row, topmost first, or zero. */
    int fieldAt(int row, const QPointF &pagePoint) const;

    /** Puts a new field of the armed kind at @p rect. Says why when it cannot. */
    void createField(int row, const QRectF &rect);

    /** A name no field and no draft is using, from @p stem. */
    QString uniqueName(const QString &stem) const;

    /**
     * True when the document will take no new fields, having said so.
     *
     * Asked at the gesture rather than at saving time, because a refusal that
     * arrives after an afternoon's work is a refusal about work already done.
     */
    bool refusesAuthoring();

    /** Changes every chosen field, then tells the window and the panel. */
    void changeChosen(const std::function<void(FormBuilder::Field &)> &change);

    /** Renames the one chosen field, refusing what the engine would refuse. */
    void renameChosen(const QString &name);

    void setRectOf(int handle, const QRectF &rect);

    /** Sets one field's box and says so, which is what the panel's numbers do. */
    void moveOne(int handle, const QRectF &rect);

    void choose(int row, const QVector<int> &handles, bool add);
    void setHover(int row, int handle);
    void repaint();

    // ── The fields, drawn as the engine will write them ───────────────────

    /**
     * Asks the view for pages without their fields, or stops asking.
     *
     * Only while this layer is up and the document has a form: a page with no
     * widgets on it has nothing to leave off, and asking anyway would throw
     * every render away for nothing.
     */
    void updateOmission();

    /**
     * Draws @p spec's own appearance at @p box, in the viewport's pixels.
     *
     * Background, border, mark and text, laid out the way FormBuilder lays out
     * the `/AP` stream it writes (the same padding, the same fitted size, the
     * same baseline), so that what is on the screen during a gesture and what
     * the file says afterwards are the same picture rather than two guesses.
     */
    void paintFace(QPainter &painter, const FormBuilder::Field &spec, const QRectF &box, bool marked) const;

    /** Whether a tick box or a radio button of @p spec is showing its mark. */
    static bool isMarked(const FormBuilder::Field &spec, const QString &onState);

    // ── What is shown is the file ─────────────────────────────────────────

    /** Starts the clock that ends in a render of the page as it will be written. */
    void scheduleLiveRender(int row);

    /** Writes the page and the work waiting on it to a scratch file, and draws it. */
    void startLiveRender();

    void liveRenderArrived();

    /** Throws away a render that no longer stands for what is on the screen. */
    void dropLiveRender();

    /**
     * Draws the settled render over @p box. True when it could.
     *
     * @p box is in the viewport's own pixels, as everything the paint sees is.
     */
    bool paintLive(QPainter &painter, int row, const QRectF &box);

    /** Keeps the panel's numbers honest while a drag moves what they describe. */
    void refreshDetails();

    QPointer<PageView> m_view;

    /** Not owned. What the one-page copy the settled render is made from is cut from. */
    Document *m_document = nullptr;

    QString m_source;
    Xfa m_xfa = Xfa::Unknown;

    /** Rows to pages in files and back; the only place the two are converted. */
    PageRows m_rows;

    /** The document's own fields, as they were read. Never edited in place. */
    QVector<FormField> m_fields;

    /**
     * Which row shows each page of the file the form was read out of.
     *
     * Keyed by the page number `Forms::read()` gave, and -1 for a page the
     * document no longer holds.
     */
    QHash<int, int> m_rowOfPage;

    /**
     * How far each of the form's pages was already turned when it was read.
     *
     * The baseline the boxes were measured against: saving under another name
     * reads the form again out of a file that has the organiser's turn written
     * into its `/Rotate`, while the page list goes on carrying that same turn.
     * What has to be applied is therefore the difference, not PageRows::rotationOf()
     * its own, or every box would be turned twice.
     */
    QHash<int, int> m_turnAtRead;

    /** Field indices by view row, so painting a page does not walk the whole form. */
    QHash<int, QVector<int>> m_byRow;

    /** The same for the boxes of a field that are not the one it is dragged by. */
    QHash<int, QVector<Extra>> m_extrasByRow;

    /** One complete description per field the user has touched, keyed by index. */
    QHash<int, FormBuilder::Field> m_changed;

    /** Indices of fields waiting to be taken off the form. */
    QSet<int> m_removed;

    /** Fields placed and not yet written, in the order they were drawn. */
    QVector<Draft> m_drafts;
    int m_nextDraft = 1;

    Kind m_kind = Kind::None;

    /** The group the next radio button joins; empty means start a new one. */
    QString m_radioGroup;

    int m_page = -1;
    QSet<int> m_chosen;

    /** The one the panel describes: the last one pointed at, or zero. */
    int m_current = 0;

    int m_hoverPage = -1;
    int m_hover = 0;

    Drag m_drag = Drag::None;
    Grip m_grip = Grip::None;
    int m_dragRow = -1;
    QPointF m_from;
    QPointF m_to;
    QRectF m_startBox;

    std::unique_ptr<Details> m_details;

    // ── The page as it will be written, drawn rather than painted ─────────

    /** Where the one-page copies the settled render is made from are written. */
    QTemporaryDir m_scratch;

    /** Runs after the last change, so a render answers the gesture, not the mouse. */
    QBasicTimer m_settle;

    /** Which row that render is for, which outlives the choice being on it. */
    int m_settleRow = -1;

    /**
     * The page as the rasteriser draws it once the work is applied.
     *
     * Kept rather than painted over: while it holds, what the user is looking at
     * is the file and not a drawing of the file, which is the only way the
     * question "will it really look like this" has a truthful answer.
     */
    QImage m_live;
    int m_liveRow = -1;
    double m_liveZoom = 0.0;
    QSize m_livePage;

    /**
     * True when that render was made from the work exactly as it stands now.
     *
     * The difference between a picture of the page and a picture of the page a
     * gesture ago. A stale one is still the truth about every field nobody is
     * working on, which is why it is left up rather than taken down, so that
     * nudging one box does not make the whole sheet flicker. But the boxes that
     * *are* being worked on have to be drawn from their own description until it
     * catches up. Once it has, it is the truth about all of them.
     */
    bool m_liveCurrent = false;

    /** Counts the requests, so that each one writes to scratch files of its own. */
    quint64 m_liveWanted = 0;

    QFutureWatcher<QImage> *m_liveWatcher = nullptr;
};

} // namespace ps
