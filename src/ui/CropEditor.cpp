/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "CropEditor.h"

#include "PageNavigation.h"

#include "CropView.h"
#include "PageProcessor.h"
#include "core/Document.h"
#include "core/RenderBackend.h"

#include <KLocalizedString>

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QWidget>

#include <numeric>

namespace ps {

namespace {

QImage rotated(const QImage &image, int degrees)
{
    if (degrees == 0 || image.isNull()) {
        return image;
    }
    return image.transformed(QTransform().rotate(degrees), Qt::SmoothTransformation);
}

} // namespace

CropEditor::CropEditor(Document *document, RenderBackend *backend, const QVector<int> &selection,
                       PageProcessor *processor, QObject *parent)
    : EditorMode(parent)
    , m_document(document)
    , m_backend(backend)
    , m_processor(processor)
    , m_selection(selection)
    , m_row(selection.isEmpty() ? 0 : selection.first())
    , m_view(new CropView)
    , m_pageBox(new QSpinBox)
    , m_left(new QDoubleSpinBox)
    , m_right(new QDoubleSpinBox)
    , m_top(new QDoubleSpinBox)
    , m_bottom(new QDoubleSpinBox)
    , m_reset(new QCheckBox(i18nc("@option:check", "Show the full pages again")))
    , m_note(new QLabel)
{
    setWidgets(buildStage(), buildPanel());

    // Both ways round: dragging moves the numbers, typing moves the box. Only
    // one of the two can be the truth, and it is the margins in the view.
    connect(m_view, &CropView::marginsChanged, this,
            [this](const PageCrop::Margins &margins) { pushToBoxes(margins); });
    for (QDoubleSpinBox *box : { m_left, m_right, m_top, m_bottom }) {
        connect(box, &QDoubleSpinBox::valueChanged, this, [this] { m_view->setMargins(fromBoxes()); });
    }

    showRow(m_row);
}

QWidget *CropEditor::buildStage()
{
    auto *stage = new QWidget;

    m_pageBox->setValue(m_row + 1);
    auto *navigation = pageNavigation(stage, m_pageBox, m_document->pageCount());

    auto *hint = new QLabel(i18n("Drag the edges of the bright area to set what stays visible. "
                                 "The trim applies to every page in the chosen range, not only this one."),
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

QWidget *CropEditor::buildPanel()
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

    for (QDoubleSpinBox *box : { m_left, m_right, m_top, m_bottom }) {
        box->setRange(0, 400);
        box->setDecimals(1);
        box->setSuffix(i18nc("@item point suffix in a spin box", " pt"));
    }

    auto *form = new QFormLayout;
    form->addRow(i18nc("@label:spinbox", "From the left:"), m_left);
    form->addRow(i18nc("@label:spinbox", "From the right:"), m_right);
    form->addRow(i18nc("@label:spinbox", "From the top:"), m_top);
    form->addRow(i18nc("@label:spinbox", "From the bottom:"), m_bottom);

    auto *detect = new QPushButton(QIcon::fromTheme(QStringLiteral("edit-select-all")),
                                   i18nc("@action:button", "Find the border for me"), panel);
    detect->setToolTip(i18nc("@info:tooltip",
                             "Looks at the pages and measures the empty margin, taking the smallest trim that is "
                             "safe for all of them."));
    connect(detect, &QPushButton::clicked, this, &CropEditor::detectMargins);

    m_note->setWordWrap(true);
    m_note->setText(i18n("Nothing is cut away. Trimming sets the visible area, so it can be undone at any time and "
                         "the page keeps its content."));

    connect(m_reset, &QCheckBox::toggled, this, [this, detect](bool on) {
        const QList<QWidget *> affected { m_left, m_right, m_top, m_bottom, detect };
        for (QWidget *widget : affected) {
            widget->setEnabled(!on);
        }
        // Showing the trim while promising to remove it would be two answers to
        // one question.
        m_view->setMargins(on ? PageCrop::Margins() : fromBoxes());
    });

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, panel);
    buttons->button(QDialogButtonBox::Ok)->setText(i18nc("@action:button", "Trim"));
    connect(buttons, &QDialogButtonBox::accepted, this, [this] { Q_EMIT finished(true); });
    connect(buttons, &QDialogButtonBox::rejected, this, [this] { Q_EMIT finished(false); });

    auto *layout = new QVBoxLayout(panel);
    layout->addWidget(scope);
    layout->addLayout(form);
    layout->addWidget(detect);
    layout->addSpacing(8);
    layout->addWidget(m_reset);
    layout->addWidget(m_note);
    layout->addStretch(1);
    layout->addWidget(buttons);

    return panel;
}

void CropEditor::showRow(int row)
{
    if (row < 0 || row >= m_document->pageCount()) {
        return;
    }
    m_row = row;

    const PageRef ref = m_document->pageAt(row);
    m_view->setPage(rotated(m_backend->renderPage(ref.sourceId, ref.sourcePage, 1400), ref.rotation),
                    m_document->pageSizePoints(row));
    m_view->setMargins(m_reset->isChecked() ? PageCrop::Margins() : fromBoxes());

    if (m_pageBox->value() != row + 1) {
        QSignalBlocker blocker(m_pageBox);
        m_pageBox->setValue(row + 1);
    }
}

void CropEditor::pushToBoxes(const PageCrop::Margins &margins)
{
    const QSignalBlocker l(m_left);
    const QSignalBlocker r(m_right);
    const QSignalBlocker t(m_top);
    const QSignalBlocker b(m_bottom);
    m_left->setValue(margins.left);
    m_right->setValue(margins.right);
    m_top->setValue(margins.top);
    m_bottom->setValue(margins.bottom);
}

PageCrop::Margins CropEditor::fromBoxes() const
{
    PageCrop::Margins margins;
    margins.left = m_left->value();
    margins.right = m_right->value();
    margins.top = m_top->value();
    margins.bottom = m_bottom->value();
    return margins;
}

void CropEditor::detectMargins()
{
    QVector<int> pages;
    const int count = m_document->pageCount();

    // Measured on at most a dozen pages: a four-hundred-page scan would take
    // noticeably long for an answer that barely changes after the first few.
    const int step = std::max(1, count / 12);
    for (int row = 0; row < count; row += step) {
        pages.append(m_document->pageAt(row).sourcePage);
    }
    if (pages.isEmpty()) {
        return;
    }

    const PageCrop::Margins found = PageCrop::detectCommon(m_backend, m_document->pageAt(0).sourceId, pages);
    if (found.isEmpty()) {
        m_note->setText(i18n("These pages have no empty border in common, so there is nothing to trim."));
        return;
    }

    pushToBoxes(found);
    m_view->setMargins(found);
    m_note->setText(
        i18n("Measured across %1 pages. Adjust anything that looks too tight before trimming.", pages.size()));
}

QVector<int> CropEditor::targetRows() const
{
    if (m_selectionOnly->isChecked() && !m_selection.isEmpty()) {
        return m_selection;
    }
    QVector<int> rows(m_document->pageCount());
    std::iota(rows.begin(), rows.end(), 0);
    return rows;
}

QString CropEditor::title() const
{
    return i18nc("@title:window", "Trim Pages");
}

QString CropEditor::iconName() const
{
    return QStringLiteral("transform-crop");
}

bool CropEditor::isUnchanged() const
{
    return !m_reset->isChecked() && fromBoxes().isEmpty();
}

bool CropEditor::commit(QString *error)
{
    const bool removing = m_reset->isChecked();
    const PageCrop::Margins margins = fromBoxes();
    if (!removing && margins.isEmpty()) {
        return true;
    }

    if (!m_processor) {
        if (error) {
            *error = i18nc("@info a fault inside the program, not anything the reader did",
                           "Nothing was trimmed: this editor has no document behind it. That is a fault in the "
                           "program, not in your file.");
        }
        return false;
    }

    const QVector<int> rows = targetRows();
    if (!m_processor->crop(rows, margins, removing, error)) {
        return false;
    }

    Q_EMIT statusMessage(removing ? i18ncp("@info:status", "Restored %1 page.", "Restored %1 pages.", rows.size())
                                  : i18ncp("@info:status", "Trimmed %1 page.", "Trimmed %1 pages.", rows.size()));
    return true;
}

} // namespace ps
