/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "ToolDialogs.h"

#include "ToolSupport.h"
#include "core/Document.h"
#include "core/PageLayout.h"
#include "core/PageRange.h"

#include <KLocalizedString>
#include <KMessageBox>

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>

using namespace Qt::Literals::StringLiterals;

namespace ps::tools {

namespace {

constexpr double kPointsPerMillimetre = 72.0 / 25.4;

double toMillimetres(double points)
{
    return points / kPointsPerMillimetre;
}

double toPoints(double millimetres)
{
    return millimetres * kPointsPerMillimetre;
}

/** A length in points, written the way this dialog writes every length. */
QString inMillimetres(double points)
{
    return QLocale().toString(toMillimetres(points), 'f', 1);
}

struct Paper {
    const char *name;
    double widthMm;
    double heightMm;
};

// Held in millimetres and converted, so A4 comes out 595.276 × 841.89 rather
// than the rounded 595 × 842 that leaves a hairline of the old page showing.
constexpr Paper kPapers[] = {
    { "A0", 841.0, 1189.0 },        { "A1", 594.0, 841.0 },     { "A2", 420.0, 594.0 },    { "A3", 297.0, 420.0 },
    { "A4", 210.0, 297.0 },         { "A5", 148.0, 210.0 },     { "A6", 105.0, 148.0 },    { "B4", 250.0, 353.0 },
    { "B5", 176.0, 250.0 },         { "Letter", 215.9, 279.4 }, { "Legal", 215.9, 355.6 }, { "Tabloid", 279.4, 431.8 },
    { "Executive", 184.15, 266.7 },
};

/** The five boxes, named the way the file names them. */
struct BoxField {
    const char *pdfName;
    QRectF PageLayout::Boxes::*member;
};

constexpr BoxField kBoxFields[] = {
    { "/MediaBox", &PageLayout::Boxes::media }, { "/CropBox", &PageLayout::Boxes::crop },
    { "/BleedBox", &PageLayout::Boxes::bleed }, { "/TrimBox", &PageLayout::Boxes::trim },
    { "/ArtBox", &PageLayout::Boxes::art },
};

constexpr int kBoxCount = 5;

/** A length in millimetres, which is the only unit this dialog shows. */
QDoubleSpinBox *millimetreBox(QWidget *parent, double minimum, double maximum, double value)
{
    auto *box = new QDoubleSpinBox(parent);
    box->setDecimals(1);
    box->setRange(minimum, maximum);
    box->setSingleStep(1.0);
    box->setValue(value);
    box->setSuffix(i18nc("@item millimetre suffix in a spin box", " mm"));
    return box;
}

QString describeFit(PageLayout::Fit fit)
{
    switch (fit) {
    case PageLayout::Fit::Scale:
        return i18nc("@item:inlistbox how content lands on a new sheet", "Scale the content to the new sheet");
    case PageLayout::Fit::Centre:
        return i18nc("@item:inlistbox how content lands on a new sheet", "Keep the content's size and centre it");
    case PageLayout::Fit::ScaleAndCentre:
        return i18nc("@item:inlistbox how content lands on a new sheet", "Shrink what is too big, then centre");
    }
    return {};
}

QComboBox *fitChooser(QWidget *parent)
{
    auto *box = new QComboBox(parent);
    for (PageLayout::Fit fit : { PageLayout::Fit::Scale, PageLayout::Fit::Centre, PageLayout::Fit::ScaleAndCentre }) {
        box->addItem(describeFit(fit), static_cast<int>(fit));
    }
    return box;
}

QString describeStyle(PageLayout::Labels::Style style)
{
    switch (style) {
    case PageLayout::Labels::Style::Decimal:
        return i18nc("@item:inlistbox a numbering style, shown by example", "1, 2, 3");
    case PageLayout::Labels::Style::RomanUpper:
        return i18nc("@item:inlistbox a numbering style, shown by example", "I, II, III");
    case PageLayout::Labels::Style::RomanLower:
        return i18nc("@item:inlistbox a numbering style, shown by example", "i, ii, iii");
    case PageLayout::Labels::Style::LetterUpper:
        return i18nc("@item:inlistbox a numbering style, shown by example", "A, B, C");
    case PageLayout::Labels::Style::LetterLower:
        return i18nc("@item:inlistbox a numbering style, shown by example", "a, b, c");
    case PageLayout::Labels::Style::None:
        return i18nc("@item:inlistbox a numbering style", "No number, the prefix alone");
    }
    return {};
}

QString describeBox(const QRectF &rect)
{
    if (!rect.isValid()) {
        return i18nc("@info a page box this page does not have at all", "absent");
    }
    const QString size = i18nc("@info the size of a page box", "%1 × %2 mm", inMillimetres(rect.width()),
                               inMillimetres(rect.height()));
    if (qFuzzyIsNull(rect.x()) && qFuzzyIsNull(rect.y())) {
        return size;
    }
    // The corner is only worth the room it takes when it is not the origin,
    // which is exactly the case where a box has been moved and somebody wants
    // to know by how much.
    return i18nc("@info a page box: its size, then its bottom-left corner", "%1 at %2, %3", size,
                 inMillimetres(rect.x()), inMillimetres(rect.y()));
}

/**
 * A paper size: one of the names everybody knows, or a width and a height.
 *
 * Millimetres in the boxes and points out of sizePoints(), because the engine
 * works in points and nobody measures paper in them.
 */
class PaperChooser : public QWidget
{
public:
    PaperChooser(bool offerLargest, QWidget *parent)
        : QWidget(parent)
    {
        m_named = new QComboBox(this);
        if (offerLargest) {
            m_named->addItem(i18nc("@item:inlistbox paper size", "The largest page in the document"), kLargest);
        }

        int defaultIndex = 0;
        int paperIndex = 0;
        for (const Paper &paper : kPapers) {
            if (QLatin1String(paper.name) == QLatin1String("A4")) {
                defaultIndex = m_named->count();
            }
            m_named->addItem(i18nc("@item:inlistbox a paper size and its measurements", "%1 (%2 × %3 mm)",
                                   QString::fromLatin1(paper.name), QLocale().toString(paper.widthMm, 'g', 5),
                                   QLocale().toString(paper.heightMm, 'g', 5)),
                             paperIndex);
            ++paperIndex;
        }
        m_named->addItem(i18nc("@item:inlistbox paper size", "Something else"), kCustom);

        m_width = millimetreBox(this, 1.0, 20000.0, 210.0);
        m_height = millimetreBox(this, 1.0, 20000.0, 297.0);

        auto *row = new QHBoxLayout(this);
        row->setContentsMargins(0, 0, 0, 0);
        row->addWidget(m_named, 1);
        row->addWidget(m_width);
        row->addWidget(new QLabel(u"×"_s, this));
        row->addWidget(m_height);

        connect(m_named, &QComboBox::currentIndexChanged, this, [this] { follow(); });
        m_named->setCurrentIndex(defaultIndex);
        follow();
    }

    /** The chosen size, or an empty size for "the largest page in the document". */
    QSizeF sizePoints() const
    {
        if (m_named->currentData().toInt() == kLargest) {
            return {};
        }
        return QSizeF(toPoints(m_width->value()), toPoints(m_height->value()));
    }

    QString describe() const
    {
        if (m_named->currentData().toInt() == kLargest) {
            return m_named->currentText();
        }
        return i18nc("@info a paper size", "%1 × %2 mm", QLocale().toString(m_width->value(), 'f', 1),
                     QLocale().toString(m_height->value(), 'f', 1));
    }

private:
    static constexpr int kLargest = -2;
    static constexpr int kCustom = -1;

    void follow()
    {
        const int chosen = m_named->currentData().toInt();
        const bool custom = chosen == kCustom;
        m_width->setEnabled(custom);
        m_height->setEnabled(custom);
        // A named size fills the boxes as well as disabling them, so that
        // switching to "something else" starts from the size just seen rather
        // than from whatever was left there.
        if (chosen >= 0) {
            m_width->setValue(kPapers[chosen].widthMm);
            m_height->setValue(kPapers[chosen].heightMm);
        }
    }

    QComboBox *m_named = nullptr;
    QDoubleSpinBox *m_width = nullptr;
    QDoubleSpinBox *m_height = nullptr;
};

/** The four spin boxes and the tick that make up one editable page box. */
struct BoxEditor {
    QCheckBox *use = nullptr;
    QDoubleSpinBox *x = nullptr;
    QDoubleSpinBox *y = nullptr;
    QDoubleSpinBox *width = nullptr;
    QDoubleSpinBox *height = nullptr;
};

/**
 * Everything about where the ink sits on the paper, in one window.
 *
 * The five boxes come first and stay first, because every other tab here is in
 * the end a way of moving them: a resize writes a /MediaBox, a bleed writes a
 * /TrimBox and a /BleedBox, marks grow the sheet. A user who can see all five
 * per page can tell whether the thing they just did was the thing they meant,
 * and a prepress problem is nearly always visible in that table before it is
 * visible anywhere else.
 *
 * Millimetres throughout the interface and points throughout the engine, with
 * the conversion at the edge of each call. Paper has been measured in
 * millimetres for a century and no one is going to start estimating in
 * seventy-seconds of an inch for our convenience.
 */
class LayoutDialog : public QDialog
{
public:
    LayoutDialog(Document *document, const QString &pdf, QWidget *parent);

private:
    QWidget *buildBoxes();
    QWidget *buildSetBoxes();
    QWidget *buildPaper();
    QWidget *buildBleedAndMarks();
    QWidget *buildSplitAndTile();
    QWidget *buildImposition();
    QWidget *buildNumbering();
    QWidget *buildBlanks();

    void readBoxes();
    void fillEditors(int page);
    void addLabelRow(int fromPage, PageLayout::Labels::Style style, const QString &prefix, int startAt);

    void runSetBoxes();
    void runResize();
    void runUnify();
    void runBleed();
    void runMarks();
    void runSplit();
    void runPoster();
    void runBooklet();
    void runOverlay();
    void runLabels();
    void showLabels();
    void findBlanks();
    void runRemove();
    void showLimitations();

    /** How many pages the file on disk has, which is what every tool here counts in. */
    int pageCount() const;

    /** Zero-based pages from @p box, or an empty list once the trouble has been reported. */
    QVector<int> chosenPages(QLineEdit *box);

    Document *m_document = nullptr;
    QString m_pdf;
    QVector<PageLayout::Boxes> m_boxes;

    QTableWidget *m_boxTable = nullptr;
    QLabel *m_boxSummary = nullptr;

    QLineEdit *m_boxPages = nullptr;
    QSpinBox *m_fillFrom = nullptr;
    BoxEditor m_editors[kBoxCount];

    PaperChooser *m_resizePaper = nullptr;
    QComboBox *m_resizeFit = nullptr;
    QLineEdit *m_resizePages = nullptr;
    PaperChooser *m_unifyPaper = nullptr;
    QComboBox *m_unifyFit = nullptr;

    QDoubleSpinBox *m_bleed = nullptr;
    QLineEdit *m_bleedPages = nullptr;
    QCheckBox *m_cropMarks = nullptr;
    QCheckBox *m_registrationMarks = nullptr;
    QCheckBox *m_colourBars = nullptr;
    QCheckBox *m_pageInformation = nullptr;
    QDoubleSpinBox *m_markOffset = nullptr;
    QDoubleSpinBox *m_markLength = nullptr;
    QDoubleSpinBox *m_markLineWidth = nullptr;

    QRadioButton *m_cutVertically = nullptr;
    QDoubleSpinBox *m_cutAt = nullptr;
    QLineEdit *m_splitPages = nullptr;
    QSpinBox *m_posterPage = nullptr;
    PaperChooser *m_posterSheet = nullptr;
    QDoubleSpinBox *m_posterOverlap = nullptr;
    QCheckBox *m_posterMarks = nullptr;

    QRadioButton *m_toPrinterSpreads = nullptr;
    QLineEdit *m_overlayFile = nullptr;
    QCheckBox *m_repeatOverlay = nullptr;

    QTableWidget *m_labels = nullptr;

    QDoubleSpinBox *m_inkThreshold = nullptr;
    QLabel *m_blankResult = nullptr;
    QLineEdit *m_removePages = nullptr;
};

LayoutDialog::LayoutDialog(Document *document, const QString &pdf, QWidget *parent)
    : QDialog(parent)
    , m_document(document)
    , m_pdf(pdf)
{
    setWindowTitle(i18nc("@title:window", "Page Layout"));
    resize(1020, 780);

    auto *tabs = new QTabWidget(this);
    tabs->addTab(buildBoxes(), i18nc("@title:tab", "The Five Boxes"));
    tabs->addTab(buildSetBoxes(), i18nc("@title:tab", "Set the Boxes"));
    tabs->addTab(buildPaper(), i18nc("@title:tab", "Paper Size"));
    tabs->addTab(buildBleedAndMarks(), i18nc("@title:tab", "Bleed and Marks"));
    tabs->addTab(buildSplitAndTile(), i18nc("@title:tab", "Cut and Tile"));
    tabs->addTab(buildImposition(), i18nc("@title:tab", "Booklet and Overlay"));
    tabs->addTab(buildNumbering(), i18nc("@title:tab", "Numbering"));
    tabs->addTab(buildBlanks(), i18nc("@title:tab", "Blank Pages"));

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(tabs, 1);
    layout->addWidget(buttons);

    readBoxes();
    addLabelRow(1, PageLayout::Labels::Style::Decimal, QString(), 1);
}

int LayoutDialog::pageCount() const
{
    if (!m_boxes.isEmpty()) {
        return static_cast<int>(m_boxes.size());
    }
    return m_document ? m_document->pageCount() : 0;
}

QVector<int> LayoutDialog::chosenPages(QLineEdit *box)
{
    QString error;
    const QVector<int> pages = PageRange::parse(box->text().trimmed(), pageCount(), &error);
    if (pages.isEmpty()) {
        KMessageBox::error(this, error.isEmpty() ? i18n("That does not name any page of this document.") : error,
                           windowTitle());
    }
    return pages;
}

// ── The five boxes ────────────────────────────────────────────────────────

QWidget *LayoutDialog::buildBoxes()
{
    auto *page = new QWidget(this);

    m_boxSummary = new QLabel(page);
    m_boxSummary->setWordWrap(true);

    m_boxTable = new QTableWidget(0, 1 + kBoxCount, page);
    QStringList headers { i18nc("@title:column", "Page") };
    for (const BoxField &field : kBoxFields) {
        headers += QString::fromLatin1(field.pdfName);
    }
    m_boxTable->setHorizontalHeaderLabels(headers);
    m_boxTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    for (int column = 1; column <= kBoxCount; ++column) {
        m_boxTable->horizontalHeader()->setSectionResizeMode(column, QHeaderView::Stretch);
    }
    m_boxTable->verticalHeader()->hide();
    m_boxTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_boxTable->setEditTriggers(QAbstractItemView::NoEditTriggers);

    auto *note = new QLabel(i18n("An absent box is shown as absent rather than filled in with the fallback the "
                                 "specification prescribes: “this page has no /TrimBox” and “this page's /TrimBox "
                                 "happens to equal its /CropBox” are different documents, and only one of them is "
                                 "ready for a press."),
                            page);
    note->setWordWrap(true);

    auto *again = new QPushButton(QIcon::fromTheme(u"view-refresh"_s), i18nc("@action:button", "Read Again"), page);
    auto *limits = new QPushButton(i18nc("@action:button", "What This Cannot Promise"), page);

    auto *row = new QHBoxLayout;
    row->addWidget(again);
    row->addWidget(limits);
    row->addStretch(1);

    auto *layout = new QVBoxLayout(page);
    layout->addWidget(m_boxSummary);
    layout->addWidget(m_boxTable, 1);
    layout->addWidget(note);
    layout->addLayout(row);

    connect(again, &QPushButton::clicked, this, [this] { readBoxes(); });
    connect(limits, &QPushButton::clicked, this, [this] { showLimitations(); });
    return page;
}

void LayoutDialog::readBoxes()
{
    QApplication::setOverrideCursor(Qt::WaitCursor);
    QString error;
    m_boxes = PageLayout::boxesOf(m_pdf, &error);
    QApplication::restoreOverrideCursor();

    if (m_boxes.isEmpty()) {
        m_boxSummary->setText(error.isEmpty() ? i18n("This document has no pages.") : error);
        m_boxTable->setRowCount(0);
        return;
    }

    m_boxTable->setRowCount(static_cast<int>(m_boxes.size()));
    int present[kBoxCount] = { 0, 0, 0, 0, 0 };

    for (int row = 0; row < m_boxes.size(); ++row) {
        auto *number = new QTableWidgetItem(QLocale().toString(row + 1));
        number->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        m_boxTable->setItem(row, 0, number);

        for (int field = 0; field < kBoxCount; ++field) {
            const QRectF rect = m_boxes.at(row).*kBoxFields[field].member;
            auto *cell = new QTableWidgetItem(describeBox(rect));
            if (rect.isValid()) {
                ++present[field];
                cell->setToolTip(i18nc("@info:tooltip the same box in points, which is what the file holds",
                                       "%1, %2 to %3, %4 pt", QLocale().toString(rect.x(), 'f', 2),
                                       QLocale().toString(rect.y(), 'f', 2),
                                       QLocale().toString(rect.x() + rect.width(), 'f', 2),
                                       QLocale().toString(rect.y() + rect.height(), 'f', 2)));
            } else {
                QFont font = cell->font();
                font.setItalic(true);
                cell->setFont(font);
            }
            m_boxTable->setItem(row, 1 + field, cell);
        }
    }

    m_boxSummary->setText(i18n("%1 pages. /CropBox on %2, /BleedBox on %3, /TrimBox on %4, /ArtBox on %5.",
                               QLocale().toString(static_cast<int>(m_boxes.size())), QLocale().toString(present[1]),
                               QLocale().toString(present[2]), QLocale().toString(present[3]),
                               QLocale().toString(present[4])));

    m_fillFrom->setMaximum(static_cast<int>(m_boxes.size()));
    m_posterPage->setMaximum(static_cast<int>(m_boxes.size()));
    fillEditors(m_fillFrom->value() - 1);
}

void LayoutDialog::showLimitations()
{
    QStringList lines { i18n("What page layout cannot promise:") };
    for (const QString &limitation : PageLayout::limitations()) {
        lines += u"  • "_s + limitation;
    }
    showReport(this, windowTitle(), lines);
}

// ── Set the boxes ─────────────────────────────────────────────────────────

QWidget *LayoutDialog::buildSetBoxes()
{
    auto *page = new QWidget(this);

    m_boxPages = new QLineEdit(page);
    m_boxPages->setPlaceholderText(i18n("All pages. Or: 1-4, 8, 12-"));

    m_fillFrom = new QSpinBox(page);
    m_fillFrom->setRange(1, qMax(1, pageCount()));
    auto *fill = new QPushButton(i18nc("@action:button", "Take the Values from This Page"), page);

    auto *fillRow = new QHBoxLayout;
    fillRow->addWidget(m_fillFrom);
    fillRow->addWidget(fill);
    fillRow->addStretch(1);

    auto *form = new QFormLayout;
    form->addRow(i18nc("@label:textbox", "Pages to change:"), m_boxPages);
    form->addRow(i18nc("@label:spinbox", "Start from page:"), fillRow);

    auto *grid = new QGridLayout;
    grid->addWidget(new QLabel(i18nc("@label:spinbox the left edge of a box", "Left"), page), 0, 1);
    grid->addWidget(new QLabel(i18nc("@label:spinbox the bottom edge of a box", "Bottom"), page), 0, 2);
    grid->addWidget(new QLabel(i18nc("@label:spinbox", "Width"), page), 0, 3);
    grid->addWidget(new QLabel(i18nc("@label:spinbox", "Height"), page), 0, 4);

    for (int field = 0; field < kBoxCount; ++field) {
        BoxEditor &editor = m_editors[field];
        editor.use = new QCheckBox(QString::fromLatin1(kBoxFields[field].pdfName), page);
        editor.x = millimetreBox(page, -20000.0, 20000.0, 0.0);
        editor.y = millimetreBox(page, -20000.0, 20000.0, 0.0);
        editor.width = millimetreBox(page, 1.0, 20000.0, 210.0);
        editor.height = millimetreBox(page, 1.0, 20000.0, 297.0);

        grid->addWidget(editor.use, 1 + field, 0);
        grid->addWidget(editor.x, 1 + field, 1);
        grid->addWidget(editor.y, 1 + field, 2);
        grid->addWidget(editor.width, 1 + field, 3);
        grid->addWidget(editor.height, 1 + field, 4);

        const auto follow = [editor](bool on) {
            editor.x->setEnabled(on);
            editor.y->setEnabled(on);
            editor.width->setEnabled(on);
            editor.height->setEnabled(on);
        };
        connect(editor.use, &QCheckBox::toggled, this, follow);
        follow(false);
    }

    auto *note = new QLabel(i18n("The four optional boxes are written as one set, so a box left unticked is taken "
                                 "away from the pages that have it. /MediaBox is the exception: a page without one "
                                 "is not a page, so leaving it unticked leaves it alone.\n\n"
                                 "Boxes have to nest. An arrangement where they do not is refused with the offender "
                                 "named rather than written, because an imposition program handed a /TrimBox outside "
                                 "its /MediaBox does something nobody can predict."),
                            page);
    note->setWordWrap(true);

    auto *run
        = new QPushButton(QIcon::fromTheme(u"transform-crop"_s), i18nc("@action:button", "Write the Boxes"), page);

    auto *layout = new QVBoxLayout(page);
    layout->addLayout(form);
    layout->addLayout(grid);
    layout->addWidget(note);
    layout->addStretch(1);
    layout->addWidget(run);

    connect(fill, &QPushButton::clicked, this, [this] { fillEditors(m_fillFrom->value() - 1); });
    connect(run, &QPushButton::clicked, this, [this] { runSetBoxes(); });
    return page;
}

void LayoutDialog::fillEditors(int page)
{
    if (page < 0 || page >= m_boxes.size()) {
        return;
    }
    for (int field = 0; field < kBoxCount; ++field) {
        const QRectF rect = m_boxes.at(page).*kBoxFields[field].member;
        BoxEditor &editor = m_editors[field];
        editor.use->setChecked(rect.isValid());
        if (!rect.isValid()) {
            continue;
        }
        editor.x->setValue(toMillimetres(rect.x()));
        editor.y->setValue(toMillimetres(rect.y()));
        editor.width->setValue(toMillimetres(rect.width()));
        editor.height->setValue(toMillimetres(rect.height()));
    }
}

void LayoutDialog::runSetBoxes()
{
    const QVector<int> pages = chosenPages(m_boxPages);
    if (pages.isEmpty()) {
        return;
    }

    PageLayout::Boxes boxes;
    QStringList given;
    QStringList dropped;
    for (int field = 0; field < kBoxCount; ++field) {
        const QString name = QString::fromLatin1(kBoxFields[field].pdfName);
        const BoxEditor &editor = m_editors[field];
        if (editor.use->isChecked()) {
            boxes.*kBoxFields[field].member = QRectF(toPoints(editor.x->value()), toPoints(editor.y->value()),
                                                     toPoints(editor.width->value()), toPoints(editor.height->value()));
            given += name;
            continue;
        }
        // Only a box some chosen page actually carries is worth warning about:
        // removing a /ArtBox from a document that has never had one is a change
        // to nothing, and saying so would train the user to click past the
        // warning that matters.
        const bool anywhere = std::any_of(pages.cbegin(), pages.cend(), [this, field](int page) {
            return page >= 0 && page < m_boxes.size() && (m_boxes.at(page).*kBoxFields[field].member).isValid();
        });
        if (anywhere && kBoxFields[field].member != &PageLayout::Boxes::media) {
            dropped += name;
        }
    }

    if (given.isEmpty() && dropped.isEmpty()) {
        KMessageBox::information(this,
                                 i18n("Nothing is ticked and none of these pages has an optional box, so there "
                                      "is nothing to write."),
                                 windowTitle());
        return;
    }

    if (!dropped.isEmpty()
        && KMessageBox::questionTwoActions(
               this,
               i18n("These pages have boxes that are not ticked, and writing the set takes them away: %1.",
                    dropped.join(u", "_s)),
               windowTitle(), KGuiItem(i18nc("@action:button", "Write the Boxes")),
               KGuiItem(i18nc("@action:button", "Go Back")))
            != KMessageBox::PrimaryAction) {
        return;
    }

    runProducing(m_document, this, windowTitle(), u"-boxes.pdf"_s,
                 [this, pages, boxes, given, dropped](const QString &out, QStringList *summary, QString *error) {
                     QApplication::setOverrideCursor(Qt::WaitCursor);
                     const bool ok = PageLayout::setBoxes(m_pdf, out, pages, boxes, error);
                     QApplication::restoreOverrideCursor();
                     if (!ok) {
                         return false;
                     }
                     *summary += i18ncp("@info", "Wrote the boxes of one page.", "Wrote the boxes of %1 pages.",
                                        pages.size());
                     if (!given.isEmpty()) {
                         *summary += i18n("Set: %1.", given.join(u", "_s));
                     }
                     if (!dropped.isEmpty()) {
                         *summary += i18n("Taken away: %1.", dropped.join(u", "_s));
                     }
                     return true;
                 });
}

// ── Paper size ────────────────────────────────────────────────────────────

QWidget *LayoutDialog::buildPaper()
{
    auto *page = new QWidget(this);

    auto *resizeBox = new QGroupBox(i18nc("@title:group", "Put the pages on a different size"), page);
    m_resizePaper = new PaperChooser(false, resizeBox);
    m_resizeFit = fitChooser(resizeBox);
    m_resizePages = new QLineEdit(resizeBox);
    m_resizePages->setPlaceholderText(i18n("All pages. Or: 1-4, 8, 12-"));

    auto *resizeForm = new QFormLayout;
    resizeForm->addRow(i18nc("@label:listbox", "New size:"), m_resizePaper);
    resizeForm->addRow(i18nc("@label:listbox", "The content:"), m_resizeFit);
    resizeForm->addRow(i18nc("@label:textbox", "Pages:"), m_resizePages);

    auto *resizeNote = new QLabel(i18n("The content stream is wrapped rather than rebuilt, so nothing is rasterised "
                                       "and nothing is re-encoded: a scan brought to A5 is the same scan at the same "
                                       "resolution. A sideways page asked for A4 gets a sideways A4."),
                                  resizeBox);
    resizeNote->setWordWrap(true);

    auto *resize
        = new QPushButton(QIcon::fromTheme(u"transform-scale"_s), i18nc("@action:button", "Resize"), resizeBox);

    auto *resizeLayout = new QVBoxLayout(resizeBox);
    resizeLayout->addLayout(resizeForm);
    resizeLayout->addWidget(resizeNote);
    resizeLayout->addWidget(resize);

    auto *unifyBox = new QGroupBox(i18nc("@title:group", "Bring every page to one size"), page);
    m_unifyPaper = new PaperChooser(true, unifyBox);
    m_unifyFit = fitChooser(unifyBox);

    auto *unifyForm = new QFormLayout;
    unifyForm->addRow(i18nc("@label:listbox", "One size:"), m_unifyPaper);
    unifyForm->addRow(i18nc("@label:listbox", "The content:"), m_unifyFit);

    auto *unifyNote = new QLabel(i18n("“The largest page” is measured by area, not by taking the widest width and "
                                      "the tallest height: a document of portrait and landscape A4 would otherwise "
                                      "land on a square sheet no paper tray has ever held. This works on the whole "
                                      "document, so there is no page selection."),
                                 unifyBox);
    unifyNote->setWordWrap(true);

    auto *unify = new QPushButton(QIcon::fromTheme(u"distribute-horizontal-equal"_s),
                                  i18nc("@action:button", "Unify the Sizes"), unifyBox);

    auto *unifyLayout = new QVBoxLayout(unifyBox);
    unifyLayout->addLayout(unifyForm);
    unifyLayout->addWidget(unifyNote);
    unifyLayout->addWidget(unify);

    auto *layout = new QVBoxLayout(page);
    layout->addWidget(resizeBox);
    layout->addWidget(unifyBox);
    layout->addStretch(1);

    connect(resize, &QPushButton::clicked, this, [this] { runResize(); });
    connect(unify, &QPushButton::clicked, this, [this] { runUnify(); });
    return page;
}

void LayoutDialog::runResize()
{
    const QVector<int> pages = chosenPages(m_resizePages);
    if (pages.isEmpty()) {
        return;
    }

    const QSizeF paper = m_resizePaper->sizePoints();
    const auto fit = static_cast<PageLayout::Fit>(m_resizeFit->currentData().toInt());
    const QString size = m_resizePaper->describe();

    runProducing(m_document, this, windowTitle(), u"-resized.pdf"_s,
                 [this, pages, paper, fit, size](const QString &out, QStringList *summary, QString *error) {
                     QApplication::setOverrideCursor(Qt::WaitCursor);
                     const bool ok = PageLayout::resize(m_pdf, out, pages, paper, fit, error);
                     QApplication::restoreOverrideCursor();
                     if (!ok) {
                         return false;
                     }
                     *summary += i18ncp("@info", "Put one page on %2.", "Put %1 pages on %2.", pages.size(), size);
                     *summary += describeFit(fit);
                     return true;
                 });
}

void LayoutDialog::runUnify()
{
    const QSizeF paper = m_unifyPaper->sizePoints();
    const auto fit = static_cast<PageLayout::Fit>(m_unifyFit->currentData().toInt());
    const QString size = m_unifyPaper->describe();

    runProducing(m_document, this, windowTitle(), u"-unified.pdf"_s,
                 [this, paper, fit, size](const QString &out, QStringList *summary, QString *error) {
                     QApplication::setOverrideCursor(Qt::WaitCursor);
                     const bool ok = PageLayout::unify(m_pdf, out, paper, fit, error);
                     QApplication::restoreOverrideCursor();
                     if (!ok) {
                         return false;
                     }
                     *summary += i18n("Brought every page to %1.", size);
                     *summary += describeFit(fit);
                     return true;
                 });
}

// ── Bleed and marks ───────────────────────────────────────────────────────

QWidget *LayoutDialog::buildBleedAndMarks()
{
    auto *page = new QWidget(this);

    auto *bleedBox = new QGroupBox(i18nc("@title:group", "Bleed"), page);
    m_bleed = millimetreBox(bleedBox, 0.0, 100.0, 3.0);
    m_bleed->setToolTip(i18nc("@info:tooltip", "Three millimetres is what most printers ask for."));
    m_bleedPages = new QLineEdit(bleedBox);
    m_bleedPages->setPlaceholderText(i18n("All pages. Or: 1-4, 8, 12-"));

    auto *bleedForm = new QFormLayout;
    bleedForm->addRow(i18nc("@label:spinbox", "Bleed:"), m_bleed);
    bleedForm->addRow(i18nc("@label:textbox", "Pages:"), m_bleedPages);

    auto *bleedNote = new QLabel(i18n("The /TrimBox becomes what the page shows now and the /BleedBox that grown on "
                                      "all four sides. The /MediaBox grows with it where it has to: bleed is ink "
                                      "that runs past the cut, and it needs paper to run onto."),
                                 bleedBox);
    bleedNote->setWordWrap(true);

    auto *bleed = new QPushButton(QIcon::fromTheme(u"select-rectangular"_s), i18nc("@action:button", "Set the Bleed"),
                                  bleedBox);

    auto *bleedLayout = new QVBoxLayout(bleedBox);
    bleedLayout->addLayout(bleedForm);
    bleedLayout->addWidget(bleedNote);
    bleedLayout->addWidget(bleed);

    auto *marksBox = new QGroupBox(i18nc("@title:group", "Printer's marks"), page);
    m_cropMarks = new QCheckBox(i18nc("@option:check a kind of printer's mark", "Crop marks"), marksBox);
    m_cropMarks->setChecked(true);
    m_registrationMarks
        = new QCheckBox(i18nc("@option:check a kind of printer's mark", "Registration marks"), marksBox);
    m_colourBars = new QCheckBox(i18nc("@option:check a kind of printer's mark", "Colour bars"), marksBox);
    m_pageInformation = new QCheckBox(i18nc("@option:check a kind of printer's mark", "Page information"), marksBox);

    m_markOffset = millimetreBox(marksBox, 0.0, 50.0, 3.0);
    m_markLength = millimetreBox(marksBox, 1.0, 50.0, 5.0);
    m_markLineWidth = new QDoubleSpinBox(marksBox);
    m_markLineWidth->setDecimals(2);
    m_markLineWidth->setRange(0.05, 3.0);
    m_markLineWidth->setSingleStep(0.05);
    m_markLineWidth->setValue(0.25);
    m_markLineWidth->setSuffix(i18nc("@item point suffix in a spin box", " pt"));

    auto *marksForm = new QFormLayout;
    marksForm->addRow(i18nc("@label:spinbox", "Outside the trim by:"), m_markOffset);
    marksForm->addRow(i18nc("@label:spinbox", "Mark length:"), m_markLength);
    marksForm->addRow(i18nc("@label:spinbox", "Line width:"), m_markLineWidth);

    auto *marksNote = new QLabel(i18n("Drawn in a separation that appears on every plate, since a registration mark "
                                      "that came out on the cyan plate alone would be worse than none. The sheet "
                                      "grows around the content where it stands, so page coordinates and "
                                      "annotations stay where they were. Every page is marked: marks have to line "
                                      "up sheet to sheet, so there is no page selection here."),
                                 marksBox);
    marksNote->setWordWrap(true);

    auto *marks
        = new QPushButton(QIcon::fromTheme(u"draw-cross"_s), i18nc("@action:button", "Add the Marks"), marksBox);

    auto *marksLayout = new QVBoxLayout(marksBox);
    marksLayout->addWidget(m_cropMarks);
    marksLayout->addWidget(m_registrationMarks);
    marksLayout->addWidget(m_colourBars);
    marksLayout->addWidget(m_pageInformation);
    marksLayout->addLayout(marksForm);
    marksLayout->addWidget(marksNote);
    marksLayout->addWidget(marks);

    auto *layout = new QVBoxLayout(page);
    layout->addWidget(bleedBox);
    layout->addWidget(marksBox);
    layout->addStretch(1);

    connect(bleed, &QPushButton::clicked, this, [this] { runBleed(); });
    connect(marks, &QPushButton::clicked, this, [this] { runMarks(); });
    return page;
}

void LayoutDialog::runBleed()
{
    const QVector<int> pages = chosenPages(m_bleedPages);
    if (pages.isEmpty()) {
        return;
    }

    const double bleed = toPoints(m_bleed->value());
    runProducing(m_document, this, windowTitle(), u"-bleed.pdf"_s,
                 [this, pages, bleed](const QString &out, QStringList *summary, QString *error) {
                     QApplication::setOverrideCursor(Qt::WaitCursor);
                     const bool ok = PageLayout::setBleed(m_pdf, out, pages, bleed, error);
                     QApplication::restoreOverrideCursor();
                     if (!ok) {
                         return false;
                     }
                     *summary += i18ncp("@info", "Gave one page a %2 mm bleed.", "Gave %1 pages a %2 mm bleed.",
                                        pages.size(), inMillimetres(bleed));
                     *summary += i18n("The trim is what those pages showed before; the sheet grew where it had to.");
                     return true;
                 });
}

void LayoutDialog::runMarks()
{
    PageLayout::Marks marks;
    marks.cropMarks = m_cropMarks->isChecked();
    marks.registrationMarks = m_registrationMarks->isChecked();
    marks.colourBars = m_colourBars->isChecked();
    marks.pageInformation = m_pageInformation->isChecked();
    marks.offsetPoints = toPoints(m_markOffset->value());
    marks.lengthPoints = toPoints(m_markLength->value());
    marks.lineWidth = m_markLineWidth->value();

    QStringList wanted;
    if (marks.cropMarks) {
        wanted += i18nc("@item a kind of printer's mark", "crop marks");
    }
    if (marks.registrationMarks) {
        wanted += i18nc("@item a kind of printer's mark", "registration marks");
    }
    if (marks.colourBars) {
        wanted += i18nc("@item a kind of printer's mark", "colour bars");
    }
    if (marks.pageInformation) {
        wanted += i18nc("@item a kind of printer's mark", "page information");
    }
    if (wanted.isEmpty()) {
        KMessageBox::information(this, i18n("No mark is ticked, so there is nothing to add."), windowTitle());
        return;
    }

    runProducing(m_document, this, windowTitle(), u"-marks.pdf"_s,
                 [this, marks, wanted](const QString &out, QStringList *summary, QString *error) {
                     QApplication::setOverrideCursor(Qt::WaitCursor);
                     const bool ok = PageLayout::addMarks(m_pdf, out, marks, error);
                     QApplication::restoreOverrideCursor();
                     if (!ok) {
                         return false;
                     }
                     *summary += i18n("Added %1, %2 mm outside the trim.", wanted.join(u", "_s),
                                      inMillimetres(marks.offsetPoints));
                     return true;
                 });
}

// ── Cut and tile ──────────────────────────────────────────────────────────

QWidget *LayoutDialog::buildSplitAndTile()
{
    auto *page = new QWidget(this);

    auto *splitBox = new QGroupBox(i18nc("@title:group", "Cut each page in two"), page);
    m_cutVertically
        = new QRadioButton(i18nc("@option:radio", "Down the middle, into a left and a right half"), splitBox);
    m_cutVertically->setChecked(true);
    auto *horizontally = new QRadioButton(i18nc("@option:radio", "Across, into a top and a bottom half"), splitBox);

    m_cutAt = new QDoubleSpinBox(splitBox);
    m_cutAt->setDecimals(1);
    m_cutAt->setRange(1.0, 99.0);
    m_cutAt->setValue(50.0);
    m_cutAt->setSuffix(i18nc("@item percent suffix in a spin box", " %"));
    m_cutAt->setToolTip(i18nc("@info:tooltip",
                              "Measured from the left for a cut down the middle, from the top for a cut across, and "
                              "on the page as it is displayed, so a rotated page cuts where it looks like it will."));

    m_splitPages = new QLineEdit(splitBox);
    m_splitPages->setPlaceholderText(i18n("All pages. Or: 1-4, 8, 12-"));

    auto *splitForm = new QFormLayout;
    splitForm->addRow(i18nc("@label:spinbox", "Cut at:"), m_cutAt);
    splitForm->addRow(i18nc("@label:textbox", "Pages:"), m_splitPages);

    auto *splitNote = new QLabel(i18n("The scanned-book fix, where one photograph holds two facing pages. Both "
                                      "halves keep pointing at the one original content stream, so no pixel is "
                                      "resampled and the file barely grows."),
                                 splitBox);
    splitNote->setWordWrap(true);

    auto *split = new QPushButton(QIcon::fromTheme(u"edit-cut"_s), i18nc("@action:button", "Cut the Pages"), splitBox);

    auto *splitLayout = new QVBoxLayout(splitBox);
    splitLayout->addWidget(m_cutVertically);
    splitLayout->addWidget(horizontally);
    splitLayout->addLayout(splitForm);
    splitLayout->addWidget(splitNote);
    splitLayout->addWidget(split);

    auto *posterBox = new QGroupBox(i18nc("@title:group", "One page across several sheets"), page);
    m_posterPage = new QSpinBox(posterBox);
    m_posterPage->setRange(1, qMax(1, pageCount()));
    m_posterSheet = new PaperChooser(false, posterBox);
    m_posterOverlap = millimetreBox(posterBox, 0.0, 100.0, 10.0);
    m_posterMarks = new QCheckBox(i18nc("@option:check", "Draw the cut line and number each sheet"), posterBox);
    m_posterMarks->setChecked(true);

    auto *posterForm = new QFormLayout;
    posterForm->addRow(i18nc("@label:spinbox", "Page:"), m_posterPage);
    posterForm->addRow(i18nc("@label:listbox", "Sheet size:"), m_posterSheet);
    posterForm->addRow(i18nc("@label:spinbox", "Sheets overlap by:"), m_posterOverlap);
    posterForm->addRow(QString(), m_posterMarks);

    auto *posterNote = new QLabel(i18n("The page is tiled at the size it already is. For a wall-sized poster out of "
                                       "an A4 page, resize it first and tile that: one call that both enlarged and "
                                       "tiled would have to guess which of the two the sheet size meant."),
                                  posterBox);
    posterNote->setWordWrap(true);

    auto *poster
        = new QPushButton(QIcon::fromTheme(u"view-grid"_s), i18nc("@action:button", "Make the Poster"), posterBox);

    auto *posterLayout = new QVBoxLayout(posterBox);
    posterLayout->addLayout(posterForm);
    posterLayout->addWidget(posterNote);
    posterLayout->addWidget(poster);

    auto *layout = new QVBoxLayout(page);
    layout->addWidget(splitBox);
    layout->addWidget(posterBox);
    layout->addStretch(1);

    connect(split, &QPushButton::clicked, this, [this] { runSplit(); });
    connect(poster, &QPushButton::clicked, this, [this] { runPoster(); });
    return page;
}

void LayoutDialog::runSplit()
{
    const QVector<int> pages = chosenPages(m_splitPages);
    if (pages.isEmpty()) {
        return;
    }

    const bool vertical = m_cutVertically->isChecked();
    const double fraction = m_cutAt->value() / 100.0;

    runProducing(m_document, this, windowTitle(), u"-split.pdf"_s,
                 [this, pages, vertical, fraction](const QString &out, QStringList *summary, QString *error) {
                     QApplication::setOverrideCursor(Qt::WaitCursor);
                     const bool ok = PageLayout::splitPages(m_pdf, out, pages, vertical, fraction, error);
                     QApplication::restoreOverrideCursor();
                     if (!ok) {
                         return false;
                     }
                     *summary += i18ncp("@info", "Cut one page in two.", "Cut %1 pages in two.", pages.size());
                     *summary += vertical ? i18n("The left half comes first, then the right.")
                                          : i18n("The top half comes first, then the bottom.");
                     return true;
                 });
}

void LayoutDialog::runPoster()
{
    const int page = m_posterPage->value() - 1;
    const QSizeF sheet = m_posterSheet->sizePoints();
    const double overlap = toPoints(m_posterOverlap->value());
    const bool marks = m_posterMarks->isChecked();
    const QString size = m_posterSheet->describe();

    runProducing(m_document, this, windowTitle(), u"-poster.pdf"_s,
                 [this, page, sheet, overlap, marks, size](const QString &out, QStringList *summary, QString *error) {
                     int sheets = 0;
                     QApplication::setOverrideCursor(Qt::WaitCursor);
                     const bool ok = PageLayout::poster(m_pdf, out, page, sheet, overlap, marks, &sheets, error);
                     QApplication::restoreOverrideCursor();
                     if (!ok) {
                         return false;
                     }
                     *summary += i18np("Tiled page %2 across one sheet of %3.", "Tiled page %2 across %1 sheets of %3.",
                                       sheets, page + 1, size);
                     if (marks) {
                         *summary += i18n("Every edge a neighbouring sheet overlaps carries its cut line, and the "
                                          "sheets are numbered.");
                     }
                     return true;
                 });
}

// ── Booklet and overlay ───────────────────────────────────────────────────

QWidget *LayoutDialog::buildImposition()
{
    auto *page = new QWidget(this);

    auto *bookletBox = new QGroupBox(i18nc("@title:group", "Booklet order"), page);
    m_toPrinterSpreads = new QRadioButton(i18nc("@option:radio", "Reading order into printer's spreads"), bookletBox);
    m_toPrinterSpreads->setChecked(true);
    auto *toReader = new QRadioButton(i18nc("@option:radio", "Printer's spreads back into reading order"), bookletBox);

    auto *bookletNote = new QLabel(i18n("Eight pages become 8, 1, 2, 7, 6, 3, 4, 5, which is the order a "
                                        "saddle-stitched booklet is laid down in. The pages are reordered, not paired onto sheets: "
                                        "run “Arrange on Sheets” with two pages per sheet over the result to get "
                                        "the sheets themselves."),
                                   bookletBox);
    bookletNote->setWordWrap(true);

    auto *booklet
        = new QPushButton(QIcon::fromTheme(u"document-swap"_s), i18nc("@action:button", "Reorder"), bookletBox);

    auto *bookletLayout = new QVBoxLayout(bookletBox);
    bookletLayout->addWidget(m_toPrinterSpreads);
    bookletLayout->addWidget(toReader);
    bookletLayout->addWidget(bookletNote);
    bookletLayout->addWidget(booklet);

    auto *overlayBox = new QGroupBox(i18nc("@title:group", "Lay another document over this one"), page);
    m_overlayFile = new QLineEdit(overlayBox);
    m_overlayFile->setPlaceholderText(i18n("The PDF to lay on top"));
    auto *browse = new QPushButton(i18nc("@action:button", "Choose…"), overlayBox);

    auto *fileRow = new QHBoxLayout;
    fileRow->addWidget(m_overlayFile, 1);
    fileRow->addWidget(browse);

    m_repeatOverlay = new QCheckBox(i18nc("@option:check", "Start the overlay again when it runs out"), overlayBox);
    m_repeatOverlay->setChecked(true);
    m_repeatOverlay->setToolTip(i18nc("@info:tooltip",
                                      "What a letterhead or a background wants. Without it, the pages past the end "
                                      "of the overlay are left exactly as they were."));

    auto *overlayNote = new QLabel(i18n("Page one goes on page one, and so on. The overlay is scaled to the base "
                                        "page's visible area, so a letterhead drawn a millimetre off still lands "
                                        "square."),
                                   overlayBox);
    overlayNote->setWordWrap(true);

    auto *overlay
        = new QPushButton(QIcon::fromTheme(u"layer-visible-on"_s), i18nc("@action:button", "Lay It Over"), overlayBox);

    auto *overlayLayout = new QVBoxLayout(overlayBox);
    overlayLayout->addLayout(fileRow);
    overlayLayout->addWidget(m_repeatOverlay);
    overlayLayout->addWidget(overlayNote);
    overlayLayout->addWidget(overlay);

    auto *layout = new QVBoxLayout(page);
    layout->addWidget(bookletBox);
    layout->addWidget(overlayBox);
    layout->addStretch(1);

    connect(browse, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getOpenFileName(this, i18nc("@title:window", "Document to Lay Over"),
                                                          QString(), i18n("PDF documents (*.pdf);;All files (*)"));
        if (!path.isEmpty()) {
            m_overlayFile->setText(path);
        }
    });
    connect(booklet, &QPushButton::clicked, this, [this] { runBooklet(); });
    connect(overlay, &QPushButton::clicked, this, [this] { runOverlay(); });
    return page;
}

void LayoutDialog::runBooklet()
{
    const bool toPrinter = m_toPrinterSpreads->isChecked();
    runProducing(m_document, this, windowTitle(), u"-booklet.pdf"_s,
                 [this, toPrinter](const QString &out, QStringList *summary, QString *error) {
                     QApplication::setOverrideCursor(Qt::WaitCursor);
                     const bool ok = PageLayout::reorderForBinding(m_pdf, out, toPrinter, error);
                     QApplication::restoreOverrideCursor();
                     if (!ok) {
                         return false;
                     }
                     *summary += toPrinter
                         ? i18n("Reordered into printer's spreads. Run “Arrange on Sheets” with two pages per sheet "
                                "over this to get the sheets themselves.")
                         : i18n("Put the pages back into reading order.");
                     return true;
                 });
}

void LayoutDialog::runOverlay()
{
    const QString overlay = m_overlayFile->text().trimmed();
    if (overlay.isEmpty() || !QFileInfo::exists(overlay)) {
        KMessageBox::error(this, i18n("Choose a PDF to lay over this document first."), windowTitle());
        return;
    }

    const bool repeat = m_repeatOverlay->isChecked();
    runProducing(m_document, this, windowTitle(), u"-overlaid.pdf"_s,
                 [this, overlay, repeat](const QString &out, QStringList *summary, QString *error) {
                     QApplication::setOverrideCursor(Qt::WaitCursor);
                     const bool ok = PageLayout::overlayPages(m_pdf, overlay, out, repeat, error);
                     QApplication::restoreOverrideCursor();
                     if (!ok) {
                         return false;
                     }
                     *summary += i18n("Laid %1 over the document.", QFileInfo(overlay).fileName());
                     *summary += repeat ? i18n("The overlay started again whenever it ran out.")
                                        : i18n("Pages past the end of the overlay were left as they were.");
                     return true;
                 });
}

// ── Numbering ─────────────────────────────────────────────────────────────

QWidget *LayoutDialog::buildNumbering()
{
    auto *page = new QWidget(this);

    m_labels = new QTableWidget(0, 4, page);
    m_labels->setHorizontalHeaderLabels({ i18nc("@title:column", "From page"), i18nc("@title:column", "Style"),
                                          i18nc("@title:column", "Prefix"), i18nc("@title:column", "Start at") });
    m_labels->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_labels->verticalHeader()->hide();

    auto *add = new QPushButton(QIcon::fromTheme(u"list-add"_s), i18nc("@action:button", "Add a Range"), page);
    auto *remove
        = new QPushButton(QIcon::fromTheme(u"list-remove"_s), i18nc("@action:button", "Remove the Range"), page);
    auto *current = new QPushButton(i18nc("@action:button", "Show the Numbers Now"), page);

    auto *row = new QHBoxLayout;
    row->addWidget(add);
    row->addWidget(remove);
    row->addWidget(current);
    row->addStretch(1);

    auto *note = new QLabel(i18n("These are the numbers a reader shows in its page box (“iv” for the fourth "
                                 "front-matter page), not anything printed on the paper. Each range runs until the "
                                 "next one begins, so front matter starting at page 1 in lower-case roman and the "
                                 "body starting at page 9 in decimal is two ranges."),
                            page);
    note->setWordWrap(true);

    auto *run = new QPushButton(QIcon::fromTheme(u"format-list-ordered"_s),
                                i18nc("@action:button", "Write the Numbering"), page);

    auto *layout = new QVBoxLayout(page);
    layout->addWidget(m_labels, 1);
    layout->addLayout(row);
    layout->addWidget(note);
    layout->addWidget(run);

    connect(add, &QPushButton::clicked, this,
            [this] { addLabelRow(m_labels->rowCount() + 1, PageLayout::Labels::Style::Decimal, QString(), 1); });
    connect(remove, &QPushButton::clicked, this, [this] {
        if (m_labels->currentRow() >= 0) {
            m_labels->removeRow(m_labels->currentRow());
        }
    });
    connect(current, &QPushButton::clicked, this, [this] { showLabels(); });
    connect(run, &QPushButton::clicked, this, [this] { runLabels(); });
    return page;
}

void LayoutDialog::addLabelRow(int fromPage, PageLayout::Labels::Style style, const QString &prefix, int startAt)
{
    const int row = m_labels->rowCount();
    m_labels->insertRow(row);

    auto *from = new QSpinBox(m_labels);
    from->setRange(1, qMax(1, pageCount()));
    from->setValue(qBound(1, fromPage, qMax(1, pageCount())));
    m_labels->setCellWidget(row, 0, from);

    auto *styles = new QComboBox(m_labels);
    for (PageLayout::Labels::Style one :
         { PageLayout::Labels::Style::Decimal, PageLayout::Labels::Style::RomanUpper,
           PageLayout::Labels::Style::RomanLower, PageLayout::Labels::Style::LetterUpper,
           PageLayout::Labels::Style::LetterLower, PageLayout::Labels::Style::None }) {
        styles->addItem(describeStyle(one), static_cast<int>(one));
    }
    styles->setCurrentIndex(qMax(0, styles->findData(static_cast<int>(style))));
    m_labels->setCellWidget(row, 1, styles);

    m_labels->setItem(row, 2, new QTableWidgetItem(prefix));

    auto *start = new QSpinBox(m_labels);
    start->setRange(1, 99999);
    start->setValue(qMax(1, startAt));
    m_labels->setCellWidget(row, 3, start);
}

void LayoutDialog::showLabels()
{
    QApplication::setOverrideCursor(Qt::WaitCursor);
    QString error;
    const QStringList labels = PageLayout::labelsOf(m_pdf, &error);
    QApplication::restoreOverrideCursor();

    if (labels.isEmpty()) {
        showReport(this, windowTitle(),
                   { error.isEmpty() ? i18n("This document has no page labels, so readers show the physical page "
                                            "numbers.")
                                     : error });
        return;
    }

    QStringList lines;
    for (int i = 0; i < labels.size(); ++i) {
        lines += i18nc("@info physical page number, then the label the reader shows", "Page %1: %2",
                       QLocale().toString(i + 1), labels.at(i));
    }
    showReport(this, windowTitle(), lines, true);
}

void LayoutDialog::runLabels()
{
    QVector<PageLayout::Labels> ranges;
    for (int row = 0; row < m_labels->rowCount(); ++row) {
        auto *from = qobject_cast<QSpinBox *>(m_labels->cellWidget(row, 0));
        auto *style = qobject_cast<QComboBox *>(m_labels->cellWidget(row, 1));
        auto *start = qobject_cast<QSpinBox *>(m_labels->cellWidget(row, 3));
        if (!from || !style || !start) {
            continue;
        }

        PageLayout::Labels range;
        range.fromPage = from->value() - 1;
        range.style = static_cast<PageLayout::Labels::Style>(style->currentData().toInt());
        range.prefix = m_labels->item(row, 2) ? m_labels->item(row, 2)->text() : QString();
        range.startAt = start->value();
        ranges.append(range);
    }

    if (ranges.isEmpty()) {
        KMessageBox::information(this, i18n("Add a numbering range first."), windowTitle());
        return;
    }

    // A number tree several readers treat as all-or-nothing is no place for
    // ranges in the order somebody happened to type them.
    std::sort(ranges.begin(), ranges.end(),
              [](const PageLayout::Labels &a, const PageLayout::Labels &b) { return a.fromPage < b.fromPage; });

    runProducing(
        m_document, this, windowTitle(), u"-labelled.pdf"_s,
        [this, ranges](const QString &out, QStringList *summary, QString *error) {
            QApplication::setOverrideCursor(Qt::WaitCursor);
            const bool ok = PageLayout::setLabels(m_pdf, out, ranges, error);
            QApplication::restoreOverrideCursor();
            if (!ok) {
                return false;
            }
            *summary += i18ncp("@info", "Wrote one numbering range.", "Wrote %1 numbering ranges.", ranges.size());
            for (const PageLayout::Labels &range : ranges) {
                *summary += i18nc("@info one numbering range: where it starts and how it counts", "From page %1: %2",
                                  QLocale().toString(range.fromPage + 1), describeStyle(range.style));
            }
            return true;
        });
}

// ── Blank pages ───────────────────────────────────────────────────────────

QWidget *LayoutDialog::buildBlanks()
{
    auto *page = new QWidget(this);

    m_inkThreshold = new QDoubleSpinBox(page);
    m_inkThreshold->setDecimals(0);
    m_inkThreshold->setRange(0.0, 50.0);
    m_inkThreshold->setValue(0.0);
    m_inkThreshold->setToolTip(i18nc("@info:tooltip",
                                     "How much painting a page may still contain and count as blank. Zero insists "
                                     "on a page that paints nothing whatsoever; two forgives a running head and a "
                                     "page number."));

    auto *find
        = new QPushButton(QIcon::fromTheme(u"edit-find"_s), i18nc("@action:button", "Find the Blank Pages"), page);

    auto *findRow = new QHBoxLayout;
    findRow->addWidget(new QLabel(i18nc("@label:spinbox", "Painting operators allowed:"), page));
    findRow->addWidget(m_inkThreshold);
    findRow->addWidget(find);
    findRow->addStretch(1);

    auto *note = new QLabel(i18n("Read from the content streams rather than by rendering: a page whose streams hold "
                                 "no painting operator paints nothing, and that can be established without a "
                                 "rasteriser and without an opinion about colour."),
                            page);
    note->setWordWrap(true);

    m_blankResult = new QLabel(i18n("Not looked for yet."), page);
    m_blankResult->setWordWrap(true);
    m_blankResult->setTextInteractionFlags(Qt::TextSelectableByMouse);

    m_removePages = new QLineEdit(page);
    m_removePages->setPlaceholderText(i18n("Pages to take out, e.g. 3, 7-9"));

    auto *remove = new QPushButton(QIcon::fromTheme(u"edit-delete-page"_s),
                                   i18nc("@action:button", "Take These Pages Out"), page);

    auto *removeForm = new QFormLayout;
    removeForm->addRow(i18nc("@label:textbox", "Pages:"), m_removePages);

    auto *removeBox = new QGroupBox(i18nc("@title:group", "Take pages out"), page);
    auto *removeNote = new QLabel(i18n("Filled in by the search above, and editable afterwards. This writes a "
                                       "shorter document rather than touching the one on screen, so nothing is lost "
                                       "by trying it."),
                                  removeBox);
    removeNote->setWordWrap(true);

    auto *removeLayout = new QVBoxLayout(removeBox);
    removeLayout->addLayout(removeForm);
    removeLayout->addWidget(removeNote);
    removeLayout->addWidget(remove);

    auto *layout = new QVBoxLayout(page);
    layout->addLayout(findRow);
    layout->addWidget(note);
    layout->addWidget(m_blankResult);
    layout->addStretch(1);
    layout->addWidget(removeBox);

    connect(find, &QPushButton::clicked, this, [this] { findBlanks(); });
    connect(remove, &QPushButton::clicked, this, [this] { runRemove(); });
    return page;
}

void LayoutDialog::findBlanks()
{
    QApplication::setOverrideCursor(Qt::WaitCursor);
    QString error;
    const QVector<int> blanks = PageLayout::findBlankPages(m_pdf, m_inkThreshold->value(), &error);
    QApplication::restoreOverrideCursor();

    if (blanks.isEmpty()) {
        if (!error.isEmpty()) {
            KMessageBox::error(this, error, windowTitle());
            return;
        }
        m_blankResult->setText(i18n("Every page paints something."));
        return;
    }

    m_blankResult->setText(
        i18np("One page paints nothing: %2.", "%1 pages paint nothing: %2.", blanks.size(), PageRange::format(blanks)));
    m_removePages->setText(PageRange::format(blanks));
}

void LayoutDialog::runRemove()
{
    // An empty range means "all pages" everywhere else in this program, and here
    // that would empty the document. Removal has to be asked for by name.
    if (m_removePages->text().trimmed().isEmpty()) {
        KMessageBox::error(this, i18n("Name the pages to take out. Leaving this empty would mean all of them."),
                           windowTitle());
        return;
    }

    const QVector<int> pages = chosenPages(m_removePages);
    if (pages.isEmpty()) {
        return;
    }

    // Counted without repeats, because "3, 3" names one page twice rather than
    // two pages, and refusing that would be refusing a legitimate request.
    QVector<int> distinct = pages;
    std::sort(distinct.begin(), distinct.end());
    distinct.erase(std::unique(distinct.begin(), distinct.end()), distinct.end());
    if (distinct.size() >= pageCount()) {
        KMessageBox::error(this, i18n("That is every page, and a document without pages is not a document."),
                           windowTitle());
        return;
    }

    runProducing(m_document, this, windowTitle(), u"-shorter.pdf"_s,
                 [this, pages](const QString &out, QStringList *summary, QString *error) {
                     QApplication::setOverrideCursor(Qt::WaitCursor);
                     const bool ok = PageLayout::removePages(m_pdf, out, pages, error);
                     QApplication::restoreOverrideCursor();
                     if (!ok) {
                         return false;
                     }
                     *summary += i18ncp("@info", "Took out one page: %2.", "Took out %1 pages: %2.", pages.size(),
                                        PageRange::format(pages));
                     return true;
                 });
}

} // namespace

void showLayout(Document *document, QWidget *parent)
{
    const QString pdf = savedPath(document, parent);
    if (pdf.isEmpty()) {
        return;
    }
    LayoutDialog(document, pdf, parent).exec();
}

} // namespace ps::tools
