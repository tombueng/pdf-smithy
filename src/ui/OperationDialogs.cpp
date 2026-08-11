/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "OperationDialogs.h"

#include <QCheckBox>
#include <QPageSize>
#include <QColorDialog>
#include <QApplication>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QRadioButton>
#include <QSlider>
#include <QSpinBox>
#include <QHBoxLayout>
#include <QVBoxLayout>

#include <algorithm>

#include <KLocalizedString>
#include <KMessageBox>

namespace ps {

namespace {

/**
 * The "these pages or all of them" chooser both operations need.
 *
 * Defaulting to the selection when there is one is the whole point: it is what
 * makes "recognise the text on just this page" a thing you can actually do.
 */
QGroupBox *buildScopeBox(int selectedCount, int totalCount, QRadioButton **selectionOnly, QRadioButton **wholeDocument,
                         QWidget *parent)
{
    auto *box = new QGroupBox(i18nc("@title:group", "Apply to"), parent);

    *selectionOnly
        = new QRadioButton(i18ncp("@option:radio", "The selected page", "The %1 selected pages", selectedCount), box);
    *wholeDocument = new QRadioButton(
        i18ncp("@option:radio", "The whole document (%1 page)", "The whole document (%1 pages)", totalCount), box);

    (*selectionOnly)->setEnabled(selectedCount > 0);
    if (selectedCount > 0) {
        (*selectionOnly)->setChecked(true);
    } else {
        (*wholeDocument)->setChecked(true);
    }

    auto *layout = new QVBoxLayout(box);
    layout->addWidget(*selectionOnly);
    layout->addWidget(*wholeDocument);
    return box;
}

} // namespace

// ══ Insert position ═══════════════════════════════════════════════════════

namespace {

/**
 * The paper a person is likely to want, in the order they are likely to want it.
 *
 * Taken from QPageSize rather than written out here, so the millimetres are the
 * ones the rest of the desktop uses and a size added to Qt arrives for free.
 * The names are left untranslated on purpose: A4 and Letter are what the paper
 * is called in every language this program is likely to be read in.
 */
const QPageSize::PageSizeId KnownSizes[] = {
    QPageSize::A4, QPageSize::A3, QPageSize::A5, QPageSize::Letter, QPageSize::Legal, QPageSize::Tabloid,
};

} // namespace

BlankPagesDialog::BlankPagesDialog(const QSizeF &matchSize, QWidget *parent)
    : QDialog(parent)
    , m_matchSize(matchSize)
    , m_count(new QSpinBox(this))
    , m_size(new QComboBox(this))
    , m_upright(new QRadioButton(i18nc("@option:radio page orientation", "Upright"), this))
    , m_sideways(new QRadioButton(i18nc("@option:radio page orientation", "Sideways"), this))
{
    setWindowTitle(i18nc("@title:window", "Add Blank Pages"));

    m_count->setRange(1, 999);
    m_count->setValue(1);

    // Matching the document comes first and is chosen by default: a blank page
    // of a different size from the ones around it is nearly always a mistake.
    if (!m_matchSize.isEmpty()) {
        m_size->addItem(i18nc("@item:inlistbox page size", "Same as this document"));
    }
    for (const QPageSize::PageSizeId known : KnownSizes) {
        m_size->addItem(QPageSize::name(known));
    }

    auto *orientation = new QHBoxLayout;
    orientation->setContentsMargins(0, 0, 0, 0);
    orientation->addWidget(m_upright);
    orientation->addWidget(m_sideways);
    orientation->addStretch(1);
    m_upright->setChecked(true);

    // The orientation of a matched size is the document's own, so offering to
    // change it there would be offering to break the match.
    const auto followMatch = [this] {
        const bool matching = !m_matchSize.isEmpty() && m_size->currentIndex() == 0;
        m_upright->setEnabled(!matching);
        m_sideways->setEnabled(!matching);
    };
    connect(m_size, &QComboBox::currentIndexChanged, this, followMatch);
    followMatch();

    auto *form = new QFormLayout;
    form->addRow(i18nc("@label:spinbox", "How many pages:"), m_count);
    form->addRow(i18nc("@label:listbox", "Page size:"), m_size);
    form->addRow(i18nc("@label", "Orientation:"), orientation);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(i18nc("@action:button", "Add the Pages"));
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(buttons);
}

int BlankPagesDialog::pageCount() const
{
    return m_count->value();
}

QSizeF BlankPagesDialog::sizePoints() const
{
    const bool canMatch = !m_matchSize.isEmpty();
    const int index = m_size->currentIndex();
    if (canMatch && index == 0) {
        return m_matchSize;
    }

    const int known = canMatch ? index - 1 : index;
    if (known < 0 || known >= int(std::size(KnownSizes))) {
        return QSizeF(595.276, 841.89);
    }

    QSizeF size = QPageSize(KnownSizes[known]).size(QPageSize::Point);
    if (m_sideways->isChecked()) {
        size.transpose();
    }
    return size;
}

InsertPositionDialog::InsertPositionDialog(int selectedPage, int fileCount, QWidget *parent)
    : QDialog(parent)
    , m_start(new QRadioButton(i18nc("@option:radio", "At the beginning"), this))
    , m_before(new QRadioButton(this))
    , m_after(new QRadioButton(this))
    , m_end(new QRadioButton(i18nc("@option:radio", "At the end"), this))
{
    setWindowTitle(i18ncp("@title:window", "Insert File", "Insert %1 Files", fileCount));

    if (selectedPage > 0) {
        m_before->setText(i18nc("@option:radio", "Before page %1", selectedPage));
        m_after->setText(i18nc("@option:radio", "After page %1", selectedPage));
    } else {
        // Spelled out rather than greyed out with no explanation.
        m_before->setText(i18nc("@option:radio", "Before the selected page (none is selected)"));
        m_after->setText(i18nc("@option:radio", "After the selected page (none is selected)"));
        m_before->setEnabled(false);
        m_after->setEnabled(false);
    }

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(
        new QLabel(i18ncp("@label", "Where should the file go?", "Where should the files go?", fileCount), this));
    layout->addSpacing(6);
    layout->addWidget(m_start);
    layout->addWidget(m_before);
    layout->addWidget(m_after);
    layout->addWidget(m_end);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addSpacing(6);
    layout->addWidget(buttons);

    setPosition(selectedPage > 0 ? Position::AfterSelection : Position::End);
}

InsertPositionDialog::Position InsertPositionDialog::position() const
{
    if (m_start->isChecked()) {
        return Position::Start;
    }
    if (m_before->isChecked()) {
        return Position::BeforeSelection;
    }
    if (m_after->isChecked()) {
        return Position::AfterSelection;
    }
    return Position::End;
}

void InsertPositionDialog::setPosition(Position position)
{
    switch (position) {
    case Position::Start:
        m_start->setChecked(true);
        break;
    case Position::BeforeSelection:
        (m_before->isEnabled() ? m_before : m_end)->setChecked(true);
        break;
    case Position::AfterSelection:
        (m_after->isEnabled() ? m_after : m_end)->setChecked(true);
        break;
    case Position::End:
        m_end->setChecked(true);
        break;
    }
}

// ══ Compression ═══════════════════════════════════════════════════════════

CompressDialog::CompressDialog(int selectedCount, int totalCount, QWidget *parent)
    : QDialog(parent)
    , m_level(new QComboBox(this))
    , m_grayscale(new QCheckBox(i18nc("@option:check", "Convert to grayscale"), this))
    , m_note(new QLabel(this))
{
    setWindowTitle(i18nc("@title:window", "Make Smaller"));

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(buildScopeBox(selectedCount, totalCount, &m_selectionOnly, &m_wholeDocument, this));

    using Level = Compressor::Level;
    for (const Level level : { Level::Lossless, Level::Balanced, Level::Strong, Level::Extreme }) {
        m_level->addItem(Compressor::levelName(level), static_cast<int>(level));
    }
    m_level->setCurrentIndex(1);

    auto *form = new QFormLayout;
    form->addRow(i18nc("@label:listbox", "How hard:"), m_level);
    layout->addLayout(form);
    layout->addWidget(m_grayscale);

    m_grayscale->setToolTip(
        i18nc("@info:tooltip", "Often halves a colour scan, and no use at all on a text document."));

    m_note->setWordWrap(true);
    layout->addSpacing(6);
    layout->addWidget(m_note);

    // Answering "how much smaller?" before committing, by compressing a few
    // pages for real and scaling up. Waiting through a whole document just to
    // find out whether it is worth it is not a fair thing to ask.
    m_estimateButton = new QPushButton(QIcon::fromTheme(QStringLiteral("view-preview")),
                                       i18nc("@action:button", "How much would this save?"), this);
    m_estimateResult = new QLabel(this);
    m_estimateResult->setWordWrap(true);
    layout->addWidget(m_estimateButton);
    layout->addWidget(m_estimateResult);
    connect(m_estimateButton, &QPushButton::clicked, this, &CompressDialog::runEstimate);

    const auto updateNote = [this] {
        m_estimateResult->clear();
        const auto level = static_cast<Compressor::Level>(m_level->currentData().toInt());
        if (level == Compressor::Level::Lossless) {
            m_note->setText(i18n("Nothing is re-encoded. Every pixel survives, and the saving is usually modest."));
        } else if (!Compressor::isGhostscriptAvailable()) {
            m_note->setText(i18n("Ghostscript is not installed, so images cannot be shrunk. "
                                 "Install the “ghostscript” package, or choose the lossless level."));
        } else {
            m_note->setText(i18n("Images are re-encoded at %1 dpi. This cannot be undone by re-compressing, "
                                 "but it can be undone in this window.",
                                 Compressor::dpiForLevel(level)));
        }
    };
    connect(m_level, &QComboBox::currentIndexChanged, this, updateNote);
    updateNote();

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(i18nc("@action:button", "Make Smaller"));
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

void CompressDialog::setEstimator(Estimator estimator)
{
    m_estimator = std::move(estimator);
    m_estimateButton->setEnabled(static_cast<bool>(m_estimator));
}

void CompressDialog::runEstimate()
{
    if (!m_estimator) {
        return;
    }

    m_estimateButton->setEnabled(false);
    m_estimateResult->setText(i18n("Measuring a few pages…"));
    QApplication::setOverrideCursor(Qt::BusyCursor);
    QCoreApplication::processEvents();

    Compressor::Estimate estimate;
    QString error;
    const bool ok = m_estimator(options(), &estimate, &error);

    QApplication::restoreOverrideCursor();
    m_estimateButton->setEnabled(true);

    if (!ok) {
        m_estimateResult->setText(error.isEmpty() ? i18n("The estimate could not be made.") : error);
        return;
    }

    const QString sizes
        = i18n("%1 → about %2, roughly %3% smaller", QLocale().formattedDataSize(estimate.originalBytes),
               QLocale().formattedDataSize(estimate.expectedBytes), QString::number(estimate.savedPercent()));

    // Labelled as the extrapolation it is. A number presented as exact when it
    // came from four pages would be a small lie repeated on every document.
    m_estimateResult->setText(estimate.reliable ? i18np("%2, measured on %1 page.", "%2, measured on %1 pages.",
                                                        estimate.pagesSampled, sizes)
                                                : i18n("%1, a rough guess from very few pages.", sizes));
}

bool CompressDialog::appliesToSelectionOnly() const
{
    return m_selectionOnly->isChecked();
}

Compressor::Options CompressDialog::options() const
{
    Compressor::Options options;
    options.level = static_cast<Compressor::Level>(m_level->currentData().toInt());
    options.grayscale = m_grayscale->isChecked();
    return options;
}

// ══ Text recognition ══════════════════════════════════════════════════════

#ifdef PS_WITH_OCR

bool ScanDialog::isUsable()
{
    return ScanProcessor::isAvailable();
}

ScanDialog::ScanDialog(int selectedCount, int totalCount, QWidget *parent)
    : QDialog(parent)
    , m_languages(new QListWidget(this))
    , m_resolution(new QComboBox(this))
    , m_straighten(new QCheckBox(i18nc("@option:check", "Straighten crooked pages"), this))
    , m_skipPagesWithText(new QCheckBox(i18nc("@option:check", "Skip pages that already have text"), this))
    , m_evenOutLighting(new QCheckBox(i18nc("@option:check", "Even out uneven lighting"), this))
    , m_despeckle(new QSpinBox(this))
{
    setWindowTitle(i18nc("@title:window", "Recognise Text"));

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(buildScopeBox(selectedCount, totalCount, &m_selectionOnly, &m_wholeDocument, this));

    m_languages->setSelectionMode(QAbstractItemView::NoSelection);
    m_languages->setMaximumHeight(130);
    const QStringList available = ScanProcessor::availableLanguages();
    for (const QString &code : available) {
        auto *item = new QListWidgetItem(ScanProcessor::languageName(code), m_languages);
        item->setData(Qt::UserRole, code);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        // Whatever the interface is running in is the best first guess.
        const bool preselect = (code == QLatin1String("deu") && QLocale().language() == QLocale::German)
            || (code == QLatin1String("eng") && QLocale().language() == QLocale::English);
        item->setCheckState(preselect ? Qt::Checked : Qt::Unchecked);
    }
    // Never leave the user with nothing ticked.
    bool anyChecked = false;
    for (int i = 0; i < m_languages->count(); ++i) {
        anyChecked = anyChecked || m_languages->item(i)->checkState() == Qt::Checked;
    }
    if (!anyChecked && m_languages->count() > 0) {
        m_languages->item(0)->setCheckState(Qt::Checked);
    }

    m_resolution->addItem(i18nc("@item scan resolution", "200 dpi (fastest)"), 200);
    m_resolution->addItem(i18nc("@item scan resolution", "300 dpi (recommended)"), 300);
    m_resolution->addItem(i18nc("@item scan resolution", "400 dpi (small print)"), 400);
    m_resolution->addItem(i18nc("@item scan resolution", "600 dpi (slow, for poor scans)"), 600);
    m_resolution->setCurrentIndex(1);

    auto *form = new QFormLayout;
    form->addRow(i18nc("@label:listbox", "Languages:"), m_languages);
    form->addRow(i18nc("@label:listbox", "Detail:"), m_resolution);
    layout->addLayout(form);

    m_straighten->setChecked(true);
    m_skipPagesWithText->setChecked(true);
    m_skipPagesWithText->setToolTip(i18nc("@info:tooltip",
                                          "A page that already has text would end up with two text layers, "
                                          "which is how searchable documents get ruined."));
    layout->addWidget(m_straighten);
    layout->addWidget(m_skipPagesWithText);

    // Grouped and labelled apart from the rest, because these two change only
    // what Tesseract looks at. Cleaning up the page itself would mean
    // re-encoding it, and a scan that comes out different from how it went in
    // is not something this application does.
    auto *cleanup = new QGroupBox(i18nc("@title:group", "Help the recognition along"), this);
    m_despeckle->setRange(0, 40);
    m_despeckle->setValue(0);
    m_despeckle->setSpecialValueText(i18nc("@item spin box zero value", "off"));
    m_despeckle->setSuffix(i18nc("@item pixel suffix in a spin box", " px"));
    m_despeckle->setToolTip(i18nc("@info:tooltip", "Ignore specks smaller than this: dust and scanner noise."));
    m_evenOutLighting->setToolTip(
        i18nc("@info:tooltip", "For photographed pages that are bright in the middle and dim at the edges."));

    auto *cleanupForm = new QFormLayout;
    cleanupForm->addRow(i18nc("@label:spinbox", "Ignore specks up to:"), m_despeckle);
    auto *cleanupLayout = new QVBoxLayout(cleanup);
    cleanupLayout->addLayout(cleanupForm);
    cleanupLayout->addWidget(m_evenOutLighting);
    cleanupLayout->addWidget(
        new QLabel(i18n("These affect recognition only. The page itself is never re-encoded."), cleanup));
    layout->addWidget(cleanup);

    auto *hint = new QLabel(i18n("The pages themselves are not changed. The recognised words are laid over them "
                                 "invisibly, so the document looks the same and becomes selectable."),
                            this);
    hint->setWordWrap(true);
    layout->addSpacing(6);
    layout->addWidget(hint);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(i18nc("@action:button", "Recognise Text"));
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

bool ScanDialog::appliesToSelectionOnly() const
{
    return m_selectionOnly->isChecked();
}

ScanProcessor::Options ScanDialog::options() const
{
    ScanProcessor::Options options;

    QStringList languages;
    for (int i = 0; i < m_languages->count(); ++i) {
        if (m_languages->item(i)->checkState() == Qt::Checked) {
            languages.append(m_languages->item(i)->data(Qt::UserRole).toString());
        }
    }
    if (!languages.isEmpty()) {
        options.languages = languages;
    }

    options.dpi = m_resolution->currentData().toInt();
    options.straighten = m_straighten->isChecked();
    options.skipPagesWithText = m_skipPagesWithText->isChecked();
    options.despeckle = m_despeckle->value();
    options.evenOutLighting = m_evenOutLighting->isChecked();
    return options;
}

#endif // PS_WITH_OCR

} // namespace ps
