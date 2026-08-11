/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "ToolDialogs.h"

#include "FormFieldView.h"
#include "ToolSupport.h"
#include "core/Document.h"
#include "core/FormBuilder.h"
#include "core/Forms.h"
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
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMouseEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>

#include <functional>
#include <utility>

using namespace Qt::Literals::StringLiterals;

namespace ps::tools {

namespace {

using Field = FormBuilder::Field;

QImage rotated(const QImage &image, int degrees)
{
    if (degrees == 0 || image.isNull()) {
        return image;
    }
    return image.transformed(QTransform().rotate(degrees), Qt::SmoothTransformation);
}

QString points(double value)
{
    return QLocale().toString(value, 'f', 1);
}

QString describeRect(const QRectF &rect)
{
    return i18nc("@item where a field sits and how big it is, in points", "%1,%2 · %3 × %4 pt", points(rect.x()),
                 points(rect.y()), points(rect.width()), points(rect.height()));
}

QString describeKind(Field::Kind kind)
{
    switch (kind) {
    case Field::Kind::Text:
        return i18nc("@item:inlistbox kind of form field", "Text");
    case Field::Kind::Checkbox:
        return i18nc("@item:inlistbox kind of form field", "Tick box");
    case Field::Kind::Radio:
        return i18nc("@item:inlistbox kind of form field", "Radio group");
    case Field::Kind::Dropdown:
        return i18nc("@item:inlistbox kind of form field", "Drop-down");
    case Field::Kind::ListBox:
        return i18nc("@item:inlistbox kind of form field", "List");
    case Field::Kind::PushButton:
        return i18nc("@item:inlistbox kind of form field", "Button");
    case Field::Kind::Signature:
        return i18nc("@item:inlistbox kind of form field", "Signature");
    }
    return {};
}

QStringList linesOf(const QPlainTextEdit *edit)
{
    QStringList lines;
    const QStringList given = edit->toPlainText().split(u'\n', Qt::SkipEmptyParts);
    for (const QString &line : given) {
        const QString trimmed = line.trimmed();
        if (!trimmed.isEmpty()) {
            lines << trimmed;
        }
    }
    return lines;
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
 * The page, the fields it already has, and a rectangle drawn with the mouse.
 *
 * ps::FormFieldView already draws a page's fields and owns the sums between
 * pixels and points, so placing a new field is that plus a rubber band. The
 * band comes back in points (what ps::FormBuilder takes and what the boxes
 * beside the page show), so drawing a field and typing where it goes are two
 * ways of saying the same number rather than two roundings of it.
 */
class PlacementView : public FormFieldView
{
public:
    explicit PlacementView(QWidget *parent = nullptr)
        : FormFieldView(parent)
    {
        setCursor(Qt::CrossCursor);
    }

    void onRectangleDrawn(std::function<void(const QRectF &)> handler) { m_drawn = std::move(handler); }

    /** The rectangle the boxes beside the page describe, in points. */
    void setDraft(const QRectF &rectInPoints)
    {
        m_draft = rectInPoints;
        update();
    }

protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton || !hasPage()) {
            FormFieldView::mousePressEvent(event);
            return;
        }
        m_anchor = event->pos();
        m_band = QRect(m_anchor, m_anchor);
        m_dragging = true;
        update();
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (!m_dragging) {
            FormFieldView::mouseMoveEvent(event);
            return;
        }
        m_band = QRect(m_anchor, event->pos()).normalized();
        update();
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (!m_dragging) {
            FormFieldView::mouseReleaseEvent(event);
            return;
        }
        m_dragging = false;
        const QRect band = m_band;
        m_band = QRect();
        update();

        // Under a few pixels the user was missing rather than aiming, and a
        // field a tenth of a point across is a field nobody can find again.
        if (band.width() < 6 || band.height() < 6) {
            return;
        }
        if (m_drawn) {
            m_drawn(toPoints(fromWidget(band)));
        }
    }

    void paintEvent(QPaintEvent *event) override
    {
        FormFieldView::paintEvent(event);
        if (!hasPage()) {
            return;
        }

        const QRect draft = m_dragging ? m_band : toWidget(fromPoints(m_draft));
        if (draft.isEmpty()) {
            return;
        }

        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        QColor tint(60, 150, 90);
        painter.setPen(QPen(tint, 2.0, Qt::DashLine));
        tint.setAlpha(45);
        painter.setBrush(tint);
        painter.drawRect(draft);
    }

private:
    std::function<void(const QRectF &)> m_drawn;
    QRectF m_draft;
    QPoint m_anchor;
    QRect m_band;
    bool m_dragging = false;
};

/**
 * Putting fields on a document, rather than filling in someone else's.
 *
 * The tab that matters is the first one, and it is the reason this tool is
 * given a page and a renderer at all: a field is a rectangle on a piece of
 * paper, and every other way of describing one (four numbers, a name, a list
 * of coordinates in a script) is a way of not looking at the paper. Dragging
 * the box where it belongs and reading the same rectangle back in points is one
 * gesture instead of two guesses.
 *
 * New fields are collected in a list and written in one go. Each of the writes
 * here produces a *new* file, so adding six fields one at a time would leave
 * six files and five superseded ones, and the sixth would be built on the
 * first, not on the fifth.
 */
class FormBuilderDialog : public QDialog
{
public:
    FormBuilderDialog(Document *document, RenderBackend *backend, const QString &pdf, int page, QWidget *parent);

private:
    QWidget *buildPlace();
    QWidget *buildInventory();
    QWidget *buildOrder();
    QWidget *buildBehaviour();
    QWidget *buildData();

    QGroupBox *buildWhat(QWidget *parent);
    QGroupBox *buildWhere(QWidget *parent);
    QGroupBox *buildChoices(QWidget *parent);
    QGroupBox *buildLook(QWidget *parent);

    void readFields();
    void fillInventory();
    void fillNameChoosers();
    void fillOrderList();
    void showPage(int page);
    void refreshOverlay();
    void followKind();

    QRectF currentRectangle() const;
    void takeRectangle(const QRectF &rect);
    bool describedFields(QVector<Field> *fields, QString *error) const;
    void queueField();
    void fillQueue();
    void writeQueued();

    QStringList tickedNames() const;
    void removeTicked();
    void renameChosen();
    void applyOrder();

    void applyFormat();
    void applyValidation();
    void applyCalculation();
    void applyButtonAction();

    void exportAnswers();
    void importAnswers();
    void collectAnswers();
    void copyFromTemplate();

    /**
     * Closes the dialog once a tool has written a new file.
     *
     * Everything here works on the file as it stood when the dialog opened. A
     * second change made afterwards would be made to that older file and would
     * quietly drop what the first one did, so the dialog goes when the document
     * it was showing stops being the current one.
     */
    void closeAfterWriting(bool written);

    Document *m_document = nullptr;
    RenderBackend *m_backend = nullptr;
    QString m_pdf;
    int m_shown = 0;

    QVector<FormField> m_existing;
    QVector<Field> m_pending;

    PlacementView *m_view = nullptr;
    QSpinBox *m_pageBox = nullptr;
    QLabel *m_hint = nullptr;

    QComboBox *m_kind = nullptr;
    QLineEdit *m_name = nullptr;
    QLineEdit *m_label = nullptr;
    QLineEdit *m_value = nullptr;

    QDoubleSpinBox *m_x = nullptr;
    QDoubleSpinBox *m_y = nullptr;
    QDoubleSpinBox *m_width = nullptr;
    QDoubleSpinBox *m_height = nullptr;

    QCheckBox *m_required = nullptr;
    QCheckBox *m_readOnly = nullptr;
    QCheckBox *m_multiline = nullptr;
    QCheckBox *m_password = nullptr;
    QCheckBox *m_comb = nullptr;
    QCheckBox *m_editable = nullptr;
    QCheckBox *m_multiSelect = nullptr;
    QSpinBox *m_maxLength = nullptr;

    QGroupBox *m_choicesBox = nullptr;
    QPlainTextEdit *m_options = nullptr;
    QPlainTextEdit *m_exports = nullptr;
    QLineEdit *m_onState = nullptr;
    QGroupBox *m_buttonsBox = nullptr;
    QListWidget *m_boxes = nullptr;

    QComboBox *m_font = nullptr;
    QDoubleSpinBox *m_fontSize = nullptr;
    QComboBox *m_align = nullptr;
    SwatchButton *m_ink = nullptr;
    QCheckBox *m_fill = nullptr;
    SwatchButton *m_background = nullptr;
    QCheckBox *m_frame = nullptr;
    SwatchButton *m_edge = nullptr;
    QDoubleSpinBox *m_borderWidth = nullptr;
    QComboBox *m_borderStyle = nullptr;

    QTableWidget *m_queue = nullptr;
    QTableWidget *m_inventory = nullptr;

    QSpinBox *m_orderPage = nullptr;
    QListWidget *m_order = nullptr;

    QComboBox *m_formatField = nullptr;
    QComboBox *m_format = nullptr;
    QSpinBox *m_decimals = nullptr;
    QLineEdit *m_symbol = nullptr;

    QComboBox *m_validateField = nullptr;
    QDoubleSpinBox *m_minimum = nullptr;
    QDoubleSpinBox *m_maximum = nullptr;

    QComboBox *m_calcField = nullptr;
    QComboBox *m_calcHow = nullptr;
    QListWidget *m_calcSources = nullptr;

    QComboBox *m_buttonField = nullptr;
    QComboBox *m_buttonAction = nullptr;
    QLineEdit *m_buttonTarget = nullptr;
};

FormBuilderDialog::FormBuilderDialog(Document *document, RenderBackend *backend, const QString &pdf, int page,
                                     QWidget *parent)
    : QDialog(parent)
    , m_document(document)
    , m_backend(backend)
    , m_pdf(pdf)
    , m_shown(qBound(0, page, document ? qMax(0, document->pageCount() - 1) : 0))
{
    setWindowTitle(i18nc("@title:window", "Build a Form"));
    resize(1180, 820);

    auto *tabs = new QTabWidget(this);
    tabs->addTab(buildPlace(), i18nc("@title:tab", "Place a Field"));
    tabs->addTab(buildInventory(), i18nc("@title:tab", "The Fields It Has"));
    tabs->addTab(buildOrder(), i18nc("@title:tab", "Tab Order"));
    tabs->addTab(buildBehaviour(), i18nc("@title:tab", "Behaviour"));
    tabs->addTab(buildData(), i18nc("@title:tab", "Answers"));

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(tabs, 1);
    layout->addWidget(buttons);

    // Reading the fields is also what puts the page on the canvas, so the tool
    // opens showing the page the user had selected with its form already on it.
    readFields();
    followKind();
}

// ── Place a field ─────────────────────────────────────────────────────────

QWidget *FormBuilderDialog::buildPlace()
{
    auto *page = new QWidget(this);

    auto *stage = new QWidget(page);
    auto *previous = new QPushButton(i18nc("@action:button go to the previous page", "Previous"), stage);
    auto *next = new QPushButton(i18nc("@action:button go to the next page", "Next"), stage);
    const int count = m_document ? m_document->pageCount() : 0;

    m_pageBox = new QSpinBox(stage);
    m_pageBox->setRange(1, qMax(1, count));
    m_pageBox->setPrefix(i18nc("@label:spinbox prefix before a page number", "Page "));

    auto *navigation = new QHBoxLayout;
    navigation->addWidget(previous);
    navigation->addWidget(m_pageBox);
    navigation->addWidget(new QLabel(i18nc("@info total page count", "of %1", count), stage));
    navigation->addStretch(1);

    m_view = new PlacementView(stage);
    m_hint = new QLabel(stage);
    m_hint->setWordWrap(true);

    auto *stageLayout = new QVBoxLayout(stage);
    stageLayout->setContentsMargins(0, 0, 0, 0);
    stageLayout->addLayout(navigation);
    stageLayout->addWidget(m_view, 1);
    stageLayout->addWidget(m_hint);

    auto *sheet = new QWidget(page);
    auto *sheetLayout = new QVBoxLayout(sheet);
    sheetLayout->setContentsMargins(0, 0, 0, 0);
    sheetLayout->addWidget(buildWhat(sheet));
    sheetLayout->addWidget(buildWhere(sheet));
    sheetLayout->addWidget(buildChoices(sheet));
    sheetLayout->addWidget(buildLook(sheet));
    sheetLayout->addStretch(1);

    auto *scroll = new QScrollArea(page);
    scroll->setWidget(sheet);
    scroll->setWidgetResizable(true);
    scroll->setMinimumWidth(430);

    auto *splitter = new QSplitter(Qt::Horizontal, page);
    splitter->addWidget(stage);
    splitter->addWidget(scroll);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);

    m_queue = new QTableWidget(0, 4, page);
    m_queue->setHorizontalHeaderLabels({ i18nc("@title:column", "Name"), i18nc("@title:column", "Kind"),
                                         i18nc("@title:column", "Page"), i18nc("@title:column", "Where") });
    m_queue->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_queue->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    m_queue->verticalHeader()->hide();
    m_queue->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_queue->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_queue->setMaximumHeight(160);

    auto *add = new QPushButton(QIcon::fromTheme(u"list-add"_s), i18nc("@action:button", "Add to the List"), page);
    auto *drop
        = new QPushButton(QIcon::fromTheme(u"list-remove"_s), i18nc("@action:button", "Take Off the List"), page);
    auto *write = new QPushButton(QIcon::fromTheme(u"document-save"_s),
                                  i18nc("@action:button", "Put Them on the Document"), page);

    auto *queueButtons = new QHBoxLayout;
    queueButtons->addWidget(add);
    queueButtons->addWidget(drop);
    queueButtons->addStretch(1);
    queueButtons->addWidget(write);

    auto *queueBox = new QGroupBox(i18nc("@title:group", "Waiting to be put on"), page);
    auto *queueLayout = new QVBoxLayout(queueBox);
    queueLayout->addWidget(m_queue);
    queueLayout->addLayout(queueButtons);

    auto *layout = new QVBoxLayout(page);
    layout->addWidget(splitter, 1);
    layout->addWidget(queueBox);

    connect(previous, &QPushButton::clicked, this, [this] { m_pageBox->setValue(m_pageBox->value() - 1); });
    connect(next, &QPushButton::clicked, this, [this] { m_pageBox->setValue(m_pageBox->value() + 1); });
    connect(m_pageBox, &QSpinBox::valueChanged, this, [this](int value) { showPage(value - 1); });
    connect(add, &QPushButton::clicked, this, [this] { queueField(); });
    connect(drop, &QPushButton::clicked, this, [this] {
        const int row = m_queue->currentRow();
        if (row >= 0 && row < m_pending.size()) {
            m_pending.remove(row);
            fillQueue();
        }
    });
    connect(write, &QPushButton::clicked, this, [this] { writeQueued(); });

    m_view->onRectangleDrawn([this](const QRectF &rect) { takeRectangle(rect); });
    return page;
}

QGroupBox *FormBuilderDialog::buildWhat(QWidget *parent)
{
    auto *box = new QGroupBox(i18nc("@title:group", "The field"), parent);

    m_kind = new QComboBox(box);
    for (Field::Kind kind : { Field::Kind::Text, Field::Kind::Checkbox, Field::Kind::Radio, Field::Kind::Dropdown,
                              Field::Kind::ListBox, Field::Kind::PushButton, Field::Kind::Signature }) {
        m_kind->addItem(describeKind(kind), static_cast<int>(kind));
    }

    m_name = new QLineEdit(box);
    m_name->setPlaceholderText(i18nc("@info:placeholder an example of a field name", "Adresse.Strasse"));
    m_name->setToolTip(i18nc("@info:tooltip",
                             "The name filling in addresses. A full stop in it makes a group, so that "
                             "Adresse.Strasse and Adresse.Ort belong together."));

    m_label = new QLineEdit(box);
    m_label->setToolTip(i18nc("@info:tooltip",
                              "What a reader shows next to the field. On a push button it is the caption "
                              "written across it."));

    m_value = new QLineEdit(box);
    m_value->setToolTip(i18nc("@info:tooltip", "What the field holds before anybody types in it."));

    m_required = new QCheckBox(i18nc("@option:check", "Must be filled in"), box);
    m_readOnly = new QCheckBox(i18nc("@option:check", "Shown, but nobody can change it"), box);
    m_multiline = new QCheckBox(i18nc("@option:check", "Several lines"), box);
    m_password = new QCheckBox(i18nc("@option:check", "Hide what is typed"), box);
    m_comb = new QCheckBox(i18nc("@option:check", "One character per cell"), box);
    m_editable = new QCheckBox(i18nc("@option:check", "Also takes typing"), box);
    m_multiSelect = new QCheckBox(i18nc("@option:check", "Several entries may be picked"), box);

    m_maxLength = new QSpinBox(box);
    m_maxLength->setRange(0, 4096);
    m_maxLength->setSpecialValueText(i18nc("@item no limit on the length of a field", "No limit"));

    auto *form = new QFormLayout(box);
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    form->addRow(i18nc("@label:listbox", "Kind:"), m_kind);
    form->addRow(i18nc("@label:textbox", "Name:"), m_name);
    form->addRow(i18nc("@label:textbox what a reader shows next to a field", "Label:"), m_label);
    form->addRow(i18nc("@label:textbox", "Starts as:"), m_value);
    form->addRow(QString(), m_required);
    form->addRow(QString(), m_readOnly);
    form->addRow(QString(), m_multiline);
    form->addRow(QString(), m_password);
    form->addRow(QString(), m_comb);
    form->addRow(QString(), m_editable);
    form->addRow(QString(), m_multiSelect);
    form->addRow(i18nc("@label:spinbox", "At most:"), m_maxLength);

    connect(m_kind, &QComboBox::currentIndexChanged, this, [this] { followKind(); });
    return box;
}

QGroupBox *FormBuilderDialog::buildWhere(QWidget *parent)
{
    auto *box = new QGroupBox(i18nc("@title:group", "Where it goes"), parent);

    const auto measure = [box](double value) {
        auto *spin = new QDoubleSpinBox(box);
        spin->setRange(0.0, 20000.0);
        spin->setDecimals(1);
        spin->setSingleStep(1.0);
        spin->setValue(value);
        spin->setSuffix(i18nc("@item point suffix in a spin box", " pt"));
        return spin;
    };
    m_x = measure(72.0);
    m_y = measure(700.0);
    m_width = measure(200.0);
    m_height = measure(18.0);

    auto *form = new QFormLayout(box);
    form->addRow(i18nc("@label:spinbox distance from the left edge", "From the left:"), m_x);
    form->addRow(i18nc("@label:spinbox distance from the bottom edge", "From the bottom:"), m_y);
    form->addRow(i18nc("@label:spinbox", "Width:"), m_width);
    form->addRow(i18nc("@label:spinbox", "Height:"), m_height);

    const auto follow = [this] { m_view->setDraft(currentRectangle()); };
    for (QDoubleSpinBox *spin : { m_x, m_y, m_width, m_height }) {
        connect(spin, &QDoubleSpinBox::valueChanged, this, follow);
    }
    return box;
}

QGroupBox *FormBuilderDialog::buildChoices(QWidget *parent)
{
    m_choicesBox = new QGroupBox(i18nc("@title:group", "What may be chosen"), parent);

    m_options = new QPlainTextEdit(m_choicesBox);
    m_options->setPlaceholderText(i18n("One choice per line"));
    m_options->setMaximumHeight(90);

    m_exports = new QPlainTextEdit(m_choicesBox);
    m_exports->setPlaceholderText(i18n("Left empty, the choice itself is stored"));
    m_exports->setMaximumHeight(90);

    m_onState = new QLineEdit(u"/Yes"_s, m_choicesBox);
    m_onState->setToolTip(i18nc("@info:tooltip", "What a ticked box stores. Anything but /Off means ticked."));

    m_boxes = new QListWidget(m_choicesBox);
    m_boxes->setMaximumHeight(90);

    auto *clear = new QPushButton(i18nc("@action:button", "Forget the Boxes"), m_choicesBox);
    auto *dropBox = new QPushButton(i18nc("@action:button", "Take This Box Out"), m_choicesBox);
    auto *boxButtons = new QHBoxLayout;
    boxButtons->addWidget(dropBox);
    boxButtons->addWidget(clear);
    boxButtons->addStretch(1);

    m_buttonsBox = new QGroupBox(i18nc("@title:group", "The buttons"), m_choicesBox);
    auto *buttonsLayout = new QVBoxLayout(m_buttonsBox);
    auto *buttonsNote = new QLabel(i18n("A radio group is one field with a box for each of its buttons, which is "
                                        "what makes them take it in turns instead of toggling as one. Draw one "
                                        "box per choice, in the order the choices are written above."),
                                   m_buttonsBox);
    buttonsNote->setWordWrap(true);
    buttonsLayout->addWidget(buttonsNote);
    buttonsLayout->addWidget(m_boxes);
    buttonsLayout->addLayout(boxButtons);

    auto *form = new QFormLayout(m_choicesBox);
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    form->addRow(i18nc("@label:textbox", "Choices:"), m_options);
    form->addRow(i18nc("@label:textbox what is stored for each choice", "Stored as:"), m_exports);
    form->addRow(i18nc("@label:textbox", "A tick stores:"), m_onState);
    form->addRow(m_buttonsBox);

    connect(clear, &QPushButton::clicked, this, [this] { m_boxes->clear(); });
    connect(dropBox, &QPushButton::clicked, this, [this] { delete m_boxes->takeItem(m_boxes->currentRow()); });
    return m_choicesBox;
}

QGroupBox *FormBuilderDialog::buildLook(QWidget *parent)
{
    auto *box = new QGroupBox(i18nc("@title:group", "How it looks"), parent);

    // The fourteen resource names and the fonts behind them are product names,
    // so they stay as they are in every language.
    struct {
        const char16_t *shown;
        const char16_t *resource;
    } const fonts[] = {
        { u"Helvetica", u"Helv" },
        { u"Helvetica Bold", u"HeBo" },
        { u"Helvetica Oblique", u"HeOb" },
        { u"Helvetica Bold Oblique", u"HeBO" },
        { u"Times Roman", u"TiRo" },
        { u"Times Bold", u"TiBo" },
        { u"Times Italic", u"TiIt" },
        { u"Times Bold Italic", u"TiBI" },
        { u"Courier", u"Cour" },
        { u"Courier Bold", u"CoBo" },
        { u"Courier Oblique", u"CoOb" },
        { u"Courier Bold Oblique", u"CoBO" },
        { u"Symbol", u"Symb" },
        { u"ZapfDingbats", u"ZaDb" },
    };

    m_font = new QComboBox(box);
    for (const auto &entry : fonts) {
        m_font->addItem(QString::fromUtf16(entry.shown), QString::fromUtf16(entry.resource));
    }
    m_font->setToolTip(i18nc("@info:tooltip",
                             "Only the fourteen fonts every reader already knows. A font of your own would "
                             "have to be embedded, which is a separate job."));

    m_fontSize = new QDoubleSpinBox(box);
    m_fontSize->setRange(0.0, 144.0);
    m_fontSize->setDecimals(1);
    m_fontSize->setSpecialValueText(i18nc("@item a text size the reader works out itself", "Fit to the box"));
    m_fontSize->setSuffix(i18nc("@item point suffix in a spin box", " pt"));

    m_align = new QComboBox(box);
    m_align->addItem(i18nc("@item:inlistbox where the text sits", "Left"), int(Qt::AlignLeft));
    m_align->addItem(i18nc("@item:inlistbox where the text sits", "Centred"), int(Qt::AlignHCenter));
    m_align->addItem(i18nc("@item:inlistbox where the text sits", "Right"), int(Qt::AlignRight));

    m_ink = new SwatchButton(QColor(0, 0, 0), box);

    m_fill = new QCheckBox(i18nc("@option:check", "Fill the box"), box);
    m_background = new SwatchButton(QColor(240, 240, 240), box);
    auto *fillRow = new QHBoxLayout;
    fillRow->addWidget(m_fill);
    fillRow->addWidget(m_background, 1);

    m_frame = new QCheckBox(i18nc("@option:check", "Draw a frame"), box);
    m_frame->setChecked(true);
    m_edge = new SwatchButton(QColor(60, 60, 60), box);
    auto *frameRow = new QHBoxLayout;
    frameRow->addWidget(m_frame);
    frameRow->addWidget(m_edge, 1);

    m_borderWidth = new QDoubleSpinBox(box);
    m_borderWidth->setRange(0.0, 12.0);
    m_borderWidth->setDecimals(1);
    m_borderWidth->setSingleStep(0.5);
    m_borderWidth->setValue(1.0);
    m_borderWidth->setSuffix(i18nc("@item point suffix in a spin box", " pt"));

    m_borderStyle = new QComboBox(box);
    m_borderStyle->addItem(i18nc("@item:inlistbox kind of frame", "Solid"), u"solid"_s);
    m_borderStyle->addItem(i18nc("@item:inlistbox kind of frame", "Dashed"), u"dashed"_s);
    m_borderStyle->addItem(i18nc("@item:inlistbox kind of frame", "Raised"), u"beveled"_s);
    m_borderStyle->addItem(i18nc("@item:inlistbox kind of frame", "Sunken"), u"inset"_s);
    m_borderStyle->addItem(i18nc("@item:inlistbox kind of frame", "Underline only"), u"underline"_s);

    auto *note = new QLabel(i18n("A field with no drawing of its own is invisible in about half the readers in "
                                 "use, including most of the ones that print, so a frame or a filled box is "
                                 "worth keeping."),
                            box);
    note->setWordWrap(true);

    auto *form = new QFormLayout(box);
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    form->addRow(i18nc("@label:listbox", "Font:"), m_font);
    form->addRow(i18nc("@label:spinbox", "Size:"), m_fontSize);
    form->addRow(i18nc("@label:listbox", "Text sits:"), m_align);
    form->addRow(i18nc("@label:chooser", "Text colour:"), m_ink);
    form->addRow(QString(), fillRow);
    form->addRow(QString(), frameRow);
    form->addRow(i18nc("@label:spinbox", "Frame width:"), m_borderWidth);
    form->addRow(i18nc("@label:listbox", "Frame:"), m_borderStyle);
    form->addRow(note);

    return box;
}

void FormBuilderDialog::followKind()
{
    const auto kind = static_cast<Field::Kind>(m_kind->currentData().toInt());
    const bool text = kind == Field::Kind::Text;
    const bool listed = kind == Field::Kind::Dropdown || kind == Field::Kind::ListBox;
    const bool radio = kind == Field::Kind::Radio;
    const bool tick = kind == Field::Kind::Checkbox;
    const bool holdsValue = kind != Field::Kind::PushButton && kind != Field::Kind::Signature;

    m_multiline->setEnabled(text);
    m_password->setEnabled(text);
    m_comb->setEnabled(text);
    m_maxLength->setEnabled(text);
    m_editable->setEnabled(kind == Field::Kind::Dropdown);
    m_multiSelect->setEnabled(kind == Field::Kind::ListBox);
    m_value->setEnabled(holdsValue && !radio);
    // A signature field holds no value but may well be the thing without which
    // the form is not finished, so it keeps the required box.
    m_required->setEnabled(kind != Field::Kind::PushButton);

    m_choicesBox->setVisible(listed || radio || tick);
    m_options->setEnabled(listed || radio);
    m_exports->setEnabled(listed || radio);
    m_onState->setEnabled(tick);
    m_buttonsBox->setVisible(radio);

    m_hint->setText(radio ? i18n("Drag one box on the page for each choice, in the order the choices are written. "
                                 "The boxes gather in the list on the right.")
                          : i18n("Drag the box where the field goes, or type the four numbers on the right. The "
                                 "faint boxes are the fields this page already has."));
}

QRectF FormBuilderDialog::currentRectangle() const
{
    return QRectF(m_x->value(), m_y->value(), m_width->value(), m_height->value());
}

void FormBuilderDialog::takeRectangle(const QRectF &rect)
{
    const QSignalBlocker blockX(m_x);
    const QSignalBlocker blockY(m_y);
    const QSignalBlocker blockWidth(m_width);
    const QSignalBlocker blockHeight(m_height);
    m_x->setValue(rect.x());
    m_y->setValue(rect.y());
    m_width->setValue(rect.width());
    m_height->setValue(rect.height());
    m_view->setDraft(rect);

    if (static_cast<Field::Kind>(m_kind->currentData().toInt()) == Field::Kind::Radio) {
        auto *item = new QListWidgetItem(describeRect(rect), m_boxes);
        item->setData(Qt::UserRole, QVariant::fromValue(rect));
    }
}

bool FormBuilderDialog::describedFields(QVector<Field> *fields, QString *error) const
{
    Field spec;
    spec.kind = static_cast<Field::Kind>(m_kind->currentData().toInt());
    spec.name = m_name->text().trimmed();
    if (spec.name.isEmpty()) {
        *error = i18n("Give the field a name. That is the name filling in addresses, and a full stop in it makes "
                      "a group: Adresse.Strasse and Adresse.Ort belong together.");
        return false;
    }
    for (const FormField &already : m_existing) {
        if (already.name == spec.name) {
            *error = i18n("This document already has a field called “%1”.", spec.name);
            return false;
        }
    }
    // Every button of a radio group is queued in one go under the group's name,
    // so a second entry under a name already in the list is always a clash and
    // never the second button of the group being described.
    for (const Field &waiting : m_pending) {
        if (waiting.name == spec.name) {
            *error = i18n("“%1” is already waiting to be put on the document.", spec.name);
            return false;
        }
    }

    spec.page = m_pageBox->value() - 1;
    spec.label = m_label->text().trimmed();
    spec.defaultValue = m_value->text();
    spec.required = m_required->isChecked() && m_required->isEnabled();
    spec.readOnly = m_readOnly->isChecked();
    spec.multiline = m_multiline->isChecked() && m_multiline->isEnabled();
    spec.password = m_password->isChecked() && m_password->isEnabled();
    spec.comb = m_comb->isChecked() && m_comb->isEnabled();
    spec.editable = m_editable->isChecked() && m_editable->isEnabled();
    spec.multiSelect = m_multiSelect->isChecked() && m_multiSelect->isEnabled();
    spec.maxLength = m_maxLength->isEnabled() ? m_maxLength->value() : 0;
    spec.options = linesOf(m_options);
    spec.exportValues = linesOf(m_exports);
    spec.fontName = m_font->currentData().toString();
    spec.fontSize = m_fontSize->value();
    spec.textColour = m_ink->colour();
    spec.backgroundColour = m_fill->isChecked() ? m_background->colour() : QColor();
    spec.borderColour = m_frame->isChecked() ? m_edge->colour() : QColor();
    spec.borderWidth = m_borderWidth->value();
    spec.borderStyle = m_borderStyle->currentData().toString();
    spec.alignment = static_cast<Qt::Alignment>(m_align->currentData().toInt());
    if (m_onState->isEnabled() && !m_onState->text().trimmed().isEmpty()) {
        spec.onState = m_onState->text().trimmed();
    }

    if (spec.comb && spec.maxLength == 0) {
        *error = i18n("A field of one character per cell needs to know how many cells, so give it a length.");
        return false;
    }
    if ((spec.kind == Field::Kind::Dropdown || spec.kind == Field::Kind::ListBox) && spec.options.isEmpty()) {
        *error = i18n("A list needs something to choose from. Write one choice per line.");
        return false;
    }

    if (spec.kind == Field::Kind::Radio) {
        if (spec.options.size() < 2) {
            *error = i18n("A radio group needs at least two choices, one per line.");
            return false;
        }
        if (m_boxes->count() != spec.options.size()) {
            *error = i18np("A radio group needs one box for each of its choices: %2 drawn for one choice.",
                           "A radio group needs one box for each of its choices: %2 drawn for %1 choices.",
                           int(spec.options.size()), m_boxes->count());
            return false;
        }
        for (int i = 0; i < spec.options.size(); ++i) {
            Field button = spec;
            button.radioGroup = spec.name;
            button.rect = m_boxes->item(i)->data(Qt::UserRole).toRectF();
            button.options.clear();
            button.exportValues.clear();
            // What this button stores, and what a reader shows beside it. The
            // label is the only place the order of the buttons is written down.
            button.onState = i < spec.exportValues.size() ? spec.exportValues.at(i) : spec.options.at(i);
            button.label = spec.options.at(i);
            fields->append(button);
        }
        return true;
    }

    const QRectF rect = currentRectangle();
    if (rect.width() <= 0.0 || rect.height() <= 0.0) {
        *error = i18n("Draw the box on the page, or give it a width and a height.");
        return false;
    }
    spec.rect = rect;
    fields->append(spec);
    return true;
}

void FormBuilderDialog::queueField()
{
    QVector<Field> described;
    QString error;
    if (!describedFields(&described, &error)) {
        KMessageBox::error(this, error, windowTitle());
        return;
    }

    m_pending += described;
    fillQueue();

    // The name is the one thing that must differ between two fields, so it is
    // the one thing cleared: everything else is usually right for the next one.
    m_name->clear();
    m_boxes->clear();
    m_name->setFocus(Qt::OtherFocusReason);
}

void FormBuilderDialog::fillQueue()
{
    m_queue->setRowCount(m_pending.size());
    for (int row = 0; row < m_pending.size(); ++row) {
        const Field &field = m_pending.at(row);
        m_queue->setItem(row, 0, new QTableWidgetItem(field.name));
        m_queue->setItem(row, 1, new QTableWidgetItem(describeKind(field.kind)));

        auto *page = new QTableWidgetItem(QLocale().toString(field.page + 1));
        page->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        m_queue->setItem(row, 2, page);
        m_queue->setItem(row, 3, new QTableWidgetItem(describeRect(field.rect)));
    }
    m_queue->resizeColumnToContents(1);
    m_queue->resizeColumnToContents(2);
    refreshOverlay();
}

void FormBuilderDialog::writeQueued()
{
    if (m_pending.isEmpty()) {
        KMessageBox::information(
            this, i18n("Nothing is waiting to be put on. Describe a field and add it to the list."), windowTitle());
        return;
    }

    const QVector<Field> fields = m_pending;
    bool hasRadio = false;
    for (const Field &field : fields) {
        hasRadio = hasRadio || field.kind == Field::Kind::Radio;
    }

    const bool written
        = runProducing(m_document, this, windowTitle(), u"-form.pdf"_s,
                       [this, fields, hasRadio](const QString &out, QStringList *summary, QString *error) {
                           int added = 0;
                           QApplication::setOverrideCursor(Qt::WaitCursor);
                           const bool ok = FormBuilder::addFields(m_pdf, out, fields, &added, error);
                           QApplication::restoreOverrideCursor();
                           if (!ok) {
                               return false;
                           }
                           *summary += i18np("Put one box on the document.", "Put %1 boxes on the document.", added);
                           if (hasRadio) {
                               *summary += i18n("A radio group counts as one box per button, since "
                                                "that is what it is: one field with several of them.");
                           }
                           *summary += i18n("Each box was given a drawing of its own, because a "
                                            "field that relies on the reader to draw it is invisible "
                                            "in about half the readers in use.");
                           return true;
                       });
    closeAfterWriting(written);
}

// ── The fields it has ─────────────────────────────────────────────────────

QWidget *FormBuilderDialog::buildInventory()
{
    auto *page = new QWidget(this);

    m_inventory = new QTableWidget(0, 5, page);
    m_inventory->setHorizontalHeaderLabels({ i18nc("@title:column", "Name"), i18nc("@title:column", "Kind"),
                                             i18nc("@title:column", "Page"), i18nc("@title:column", "Where"),
                                             i18nc("@title:column", "Notes") });
    m_inventory->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_inventory->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
    m_inventory->verticalHeader()->hide();
    m_inventory->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_inventory->setEditTriggers(QAbstractItemView::NoEditTriggers);

    auto *again = new QPushButton(QIcon::fromTheme(u"view-refresh"_s), i18nc("@action:button", "Read Again"), page);
    auto *rename = new QPushButton(i18nc("@action:button", "Call It Something Else…"), page);
    auto *remove
        = new QPushButton(QIcon::fromTheme(u"edit-delete"_s), i18nc("@action:button", "Take the Ticked Out"), page);

    auto *buttons = new QHBoxLayout;
    buttons->addWidget(again);
    buttons->addWidget(rename);
    buttons->addStretch(1);
    buttons->addWidget(remove);

    auto *honesty = new QGroupBox(i18nc("@title:group", "What this cannot promise"), page);
    auto *notes = new QLabel(FormBuilder::limitations().join(u'\n'), honesty);
    notes->setWordWrap(true);
    notes->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto *honestyLayout = new QVBoxLayout(honesty);
    honestyLayout->addWidget(notes);

    auto *layout = new QVBoxLayout(page);
    layout->addWidget(m_inventory, 1);
    layout->addLayout(buttons);
    layout->addWidget(honesty);

    connect(again, &QPushButton::clicked, this, [this] { readFields(); });
    connect(rename, &QPushButton::clicked, this, [this] { renameChosen(); });
    connect(remove, &QPushButton::clicked, this, [this] { removeTicked(); });
    return page;
}

void FormBuilderDialog::readFields()
{
    QApplication::setOverrideCursor(Qt::WaitCursor);
    QString error;
    m_existing = Forms::read(m_pdf, &error);
    QApplication::restoreOverrideCursor();

    if (!error.isEmpty()) {
        KMessageBox::error(this, error, windowTitle());
    }

    fillInventory();
    fillNameChoosers();
    fillOrderList();
    showPage(m_shown);
}

void FormBuilderDialog::fillInventory()
{
    m_inventory->setRowCount(m_existing.size());
    for (int row = 0; row < m_existing.size(); ++row) {
        const FormField &field = m_existing.at(row);

        auto *name = new QTableWidgetItem(field.name);
        name->setFlags((name->flags() | Qt::ItemIsUserCheckable) & ~Qt::ItemIsEditable);
        name->setCheckState(Qt::Unchecked);
        m_inventory->setItem(row, 0, name);

        m_inventory->setItem(row, 1, new QTableWidgetItem(Forms::describe(field.kind)));

        auto *page = new QTableWidgetItem(field.page >= 0 ? QLocale().toString(field.page + 1)
                                                          : i18nc("@item a field that is on no page", "none"));
        page->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        m_inventory->setItem(row, 2, page);

        m_inventory->setItem(
            row, 3,
            new QTableWidgetItem(field.page >= 0 && !field.rect.isEmpty() ? describeRect(field.rect) : QString()));

        QStringList notes;
        if (field.required) {
            notes << i18nc("@item marks a field that must be filled in", "required");
        }
        if (field.readOnly) {
            notes << i18nc("@item marks a field that cannot be filled in", "locked");
        }
        if (field.multiline) {
            notes << i18nc("@item marks a field of several lines", "several lines");
        }
        if (!field.options.isEmpty()) {
            notes << i18nc("@item what a field may be set to", "choices: %1", field.options.join(u", "_s));
        }
        m_inventory->setItem(row, 4, new QTableWidgetItem(notes.join(u" · "_s)));
    }
    m_inventory->resizeColumnToContents(1);
    m_inventory->resizeColumnToContents(2);
    m_inventory->resizeColumnToContents(3);
}

QStringList FormBuilderDialog::tickedNames() const
{
    QStringList names;
    for (int row = 0; row < m_inventory->rowCount(); ++row) {
        if (m_inventory->item(row, 0)->checkState() == Qt::Checked) {
            names << m_inventory->item(row, 0)->text();
        }
    }
    return names;
}

void FormBuilderDialog::removeTicked()
{
    const QStringList names = tickedNames();
    if (names.isEmpty()) {
        KMessageBox::information(this, i18n("Tick the fields to take out first."), windowTitle());
        return;
    }

    const bool written = runProducing(m_document, this, windowTitle(), u"-form.pdf"_s,
                                      [this, names](const QString &out, QStringList *summary, QString *error) {
                                          int removed = 0;
                                          QApplication::setOverrideCursor(Qt::WaitCursor);
                                          const bool ok = FormBuilder::removeFields(m_pdf, out, names, &removed, error);
                                          QApplication::restoreOverrideCursor();
                                          if (!ok) {
                                              return false;
                                          }
                                          *summary += i18np("Took one field out.", "Took %1 fields out.", removed);
                                          if (removed < names.size()) {
                                              *summary += i18np("One of the names ticked was not in the document.",
                                                                "%1 of the names ticked were not in the document.",
                                                                int(names.size()) - removed);
                                          }
                                          return true;
                                      });
    closeAfterWriting(written);
}

void FormBuilderDialog::renameChosen()
{
    const int row = m_inventory->currentRow();
    if (row < 0 || row >= m_existing.size()) {
        KMessageBox::information(this, i18n("Choose the field to rename first."), windowTitle());
        return;
    }
    const QString from = m_existing.at(row).name;

    bool accepted = false;
    const QString to
        = QInputDialog::getText(this, i18nc("@title:window", "Rename a Field"),
                                i18n("What “%1” should be called instead:", from), QLineEdit::Normal, from, &accepted)
              .trimmed();
    if (!accepted || to.isEmpty() || to == from) {
        return;
    }

    const bool written = runProducing(m_document, this, windowTitle(), u"-form.pdf"_s,
                                      [this, from, to](const QString &out, QStringList *summary, QString *error) {
                                          QApplication::setOverrideCursor(Qt::WaitCursor);
                                          const bool ok = FormBuilder::renameField(m_pdf, out, from, to, error);
                                          QApplication::restoreOverrideCursor();
                                          if (!ok) {
                                              return false;
                                          }
                                          *summary += i18n("“%1” is now called “%2”.", from, to);
                                          *summary += i18n("Anything that addressed it by its old name (a script, "
                                                           "a data file, a spreadsheet column) has to be told.");
                                          return true;
                                      });
    closeAfterWriting(written);
}

// ── Tab order ─────────────────────────────────────────────────────────────

QWidget *FormBuilderDialog::buildOrder()
{
    auto *page = new QWidget(this);

    m_orderPage = new QSpinBox(page);
    m_orderPage->setRange(1, qMax(1, m_document ? m_document->pageCount() : 1));
    m_orderPage->setPrefix(i18nc("@label:spinbox prefix before a page number", "Page "));
    m_orderPage->setValue(m_shown + 1);

    auto *up = new QPushButton(QIcon::fromTheme(u"go-up"_s), i18nc("@action:button", "Up"), page);
    auto *down = new QPushButton(QIcon::fromTheme(u"go-down"_s), i18nc("@action:button", "Down"), page);

    auto *top = new QHBoxLayout;
    top->addWidget(m_orderPage);
    top->addStretch(1);
    top->addWidget(up);
    top->addWidget(down);

    m_order = new QListWidget(page);
    m_order->setDragDropMode(QAbstractItemView::InternalMove);
    m_order->setSelectionMode(QAbstractItemView::SingleSelection);

    auto *note = new QLabel(i18n("The order the tab key walks the fields of this page. Drag the names, or use the "
                                 "buttons. Fields left out keep their place behind the ones listed."),
                            page);
    note->setWordWrap(true);

    auto *apply = new QPushButton(QIcon::fromTheme(u"document-save"_s), i18nc("@action:button", "Set the Order"), page);

    auto *layout = new QVBoxLayout(page);
    layout->addLayout(top);
    layout->addWidget(m_order, 1);
    layout->addWidget(note);
    layout->addWidget(apply);

    const auto move = [this](int delta) {
        const int row = m_order->currentRow();
        const int to = row + delta;
        if (row < 0 || to < 0 || to >= m_order->count()) {
            return;
        }
        m_order->insertItem(to, m_order->takeItem(row));
        m_order->setCurrentRow(to);
    };
    connect(up, &QPushButton::clicked, this, [move] { move(-1); });
    connect(down, &QPushButton::clicked, this, [move] { move(1); });
    connect(m_orderPage, &QSpinBox::valueChanged, this, [this] { fillOrderList(); });
    connect(apply, &QPushButton::clicked, this, [this] { applyOrder(); });
    return page;
}

void FormBuilderDialog::fillOrderList()
{
    m_order->clear();
    const int page = m_orderPage->value() - 1;
    for (const FormField &field : std::as_const(m_existing)) {
        if (field.page == page) {
            m_order->addItem(field.name);
        }
    }
}

void FormBuilderDialog::applyOrder()
{
    QStringList names;
    for (int row = 0; row < m_order->count(); ++row) {
        names << m_order->item(row)->text();
    }
    if (names.isEmpty()) {
        KMessageBox::information(this, i18n("This page has no fields to put in order."), windowTitle());
        return;
    }

    const int page = m_orderPage->value() - 1;
    const bool written = runProducing(m_document, this, windowTitle(), u"-form.pdf"_s,
                                      [this, page, names](const QString &out, QStringList *summary, QString *error) {
                                          QApplication::setOverrideCursor(Qt::WaitCursor);
                                          const bool ok = FormBuilder::setTabOrder(m_pdf, out, page, names, error);
                                          QApplication::restoreOverrideCursor();
                                          if (!ok) {
                                              return false;
                                          }
                                          *summary += i18np("The tab key now walks one field of page %2 in that order.",
                                                            "The tab key now walks %1 fields of page %2 in that order.",
                                                            int(names.size()), page + 1);
                                          return true;
                                      });
    closeAfterWriting(written);
}

// ── Behaviour ─────────────────────────────────────────────────────────────

QWidget *FormBuilderDialog::buildBehaviour()
{
    auto *page = new QWidget(this);

    const auto chooser = [page](QComboBox **target) {
        *target = new QComboBox(page);
        // Editable, because a field added on the first tab is not in this file
        // yet (it is in the file that tab wrote), and a name may be typed.
        (*target)->setEditable(true);
        (*target)->setInsertPolicy(QComboBox::NoInsert);
    };

    // Formatting
    chooser(&m_formatField);
    m_format = new QComboBox(page);
    m_format->addItem(i18nc("@item:inlistbox how a value is shown", "As typed"),
                      static_cast<int>(FormBuilder::Format::None));
    m_format->addItem(i18nc("@item:inlistbox how a value is shown", "Number"),
                      static_cast<int>(FormBuilder::Format::Number));
    m_format->addItem(i18nc("@item:inlistbox how a value is shown", "Amount of money"),
                      static_cast<int>(FormBuilder::Format::Currency));
    m_format->addItem(i18nc("@item:inlistbox how a value is shown", "Percentage"),
                      static_cast<int>(FormBuilder::Format::Percent));
    m_format->addItem(i18nc("@item:inlistbox how a value is shown", "Date"),
                      static_cast<int>(FormBuilder::Format::Date));
    m_format->addItem(i18nc("@item:inlistbox how a value is shown", "Time"),
                      static_cast<int>(FormBuilder::Format::Time));
    m_format->addItem(i18nc("@item:inlistbox how a value is shown", "Postcode"),
                      static_cast<int>(FormBuilder::Format::ZipCode));
    m_format->addItem(i18nc("@item:inlistbox how a value is shown", "Telephone number"),
                      static_cast<int>(FormBuilder::Format::Phone));

    m_decimals = new QSpinBox(page);
    m_decimals->setRange(0, 6);
    m_decimals->setValue(2);

    m_symbol = new QLineEdit(QLocale().currencySymbol(), page);

    auto *formatApply = new QPushButton(i18nc("@action:button", "Set the Formatting"), page);
    auto *formatBox = new QGroupBox(i18nc("@title:group", "Show a value as"), page);
    auto *formatForm = new QFormLayout;
    formatForm->addRow(i18nc("@label:listbox", "Field:"), m_formatField);
    formatForm->addRow(i18nc("@label:listbox", "Shown as:"), m_format);
    formatForm->addRow(i18nc("@label:spinbox places after the decimal point", "Decimals:"), m_decimals);
    formatForm->addRow(i18nc("@label:textbox", "Currency symbol:"), m_symbol);
    auto *formatLayout = new QVBoxLayout(formatBox);
    formatLayout->addLayout(formatForm);
    formatLayout->addWidget(formatApply);

    // Validation
    chooser(&m_validateField);
    m_minimum = new QDoubleSpinBox(page);
    m_minimum->setRange(-1e9, 1e9);
    m_maximum = new QDoubleSpinBox(page);
    m_maximum->setRange(-1e9, 1e9);
    m_maximum->setValue(100.0);

    auto *validateApply = new QPushButton(i18nc("@action:button", "Set the Range"), page);
    auto *validateBox = new QGroupBox(i18nc("@title:group", "Refuse a number outside a range"), page);
    auto *validateForm = new QFormLayout;
    validateForm->addRow(i18nc("@label:listbox", "Field:"), m_validateField);
    validateForm->addRow(i18nc("@label:spinbox", "Smallest:"), m_minimum);
    validateForm->addRow(i18nc("@label:spinbox", "Largest:"), m_maximum);
    auto *validateLayout = new QVBoxLayout(validateBox);
    validateLayout->addLayout(validateForm);
    validateLayout->addWidget(validateApply);

    // Calculation
    chooser(&m_calcField);
    m_calcHow = new QComboBox(page);
    m_calcHow->addItem(i18nc("@item:inlistbox what to work out", "Total"),
                       static_cast<int>(FormBuilder::Calculation::Sum));
    m_calcHow->addItem(i18nc("@item:inlistbox what to work out", "Product"),
                       static_cast<int>(FormBuilder::Calculation::Product));
    m_calcHow->addItem(i18nc("@item:inlistbox what to work out", "Average"),
                       static_cast<int>(FormBuilder::Calculation::Average));
    m_calcHow->addItem(i18nc("@item:inlistbox what to work out", "Smallest"),
                       static_cast<int>(FormBuilder::Calculation::Minimum));
    m_calcHow->addItem(i18nc("@item:inlistbox what to work out", "Largest"),
                       static_cast<int>(FormBuilder::Calculation::Maximum));

    m_calcSources = new QListWidget(page);
    m_calcSources->setMaximumHeight(140);

    auto *calcApply = new QPushButton(i18nc("@action:button", "Set the Calculation"), page);
    auto *calcBox = new QGroupBox(i18nc("@title:group", "Work a value out from other fields"), page);
    auto *calcForm = new QFormLayout;
    calcForm->addRow(i18nc("@label:listbox", "Field:"), m_calcField);
    calcForm->addRow(i18nc("@label:listbox", "Work out the:"), m_calcHow);
    calcForm->addRow(i18nc("@label:listbox", "From:"), m_calcSources);
    auto *calcLayout = new QVBoxLayout(calcBox);
    calcLayout->addLayout(calcForm);
    calcLayout->addWidget(calcApply);

    // What a button does
    chooser(&m_buttonField);
    m_buttonAction = new QComboBox(page);
    m_buttonAction->addItem(i18nc("@item:inlistbox what pressing a button does", "Empty the form"),
                            static_cast<int>(FormBuilder::ButtonAction::ResetForm));
    m_buttonAction->addItem(i18nc("@item:inlistbox what pressing a button does", "Send the answers"),
                            static_cast<int>(FormBuilder::ButtonAction::SubmitForm));
    m_buttonAction->addItem(i18nc("@item:inlistbox what pressing a button does", "Jump to a page"),
                            static_cast<int>(FormBuilder::ButtonAction::GoToPage));
    m_buttonAction->addItem(i18nc("@item:inlistbox what pressing a button does", "Open an address"),
                            static_cast<int>(FormBuilder::ButtonAction::OpenUrl));

    m_buttonTarget = new QLineEdit(page);
    m_buttonTarget->setPlaceholderText(i18n("An address, or a page number"));

    auto *buttonApply = new QPushButton(i18nc("@action:button", "Set What It Does"), page);
    auto *buttonBox = new QGroupBox(i18nc("@title:group", "What pressing a button does"), page);
    auto *buttonForm = new QFormLayout;
    buttonForm->addRow(i18nc("@label:listbox", "Button:"), m_buttonField);
    buttonForm->addRow(i18nc("@label:listbox", "Does:"), m_buttonAction);
    buttonForm->addRow(i18nc("@label:textbox", "Where to:"), m_buttonTarget);
    auto *buttonLayout = new QVBoxLayout(buttonBox);
    buttonLayout->addLayout(buttonForm);
    buttonLayout->addWidget(buttonApply);

    auto *note = new QLabel(i18n("Formatting, ranges and calculations are JavaScript, which is how every PDF form "
                                 "does them, and which PDF/A forbids and several readers do not run at all. What "
                                 "a reader will not run, it simply shows as typed."),
                            page);
    note->setWordWrap(true);

    auto *columns = new QHBoxLayout;
    auto *left = new QVBoxLayout;
    left->addWidget(formatBox);
    left->addWidget(validateBox);
    left->addStretch(1);
    auto *right = new QVBoxLayout;
    right->addWidget(calcBox);
    right->addWidget(buttonBox);
    right->addStretch(1);
    columns->addLayout(left, 1);
    columns->addLayout(right, 1);

    auto *layout = new QVBoxLayout(page);
    layout->addLayout(columns, 1);
    layout->addWidget(note);

    const auto followFormat = [this] {
        const auto format = static_cast<FormBuilder::Format>(m_format->currentData().toInt());
        m_symbol->setEnabled(format == FormBuilder::Format::Currency);
        m_decimals->setEnabled(format == FormBuilder::Format::Number || format == FormBuilder::Format::Currency
                               || format == FormBuilder::Format::Percent);
    };
    connect(m_format, &QComboBox::currentIndexChanged, this, followFormat);
    followFormat();
    connect(formatApply, &QPushButton::clicked, this, [this] { applyFormat(); });
    connect(validateApply, &QPushButton::clicked, this, [this] { applyValidation(); });
    connect(calcApply, &QPushButton::clicked, this, [this] { applyCalculation(); });
    connect(buttonApply, &QPushButton::clicked, this, [this] { applyButtonAction(); });
    return page;
}

void FormBuilderDialog::fillNameChoosers()
{
    QStringList fillable;
    QStringList buttons;
    for (const FormField &field : std::as_const(m_existing)) {
        if (field.kind == FormField::Kind::Button) {
            buttons << field.name;
        } else if (field.kind != FormField::Kind::Signature) {
            fillable << field.name;
        }
    }

    // A name typed by hand survives the list being read again; that is the only
    // way to name a field this file does not have yet.
    for (QComboBox *chooser : { m_formatField, m_validateField, m_calcField, m_buttonField }) {
        const QString was = chooser->currentText();
        chooser->clear();
        chooser->addItems(chooser == m_buttonField ? buttons : fillable);
        if (!was.isEmpty()) {
            chooser->setCurrentText(was);
        }
    }

    m_calcSources->clear();
    for (const QString &name : std::as_const(fillable)) {
        auto *item = new QListWidgetItem(name, m_calcSources);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(Qt::Unchecked);
    }
}

void FormBuilderDialog::applyFormat()
{
    const QString name = m_formatField->currentText().trimmed();
    if (name.isEmpty()) {
        KMessageBox::information(this, i18n("Say which field should show its value that way."), windowTitle());
        return;
    }

    const auto format = static_cast<FormBuilder::Format>(m_format->currentData().toInt());
    const QString shown = m_format->currentText();
    const int decimals = m_decimals->value();
    const QString symbol = m_symbol->text();

    const bool written = runProducing(
        m_document, this, windowTitle(), u"-form.pdf"_s,
        [this, name, format, shown, decimals, symbol](const QString &out, QStringList *summary, QString *error) {
            QStringList warnings;
            QApplication::setOverrideCursor(Qt::WaitCursor);
            const bool ok = FormBuilder::setFormat(m_pdf, out, name, format, decimals, symbol, &warnings, error);
            QApplication::restoreOverrideCursor();
            if (!ok) {
                return false;
            }
            *summary += format == FormBuilder::Format::None
                ? i18n("“%1” no longer formats what is typed into it.", name)
                : i18n("“%1” now shows what is typed into it as: %2.", name, shown);
            *summary += warnings;
            return true;
        });
    closeAfterWriting(written);
}

void FormBuilderDialog::applyValidation()
{
    const QString name = m_validateField->currentText().trimmed();
    if (name.isEmpty()) {
        KMessageBox::information(this, i18n("Say which field the range belongs to."), windowTitle());
        return;
    }
    const double minimum = m_minimum->value();
    const double maximum = m_maximum->value();
    if (minimum > maximum) {
        KMessageBox::error(this, i18n("The smallest value is larger than the largest, so nothing would be accepted."),
                           windowTitle());
        return;
    }

    const bool written = runProducing(
        m_document, this, windowTitle(), u"-form.pdf"_s,
        [this, name, minimum, maximum](const QString &out, QStringList *summary, QString *error) {
            QStringList warnings;
            QApplication::setOverrideCursor(Qt::WaitCursor);
            const bool ok = FormBuilder::setValidation(m_pdf, out, name, minimum, maximum, &warnings, error);
            QApplication::restoreOverrideCursor();
            if (!ok) {
                return false;
            }
            *summary += i18n("“%1” now takes numbers from %2 to %3 only.", name, points(minimum), points(maximum));
            *summary += warnings;
            return true;
        });
    closeAfterWriting(written);
}

void FormBuilderDialog::applyCalculation()
{
    const QString name = m_calcField->currentText().trimmed();
    QStringList sources;
    for (int row = 0; row < m_calcSources->count(); ++row) {
        if (m_calcSources->item(row)->checkState() == Qt::Checked) {
            sources << m_calcSources->item(row)->text();
        }
    }
    if (name.isEmpty() || sources.isEmpty()) {
        KMessageBox::information(this,
                                 i18n("Say which field is worked out, and tick the fields it is worked out "
                                      "from."),
                                 windowTitle());
        return;
    }
    if (sources.contains(name)) {
        KMessageBox::error(this, i18n("“%1” cannot be worked out from itself.", name), windowTitle());
        return;
    }

    const auto how = static_cast<FormBuilder::Calculation>(m_calcHow->currentData().toInt());
    const QString what = m_calcHow->currentText();

    const bool written
        = runProducing(m_document, this, windowTitle(), u"-form.pdf"_s,
                       [this, name, how, what, sources](const QString &out, QStringList *summary, QString *error) {
                           QStringList warnings;
                           QApplication::setOverrideCursor(Qt::WaitCursor);
                           const bool ok
                               = FormBuilder::setCalculation(m_pdf, out, name, how, sources, &warnings, error);
                           QApplication::restoreOverrideCursor();
                           if (!ok) {
                               return false;
                           }
                           *summary += i18np("“%2” is now the %3 of one field.", "“%2” is now the %3 of %1 fields.",
                                             int(sources.size()), name, what.toLower());
                           *summary += warnings;
                           return true;
                       });
    closeAfterWriting(written);
}

void FormBuilderDialog::applyButtonAction()
{
    const QString name = m_buttonField->currentText().trimmed();
    if (name.isEmpty()) {
        KMessageBox::information(this,
                                 i18n("Say which button this is about. A document with no push button on it "
                                      "has nothing to press."),
                                 windowTitle());
        return;
    }

    const auto action = static_cast<FormBuilder::ButtonAction>(m_buttonAction->currentData().toInt());
    const QString target = m_buttonTarget->text().trimmed();
    if (target.isEmpty() && action != FormBuilder::ButtonAction::ResetForm) {
        KMessageBox::information(this, i18n("Say where it sends or goes: an address, or a page number."),
                                 windowTitle());
        return;
    }
    const QString does = m_buttonAction->currentText();

    const bool written
        = runProducing(m_document, this, windowTitle(), u"-form.pdf"_s,
                       [this, name, action, target, does](const QString &out, QStringList *summary, QString *error) {
                           QApplication::setOverrideCursor(Qt::WaitCursor);
                           const bool ok = FormBuilder::setButtonAction(m_pdf, out, name, action, target, error);
                           QApplication::restoreOverrideCursor();
                           if (!ok) {
                               return false;
                           }
                           *summary += i18nc("@info a button and what pressing it now does",
                                             "Pressing “%1” now does this: %2.", name, does.toLower());
                           return true;
                       });
    closeAfterWriting(written);
}

// ── Answers ───────────────────────────────────────────────────────────────

QWidget *FormBuilderDialog::buildData()
{
    auto *page = new QWidget(this);

    auto *outBox = new QGroupBox(i18nc("@title:group", "Take the answers out"), page);
    auto *outNote = new QLabel(i18n("Writes what the fields hold to an FDF, XFDF or CSV file, chosen by what the "
                                    "file is called. The document itself is not in it, which is what makes it "
                                    "small enough to send and useless to anybody without the form."),
                               outBox);
    outNote->setWordWrap(true);
    auto *exportButton = new QPushButton(QIcon::fromTheme(u"document-export"_s),
                                         i18nc("@action:button", "Write the Answers…"), outBox);
    auto *outLayout = new QVBoxLayout(outBox);
    outLayout->addWidget(outNote);
    outLayout->addWidget(exportButton);

    auto *inBox = new QGroupBox(i18nc("@title:group", "Put answers in"), page);
    auto *inNote = new QLabel(i18n("Fills the fields from an FDF, XFDF or CSV file: one filled copy per run, "
                                   "which is how a hundred contracts get made from one letter and a spreadsheet."),
                              inBox);
    inNote->setWordWrap(true);
    auto *importButton
        = new QPushButton(QIcon::fromTheme(u"document-import"_s), i18nc("@action:button", "Read the Answers…"), inBox);
    auto *inLayout = new QVBoxLayout(inBox);
    inLayout->addWidget(inNote);
    inLayout->addWidget(importButton);

    auto *gatherBox = new QGroupBox(i18nc("@title:group", "Gather many filled-in copies"), page);
    auto *gatherNote = new QLabel(i18n("One row per document, one column per field. What comes back from twenty "
                                       "people as twenty files becomes a table."),
                                  gatherBox);
    gatherNote->setWordWrap(true);
    auto *collectButton
        = new QPushButton(QIcon::fromTheme(u"table"_s), i18nc("@action:button", "Gather into a Table…"), gatherBox);
    auto *gatherLayout = new QVBoxLayout(gatherBox);
    gatherLayout->addWidget(gatherNote);
    gatherLayout->addWidget(collectButton);

    auto *copyBox = new QGroupBox(i18nc("@title:group", "Take the fields from another document"), page);
    auto *copyNote = new QLabel(i18n("Copies every field of an empty form onto this one, where it sits and how it "
                                     "looks. It needs the two to have the same layout: the fields land at the "
                                     "coordinates they had, not next to whatever text they were beside."),
                                copyBox);
    copyNote->setWordWrap(true);
    auto *copyButton
        = new QPushButton(QIcon::fromTheme(u"edit-copy"_s), i18nc("@action:button", "Copy the Fields Over…"), copyBox);
    auto *copyLayout = new QVBoxLayout(copyBox);
    copyLayout->addWidget(copyNote);
    copyLayout->addWidget(copyButton);

    auto *layout = new QVBoxLayout(page);
    layout->addWidget(outBox);
    layout->addWidget(inBox);
    layout->addWidget(gatherBox);
    layout->addWidget(copyBox);
    layout->addStretch(1);

    connect(exportButton, &QPushButton::clicked, this, [this] { exportAnswers(); });
    connect(importButton, &QPushButton::clicked, this, [this] { importAnswers(); });
    connect(collectButton, &QPushButton::clicked, this, [this] { collectAnswers(); });
    connect(copyButton, &QPushButton::clicked, this, [this] { copyFromTemplate(); });
    return page;
}

void FormBuilderDialog::exportAnswers()
{
    const QFileInfo file(m_pdf);
    const QString suggested = file.absolutePath() + u'/' + file.completeBaseName() + u".fdf"_s;
    const QString path = QFileDialog::getSaveFileName(this, i18nc("@title:window", "Write the Answers"), suggested,
                                                      i18n("Form data (*.fdf *.xfdf *.csv);;All files (*)"));
    if (path.isEmpty()) {
        return;
    }

    QApplication::setOverrideCursor(Qt::WaitCursor);
    QString error;
    const bool ok = FormBuilder::exportData(m_pdf, path, &error);
    QApplication::restoreOverrideCursor();

    if (!ok) {
        KMessageBox::error(this, error.isEmpty() ? i18n("The answers could not be written.") : error, windowTitle());
        return;
    }
    KMessageBox::information(
        this, i18n("Wrote the answers to %1. The document itself is not in it.", QFileInfo(path).fileName()),
        windowTitle());
}

void FormBuilderDialog::importAnswers()
{
    const QString path = QFileDialog::getOpenFileName(this, i18nc("@title:window", "Read the Answers"), QString(),
                                                      i18n("Form data (*.fdf *.xfdf *.csv);;All files (*)"));
    if (path.isEmpty()) {
        return;
    }

    const bool written = runProducing(m_document, this, windowTitle(), u"-filled.pdf"_s,
                                      [this, path](const QString &out, QStringList *summary, QString *error) {
                                          int filled = 0;
                                          QApplication::setOverrideCursor(Qt::WaitCursor);
                                          const bool ok = FormBuilder::importData(m_pdf, out, path, &filled, error);
                                          QApplication::restoreOverrideCursor();
                                          if (!ok) {
                                              return false;
                                          }
                                          *summary += filled == 0
                                              ? i18n("None of the names in that file is a field of this document.")
                                              : i18np("Filled in one field.", "Filled in %1 fields.", filled);
                                          return true;
                                      });
    closeAfterWriting(written);
}

void FormBuilderDialog::collectAnswers()
{
    const QStringList pdfs
        = QFileDialog::getOpenFileNames(this, i18nc("@title:window", "The Filled-In Copies"), QFileInfo(m_pdf).path(),
                                        i18n("PDF documents (*.pdf);;All files (*)"));
    if (pdfs.isEmpty()) {
        return;
    }

    const QFileInfo file(m_pdf);
    const QString suggested = file.absolutePath() + u'/' + file.completeBaseName() + u"-answers.csv"_s;
    const QString table = QFileDialog::getSaveFileName(this, i18nc("@title:window", "Where the Table Goes"), suggested,
                                                       i18n("Tables (*.csv);;All files (*)"));
    if (table.isEmpty()) {
        return;
    }

    QApplication::setOverrideCursor(Qt::WaitCursor);
    QString error;
    const bool ok = FormBuilder::collect(pdfs, table, &error);
    QApplication::restoreOverrideCursor();

    if (!ok) {
        KMessageBox::error(this, error.isEmpty() ? i18n("The table could not be written.") : error, windowTitle());
        return;
    }
    KMessageBox::information(this,
                             i18np("Wrote the answers of one document to %2, one row each.",
                                   "Wrote the answers of %1 documents to %2, one row each.", int(pdfs.size()),
                                   QFileInfo(table).fileName()),
                             windowTitle());
}

void FormBuilderDialog::copyFromTemplate()
{
    const QString source
        = QFileDialog::getOpenFileName(this, i18nc("@title:window", "The Form to Take the Fields From"),
                                       QFileInfo(m_pdf).path(), i18n("PDF documents (*.pdf);;All files (*)"));
    if (source.isEmpty()) {
        return;
    }

    const bool written = runProducing(m_document, this, windowTitle(), u"-form.pdf"_s,
                                      [this, source](const QString &out, QStringList *summary, QString *error) {
                                          int copied = 0;
                                          QApplication::setOverrideCursor(Qt::WaitCursor);
                                          const bool ok
                                              = FormBuilder::copyFieldsFrom(source, m_pdf, out, &copied, error);
                                          QApplication::restoreOverrideCursor();
                                          if (!ok) {
                                              return false;
                                          }
                                          *summary += i18np("Took one field from “%2” and put it on this document.",
                                                            "Took %1 fields from “%2” and put them on this document.",
                                                            copied, QFileInfo(source).fileName());
                                          return true;
                                      });
    closeAfterWriting(written);
}

// ── The page being worked on ──────────────────────────────────────────────

void FormBuilderDialog::showPage(int page)
{
    if (!m_document || page < 0 || page >= m_document->pageCount()) {
        return;
    }
    m_shown = page;

    const PageRef ref = m_document->pageAt(page);
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const QImage image
        = m_backend ? rotated(m_backend->renderPage(ref.sourceId, ref.sourcePage, 1400), ref.rotation) : QImage();
    QApplication::restoreOverrideCursor();
    m_view->setPage(image, m_document->pageSizePoints(page));
    refreshOverlay();

    if (m_pageBox->value() != page + 1) {
        const QSignalBlocker blocker(m_pageBox);
        m_pageBox->setValue(page + 1);
    }
}

void FormBuilderDialog::refreshOverlay()
{
    QVector<FormField> here;
    QVector<int> indices;
    for (int i = 0; i < m_existing.size(); ++i) {
        if (m_existing.at(i).page == m_shown) {
            here.append(m_existing.at(i));
            indices.append(i);
        }
    }

    // A field still waiting to be written is drawn too, because the question
    // the page answers is "where will the boxes be", and half an answer to that
    // is what makes two of them overlap. Its index of -1 is the one
    // ps::FormFieldView draws as current, so it stands out from what is already
    // on the document.
    for (const Field &waiting : std::as_const(m_pending)) {
        if (waiting.page != m_shown) {
            continue;
        }
        FormField sketch;
        sketch.name = waiting.name;
        sketch.page = waiting.page;
        sketch.rect = waiting.rect;
        sketch.required = waiting.required;
        here.append(sketch);
        indices.append(-1);
    }

    m_view->setFields(here, indices);
    m_view->setDraft(currentRectangle());
}

void FormBuilderDialog::closeAfterWriting(bool written)
{
    if (written) {
        accept();
    }
}

} // namespace

void showFormBuilder(Document *document, RenderBackend *backend, int page, QWidget *parent)
{
    const QString pdf = savedPath(document, parent);
    if (pdf.isEmpty()) {
        return;
    }
    FormBuilderDialog(document, backend, pdf, page, parent).exec();
}

} // namespace ps::tools
