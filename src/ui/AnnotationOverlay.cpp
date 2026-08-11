/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "AnnotationOverlay.h"

#include "core/Document.h"
#include "core/PageRef.h"
#include "core/Source.h"

#include <KLocalizedString>

#include <QColorDialog>
#include <QComboBox>
#include <QDateTime>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFormLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPlainTextEdit>
#include <QPolygonF>
#include <QPushButton>
#include <QScrollBar>
#include <QSpinBox>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <functional>
#include <utility>

using namespace Qt::Literals::StringLiterals;

namespace ps {

namespace {

/** How big a note marker is, in points. */
constexpr double NoteSize = 20.0;

/** A drag shorter than this in both directions is a click that slipped. */
constexpr double MinimumDragPoints = 3.0;

/** The least a marked band may be tall, in points: about one line of text. */
constexpr double MinimumMarkupHeight = 10.0;

/** How far outside its own boundary a comment still answers a click, in points. */
constexpr double SlackPoints = 2.0;

/** How far the mouse has to travel before freehand keeps another point. */
constexpr double InkStepPoints = 1.0;

bool isMarkup(Annotation::Type type)
{
    return type == Annotation::Type::Highlight || type == Annotation::Type::Underline
        || type == Annotation::Type::StrikeOut || type == Annotation::Type::Squiggly;
}

/** True for the kinds drawn with a pen, which are the ones a line width means anything to. */
bool isStroked(Annotation::Type type)
{
    return type == Annotation::Type::Square || type == Annotation::Type::Circle || type == Annotation::Type::Line
        || type == Annotation::Type::Ink;
}

Annotation::Type typeFor(AnnotationOverlay::Tool tool)
{
    switch (tool) {
    case AnnotationOverlay::Tool::Highlight:
        return Annotation::Type::Highlight;
    case AnnotationOverlay::Tool::Underline:
        return Annotation::Type::Underline;
    case AnnotationOverlay::Tool::StrikeOut:
        return Annotation::Type::StrikeOut;
    case AnnotationOverlay::Tool::Rectangle:
        return Annotation::Type::Square;
    case AnnotationOverlay::Tool::Ellipse:
        return Annotation::Type::Circle;
    case AnnotationOverlay::Tool::Line:
        return Annotation::Type::Line;
    case AnnotationOverlay::Tool::Freehand:
        return Annotation::Type::Ink;
    case AnnotationOverlay::Tool::TextBox:
        return Annotation::Type::FreeText;
    case AnnotationOverlay::Tool::Note:
        return Annotation::Type::Note;
    case AnnotationOverlay::Tool::Select:
        break;
    }
    return Annotation::Type::Square;
}

/**
 * The kinds, in the order the panel offers them.
 *
 * Squiggly is here although no tool draws one: a document may well carry one,
 * and a list that cannot say what something is would be worse than one that
 * cannot make it.
 */
constexpr Annotation::Type allTypes[] = {
    Annotation::Type::Highlight, Annotation::Type::Underline, Annotation::Type::StrikeOut, Annotation::Type::Squiggly,
    Annotation::Type::Square,    Annotation::Type::Circle,    Annotation::Type::Line,      Annotation::Type::Ink,
    Annotation::Type::FreeText,  Annotation::Type::Note,
};

QString describe(Annotation::Type type)
{
    switch (type) {
    case Annotation::Type::Highlight:
        return i18nc("@item annotation kind", "Highlight");
    case Annotation::Type::Underline:
        return i18nc("@item annotation kind", "Underline");
    case Annotation::Type::StrikeOut:
        return i18nc("@item annotation kind", "Strikethrough");
    case Annotation::Type::Squiggly:
        return i18nc("@item annotation kind", "Squiggly underline");
    case Annotation::Type::Ink:
        return i18nc("@item annotation kind", "Freehand");
    case Annotation::Type::Square:
        return i18nc("@item annotation kind", "Rectangle");
    case Annotation::Type::Circle:
        return i18nc("@item annotation kind", "Ellipse");
    case Annotation::Type::Line:
        return i18nc("@item annotation kind", "Line");
    case Annotation::Type::FreeText:
        return i18nc("@item annotation kind", "Text box");
    case Annotation::Type::Note:
        return i18nc("@item annotation kind", "Note");
    }
    return {};
}

/**
 * Everything the comment covers, in points.
 *
 * A comment says where it is in whichever of three ways suits its kind, and hit
 * testing, moving and the selection outline all need one answer.
 */
QRectF boundsOf(const Annotation &annotation)
{
    QRectF area = annotation.rect.normalized();
    for (const QRectF &quad : annotation.quads) {
        area = area.isNull() ? quad.normalized() : area.united(quad.normalized());
    }
    for (const QVector<QPointF> &stroke : annotation.strokes) {
        for (const QPointF &point : stroke) {
            const QRectF dot(point, QSizeF(0.01, 0.01));
            area = area.isNull() ? dot : area.united(dot);
        }
    }
    return area;
}

/** Moves every part of a comment together, so the three ways stay one shape. */
void translateBy(Annotation &annotation, const QPointF &delta)
{
    annotation.rect.translate(delta);
    for (QRectF &quad : annotation.quads) {
        quad.translate(delta);
    }
    for (QVector<QPointF> &stroke : annotation.strokes) {
        for (QPointF &point : stroke) {
            point += delta;
        }
    }
}

/**
 * Turns comments read from a source page into the space the view draws in.
 *
 * The engine reads a page as its own file has it; the view shows the page as the
 * organiser has since turned it. Without this, every mark on a page someone
 * straightened would sit at ninety degrees to the words it is about.
 *
 * The turn itself is ps::pageTurn(); what is here is only that a comment is made
 * of loose points as often as of rectangles, so every part of it has to go.
 */
void applyRotation(Annotation &annotation, int rotation, const QSizeF &shown)
{
    const QTransform turn = pageTurn(rotation, shown);
    if (turn.isIdentity()) {
        return;
    }

    annotation.rect = turn.mapRect(annotation.rect.normalized());
    for (QRectF &quad : annotation.quads) {
        quad = turn.mapRect(quad.normalized());
    }
    for (QVector<QPointF> &stroke : annotation.strokes) {
        for (QPointF &point : stroke) {
            point = turn.map(point);
        }
    }
}

/** Whether a comment holds enough to be shown as @p type at all. */
bool canBecome(const Annotation &annotation, Annotation::Type type)
{
    // Freehand is the one kind that cannot be made out of a box: there is no
    // ink in a rectangle, and an empty Ink annotation draws nothing at all.
    return type != Annotation::Type::Ink || !annotation.strokes.isEmpty();
}

/**
 * Makes @p annotation into @p type, inventing only the geometry that is missing.
 *
 * What the old kind used is kept rather than thrown away, so that changing one's
 * mind twice comes back to where it started.
 */
void convertTo(Annotation &annotation, Annotation::Type type)
{
    const QRectF bounds = boundsOf(annotation);
    annotation.type = type;
    if (bounds.isNull()) {
        return;
    }
    if (annotation.rect.normalized().isNull()) {
        annotation.rect = bounds;
    }

    if (isMarkup(type) && annotation.quads.isEmpty()) {
        annotation.quads = { annotation.rect.normalized() };
        annotation.rect = annotation.quads.constFirst();
    }
    if (type == Annotation::Type::Line
        && (annotation.strokes.isEmpty() || annotation.strokes.constFirst().size() < 2)) {
        // Points run up the page, so bottom() is the higher edge on paper: this
        // is the diagonal from the top left corner down to the bottom right one.
        annotation.strokes = { { QPointF(bounds.left(), bounds.bottom()), QPointF(bounds.right(), bounds.top()) } };
    }
    if (type == Annotation::Type::Note) {
        // A note is a marker of one size wherever it came from, and a marker the
        // size of a paragraph would be a coloured block nobody could read past.
        annotation.rect = QRectF(bounds.left(), bounds.bottom() - NoteSize, NoteSize, NoteSize);
    }
}

QString shortened(const QString &text)
{
    const QString one = text.simplified();
    return one.size() <= 60 ? one : one.left(59) + QChar(0x2026);
}

/** A button that shows the colour it holds and picks a new one when pressed. */
class Swatch : public QPushButton
{
public:
    Swatch(const QColor &initial, QWidget *parent, const std::function<void(const QColor &)> &picked)
        : QPushButton(parent)
        , m_colour(initial)
    {
        setAutoFillBackground(true);
        refresh();
        connect(this, &QPushButton::clicked, this, [this, picked] {
            const QColor chosen = QColorDialog::getColor(m_colour.isValid() ? m_colour : QColor(Qt::yellow), this,
                                                         i18nc("@title:window", "Comment Colour"));
            if (chosen.isValid()) {
                m_colour = chosen;
                refresh();
                picked(chosen);
            }
        });
    }

private:
    void refresh()
    {
        setText(m_colour.isValid() ? m_colour.name() : i18nc("@info no colour is set", "none"));
        if (!m_colour.isValid()) {
            setStyleSheet(QString());
            return;
        }
        // Black text on a dark swatch is unreadable, so the label follows the
        // swatch rather than the theme.
        const QString ink = m_colour.lightnessF() > 0.55 ? u"#101010"_s : u"#f0f0f0"_s;
        setStyleSheet(u"background-color: %1; color: %2;"_s.arg(m_colour.name(), ink));
    }

    QColor m_colour;
};

} // namespace

// ── What the properties panel shows ───────────────────────────────────────

/**
 * The selected comment's attributes, as the panel shows them.
 *
 * Everything here is editable, because unlike a form field a comment is the
 * user's own: what it says, who said it, what it looks like, what kind of mark
 * it is and which page it is on. The one thing not here is where on the page it
 * sits, which is dragged rather than typed.
 */
class AnnotationOverlay::Details : public Inspectable
{
public:
    explicit Details(AnnotationOverlay *overlay)
        : m_overlay(overlay)
    {
    }

    QString kindName() const override
    {
        return i18nc("@title the kind of thing shown in the properties panel", "Comment");
    }

    QString description() const override;
    QWidget *buildEditor(QWidget *parent) override;

private:
    /** The line width only means something to some kinds, and the kind can change. */
    void refreshWidth();

    AnnotationOverlay *m_overlay;

    // The panel is rebuilt whenever something else is selected and may be gone at
    // any moment, so this is held weakly and checked before it is used.
    QPointer<QDoubleSpinBox> m_width;
};

QString AnnotationOverlay::Details::description() const
{
    const Annotation comment = m_overlay->selectedAnnotation();
    const QString what = describe(comment.type);
    if (comment.contents.isEmpty()) {
        return what;
    }
    return i18nc("@info a comment's kind and what it says", "%1: “%2”", what, shortened(comment.contents));
}

QWidget *AnnotationOverlay::Details::buildEditor(QWidget *parent)
{
    auto *sheet = new QWidget(parent);
    auto *form = new QFormLayout(sheet);
    form->setContentsMargins(0, 0, 0, 0);
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

    const Annotation comment = m_overlay->selectedAnnotation();

    // ── what it says ──────────────────────────────────────────────────────
    auto *text = new QPlainTextEdit(sheet);
    text->setPlainText(comment.contents);
    text->setMaximumHeight(110);
    text->setPlaceholderText(i18n("What this comment says."));
    form->addRow(i18nc("@label:textbox what a comment says", "Comment:"), text);
    QObject::connect(text, &QPlainTextEdit::textChanged, sheet,
                     [this, text] { m_overlay->setSelectedContents(text->toPlainText()); });

    // ── how it looks ──────────────────────────────────────────────────────
    auto *colour
        = new Swatch(comment.colour, sheet, [this](const QColor &picked) { m_overlay->setSelectedColour(picked); });
    form->addRow(i18nc("@label:chooser the colour a comment is drawn in", "Colour:"), colour);

    m_width = new QDoubleSpinBox(sheet);
    m_width->setRange(0.5, 24.0);
    m_width->setDecimals(1);
    m_width->setSingleStep(0.5);
    m_width->setSuffix(i18nc("@item point suffix in a spin box", " pt"));
    m_width->setKeyboardTracking(false); // Or every digit typed on the way to 12 is a change of its own.
    m_width->setValue(comment.lineWidth);
    m_width->setToolTip(
        i18nc("@info:tooltip", "How thick the line is. Marked-up text and notes take no notice of it."));
    form->addRow(i18nc("@label:spinbox how thick a comment's line is", "Line:"), m_width);
    refreshWidth();
    QObject::connect(m_width, &QDoubleSpinBox::valueChanged, sheet,
                     [this](double value) { m_overlay->setSelectedLineWidth(value); });

    // ── what kind of mark it is ───────────────────────────────────────────
    auto *kind = new QComboBox(sheet);
    for (const Annotation::Type type : allTypes) {
        if (canBecome(comment, type)) {
            kind->addItem(describe(type), int(type));
        }
    }
    kind->setCurrentIndex(std::max(0, kind->findData(int(comment.type))));
    form->addRow(i18nc("@label:listbox highlight, note, rectangle and so on", "Kind:"), kind);
    QObject::connect(kind, &QComboBox::activated, sheet, [this, kind](int index) {
        m_overlay->setSelectedType(Annotation::Type(kind->itemData(index).toInt()));
        refreshWidth();
    });

    // ── who made it, and where ────────────────────────────────────────────
    auto *author = new QLineEdit(comment.author, sheet);
    author->setPlaceholderText(i18n("Whoever is making the comments."));
    form->addRow(i18nc("@label:textbox who made the comment", "Author:"), author);
    QObject::connect(author, &QLineEdit::textEdited, sheet,
                     [this](const QString &value) { m_overlay->setSelectedAuthor(value); });

    auto *page = new QSpinBox(sheet);
    page->setRange(1, std::max(1, m_overlay->m_document ? m_overlay->m_document->pageCount() : 1));
    page->setValue(m_overlay->selectedRow() + 1);
    page->setKeyboardTracking(false);
    form->addRow(i18nc("@label:spinbox which page the comment is on", "Page:"), page);
    QObject::connect(page, &QSpinBox::valueChanged, sheet,
                     [this](int value) { m_overlay->moveSelectionTo(value - 1); });

    auto *note = new QLabel(i18n("Comments sit beside the page rather than in it, so nothing underneath is changed. "
                                 "Nothing is written until the document is saved."),
                            sheet);
    note->setWordWrap(true);
    form->addRow(note);

    return sheet;
}

void AnnotationOverlay::Details::refreshWidth()
{
    if (m_width) {
        m_width->setEnabled(isStroked(m_overlay->selectedAnnotation().type));
    }
}

// ── Setting up ────────────────────────────────────────────────────────────

AnnotationOverlay::AnnotationOverlay(PageView *view, Document *document, QObject *parent)
    : QObject(parent)
    , m_view(view)
    , m_document(document)
{
    if (m_view) {
        m_view->addOverlay(this);

        // Key presses go to the view, which is what has the focus; the Overlay
        // interface has no hook for them, and Delete has to reach the comment
        // that was just clicked.
        m_view->installEventFilter(this);
        m_view->viewport()->installEventFilter(this);

        // What a note says is a widget over the page rather than part of it, so
        // everything that moves the page under it has to move it too.
        connect(m_view->verticalScrollBar(), &QScrollBar::valueChanged, this, &AnnotationOverlay::placeNote);
        connect(m_view->horizontalScrollBar(), &QScrollBar::valueChanged, this, &AnnotationOverlay::placeNote);
        connect(m_view, &PageView::zoomChanged, this, &AnnotationOverlay::placeNote);
        connect(m_view, &PageView::layoutChanged, this, &AnnotationOverlay::placeNote);

        connect(m_view, &PageView::modeChanged, this, [this] {
            // A mark half drawn when the mode changed was going to be drawn on a
            // page that is no longer offering to take it.
            m_drawing = false;
            m_moving = false;
            m_path.clear();
            hideNote();
            repaint();
        });
    }

    if (m_document) {
        m_rows.setDocument(m_document);

        // A comment is held against a view row, and a row is a position rather
        // than a page: inserting a sheet in front of one moves every comment
        // after it by one. Followed rather than forgotten, because the work is
        // the user's and the page it was made on is usually still there. The
        // one that is not is reported instead of being thrown away in silence.
        //
        // Queued, because moving a page is a removal followed by an insertion:
        // between the two signals the page is nowhere at all, and answering that
        // state would announce as lost what the user only dragged elsewhere.
        connect(m_document, &Document::pagesInserted, this, &AnnotationOverlay::rebaseRows, Qt::QueuedConnection);
        connect(m_document, &Document::pagesRemoved, this, &AnnotationOverlay::rebaseRows, Qt::QueuedConnection);

        // A row that changed in place kept its number: it was turned, or an
        // operation put a freshly written file behind it. Nothing can be lost by
        // that, so it is answered at once.
        connect(m_document, &Document::pagesChanged, this, &AnnotationOverlay::refreshRows);

        // A reset is a different document, and nothing said about the old one
        // means anything about it.
        connect(m_document, &Document::wasReset, this, &AnnotationOverlay::reload);
    }
}

AnnotationOverlay::~AnnotationOverlay()
{
    // The view keeps raw pointers to its overlays, so leaving one behind would be
    // a crash on the next repaint rather than a leak.
    if (m_view) {
        m_view->viewport()->removeEventFilter(this);
        m_view->removeEventFilter(this);
        m_view->removeOverlay(this);
    }
    delete m_note;
}

// ── The tools ─────────────────────────────────────────────────────────────

void AnnotationOverlay::setTool(Tool tool)
{
    if (m_tool == tool) {
        return;
    }
    m_tool = tool;

    // What is selected can only be moved and recoloured with the select tool, so
    // leaving it picked while another tool is armed would offer a handle that no
    // longer answers.
    if (tool != Tool::Select) {
        deselect();
    }
    repaint();
    Q_EMIT toolChanged(m_tool);
}

void AnnotationOverlay::setColour(const QColor &colour)
{
    if (!colour.isValid()) {
        return;
    }
    // Recolouring what is picked is what everyone expects from a colour button
    // while something is picked, and arming the next mark is what they expect
    // when nothing is.
    setSelectedColour(colour);
    if (m_colour == colour) {
        return;
    }
    m_colour = colour;
    Q_EMIT colourChanged(m_colour);
}

void AnnotationOverlay::setLineWidth(double width)
{
    setSelectedLineWidth(width);
    if (qFuzzyCompare(m_lineWidth, width)) {
        return;
    }
    m_lineWidth = width;
    Q_EMIT lineWidthChanged(m_lineWidth);
}

void AnnotationOverlay::setAuthor(const QString &author)
{
    m_author = author;
}

// ── What the document already says ────────────────────────────────────────

const QVector<Annotation> &AnnotationOverlay::commentsOf(int sourceId)
{
    const auto cached = m_bySource.constFind(sourceId);
    if (cached != m_bySource.constEnd()) {
        return cached.value();
    }

    // A whole file at a time, although only one page asked: reading one page
    // means opening the document and walking its page tree, so a file read once
    // per page would be read two thousand times for a book.
    QVector<Annotation> found;
    if (m_document) {
        if (const Source *source = m_document->source(sourceId)) {
            found = Annotations::read(source->path(), {}, nullptr);
        }
    }
    return *m_bySource.insert(sourceId, found);
}

const QVector<Annotation> &AnnotationOverlay::annotationsOf(int row)
{
    const auto cached = m_byRow.constFind(row);
    if (cached != m_byRow.constEnd()) {
        return cached.value();
    }

    QVector<Annotation> found;
    if (m_document && row >= 0 && row < m_document->pageCount()) {
        const PageRef ref = m_document->pageAt(row);
        const QSizeF shown = m_document->pageSizePoints(row);
        for (const Annotation &annotation : commentsOf(ref.sourceId)) {
            if (annotation.page != ref.sourcePage) {
                continue;
            }
            Annotation moved = annotation;
            moved.page = row;
            applyRotation(moved, ref.rotation, shown);
            found.append(moved);
        }
    }
    return *m_byRow.insert(row, found);
}

QHash<int, QVector<Annotation>> AnnotationOverlay::annotationsByRow() const
{
    QHash<int, QVector<Annotation>> all;
    for (auto page = m_byRow.constBegin(); page != m_byRow.constEnd(); ++page) {
        // A row that was merely looked at says nothing; a row that was emptied
        // has to be listed, or whoever writes the file would leave the comments
        // the user deleted exactly where they were.
        if (page.value().isEmpty() && !m_touched.contains(page.key())) {
            continue;
        }
        all.insert(page.key(), page.value());
    }
    return all;
}

void AnnotationOverlay::reload()
{
    m_drawing = false;
    m_moving = false;
    m_path.clear();
    hideNote();
    deselect();

    m_bySource.clear();
    m_byRow.clear();
    m_rows.setDocument(m_document);

    const bool had = !m_touched.isEmpty();
    m_touched.clear();
    if (had) {
        Q_EMIT annotationsChanged();
    }
    repaint();
}

void AnnotationOverlay::rebaseRows()
{
    carryRows(m_rows.follow());
}

void AnnotationOverlay::refreshRows()
{
    carryRows(m_rows.followInPlace());
}

void AnnotationOverlay::carryRows(const Rebase &change)
{
    if (change.isIdentity()) {
        return;
    }

    // A mark half drawn is a mark on a page that has just changed shape under
    // the hand drawing it, and the note on screen is anchored to a row that may
    // be somewhere else now.
    m_drawing = false;
    m_moving = false;
    m_path.clear();
    hideNote();

    QHash<int, QVector<Annotation>> byRow;
    QSet<int> touched;
    int lost = 0;

    for (auto page = m_byRow.constBegin(); page != m_byRow.constEnd(); ++page) {
        const int row = change.nowAt(page.key());
        if (row < 0) {
            // The page it was made on is not in the document any more, so there
            // is nothing left to hold it against. Said out loud, because a
            // remark that disappears without a word looks like a program that
            // lost it rather than a page the user deleted.
            if (m_touched.contains(page.key())) {
                lost += int(page.value().size());
            }
            continue;
        }

        const bool waiting = m_touched.contains(page.key());
        if (change.rewritten(page.key()) && !waiting) {
            continue; // Read from a file this row no longer stands for; read it again.
        }

        QVector<Annotation> here = page.value();
        const int turn = change.turnOf(page.key());
        for (Annotation &annotation : here) {
            annotation.page = row;
            // Comments read out of the file were mapped through the turn the row
            // had; ones the user drew were made in it. Both belong to what is
            // printed on the page, so both turn with it.
            applyRotation(annotation, turn, m_document ? m_document->pageSizePoints(row) : QSizeF());
        }
        byRow.insert(row, here);
        if (waiting) {
            touched.insert(row);
        }
    }

    const bool had = !m_touched.isEmpty();
    m_byRow = byRow;
    m_touched = touched;

    if (m_selectedRow >= 0) {
        const int row = change.nowAt(m_selectedRow);
        if (row < 0) {
            deselect();
        } else {
            m_selectedRow = row;
        }
    }
    m_dragRow = m_dragRow >= 0 ? change.nowAt(m_dragRow) : -1;

    if (lost > 0) {
        Q_EMIT refused(i18ncp("@info %1 is a number of comments the user had made",
                              "One comment was on a page that has been taken out of the document, so it is gone.",
                              "%1 comments were on pages that have been taken out of the document, so they are gone.",
                              lost));
    }
    if (had != !m_touched.isEmpty()) {
        Q_EMIT annotationsChanged();
    }
    repaint();
}

// ── Drawing ───────────────────────────────────────────────────────────────

bool AnnotationOverlay::appliesTo(PageView::Mode mode) const
{
    Q_UNUSED(mode)
    // Both: a reader shows comments, and a document handed over in reading mode
    // with its markup missing would look like a document nobody had read.
    return true;
}

void AnnotationOverlay::paint(QPainter &painter, int row, const QRect &pageRect)
{
    Q_UNUSED(pageRect)
    if (!m_view) {
        return;
    }

    const QVector<Annotation> here = annotationsOf(row);
    for (int i = 0; i < here.size(); ++i) {
        drawAnnotation(painter, row, here.at(i), row == m_selectedRow && i == m_selected);
    }
    if (m_drawing && row == m_dragRow) {
        drawPending(painter, row);
    }
}

void AnnotationOverlay::drawAnnotation(QPainter &painter, int row, const Annotation &annotation, bool selected)
{
    const auto boxOf = [this, row](const QRectF &points) { return m_view->fromPoints(row, points.normalized()); };
    const auto pointAt = [this, row](const QPointF &points) { return m_view->fromPoints(row, points); };

    // Line widths are given in points; on screen they have to follow the zoom or
    // a hairline looks fat on a small page and disappears on a large one.
    const double scale = m_view->zoom();
    const double pen = std::max(1.0, annotation.lineWidth * scale);

    // A markup annotation read from a file that gave no quads still has its own
    // rectangle, and drawing nothing at all would be the wrong way to say so.
    const QVector<QRectF> quads = annotation.quads.isEmpty() ? QVector<QRectF> { annotation.rect } : annotation.quads;

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setOpacity(annotation.opacity);

    switch (annotation.type) {
    case Annotation::Type::Highlight:
        // Multiply, as the engine does, so black text under yellow stays black
        // instead of turning grey.
        painter.setCompositionMode(QPainter::CompositionMode_Multiply);
        for (const QRectF &quad : quads) {
            painter.fillRect(boxOf(quad), annotation.colour);
        }
        painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
        break;

    case Annotation::Type::Underline:
    case Annotation::Type::StrikeOut:
        for (const QRectF &quad : quads) {
            const QRectF box = boxOf(quad);
            const double thickness = std::max(1.0, box.height() / 14.0);
            // The engine measures both from the foot of the marked box, which on
            // screen is the larger y.
            const double up = annotation.type == Annotation::Type::Underline ? 0.06 : 0.42;
            painter.fillRect(QRectF(box.left(), box.bottom() - box.height() * up - thickness, box.width(), thickness),
                             annotation.colour);
        }
        break;

    case Annotation::Type::Squiggly:
        for (const QRectF &quad : quads) {
            const QRectF box = boxOf(quad);
            const double amplitude = std::max(1.0, box.height() / 12.0);
            const double baseline = box.bottom() - amplitude;
            QPen ink(annotation.colour, std::max(0.5, amplitude / 2.0));
            ink.setCapStyle(Qt::RoundCap);
            ink.setJoinStyle(Qt::RoundJoin);
            painter.setPen(ink);

            QPolygonF wave;
            wave.append(QPointF(box.left(), baseline));
            bool up = true;
            for (double x = box.left() + amplitude * 2.0; x < box.right(); x += amplitude * 2.0) {
                wave.append(QPointF(x, baseline + (up ? -amplitude : amplitude)));
                up = !up;
            }
            painter.drawPolyline(wave);
        }
        break;

    case Annotation::Type::Ink:
    case Annotation::Type::Line: {
        QPen ink(annotation.colour, pen);
        ink.setCapStyle(Qt::RoundCap);
        ink.setJoinStyle(Qt::RoundJoin);
        painter.setPen(ink);
        for (const QVector<QPointF> &stroke : annotation.strokes) {
            if (stroke.size() == 1) {
                // A single tap still deserves a mark, as it does in the file.
                painter.setBrush(annotation.colour);
                painter.drawEllipse(pointAt(stroke.constFirst()), pen / 2.0, pen / 2.0);
                painter.setBrush(Qt::NoBrush);
                continue;
            }
            QPolygonF path;
            for (const QPointF &point : stroke) {
                path.append(pointAt(point));
            }
            painter.drawPolyline(path);
        }
        break;
    }

    case Annotation::Type::Square:
    case Annotation::Type::Circle: {
        painter.setPen(QPen(annotation.colour, pen));
        painter.setBrush(annotation.interior.isValid() ? QBrush(annotation.interior) : QBrush(Qt::NoBrush));
        // Inset by half the pen, as the engine insets it, or the stroke hangs
        // outside the rectangle the user drew and the shape looks bigger.
        const QRectF box = boxOf(annotation.rect).adjusted(pen / 2.0, pen / 2.0, -pen / 2.0, -pen / 2.0);
        if (annotation.type == Annotation::Type::Circle) {
            painter.drawEllipse(box);
        } else {
            painter.drawRect(box);
        }
        break;
    }

    case Annotation::Type::FreeText: {
        const QRectF box = boxOf(annotation.rect);
        painter.setPen(QPen(annotation.colour, 1));
        painter.setBrush(annotation.interior.isValid() ? QBrush(annotation.interior) : QBrush(Qt::white));
        painter.drawRect(box);

        QFont font = painter.font();
        font.setPixelSize(std::max(4, int(std::lround(annotation.fontSize * scale))));
        painter.setFont(font);
        // Black, because the engine sets this text in black; the colour is the
        // box's own and using it for both would make an unreadable pairing easy.
        painter.setPen(Qt::black);
        painter.drawText(box.adjusted(3, 2, -3, -2), Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap,
                         annotation.contents);
        break;
    }

    case Annotation::Type::Note: {
        const QRectF box = boxOf(annotation.rect);
        painter.setPen(Qt::NoPen);
        painter.setBrush(annotation.colour);
        painter.drawRect(QRectF(box.left(), box.top(), box.width(), box.height() * 0.75));
        // The little tail that makes it read as a speech bubble rather than as a
        // coloured square somebody left behind.
        painter.drawPolygon(QPolygonF({ QPointF(box.left() + box.width() * 0.2, box.top() + box.height() * 0.7),
                                        QPointF(box.left() + box.width() * 0.2, box.bottom()),
                                        QPointF(box.left() + box.width() * 0.45, box.top() + box.height() * 0.7) }));

        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(Qt::white, std::max(1.0, box.height() / 14.0)));
        for (int line = 0; line < 3; ++line) {
            const double y = box.bottom() - box.height() * (0.42 + line * 0.19);
            painter.drawLine(QPointF(box.left() + box.width() * 0.18, y), QPointF(box.right() - box.width() * 0.18, y));
        }
        break;
    }
    }

    if (selected) {
        painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
        painter.setOpacity(1.0);
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(m_view->palette().color(QPalette::Highlight), 2, Qt::DashLine));
        painter.drawRect(boxOf(boundsOf(annotation)).adjusted(-2, -2, 2, 2));
    }
    painter.restore();
}

void AnnotationOverlay::drawPending(QPainter &painter, int row)
{
    const QRectF band = m_view->fromPoints(row, QRectF(m_from, m_to).normalized());
    const double pen = std::max(1.0, m_lineWidth * m_view->zoom());

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    // Not solid: what is being dragged out is a proposal, and it should not be
    // mistaken for a mark that is already on the page.
    painter.setOpacity(0.7);
    painter.setBrush(Qt::NoBrush);

    switch (m_tool) {
    case Tool::Highlight:
    case Tool::Underline:
    case Tool::StrikeOut:
        painter.fillRect(band, m_colour);
        break;

    case Tool::Freehand: {
        QPen ink(m_colour, pen);
        ink.setCapStyle(Qt::RoundCap);
        ink.setJoinStyle(Qt::RoundJoin);
        painter.setPen(ink);
        QPolygonF path;
        for (const QPointF &point : m_path) {
            path.append(m_view->fromPoints(row, point));
        }
        painter.drawPolyline(path);
        break;
    }

    case Tool::Line:
        painter.setPen(QPen(m_colour, pen));
        painter.drawLine(m_view->fromPoints(row, m_from), m_view->fromPoints(row, m_to));
        break;

    case Tool::Ellipse:
        painter.setPen(QPen(m_colour, pen, Qt::DashLine));
        painter.drawEllipse(band);
        break;

    default:
        painter.setPen(QPen(m_colour, pen, Qt::DashLine));
        painter.drawRect(band);
        break;
    }
    painter.restore();
}

// ── The mouse ─────────────────────────────────────────────────────────────

int AnnotationOverlay::annotationAt(int row, const QPointF &pagePoint)
{
    const QVector<Annotation> here = annotationsOf(row);
    // Backwards, so the comment drawn last (the one on top) is the one hit.
    for (int i = int(here.size()) - 1; i >= 0; --i) {
        const QRectF bounds = boundsOf(here.at(i));
        if (!bounds.isNull()
            && bounds.adjusted(-SlackPoints, -SlackPoints, SlackPoints, SlackPoints).contains(pagePoint)) {
            return i;
        }
    }
    return -1;
}

QPointF AnnotationOverlay::clampToPage(int row, const QPointF &pagePoint) const
{
    if (!m_document || row < 0 || row >= m_document->pageCount()) {
        return pagePoint;
    }
    const QSizeF size = m_document->pageSizePoints(row);
    if (size.isEmpty()) {
        return pagePoint;
    }
    return QPointF(std::clamp(pagePoint.x(), 0.0, size.width()), std::clamp(pagePoint.y(), 0.0, size.height()));
}

bool AnnotationOverlay::press(int row, const QPointF &pagePoint, Qt::MouseButtons buttons)
{
    if (!(buttons & Qt::LeftButton) || !m_view) {
        return false;
    }
    hideNote();

    const int hit = annotationAt(row, pagePoint);

    if (m_view->mode() != PageView::Mode::Edit) {
        // The reader's mouse stays the reader's: the press is not taken, so
        // dragging over a highlighted line still selects the words underneath,
        // which is why anybody highlighted them. A click says which comment the
        // panel should describe, and a note gives up what it says.
        select(hit >= 0 ? row : -1, hit);
        showNoteFor(row, hit);
        return false;
    }

    if (m_tool == Tool::Select) {
        select(hit >= 0 ? row : -1, hit);
        if (hit < 0) {
            return false; // Bare paper belongs to whatever bands a selection on it.
        }
        showNoteFor(row, hit);
        m_moving = true;
        m_moved = false;
        m_dragRow = row;
        m_from = pagePoint;
        return true;
    }

    if (m_tool == Tool::Note) {
        // A note is a marker of a fixed size, so a click is the whole gesture;
        // dragging one out would only make it an odd size.
        Annotation note = newAnnotation(Annotation::Type::Note);
        const QPointF corner = clampToPage(row, pagePoint);
        note.rect = QRectF(corner.x(), corner.y() - NoteSize, NoteSize, NoteSize);
        addAnnotation(row, note);
        return true;
    }

    m_drawing = true;
    m_dragRow = row;
    m_from = m_to = clampToPage(row, pagePoint);
    m_path = { m_from };
    return true;
}

bool AnnotationOverlay::move(int row, const QPointF &pagePoint, Qt::MouseButtons buttons)
{
    Q_UNUSED(buttons)
    if (row != m_dragRow) {
        return false;
    }

    if (m_moving) {
        Annotation *comment = selectedComment();
        if (!comment) {
            m_moving = false;
            return false;
        }
        // What a note said belongs beside the note, and the note is on its way
        // somewhere else.
        hideNote();
        // Not clamped: a comment is allowed to hang over the edge of the page it
        // is about, and stopping it dead at the margin would feel like a fault.
        translateBy(*comment, pagePoint - m_from);
        m_from = pagePoint;
        m_moved = true;
        m_touched.insert(m_dragRow);
        repaint();
        return true;
    }

    if (!m_drawing) {
        return false;
    }

    m_to = clampToPage(row, pagePoint);
    if (m_tool == Tool::Freehand) {
        // Thinning the path keeps the stored ink small without a visible
        // difference; every mouse move would otherwise store hundreds of points
        // a second, all of which end up in the file.
        const QPointF last = m_path.isEmpty() ? m_from : m_path.constLast();
        if (std::hypot(m_to.x() - last.x(), m_to.y() - last.y()) > InkStepPoints) {
            m_path.append(m_to);
        }
    }
    repaint();
    return true;
}

bool AnnotationOverlay::release(int row, const QPointF &pagePoint)
{
    Q_UNUSED(row)
    Q_UNUSED(pagePoint)

    if (m_moving) {
        m_moving = false;
        // One change per gesture rather than one per mouse move: whoever is
        // listening is keeping a list up to date, not following the pointer. A
        // click that picked something up and put it down again moved nothing and
        // is not announced at all.
        if (std::exchange(m_moved, false)) {
            Q_EMIT annotationsChanged();
        }
        return true;
    }
    if (!m_drawing) {
        return false;
    }

    m_drawing = false;
    const Annotation drawn = finishDrawing();
    m_path.clear();

    if (boundsOf(drawn).isNull()) {
        repaint(); // A click that slipped: take the proposal off the page again.
        return true;
    }
    addAnnotation(m_dragRow, drawn);
    return true;
}

Annotation AnnotationOverlay::finishDrawing() const
{
    const double across = std::abs(m_to.x() - m_from.x());
    const double down = std::abs(m_to.y() - m_from.y());
    if (m_tool == Tool::Freehand ? m_path.size() < 2 : std::max(across, down) < MinimumDragPoints) {
        return {};
    }

    Annotation drawn = newAnnotation(typeFor(m_tool));
    QRectF band = QRectF(m_from, m_to).normalized();

    switch (m_tool) {
    case Tool::Highlight:
    case Tool::Underline:
    case Tool::StrikeOut:
        // A line of text is marked by dragging along it, and a steady hand would
        // otherwise produce a band with no height and nothing to see. It is grown
        // about the line the drag followed rather than refused.
        if (band.height() < MinimumMarkupHeight) {
            const double middle = band.center().y();
            band.setTop(middle - MinimumMarkupHeight / 2.0);
            band.setBottom(middle + MinimumMarkupHeight / 2.0);
        }
        drawn.quads = { band };
        drawn.rect = band;
        break;

    case Tool::Freehand:
        drawn.strokes = { m_path };
        break;

    case Tool::Line:
        // The two ends the drag actually had, not the corners of the box round
        // them: a line dragged up to the right is not the same as one dragged
        // down to the right.
        drawn.strokes = { { m_from, m_to } };
        break;

    case Tool::TextBox:
        drawn.rect = band;
        drawn.interior = QColor(255, 255, 255);
        break;

    default:
        drawn.rect = band;
        break;
    }
    return drawn;
}

Qt::CursorShape AnnotationOverlay::cursor(int row, const QPointF &pagePoint)
{
    if (!m_view) {
        return Qt::ArrowCursor;
    }
    if (m_view->mode() == PageView::Mode::Edit && m_tool != Tool::Select) {
        // The tool is armed anywhere on the page, and saying so before the press
        // is what stops somebody trying to select a word with a highlighter.
        return Qt::CrossCursor;
    }
    if (annotationAt(row, pagePoint) < 0) {
        return Qt::ArrowCursor;
    }
    return m_view->mode() == PageView::Mode::Edit ? Qt::SizeAllCursor : Qt::PointingHandCursor;
}

// ── The comments themselves ───────────────────────────────────────────────

Annotation AnnotationOverlay::newAnnotation(Annotation::Type type) const
{
    Annotation made;
    made.type = type;
    made.colour = m_colour;
    made.lineWidth = m_lineWidth;
    made.author = m_author;
    made.created = QDateTime::currentDateTime();
    return made;
}

void AnnotationOverlay::addAnnotation(int row, const Annotation &annotation)
{
    annotationsOf(row); // The file's own comments come first, or they would be lost.

    Annotation placed = annotation;
    placed.page = row;
    m_byRow[row].append(placed);
    m_touched.insert(row);

    select(row, int(m_byRow.value(row).size()) - 1);
    Q_EMIT annotationsChanged();
    repaint();
}

Annotation *AnnotationOverlay::selectedComment()
{
    if (m_selectedRow < 0 || m_selected < 0) {
        return nullptr;
    }
    const auto page = m_byRow.find(m_selectedRow);
    if (page == m_byRow.end() || m_selected >= page->size()) {
        return nullptr;
    }
    return &(*page)[m_selected];
}

Annotation AnnotationOverlay::selectedAnnotation() const
{
    if (m_selectedRow < 0 || m_selected < 0) {
        return {};
    }
    return m_byRow.value(m_selectedRow).value(m_selected);
}

void AnnotationOverlay::recordChange(int row)
{
    m_touched.insert(row);
    Q_EMIT annotationsChanged();
    repaint();
}

void AnnotationOverlay::select(int row, int index)
{
    const int wanted = index >= 0 ? row : -1;
    if (wanted == m_selectedRow && index == m_selected) {
        return;
    }
    m_selectedRow = wanted;
    m_selected = index;

    // A fresh one, so that the panel notices it is describing something else and
    // rebuilds. Changing an attribute keeps this object, which is what stops the
    // panel from rebuilding under the control being used.
    if (index >= 0) {
        m_details = std::make_unique<Details>(this);
    } else {
        m_details.reset();
    }
    repaint();
    Q_EMIT selectionChanged();
}

void AnnotationOverlay::selectComment(int row, int index)
{
    if (!m_document || row < 0 || row >= m_document->pageCount() || index < 0 || index >= annotationsOf(row).size()) {
        select(-1, -1);
        return;
    }
    select(row, index);
}

void AnnotationOverlay::deselect()
{
    hideNote();
    select(-1, -1);
}

void AnnotationOverlay::removeSelected()
{
    if (!m_view || m_view->mode() != PageView::Mode::Edit) {
        return; // Reading mode shows comments; it does not take them away.
    }
    const auto page = m_byRow.find(m_selectedRow);
    if (page == m_byRow.end() || m_selected < 0 || m_selected >= page->size()) {
        return;
    }
    const int row = m_selectedRow;
    page->remove(m_selected);

    deselect();
    recordChange(row);
}

Inspectable *AnnotationOverlay::inspected()
{
    return m_details.get();
}

// ── Attributes, as the properties panel changes them ──────────────────────

void AnnotationOverlay::setSelectedContents(const QString &text)
{
    Annotation *comment = selectedComment();
    if (!comment || comment->contents == text) {
        return;
    }
    comment->contents = text;
    recordChange(m_selectedRow);
}

void AnnotationOverlay::setSelectedAuthor(const QString &author)
{
    Annotation *comment = selectedComment();
    if (!comment || comment->author == author) {
        return;
    }
    comment->author = author;
    recordChange(m_selectedRow);
}

void AnnotationOverlay::setSelectedColour(const QColor &colour)
{
    Annotation *comment = selectedComment();
    if (!comment || !colour.isValid() || comment->colour == colour) {
        return;
    }
    comment->colour = colour;
    recordChange(m_selectedRow);
}

void AnnotationOverlay::setSelectedLineWidth(double width)
{
    Annotation *comment = selectedComment();
    if (!comment || qFuzzyCompare(comment->lineWidth, width)) {
        return;
    }
    comment->lineWidth = width;
    recordChange(m_selectedRow);
}

void AnnotationOverlay::setSelectedType(Annotation::Type type)
{
    Annotation *comment = selectedComment();
    if (!comment || comment->type == type || !canBecome(*comment, type)) {
        return;
    }
    convertTo(*comment, type);
    recordChange(m_selectedRow);
}

void AnnotationOverlay::moveSelectionTo(int row)
{
    if (!m_document || row < 0 || row >= m_document->pageCount() || m_selectedRow < 0 || row == m_selectedRow) {
        return;
    }
    const Annotation *comment = selectedComment();
    if (!comment) {
        return;
    }
    Annotation carried = *comment;

    // The pages of one document are not all the same size, so a comment that
    // would land off the edge of its new page is brought back onto it rather
    // than left somewhere nobody can see it.
    const QSizeF size = m_document->pageSizePoints(row);
    const QRectF bounds = boundsOf(carried);
    if (!bounds.isNull() && !size.isEmpty()) {
        const double dx = std::min(0.0, size.width() - bounds.right()) - std::min(0.0, bounds.left());
        const double dy = std::min(0.0, size.height() - bounds.bottom()) - std::min(0.0, bounds.top());
        if (!qFuzzyIsNull(dx) || !qFuzzyIsNull(dy)) {
            translateBy(carried, QPointF(dx, dy));
        }
    }
    carried.page = row;

    const int from = m_selectedRow;
    m_byRow[from].remove(m_selected);
    annotationsOf(row);
    m_byRow[row].append(carried);

    // The same Details object on purpose: the panel's own page box is what asked
    // for this, and rebuilding the panel would take that box out from under the
    // finger still on it.
    m_selectedRow = row;
    m_selected = int(m_byRow.value(row).size()) - 1;
    m_touched.insert(from);
    m_touched.insert(row);

    hideNote();
    if (m_view) {
        // The comment has gone somewhere else, and a page the user cannot see is
        // no place to put something they have just moved.
        m_view->goToPage(row);
    }
    Q_EMIT annotationsChanged();
    repaint();
}

// ── What a note says ──────────────────────────────────────────────────────

void AnnotationOverlay::showNoteFor(int row, int index)
{
    const Annotation comment = annotationsOf(row).value(index);
    if (index < 0 || comment.type != Annotation::Type::Note) {
        // Everything else already shows what it is about on the page: a text box
        // draws its own words, and a highlight is the words it lies over.
        hideNote();
        return;
    }

    m_noteRow = row;
    m_noteAnchor = boundsOf(comment);
    m_noteText = comment.contents.isEmpty() ? i18n("This note has nothing written in it yet.") : comment.contents;
    placeNote();
}

void AnnotationOverlay::placeNote()
{
    if (m_noteRow < 0 || m_noteText.isEmpty() || !m_view) {
        return;
    }

    if (!m_note) {
        m_note = new QLabel(m_view->viewport());
        m_note->setWordWrap(true);
        m_note->setFrameShape(QFrame::StyledPanel);
        m_note->setMargin(6);
        m_note->setAutoFillBackground(true);
        m_note->setTextInteractionFlags(Qt::TextSelectableByMouse);

        // A tooltip's colours rather than the window's, because this is a remark
        // about the page and not part of it.
        QPalette palette = m_note->palette();
        palette.setColor(QPalette::Window, palette.color(QPalette::ToolTipBase));
        palette.setColor(QPalette::WindowText, palette.color(QPalette::ToolTipText));
        m_note->setPalette(palette);
    }

    const QRect anchor = m_view->fromPoints(m_noteRow, m_noteAnchor).toAlignedRect();
    const QRect viewport = m_view->viewport()->rect();
    if (!viewport.intersects(anchor)) {
        m_note->hide(); // Scrolled away, and it belongs beside its marker.
        return;
    }

    const int width = std::min(340, std::max(140, viewport.width() - 16));
    m_note->setText(m_noteText);
    m_note->resize(width, m_note->heightForWidth(width));

    // Under the marker it belongs to, unless that would put it off the bottom of
    // the window, where nobody would read it.
    QRect box = m_note->geometry();
    box.moveTopLeft(QPoint(anchor.left(), anchor.bottom() + 6));
    if (box.bottom() > viewport.bottom()) {
        box.moveBottom(anchor.top() - 6);
    }
    box.moveLeft(
        std::clamp(box.left(), viewport.left() + 8, std::max(viewport.left() + 8, viewport.right() - width - 8)));
    box.moveTop(std::max(viewport.top() + 8, box.top()));
    m_note->setGeometry(box);
    m_note->show();
    m_note->raise();
}

void AnnotationOverlay::hideNote()
{
    m_noteRow = -1;
    m_noteText.clear();
    if (m_note) {
        m_note->hide();
    }
}

void AnnotationOverlay::repaint()
{
    if (m_view) {
        m_view->viewport()->update();
    }
}

// ── The keys the view does not hand on ────────────────────────────────────

bool AnnotationOverlay::eventFilter(QObject *watched, QEvent *event)
{
    if (!m_view) {
        return QObject::eventFilter(watched, event);
    }

    if (watched == m_view->viewport() && event->type() == QEvent::Resize) {
        // Resizing re-centres the pages, and a note being read is not one of the
        // things the view moves when it does.
        placeNote();
        return QObject::eventFilter(watched, event);
    }

    if (watched != m_view || event->type() != QEvent::KeyPress) {
        return QObject::eventFilter(watched, event);
    }

    auto *key = static_cast<QKeyEvent *>(event);
    if (key->key() == Qt::Key_Escape && (m_selectedRow >= 0 || m_drawing || m_noteRow >= 0)) {
        m_drawing = false;
        m_path.clear();
        deselect();
        return true;
    }
    if ((key->key() == Qt::Key_Delete || key->key() == Qt::Key_Backspace) && m_selectedRow >= 0
        && m_view->mode() == PageView::Mode::Edit) {
        removeSelected();
        return true;
    }
    return QObject::eventFilter(watched, event);
}

} // namespace ps
