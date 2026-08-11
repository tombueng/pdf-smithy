/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "ToolDialogs.h"

#include "ToolSupport.h"
#include "core/ColourTools.h"
#include "core/Document.h"
#include "core/PageRange.h"

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
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>

using namespace Qt::Literals::StringLiterals;

namespace ps::tools {

namespace {

/** A button that shows the colour it holds. */
class ColourButton : public QPushButton
{
public:
    explicit ColourButton(const QColor &initial, QWidget *parent = nullptr)
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

    QColor colour() const
    {
        return m_colour;
    }

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

QString describeTarget(ColourTools::Target target)
{
    switch (target) {
    case ColourTools::Target::Grayscale:
        return i18nc("@item:inlistbox colour target", "Greyscale");
    case ColourTools::Target::BlackWhite:
        return i18nc("@item:inlistbox colour target", "Black and white only");
    case ColourTools::Target::Rgb:
        return i18nc("@item:inlistbox colour target", "RGB");
    case ColourTools::Target::Cmyk:
        return i18nc("@item:inlistbox colour target", "CMYK");
    }
    return {};
}

/**
 * Everything a print shop asks about a document's colour, in one window.
 *
 * Seven jobs that all read and write the same file and all belong to the same
 * conversation ("will this print?"), so they are tabs rather than seven menu
 * entries. Each writes a *new* file and says which route it took, because the
 * difference between rewriting a colour operator and re-encoding a photograph
 * is the difference between a lossless edit and a lossy one, and the user is
 * the one who has to decide whether that was acceptable.
 */
class ColourDialog : public QDialog
{
public:
    ColourDialog(Document *document, const QString &pdf, QWidget *parent);

private:
    QWidget *buildOverview();
    QWidget *buildConvert();
    QWidget *buildReplace();
    QWidget *buildSeparations();
    QWidget *buildPrepress();
    QWidget *buildPlates();

    void inspect();
    void measureInk();
    void runConvert();
    void runReplace();
    void runSeparations();
    void runHairlines();
    void runOverprint();
    void showPlate();

    /** Zero-based pages from the range box, or empty for all of them. */
    QVector<int> chosenPages();

    Document *m_document = nullptr;
    QString m_pdf;
    ColourTools::Inventory m_inventory;

    QPlainTextEdit *m_report = nullptr;
    QLineEdit *m_pages = nullptr;
    QComboBox *m_target = nullptr;
    QLineEdit *m_profile = nullptr;
    QDoubleSpinBox *m_threshold = nullptr;
    QCheckBox *m_recompress = nullptr;
    QSpinBox *m_quality = nullptr;

    ColourButton *m_from = nullptr;
    ColourButton *m_to = nullptr;
    QDoubleSpinBox *m_tolerance = nullptr;

    QTableWidget *m_inks = nullptr;

    QDoubleSpinBox *m_hairline = nullptr;
    QRadioButton *m_overprintOn = nullptr;

    QSpinBox *m_platePage = nullptr;
    QComboBox *m_plateInk = nullptr;
    QSpinBox *m_plateDpi = nullptr;
    QLabel *m_plate = nullptr;
};

ColourDialog::ColourDialog(Document *document, const QString &pdf, QWidget *parent)
    : QDialog(parent)
    , m_document(document)
    , m_pdf(pdf)
{
    setWindowTitle(i18nc("@title:window", "Colour"));
    resize(1000, 760);

    auto *tabs = new QTabWidget(this);
    tabs->addTab(buildOverview(), i18nc("@title:tab", "What Is in Here"));
    tabs->addTab(buildConvert(), i18nc("@title:tab", "Convert"));
    tabs->addTab(buildReplace(), i18nc("@title:tab", "Replace a Colour"));
    tabs->addTab(buildSeparations(), i18nc("@title:tab", "Spot Colours"));
    tabs->addTab(buildPrepress(), i18nc("@title:tab", "Prepress"));
    tabs->addTab(buildPlates(), i18nc("@title:tab", "Plates"));

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(tabs, 1);
    layout->addWidget(buttons);

    inspect();
}

// ── What is in here ───────────────────────────────────────────────────────

QWidget *ColourDialog::buildOverview()
{
    auto *page = new QWidget(this);

    m_report = new QPlainTextEdit(page);
    m_report->setReadOnly(true);

    auto *again = new QPushButton(QIcon::fromTheme(u"view-refresh"_s), i18nc("@action:button", "Read Again"), page);
    auto *ink = new QPushButton(QIcon::fromTheme(u"format-fill-color"_s),
                                i18nc("@action:button", "Measure the Ink Coverage"), page);
    ink->setToolTip(i18nc("@info:tooltip",
                          "Renders every page to a CMYK raster through Ghostscript and reports the highest "
                          "C+M+Y+K found. Slow on a long document."));

    auto *row = new QHBoxLayout;
    row->addWidget(again);
    row->addWidget(ink);
    row->addStretch(1);

    auto *layout = new QVBoxLayout(page);
    layout->addWidget(m_report, 1);
    layout->addLayout(row);

    connect(again, &QPushButton::clicked, this, [this] { inspect(); });
    connect(ink, &QPushButton::clicked, this, [this] { measureInk(); });
    return page;
}

void ColourDialog::inspect()
{
    QApplication::setOverrideCursor(Qt::WaitCursor);
    QString error;
    m_inventory = ColourTools::inspect(m_pdf, &error);
    QApplication::restoreOverrideCursor();

    if (!error.isEmpty()) {
        m_report->setPlainText(error);
        return;
    }

    QStringList lines;
    lines += i18n("Read %1 pages.", QLocale().toString(m_inventory.pages));
    lines += QString();
    lines += i18n("Colour spaces used: %1",
                  m_inventory.spaces.isEmpty() ? i18nc("@item nothing found", "none") : m_inventory.spaces.join(u", "_s));
    lines += i18n("Pages with RGB: %1 · pages with CMYK: %2", m_inventory.pagesWithRgb, m_inventory.pagesWithCmyk);

    if (m_inventory.spotColours.isEmpty()) {
        lines += i18n("No spot colours.");
    } else {
        lines += i18np("One spot colour, which is its own plate on press: %2",
                       "%1 spot colours, each its own plate on press: %2", m_inventory.spotColours.size(),
                       m_inventory.spotColours.join(u", "_s));
    }

    lines += QString();
    lines += i18n("ICC profile in the file: %1", m_inventory.hasIccProfile ? i18n("yes") : i18n("no"));
    lines += i18n("Output intent: %1", m_inventory.hasOutputIntent ? i18n("yes") : i18n("no"));
    lines += i18n("Transparency: %1", m_inventory.usesTransparency ? i18n("yes") : i18n("no"));
    lines += i18n("Overprint set anywhere: %1", m_inventory.usesOverprint ? i18n("yes") : i18n("no"));

    lines += QString();
    if (m_inventory.strokes == 0) {
        lines += i18n("The document strokes nothing, so there are no hairlines to worry about.");
    } else {
        lines += i18np("One stroke seen.", "%1 strokes seen.", m_inventory.strokes);
        lines += m_inventory.hairlines == 0
            ? i18n("None of them is a hairline. The thinnest is %1 pt.",
                   QLocale().toString(m_inventory.thinnestStroke, 'f', 3))
            : i18np("One of them is thinner than a quarter point and may vanish in print. The thinnest is %2 pt.",
                    "%1 of them are thinner than a quarter point and may vanish in print. The thinnest is %2 pt.",
                    m_inventory.hairlines, QLocale().toString(m_inventory.thinnestStroke, 'f', 3));
    }

    if (!m_inventory.notes.isEmpty()) {
        lines += QString();
        lines += i18n("What this could not deal with in place:");
        for (const QString &note : std::as_const(m_inventory.notes)) {
            lines += u"  • "_s + note;
        }
    }

    lines += QString();
    lines += i18n("What this cannot promise:");
    for (const QString &limit : ColourTools::limitations()) {
        lines += u"  • "_s + limit;
    }

    m_report->setPlainText(lines.join(u'\n'));

    // The ink list follows what the document actually has, so it is filled from
    // here rather than guessed at when the tab is built.
    m_inks->setRowCount(m_inventory.spotColours.size());
    for (int i = 0; i < m_inventory.spotColours.size(); ++i) {
        auto *name = new QTableWidgetItem(m_inventory.spotColours.at(i));
        name->setFlags(name->flags() & ~Qt::ItemIsEditable);
        m_inks->setItem(i, 0, name);
        m_inks->setItem(i, 1, new QTableWidgetItem(QString()));

        auto *dissolve = new QTableWidgetItem();
        dissolve->setFlags((dissolve->flags() | Qt::ItemIsUserCheckable) & ~Qt::ItemIsEditable);
        dissolve->setCheckState(Qt::Unchecked);
        m_inks->setItem(i, 2, dissolve);
    }

    const QStringList inks = QStringList { i18nc("@item process ink", "Cyan"), i18nc("@item process ink", "Magenta"),
                                           i18nc("@item process ink", "Yellow"), i18nc("@item process ink", "Black") }
        + m_inventory.spotColours;
    const QString wasSelected = m_plateInk->currentText();
    m_plateInk->clear();
    m_plateInk->addItems(inks);
    if (inks.contains(wasSelected)) {
        m_plateInk->setCurrentText(wasSelected);
    }
    m_platePage->setMaximum(qMax(1, m_document ? m_document->pageCount() : 1));
}

void ColourDialog::measureInk()
{
    QApplication::setOverrideCursor(Qt::WaitCursor);
    QString error;
    const QVector<double> coverage = ColourTools::inkCoverage(m_pdf, &error);
    QApplication::restoreOverrideCursor();

    if (coverage.isEmpty()) {
        KMessageBox::error(this, error.isEmpty() ? i18n("The ink coverage could not be measured.") : error,
                           windowTitle());
        return;
    }

    QStringList lines { QString(), i18n("Highest total ink per page, as a percentage. Over 300 is trouble:") };
    for (int i = 0; i < coverage.size(); ++i) {
        const QString mark = coverage.at(i) > 300.0 ? u"  ← "_s + i18n("too much") : QString();
        lines += i18nc("@item ink coverage of one page", "  Page %1: %2 %%3", QLocale().toString(i + 1),
                       QLocale().toString(coverage.at(i), 'f', 1), mark);
    }
    m_report->setPlainText(m_report->toPlainText() + u'\n' + lines.join(u'\n'));
}

// ── Convert ───────────────────────────────────────────────────────────────

QWidget *ColourDialog::buildConvert()
{
    auto *page = new QWidget(this);

    m_target = new QComboBox(page);
    for (ColourTools::Target target : { ColourTools::Target::Grayscale, ColourTools::Target::BlackWhite,
                                        ColourTools::Target::Rgb, ColourTools::Target::Cmyk }) {
        m_target->addItem(describeTarget(target), static_cast<int>(target));
    }

    m_pages = new QLineEdit(page);
    m_pages->setPlaceholderText(i18n("All pages. Or: 1-4, 8, 12-"));

    m_profile = new QLineEdit(page);
    m_profile->setPlaceholderText(i18n("Left empty, a profile on this system is looked for"));
    auto *browse = new QPushButton(i18nc("@action:button", "Choose…"), page);

    auto *profileRow = new QHBoxLayout;
    profileRow->addWidget(m_profile, 1);
    profileRow->addWidget(browse);

    m_threshold = new QDoubleSpinBox(page);
    m_threshold->setRange(0.0, 1.0);
    m_threshold->setSingleStep(0.05);
    m_threshold->setValue(0.5);
    m_threshold->setToolTip(i18nc("@info:tooltip", "Above this brightness a colour becomes white, below it black."));

    m_recompress = new QCheckBox(i18nc("@option:check", "Re-encode converted photographs as JPEG"), page);
    m_recompress->setChecked(true);
    m_recompress->setToolTip(i18nc("@info:tooltip",
                                   "A greyscaled 300 dpi scan stored raw is about eight times the size of the "
                                   "JPEG it came from."));

    m_quality = new QSpinBox(page);
    m_quality->setRange(40, 100);
    m_quality->setValue(92);
    m_quality->setSuffix(i18nc("@item percent suffix in a spin box", " %"));

    auto *form = new QFormLayout;
    form->addRow(i18nc("@label:listbox", "Into:"), m_target);
    form->addRow(i18nc("@label:textbox", "Pages:"), m_pages);
    form->addRow(i18nc("@label:textbox", "ICC profile:"), profileRow);
    form->addRow(i18nc("@label:spinbox", "Black-and-white threshold:"), m_threshold);
    form->addRow(QString(), m_recompress);
    form->addRow(i18nc("@label:spinbox", "JPEG quality:"), m_quality);

    auto *note = new QLabel(page);
    note->setWordWrap(true);

    auto *run = new QPushButton(QIcon::fromTheme(u"color-management"_s), i18nc("@action:button", "Convert"), page);

    auto *layout = new QVBoxLayout(page);
    layout->addLayout(form);
    layout->addWidget(note);
    layout->addStretch(1);
    layout->addWidget(run);

    const auto followTarget = [this, note] {
        const auto target = static_cast<ColourTools::Target>(m_target->currentData().toInt());
        const bool cmyk = target == ColourTools::Target::Cmyk;
        m_threshold->setEnabled(target == ColourTools::Target::BlackWhite);
        m_profile->setEnabled(cmyk);

        if (!cmyk) {
            note->setText(i18n("Text and vector colour is rewritten operator by operator, which is exact: every "
                               "byte that is not a colour comes out untouched. Only pictures have their pixels "
                               "converted."));
        } else if (ColourTools::hasColourManagement()) {
            note->setText(i18n("This build converts into CMYK itself, through the profile, in place. The page "
                               "selection is honoured and the text is not rasterised."));
        } else {
            note->setText(i18n("This build has no colour management, so Ghostscript converts the whole document. "
                               "The page selection cannot be honoured on that route, and the result is a rewritten "
                               "file rather than an edited one."));
        }
    };
    connect(m_target, &QComboBox::currentIndexChanged, this, followTarget);
    followTarget();

    connect(browse, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getOpenFileName(this, i18nc("@title:window", "ICC Profile"), QString(),
                                                          i18n("ICC profiles (*.icc *.icm);;All files (*)"));
        if (!path.isEmpty()) {
            m_profile->setText(path);
        }
    });
    connect(run, &QPushButton::clicked, this, [this] { runConvert(); });
    return page;
}

QVector<int> ColourDialog::chosenPages()
{
    const QString text = m_pages->text().trimmed();
    if (text.isEmpty() || !m_document) {
        return {};
    }
    QString error;
    const QVector<int> pages = PageRange::parse(text, m_document->pageCount(), &error);
    if (!error.isEmpty()) {
        KMessageBox::error(this, error, windowTitle());
        return { -1 }; // a marker the caller checks; never a valid page
    }
    return pages;
}

void ColourDialog::runConvert()
{
    const QVector<int> pages = chosenPages();
    if (pages.size() == 1 && pages.first() == -1) {
        return;
    }

    ColourTools::ConvertOptions options;
    options.target = static_cast<ColourTools::Target>(m_target->currentData().toInt());
    options.pages = pages;
    options.iccProfilePath = m_profile->text().trimmed();
    options.threshold = m_threshold->value();
    options.recompressPhotographs = m_recompress->isChecked();
    options.jpegQuality = m_quality->value();

    runProducing(m_document, this, windowTitle(), u"-colour.pdf"_s,
                 [this, options](const QString &out, QStringList *summary, QString *error) {
                     QStringList changes;
                     QApplication::setOverrideCursor(Qt::WaitCursor);
                     const bool ok = ColourTools::convert(m_pdf, out, options, &changes, error);
                     QApplication::restoreOverrideCursor();
                     if (!ok) {
                         return false;
                     }
                     *summary += changes.isEmpty() ? i18n("Nothing needed converting.") : changes.join(u'\n');
                     return true;
                 });
}

// ── Replace a colour ──────────────────────────────────────────────────────

QWidget *ColourDialog::buildReplace()
{
    auto *page = new QWidget(this);

    m_from = new ColourButton(QColor(200, 30, 40), page);
    m_to = new ColourButton(QColor(0, 0, 0), page);

    m_tolerance = new QDoubleSpinBox(page);
    m_tolerance->setRange(0.0, 1.0);
    m_tolerance->setDecimals(3);
    m_tolerance->setSingleStep(0.01);
    m_tolerance->setValue(0.02);
    m_tolerance->setToolTip(i18nc("@info:tooltip",
                                  "A distance in unit RGB. Zero matches only that exact colour; a few hundredths "
                                  "forgives the rounding a design tool leaves behind."));

    auto *form = new QFormLayout;
    form->addRow(i18nc("@label:chooser", "Replace:"), m_from);
    form->addRow(i18nc("@label:chooser", "With:"), m_to);
    form->addRow(i18nc("@label:spinbox", "Tolerance:"), m_tolerance);

    auto *note = new QLabel(i18n("Written back as RGB, or as grey when both colours are greys. Putting an arbitrary "
                                 "colour back into CMYK or into a spot ink would need a profile this has not been "
                                 "given, so it does not pretend to."),
                            page);
    note->setWordWrap(true);

    auto *run = new QPushButton(QIcon::fromTheme(u"color-picker"_s), i18nc("@action:button", "Replace"), page);

    auto *layout = new QVBoxLayout(page);
    layout->addLayout(form);
    layout->addWidget(note);
    layout->addStretch(1);
    layout->addWidget(run);

    connect(run, &QPushButton::clicked, this, [this] { runReplace(); });
    return page;
}

void ColourDialog::runReplace()
{
    const QColor from = m_from->colour();
    const QColor to = m_to->colour();
    const double tolerance = m_tolerance->value();

    runProducing(m_document, this, windowTitle(), u"-recoloured.pdf"_s,
                 [this, from, to, tolerance](const QString &out, QStringList *summary, QString *error) {
                     int replaced = 0;
                     QApplication::setOverrideCursor(Qt::WaitCursor);
                     const bool ok = ColourTools::replaceColour(m_pdf, out, from, to, tolerance, &replaced, error);
                     QApplication::restoreOverrideCursor();
                     if (!ok) {
                         return false;
                     }
                     *summary += replaced == 0
                         ? i18n("That colour is not used anywhere in the document.")
                         : i18np("Replaced one colour.", "Replaced %1 colours.", replaced);
                     return true;
                 });
}

// ── Spot colours ──────────────────────────────────────────────────────────

QWidget *ColourDialog::buildSeparations()
{
    auto *page = new QWidget(this);

    m_inks = new QTableWidget(0, 3, page);
    m_inks->setHorizontalHeaderLabels({ i18nc("@title:column", "Ink"), i18nc("@title:column", "Rename to"),
                                        i18nc("@title:column", "Dissolve into process") });
    m_inks->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_inks->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_inks->verticalHeader()->hide();

    auto *note = new QLabel(i18n("A printer plates by name, so pointing two inks at the same new name is how they are "
                                 "merged onto one plate. Dissolving uses the tint transform the document already "
                                 "carries, so it needs no profile and is exact arithmetic."),
                            page);
    note->setWordWrap(true);

    auto *run = new QPushButton(QIcon::fromTheme(u"format-fill-color"_s), i18nc("@action:button", "Apply to the Inks"),
                                page);

    auto *layout = new QVBoxLayout(page);
    layout->addWidget(m_inks, 1);
    layout->addWidget(note);
    layout->addWidget(run);

    connect(run, &QPushButton::clicked, this, [this] { runSeparations(); });
    return page;
}

void ColourDialog::runSeparations()
{
    QHash<QString, QString> renames;
    QStringList toProcess;
    for (int row = 0; row < m_inks->rowCount(); ++row) {
        const QString name = m_inks->item(row, 0)->text();
        const QString newName = m_inks->item(row, 1)->text().trimmed();
        if (!newName.isEmpty() && newName != name) {
            renames.insert(name, newName);
        }
        if (m_inks->item(row, 2)->checkState() == Qt::Checked) {
            toProcess.append(name);
        }
    }

    if (renames.isEmpty() && toProcess.isEmpty()) {
        KMessageBox::information(this, i18n("Nothing is being renamed or dissolved, so there is nothing to do."),
                                 windowTitle());
        return;
    }

    runProducing(m_document, this, windowTitle(), u"-inks.pdf"_s,
                 [this, renames, toProcess](const QString &out, QStringList *summary, QString *error) {
                     int changed = 0;
                     QApplication::setOverrideCursor(Qt::WaitCursor);
                     const bool ok = ColourTools::mapSpotColours(m_pdf, out, renames, toProcess, &changed, error);
                     QApplication::restoreOverrideCursor();
                     if (!ok) {
                         return false;
                     }
                     *summary += i18np("Changed one separation.", "Changed %1 separations.", changed);
                     return true;
                 });
}

// ── Prepress ──────────────────────────────────────────────────────────────

QWidget *ColourDialog::buildPrepress()
{
    auto *page = new QWidget(this);

    m_hairline = new QDoubleSpinBox(page);
    m_hairline->setRange(0.05, 3.0);
    m_hairline->setDecimals(2);
    m_hairline->setSingleStep(0.05);
    m_hairline->setValue(0.25);
    m_hairline->setSuffix(i18nc("@item point suffix in a spin box", " pt"));

    auto *hairlineBox = new QGroupBox(i18nc("@title:group", "Hairlines"), page);
    auto *hairlineLayout = new QVBoxLayout(hairlineBox);
    auto *hairlineNote = new QLabel(i18n("Strokes thinner than this are set to exactly this width, measured on the "
                                         "paper: a 0.5 pt line inside a tenth-scale transform is a twentieth of a "
                                         "point, whatever the operator says."),
                                    hairlineBox);
    hairlineNote->setWordWrap(true);
    auto *hairlineForm = new QFormLayout;
    hairlineForm->addRow(i18nc("@label:spinbox", "Thinnest allowed:"), m_hairline);
    auto *fixHairlines = new QPushButton(i18nc("@action:button", "Thicken the Hairlines"), hairlineBox);
    hairlineLayout->addLayout(hairlineForm);
    hairlineLayout->addWidget(hairlineNote);
    hairlineLayout->addWidget(fixHairlines);

    auto *overprintBox = new QGroupBox(i18nc("@title:group", "Black overprint"), page);
    auto *overprintLayout = new QVBoxLayout(overprintBox);
    m_overprintOn = new QRadioButton(i18nc("@option:radio", "Black overprints"), overprintBox);
    auto *off = new QRadioButton(i18nc("@option:radio", "Black knocks out"), overprintBox);
    m_overprintOn->setChecked(true);
    auto *overprintNote = new QLabel(i18n("Set wherever the page selects black, and nowhere else, so nothing but "
                                          "black gains or loses overprint by accident."),
                                     overprintBox);
    overprintNote->setWordWrap(true);
    auto *applyOverprint = new QPushButton(i18nc("@action:button", "Set the Overprint"), overprintBox);
    overprintLayout->addWidget(m_overprintOn);
    overprintLayout->addWidget(off);
    overprintLayout->addWidget(overprintNote);
    overprintLayout->addWidget(applyOverprint);

    auto *layout = new QVBoxLayout(page);
    layout->addWidget(hairlineBox);
    layout->addWidget(overprintBox);
    layout->addStretch(1);

    connect(fixHairlines, &QPushButton::clicked, this, [this] { runHairlines(); });
    connect(applyOverprint, &QPushButton::clicked, this, [this] { runOverprint(); });
    return page;
}

void ColourDialog::runHairlines()
{
    const double minimum = m_hairline->value();
    runProducing(m_document, this, windowTitle(), u"-hairlines.pdf"_s,
                 [this, minimum](const QString &out, QStringList *summary, QString *error) {
                     int fixed = 0;
                     QApplication::setOverrideCursor(Qt::WaitCursor);
                     const bool ok = ColourTools::fixHairlines(m_pdf, out, minimum, &fixed, error);
                     QApplication::restoreOverrideCursor();
                     if (!ok) {
                         return false;
                     }
                     *summary += fixed == 0
                         ? i18n("No stroke was thinner than that.")
                         : i18np("Thickened one stroke.", "Thickened %1 strokes.", fixed);
                     return true;
                 });
}

void ColourDialog::runOverprint()
{
    const bool on = m_overprintOn->isChecked();
    runProducing(m_document, this, windowTitle(), u"-overprint.pdf"_s,
                 [this, on](const QString &out, QStringList *summary, QString *error) {
                     int changed = 0;
                     QApplication::setOverrideCursor(Qt::WaitCursor);
                     const bool ok = ColourTools::setBlackOverprint(m_pdf, out, on, &changed, error);
                     QApplication::restoreOverrideCursor();
                     if (!ok) {
                         return false;
                     }
                     *summary += changed == 0
                         ? i18n("The document selects black nowhere, so nothing changed.")
                         : (on ? i18np("Black now overprints in one place.", "Black now overprints in %1 places.",
                                       changed)
                               : i18np("Black now knocks out in one place.", "Black now knocks out in %1 places.",
                                       changed));
                     return true;
                 });
}

// ── Plates ────────────────────────────────────────────────────────────────

QWidget *ColourDialog::buildPlates()
{
    auto *page = new QWidget(this);

    m_platePage = new QSpinBox(page);
    m_platePage->setRange(1, qMax(1, m_document ? m_document->pageCount() : 1));

    m_plateInk = new QComboBox(page);

    m_plateDpi = new QSpinBox(page);
    m_plateDpi->setRange(36, 300);
    m_plateDpi->setValue(96);
    m_plateDpi->setSuffix(i18nc("@item dots-per-inch suffix in a spin box", " dpi"));

    auto *show = new QPushButton(QIcon::fromTheme(u"view-preview"_s), i18nc("@action:button", "Show the Plate"), page);

    auto *row = new QHBoxLayout;
    row->addWidget(new QLabel(i18nc("@label:spinbox", "Page:"), page));
    row->addWidget(m_platePage);
    row->addWidget(new QLabel(i18nc("@label:listbox", "Ink:"), page));
    row->addWidget(m_plateInk, 1);
    row->addWidget(new QLabel(i18nc("@label:spinbox", "Resolution:"), page));
    row->addWidget(m_plateDpi);
    row->addWidget(show);

    m_plate = new QLabel(page);
    m_plate->setAlignment(Qt::AlignCenter);
    m_plate->setText(i18n("An ink density map: black is full ink, white is bare paper. It looks like the plate "
                          "rather than like the page, which is the point."));
    m_plate->setWordWrap(true);

    auto *scroll = new QScrollArea(page);
    scroll->setWidget(m_plate);
    scroll->setWidgetResizable(true);

    auto *layout = new QVBoxLayout(page);
    layout->addLayout(row);
    layout->addWidget(scroll, 1);

    connect(show, &QPushButton::clicked, this, [this] { showPlate(); });
    return page;
}

void ColourDialog::showPlate()
{
    if (m_plateInk->currentText().isEmpty()) {
        return;
    }

    QApplication::setOverrideCursor(Qt::WaitCursor);
    QString error;
    const QImage plate = ColourTools::separation(m_pdf, m_platePage->value() - 1, m_plateInk->currentText(),
                                                 m_plateDpi->value(), &error);
    QApplication::restoreOverrideCursor();

    if (plate.isNull()) {
        KMessageBox::error(this, error.isEmpty() ? i18n("That plate could not be rendered.") : error, windowTitle());
        return;
    }
    m_plate->setPixmap(QPixmap::fromImage(plate));
    m_plate->resize(plate.size());
}

} // namespace

void showColour(Document *document, QWidget *parent)
{
    const QString pdf = savedPath(document, parent);
    if (pdf.isEmpty()) {
        return;
    }
    ColourDialog(document, pdf, parent).exec();
}

} // namespace ps::tools
