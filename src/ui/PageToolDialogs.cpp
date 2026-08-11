/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "PageToolDialogs.h"

#include "core/Document.h"
#include "core/PageRange.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QPainter>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include <KLocalizedString>

#include <numeric>

namespace ps {

namespace {

QGroupBox *scopeBox(int selectedCount, int totalCount, QRadioButton **selectionOnly, QRadioButton **wholeDocument,
                    QWidget *parent)
{
    auto *box = new QGroupBox(i18nc("@title:group", "Apply to"), parent);
    *selectionOnly
        = new QRadioButton(i18ncp("@option:radio", "The selected page", "The %1 selected pages", selectedCount), box);
    *wholeDocument = new QRadioButton(
        i18ncp("@option:radio", "The whole document (%1 page)", "The whole document (%1 pages)", totalCount), box);

    (*selectionOnly)->setEnabled(selectedCount > 0);
    ((selectedCount > 0) ? *selectionOnly : *wholeDocument)->setChecked(true);

    auto *layout = new QVBoxLayout(box);
    layout->addWidget(*selectionOnly);
    layout->addWidget(*wholeDocument);
    return box;
}

} // namespace

// ══ Splitting ═════════════════════════════════════════════════════════════

SplitDialog::SplitDialog(const Document *document, const QString &sourcePath, QWidget *parent)
    : QDialog(parent)
    , m_document(document)
    , m_sourcePath(sourcePath)
    , m_everyN(new QRadioButton(i18nc("@option:radio", "Every"), this))
    , m_ranges(new QRadioButton(i18nc("@option:radio", "These page ranges"), this))
    , m_bySize(new QRadioButton(i18nc("@option:radio", "Pieces no bigger than"), this))
    , m_byBookmarks(new QRadioButton(this))
    , m_pagesPerFile(new QSpinBox(this))
    , m_rangeText(new QLineEdit(this))
    , m_maxMegabytes(new QSpinBox(this))
    , m_outputTemplate(new QLineEdit(this))
    , m_preview(new QListWidget(this))
    , m_summary(new QLabel(this))
{
    setWindowTitle(i18nc("@title:window", "Split Document"));
    resize(620, 620);

    m_chapters = Splitter::chaptersOf(sourcePath);
    m_byBookmarks->setText(m_chapters.isEmpty() ? i18nc("@option:radio", "At each bookmark (this document has none)")
                                                : i18ncp("@option:radio", "At each bookmark (one found)",
                                                         "At each bookmark (%1 found)", m_chapters.size()));
    m_byBookmarks->setEnabled(!m_chapters.isEmpty());

    m_pagesPerFile->setRange(1, 1000);
    m_pagesPerFile->setValue(1);
    m_pagesPerFile->setSuffix(i18ncp("@item page suffix in a spin box", " page", " pages", 1));

    m_maxMegabytes->setRange(1, 500);
    m_maxMegabytes->setValue(5);
    m_maxMegabytes->setSuffix(i18nc("@item megabyte suffix in a spin box", " MB"));

    m_rangeText->setPlaceholderText(i18nc("@info:placeholder %1 is the word for the final page, as in “10-last”",
                                          "1-3, 4-9, 10-%1", PageRange::lastWord()));
    m_rangeText->setToolTip(i18nc("@info:tooltip", "Each group separated by a comma becomes its own file."));

    const QFileInfo info(sourcePath);
    m_outputTemplate->setText(info.absolutePath().isEmpty() ? QStringLiteral("part-%1.pdf")
                                                            : info.absolutePath() + QLatin1Char('/')
                                      + info.completeBaseName() + QStringLiteral("-%1.pdf"));

    auto *whereBox = new QGroupBox(i18nc("@title:group", "Cut the document"), this);
    auto *whereLayout = new QVBoxLayout(whereBox);

    auto *everyRow = new QHBoxLayout;
    everyRow->addWidget(m_everyN);
    everyRow->addWidget(m_pagesPerFile);
    everyRow->addStretch();
    whereLayout->addLayout(everyRow);

    whereLayout->addWidget(m_ranges);
    whereLayout->addWidget(m_rangeText);

    auto *sizeRow = new QHBoxLayout;
    sizeRow->addWidget(m_bySize);
    sizeRow->addWidget(m_maxMegabytes);
    sizeRow->addStretch();
    whereLayout->addLayout(sizeRow);

    whereLayout->addWidget(m_byBookmarks);

    m_everyN->setChecked(true);

    auto *nameForm = new QFormLayout;
    nameForm->addRow(i18nc("@label:textbox", "Write to:"), m_outputTemplate);

    m_preview->setAlternatingRowColors(true);
    m_summary->setWordWrap(true);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(whereBox);
    layout->addLayout(nameForm);
    layout->addWidget(new QLabel(i18nc("@label", "This will write:"), this));
    layout->addWidget(m_preview, 1);
    layout->addWidget(m_summary);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(i18nc("@action:button", "Split"));
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    const auto refresh = [this] {
        m_pagesPerFile->setEnabled(m_everyN->isChecked());
        m_rangeText->setEnabled(m_ranges->isChecked());
        m_maxMegabytes->setEnabled(m_bySize->isChecked());
        updatePreview();
    };
    for (QRadioButton *button : { m_everyN, m_ranges, m_bySize, m_byBookmarks }) {
        connect(button, &QRadioButton::toggled, this, refresh);
    }
    connect(m_pagesPerFile, &QSpinBox::valueChanged, this, refresh);
    connect(m_rangeText, &QLineEdit::textChanged, this, refresh);
    connect(m_outputTemplate, &QLineEdit::textChanged, this, refresh);
    refresh();
}

void SplitDialog::updatePreview()
{
    m_preview->clear();
    if (!m_document) {
        return;
    }

    const int pageCount = m_document->pageCount();
    QVector<QPair<QString, int>> parts; // name, page count

    if (m_everyN->isChecked()) {
        const int step = m_pagesPerFile->value();
        const int files = (pageCount + step - 1) / step;
        for (int i = 0; i < files; ++i) {
            const int pages = std::min(step, pageCount - i * step);
            parts.append({ Splitter::expandTemplate(m_outputTemplate->text(), i + 1, files), pages });
        }
    } else if (m_ranges->isChecked()) {
        const QStringList groups = m_rangeText->text().split(QLatin1Char(','), Qt::SkipEmptyParts);
        for (int i = 0; i < groups.size(); ++i) {
            const QVector<int> pages = PageRange::parse(groups.at(i), pageCount, nullptr);
            parts.append({ Splitter::expandTemplate(m_outputTemplate->text(), i + 1, groups.size()), pages.size() });
        }
    } else if (m_byBookmarks->isChecked()) {
        for (int i = 0; i < m_chapters.size(); ++i) {
            const int from = m_chapters.at(i).firstPage;
            const int to = (i + 1 < m_chapters.size()) ? m_chapters.at(i + 1).firstPage : pageCount;
            // Named after the bookmark, which is what will actually be written.
            parts.append({ QStringLiteral("%1 - %2.pdf")
                               .arg(i + 1, m_chapters.size() < 10 ? 1 : 2, 10, QLatin1Char('0'))
                               .arg(m_chapters.at(i).title),
                           to - from });
        }
    } else {
        // Sizes cannot be known without writing the files, so this one says so
        // rather than inventing a number.
        m_summary->setText(i18n("The pieces are measured as they are written, so the exact split is only known "
                                "afterwards. Nothing existing is overwritten without asking."));
        m_preview->addItem(i18n("Pieces of at most %1 MB", m_maxMegabytes->value()));
        return;
    }

    for (const auto &part : std::as_const(parts)) {
        m_preview->addItem(i18nc("@item file name and how many pages it gets", "%1 (%2)",
                                 QFileInfo(part.first).fileName(),
                                 i18ncp("@item", "%1 page", "%1 pages", part.second)));
    }

    m_summary->setText(i18ncp("@info", "%1 file will be written.", "%1 files will be written.", parts.size()));
}

Splitter::Options SplitDialog::options() const
{
    Splitter::Options options;
    options.outputTemplate = m_outputTemplate->text();

    if (m_ranges->isChecked()) {
        options.mode = Splitter::Mode::Ranges;
        options.ranges = m_rangeText->text();
    } else if (m_bySize->isChecked()) {
        options.mode = Splitter::Mode::MaxFileSize;
        options.maxBytes = static_cast<qint64>(m_maxMegabytes->value()) * 1024 * 1024;
    } else if (m_byBookmarks->isChecked()) {
        options.mode = Splitter::Mode::Bookmarks;
        options.bookmarkSource = m_sourcePath;
    } else {
        options.mode = Splitter::Mode::EveryNPages;
        options.pagesPerFile = m_pagesPerFile->value();
    }

    return options;
}

// ── NUpDialog ─────────────────────────────────────────────────────────────

namespace {

/** Draws the chosen grid, so the arrangement can be seen rather than imagined. */
class SheetPreview : public QWidget
{
public:
    explicit SheetPreview(QWidget *parent)
        : QWidget(parent)
    {
        setMinimumSize(150, 190);
    }

    void setGrid(const QSize &grid, bool landscape, bool borders, bool downFirst)
    {
        m_grid = grid;
        m_landscape = landscape;
        m_borders = borders;
        m_downFirst = downFirst;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);

        const double ratio = m_landscape ? 297.0 / 210.0 : 210.0 / 297.0;
        QSizeF sheet(height() * ratio, height());
        if (sheet.width() > width()) {
            sheet = QSizeF(width(), width() / ratio);
        }
        const QRectF paper((width() - sheet.width()) / 2.0, (height() - sheet.height()) / 2.0, sheet.width(),
                           sheet.height());

        painter.fillRect(paper, palette().color(QPalette::Base));
        painter.setPen(palette().color(QPalette::Mid));
        painter.drawRect(paper);

        const double margin = paper.width() * 0.05;
        const double gutter = paper.width() * 0.02;
        const double cellWidth = (paper.width() - 2 * margin - (m_grid.width() - 1) * gutter) / m_grid.width();
        const double cellHeight = (paper.height() - 2 * margin - (m_grid.height() - 1) * gutter) / m_grid.height();

        QFont numbers = font();
        numbers.setPointSizeF(qMax(6.0, qMin(cellWidth, cellHeight) * 0.45));
        painter.setFont(numbers);

        for (int slot = 0; slot < m_grid.width() * m_grid.height(); ++slot) {
            const int column = m_downFirst ? slot / m_grid.height() : slot % m_grid.width();
            const int row = m_downFirst ? slot % m_grid.height() : slot / m_grid.width();
            const QRectF cell(paper.left() + margin + column * (cellWidth + gutter),
                              paper.top() + margin + row * (cellHeight + gutter), cellWidth, cellHeight);

            painter.fillRect(cell, palette().color(QPalette::AlternateBase));
            if (m_borders) {
                painter.setPen(palette().color(QPalette::Mid));
                painter.drawRect(cell);
            }
            painter.setPen(palette().color(QPalette::PlaceholderText));
            painter.drawText(cell, Qt::AlignCenter, QString::number(slot + 1));
        }
    }

private:
    QSize m_grid { 2, 2 };
    bool m_landscape = false;
    bool m_borders = false;
    bool m_downFirst = false;
};

} // namespace

NUpDialog::NUpDialog(int selectedCount, int totalCount, bool pagesAreLandscape, QWidget *parent)
    : QDialog(parent)
    , m_pagesAreLandscape(pagesAreLandscape)
    , m_perSheet(new QComboBox(this))
    , m_margin(new QDoubleSpinBox(this))
    , m_gutter(new QDoubleSpinBox(this))
    , m_borders(new QCheckBox(i18nc("@option:check", "Draw a thin line around each page"), this))
    , m_downFirst(new QCheckBox(i18nc("@option:check", "Fill downwards before moving right"), this))
    , m_summary(new QLabel(this))
    , m_preview(new SheetPreview(this))
{
    setWindowTitle(i18nc("@title:window", "Arrange on Sheets"));
    resize(620, 480);

    for (int count : { 2, 4, 6, 8, 9, 16 }) {
        m_perSheet->addItem(i18ncp("@item:inlistbox", "%1 page per sheet", "%1 pages per sheet", count), count);
    }
    m_perSheet->setCurrentIndex(1);

    m_margin->setRange(0, 120);
    m_margin->setDecimals(1);
    m_margin->setValue(14);
    m_gutter->setRange(0, 120);
    m_gutter->setDecimals(1);
    m_gutter->setValue(8);
    for (QDoubleSpinBox *box : { m_margin, m_gutter }) {
        box->setSuffix(i18nc("@item point suffix in a spin box", " pt"));
    }

    auto *form = new QFormLayout;
    form->addRow(i18nc("@label:listbox", "Arrangement:"), m_perSheet);
    form->addRow(i18nc("@label:spinbox", "Border around the sheet:"), m_margin);
    form->addRow(i18nc("@label:spinbox", "Gap between pages:"), m_gutter);

    m_summary->setWordWrap(true);

    auto *left = new QVBoxLayout;
    left->addWidget(scopeBox(selectedCount, totalCount, &m_selectionOnly, &m_wholeDocument, this));
    left->addLayout(form);
    left->addWidget(m_borders);
    left->addWidget(m_downFirst);
    left->addSpacing(8);
    left->addWidget(m_summary);
    left->addStretch(1);

    auto *columns = new QHBoxLayout;
    columns->addLayout(left, 3);
    columns->addWidget(m_preview, 2);

    auto *note = new QLabel(i18n("Nothing is re-rendered. The pages keep their text, so the sheets stay selectable "
                                 "and searchable."),
                            this);
    note->setWordWrap(true);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(i18nc("@action:button", "Arrange"));
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(columns, 1);
    layout->addWidget(note);
    layout->addWidget(buttons);

    connect(m_perSheet, &QComboBox::currentIndexChanged, this, &NUpDialog::refresh);
    connect(m_borders, &QCheckBox::toggled, this, &NUpDialog::refresh);
    connect(m_downFirst, &QCheckBox::toggled, this, &NUpDialog::refresh);
    refresh();
}

void NUpDialog::refresh()
{
    const NUp::Options chosen = options();
    static_cast<SheetPreview *>(m_preview)->setGrid(
        QSize(chosen.columns, chosen.rows), chosen.columns > chosen.rows ? !m_pagesAreLandscape : m_pagesAreLandscape,
        chosen.drawBorders, chosen.columnsFirst);
    m_summary->setText(
        i18nc("@info", "%1 across and %2 down, on a sheet the size of one page.", chosen.columns, chosen.rows));
}

bool NUpDialog::appliesToSelectionOnly() const
{
    return m_selectionOnly->isChecked();
}

NUp::Options NUpDialog::options() const
{
    const QSize grid = NUp::gridFor(m_perSheet->currentData().toInt(), m_pagesAreLandscape);

    NUp::Options chosen;
    chosen.columns = grid.width();
    chosen.rows = grid.height();
    chosen.marginPoints = m_margin->value();
    chosen.gutterPoints = m_gutter->value();
    chosen.drawBorders = m_borders->isChecked();
    chosen.columnsFirst = m_downFirst->isChecked();
    return chosen;
}

} // namespace ps
