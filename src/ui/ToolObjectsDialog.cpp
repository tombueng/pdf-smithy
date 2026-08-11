/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "ToolDialogs.h"

#include "PageCanvas.h"
#include "ToolSupport.h"
#include "core/Document.h"
#include "core/PageObjects.h"
#include "core/RenderBackend.h"

#include <KLocalizedString>
#include <KMessageBox>

#include <QApplication>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMouseEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSet>
#include <QSpinBox>
#include <QSplitter>
#include <QTabWidget>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QtMath>

#include <algorithm>
#include <cmath>
#include <functional>

using namespace Qt::Literals::StringLiterals;

namespace ps::tools {

namespace {

/** One measurement, written the way a page is measured: points, one decimal. */
QString points(double value)
{
    return QLocale().toString(value, 'f', 1);
}

/**
 * One hue per kind, so that a page reads as its parts before anything is clicked.
 *
 * Fixed rather than taken from the palette: these are six categories that have to
 * stay apart from each other and from the paper underneath them, which a theme's
 * two or three accent colours cannot promise.
 */
QColor tintOf(PageObject::Kind kind)
{
    switch (kind) {
    case PageObject::Kind::Text:
        return { 60, 120, 205 };
    case PageObject::Kind::Image:
    case PageObject::Kind::InlineImage:
        return { 40, 155, 110 };
    case PageObject::Kind::Path:
        return { 205, 130, 45 };
    case PageObject::Kind::Drawing:
        return { 145, 95, 190 };
    case PageObject::Kind::Shading:
        return { 200, 85, 140 };
    }
    return { 120, 120, 120 };
}

QString colourText(const QColor &colour)
{
    return colour.isValid() ? colour.name() : i18nc("@item no colour is set", "none");
}

/** The part of an object that only its kind has. */
QString detailOf(const PageObject &object)
{
    switch (object.kind) {
    case PageObject::Kind::Text:
        return i18nc("@item what a text object says, in which font and at which size", "“%1” · %2 %3 pt", object.text,
                     object.fontResource.isEmpty() ? i18nc("@item the object names no font", "none")
                                                   : object.fontResource,
                     points(object.fontSize));
    case PageObject::Kind::Image:
    case PageObject::Kind::InlineImage:
        return i18nc("@item a picture: its resource name and how many pixels it holds", "%1 · %2 × %3 px",
                     object.resourceName.isEmpty() ? i18nc("@item the picture has no resource name", "none")
                                                   : object.resourceName,
                     object.pixelSize.width(), object.pixelSize.height());
    case PageObject::Kind::Drawing:
    case PageObject::Kind::Shading:
        return object.resourceName;
    case PageObject::Kind::Path:
        return i18nc("@item how a shape is painted", "fill %1 · outline %2 · %3 pt", colourText(object.fill),
                     colourText(object.stroke), points(object.lineWidth));
    }
    return {};
}

QString whereOf(const QRectF &bounds)
{
    return i18nc("@item where an object sits and how big it is, in points", "%1, %2 · %3 × %4", points(bounds.x()),
                 points(bounds.y()), points(bounds.width()), points(bounds.height()));
}

/**
 * What one waiting change will do, in the words of the gesture that made it.
 *
 * A matrix is taken apart again rather than remembered as "the user pressed Move
 * twice": three nudges and a resize are one matrix by the time they reach the
 * file, and a list that still said "moved, moved, resized" would be describing
 * the history instead of the result.
 */
QString changeOf(const PageEdit &edit, const QRectF &bounds)
{
    if (edit.kind == PageEdit::Kind::Delete) {
        return i18nc("@item what is waiting to happen to an object", "will be taken off the page");
    }

    if (edit.kind == PageEdit::Kind::Restyle) {
        QStringList parts;
        if (edit.fill.isValid()) {
            parts += i18nc("@item part of a waiting style change", "fill %1", edit.fill.name());
        }
        if (edit.stroke.isValid()) {
            parts += i18nc("@item part of a waiting style change", "outline %1", edit.stroke.name());
        }
        if (edit.lineWidth >= 0.0) {
            parts += i18nc("@item part of a waiting style change", "%1 pt line", points(edit.lineWidth));
        }
        return parts.join(u" · "_s);
    }

    const QTransform &matrix = edit.transform;
    const double across = std::hypot(matrix.m11(), matrix.m12());
    const double down = std::hypot(matrix.m21(), matrix.m22());
    const double turn = qRadiansToDegrees(std::atan2(matrix.m12(), matrix.m11()));
    const QPointF shift = matrix.mapRect(bounds).center() - bounds.center();

    QStringList parts;
    if (!bounds.isNull() && (qAbs(shift.x()) > 0.05 || qAbs(shift.y()) > 0.05)) {
        parts += i18nc("@item how far a waiting change moves an object", "%1, %2 pt", points(shift.x()),
                       points(shift.y()));
    }
    if (qAbs(across - 1.0) > 0.001 || qAbs(down - 1.0) > 0.001) {
        parts += i18nc("@item how much a waiting change resizes an object", "%1 %",
                       QLocale().toString((across + down) / 2.0 * 100.0, 'f', 0));
    }
    if (qAbs(turn) > 0.05) {
        parts += i18nc("@item how far a waiting change turns an object", "%1°", QLocale().toString(turn, 'f', 1));
    }
    return parts.isEmpty() ? i18nc("@item a change that comes to nothing", "no change") : parts.join(u" · "_s);
}

/** A button that shows the colour it holds. */
class SwatchButton : public QPushButton
{
public:
    explicit SwatchButton(const QColor &initial, QWidget *parent = nullptr)
        : QPushButton(parent)
        , m_colour(initial)
    {
        setAutoFillBackground(true);
        setMinimumWidth(90);
        refresh();
        connect(this, &QPushButton::clicked, this, [this] {
            const QColor picked = QColorDialog::getColor(m_colour, this, i18nc("@title:window", "Pick a Colour"));
            if (picked.isValid()) {
                m_colour = picked;
                refresh();
            }
        });
    }

    QColor colour() const { return m_colour; }

private:
    void refresh()
    {
        setText(m_colour.name());
        // Black text on a dark swatch is unreadable, so the label follows the
        // swatch rather than the theme.
        const QString ink = m_colour.lightnessF() > 0.55 ? u"#101010"_s : u"#f0f0f0"_s;
        setStyleSheet(u"background-color: %1; color: %2;"_s.arg(m_colour.name(), ink));
    }

    QColor m_colour;
};

/**
 * The page with a boundary drawn round everything on it.
 *
 * The engine's promise is that a page can be pointed at; this is where the
 * pointing happens. Choosing goes through PageObjects::hitTest and
 * PageObjects::within rather than through boxes worked out here, so that a click
 * in the window picks exactly what "objects at" would print, including the rule
 * that a thin diagonal line is caught by its own shape and not by the
 * page-sized rectangle around it.
 *
 * Reports rather than commands: it says what was clicked and how far something
 * was dragged, and the dialog decides what that means. No Q_OBJECT, so the
 * reports are plain callbacks.
 */
class ObjectView : public PageCanvas
{
public:
    explicit ObjectView(QWidget *parent = nullptr)
        : PageCanvas(parent)
    {
        setCursor(Qt::ArrowCursor);
    }

    /** @p objects are as they now stand, with everything waiting already in them. */
    void setObjects(const QVector<PageObject> &objects, const QSet<int> &doomed)
    {
        m_objects = objects;
        m_doomed = doomed;
        update();
    }

    void setChosen(const QSet<int> &chosen)
    {
        m_chosen = chosen;
        update();
    }

    /** Places waiting to be filled with something new, in points. */
    void setInsertions(const QVector<QRectF> &places)
    {
        m_insertions = places;
        update();
    }

    /** While placing, a drag draws a new object's place instead of choosing. */
    void setPlacing(bool placing)
    {
        m_placing = placing;
        setCursor(placing ? Qt::CrossCursor : Qt::ArrowCursor);
    }

    std::function<void(const QVector<int> &objects, bool add)> chosenChanged;
    std::function<void(const QPointF &byPoints)> dragged;
    std::function<void(const QRectF &placeInPoints)> placed;
    std::function<void()> removeAsked;

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    /** The boundary as a rectangle on screen, never so thin that it vanishes. */
    QRect boxOf(const QRectF &bounds) const
    {
        QRect box = toWidget(fromPoints(bounds.normalized())).normalized();
        if (box.width() < 3) {
            box.adjust(-2, 0, 2, 0);
        }
        if (box.height() < 3) {
            box.adjust(0, -2, 0, 2);
        }
        return box;
    }

    QPointF pointsAt(const QPoint &where) const { return toPoints(fromWidget(where)); }

    QVector<PageObject> m_objects;
    QVector<QRectF> m_insertions;
    QSet<int> m_chosen;
    QSet<int> m_doomed;
    bool m_placing = false;
    bool m_banding = false;
    bool m_moving = false;
    QPoint m_from;
    QPoint m_to;
};

void ObjectView::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event)

    QPainter painter(this);
    drawPage(painter);
    if (!hasPage()) {
        return;
    }

    const QColor highlight = palette().color(QPalette::Highlight);
    // Drawn while the mouse is still down, so that dragging shows where the
    // thing is going rather than where it was.
    const QPoint shift = m_moving ? m_to - m_from : QPoint();

    for (const PageObject &object : std::as_const(m_objects)) {
        QRect box = boxOf(object.bounds);
        const bool chosen = m_chosen.contains(object.index);
        const bool doomed = m_doomed.contains(object.index);
        if (chosen) {
            box.translate(shift);
        }

        QColor tint = doomed ? QColor(200, 60, 60) : tintOf(object.kind);
        QColor wash = tint;
        wash.setAlpha(chosen ? 60 : 22);
        painter.setPen(Qt::NoPen);
        painter.setBrush(wash);
        painter.drawRect(box);

        painter.setPen(QPen(chosen ? highlight : tint, chosen ? 2.0 : 1.0, doomed ? Qt::DashLine : Qt::SolidLine));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(box);

        if (doomed) {
            painter.drawLine(box.topLeft(), box.bottomRight());
            painter.drawLine(box.bottomLeft(), box.topRight());
        }

        if (chosen) {
            // The number is what the list beside the page calls it, so that a box
            // on a crowded page and a row in the list can be matched by eye.
            const QString label = QString::number(object.index);
            const QRect plate(box.topLeft(), QSize(painter.fontMetrics().horizontalAdvance(label) + 8, 16));
            painter.setPen(Qt::NoPen);
            painter.setBrush(highlight);
            painter.drawRect(plate);
            painter.setPen(palette().color(QPalette::HighlightedText));
            painter.drawText(plate, Qt::AlignCenter, label);
        }
    }

    for (const QRectF &place : std::as_const(m_insertions)) {
        painter.setPen(QPen(QColor(40, 155, 110), 2.0, Qt::DashLine));
        painter.setBrush(QColor(40, 155, 110, 30));
        painter.drawRect(boxOf(place));
    }

    if (m_banding) {
        painter.setPen(QPen(highlight, 1.0, Qt::DashLine));
        QColor wash = highlight;
        wash.setAlpha(30);
        painter.setBrush(wash);
        painter.drawRect(QRect(m_from, m_to).normalized());
    }
}

void ObjectView::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton || !hasPage()) {
        PageCanvas::mousePressEvent(event);
        return;
    }

    setFocus(Qt::MouseFocusReason); // So that the arrow keys nudge what was just clicked.
    m_from = event->pos();
    m_to = m_from;

    if (m_placing) {
        m_banding = true;
        update();
        return;
    }

    const bool add = event->modifiers().testFlag(Qt::ControlModifier);
    const int hit = PageObjects::hitTest(m_objects, pointsAt(m_from));

    if (hit < 0) {
        m_banding = true;
        if (!add && chosenChanged) {
            chosenChanged({}, false);
        }
        update();
        return;
    }

    if (add) {
        if (chosenChanged) {
            chosenChanged({ hit }, true);
        }
    } else {
        // Pressing on something already chosen keeps the whole choice, so that a
        // group of five can be dragged without picking them again one by one.
        if (!m_chosen.contains(hit) && chosenChanged) {
            chosenChanged({ hit }, false);
        }
        m_moving = true;
    }
    update();
}

void ObjectView::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_banding && !m_moving) {
        PageCanvas::mouseMoveEvent(event);
        return;
    }
    m_to = event->pos();
    update();
}

void ObjectView::mouseReleaseEvent(QMouseEvent *event)
{
    if (m_banding) {
        m_banding = false;
        const QRect band = QRect(m_from, m_to).normalized();
        const bool real = band.width() > 3 && band.height() > 3;
        const QRectF area = toPoints(fromWidget(band));

        if (m_placing && real && placed) {
            placed(area);
        } else if (!m_placing && real && chosenChanged) {
            chosenChanged(PageObjects::within(m_objects, area), event->modifiers().testFlag(Qt::ControlModifier));
        }
        update();
        return;
    }

    if (m_moving) {
        m_moving = false;
        const QPointF delta = pointsAt(m_to) - pointsAt(m_from);
        if (dragged && (qAbs(delta.x()) > 0.05 || qAbs(delta.y()) > 0.05)) {
            dragged(delta);
        }
        update();
        return;
    }
    PageCanvas::mouseReleaseEvent(event);
}

void ObjectView::keyPressEvent(QKeyEvent *event)
{
    // A point at a time, ten with Shift: the numbers people reach for when
    // something is nearly in the right place.
    const double step = event->modifiers().testFlag(Qt::ShiftModifier) ? 10.0 : 1.0;
    QPointF delta;

    switch (event->key()) {
    case Qt::Key_Left:
        delta = QPointF(-step, 0);
        break;
    case Qt::Key_Right:
        delta = QPointF(step, 0);
        break;
    case Qt::Key_Up:
        delta = QPointF(0, step);
        break;
    case Qt::Key_Down:
        delta = QPointF(0, -step);
        break;
    case Qt::Key_Delete:
        if (removeAsked) {
            removeAsked();
        }
        return;
    default:
        PageCanvas::keyPressEvent(event);
        return;
    }

    if (dragged) {
        dragged(delta);
    }
}

enum Column { NumberColumn, KindColumn, WhereColumn, DetailColumn, ChangeColumn, ColumnCount };

/**
 * A page as things that can be pointed at, moved and thrown away.
 *
 * There is nothing in a PDF to select: a page is a list of drawing instructions.
 * PageObjects invents the handle, and this window is what that handle is for:
 * the boxes on the page and the rows beside it are the same list twice, so that
 * "the caption near the bottom" and "object 47" are the same thing.
 *
 * Changes are collected rather than performed. Editing rewrites the instruction
 * stream in one pass, and every number in it counts painting operations from the
 * start of the page, so deleting one thing renumbers everything after it. Two
 * passes would mean the second one was addressed to a page the first one had
 * already changed. Hence one edit per object, all of them written together, and
 * a column that says what each is going to do before it is done.
 */
class ObjectsDialog : public QDialog
{
public:
    ObjectsDialog(Document *document, RenderBackend *backend, const QString &pdf, int page, QWidget *parent);

private:
    QWidget *buildStage();
    QWidget *buildPanel();
    QWidget *buildChangeTab();
    QWidget *buildAddTab();

    void showPage(int page);
    void fillList();
    void refresh();

    /** Carries the choice from the list to the page, and to what may act on it. */
    void syncChoice();

    /** The page as it would now look, with everything waiting applied. */
    QVector<PageObject> asShown() const;
    const PageObject *objectAt(int index) const;

    QVector<int> chosenObjects() const;
    void chooseObjects(const QVector<int> &objects, bool add);

    /**
     * Folds one gesture into what is already waiting on each chosen object.
     *
     * @p gesture is handed the object's boundary *as it now stands*, because
     * turning something round its middle means the middle it has after the two
     * moves that came before, not the one it had in the file.
     */
    void addGesture(const std::function<QTransform(const QRectF &now)> &gesture);
    void moveBy(const QPointF &delta);
    void removeChosen();
    void restyleChosen();
    void takeBack();
    void takeBackAll();
    void place(const QRectF &where);
    void write();
    void showLimits();

    Document *m_document = nullptr;
    RenderBackend *m_backend = nullptr;
    QString m_pdf;
    int m_page = 0;
    QVector<PageObject> m_objects;

    /** Page, then object, then the one change waiting on it: what apply() takes. */
    QHash<int, QHash<int, PageEdit>> m_pending;
    QVector<NewContent> m_insertions;

    ObjectView *m_view = nullptr;
    QSpinBox *m_pageBox = nullptr;
    QLabel *m_pageLabel = nullptr;
    QTreeWidget *m_list = nullptr;
    QHash<int, QTreeWidgetItem *> m_rows;
    QLabel *m_status = nullptr;
    QTabWidget *m_tabs = nullptr;
    QVector<QPushButton *> m_needChoice;

    QDoubleSpinBox *m_dx = nullptr;
    QDoubleSpinBox *m_dy = nullptr;
    QDoubleSpinBox *m_scale = nullptr;
    QDoubleSpinBox *m_angle = nullptr;
    QCheckBox *m_setFill = nullptr;
    QCheckBox *m_setStroke = nullptr;
    QCheckBox *m_setWidth = nullptr;
    SwatchButton *m_fill = nullptr;
    SwatchButton *m_stroke = nullptr;
    QDoubleSpinBox *m_lineWidth = nullptr;

    QComboBox *m_shape = nullptr;
    QPlainTextEdit *m_text = nullptr;
    QComboBox *m_family = nullptr;
    QDoubleSpinBox *m_fontSize = nullptr;
    QLineEdit *m_image = nullptr;
    QPushButton *m_browse = nullptr;
    QSpinBox *m_quality = nullptr;
    QCheckBox *m_newFilled = nullptr;
    SwatchButton *m_newFill = nullptr;
    SwatchButton *m_newStroke = nullptr;
    QDoubleSpinBox *m_newWidth = nullptr;
    QSpinBox *m_opacity = nullptr;
    QLabel *m_waiting = nullptr;
};

ObjectsDialog::ObjectsDialog(Document *document, RenderBackend *backend, const QString &pdf, int page, QWidget *parent)
    : QDialog(parent)
    , m_document(document)
    , m_backend(backend)
    , m_pdf(pdf)
{
    setWindowTitle(i18nc("@title:window", "Objects on the Page"));
    resize(1180, 800);

    auto *split = new QSplitter(Qt::Horizontal, this);
    split->addWidget(buildStage());
    split->addWidget(buildPanel());
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 2);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    auto *apply = buttons->addButton(i18nc("@action:button", "Write the Changes…"), QDialogButtonBox::ActionRole);
    apply->setIcon(QIcon::fromTheme(u"document-save-as"_s));
    auto *limits = buttons->addButton(i18nc("@action:button", "What This Cannot Do"), QDialogButtonBox::HelpRole);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(apply, &QPushButton::clicked, this, [this] { write(); });
    connect(limits, &QPushButton::clicked, this, [this] { showLimits(); });

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(split, 1);
    layout->addWidget(buttons);

    showPage(qBound(0, page, m_document ? qMax(0, m_document->pageCount() - 1) : 0));
}

// ── the page ──────────────────────────────────────────────────────────────

QWidget *ObjectsDialog::buildStage()
{
    auto *stage = new QWidget(this);

    const int count = m_document ? m_document->pageCount() : 0;
    m_pageBox = new QSpinBox(stage);
    m_pageBox->setRange(1, qMax(1, count));
    m_pageBox->setPrefix(i18nc("@label:spinbox prefix before a page number", "Page "));
    m_pageLabel = new QLabel(i18nc("@info total page count", "of %1", count), stage);

    auto *previous = new QPushButton(i18nc("@action:button go to the previous page", "Previous"), stage);
    auto *next = new QPushButton(i18nc("@action:button go to the next page", "Next"), stage);

    auto *navigation = new QHBoxLayout;
    navigation->addWidget(previous);
    navigation->addWidget(m_pageBox);
    navigation->addWidget(m_pageLabel);
    navigation->addStretch(1);

    m_view = new ObjectView(stage);

    auto *hint = new QLabel(i18n("Click something to choose it, Ctrl to add to the choice, drag on bare paper to "
                                 "draw a box round several. Dragging a chosen object moves it; so do the arrow "
                                 "keys, by a point at a time and ten with Shift."),
                            stage);
    hint->setWordWrap(true);

    auto *layout = new QVBoxLayout(stage);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addLayout(navigation);
    layout->addWidget(m_view, 1);
    layout->addWidget(hint);

    connect(previous, &QPushButton::clicked, this, [this] { m_pageBox->setValue(m_pageBox->value() - 1); });
    connect(next, &QPushButton::clicked, this, [this] { m_pageBox->setValue(m_pageBox->value() + 1); });
    connect(m_pageBox, &QSpinBox::valueChanged, this, [this](int value) { showPage(value - 1); });

    m_view->chosenChanged = [this](const QVector<int> &objects, bool add) { chooseObjects(objects, add); };
    m_view->dragged = [this](const QPointF &delta) { moveBy(delta); };
    m_view->placed = [this](const QRectF &where) { place(where); };
    m_view->removeAsked = [this] { removeChosen(); };
    return stage;
}

void ObjectsDialog::showPage(int page)
{
    if (!m_document || page < 0 || page >= m_document->pageCount()) {
        return;
    }
    m_page = page;

    QApplication::setOverrideCursor(Qt::WaitCursor);
    QString error;
    m_objects = PageObjects::read(m_pdf, page, &error);

    const PageRef ref = m_document->pageAt(page);
    QImage image = m_backend ? m_backend->renderPage(ref.sourceId, ref.sourcePage, 1400) : QImage();
    if (!image.isNull() && ref.rotation != 0) {
        image = image.transformed(QTransform().rotate(ref.rotation), Qt::SmoothTransformation);
    }
    m_view->setPage(image, m_document->pageSizePoints(page));
    QApplication::restoreOverrideCursor();

    if (!error.isEmpty()) {
        KMessageBox::error(this, error, windowTitle());
    }

    if (m_pageBox->value() != page + 1) {
        QSignalBlocker blocker(m_pageBox);
        m_pageBox->setValue(page + 1);
    }

    fillList();
    refresh();
}

// ── the list ──────────────────────────────────────────────────────────────

QWidget *ObjectsDialog::buildPanel()
{
    auto *panel = new QWidget(this);

    m_list = new QTreeWidget(panel);
    m_list->setColumnCount(ColumnCount);
    m_list->setHeaderLabels({ i18nc("@title:column the number that names an object", "No"),
                              i18nc("@title:column what kind of thing it is", "Kind"),
                              i18nc("@title:column where it sits on the page", "Place"),
                              i18nc("@title:column what only this kind of object has", "Detail"),
                              i18nc("@title:column what is waiting to happen to it", "Change") });
    m_list->setRootIsDecorated(false);
    m_list->setUniformRowHeights(true);
    m_list->setAllColumnsShowFocus(true);
    m_list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_list->header()->setSectionResizeMode(DetailColumn, QHeaderView::Stretch);

    m_status = new QLabel(panel);
    m_status->setWordWrap(true);

    m_tabs = new QTabWidget(panel);
    m_tabs->addTab(buildChangeTab(), i18nc("@title:tab", "Change"));
    m_tabs->addTab(buildAddTab(), i18nc("@title:tab", "Add"));

    auto *heading = new QLabel(i18n("What this page draws, topmost first."), panel);

    auto *layout = new QVBoxLayout(panel);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(heading);
    layout->addWidget(m_list, 1);
    layout->addWidget(m_status);
    layout->addWidget(m_tabs);

    connect(m_list, &QTreeWidget::itemSelectionChanged, this, [this] { syncChoice(); });

    // Placing is the Add tab rather than a mode of its own: the tab is already
    // the answer to "what is a drag on the page for now".
    connect(m_tabs, &QTabWidget::currentChanged, this, [this](int index) { m_view->setPlacing(index == 1); });
    return panel;
}

void ObjectsDialog::fillList()
{
    m_list->clear();
    m_rows.clear();

    // Backwards through the drawing order: what lies on top belongs at the top of
    // a list of layers, which is where everyone looks for it.
    for (int i = m_objects.size() - 1; i >= 0; --i) {
        const PageObject &object = m_objects.at(i);
        auto *row = new QTreeWidgetItem(m_list);
        row->setText(NumberColumn, QString::number(object.index));
        row->setTextAlignment(NumberColumn, Qt::AlignRight | Qt::AlignVCenter);
        row->setText(KindColumn, PageObjects::describe(object.kind));
        row->setForeground(KindColumn, tintOf(object.kind));
        row->setText(WhereColumn, whereOf(object.bounds));
        row->setText(DetailColumn, detailOf(object));
        row->setData(NumberColumn, Qt::UserRole, object.index);
        m_rows.insert(object.index, row);
    }

    for (int column = NumberColumn; column < DetailColumn; ++column) {
        m_list->resizeColumnToContents(column);
    }
}

void ObjectsDialog::refresh()
{
    const QHash<int, PageEdit> here = m_pending.value(m_page);

    QSet<int> doomed;
    for (auto it = here.constBegin(); it != here.constEnd(); ++it) {
        if (it->kind == PageEdit::Kind::Delete) {
            doomed.insert(it.key());
        }
    }

    for (const PageObject &object : std::as_const(m_objects)) {
        QTreeWidgetItem *row = m_rows.value(object.index);
        if (!row) {
            continue;
        }
        const auto waiting = here.constFind(object.index);
        row->setText(ChangeColumn, waiting == here.constEnd() ? QString() : changeOf(*waiting, object.bounds));
    }
    m_list->resizeColumnToContents(ChangeColumn);

    m_view->setObjects(asShown(), doomed);

    QVector<QRectF> places;
    for (const NewContent &item : std::as_const(m_insertions)) {
        if (item.page == m_page) {
            places.append(item.rect);
        }
    }
    m_view->setInsertions(places);

    int changes = 0;
    for (auto it = m_pending.constBegin(); it != m_pending.constEnd(); ++it) {
        changes += it->size();
    }
    changes += m_insertions.size();

    const QString drawn = m_objects.isEmpty()
        ? i18n("This page draws nothing.")
        : i18np("One object on this page.", "%1 objects on this page.", m_objects.size());
    const QString waiting = changes == 0 ? i18n("Nothing is waiting to be written.")
                                         : i18np("One change is waiting.", "%1 changes are waiting.", changes);
    m_status->setText(drawn + u' ' + waiting);
    syncChoice();

    m_waiting->setText(m_insertions.isEmpty() ? i18n("Nothing added yet.")
                                              : i18np("One thing waiting to be added.",
                                                      "%1 things waiting to be added.", m_insertions.size()));
}

void ObjectsDialog::syncChoice()
{
    QSet<int> chosen;
    for (const int index : chosenObjects()) {
        chosen.insert(index);
    }
    m_view->setChosen(chosen);
    for (QPushButton *button : std::as_const(m_needChoice)) {
        button->setEnabled(!chosen.isEmpty());
    }
}

QVector<PageObject> ObjectsDialog::asShown() const
{
    const QHash<int, PageEdit> here = m_pending.value(m_page);
    QVector<PageObject> shown = m_objects;
    for (PageObject &object : shown) {
        const auto waiting = here.constFind(object.index);
        if (waiting != here.constEnd() && waiting->kind == PageEdit::Kind::Transform) {
            object.bounds = waiting->transform.mapRect(object.bounds);
            object.outline = waiting->transform.map(object.outline);
        }
    }
    return shown;
}

const PageObject *ObjectsDialog::objectAt(int index) const
{
    for (const PageObject &object : m_objects) {
        if (object.index == index) {
            return &object;
        }
    }
    return nullptr;
}

QVector<int> ObjectsDialog::chosenObjects() const
{
    QVector<int> objects;
    const QList<QTreeWidgetItem *> rows = m_list->selectedItems();
    for (const QTreeWidgetItem *row : rows) {
        objects.append(row->data(NumberColumn, Qt::UserRole).toInt());
    }
    std::sort(objects.begin(), objects.end());
    return objects;
}

void ObjectsDialog::chooseObjects(const QVector<int> &objects, bool add)
{
    if (!add) {
        m_list->clearSelection();
    }
    for (const int index : objects) {
        if (QTreeWidgetItem *row = m_rows.value(index)) {
            row->setSelected(add ? !row->isSelected() : true);
        }
    }
    if (!objects.isEmpty()) {
        if (QTreeWidgetItem *row = m_rows.value(objects.last())) {
            m_list->scrollToItem(row);
        }
    }
}

// ── changing what is there ────────────────────────────────────────────────

QWidget *ObjectsDialog::buildChangeTab()
{
    auto *tab = new QWidget(this);

    const auto measure = [tab](double from, double to, double value) {
        auto *box = new QDoubleSpinBox(tab);
        box->setRange(from, to);
        box->setDecimals(1);
        box->setValue(value);
        return box;
    };

    m_dx = measure(-3000, 3000, 10);
    m_dx->setSuffix(i18nc("@item point suffix in a spin box", " pt"));
    m_dy = measure(-3000, 3000, 0);
    m_dy->setSuffix(i18nc("@item point suffix in a spin box", " pt"));
    auto *move = new QPushButton(QIcon::fromTheme(u"transform-move"_s), i18nc("@action:button", "Move"), tab);

    auto *moveRow = new QHBoxLayout;
    moveRow->addWidget(new QLabel(i18nc("@label:spinbox how far to the right", "Across:"), tab));
    moveRow->addWidget(m_dx);
    moveRow->addWidget(new QLabel(i18nc("@label:spinbox how far up", "Up:"), tab));
    moveRow->addWidget(m_dy);
    moveRow->addWidget(move);
    moveRow->addStretch(1);

    m_scale = measure(1, 2000, 100);
    m_scale->setSuffix(i18nc("@item percent suffix in a spin box", " %"));
    m_scale->setToolTip(i18nc("@info:tooltip", "Each object grows or shrinks around its own middle."));
    auto *resize = new QPushButton(QIcon::fromTheme(u"transform-scale"_s), i18nc("@action:button", "Resize"), tab);

    m_angle = measure(-360, 360, 90);
    m_angle->setSuffix(i18nc("@item degree suffix in a spin box", "°"));
    m_angle->setToolTip(i18nc("@info:tooltip", "Counter-clockwise, around the object's own middle."));
    auto *turn = new QPushButton(QIcon::fromTheme(u"transform-rotate"_s), i18nc("@action:button", "Turn"), tab);

    auto *sizeRow = new QHBoxLayout;
    sizeRow->addWidget(new QLabel(i18nc("@label:spinbox", "Size:"), tab));
    sizeRow->addWidget(m_scale);
    sizeRow->addWidget(resize);
    sizeRow->addWidget(new QLabel(i18nc("@label:spinbox", "Turn:"), tab));
    sizeRow->addWidget(m_angle);
    sizeRow->addWidget(turn);
    sizeRow->addStretch(1);

    auto *placeBox = new QGroupBox(i18nc("@title:group", "Where it is and how big"), tab);
    auto *placeLayout = new QVBoxLayout(placeBox);
    placeLayout->addLayout(moveRow);
    placeLayout->addLayout(sizeRow);

    m_setFill = new QCheckBox(i18nc("@option:check", "Fill:"), tab);
    m_fill = new SwatchButton(QColor(30, 30, 30), tab);
    m_setStroke = new QCheckBox(i18nc("@option:check", "Outline:"), tab);
    m_stroke = new SwatchButton(QColor(30, 30, 30), tab);
    m_setWidth = new QCheckBox(i18nc("@option:check", "Line width:"), tab);
    m_lineWidth = measure(0, 100, 1);
    m_lineWidth->setDecimals(2);
    m_lineWidth->setSuffix(i18nc("@item point suffix in a spin box", " pt"));
    auto *restyle
        = new QPushButton(QIcon::fromTheme(u"format-fill-color"_s), i18nc("@action:button", "Set the Colours"), tab);

    auto *styleBox = new QGroupBox(i18nc("@title:group", "Colours and line"), tab);
    auto *styleForm = new QFormLayout(styleBox);
    styleForm->addRow(m_setFill, m_fill);
    styleForm->addRow(m_setStroke, m_stroke);
    styleForm->addRow(m_setWidth, m_lineWidth);
    styleForm->addRow(QString(), restyle);

    auto *styleNote = new QLabel(i18n("For a shape this is the fill and the outline; for text it is the colour of "
                                      "the letters. A picture takes no notice."),
                                 styleBox);
    styleNote->setWordWrap(true);
    styleForm->addRow(styleNote);

    auto *remove
        = new QPushButton(QIcon::fromTheme(u"edit-delete"_s), i18nc("@action:button", "Take Off the Page"), tab);
    auto *undo = new QPushButton(QIcon::fromTheme(u"edit-undo"_s), i18nc("@action:button", "Let It Be"), tab);
    undo->setToolTip(i18nc("@info:tooltip", "Drops the change waiting on the chosen objects."));
    auto *undoAll = new QPushButton(i18nc("@action:button", "Let Everything Be"), tab);

    auto *buttonRow = new QHBoxLayout;
    buttonRow->addWidget(remove);
    buttonRow->addWidget(undo);
    buttonRow->addWidget(undoAll);
    buttonRow->addStretch(1);

    auto *note = new QLabel(i18n("An object carries one change at a time, because the page is rewritten in a single "
                                 "pass. Giving something a colour therefore takes the place of a move that was "
                                 "waiting on it. The Change column says what each object will do."),
                            tab);
    note->setWordWrap(true);

    auto *layout = new QVBoxLayout(tab);
    layout->addWidget(placeBox);
    layout->addWidget(styleBox);
    layout->addLayout(buttonRow);
    layout->addWidget(note);
    layout->addStretch(1);

    connect(move, &QPushButton::clicked, this, [this] { moveBy(QPointF(m_dx->value(), m_dy->value())); });
    connect(resize, &QPushButton::clicked, this, [this] {
        const double factor = m_scale->value() / 100.0;
        addGesture([factor](const QRectF &now) {
            const QPointF pivot = now.center();
            return QTransform::fromTranslate(-pivot.x(), -pivot.y()) * QTransform::fromScale(factor, factor)
                * QTransform::fromTranslate(pivot.x(), pivot.y());
        });
    });
    connect(turn, &QPushButton::clicked, this, [this] {
        // Built by itself, on an identity matrix, so that there is no doubt about
        // which side of the multiplication the rotation lands on.
        QTransform rotation;
        rotation.rotate(m_angle->value());
        addGesture([rotation](const QRectF &now) {
            const QPointF pivot = now.center();
            return QTransform::fromTranslate(-pivot.x(), -pivot.y()) * rotation
                * QTransform::fromTranslate(pivot.x(), pivot.y());
        });
    });
    connect(restyle, &QPushButton::clicked, this, [this] { restyleChosen(); });
    connect(remove, &QPushButton::clicked, this, [this] { removeChosen(); });
    connect(undo, &QPushButton::clicked, this, [this] { takeBack(); });
    connect(undoAll, &QPushButton::clicked, this, [this] { takeBackAll(); });

    m_needChoice = { move, resize, turn, restyle, remove, undo };
    for (QPushButton *button : std::as_const(m_needChoice)) {
        button->setEnabled(false);
    }
    return tab;
}

void ObjectsDialog::addGesture(const std::function<QTransform(const QRectF &now)> &gesture)
{
    const QVector<int> chosen = chosenObjects();
    if (chosen.isEmpty()) {
        return;
    }

    QHash<int, PageEdit> &here = m_pending[m_page];
    for (const int index : chosen) {
        const PageObject *object = objectAt(index);
        if (!object) {
            continue;
        }

        PageEdit &edit = here[index];
        if (edit.kind != PageEdit::Kind::Transform) {
            edit = PageEdit(); // A deletion or a colour was waiting; this replaces it.
        }
        edit.kind = PageEdit::Kind::Transform;
        edit.page = m_page;
        edit.object = index;
        edit.transform = edit.transform * gesture(edit.transform.mapRect(object->bounds));
    }
    refresh();
}

void ObjectsDialog::moveBy(const QPointF &delta)
{
    addGesture([delta](const QRectF &now) {
        Q_UNUSED(now)
        return QTransform::fromTranslate(delta.x(), delta.y());
    });
}

void ObjectsDialog::removeChosen()
{
    const QVector<int> chosen = chosenObjects();
    if (chosen.isEmpty()) {
        return;
    }
    QHash<int, PageEdit> &here = m_pending[m_page];
    for (const int index : chosen) {
        PageEdit edit;
        edit.kind = PageEdit::Kind::Delete;
        edit.page = m_page;
        edit.object = index;
        here.insert(index, edit);
    }
    refresh();
}

void ObjectsDialog::restyleChosen()
{
    const QVector<int> chosen = chosenObjects();
    if (chosen.isEmpty()) {
        return;
    }
    if (!m_setFill->isChecked() && !m_setStroke->isChecked() && !m_setWidth->isChecked()) {
        KMessageBox::information(this, i18n("Tick what should change: the fill, the outline or the line width."),
                                 windowTitle());
        return;
    }

    QHash<int, PageEdit> &here = m_pending[m_page];
    for (const int index : chosen) {
        PageEdit edit;
        edit.kind = PageEdit::Kind::Restyle;
        edit.page = m_page;
        edit.object = index;
        if (m_setFill->isChecked()) {
            edit.fill = m_fill->colour();
        }
        if (m_setStroke->isChecked()) {
            edit.stroke = m_stroke->colour();
        }
        edit.lineWidth = m_setWidth->isChecked() ? m_lineWidth->value() : -1.0;
        here.insert(index, edit);
    }
    refresh();
}

void ObjectsDialog::takeBack()
{
    QHash<int, PageEdit> &here = m_pending[m_page];
    for (const int index : chosenObjects()) {
        here.remove(index);
    }
    refresh();
}

void ObjectsDialog::takeBackAll()
{
    m_pending.clear();
    m_insertions.clear();
    refresh();
}

// ── adding something new ──────────────────────────────────────────────────

QWidget *ObjectsDialog::buildAddTab()
{
    auto *tab = new QWidget(this);

    m_shape = new QComboBox(tab);
    m_shape->addItem(i18nc("@item:inlistbox what to put on the page", "Text"),
                     static_cast<int>(NewContent::Kind::Text));
    m_shape->addItem(i18nc("@item:inlistbox what to put on the page", "Picture"),
                     static_cast<int>(NewContent::Kind::Image));
    m_shape->addItem(i18nc("@item:inlistbox what to put on the page", "Rectangle"),
                     static_cast<int>(NewContent::Kind::Rectangle));
    m_shape->addItem(i18nc("@item:inlistbox what to put on the page", "Ellipse"),
                     static_cast<int>(NewContent::Kind::Ellipse));
    m_shape->addItem(i18nc("@item:inlistbox what to put on the page", "Line"),
                     static_cast<int>(NewContent::Kind::Line));

    m_text = new QPlainTextEdit(tab);
    m_text->setPlaceholderText(i18n("One line per line; nothing is wrapped for you"));
    m_text->setMaximumHeight(70);

    m_family = new QComboBox(tab);
    // The three the format itself guarantees: an inserted label has no font file
    // behind it, so anything else would be a name no reader can resolve.
    m_family->addItems({ u"Helvetica"_s, u"Times-Roman"_s, u"Courier"_s });

    m_fontSize = new QDoubleSpinBox(tab);
    m_fontSize->setRange(2, 400);
    m_fontSize->setDecimals(1);
    m_fontSize->setValue(11);
    m_fontSize->setSuffix(i18nc("@item point suffix in a spin box", " pt"));

    auto *fontRow = new QHBoxLayout;
    fontRow->addWidget(m_family, 1);
    fontRow->addWidget(m_fontSize);

    m_image = new QLineEdit(tab);
    m_browse = new QPushButton(i18nc("@action:button", "Choose…"), tab);
    m_quality = new QSpinBox(tab);
    m_quality->setRange(-1, 100);
    m_quality->setValue(-1);
    m_quality->setSpecialValueText(i18nc("@item:valuetext embed the picture untouched", "unchanged"));
    m_quality->setToolTip(i18nc("@info:tooltip",
                                "Left unchanged, the pixels go in as they are. A JPEG quality re-encodes them, "
                                "which is only worth it for a photograph."));

    auto *imageRow = new QHBoxLayout;
    imageRow->addWidget(m_image, 1);
    imageRow->addWidget(m_browse);
    imageRow->addWidget(m_quality);

    m_newFilled = new QCheckBox(i18nc("@option:check", "Filled"), tab);
    m_newFill = new SwatchButton(QColor(240, 200, 60), tab);
    m_newStroke = new SwatchButton(QColor(20, 20, 20), tab);
    m_newWidth = new QDoubleSpinBox(tab);
    m_newWidth->setRange(0, 100);
    m_newWidth->setDecimals(2);
    m_newWidth->setValue(1);
    m_newWidth->setSuffix(i18nc("@item point suffix in a spin box", " pt"));
    m_opacity = new QSpinBox(tab);
    m_opacity->setRange(1, 100);
    m_opacity->setValue(100);
    m_opacity->setSuffix(i18nc("@item percent suffix in a spin box", " %"));

    auto *fillRow = new QHBoxLayout;
    fillRow->addWidget(m_newFilled);
    fillRow->addWidget(m_newFill, 1);

    auto *strokeRow = new QHBoxLayout;
    strokeRow->addWidget(m_newStroke, 1);
    strokeRow->addWidget(m_newWidth);

    auto *form = new QFormLayout;
    form->addRow(i18nc("@label:listbox", "What:"), m_shape);
    form->addRow(i18nc("@label:textbox", "Text:"), m_text);
    form->addRow(i18nc("@label:listbox", "Font:"), fontRow);
    form->addRow(i18nc("@label:textbox", "Picture:"), imageRow);
    form->addRow(i18nc("@label:chooser", "Fill:"), fillRow);
    form->addRow(i18nc("@label:chooser", "Outline:"), strokeRow);
    form->addRow(i18nc("@label:spinbox", "Opacity:"), m_opacity);

    auto *hint = new QLabel(i18n("Drag a rectangle on the page to put it there. A line runs from the bottom left of "
                                 "that rectangle to its top right."),
                            tab);
    hint->setWordWrap(true);

    m_waiting = new QLabel(tab);
    auto *drop
        = new QPushButton(QIcon::fromTheme(u"edit-undo"_s), i18nc("@action:button", "Take Back the Last One"), tab);

    auto *waitingRow = new QHBoxLayout;
    waitingRow->addWidget(m_waiting, 1);
    waitingRow->addWidget(drop);

    auto *layout = new QVBoxLayout(tab);
    layout->addLayout(form);
    layout->addWidget(hint);
    layout->addLayout(waitingRow);
    layout->addStretch(1);

    const auto followShape = [this] {
        const auto kind = static_cast<NewContent::Kind>(m_shape->currentData().toInt());
        const bool text = kind == NewContent::Kind::Text;
        const bool picture = kind == NewContent::Kind::Image;
        const bool area = kind == NewContent::Kind::Rectangle || kind == NewContent::Kind::Ellipse;
        m_text->setEnabled(text);
        m_family->setEnabled(text);
        m_fontSize->setEnabled(text);
        m_image->setEnabled(picture);
        m_browse->setEnabled(picture);
        m_quality->setEnabled(picture);
        // A picture carries its own colours, a line has nothing to fill, and for
        // text the fill is what the letters are written in.
        m_newFilled->setEnabled(area);
        m_newFill->setEnabled(area || text);
        m_newStroke->setEnabled(area || kind == NewContent::Kind::Line);
        m_newWidth->setEnabled(area || kind == NewContent::Kind::Line);
    };
    connect(m_shape, &QComboBox::currentIndexChanged, this, followShape);
    followShape();

    connect(m_browse, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getOpenFileName(this, i18nc("@title:window", "Picture"), QString(),
                                                          i18n("Images (*.png *.jpg *.jpeg *.tif *.tiff);;"
                                                               "All files (*)"));
        if (!path.isEmpty()) {
            m_image->setText(path);
        }
    });
    connect(drop, &QPushButton::clicked, this, [this] {
        if (!m_insertions.isEmpty()) {
            m_insertions.removeLast();
            refresh();
        }
    });
    return tab;
}

void ObjectsDialog::place(const QRectF &where)
{
    NewContent item;
    item.kind = static_cast<NewContent::Kind>(m_shape->currentData().toInt());
    item.page = m_page;
    item.rect = where;
    item.lineWidth = m_newWidth->value();
    item.opacity = m_opacity->value() / 100.0;
    item.stroke = m_newStroke->colour();
    if (m_newFilled->isChecked() || item.kind == NewContent::Kind::Text) {
        item.fill = m_newFill->colour();
    }

    if (item.kind == NewContent::Kind::Text) {
        item.text = m_text->toPlainText();
        if (item.text.trimmed().isEmpty()) {
            KMessageBox::information(this, i18n("Type what it should say first."), windowTitle());
            return;
        }
        item.fontFamily = m_family->currentText();
        item.fontSize = m_fontSize->value();
    }

    if (item.kind == NewContent::Kind::Image) {
        // Read now rather than at writing time: a path that turns out not to be a
        // picture is worth knowing about while the mouse is still on the spot.
        item.image = QImage(m_image->text());
        if (item.image.isNull()) {
            KMessageBox::error(this,
                               m_image->text().isEmpty()
                                   ? i18n("Choose a picture first.")
                                   : i18n("“%1” could not be read as a picture.", m_image->text()),
                               windowTitle());
            return;
        }
        item.jpegQuality = m_quality->value();
    }

    m_insertions.append(item);
    refresh();
}

// ── writing it out ────────────────────────────────────────────────────────

void ObjectsDialog::write()
{
    QVector<PageEdit> edits;
    for (auto page = m_pending.constBegin(); page != m_pending.constEnd(); ++page) {
        for (auto edit = page->constBegin(); edit != page->constEnd(); ++edit) {
            edits.append(*edit);
        }
    }

    if (edits.isEmpty() && m_insertions.isEmpty()) {
        KMessageBox::information(this, i18n("Nothing is waiting to be changed, so there is nothing to write."),
                                 windowTitle());
        return;
    }

    const bool written = runProducing(
        m_document, this, windowTitle(), u"-objects.pdf"_s,
        [this, edits](const QString &out, QStringList *summary, QString *error) {
            PageComposer::Report report;
            QApplication::setOverrideCursor(Qt::WaitCursor);
            const bool ok = PageComposer::apply(m_pdf, out, edits, m_insertions, &report, error);
            QApplication::restoreOverrideCursor();
            if (!ok) {
                return false;
            }

            QStringList did;
            if (report.transformed > 0) {
                did += i18np("Moved one object.", "Moved %1 objects.", report.transformed);
            }
            if (report.deleted > 0) {
                did += i18np("Took one object off the page.", "Took %1 objects off the page.", report.deleted);
            }
            if (report.restyled > 0) {
                did += i18np("Recoloured one object.", "Recoloured %1 objects.", report.restyled);
            }
            if (report.inserted > 0) {
                did += i18np("Added one thing.", "Added %1 things.", report.inserted);
            }
            *summary += did.isEmpty() ? i18n("Nothing on the page changed. The file was still written, unchanged.")
                                      : did.join(u' ');

            // What was asked for and could not be done belongs in
            // the same breath as what was: a count on its own lets
            // a half-finished run look like a finished one.
            for (const QString &refusal : std::as_const(report.refusals)) {
                *summary += refusal;
            }
            return true;
        });

    // The waiting changes are in the new file now, and the numbers they were
    // written against belong to the old one, so this window has done its work.
    if (written) {
        accept();
    }
}

void ObjectsDialog::showLimits()
{
    QStringList lines { i18n("Reading a page as objects:") };
    for (const QString &limit : PageObjects::limitations()) {
        lines += u"  • "_s + limit;
    }
    lines += QString();
    lines += i18n("Changing what is on a page:");
    for (const QString &limit : PageComposer::limitations()) {
        lines += u"  • "_s + limit;
    }
    showReport(this, i18nc("@title:window", "What This Cannot Do"), lines);
}

} // namespace

void showObjects(Document *document, RenderBackend *backend, int page, QWidget *parent)
{
    const QString pdf = savedPath(document, parent);
    if (pdf.isEmpty()) {
        return;
    }
    ObjectsDialog(document, backend, pdf, page, parent).exec();
}

} // namespace ps::tools
