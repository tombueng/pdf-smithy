/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#pragma once

#include "Alignment.h"
#include "Inspector.h"
#include "PageClipboard.h"
#include "PageRows.h"
#include "PageView.h"
#include "core/PageObjects.h"

#include <QBasicTimer>
#include <QFutureWatcher>
#include <QHash>
#include <QObject>
#include <QPointF>
#include <QPointer>
#include <QRectF>
#include <QSet>
#include <QString>
#include <QTemporaryDir>
#include <QVector>

#include <functional>
#include <memory>

namespace ps {

/**
 * What the page draws, with handles on it.
 *
 * A PDF page is a list of drawing instructions rather than a list of objects;
 * PageObjects invents the handle, and this is where that handle is taken hold
 * of. In Edit mode every painting operation gets a boundary that lights up
 * under the pointer, and whatever is chosen gets eight corners and a turn
 * handle, so a picture is moved by dragging the picture, which is the one
 * thing every other program on the machine has taught the user to try first.
 *
 * ## Nothing is written here
 *
 * Changes are collected as one PageEdit per (page, object) and handed to the
 * window, which owns saving and undo. One edit per object is not tidiness: the
 * page is rewritten in a single pass and every object number counts painting
 * operations from the start of the page, so a second pass would be addressed to
 * a page the first one had already renumbered. The same rule means a colour
 * takes the place of a move that was waiting on the same object, which is said
 * out loud when it happens rather than discovered in the file afterwards.
 *
 * ## A copy is built again, not duplicated
 *
 * The composer wraps operators that are already on the page; it has no way to
 * draw one of them a second time somewhere else. So copying an object means
 * describing it well enough to write it from nothing (the words of a line, the
 * pixels of a picture), and pasting means a waiting NewContent rather than a
 * second reference to the original. What has no such description, a vector
 * drawing or an embedded form, is named at the paste instead of being pasted
 * wrong. PageClipboard carries both halves across the clipboard.
 *
 * ## What was pasted is an object, not a preview
 *
 * A pasted copy is not in the file yet, and for a while that made it a second
 * class of thing: drawn, but with no boundary to click, no handles and no way to
 * drag it, until the document had been saved and read again. It is one class of
 * thing now. Each waiting insertion is given an object number of its own from -2
 * downwards (see @ref FirstPasteHandle), and appears in every list of what is on
 * the page beside the operators the file holds. Hit-testing, the band, the
 * handles, the gestures, alignment, the properties panel, cut, delete and taking
 * a change back are then the same code for both, because they are looking at the
 * same kind of record.
 *
 * A gesture made on a pasted object is folded into @ref m_pending under that
 * negative number, exactly as a gesture on a read one is. Nothing addressed to a
 * negative number is ever handed to the engine as an edit: it is applied to the
 * insertion on the way out instead, so what the user dragged is the geometry
 * that gets written.
 *
 * That last step is why ps::NewContent carries a matrix beside its rectangle.
 * A rectangle can say a move and a stretch and nothing else, so for a while a
 * turn made on something pasted had nowhere to be written down and was refused
 * outright. It goes into the matrix now, and the composer writes the two
 * together, which makes an upright box simply the case where the matrix is the
 * identity, rather than a different kind of thing.
 *
 * ## The page underneath is a photograph, so the object is taken off it
 *
 * What the layer draws over is a picture of the file as it stands, and nothing
 * in it moves when a boundary here does. Left alone, that is a frame sliding
 * across a page while the thing it is round sits exactly where it was, which is
 * what "the images do not travel at all" meant.
 *
 * So the moment something is chosen, and before anybody has dragged anything,
 * two pictures are made off the GUI thread and kept: the page with the chosen
 * objects left out, and the chosen objects on their own with a proper edge. From
 * then on the gesture is nothing but blitting. The hole is covered with the
 * first, the ink is drawn wherever the gesture has carried it, and no file is
 * touched, nothing is rendered and nothing is decided between one frame and the
 * next. That is @ref Lift, and @ref startLift explains how the second picture
 * gets an alpha channel out of a rasteriser that has none.
 *
 * The preparation costs a fifth of a second on a dense page, which is why it is
 * started by the *choice* rather than by the drag: a drag is always preceded by
 * the click that chose the thing. A gesture that starts before the pixels are
 * ready simply moves the boundary on its own, as it always did, and takes up the
 * ink the moment it arrives.
 *
 * What was dragged and then let go of does not go back: the same picture carries
 * everything else waiting on that row, drawn where it now stands, so a page with
 * five changes on it shows all five whether or not anything is chosen.
 *
 * The limits are worth knowing. The ink is drawn on top of the whole page, so an
 * object that was under something else on the sheet comes out from under it as
 * soon as it moves. Scaling and turning are a bitmap transform of the ink until
 * the gesture settles, and then the pixels are made again with the turn written
 * into the page they come from; what makes them exact for good is saving the
 * document. And a highlight or a comment lying over the object stays where it
 * is, which is right: it was put on the page, not on the object. Only one row is
 * held, so changes waiting on a page that is not the one being worked on wait
 * for the save to be seen.
 *
 * ## Several things at once are lined up, not dragged into line
 *
 * From two objects upwards the properties panel grows a row of alignment
 * buttons, because the eye cannot put two left edges on the same line by
 * dragging and the arithmetic can. The buttons work on the boundaries as the
 * page now shows them, waiting changes and all, and hand back a new boundary
 * per object, which becomes one more waiting change apiece.
 *
 * ## Refusals arrive while the mouse is still on the spot
 *
 * An object whose placement matrix cannot be reversed arithmetically cannot be
 * moved at all, and the stacking order cannot be changed yet. Both come back
 * through @ref refused() at the moment the gesture is made, because a refusal
 * that arrives at saving time is a refusal about work already done.
 *
 * ## Where it goes in the order
 *
 * A drag that starts on bare paper bands a selection here, so this belongs
 * after any overlay that has to win a press on empty page.
 *
 * ## Which page is read, and which page is written
 *
 * A row of the view is a position in a list the organiser rearranges; the page
 * it shows lives at some index inside one of the files the session has open, and
 * on a merged document it is not even the same file from one row to the next. So
 * a page is *read* through @ref setDocument's ps::PageRows (the file that holds
 * it and its index inside that file) and turned by however much the organiser
 * has turned the row, exactly as TextOverlay and AnnotationOverlay do. What is
 * *written* stays addressed by row, because the window hands the engine a
 * document assembled row by row. Getting that pair the wrong way round is what
 * put a handle on another page's text; see PageRows.h.
 */
class ObjectOverlay : public QObject, public PageView::Overlay, public InspectionSource
{
    Q_OBJECT

public:
    /** Adds itself to @p view and takes itself off again when it dies. */
    explicit ObjectOverlay(PageView *view, QObject *parent = nullptr);
    ~ObjectOverlay() override;

    /**
     * The document whose rows are being edited. Not owned; must outlive this.
     *
     * Where a row's page is actually read from, and what lets the layer follow
     * the pages when the organiser moves them. Without it the rows are taken to
     * be pages of @ref setSource's file, which is what this did before and is
     * true only until the first page is deleted.
     */
    void setDocument(Document *document);

    /**
     * The file the objects are read from when there is no document to ask.
     *
     * Object numbers belong to a file, so a new one drops everything that was
     * waiting rather than pointing old numbers at new operators.
     */
    void setSource(const QString &pdfPath);

    QString source() const { return m_source; }

    /** Reads the pages again, keeping what is waiting. For after the file changed. */
    void reread();

    // ── PageView::Overlay ─────────────────────────────────────────────────
    bool appliesTo(PageView::Mode mode) const override;
    void paint(QPainter &painter, int row, const QRect &pageRect) override;
    bool press(int row, const QPointF &pagePoint, Qt::MouseButtons buttons) override;
    bool move(int row, const QPointF &pagePoint, Qt::MouseButtons buttons) override;
    bool release(int row, const QPointF &pagePoint) override;
    Qt::CursorShape cursor(int row, const QPointF &pagePoint) override;
    bool copy() override;
    bool cut() override;
    bool paste(const QMimeData *data, int row) override;

    // ── InspectionSource ──────────────────────────────────────────────────
    Inspectable *inspected() override;

    /**
     * Everything waiting on the page's own operators, as the composer takes it.
     *
     * A change made to something pasted is left out: it names no operator, and
     * it has already been folded into what @ref pendingInsertions() answers.
     */
    QVector<PageEdit> pendingEdits() const;

    bool hasPendingEdits() const;

    /**
     * Everything pasted and not yet written, as PageComposer::apply() takes it.
     *
     * Separate from pendingEdits() because the engine takes the two separately:
     * an edit names an operator the page already has, an insertion names none.
     *
     * Where each one has since been dragged, scaled or recoloured is already in
     * it, and one that has been deleted is not in it at all, so what comes out
     * of here is the page the user is looking at, not the paste as it landed.
     */
    QVector<NewContent> pendingInsertions() const;

    bool hasPendingInsertions() const;

    /** The page the choice lives on, or -1. A choice never spans two pages. */
    int chosenPage() const { return m_page; }

    QVector<int> chosenObjects() const;

    /**
     * True when @p row is showing the chosen objects as ink rather than as a box.
     *
     * The distinction the whole arrangement exists for, so it is worth being
     * able to ask: until it answers true a gesture moves a boundary over a page
     * that has not changed, and once it does, what moves is the object.
     */
    bool isLifted(int row) const;

    /**
     * True while better pixels than the ones in hand are being made.
     *
     * Not the opposite of @ref isLifted: the two are true together for as long
     * as a lift that still says something true is being replaced by a sharper or
     * a fuller one, which is exactly the state that keeps the object under the
     * pointer instead of flickering back to where the file draws it.
     */
    bool isLifting() const;

public Q_SLOTS:
    void clearChoice();

    /** Takes the chosen objects off the page, as a waiting change. */
    void removeChosen();

    /** Drops the change waiting on the chosen objects. */
    void takeBack();

    /** Drops every waiting change, on every page. */
    void takeBackAll();

    /** Moves what is chosen, in points; up is positive, as on the page. */
    void nudge(double acrossPoints, double upPoints);

    /**
     * Puts every chosen object on the same @p edge of the box round them all.
     *
     * The same command the panel's buttons give, so that a menu entry or a
     * shortcut reaches it without the properties panel having to be open.
     * Objects the engine cannot move are left where they are and named through
     * @ref refused().
     */
    void alignChosen(Alignment::Edge edge);

    /** Leaves equal gaps between the chosen objects along @p spread. */
    void distributeChosen(Alignment::Spread spread);

    /** Gives every chosen object the size of the largest of them. */
    void matchChosenSize(Alignment::Match what);

    void raiseChosen();
    void lowerChosen();

Q_SIGNALS:
    /** Something is waiting that was not waiting before, or is no longer. */
    void editsChanged();

    /** A different thing is chosen: the properties panel should be rebuilt. */
    void choiceChanged();

    /** What could not be done, in words the user can act on, as it is asked. */
    void refused(const QString &reason);

protected:
    /** The Overlay interface has no key hook; see the definition for why this does. */
    bool eventFilter(QObject *watched, QEvent *event) override;

    /** Runs after the choice has settled, and starts the lift; see @ref Lift. */
    void timerEvent(QTimerEvent *event) override;

private:
    /** The attributes of the chosen object, for the properties panel. */
    class Details;

    /** Which of the nine marks on the chosen box is being held. */
    enum class Grip { None, TopLeft, Top, TopRight, Right, BottomRight, Bottom, BottomLeft, Left, Turn };

    /** What the mouse is doing between a press and its release. */
    enum class Drag { None, Band, Move, Resize, Turn };

    /** No object at all: what a hit test answers when it missed everything. */
    static constexpr int NoObject = -1;

    /**
     * The first number given to something pasted, counting down from there.
     *
     * An object the file has is numbered by its painting operation, counting up
     * from zero. Something pasted has no painting operation to be numbered by,
     * so it is given a number below @ref NoObject, far enough from a real one
     * that both can share every list, every hit test and every gesture without
     * ever colliding.
     */
    static constexpr int FirstPasteHandle = -2;

    static bool isPasted(int object) { return object <= FirstPasteHandle; }

    /**
     * One pasted thing, waiting to be written, with a number to take it by.
     *
     * The content is kept as it was pasted; where the user has since dragged it
     * lives in @ref m_pending under @ref handle, in the same place and the same
     * shape as a move made to an object the file already has. That is what lets
     * one gesture path serve both.
     */
    struct Insertion {
        NewContent content;
        int handle = FirstPasteHandle;
    };

    /** What the file draws on @p row. Read once and kept; parsing is not free. */
    const QVector<PageObject> &readOf(int row);

    /** Everything on @p row a person can point at: the file's and the pasted. */
    QVector<PageObject> objectsOf(int row);

    /** @p insertion as something with a boundary, a number and a place in the order. */
    static PageObject objectOf(const Insertion &insertion, int layer);

    /** The pasted thing numbered @p handle, or nullptr. */
    const Insertion *insertionFor(int handle) const;

    /** @p insertion as it will be written: the change waiting on it applied. */
    NewContent insertionAsWritten(const Insertion &insertion) const;

    /** The one change waiting on an object, or a default one meaning none. */
    PageEdit waitingOn(int page, int object) const;

    /** Whether anything chosen was pasted rather than read. */
    bool choiceHoldsPaste() const;

    /**
     * Follows the pages when the organiser moves them, and says what is lost.
     *
     * Everything waiting is addressed by row, and a row is a position rather
     * than a page. A change waiting on a page that merely moved goes with it; a
     * change waiting on a page that has been deleted has nowhere left to be
     * written, so it is dropped and named through @ref refused() rather than
     * applied to whatever has moved into that slot.
     */
    void rebaseRows();

    /** The same for a row that kept its number and changed underneath. */
    void refreshRows();

    /** One body for both: carries everything waiting through @p change. */
    void carryRows(const Rebase &change);

    /** @p row as it now stands, with everything waiting already applied. */
    QVector<PageObject> shownOf(int row);

    /** One object of @p row with what is waiting on it applied; index -1 when gone. */
    PageObject shownObject(int row, int index);

    /** The box round everything chosen, in points, without the drag in progress. */
    QRectF choiceBounds();

    Grip gripAt(int row, const QPointF &pagePoint);

    /** Where a grip is drawn, in viewport pixels, given the chosen box in pixels. */
    static QPointF gripCentre(const QRectF &box, Grip grip);
    static Qt::CursorShape cursorForGrip(Grip grip);

    /** The matrix the drag in progress stands for, in page space. */
    QTransform liveTransform() const;
    QTransform transformFor(Drag drag, const QPointF &to) const;

    /**
     * Folds one gesture into what is already waiting on each of @p objects.
     *
     * The gesture is handed the object's boundary *as it now stands*, because
     * turning something round its middle means the middle it has after the two
     * moves that came before, not the one it had in the file.
     */
    void applyGesture(const std::function<QTransform(const QRectF &now)> &gesture, const QVector<int> &objects);
    void applyGesture(const std::function<QTransform(const QRectF &now)> &gesture);

    /**
     * The same, where each object is given a matrix of its own.
     *
     * Lining a row up moves every object by a different amount, and doing that
     * one applyGesture() call at a time would say "one change at a time" once
     * per object for what the user did once.
     */
    void applyGestures(const std::function<QTransform(int index, const QRectF &now)> &gesture,
                       const QVector<int> &objects);

    /** The chosen objects the engine can actually move, without saying anything. */
    QVector<int> movableChosen();

    /**
     * The boundaries alignment works on: the movable chosen ones, as shown.
     *
     * In movableChosen()'s order (the same walk over the same list), so that a
     * set of boundaries handed out here can be handed back in and still name
     * the objects it came from.
     */
    QVector<QRectF> alignableBoxes();

    /**
     * Puts the movable chosen objects at @p boxes, one waiting change apiece.
     *
     * The counterpart of alignableBoxes(): what the alignment arithmetic hands
     * back comes in here, in the order it went out.
     */
    void placeChosen(const QVector<QRectF> &boxes);

    /** What is chosen, described as content that could be written again. */
    PageClipboard::Parcel parcelOfChoice();

    /** Puts the choice on the clipboard; @p missing takes what it could not carry. */
    bool putChoiceOnClipboard(QStringList *missing);

    /**
     * The stored pixels of one placed picture, or a null image and the reason.
     *
     * @p row rather than the object's own page number: the picture has to be
     * read out of the file that actually holds it, and on a merged document that
     * is not the same file for every row.
     */
    QImage pixelsOf(int row, const PageObject &object, QString *why);

    /**
     * Draws the content of whatever in @p shown was pasted rather than read.
     *
     * Given the objects of @p row as they now stand, and @p live, the drag in
     * progress, so that the preview is under the same boundary the same pass is
     * about to draw round it, turn and all.
     */
    void paintInsertions(QPainter &painter, int row, const QVector<PageObject> &shown, const QTransform &live);

    /**
     * The chosen objects taken off the page as pixels, so a gesture is a blit.
     *
     * Two pictures of one row, both made from the file as it stands and neither
     * of them made again while the gesture lasts. Where the objects have got to
     * is read afresh on every frame out of @ref m_pending, so a drag, a nudge
     * and a turn all cost the same as drawing two images: nothing is rendered,
     * nothing is written and nothing is decided.
     */
    struct Lift {
        /** Which row these are pixels of. Below zero when there are none. */
        int row = -1;

        /** Which run made them, so that a late answer to an old question goes. */
        quint64 run = 0;

        /**
         * Where @ref patch belongs, in display points.
         *
         * In points rather than in pixels, so that a lift made before a zoom
         * goes on being drawn at the right place, merely stretched. Which is
         * what keeps the object under the pointer while sharper pixels are made.
         */
        QRectF where;

        /** The page as the file draws it, with the lifted objects left out. */
        QImage patch;

        /**
         * True when @ref patch also carries somebody else's waiting change.
         *
         * Something moved and then let go of is still not where the file draws
         * it, and the sheet underneath goes on showing it in the old place until
         * the document is saved. So the patch takes those in as well, at the
         * places they now stand, and then it has to be drawn even when nothing
         * is being dragged at all.
         */
        bool corrects = false;

        /** One waiting change, the objects under it, and their ink. */
        struct Ink {
            /**
             * How much of the waiting change is already in the pixels.
             *
             * A move is never in them: it is a blit, and a blit is exact at any
             * distance. A scale or a turn is, once the gesture has settled,
             * because a bitmap put through either goes soft, and the whole point
             * of this is that the screen shows what the file will. What is drawn
             * is therefore whatever the object stands under now with this taken
             * back off it, which for the ordinary case is the identity and for a
             * settled turn is the move made since.
             */
            QTransform baked;

            /**
             * The objects, which share one waiting change and so one matrix.
             *
             * They have to share it: one picture of several objects can only be
             * carried by one matrix, and two objects dragged to different places
             * are two pictures.
             */
            QVector<int> objects;

            /** Where @ref pixels belongs, in display points, @ref baked included. */
            QRectF where;

            /**
             * The waiting change the pixels were made under.
             *
             * Only its *look* is ever compared, which is the whole point: what
             * the objects have been dragged to since does not touch the ink, and
             * what they have been recoloured to does.
             */
            PageEdit under;

            /** Premultiplied, with the coverage the rasteriser gave the edges. */
            QImage pixels;
        };

        QVector<Ink> inks;
    };

    /**
     * What a lift was asked for, so that the same question is not asked twice.
     *
     * A drag changes where the objects are and not what they look like, which is
     * exactly the difference this records: the matrices are deliberately absent,
     * and only how the objects are *grouped* by them is kept. So dragging a
     * choice about costs nothing here, while splitting it, recolouring it or
     * choosing something else asks again.
     */
    struct LiftAsk {
        int row = -1;
        int widthPx = 0;

        /** Sorted: what is chosen on the row, and what is waiting on it. */
        QVector<int> objects;

        /** objects.at(i) shares its waiting change with objects.at(groups.at(i)). */
        QVector<int> groups;

        /** One per group, in the order the groups first appear in @ref objects. */
        QVector<PageEdit> looks;

        /**
         * One per group: its waiting change with the movement taken out of it.
         *
         * Which is to say what the change does to the object's *shape*. Moving
         * something asks for no new pixels, because a blit lands anywhere
         * exactly; scaling or turning it does, because putting a bitmap through
         * either is soft and the page it will be saved as is not. Keeping only
         * the shape is what stops a settled turn asking again on every nudge
         * that follows it.
         */
        QVector<QTransform> shapes;
    };

    /**
     * Draws @p row as it now stands rather than as the file has it, or nothing.
     *
     * Nothing while everything is still where the file draws it: the sheet
     * underneath is already the truth then, and blitting a copy of it over
     * itself can only be a way of getting it slightly wrong.
     */
    void paintLift(QPainter &painter, int row, const QTransform &live);

    /** What a lift for the choice as it now stands would be made of. */
    LiftAsk liftWanted() const;

    /** Notices that the answer in hand is no longer the answer to the question. */
    void reviewLift();

    /**
     * Whether the pixels in hand still say something true about the page.
     *
     * Deliberately weaker than "these are the pixels that would be made now".
     * Choosing a second object, or letting go of one, asks for a better lift
     * without making the one in hand a lie, and dropping it for that would take
     * the ink off the page for as long as the new pictures take to arrive. What
     * does make it a lie is an object recoloured under it, or a group of them
     * pulled apart so that one picture can no longer carry them.
     */
    bool liftHolds() const;

    /** Throws the pixels away. For when they would be a lie rather than a wait. */
    void dropLift();

    /**
     * Composes the pages the lift is made of and has them drawn off this thread.
     *
     * Three renders of one page: the page with the chosen objects taken out of
     * it and everything else waiting on it written in, which covers the hole and
     * puts the rest of the page right in one go; and the chosen objects alone
     * over a white backdrop and over a black one.
     *
     * The two backdrops are how an object gets an alpha channel out of a
     * rasteriser that only ever hands out opaque sheets. Over black a pixel
     * comes back as coverage times colour, which is exactly a premultiplied
     * pixel; over white the same pixel comes back with however much of the white
     * still shows through added to it. The difference between the two is that
     * leftover white, so it says what the coverage was, and it says it exactly:
     * antialiased edges, soft masks and transparency groups all come out right,
     * because this is the arithmetic compositing itself is defined by.
     */
    void startLift();

    void liftArrived();

    /** Everything one lift needs, and nothing that belongs to the GUI thread. */
    struct LiftJob {
        /** The file the page is composed from, and which page of it that is. */
        QString file;
        int page = 0;

        /** Where the composed copies are written, and which run they belong to. */
        QString scratch;
        quint64 run = 0;

        int row = -1;
        int widthPx = 0;

        /**
         * What the patch is composed with: the lifted objects taken off the
         * page, and everything else waiting on it written in where it now
         * stands.
         */
        QVector<PageEdit> hide;
        bool corrects = false;
        QRect patchTile;
        QRectF patchWhere;

        /** One waiting change, and the page composed to hold only what is under it. */
        struct Group {
            QVector<int> objects;

            /** Written into the composed page rather than left to the painter. */
            QTransform bake;

            /** The waiting change these are the pixels of; see Lift::Ink::under. */
            PageEdit under;

            /** Deletes for everything else, and the group's own recolouring. */
            QVector<PageEdit> alone;
            QRect tile;
            QRectF where;
        };

        QVector<Group> groups;
    };

    /** Runs off the GUI thread: composes, renders, and touches nothing here. */
    static Lift liftFrom(const LiftJob &job);

    /** The part of @p row's whole-page pixels that @p wanted covers, in points. */
    QRect tileFor(int row, const QRectF &wanted) const;

    /** The whole-page device width @p row is drawn at, or zero. */
    int liftWidthOf(int row) const;

    /**
     * Everything between @p index as the file draws it and as it is drawn now.
     *
     * The change waiting on it and, when it is one of the chosen and @p live is
     * a gesture in progress, that gesture on top. One answer for the boundary,
     * the picture under the boundary and anything else that has to agree with
     * them about where a thing has got to.
     */
    QTransform standingOn(int row, int index, const QTransform &live) const;

    /**
     * The view's own points-to-pixels conversion for @p row, as one matrix.
     *
     * For drawing something that is turned, where a box in pixels is not enough
     * to say where the content goes.
     */
    QTransform toPixels(int row) const;

    void restyleChosen(const QColor &fill, const QColor &stroke, double lineWidth);

    void choose(int row, const QVector<int> &objects, bool add);
    void setHover(int row, int index);
    void repaint();

    /** Keeps the panel's numbers honest while a drag moves what it describes. */
    void refreshDetails();

    QPointer<PageView> m_view;
    QString m_source;

    /** Rows to pages in files and back; the only place the two are converted. */
    PageRows m_rows;

    /** Read once per row and kept: parsing a content stream is not free. */
    QHash<int, QVector<PageObject>> m_objects;

    /** Where the row's page is read from when there is a document to ask. */
    Document *m_document = nullptr;

    /**
     * One row of the document as pixels: its hole and its travelling ink.
     *
     * One of them, because there is one choice, and because two page-sized
     * pictures is already tens of megabytes. What that costs is bounded rather
     * than counted: see LiftBudgetBytes.
     */
    Lift m_lift;

    /** What @ref m_lift answers, or what the run in flight was started for. */
    LiftAsk m_liftAsk;

    /** Where the one-page copies the lift is composed from are written. */
    QTemporaryDir m_scratch;

    /** Runs after the choice settles, so ten Ctrl-clicks are one preparation. */
    QBasicTimer m_liftSettle;

    /** Counts the runs, so that each writes to scratch files of its own. */
    quint64 m_liftRun = 0;

    /**
     * Set between a run being started and its answer being taken.
     *
     * Kept here rather than asked of the future, which stops running a moment
     * before its answer is delivered. In that moment a caller waiting for the
     * pixels would be told they had arrived while the ones in hand were still
     * the ones from before.
     */
    bool m_liftBusy = false;

    QFutureWatcher<Lift> *m_liftWatcher = nullptr;

    /**
     * Page, then object, then the one change waiting on it.
     *
     * Holds changes to what was pasted too, under the pasted thing's own
     * negative number. Which is the point: one drag, one code path, one record,
     * whether the thing under the pointer came out of the file or off the
     * clipboard a moment ago.
     */
    QHash<int, QHash<int, PageEdit>> m_pending;

    /** What has been pasted and not yet written. Each carries the page it is on. */
    QVector<Insertion> m_insertions;

    /** Never given out twice, so a stale change can never name a fresh paste. */
    int m_nextHandle = FirstPasteHandle;

    /**
     * How many pastes deep the clipboard's content is on the page.
     *
     * Each one steps further from the last, so pasting three times leaves three
     * copies to be found rather than three in one place looking like one.
     */
    int m_pasteWalk = 0;

    int m_page = -1;
    QSet<int> m_chosen;

    /** The one the panel describes: the last one pointed at. */
    int m_current = NoObject;

    int m_hoverPage = -1;
    int m_hover = NoObject;

    Drag m_drag = Drag::None;
    Grip m_grip = Grip::None;
    int m_dragRow = -1;
    QPointF m_from;
    QPointF m_to;
    QRectF m_startBox;

    std::unique_ptr<Details> m_details;
};

} // namespace ps
