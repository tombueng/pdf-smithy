/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "NumberEditor.h"

#include "PageNavigation.h"

#include "PageProcessor.h"
#include "WatermarkView.h"
#include "core/Document.h"
#include "core/RenderBackend.h"

#include <KLocalizedString>

#include <QColorDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QWidget>

#include <numeric>

using namespace Qt::Literals::StringLiterals;

namespace ps {

namespace {

QImage rotated(const QImage &image, int degrees)
{
    if (degrees == 0 || image.isNull()) {
        return image;
    }
    return image.transformed(QTransform().rotate(degrees), Qt::SmoothTransformation);
}

const Overlay::Anchor anchors[] = {
    Overlay::Anchor::TopLeft,    Overlay::Anchor::TopCentre,    Overlay::Anchor::TopRight,
    Overlay::Anchor::BottomLeft, Overlay::Anchor::BottomCentre, Overlay::Anchor::BottomRight,
};

QStringList anchorNames()
{
    return { i18nc("@item:inlistbox where a page number sits", "Top left"),
             i18nc("@item:inlistbox where a page number sits", "Top centre"),
             i18nc("@item:inlistbox where a page number sits", "Top right"),
             i18nc("@item:inlistbox where a page number sits", "Bottom left"),
             i18nc("@item:inlistbox where a page number sits", "Bottom centre"),
             i18nc("@item:inlistbox where a page number sits", "Bottom right") };
}

/** The wordings people ask for, and the placeholder text behind each. */
struct Preset {
    const char *label;
    const char *pattern;
};

const Preset presets[] = {
    { QT_TRANSLATE_NOOP("@item:inlistbox", "1 / 12"), "{page} / {pages}" },
    { QT_TRANSLATE_NOOP("@item:inlistbox", "Page 1 of 12"), "Page {page} of {pages}" },
    { QT_TRANSLATE_NOOP("@item:inlistbox", "Just the number"), "{page}" },
    { QT_TRANSLATE_NOOP("@item:inlistbox", "Bates number"), "{bates}" },
    { QT_TRANSLATE_NOOP("@item:inlistbox", "File name and number"), "{file} · {page}" },
    { QT_TRANSLATE_NOOP("@item:inlistbox", "Date and number"), "{date} · {page}" },
};

} // namespace

NumberEditor::NumberEditor(Document *document, RenderBackend *backend, const QVector<int> &selection,
                           PageProcessor *processor, QObject *parent)
    : EditorMode(parent)
    , m_document(document)
    , m_backend(backend)
    , m_processor(processor)
    , m_selection(selection)
    , m_fileName(QFileInfo(document->displayName()).completeBaseName())
    , m_row(selection.isEmpty() ? 0 : selection.first())
    , m_view(new WatermarkView)
    , m_pageBox(new QSpinBox)
    , m_rendered(new QLabel)
{
    setWidgets(buildStage(), buildPanel());
    showRow(m_row);
}

QWidget *NumberEditor::buildStage()
{
    auto *stage = new QWidget;

    m_pageBox->setValue(m_row + 1);
    auto *navigation = pageNavigation(stage, m_pageBox, m_document->pageCount());

    m_rendered->setWordWrap(true);

    auto *layout = new QVBoxLayout(stage);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addLayout(navigation);
    layout->addWidget(m_view, 1);
    layout->addWidget(m_rendered);

    connect(m_pageBox, &QSpinBox::valueChanged, this, [this](int value) { showRow(value - 1); });

    return stage;
}

QWidget *NumberEditor::buildPanel()
{
    auto *panel = new QWidget;

    auto *scope = new QGroupBox(i18nc("@title:group", "Apply to"), panel);
    m_selectionOnly = new QRadioButton(
        i18ncp("@option:radio", "The selected page", "The %1 selected pages", m_selection.size()), scope);
    m_wholeDocument = new QRadioButton(i18ncp("@option:radio", "The whole document (%1 page)",
                                              "The whole document (%1 pages)", m_document->pageCount()),
                                       scope);
    m_selectionOnly->setEnabled(!m_selection.isEmpty());
    (m_selection.isEmpty() ? m_wholeDocument : m_selectionOnly)->setChecked(true);
    auto *scopeLayout = new QVBoxLayout(scope);
    scopeLayout->addWidget(m_selectionOnly);
    scopeLayout->addWidget(m_wholeDocument);

    m_preset = new QComboBox(panel);
    for (const Preset &preset : presets) {
        m_preset->addItem(i18nc("@item:inlistbox", preset.label), QString::fromLatin1(preset.pattern));
    }

    m_text = new QLineEdit(QString::fromLatin1(presets[0].pattern), panel);
    m_text->setToolTip(i18nc("@info:tooltip",
                             "Placeholders: {page} the running number, {pages} how many the run covers, "
                             "{bates} the Bates number, {file} the file name, {date} today."));

    m_position = new QComboBox(panel);
    m_position->addItems(anchorNames());
    m_position->setCurrentIndex(4); // bottom centre, which is where they belong

    m_fontSize = new QSpinBox(panel);
    m_fontSize->setRange(5, 48);
    m_fontSize->setValue(10);
    m_fontSize->setSuffix(i18nc("@item point suffix in a spin box", " pt"));

    m_margin = new QSpinBox(panel);
    m_margin->setRange(4, 144);
    m_margin->setValue(28);
    m_margin->setSuffix(i18nc("@item point suffix in a spin box", " pt"));

    m_startNumber = new QSpinBox(panel);
    m_startNumber->setRange(0, 100000);
    m_startNumber->setValue(1);
    m_startNumber->setToolTip(i18nc("@info:tooltip", "What the first stamped page is called, not which page it is."));

    m_colourButton = new QPushButton(panel);
    m_colourButton->setAutoFillBackground(true);
    m_colourButton->setStyleSheet(u"background-color: %1;"_s.arg(m_colour.name()));

    m_batesPrefix = new QLineEdit(panel);
    m_batesPrefix->setPlaceholderText(i18n("e.g. ACME-"));

    m_batesDigits = new QSpinBox(panel);
    m_batesDigits->setRange(1, 12);
    m_batesDigits->setValue(6);

    auto *form = new QFormLayout;
    form->addRow(i18nc("@label:listbox", "Wording:"), m_preset);
    form->addRow(i18nc("@label:textbox", "Text:"), m_text);
    form->addRow(i18nc("@label:listbox", "Position:"), m_position);
    form->addRow(i18nc("@label:spinbox", "Size:"), m_fontSize);
    form->addRow(i18nc("@label:spinbox", "From the edge:"), m_margin);
    form->addRow(i18nc("@label:chooser", "Colour:"), m_colourButton);
    form->addRow(i18nc("@label:spinbox", "First page is called:"), m_startNumber);

    auto *bates = new QGroupBox(i18nc("@title:group", "Bates numbering"), panel);
    auto *batesForm = new QFormLayout(bates);
    batesForm->addRow(i18nc("@label:textbox", "Prefix:"), m_batesPrefix);
    batesForm->addRow(i18nc("@label:spinbox", "Digits:"), m_batesDigits);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, panel);
    buttons->button(QDialogButtonBox::Ok)->setText(i18nc("@action:button", "Add the Numbers"));
    connect(buttons, &QDialogButtonBox::accepted, this, [this] { Q_EMIT finished(true); });
    connect(buttons, &QDialogButtonBox::rejected, this, [this] { Q_EMIT finished(false); });

    auto *layout = new QVBoxLayout(panel);
    layout->addWidget(scope);
    layout->addLayout(form);
    layout->addWidget(bates);
    layout->addStretch(1);
    layout->addWidget(buttons);

    // Only a settings change counts as a decision; turning the page redraws the
    // preview too, and closing the panel after merely looking must not ask
    // whether to keep anything.
    const auto redraw = [this] {
        m_touched = true;
        refreshPreview();
    };
    connect(m_preset, &QComboBox::currentIndexChanged, this, [this](int index) {
        m_text->setText(m_preset->itemData(index).toString());
    });
    connect(m_text, &QLineEdit::textChanged, this, redraw);
    connect(m_position, &QComboBox::currentIndexChanged, this, redraw);
    connect(m_fontSize, &QSpinBox::valueChanged, this, redraw);
    connect(m_margin, &QSpinBox::valueChanged, this, redraw);
    connect(m_startNumber, &QSpinBox::valueChanged, this, redraw);
    connect(m_batesPrefix, &QLineEdit::textChanged, this, redraw);
    connect(m_batesDigits, &QSpinBox::valueChanged, this, redraw);
    connect(m_selectionOnly, &QRadioButton::toggled, this, redraw);
    connect(m_colourButton, &QPushButton::clicked, this, [this, panel] {
        const QColor picked = QColorDialog::getColor(m_colour, panel, i18nc("@title:window", "Number Colour"));
        if (picked.isValid()) {
            m_colour = picked;
            m_touched = true;
            m_colourButton->setStyleSheet(u"background-color: %1;"_s.arg(m_colour.name()));
            refreshPreview();
        }
    });

    return panel;
}

PageNumbering::Options NumberEditor::options() const
{
    PageNumbering::Options options;
    options.text = m_text->text();
    options.anchor = anchors[qBound(0, m_position->currentIndex(), 5)];
    options.fontSize = m_fontSize->value();
    options.colour = m_colour;
    options.marginPoints = m_margin->value();
    options.startNumber = m_startNumber->value();
    options.batesPrefix = m_batesPrefix->text();
    options.batesDigits = m_batesDigits->value();
    options.fileName = m_fileName;
    return options;
}

QVector<int> NumberEditor::targetRows() const
{
    if (m_selectionOnly->isChecked() && !m_selection.isEmpty()) {
        return m_selection;
    }
    QVector<int> rows(m_document->pageCount());
    std::iota(rows.begin(), rows.end(), 0);
    return rows;
}

void NumberEditor::showRow(int row)
{
    if (row < 0 || row >= m_document->pageCount()) {
        return;
    }
    m_row = row;

    const PageRef ref = m_document->pageAt(row);
    m_view->setPage(rotated(m_backend->renderPage(ref.sourceId, ref.sourcePage, 1400), ref.rotation),
                    m_document->pageSizePoints(row));

    if (m_pageBox->value() != row + 1) {
        QSignalBlocker blocker(m_pageBox);
        m_pageBox->setValue(row + 1);
    }
    refreshPreview();
}

void NumberEditor::refreshPreview()
{
    const QVector<int> rows = targetRows();
    const int position = rows.indexOf(m_row);

    if (position < 0) {
        // The page on screen is outside the run, which is worth saying: an
        // empty stamp here would read as "the settings are wrong".
        m_view->setTextStamp({});
        m_rendered->setText(i18n("This page is not in the range being numbered, so nothing is stamped on it."));
        return;
    }

    const PageNumbering::Options settings = options();
    Overlay::TextStamp stamp;
    stamp.text = PageNumbering::render(settings, position, rows.size());
    stamp.fontSize = settings.fontSize;
    stamp.colour = settings.colour;
    stamp.rotation = 0.0;
    stamp.opacity = 1.0;
    stamp.anchor = settings.anchor;
    stamp.marginPoints = settings.marginPoints;
    m_view->setTextStamp(stamp);

    m_rendered->setText(i18nc("@info what one page's stamp comes out as", "On this page it reads: %1", stamp.text));
}

QString NumberEditor::title() const
{
    return i18nc("@title:window", "Page Numbers");
}

QString NumberEditor::iconName() const
{
    return QStringLiteral("format-text-symbol");
}

bool NumberEditor::isUnchanged() const
{
    return !m_touched || m_text->text().trimmed().isEmpty();
}

bool NumberEditor::commit(QString *error)
{
    if (isUnchanged()) {
        return true;
    }
    if (!m_processor) {
        if (error) {
            *error = i18n("This editor has nothing to number with.");
        }
        return false;
    }

    const QVector<int> rows = targetRows();
    if (!m_processor->numberPages(rows, options(), error)) {
        return false;
    }
    Q_EMIT statusMessage(i18ncp("@info:status", "Numbered %1 page.", "Numbered %1 pages.", rows.size()));
    return true;
}

} // namespace ps
