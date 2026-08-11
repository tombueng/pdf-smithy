/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "FormOverlay.h"

#include <KLocalizedString>

#include <QComboBox>
#include <QEvent>
#include <QFontMetricsF>
#include <QFormLayout>
#include <QHelpEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPolygonF>
#include <QScrollBar>
#include <QSet>
#include <QToolTip>
#include <QWidget>

#include <algorithm>
#include <cmath>

using namespace Qt::Literals::StringLiterals;

namespace ps {

namespace {

/** How far a field is kept from the window's edge when Tab jumps to it. */
constexpr int ScrollMargin = 28;

/** How far from a field a word may sit and still be its label, in points. */
constexpr double LabelReach = 150.0;

/** As many words as a label ever is. Past that it is a sentence about it. */
constexpr int LabelWords = 8;

/**
 * The wash that says "this one is yours to fill in".
 *
 * Every reader people already know marks fillable fields with a pale blue, and a
 * form whose boxes are invisible until the pointer is over them is a form people
 * fill in on paper instead. It is multiplied onto the page rather than painted
 * over it, the way a highlighter works: white paper takes the tint while the
 * printed rules, the shading and whatever value the file already carries all
 * keep their contrast underneath.
 */
QColor fillableWash()
{
    return QColor(202, 223, 247);
}

/** The same wash a shade deeper, for the field (or the group) under the pointer. */
QColor hoveredWash()
{
    return QColor(172, 203, 240);
}

/** What Forms::fill() understands as a tick and as no tick. */
QString onValue()
{
    return u"on"_s;
}

QString offValue()
{
    return u"off"_s;
}

/** The state a tick box arrives in, before anyone touches it. */
bool wasTicked(const FormField &field)
{
    return !field.value.isEmpty() && field.value != "/Off"_L1;
}

void drawTick(QPainter &painter, const QRectF &box)
{
    const double inset = std::max(1.0, std::min(box.width(), box.height()) * 0.22);
    const QRectF inner = box.adjusted(inset, inset, -inset, -inset);
    if (inner.isEmpty()) {
        return;
    }
    painter.setPen(
        QPen(QColor(20, 20, 20), std::max(1.2, inner.height() * 0.2), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);
    painter.drawPolyline(QPolygonF({ QPointF(inner.left(), inner.center().y()),
                                     QPointF(inner.left() + inner.width() * 0.35, inner.bottom()),
                                     QPointF(inner.right(), inner.top()) }));
}

void drawDot(QPainter &painter, const QRectF &box)
{
    const double radius = std::min(box.width(), box.height()) * 0.26;
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(20, 20, 20));
    painter.drawEllipse(box.center(), radius, radius);
}

/**
 * The words next to @p box that read as its label, or nothing at all.
 *
 * To the right first and then to the left, because a tick box is printed
 * "☐ Einverstanden" far more often than the other way round, while the caption
 * of a text field is nearly always in front of it, so the side that answers
 * first is the side that is right for the fields that need this most.
 *
 * @p obstacles are the other widgets on the page, and the run of words stops at
 * the nearest of them: a row of radio buttons would otherwise hand its first
 * button the words of all three choices, which is worse than no label.
 *
 * Everything here is in display points on the page, origin at the bottom left,
 * which is what both FormField::Widget::rect and RenderBackend::Word::rect are
 * measured in.
 */
QString wordsBeside(const QVector<RenderBackend::Word> &words, const QRectF &box, const QVector<QRectF> &obstacles)
{
    if (box.isEmpty() || words.isEmpty()) {
        return {};
    }

    // A word is on this line when its middle is within the box's own height of
    // the box's middle. Measuring middles rather than overlaps is what lets a
    // twenty-point tick box and an eight-point caption still find each other.
    const double middle = box.center().y();
    const double band = std::max(box.height(), 6.0);
    const auto onThisLine = [middle, band](const QRectF &rect) {
        return rect.center().y() > middle - band && rect.center().y() < middle + band;
    };

    // Where somebody else's label starts. A widget to the right of this one on
    // the same line owns everything from its own left edge onwards.
    double stopRight = box.right() + LabelReach;
    double stopLeft = box.left() - LabelReach;
    for (const QRectF &other : obstacles) {
        if (!onThisLine(other)) {
            continue;
        }
        if (other.left() >= box.right()) {
            stopRight = std::min(stopRight, other.left());
        }
        if (other.right() <= box.left()) {
            stopLeft = std::max(stopLeft, other.right());
        }
    }

    QVector<const RenderBackend::Word *> line;
    for (const RenderBackend::Word &word : words) {
        if (!word.text.trimmed().isEmpty() && onThisLine(word.rect)) {
            line.append(&word);
        }
    }
    std::sort(line.begin(), line.end(), [](const RenderBackend::Word *a, const RenderBackend::Word *b) {
        return a->rect.left() < b->rect.left();
    });

    // Two distances, because the space between a box and its label is a design
    // decision and the space between two words of that label is typography. One
    // limit for both would either skip the label or run into the next column.
    const double firstGap = std::max(band * 3.0, 36.0);
    const double wordGap = std::max(band * 1.2, 14.0);

    QStringList text;
    double edge = box.right();
    for (const RenderBackend::Word *word : std::as_const(line)) {
        if (word->rect.left() < box.right() - 0.5) {
            continue; // Beside it or behind it, not after it.
        }
        if (word->rect.right() > stopRight + 0.5 || word->rect.left() - edge > (text.isEmpty() ? firstGap : wordGap)) {
            break;
        }
        text << word->text.trimmed();
        edge = word->rect.right();
        if (text.size() >= LabelWords) {
            break;
        }
    }

    if (text.isEmpty()) {
        edge = box.left();
        for (int i = line.size() - 1; i >= 0; --i) {
            const RenderBackend::Word *word = line.at(i);
            if (word->rect.right() > box.left() + 0.5) {
                continue;
            }
            if (word->rect.left() < stopLeft - 0.5
                || edge - word->rect.right() > (text.isEmpty() ? firstGap : wordGap)) {
                break;
            }
            text.prepend(word->text.trimmed()); // Read leftwards, kept in reading order.
            edge = word->rect.left();
            if (text.size() >= LabelWords) {
                break;
            }
        }
    }

    const QString guess = text.join(QLatin1Char(' ')).simplified();

    // A rule, a bullet or a row of dots is next to a great many fields and names
    // none of them, and a label of punctuation would look like a bug.
    const bool sayable
        = std::any_of(guess.cbegin(), guess.cend(), [](QChar character) { return character.isLetterOrNumber(); });
    return sayable ? guess : QString();
}

} // namespace

FormOverlay::FormOverlay(PageView *view, QObject *parent)
    : QObject(parent)
    , m_view(view)
{
    if (!m_view) {
        return;
    }

    // The editor is a plain child widget of the viewport, so every change to
    // where the page is drawn has to be answered by moving it. Scrolling is the
    // one that happens constantly and the one the view has no signal for, so it
    // is taken from the scroll bars themselves.
    connect(m_view, &PageView::zoomChanged, this, [this] { placeEditor(); });
    connect(m_view, &PageView::currentPageChanged, this, [this] { placeEditor(); });
    connect(m_view, &PageView::layoutChanged, this, [this] { placeEditor(); });
    connect(m_view->verticalScrollBar(), &QScrollBar::valueChanged, this, [this] { placeEditor(); });
    connect(m_view->horizontalScrollBar(), &QScrollBar::valueChanged, this, [this] { placeEditor(); });

    // Filling in is a reading-mode gesture; in Edit mode the boxes belong to
    // whoever is moving them about, and an editor left open over one would be
    // typing into a field that is no longer where it was.
    connect(m_view, &PageView::modeChanged, this, [this](PageView::Mode mode) {
        if (mode != PageView::Mode::View) {
            deselect();
        }
    });

    m_view->viewport()->installEventFilter(this);
    m_view->installEventFilter(this);
}

FormOverlay::~FormOverlay()
{
    // The view keeps raw pointers to its overlays, so leaving one behind would
    // be a crash on the next repaint rather than a leak.
    if (m_view) {
        m_view->removeOverlay(this);
    }
    if (m_editor) {
        delete m_editor;
    }
}

// ── What there is to fill in ──────────────────────────────────────────────

void FormOverlay::setDocument(Document *document)
{
    m_rows.setDocument(document);
    if (document) {
        // Moving a page is a removal followed by an insertion, so between the
        // two signals the page is briefly nowhere at all. Answering that state
        // would put every field of the document somewhere it is not, so both are
        // queued: whichever arrives first is answered once the list has settled,
        // and the second finds nothing left to do.
        connect(document, &Document::pagesInserted, this, &FormOverlay::rebaseRows, Qt::QueuedConnection);
        connect(document, &Document::pagesRemoved, this, &FormOverlay::rebaseRows, Qt::QueuedConnection);

        // A row that changed in place kept its number: it was turned, or an
        // operation put a freshly written file behind it. Nothing has moved, so
        // it is answered at once.
        connect(document, &Document::pagesChanged, this, &FormOverlay::refreshRows);
    }
    rebuildRows();
}

void FormOverlay::setFields(const QVector<FormField> &fields)
{
    deselect();
    m_fields = fields;
    m_edited.clear();

    // Set here and not worked out later: the form has just been read out of the
    // document's own file, which is the one instant at which page eight of that
    // file and row eight of the view are the same sheet. Everything afterwards
    // moves this map rather than assuming the two still agree.
    m_rowOfPage.clear();
    m_turnAtRead.clear();
    m_rows.resync();
    for (const FormField &field : m_fields) {
        for (const FormField::Widget &widget : field.widgets) {
            if (widget.page >= 0) {
                m_rowOfPage.insert(widget.page, widget.page);
                // The turn the boxes were measured against, taken at the same
                // instant and for the same reason: the file they came out of may
                // already have been written with a turn in it.
                m_turnAtRead.insert(widget.page, m_rows.rotationOf(widget.page));
            }
        }
    }

    rebuildRows();
    forgetGuesses();
}

void FormOverlay::rebaseRows()
{
    carryRows(m_rows.follow());
}

void FormOverlay::refreshRows()
{
    carryRows(m_rows.followInPlace());
}

void FormOverlay::carryRows(const Rebase &change)
{
    if (change.isIdentity()) {
        return;
    }
    for (auto page = m_rowOfPage.begin(); page != m_rowOfPage.end(); ++page) {
        page.value() = page.value() >= 0 ? change.nowAt(page.value()) : -1;
    }

    // What was read off a page belongs to the page that was read, and a row
    // pointed at a freshly written file is a different page underneath.
    forgetGuesses();
    rebuildRows();
}

void FormOverlay::rebuildRows()
{
    m_byRow.clear();

    for (int i = 0; i < m_fields.size(); ++i) {
        const FormField &field = m_fields.at(i);
        if (field.kind == FormField::Kind::Button) {
            continue; // Nothing anyone fills in.
        }
        for (int w = 0; w < field.widgets.size(); ++w) {
            const FormField::Widget &widget = field.widgets.at(w);
            if (widget.page < 0 || widget.rect.isEmpty()) {
                continue; // Nothing to draw: no page admits to holding it.
            }
            const int row = rowOfWidget(widget);
            if (row < 0) {
                continue; // The page it is printed on is no longer in the document.
            }
            m_byRow[row].append(Spot { i, w });
        }
    }

    rebuildOrder();

    // The field being filled in may have been on a page that has just gone, and
    // an editor left over a row that no longer holds it types into another page.
    if (m_selected >= 0 && boxOf(Spot { m_selected, m_selectedWidget }).isNull()) {
        deselect();
    } else {
        placeEditor();
    }

    if (m_view) {
        m_view->viewport()->update();
    }
}

int FormOverlay::rowOfWidget(const FormField::Widget &widget) const
{
    if (widget.page < 0) {
        return -1;
    }
    // Absent means the fields arrived without a document to convert through, in
    // which case the file's own numbering is all there is.
    return m_rowOfPage.value(widget.page, widget.page);
}

SourcePage FormOverlay::pageOfWidget(const FormField::Widget &widget) const
{
    const int row = rowOfWidget(widget);
    const SourcePage where = m_rows.pageOf(row);
    // No document: the one file the label source names, at the form's own page.
    return where.isValid() ? where : SourcePage(m_sourceId, widget.page);
}

QTransform FormOverlay::turnOf(int row, int page) const
{
    const int since = normalizeRotation(m_rows.rotationOf(row) - m_turnAtRead.value(page));

    // Answered before the page is measured, because measuring one is a question
    // for the rasteriser and on a document nobody has turned this is every call:
    // one per box, on every repaint and every movement of the mouse.
    return since == 0 ? QTransform() : pageTurn(since, m_rows.sizeOf(row));
}

int FormOverlay::rowOfField(int index) const
{
    if (index < 0 || index >= m_fields.size()) {
        return -1;
    }
    // The first widget a page actually lists, which is the one FormField::page
    // repeats: a field drawn on three sheets is on the first of them.
    for (const FormField::Widget &widget : m_fields.at(index).widgets) {
        if (widget.page >= 0) {
            return rowOfWidget(widget);
        }
    }
    return -1;
}

void FormOverlay::setLabelSource(RenderBackend *backend, int sourceId)
{
    m_backend = backend;
    m_sourceId = sourceId;
    forgetGuesses();
    if (m_view) {
        m_view->viewport()->update();
    }
}

void FormOverlay::forgetGuesses()
{
    m_guessed.clear();
    m_guessed.resize(m_fields.size());
    m_words.clear();
}

void FormOverlay::rebuildOrder()
{
    m_order.clear();

    // Tab follows the order the form declares its fields in, grouped by the row
    // they are shown at. That order is the author's own and is what the fields
    // were laid out in; sorting by position instead would look tidy and then
    // walk a two-column form across the gutter, which is exactly what people
    // complain about. By row rather than by the file's page, so that Tab runs
    // down the document the reader is looking at.
    QList<int> rows = m_byRow.keys();
    std::sort(rows.begin(), rows.end());
    QSet<int> seen;
    for (const int row : std::as_const(rows)) {
        for (const Spot &spot : m_byRow.value(row)) {
            const FormField &field = m_fields.at(spot.field);
            // A field that cannot take an answer is a dead stop in a tab run.
            if (field.readOnly || !isFillable(field) || seen.contains(spot.field)) {
                continue;
            }
            // One stop per field, not per widget: a radio group is one question
            // with one answer, and a field repeated at the head of every page is
            // one answer as well. Tab moves between questions; the arrow keys
            // and the mouse choose within a group, as they do in every other
            // form on the machine.
            seen.insert(spot.field);
            m_order.append(spot.field);
        }
    }
}

const QStringList &FormOverlay::guessesOf(int index) const
{
    static const QStringList nothing;
    if (index < 0 || index >= m_guessed.size()) {
        return nothing;
    }

    // Read off the page only for the fields somebody actually asks about, and
    // only once each. Pulling the words out of a page costs about what drawing
    // it costs, and a hundred-page form would spend that a hundred times over
    // for labels nobody ever looked at, while the window waited.
    QStringList &cached = m_guessed[index];
    const FormField &field = m_fields.at(index);
    if (!cached.isEmpty() || !m_backend || field.widgets.isEmpty()) {
        return cached;
    }

    for (const FormField::Widget &widget : field.widgets) {
        if (widget.page < 0 || widget.rect.isEmpty()) {
            cached << QString(); // Nowhere to look, and the entry has to line up.
            continue;
        }

        // Every widget on the page, this field's own included, because a label
        // runs up to the next box whichever field that box belongs to.
        QVector<QRectF> boxes;
        for (const FormField &other : std::as_const(m_fields)) {
            for (const FormField::Widget &box : other.widgets) {
                if (box.page == widget.page && !box.rect.isEmpty()) {
                    boxes.append(box.rect);
                }
            }
        }

        // The file this widget is printed in, which on a document assembled from
        // two of them is not the file the one beside it is printed in. Asking
        // the wrong document for the words beside a box is how every label on
        // half a merged form came out belonging to the other half.
        const SourcePage where = pageOfWidget(widget);
        if (!where.isValid()) {
            cached << QString();
            continue;
        }

        auto words = m_words.find(where);
        if (words == m_words.end()) {
            words = m_words.insert(where, m_backend->words(where.sourceId(), where.pageInFile()));
        }
        cached << wordsBeside(words.value(), widget.rect, boxes);
    }
    return cached;
}

FormOverlay::FieldLabel FormOverlay::labelOf(int index, int widget) const
{
    if (index < 0 || index >= m_fields.size()) {
        return {};
    }
    const FormField &field = m_fields.at(index);
    // Forms::read() answers with the field's name where the document states no
    // /TU, so a label equal to the name is the document saying nothing.
    if (!field.label.isEmpty() && field.label != field.name) {
        return { field.label, false };
    }

    const QString guess = guessesOf(index).value(widget);
    // Nothing beside it and nothing in the document: no label at all, which is
    // what is shown. An empty guess is not offered as a guess.
    return guess.isEmpty() ? FieldLabel {} : FieldLabel { guess, true };
}

bool FormOverlay::isFillable(const FormField &field)
{
    switch (field.kind) {
    case FormField::Kind::Text:
    case FormField::Kind::Checkbox:
    case FormField::Kind::Radio:
    case FormField::Kind::Choice:
        return true;
    case FormField::Kind::Button:
    case FormField::Kind::Signature:
    case FormField::Kind::Unknown:
        break;
    }
    return false;
}

QString FormOverlay::valueOf(int index) const
{
    if (index < 0 || index >= m_fields.size()) {
        return {};
    }
    const FormField &field = m_fields.at(index);
    return m_edited.value(field.name, field.value);
}

bool FormOverlay::isTicked(int index) const
{
    const FormField &field = m_fields.at(index);
    const auto edited = m_edited.constFind(field.name);
    return edited == m_edited.constEnd() ? wasTicked(field) : edited.value() == onValue();
}

bool FormOverlay::isChosen(int index, int widget) const
{
    if (index < 0 || index >= m_fields.size()) {
        return false;
    }
    const FormField &field = m_fields.at(index);
    if (field.kind != FormField::Kind::Radio) {
        return isTicked(index);
    }

    // The whole of the bug this exists to fix: a group's value names one of its
    // buttons, so asking the field whether it is "on" answers yes for every
    // button in it. Only the one whose own state the value names is chosen.
    const QString state = field.widgets.value(widget).onState;
    return !state.isEmpty() && valueOf(index) == state;
}

int FormOverlay::widgetShowing(int index) const
{
    if (index < 0 || index >= m_fields.size()) {
        return 0;
    }
    const FormField &field = m_fields.at(index);
    for (int w = 0; w < field.widgets.size(); ++w) {
        if (field.kind == FormField::Kind::Radio && isChosen(index, w)) {
            return w;
        }
    }
    return 0;
}

void FormOverlay::clearChanges()
{
    for (FormField &field : m_fields) {
        const auto edited = m_edited.constFind(field.name);
        if (edited == m_edited.constEnd()) {
            continue;
        }
        // A tick is written as "on"/"off" but stored as the name the widget's
        // own appearance uses, and folding the one into the other would leave
        // every box reading as ticked.
        if (field.kind == FormField::Kind::Checkbox) {
            field.value = edited.value() == onValue() ? field.options.value(0, u"/Yes"_s) : u"/Off"_s;
        } else if (field.kind == FormField::Kind::Radio) {
            // A chosen radio button is written as its own state name, which is
            // already what the document now holds; "off" is the only other thing
            // this overlay ever writes into a group.
            field.value = edited.value() == offValue() ? u"/Off"_s : edited.value();
        } else {
            field.value = edited.value();
        }
    }
    m_edited.clear();
    if (m_view) {
        m_view->viewport()->update();
    }
}

// ── Drawing ───────────────────────────────────────────────────────────────

bool FormOverlay::appliesTo(PageView::Mode mode) const
{
    return mode == PageView::Mode::View;
}

void FormOverlay::paint(QPainter &painter, int row, const QRect &pageRect)
{
    Q_UNUSED(pageRect)
    const auto here = m_byRow.constFind(row);
    if (here == m_byRow.constEnd() || !m_view) {
        return;
    }

    painter.setRenderHint(QPainter::Antialiasing, true);
    const double zoom = m_view->zoom();

    for (const Spot &spot : here.value()) {
        const int index = spot.field;
        const FormField &field = m_fields.at(index);
        const FormField::Widget &widget = field.widgets.at(spot.widget);
        const QRectF box = m_view->fromPoints(row, turnOf(row, widget.page).mapRect(widget.rect));
        if (box.isEmpty()) {
            continue;
        }

        const bool tick = field.kind == FormField::Kind::Checkbox || field.kind == FormField::Kind::Radio;
        const bool answered = m_edited.contains(field.name);
        const bool covered = index == m_selected && spot.widget == m_selectedWidget && m_editor != nullptr;

        if (answered && !covered) {
            // What is underneath is the render of the file as it was opened, so
            // it still shows the old answer; the new one goes over a cover
            // rather than beside it. The document's own /MK /BG is the right
            // colour for that cover; white is only the fallback for a form
            // that states none.
            painter.setPen(Qt::NoPen);
            painter.setBrush(field.background.isValid() ? field.background : QColor(Qt::white));
            painter.drawRect(box.adjusted(0.6, 0.6, -0.6, -0.6));

            if (tick) {
                // Per widget, so that choosing one button of a group draws the
                // dot in that one and clears every other button of the group in
                // the same stroke.
                if (isChosen(index, spot.widget)) {
                    field.kind == FormField::Kind::Radio ? drawDot(painter, box) : drawTick(painter, box);
                }
            } else {
                const double pad = std::max(1.0, 2.0 * zoom);
                // A /DA with no colour operator means black by the graphics
                // state's own default, which is why an unstated colour is
                // black here rather than something chosen.
                painter.setPen(field.textColour.isValid() ? field.textColour : QColor(20, 20, 20));
                painter.setFont(fontOf(field));
                const Qt::Alignment across = field.alignment == 1 ? Qt::AlignHCenter
                    : field.alignment == 2                        ? Qt::AlignRight
                                                                  : Qt::AlignLeft;
                const int flags
                    = field.multiline ? int(across | Qt::AlignTop | Qt::TextWordWrap) : int(across | Qt::AlignVCenter);
                painter.drawText(box.adjusted(pad, pad, -pad, -pad), flags, valueOf(index));
            }
        }

        // A signature field cannot be signed from here, so it is shown with the
        // same dash as a locked one: the answer to "why will this not open" has
        // to be visible on the page.
        const bool locked = field.readOnly || !isFillable(field);

        // Colour says whether the form insists on an answer, the dash says
        // whether it will take one at all, and neither fact is anywhere in the
        // page's own appearance, which is the whole reason anything is drawn
        // over a document that already knows how to draw itself.
        QColor edge = field.required ? QColor(200, 60, 60) : QColor(120, 120, 120);
        double width = field.required ? 1.5 : 1.0;
        Qt::PenStyle dash = locked ? Qt::DashLine : Qt::SolidLine;

        if (!field.required && !locked) {
            if (field.hasAppearance) {
                // The page already draws this field, and drawing a second box
                // over one that has its own would double every border on the
                // form. A hairline that disappears into it is enough to say
                // there is something here to click.
                edge = m_view->palette().color(QPalette::Highlight);
                edge.setAlpha(70);
            } else if (field.border.isValid()) {
                // No appearance stream but the form says what its border should
                // look like, so draw the one it asked for rather than ours.
                edge = field.border;
                width = std::max(0.5, field.borderWidth) * zoom;
                dash = field.borderStyle == FormField::BorderStyle::Dashed   ? Qt::DashLine
                    : field.borderStyle == FormField::BorderStyle::Underline ? Qt::SolidLine
                                                                             : Qt::SolidLine;
            } else {
                edge = m_view->palette().color(QPalette::Highlight);
                edge.setAlpha(110);
            }
        }

        if (!locked) {
            // The wash stops short of the border by the border's own width, so
            // that a red edge reads as an edge and not as the rim of the tint.
            const double inset = std::max(1.0, width);
            const QRectF washed = box.adjusted(inset, inset, -inset, -inset);
            if (!washed.isEmpty()) {
                // Every widget of a group takes the same tint, and all of them
                // deepen together while the pointer is over any one of them.
                // That is how the group is shown to be a group: the alternative,
                // a line drawn from button to button, is ink this program
                // invented, laid across whatever the form printed between them,
                // which on a real form is the words of the choices themselves. A
                // hover comes and goes and destroys nothing, and it answers the
                // question that matters before the click rather than after it:
                // which other buttons will go off when I press this one.
                const bool lit = index == m_hovered || index == m_selected;
                painter.setPen(Qt::NoPen);
                painter.setCompositionMode(QPainter::CompositionMode_Multiply);
                painter.fillRect(washed, lit ? hoveredWash() : fillableWash());
                painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
            }
        }

        painter.setPen(QPen(edge, width, dash));
        painter.setBrush(Qt::NoBrush);
        if (!field.required && !locked && !field.hasAppearance
            && field.borderStyle == FormField::BorderStyle::Underline) {
            // An underlined field is a line under the writing, not a box round
            // it: a form that asked for one and got a box looks wrong in a way
            // people notice immediately.
            painter.drawLine(box.bottomLeft(), box.bottomRight());
        } else {
            painter.drawRect(box.adjusted(0.5, 0.5, -0.5, -0.5));
        }

        if (index == m_selected) {
            // Outside the box, because the editor widget sits exactly on it and
            // would hide anything drawn within. Every widget of the field is
            // ringed, not only the one that was clicked: what is selected is the
            // question, and for a group the ring is the honest answer to "which
            // buttons is this one choice made of".
            QColor ring = m_view->palette().color(QPalette::Highlight);
            const bool clicked = spot.widget == m_selectedWidget;
            if (!clicked) {
                ring.setAlpha(120);
            }
            painter.setPen(QPen(ring, clicked ? 2.0 : 1.2));
            painter.drawRect(box.adjusted(-1.5, -1.5, 1.5, 1.5));
        }
    }
}

QFont FormOverlay::fontOf(const FormField &field) const
{
    QFont font = m_view ? m_view->font() : QFont();

    // The size the form itself asked for, when it asked for one. A /DA size of
    // zero means "fit to the box" and is not a size at all, so it falls through
    // to the same arithmetic every reader uses: a single-line widget set at
    // roughly two thirds of its height, which is what puts the text where it
    // will print rather than a line above or below.
    const double points = field.fontSize > 0.0
        ? field.fontSize
        : (field.multiline ? 10.0 : std::clamp(field.rect.height() * 0.62, 5.0, 24.0));
    const double zoom = m_view ? m_view->zoom() : 1.0;
    font.setPixelSize(std::max(1, int(std::lround(points * zoom))));
    return font;
}

// ── The mouse ─────────────────────────────────────────────────────────────

FormOverlay::Spot FormOverlay::spotAt(int row, const QPointF &pagePoint) const
{
    const auto here = m_byRow.constFind(row);
    if (here == m_byRow.constEnd()) {
        return {};
    }
    // Back to front, so a widget lying over another is the one a click on the
    // overlap picks: the same order they were drawn in.
    const QVector<Spot> &spots = here.value();
    for (int i = spots.size() - 1; i >= 0; --i) {
        const Spot &spot = spots.at(i);
        // @p pagePoint is where the view drew the page, so the box has to be
        // turned onto the same sheet before the two can be compared at all.
        const FormField::Widget &widget = m_fields.at(spot.field).widgets.at(spot.widget);
        if (turnOf(row, widget.page).mapRect(widget.rect).contains(pagePoint)) {
            return spot;
        }
    }
    return {};
}

QRectF FormOverlay::boxOf(const Spot &spot) const
{
    if (!m_view || spot.field < 0 || spot.field >= m_fields.size()) {
        return {};
    }
    const FormField::Widget widget = m_fields.at(spot.field).widgets.value(spot.widget);
    const int row = rowOfWidget(widget);
    return row < 0 ? QRectF() : m_view->fromPoints(row, turnOf(row, widget.page).mapRect(widget.rect));
}

bool FormOverlay::press(int row, const QPointF &pagePoint, Qt::MouseButtons buttons)
{
    if (!(buttons & Qt::LeftButton)) {
        return false;
    }

    const Spot spot = spotAt(row, pagePoint);
    if (spot.field < 0) {
        // Clicking off a field is how a reader leaves it, and the press still
        // belongs to the view so that a selection can start there.
        deselect();
        return false;
    }

    selectSpot(spot.field, spot.widget);

    const FormField &field = m_fields.at(spot.field);
    if (!field.readOnly) {
        // One click is the whole interaction: a widget over a tick box would be
        // something to click a second time.
        if (field.kind == FormField::Kind::Radio) {
            choose(spot.field, spot.widget);
        } else if (field.kind == FormField::Kind::Checkbox) {
            toggle(spot.field);
        }
    }
    return true;
}

Qt::CursorShape FormOverlay::cursor(int row, const QPointF &pagePoint)
{
    const Spot spot = spotAt(row, pagePoint);
    if (spot.field < 0) {
        return Qt::ArrowCursor;
    }
    const FormField &field = m_fields.at(spot.field);
    if (field.readOnly || !isFillable(field)) {
        return Qt::ForbiddenCursor; // Answers "why can I not type here" before the click.
    }
    return field.kind == FormField::Kind::Text ? Qt::IBeamCursor : Qt::PointingHandCursor;
}

// ── Which field is being filled in ────────────────────────────────────────

void FormOverlay::selectField(int index)
{
    // Asked for the field rather than one of its widgets (from Tab, or from the
    // window), so the one that carries the answer is the one to land on.
    selectSpot(index, widgetShowing(index));
}

void FormOverlay::selectSpot(int index, int widget)
{
    if (index < 0 || index >= m_fields.size()) {
        deselect();
        return;
    }
    const int at = std::clamp(widget, 0, std::max(0, int(m_fields.at(index).widgets.size()) - 1));

    if (index == m_selected) {
        if (at != m_selectedWidget) {
            m_selectedWidget = at;
            placeEditor();
            if (m_view) {
                m_view->viewport()->update();
            }
        }
        if (m_editor) {
            m_editor->setFocus(Qt::MouseFocusReason);
        }
        return;
    }

    closeEditor();
    m_selected = index;
    m_selectedWidget = at;
    m_inspectable = std::make_unique<FormFieldInspectable>(this, index);
    openEditor();

    if (m_view) {
        m_view->viewport()->update();
    }
    Q_EMIT selectionChanged();
}

void FormOverlay::deselect()
{
    if (m_selected < 0) {
        return;
    }
    closeEditor();
    m_selected = -1;
    m_selectedWidget = 0;
    m_inspectable.reset();

    if (m_view) {
        m_view->viewport()->update();
    }
    Q_EMIT selectionChanged();
}

Inspectable *FormOverlay::inspected()
{
    return m_inspectable.get();
}

// ── The editor over the field ─────────────────────────────────────────────

void FormOverlay::openEditor()
{
    if (!m_view || m_selected < 0) {
        return;
    }
    const FormField &field = m_fields.at(m_selected);
    if (field.readOnly || !isFillable(field) || field.kind == FormField::Kind::Checkbox
        || field.kind == FormField::Kind::Radio) {
        return; // Selected, and there is nothing to type into.
    }

    QWidget *host = m_view->viewport();
    const double pad = std::max(1.0, 2.0 * m_view->zoom());

    // Every one of these is filled in before it is connected, so that putting
    // the document's own value into the widget does not come back as something
    // the user typed.
    if (field.kind == FormField::Kind::Choice) {
        auto *combo = new QComboBox(host);
        combo->setEditable(true); // Some lists take a value of their own as well.
        combo->addItems(field.options);
        combo->setCurrentText(valueOf(m_selected));
        connect(combo, &QComboBox::currentTextChanged, this,
                [this](const QString &text) { setValueOf(m_selected, text); });
        m_editor = combo;
    } else if (field.multiline) {
        auto *edit = new QPlainTextEdit(host);
        edit->setFrameShape(QFrame::NoFrame);
        edit->document()->setDocumentMargin(pad);
        edit->setPlainText(valueOf(m_selected));
        connect(edit, &QPlainTextEdit::textChanged, this,
                [this, edit] { setValueOf(m_selected, edit->toPlainText()); });
        m_editor = edit;
    } else {
        auto *edit = new QLineEdit(host);
        edit->setFrame(false); // The page already draws whatever border the field has.
        edit->setTextMargins(int(pad), 0, int(pad), 0);
        edit->setText(valueOf(m_selected));
        connect(edit, &QLineEdit::textChanged, this, [this](const QString &text) { setValueOf(m_selected, text); });
        m_editor = edit;
    }

    // The page is paper. Under a dark colour scheme the widget would otherwise
    // come up as a black hole in the middle of a white form, and the text the
    // user types would be invisible against the value already printed there.
    QPalette palette = m_editor->palette();
    palette.setColor(QPalette::Base, Qt::white);
    palette.setColor(QPalette::Text, Qt::black);
    m_editor->setPalette(palette);
    m_editor->setAutoFillBackground(true);
    m_editor->setFont(fontOf(field));
    m_editor->installEventFilter(this);

    placeEditor();
    m_editor->show();
    m_editor->setFocus(Qt::MouseFocusReason);
}

void FormOverlay::closeEditor()
{
    if (!m_editor) {
        return;
    }
    // The keyboard has to land somewhere, and the view is where Tab is caught.
    if (m_editor->hasFocus() && m_view) {
        m_view->setFocus(Qt::OtherFocusReason);
    }
    m_editor->removeEventFilter(this);
    m_editor->hide();
    m_editor->deleteLater(); // It may be inside its own event handler right now.
    m_editor = nullptr;
}

void FormOverlay::placeEditor()
{
    if (!m_editor || !m_view || m_selected < 0) {
        return;
    }
    const FormField &field = m_fields.at(m_selected);
    // The widget that was clicked, not the field's first: a field repeated at
    // the head of every page is typed into where the reader clicked it.
    const QRect box = boxOf(Spot { m_selected, m_selectedWidget }).toRect();

    // Scrolled off the page it belongs to, the widget would sit over another
    // page's text and take the keyboard with it.
    if (box.isEmpty() || !box.intersects(m_view->viewport()->rect())) {
        m_editor->hide();
        return;
    }

    m_editor->setFont(fontOf(field));
    m_editor->setGeometry(box);
    m_editor->show();
}

// ── Values ────────────────────────────────────────────────────────────────

void FormOverlay::setValueOf(int index, const QString &value)
{
    if (index < 0 || index >= m_fields.size()) {
        return;
    }
    const FormField &field = m_fields.at(index);

    // Only what actually moved is remembered. Writing every field back would
    // set a value on fields nobody touched, and a form full of empty strings is
    // not the same document as a form full of nothing.
    if (value == field.value) {
        m_edited.remove(field.name);
    } else {
        m_edited.insert(field.name, value);
    }
    Q_EMIT valueChanged(field.name, value);
}

void FormOverlay::choose(int index, int widget)
{
    if (index < 0 || index >= m_fields.size()) {
        return;
    }
    const FormField &field = m_fields.at(index);
    const QString state = field.widgets.value(widget).onState;
    if (state.isEmpty()) {
        return; // A button whose appearance names no state cannot be chosen.
    }

    // The group's one value takes this button's own state name, and that is the
    // whole of the choice: every other button of the group asks whether the
    // value is *its* state, and answers no. Nothing has to be turned off by
    // hand, which is what went wrong when a group was one box with one value.
    //
    // Clicking the chosen button again leaves it chosen. A group that could be
    // emptied by clicking its answer would let a form be handed in with the
    // question silently unanswered, which is not what the gesture means.
    setValueOf(index, state);
    if (m_view) {
        m_view->viewport()->update();
    }
}

void FormOverlay::toggle(int index)
{
    if (index < 0 || index >= m_fields.size()) {
        return;
    }
    const FormField &field = m_fields.at(index);
    const bool now = !isTicked(index);

    if (now == wasTicked(field)) {
        m_edited.remove(field.name);
    } else {
        // The name the widget's appearance uses for "on" is whatever the form
        // author chose; Forms::fill works it out from the document.
        m_edited.insert(field.name, now ? onValue() : offValue());
    }
    if (m_view) {
        m_view->viewport()->update();
    }
    Q_EMIT valueChanged(field.name, now ? onValue() : offValue());
}

// ── Moving from field to field ────────────────────────────────────────────

void FormOverlay::focusNextField()
{
    step(1);
}

void FormOverlay::focusPreviousField()
{
    step(-1);
}

void FormOverlay::step(int direction)
{
    if (m_order.isEmpty()) {
        return;
    }
    const int at = m_order.indexOf(m_selected);
    const int next
        = at < 0 ? (direction > 0 ? 0 : m_order.size() - 1) : (at + direction + m_order.size()) % m_order.size();

    const int index = m_order.at(next);
    selectField(index);
    scrollTo(Spot { index, m_selectedWidget });
    placeEditor();

    // A tick box has no widget, so the keyboard stays with the view, which is
    // also what lets Space toggle it and Tab carry on from there.
    if (!m_editor && m_view) {
        m_view->setFocus(Qt::TabFocusReason);
    }
}

void FormOverlay::scrollTo(const Spot &spot)
{
    if (!m_view || spot.field < 0 || spot.field >= m_fields.size()) {
        return;
    }
    const QRect box = boxOf(spot).toRect();
    if (box.isNull()) {
        return;
    }
    const QSize window = m_view->viewport()->size();

    // Nudged by however much it is out of sight rather than scrolled to the top
    // of its page: a field two lines below the fold should bring itself into
    // view, not throw away the rest of the page the user is reading.
    QScrollBar *vertical = m_view->verticalScrollBar();
    if (box.top() < ScrollMargin) {
        vertical->setValue(vertical->value() + box.top() - ScrollMargin);
    } else if (box.bottom() > window.height() - ScrollMargin) {
        vertical->setValue(vertical->value() + box.bottom() - window.height() + ScrollMargin);
    }

    QScrollBar *horizontal = m_view->horizontalScrollBar();
    if (box.left() < ScrollMargin) {
        horizontal->setValue(horizontal->value() + box.left() - ScrollMargin);
    } else if (box.right() > window.width() - ScrollMargin) {
        horizontal->setValue(horizontal->value() + box.right() - window.width() + ScrollMargin);
    }
}

// ── The keyboard ──────────────────────────────────────────────────────────

bool FormOverlay::eventFilter(QObject *watched, QEvent *event)
{
    if (!m_view) {
        return QObject::eventFilter(watched, event);
    }

    if (event->type() == QEvent::Resize && watched == m_view->viewport()) {
        placeEditor();
        return QObject::eventFilter(watched, event);
    }

    // What is under the pointer is followed here rather than in cursor(): the
    // view stops asking overlays for a cursor as soon as one of them answers,
    // and stops asking at all off the page, so a group would stay lit with the
    // pointer somewhere else entirely.
    if ((event->type() == QEvent::MouseMove || event->type() == QEvent::Leave) && watched == m_view->viewport()) {
        int under = -1;
        if (event->type() == QEvent::MouseMove && m_view->mode() == PageView::Mode::View) {
            const QPointF where = static_cast<QMouseEvent *>(event)->position();
            const int row = m_view->pageAt(where.toPoint());
            under = row < 0 ? -1 : spotAt(row, m_view->toPoints(row, where)).field;
        }
        if (under != m_hovered) {
            m_hovered = under;
            m_view->viewport()->update();
        }
        return QObject::eventFilter(watched, event);
    }

    if (event->type() == QEvent::ToolTip && watched == m_view->viewport() && m_view->mode() == PageView::Mode::View) {
        auto *help = static_cast<QHelpEvent *>(event);
        const int row = m_view->pageAt(help->pos());
        const Spot spot = row < 0 ? Spot {} : spotAt(row, m_view->toPoints(row, QPointF(help->pos())));
        if (spot.field >= 0) {
            // What the form asks for, where the form does not say it on the page
            // itself: a tick box's own words are usually beside it and its rules
            // are nowhere at all.
            QToolTip::showText(help->globalPos(), tooltipFor(spot), m_view->viewport());
            return true;
        }
        QToolTip::hideText();
        return QObject::eventFilter(watched, event);
    }

    if (event->type() != QEvent::KeyPress || m_view->mode() != PageView::Mode::View) {
        return QObject::eventFilter(watched, event);
    }

    auto *key = static_cast<QKeyEvent *>(event);
    const bool inEditor = watched == m_editor;

    // Tab is what makes a form fillable without the mouse, and it has to be
    // taken before the widget sees it: a text editor would swallow it and the
    // view would hand the keyboard to the next dock instead of the next field.
    if (key->key() == Qt::Key_Tab || key->key() == Qt::Key_Backtab) {
        if (m_order.isEmpty()) {
            return QObject::eventFilter(watched, event);
        }
        step(key->key() == Qt::Key_Backtab || (key->modifiers() & Qt::ShiftModifier) ? -1 : 1);
        return true;
    }

    if (key->key() == Qt::Key_Escape && inEditor) {
        deselect();
        return true;
    }

    // Enter finishes a one-line answer, the way it does in every other form.
    if ((key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) && inEditor
        && qobject_cast<QLineEdit *>(m_editor)) {
        step(1);
        return true;
    }

    if (key->key() == Qt::Key_Space && !inEditor && m_selected >= 0) {
        const FormField &field = m_fields.at(m_selected);
        if (!field.readOnly && field.kind == FormField::Kind::Radio) {
            choose(m_selected, m_selectedWidget);
            return true;
        }
        if (!field.readOnly && field.kind == FormField::Kind::Checkbox) {
            toggle(m_selected);
            return true;
        }
    }

    // Within a group the arrow keys move from button to button and choose as
    // they go, which is what a keyboard does in every other set of radio buttons.
    // Without it a group could be reached by Tab and never answered.
    const bool arrow = key->key() == Qt::Key_Left || key->key() == Qt::Key_Right || key->key() == Qt::Key_Up
        || key->key() == Qt::Key_Down;
    if (arrow && !inEditor && m_selected >= 0) {
        const FormField &field = m_fields.at(m_selected);
        if (!field.readOnly && field.kind == FormField::Kind::Radio && field.widgets.size() > 1) {
            const int forwards = key->key() == Qt::Key_Right || key->key() == Qt::Key_Down ? 1 : -1;
            const int count = int(field.widgets.size());
            const int next = (m_selectedWidget + forwards + count) % count;
            selectSpot(m_selected, next);
            choose(m_selected, next);
            scrollTo(Spot { m_selected, next });
            return true;
        }
    }

    return QObject::eventFilter(watched, event);
}

// ── What the pointer says about a field ───────────────────────────────────

QString FormOverlay::tooltipFor(const Spot &spot) const
{
    if (spot.field < 0 || spot.field >= m_fields.size()) {
        return {};
    }
    const FormField &field = m_fields.at(spot.field);
    const FieldLabel label = labelOf(spot.field, spot.widget);

    QStringList lines;
    if (!label.text.isEmpty()) {
        // Said to be a guess wherever it is shown. The words beside a box are
        // usually its label and sometimes the tail of the sentence before it,
        // and a reader who knows which is which can ignore a wrong one.
        lines << (label.guessed ? i18nc("@info:tooltip a label read off the page rather than stated by the form; "
                                        "%1 is the text found beside the field",
                                        "%1 (read from the page)", label.text)
                                : label.text);
    } else {
        lines << field.name;
    }

    lines << Forms::describe(field.kind);

    if (field.kind == FormField::Kind::Radio && field.widgets.size() > 1) {
        lines << i18ncp("@info:tooltip one button of a group of radio buttons; %1 is how many there are",
                        "One of %1 choice", "One of %1 choices", int(field.widgets.size()));
    }
    if (field.required) {
        lines << i18nc("@info:tooltip", "The form insists on an answer.");
    }
    if (field.readOnly) {
        lines << i18nc("@info:tooltip", "Locked by the form.");
    }

    return lines.join(QLatin1Char('\n'));
}

// ── What the properties panel shows ───────────────────────────────────────

FormFieldInspectable::FormFieldInspectable(FormOverlay *overlay, int index)
    : m_overlay(overlay)
    , m_index(index)
{
}

QString FormFieldInspectable::kindName() const
{
    return i18nc("@title the kind of thing shown in the properties panel", "Form field");
}

QString FormFieldInspectable::description() const
{
    if (!m_overlay || m_index < 0 || m_index >= m_overlay->fields().size()) {
        return {};
    }
    const FormOverlay::FieldLabel label = m_overlay->labelOf(m_index, m_overlay->selectedWidget());
    return label.text.isEmpty() ? m_overlay->fields().at(m_index).name : label.text;
}

QWidget *FormFieldInspectable::buildEditor(QWidget *parent)
{
    auto *sheet = new QWidget(parent);
    auto *form = new QFormLayout(sheet);
    form->setContentsMargins(0, 0, 0, 0);
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

    if (!m_overlay || m_index < 0 || m_index >= m_overlay->fields().size()) {
        return sheet;
    }
    const FormField &field = m_overlay->fields().at(m_index);

    const auto addRow = [form, sheet](const QString &caption, const QString &text) {
        auto *value = new QLabel(text, sheet);
        value->setWordWrap(true);
        // A field's name is what a form's own instructions quote, so it has to
        // be copyable out of here.
        value->setTextInteractionFlags(Qt::TextSelectableByMouse);
        form->addRow(caption, value);
        return value;
    };

    const QString yes = i18nc("@info a yes-or-no attribute of a form field", "Yes");
    const QString no = i18nc("@info a yes-or-no attribute of a form field", "No");

    addRow(i18nc("@label:textbox the name the document gives the field", "Name:"), field.name);

    // Which of the two it is has to be on the panel: a label the form states is
    // the author's own wording, and one read off the page is this program's
    // reading of it, which can be wrong and is never written back.
    const FormOverlay::FieldLabel label = m_overlay->labelOf(m_index, m_overlay->selectedWidget());
    if (!label.text.isEmpty() && label.text != field.name) {
        addRow(label.guessed ? i18nc("@label:textbox words found on the page beside the field, not stated by the form",
                                     "Label (read from the page):")
                             : i18nc("@label:textbox what the form's author wrote next to the field", "Label:"),
               label.text);
    }

    addRow(i18nc("@label:textbox text, tick box, list and so on", "Kind:"), Forms::describe(field.kind));

    // The page the reader is looking at, not the page the form's own file calls
    // it: those are the same number only until somebody deletes a sheet, and a
    // panel that says "Page: 8" about the page numbered 7 on screen is worse
    // than one that says nothing.
    const int row = m_overlay->rowOfField(m_index);
    if (row >= 0) {
        addRow(i18nc("@label:textbox the page the field sits on", "Page:"), QString::number(row + 1));
    }
    if (field.widgets.size() > 1) {
        // A group of radio buttons and a field repeated on every sheet both look
        // like one box in a list of fields and are not one box on the paper.
        addRow(i18nc("@label:textbox how many boxes on the page one field is drawn as", "Boxes on the page:"),
               QString::number(field.widgets.size()));
    }
    addRow(i18nc("@label:textbox the form insists on an answer", "Required:"), field.required ? yes : no);
    addRow(i18nc("@label:textbox the form does not allow an answer to be typed", "Locked by the form:"),
           field.readOnly ? yes : no);
    if (field.kind == FormField::Kind::Text) {
        addRow(i18nc("@label:textbox the field takes more than one line of text", "Several lines:"),
               field.multiline ? yes : no);
    }
    if (!field.options.isEmpty()) {
        const bool tick = field.kind == FormField::Kind::Checkbox || field.kind == FormField::Kind::Radio;
        addRow(tick ? i18nc("@label:textbox the names a tick box's own appearance uses", "States:")
                    : i18nc("@label:textbox what a list offers", "Choices:"),
               field.options.join(i18nc("@item separator between the choices a form field offers", ", ")));
    }

    QLabel *answer
        = addRow(i18nc("@label:textbox the answer the field holds now", "Value:"), m_overlay->valueOf(m_index));

    // The answer is typed on the page, not here, and this panel outlives none
    // of that, so the one attribute that moves keeps itself up to date. The
    // label is the connection's context, so it dies when the panel is rebuilt.
    QObject::connect(m_overlay, &FormOverlay::valueChanged, answer,
                     [answer, name = field.name](const QString &changed, const QString &value) {
                         if (changed == name) {
                             answer->setText(value);
                         }
                     });

    return sheet;
}

} // namespace ps
