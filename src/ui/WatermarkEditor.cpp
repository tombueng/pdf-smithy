/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "WatermarkEditor.h"

#include "PageNavigation.h"

#include "PageProcessor.h"
#include "WatermarkView.h"
#include "core/Document.h"
#include "core/ImageIO.h"
#include "core/RenderBackend.h"

#include <KLocalizedString>
#include <KMessageBox>

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QSlider>
#include <QSpinBox>
#include <QStackedWidget>
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

/** The nine anchors, in the order they appear in the drop-down. */
const Overlay::Anchor anchors[] = {
    Overlay::Anchor::TopLeft,    Overlay::Anchor::TopCentre,    Overlay::Anchor::TopRight,
    Overlay::Anchor::CentreLeft, Overlay::Anchor::Centre,       Overlay::Anchor::CentreRight,
    Overlay::Anchor::BottomLeft, Overlay::Anchor::BottomCentre, Overlay::Anchor::BottomRight,
};

QStringList anchorNames()
{
    return { i18nc("@item:inlistbox where a stamp sits on the page", "Top left"),
             i18nc("@item:inlistbox where a stamp sits on the page", "Top centre"),
             i18nc("@item:inlistbox where a stamp sits on the page", "Top right"),
             i18nc("@item:inlistbox where a stamp sits on the page", "Left"),
             i18nc("@item:inlistbox where a stamp sits on the page", "Centre"),
             i18nc("@item:inlistbox where a stamp sits on the page", "Right"),
             i18nc("@item:inlistbox where a stamp sits on the page", "Bottom left"),
             i18nc("@item:inlistbox where a stamp sits on the page", "Bottom centre"),
             i18nc("@item:inlistbox where a stamp sits on the page", "Bottom right") };
}

} // namespace

WatermarkEditor::WatermarkEditor(Document *document, RenderBackend *backend, const QVector<int> &selection,
                                 PageProcessor *processor, QObject *parent)
    : EditorMode(parent)
    , m_document(document)
    , m_backend(backend)
    , m_processor(processor)
    , m_selection(selection)
    , m_row(selection.isEmpty() ? 0 : selection.first())
    , m_view(new WatermarkView)
    , m_pageBox(new QSpinBox)
{
    setWidgets(buildStage(), buildPanel());
    showRow(m_row);
    refreshPreview();
}

QWidget *WatermarkEditor::buildStage()
{
    auto *stage = new QWidget;

    m_pageBox->setValue(m_row + 1);
    auto *navigation = pageNavigation(stage, m_pageBox, m_document->pageCount());

    auto *hint = new QLabel(i18n("What you see is drawn the same way the stamp will be, on the page it will go on. "
                                 "Turn to a busy page to check it stays readable through the watermark."),
                            stage);
    hint->setWordWrap(true);

    auto *layout = new QVBoxLayout(stage);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addLayout(navigation);
    layout->addWidget(m_view, 1);
    layout->addWidget(hint);

    connect(m_pageBox, &QSpinBox::valueChanged, this, [this](int value) { showRow(value - 1); });

    return stage;
}

QWidget *WatermarkEditor::buildPanel()
{
    auto *panel = new QWidget;

    // ── scope ─────────────────────────────────────────────────────────────
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

    // ── text or picture ───────────────────────────────────────────────────
    m_useText = new QRadioButton(i18nc("@option:radio", "Text"), panel);
    m_useImage = new QRadioButton(i18nc("@option:radio", "A picture (a logo or a stamp)"), panel);
    m_useText->setChecked(true);

    auto *kind = new QHBoxLayout;
    kind->addWidget(m_useText);
    kind->addWidget(m_useImage);
    kind->addStretch(1);

    // ── the text page ─────────────────────────────────────────────────────
    auto *textPage = new QWidget(panel);

    m_preset = new QComboBox(textPage);
    m_preset->addItems({ i18nc("@item:inlistbox watermark wording", "Draft"),
                         i18nc("@item:inlistbox watermark wording", "Confidential"),
                         i18nc("@item:inlistbox watermark wording", "Copy"),
                         i18nc("@item:inlistbox watermark wording", "Sample"),
                         i18nc("@item:inlistbox watermark wording", "Do not copy") });
    m_preset->setEditable(false);

    m_text = new QLineEdit(i18nc("@item default watermark wording", "DRAFT"), textPage);

    m_size = new QSpinBox(textPage);
    m_size->setRange(6, 400);
    m_size->setValue(48);
    m_size->setSuffix(i18nc("@item point suffix in a spin box", " pt"));

    m_opacity = new QSlider(Qt::Horizontal, textPage);
    m_opacity->setRange(5, 100);
    m_opacity->setValue(25);

    m_angle = new QSlider(Qt::Horizontal, textPage);
    m_angle->setRange(-90, 90);
    m_angle->setValue(45);

    m_outlineOnly = new QCheckBox(i18nc("@option:check", "Outline only, so the page stays readable"), textPage);

    m_colourButton = new QPushButton(textPage);
    m_colourButton->setAutoFillBackground(true);

    auto *textLayout = new QVBoxLayout(textPage);
    textLayout->setContentsMargins(0, 0, 0, 0);
    auto *presetRow = new QHBoxLayout;
    presetRow->addWidget(new QLabel(i18nc("@label:listbox", "Wording:"), textPage));
    presetRow->addWidget(m_preset, 1);
    textLayout->addLayout(presetRow);
    textLayout->addWidget(m_text);
    auto *sizeRow = new QHBoxLayout;
    sizeRow->addWidget(new QLabel(i18nc("@label:spinbox", "Size:"), textPage));
    sizeRow->addWidget(m_size);
    sizeRow->addWidget(new QLabel(i18nc("@label:chooser", "Colour:"), textPage));
    sizeRow->addWidget(m_colourButton, 1);
    textLayout->addLayout(sizeRow);
    textLayout->addWidget(new QLabel(i18nc("@label:slider", "How faint:"), textPage));
    textLayout->addWidget(m_opacity);
    textLayout->addWidget(new QLabel(i18nc("@label:slider", "Angle:"), textPage));
    textLayout->addWidget(m_angle);
    textLayout->addWidget(m_outlineOnly);

    // ── the picture page ──────────────────────────────────────────────────
    auto *imagePage = new QWidget(panel);

    m_imageButton = new QPushButton(QIcon::fromTheme(u"document-open"_s), i18nc("@action:button", "Choose a picture…"),
                                    imagePage);
    m_imageName = new QLabel(i18n("No picture chosen yet"), imagePage);
    m_imageName->setWordWrap(true);

    m_position = new QComboBox(imagePage);
    m_position->addItems(anchorNames());
    m_position->setCurrentIndex(4); // Centre

    m_imageSize = new QSlider(Qt::Horizontal, imagePage);
    m_imageSize->setRange(5, 100);
    m_imageSize->setValue(40);

    auto *imageLayout = new QVBoxLayout(imagePage);
    imageLayout->setContentsMargins(0, 0, 0, 0);
    imageLayout->addWidget(m_imageButton);
    imageLayout->addWidget(m_imageName);
    auto *positionRow = new QHBoxLayout;
    positionRow->addWidget(new QLabel(i18nc("@label:listbox", "Position:"), imagePage));
    positionRow->addWidget(m_position, 1);
    imageLayout->addLayout(positionRow);
    imageLayout->addWidget(new QLabel(i18nc("@label:slider", "Width, as a share of the page:"), imagePage));
    imageLayout->addWidget(m_imageSize);

    auto *pages = new QStackedWidget(panel);
    pages->addWidget(textPage);
    pages->addWidget(imagePage);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, panel);
    buttons->button(QDialogButtonBox::Ok)->setText(i18nc("@action:button", "Add the Watermark"));
    connect(buttons, &QDialogButtonBox::accepted, this, [this] { Q_EMIT finished(true); });
    connect(buttons, &QDialogButtonBox::rejected, this, [this] { Q_EMIT finished(false); });

    auto *layout = new QVBoxLayout(panel);
    layout->addWidget(scope);
    layout->addLayout(kind);
    layout->addWidget(pages, 1);
    layout->addWidget(buttons);

    // ── wiring ────────────────────────────────────────────────────────────
    // A settings change is what "the user has done something here" means. The
    // preview is also redrawn when the page is turned, which is not a decision
    // and must not make closing the panel ask a question.
    const auto redraw = [this] {
        m_touched = true;
        refreshPreview();
    };

    connect(m_useText, &QRadioButton::toggled, this, [pages, redraw](bool on) {
        pages->setCurrentIndex(on ? 0 : 1);
        redraw();
    });
    connect(m_preset, &QComboBox::currentTextChanged, this, [this](const QString &text) {
        // Upper case because that is what a watermark looks like, and because
        // the presets are single words where the shouting reads as a stamp
        // rather than as a sentence.
        m_text->setText(text.toUpper());
    });
    connect(m_text, &QLineEdit::textChanged, this, redraw);
    connect(m_size, &QSpinBox::valueChanged, this, redraw);
    connect(m_opacity, &QSlider::valueChanged, this, redraw);
    connect(m_angle, &QSlider::valueChanged, this, redraw);
    connect(m_outlineOnly, &QCheckBox::toggled, this, redraw);
    connect(m_position, &QComboBox::currentIndexChanged, this, redraw);
    connect(m_imageSize, &QSlider::valueChanged, this, redraw);
    connect(m_imageButton, &QPushButton::clicked, this, &WatermarkEditor::chooseImage);
    connect(m_colourButton, &QPushButton::clicked, this, [this, panel] {
        const QColor picked = QColorDialog::getColor(m_colour, panel, i18nc("@title:window", "Watermark Colour"));
        if (picked.isValid()) {
            m_colour = picked;
            m_colourButton->setStyleSheet(u"background-color: %1;"_s.arg(m_colour.name()));
            refreshPreview();
        }
    });

    m_colourButton->setStyleSheet(u"background-color: %1;"_s.arg(m_colour.name()));
    return panel;
}

void WatermarkEditor::showRow(int row)
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

Overlay::Anchor WatermarkEditor::chosenAnchor() const
{
    const int index = m_position ? m_position->currentIndex() : 4;
    return anchors[qBound(0, index, 8)];
}

Overlay::TextStamp WatermarkEditor::stamp() const
{
    Overlay::TextStamp stamp;
    stamp.text = m_text->text();
    stamp.fontSize = m_size->value();
    stamp.colour = m_colour;
    stamp.rotation = m_angle->value();
    stamp.opacity = m_opacity->value() / 100.0;
    stamp.outlineOnly = m_outlineOnly->isChecked();
    return stamp;
}

void WatermarkEditor::refreshPreview()
{
    if (m_useImage->isChecked() && !m_image.isNull()) {
        m_view->setImageStamp(m_image, chosenAnchor(), m_imageSize->value() / 100.0, 0.0,
                              m_opacity->value() / 100.0);
    } else {
        m_view->setTextStamp(stamp());
    }
}

void WatermarkEditor::chooseImage()
{
    const QString path
        = QFileDialog::getOpenFileName(panel(), i18nc("@title:window", "Watermark Picture"), QString(),
                                       i18n("Images (%1);;All files (*)",
                                            ImageIO::readableImageFilters().join(QLatin1Char(' '))));
    if (path.isEmpty()) {
        return;
    }

    QImage image(path);
    if (image.isNull()) {
        KMessageBox::error(panel(), i18n("“%1” could not be read as an image.", QFileInfo(path).fileName()),
                           title());
        return;
    }

    m_image = image;
    m_touched = true;
    m_imageName->setText(i18nc("@info the chosen picture and its size", "%1 (%2 × %3 pixels)",
                               QFileInfo(path).fileName(), image.width(), image.height()));
    m_useImage->setChecked(true);
    refreshPreview();
}

QVector<int> WatermarkEditor::targetRows() const
{
    if (m_selectionOnly->isChecked() && !m_selection.isEmpty()) {
        return m_selection;
    }
    QVector<int> rows(m_document->pageCount());
    std::iota(rows.begin(), rows.end(), 0);
    return rows;
}

QString WatermarkEditor::title() const
{
    return i18nc("@title:window", "Watermark");
}

QString WatermarkEditor::iconName() const
{
    return QStringLiteral("view-preview");
}

bool WatermarkEditor::isUnchanged() const
{
    // Nothing has been done to the document either way; this asks whether
    // leaving now would throw away a decision. Opening the panel and closing it
    // again is not one, however sensible the settings it came up with were.
    if (!m_touched) {
        return true;
    }
    return m_useImage->isChecked() ? m_image.isNull() : m_text->text().trimmed().isEmpty();
}

bool WatermarkEditor::commit(QString *error)
{
    if (isUnchanged()) {
        return true;
    }
    if (!m_processor) {
        if (error) {
            *error = i18n("This editor has nothing to stamp with.");
        }
        return false;
    }

    const QVector<int> rows = targetRows();
    const bool done = m_useImage->isChecked()
        ? m_processor->imageWatermark(rows, m_image, chosenAnchor(), m_imageSize->value() / 100.0, 0.0,
                                      m_opacity->value() / 100.0, error)
        : m_processor->watermark(rows, stamp(), error);
    if (!done) {
        return false;
    }

    Q_EMIT statusMessage(i18ncp("@info:status", "Watermarked %1 page.", "Watermarked %1 pages.", rows.size()));
    return true;
}

} // namespace ps
