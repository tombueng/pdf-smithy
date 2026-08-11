/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#pragma once

#include "Inspector.h"
#include "PageRows.h"
#include "PageView.h"
#include "core/Forms.h"
#include "core/RenderBackend.h"

#include <QFont>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QTransform>
#include <QVector>

#include <memory>

class QWidget;

namespace ps {

class FormFieldInspectable;

/**
 * The document's form, fillable where it stands on the page.
 *
 * A form that can only be filled in through a list of labels in a side panel is
 * a form the user has to read twice: once on the page to find out what is being
 * asked, and once in the panel to find the box that asks it. Three fields called
 * "Date" are indistinguishable there and obvious here. So the boxes are typed
 * into where they are, which is also where the answers will print.
 *
 * ## What is drawn and what is not
 *
 * The page underneath is a render of the real PDF, appearance streams and all,
 * so a field that draws its own frame already looks like a field and nothing is
 * added to it. Drawn here is what the document does not carry visibly: the pale
 * blue wash that says at a glance which parts of the sheet are to be filled in,
 * a hairline so that an unstyled box can be found at all, red where an answer is
 * required, and a grey dash where the form will not accept one. A value the user
 * has typed is drawn too, because the render below still shows the file as it
 * was opened and will keep showing it until the document is saved and re-read.
 *
 * ## Widgets, not fields
 *
 * Everything drawn and everything clicked is a FormField::Widget rather than a
 * field. A radio group is one field with one value and one widget per choice, so
 * a view that works in fields draws a single box for the whole group and reads
 * the group's value as "chosen" for every button in it, which is how a form
 * ends up with all of its options selected or none of them.
 *
 * ## Where the typing happens
 *
 * Clicking a field puts a real QLineEdit, QPlainTextEdit or QComboBox over that
 * widget's rectangle at that field's size, as a child of the view's viewport.
 * Tick boxes and radio buttons get no widget at all: a click is the whole
 * interaction.
 *
 * Nothing here writes a PDF. Values are kept in memory and handed over through
 * values(); saving belongs to whoever owns the file. A guessed label is never
 * among them: it is this program's reading of the page, not the form's own text,
 * and putting it back into the document would make a guess look like a fact.
 *
 * ## Pages in the file, rows in the view
 *
 * `Forms::read()` numbers a widget by the page it sits on **in the file it was
 * read from**. The view numbers pages by their position in the document being
 * assembled, and the two part company the moment a page is deleted, moved, or a
 * second file merged in. So every widget is anchored to a ps::SourcePage when
 * the form is read and looked up again through @ref setDocument's ps::PageRows
 * whenever it has to be drawn, clicked or described; see PageRows.h, where the
 * rule is written out. Without a document there is nothing to convert with and
 * the file's own numbering is all there is, which is what this did everywhere
 * before and is now only the fallback.
 *
 * ## Turned pages
 *
 * A page the organiser has turned is drawn turned, and its widget rectangles
 * were measured before it was, so they are put through @ref pageTurn() at the
 * two places they leave this class: where they are drawn, and where a click is
 * matched against them. What is applied is how far the page has been turned
 * *since the form was read*, not how far it is turned altogether: the file the
 * form came out of may already carry a turn baked into its `/Rotate`, and a page
 * that arrives upside-down in the file is drawn upside-down with its boxes
 * already upside-down on it.
 */
class FormOverlay : public QObject, public PageView::Overlay, public InspectionSource
{
    Q_OBJECT

public:
    /** @p view must outlive nothing: the overlay unregisters itself when it dies. */
    explicit FormOverlay(PageView *view, QObject *parent = nullptr);
    ~FormOverlay() override;

    /**
     * The document the fields are shown against. Not owned; must outlive this.
     *
     * What turns a page number in the form's own file into the row the view
     * draws it at. Give it before setFields(): the anchor for each of the form's
     * pages is taken at the moment the fields arrive, when the file's numbering
     * and the view's still agree.
     */
    void setDocument(Document *document);

    /** The fields to show, as Forms::read() returned them. Forgets any typing. */
    void setFields(const QVector<FormField> &fields);

    /**
     * Where to read the page's own words, so an unlabelled field can borrow one.
     *
     * Optional, and the overlay is fully usable without it: a tick box the
     * document never gave a /TU to then simply has no label, which is honest.
     *
     * With a document, which file and which page inside it are worked out per
     * widget, because a document assembled from two files has fields whose
     * labels are printed in different files. @p sourceId is the fallback for
     * when there is no document: the id of the one file to read them all from.
     */
    void setLabelSource(RenderBackend *backend, int sourceId = -1);

    /**
     * The row the view draws @p index on, or -1 when no row shows it any more.
     *
     * The number to put in front of a person: a field's page in the file it came
     * out of is not the page they are looking at.
     */
    int rowOfField(int index) const;

    /** What a field is called, and whether the document actually said so. */
    struct FieldLabel {
        QString text;

        /**
         * True when the text was taken off the page beside the field.
         *
         * A guess, and said to be one wherever it is shown: the nearest words to
         * a tick box are usually its label and sometimes the end of the sentence
         * before it. It is never written back into the document.
         */
        bool guessed = false;
    };

    /**
     * The label of @p index, or of one widget of it, guessed if need be.
     *
     * Per widget because the members of a radio group are told apart by the
     * words beside them and by nothing else: the group's own /TU, where there is
     * one, names the question rather than any of its answers.
     */
    FieldLabel labelOf(int index, int widget = 0) const;

    const QVector<FormField> &fields() const
    {
        return m_fields;
    }

    /** Only the fields whose value the user changed, keyed by field name. */
    QHash<QString, QString> values() const
    {
        return m_edited;
    }

    bool isModified() const
    {
        return !m_edited.isEmpty();
    }

    /** Takes the typed values to be the document's own, after they were saved. */
    void clearChanges();

    /** The field being filled in, as an index into fields(), or -1. */
    int selectedField() const
    {
        return m_selected;
    }

    /** Which widget of the selected field was clicked, for a group of them. */
    int selectedWidget() const
    {
        return m_selectedWidget;
    }

    /** What the field holds now, which is what was typed if anything was. */
    QString valueOf(int index) const;

    // ── PageView::Overlay ─────────────────────────────────────────────────

    bool appliesTo(PageView::Mode mode) const override;
    void paint(QPainter &painter, int row, const QRect &pageRect) override;
    bool press(int row, const QPointF &pagePoint, Qt::MouseButtons buttons) override;
    Qt::CursorShape cursor(int row, const QPointF &pagePoint) override;

    // ── InspectionSource ──────────────────────────────────────────────────

    Inspectable *inspected() override;

public Q_SLOTS:
    void selectField(int index);
    void deselect();

    /** The next and previous field in the form's own order, wrapping round. */
    void focusNextField();
    void focusPreviousField();

Q_SIGNALS:
    /** One field's answer changed. The document on disk is untouched. */
    void valueChanged(const QString &name, const QString &value);

    /** A different field is being filled in, so the properties panel is stale. */
    void selectionChanged();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    /** One box on a page: which field, and which of its widgets. */
    struct Spot {
        int field = -1;
        int widget = 0;
    };

    /** Which row shows which of the form file's pages, and what sits on it. */
    void rebuildRows();

    /** Follows the rows when the organiser moves them, then rebuilds. */
    void rebaseRows();

    /** The same for a row that kept its number and changed underneath. */
    void refreshRows();

    /** One body for both: takes @p change through the map and rebuilds. */
    void carryRows(const Rebase &change);

    /** The row @p widget is drawn on, or -1 when the document no longer shows it. */
    int rowOfWidget(const FormField::Widget &widget) const;

    /** Which page of which file @p widget sits on, for reading its words. */
    SourcePage pageOfWidget(const FormField::Widget &widget) const;

    /**
     * What to put a box measured on the form's @p page through to draw it at @p row.
     *
     * Identity for a page nobody has turned since the form was read, which is
     * every page of most documents.
     */
    QTransform turnOf(int row, int page) const;

    void rebuildOrder();
    void openEditor();
    void closeEditor();
    void placeEditor();
    void step(int direction);
    void scrollTo(const Spot &spot);
    void setValueOf(int index, const QString &value);
    void toggle(int index);
    bool isTicked(int index) const;

    /** Puts the group's value on @p widget's own state; the rest go off with it. */
    void choose(int index, int widget);

    /** Whether this one widget is the one showing an answer. */
    bool isChosen(int index, int widget) const;

    /** The widget of @p index the current value refers to, or 0. */
    int widgetShowing(int index) const;

    void selectSpot(int index, int widget);

    /** Throws away what was read off the page, when the pages change. */
    void forgetGuesses();

    /**
     * The words beside each widget of @p index, read off the page once.
     *
     * Empty until a label source is set; one entry per widget afterwards, itself
     * empty where there was nothing beside that widget worth calling a label.
     */
    const QStringList &guessesOf(int index) const;

    QString tooltipFor(const Spot &spot) const;

    /** True for the kinds that have an answer to give: text, choice, tick. */
    static bool isFillable(const FormField &field);

    /** The widget under @p pagePoint on @p row, topmost first, or field -1. */
    Spot spotAt(int row, const QPointF &pagePoint) const;

    /** Where a widget is drawn, in the viewport's own coordinates. */
    QRectF boxOf(const Spot &spot) const;

    /** The font the field's text is drawn in, at the view's current zoom. */
    QFont fontOf(const FormField &field) const;

    QPointer<PageView> m_view;
    QVector<FormField> m_fields;

    /** Rows to pages in files and back; the only place the two are converted. */
    PageRows m_rows;

    /**
     * Which row shows each page of the file the form was read out of.
     *
     * Keyed by the page number `Forms::read()` gave, and -1 for a page the
     * document no longer holds. The two are the same number when the form
     * arrives (the form is read out of the document's own file), and this is
     * what keeps them related to each other afterwards.
     */
    QHash<int, int> m_rowOfPage;

    /**
     * How far each of the form's pages was already turned when it was read.
     *
     * The baseline the boxes are measured against, and the reason the turn to
     * apply is a subtraction rather than PageRows::rotationOf() on its own. Save
     * the document under another name and the form is read again out of a file
     * that has the turn written into its `/Rotate`, while the page list goes on
     * carrying the same turn as the organiser's own, so the two would be
     * counted twice, and the boxes would land a further quarter turn away.
     */
    QHash<int, int> m_turnAtRead;

    /** Widgets by view row, so painting a page does not walk the whole form. */
    QHash<int, QVector<Spot>> m_byRow;

    /** The fillable fields in tab order. */
    QVector<int> m_order;

    QHash<QString, QString> m_edited;

    /**
     * The words found beside each widget, one list per field, or empty.
     *
     * Kept here rather than in FormField because it is a guess made from a
     * render of the page, and the core neither renders nor guesses. Filled in
     * as fields are asked about rather than all at once, which is why it is
     * mutable: reading a label is a question, not a change.
     */
    mutable QVector<QStringList> m_guessed;

    /**
     * The words of the pages a label has been looked for on.
     *
     * Keyed by the page in the file it was read from rather than by a row: on a
     * document merged from two files, two rows are two different files' pages
     * and a row number tells the renderer nothing.
     */
    mutable QHash<SourcePage, QVector<RenderBackend::Word>> m_words;

    RenderBackend *m_backend = nullptr;
    int m_sourceId = -1;

    int m_selected = -1;
    int m_selectedWidget = 0;

    /** The field under the pointer, so a whole radio group can light up at once. */
    int m_hovered = -1;

    QWidget *m_editor = nullptr;

    std::unique_ptr<FormFieldInspectable> m_inspectable;
};

/**
 * The selected field's attributes, for the properties panel.
 *
 * All of it is shown rather than edited: a field's name, kind and rules are the
 * form author's, and the one thing the user may change (the answer) is
 * changed on the page, where they can see what they are answering.
 */
class FormFieldInspectable : public Inspectable
{
public:
    FormFieldInspectable(FormOverlay *overlay, int index);

    QString kindName() const override;
    QString description() const override;
    QWidget *buildEditor(QWidget *parent) override;

private:
    /** Owned by the overlay, which destroys this before it goes itself. */
    FormOverlay *m_overlay = nullptr;
    int m_index = -1;
};

} // namespace ps
