/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#pragma once

#include "Inspector.h"
#include "PageRows.h"
#include "PageView.h"
#include "core/Annotation.h"

#include <QColor>
#include <QHash>
#include <QObject>
#include <QPointF>
#include <QPointer>
#include <QRectF>
#include <QSet>
#include <QString>
#include <QVector>

#include <memory>

class QLabel;

namespace ps {

class Document;

/**
 * Comments, made on the page they are about.
 *
 * A comment is a remark about a particular line, and the way it used to be made
 * here (a dialog with the page shrunk into a box beside a list of everything
 * already said) asked the user to aim at a postage stamp and then read their
 * own remarks out of a list that could not show them what they referred to.
 * Marking up the document view instead means the highlight goes over the words
 * at the size they are read at, and the remark sits where it was made.
 *
 * ## Both modes, deliberately
 *
 * A reader shows comments: in View mode everything the document carries is
 * drawn, can be clicked to bring its attributes into the properties panel, and a
 * note gives up its text, but nothing can be drawn, moved or deleted, so
 * handing someone the program in reading mode is safe. Edit mode adds the tools.
 *
 * The press is never taken from the reader in View mode. Dragging over a line
 * that happens to be highlighted has to select the words underneath, because
 * copying a quoted sentence out of a marked-up contract is the reason the
 * comments were made in the first place.
 *
 * ## Where it goes in the view's order
 *
 * Before TextOverlay and ObjectOverlay. A drawing tool has to win the press on a
 * line of text, or a highlight dragged over a paragraph would open the caret in
 * it instead.
 *
 * ## What it does not do
 *
 * It does not write the PDF. Everything is held in memory against the view's own
 * page rows and handed over through @ref annotationsByRow(), because a comment
 * has to be undoable next to every other change rather than beside it.
 *
 * ## The rows move under it
 *
 * A row is a position in a list the organiser rearranges, so a comment held
 * against row zero belongs to a different sheet the moment a page is inserted in
 * front of it. Every change to the page list is therefore followed rather than
 * treated as a reason to start again (see PageRows.h), and a comment survives
 * pages being moved, inserted, turned or replaced. The one case that cannot be
 * followed is the page it was made on being deleted: there is nothing left to
 * anchor it to, so it is dropped and said out loud through @ref refused().
 * Turning a page turns the comments on it with it, and leaves every other page
 * alone.
 */
class AnnotationOverlay : public QObject, public PageView::Overlay, public InspectionSource
{
    Q_OBJECT

public:
    /** What a press does. Select picks up what is already on the page. */
    enum class Tool {
        Select,
        Highlight,
        Underline,
        StrikeOut,
        Rectangle,
        Ellipse,
        Line,
        Freehand,
        TextBox,
        Note,
    };
    Q_ENUM(Tool)

    Q_PROPERTY(Tool tool READ tool WRITE setTool NOTIFY toolChanged)
    Q_PROPERTY(QColor colour READ colour WRITE setColour NOTIFY colourChanged)
    Q_PROPERTY(double lineWidth READ lineWidth WRITE setLineWidth NOTIFY lineWidthChanged)

    /** Adds itself to @p view and takes itself off again when it dies. */
    explicit AnnotationOverlay(PageView *view, Document *document, QObject *parent = nullptr);
    ~AnnotationOverlay() override;

    Tool tool() const { return m_tool; }

    QColor colour() const { return m_colour; }

    double lineWidth() const { return m_lineWidth; }

    /** Whose name new comments are made in. */
    QString author() const { return m_author; }

    void setAuthor(const QString &author);

    /**
     * Every comment the document should end up with, keyed by view row.
     *
     * Rows the user has never looked at are absent, so nothing is asserted about
     * them; a row whose last comment was deleted is present and empty, which is
     * how "clear this page" is said.
     */
    QHash<int, QVector<Annotation>> annotationsByRow() const;

    /** True when a comment has been added, moved, changed or deleted. */
    bool isModified() const { return !m_touched.isEmpty(); }

    /** The row the selected comment is on, or -1 when nothing is selected. */
    int selectedRow() const { return m_selectedRow; }

    /** Where the selection sits in its row's list, or -1. */
    int selectedIndex() const { return m_selected; }

    /** A copy of the selected comment, or a default-built one when there is none. */
    Annotation selectedAnnotation() const;

    // ── PageView::Overlay ─────────────────────────────────────────────────

    bool appliesTo(PageView::Mode mode) const override;
    void paint(QPainter &painter, int row, const QRect &pageRect) override;
    bool press(int row, const QPointF &pagePoint, Qt::MouseButtons buttons) override;
    bool move(int row, const QPointF &pagePoint, Qt::MouseButtons buttons) override;
    bool release(int row, const QPointF &pagePoint) override;
    Qt::CursorShape cursor(int row, const QPointF &pagePoint) override;

    // ── InspectionSource ──────────────────────────────────────────────────

    Inspectable *inspected() override;

    // ── Attributes of the selection, as the properties panel changes them ──
    //
    // Each writes through at once and reports it as a change to the comments
    // rather than as a change of selection, so the panel is not rebuilt out from
    // under the control the user is still holding.

    void setSelectedContents(const QString &text);
    void setSelectedAuthor(const QString &author);
    void setSelectedColour(const QColor &colour);
    void setSelectedLineWidth(double width);
    void setSelectedType(Annotation::Type type);

    /** Carries the selected comment to another page, and shows that page. */
    void moveSelectionTo(int row);

public Q_SLOTS:
    void setTool(Tool tool);

    /** The colour of the next mark, and of the selected comment if there is one. */
    void setColour(const QColor &colour);

    /** The same, for how thick a line is drawn, in points. */
    void setLineWidth(double width);

    void selectComment(int row, int index);
    void deselect();
    void removeSelected();

    /** Reads the pages again, dropping anything not yet written. */
    void reload();

    /**
     * Carries what is waiting across a change to the page list.
     *
     * The rows are followed rather than forgotten: a comment on a page that
     * merely moved is still on that page. Only a comment whose page has been
     * deleted cannot be carried, and that is reported through @ref refused()
     * rather than done quietly.
     */
    void rebaseRows();

    /** The same for a row that kept its number and changed underneath. */
    void refreshRows();

Q_SIGNALS:
    void toolChanged(Tool tool);
    void colourChanged(const QColor &colour);
    void lineWidthChanged(double width);

    /**
     * What was lost and why, in words a person can act on.
     *
     * Same shape as PageView::refused() and ObjectOverlay::refused(), so that
     * all of them can go to the same place in the window.
     */
    void refused(const QString &reason);

    /** A comment was added, moved, changed or deleted. The file is untouched. */
    void annotationsChanged();

    /** A different comment is selected, so the properties panel is stale. */
    void selectionChanged();

protected:
    /** The Overlay interface has no key hook; see the definition for why this does. */
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    /** The selected comment's attributes, for the properties panel. */
    class Details;

    /** Everything one opened file carries, read once and kept. */
    const QVector<Annotation> &commentsOf(int sourceId);

    /** The comments of @p row in the view's own space, read on first sight. */
    const QVector<Annotation> &annotationsOf(int row);

    /** The topmost comment under @p pagePoint on @p row, or -1. */
    int annotationAt(int row, const QPointF &pagePoint);

    /** The selected comment to write into, or null. Valid until the list moves. */
    Annotation *selectedComment();

    /** A new comment of @p type, with the current colour, width and author. */
    Annotation newAnnotation(Annotation::Type type) const;

    void addAnnotation(int row, const Annotation &annotation);
    void select(int row, int index);

    /** Says that @p row no longer holds what the file holds, and repaints. */
    void recordChange(int row);

    /** One body for both re-basing slots: carries every comment through @p change. */
    void carryRows(const Rebase &change);

    /** The mark the drag has produced so far, or an empty one when it is too small. */
    Annotation finishDrawing() const;

    void drawAnnotation(QPainter &painter, int row, const Annotation &annotation, bool selected);
    void drawPending(QPainter &painter, int row);

    /** @p pagePoint held inside @p row's paper. */
    QPointF clampToPage(int row, const QPointF &pagePoint) const;

    /** Puts what a note says on screen, or takes it away again. */
    void showNoteFor(int row, int index);
    void placeNote();
    void hideNote();

    void repaint();

    QPointer<PageView> m_view;
    Document *m_document = nullptr;

    /** Which page each row shows, so that a change to the list can be followed. */
    PageRows m_rows;

    Tool m_tool = Tool::Select;
    QColor m_colour = QColor(255, 212, 0);
    double m_lineWidth = 2.0;
    QString m_author;

    /** By source id, so a document merged from five files is read five times. */
    QHash<int, QVector<Annotation>> m_bySource;

    /** The rows that have been looked at, mapped into the view's space. */
    QHash<int, QVector<Annotation>> m_byRow;

    /** Rows that no longer hold what the file holds, empty ones included. */
    QSet<int> m_touched;

    int m_selectedRow = -1;
    int m_selected = -1;

    /** What the press started, in points on m_dragRow. */
    bool m_drawing = false;
    bool m_moving = false;

    /** Whether the drag actually shifted anything, so a click is not a change. */
    bool m_moved = false;
    int m_dragRow = -1;
    QPointF m_from;
    QPointF m_to;
    QVector<QPointF> m_path;

    /** Where the note being read is anchored, so scrolling can take it along. */
    int m_noteRow = -1;
    QRectF m_noteAnchor;
    QString m_noteText;
    QPointer<QLabel> m_note;

    std::unique_ptr<Details> m_details;
};

} // namespace ps
