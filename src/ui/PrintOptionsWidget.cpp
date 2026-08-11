/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "PrintOptionsWidget.h"

#include "core/PageRange.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QSpinBox>
#include <QVBoxLayout>

#include <KLocalizedString>

namespace ps {

PrintOptionsWidget::PrintOptionsWidget(QWidget *parent)
    : QWidget(parent)
    , m_range(new QLineEdit(this))
    , m_layout(new QComboBox(this))
    , m_scaling(new QComboBox(this))
    , m_customScale(new QSpinBox(this))
    , m_subset(new QComboBox(this))
    , m_reverse(new QCheckBox(i18nc("@option:check", "Print back to front"), this))
    , m_grayscale(new QCheckBox(i18nc("@option:check", "Print in greyscale"), this))
    , m_borders(new QCheckBox(i18nc("@option:check", "Draw a line around each page"), this))
    , m_margin(new QSpinBox(this))
    , m_summary(new QLabel(this))
{
    setWindowTitle(i18nc("@title:tab", "Pages && Layout"));

    // The keywords come from the parser rather than from the sentence, so that
    // the box can only ever offer words the parser also takes.
    m_range->setPlaceholderText(i18nc("@info:placeholder %1 is the word for the final page, as in “12-last”",
                                      "All pages, or 1-4, 8, 12-%1", PageRange::lastWord()));
    m_range->setToolTip(i18nc("@info:tooltip %1 is the word for the odd-numbered pages and %2 the word for the "
                              "even-numbered ones",
                              "Which pages to print. Leave empty for all.\n"
                              "Ranges, single pages and “%1”/“%2” all work, "
                              "and a descending range prints backwards.",
                              PageRange::oddWord(), PageRange::evenWord()));

    using Layout = PrintController::Layout;
    for (const Layout layout : { Layout::Normal, Layout::TwoUp, Layout::FourUp, Layout::SixUp, Layout::NineUp,
                                 Layout::SixteenUp, Layout::Booklet }) {
        m_layout->addItem(PrintController::layoutName(layout), QVariant::fromValue(static_cast<int>(layout)));
    }

    using Scaling = PrintController::Scaling;
    m_scaling->addItem(i18nc("@item scaling", "Shrink oversized pages"), static_cast<int>(Scaling::ShrinkToFit));
    m_scaling->addItem(i18nc("@item scaling", "Fit to the paper"), static_cast<int>(Scaling::FitToPage));
    m_scaling->addItem(i18nc("@item scaling", "Actual size"), static_cast<int>(Scaling::ActualSize));
    m_scaling->addItem(i18nc("@item scaling", "Custom scale"), static_cast<int>(Scaling::Custom));

    m_customScale->setRange(10, 400);
    m_customScale->setValue(100);
    m_customScale->setSuffix(i18nc("@item percent suffix in a spin box", " %"));
    m_customScale->setEnabled(false);

    using Subset = PrintController::Subset;
    m_subset->addItem(i18nc("@item page subset", "Every page"), static_cast<int>(Subset::All));
    m_subset->addItem(i18nc("@item page subset", "Front sides only"), static_cast<int>(Subset::OddOnly));
    m_subset->addItem(i18nc("@item page subset", "Back sides only"), static_cast<int>(Subset::EvenOnly));
    m_subset->setToolTip(i18nc("@info:tooltip",
                               "For duplex on a printer that cannot do it itself: "
                               "print the front sides, turn the stack over, print the back sides."));

    m_margin->setRange(0, 40);
    m_margin->setSuffix(i18nc("@item millimetre suffix in a spin box", " mm"));

    auto *form = new QFormLayout;
    form->addRow(i18nc("@label:textbox", "Pages:"), m_range);
    form->addRow(i18nc("@label:listbox", "Layout:"), m_layout);
    form->addRow(i18nc("@label:listbox", "Size:"), m_scaling);
    form->addRow(i18nc("@label:spinbox", "Scale:"), m_customScale);
    form->addRow(i18nc("@label:listbox", "Sides:"), m_subset);
    form->addRow(i18nc("@label:spinbox", "Extra margin:"), m_margin);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(m_reverse);
    layout->addWidget(m_grayscale);
    layout->addWidget(m_borders);
    layout->addSpacing(8);

    m_summary->setWordWrap(true);
    QFont summaryFont = m_summary->font();
    summaryFont.setBold(true);
    m_summary->setFont(summaryFont);
    layout->addWidget(m_summary);
    layout->addStretch();

    const auto notify = [this] {
        // Only the custom option needs a percentage, so the box follows it.
        m_customScale->setEnabled(m_scaling->currentData().toInt()
                                  == static_cast<int>(PrintController::Scaling::Custom));
        Q_EMIT optionsChanged();
    };

    connect(m_range, &QLineEdit::textChanged, this, notify);
    connect(m_layout, &QComboBox::currentIndexChanged, this, notify);
    connect(m_scaling, &QComboBox::currentIndexChanged, this, notify);
    connect(m_subset, &QComboBox::currentIndexChanged, this, notify);
    connect(m_customScale, &QSpinBox::valueChanged, this, notify);
    connect(m_margin, &QSpinBox::valueChanged, this, notify);
    connect(m_reverse, &QCheckBox::toggled, this, notify);
    connect(m_grayscale, &QCheckBox::toggled, this, notify);
    connect(m_borders, &QCheckBox::toggled, this, notify);
}

PrintController::Options PrintOptionsWidget::options() const
{
    PrintController::Options options;
    options.range = m_range->text();
    options.layout = static_cast<PrintController::Layout>(m_layout->currentData().toInt());
    options.scaling = static_cast<PrintController::Scaling>(m_scaling->currentData().toInt());
    options.customScalePercent = m_customScale->value();
    options.subset = static_cast<PrintController::Subset>(m_subset->currentData().toInt());
    options.reverseOrder = m_reverse->isChecked();
    options.grayscale = m_grayscale->isChecked();
    options.pageBorders = m_borders->isChecked();
    options.marginMm = m_margin->value();
    return options;
}

void PrintOptionsWidget::setOptions(const PrintController::Options &options)
{
    m_range->setText(options.range);
    m_layout->setCurrentIndex(m_layout->findData(static_cast<int>(options.layout)));
    m_scaling->setCurrentIndex(m_scaling->findData(static_cast<int>(options.scaling)));
    m_customScale->setValue(options.customScalePercent);
    m_subset->setCurrentIndex(m_subset->findData(static_cast<int>(options.subset)));
    m_reverse->setChecked(options.reverseOrder);
    m_grayscale->setChecked(options.grayscale);
    m_borders->setChecked(options.pageBorders);
    m_margin->setValue(static_cast<int>(options.marginMm));
}

void PrintOptionsWidget::setSummary(const QString &text)
{
    m_summary->setText(text);
}

} // namespace ps
