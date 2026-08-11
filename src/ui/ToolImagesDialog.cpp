/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "ToolDialogs.h"

#include "ToolSupport.h"
#include "core/Document.h"
#include "core/ImageEdit.h"
#include "core/PageRange.h"

#include <KLocalizedString>
#include <KMessageBox>

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

using namespace Qt::Literals::StringLiterals;

namespace ps::tools {

namespace {

/** Above this a picture costs bytes that no printer and no screen ever shows. */
constexpr double lavishDpi = 300.0;

/** The number a formatted column really sorts on, behind the text it shows. */
constexpr int SortRole = Qt::UserRole;

/** Which picture a row stands for, so sorting the table cannot shuffle them apart. */
constexpr int IndexRole = Qt::UserRole + 1;

/** What the scope box means; recompress() takes pages and names, not an enum. */
enum class Scope { Everything, Pages, Picked };

/** A cell that sorts on its number rather than on its text. */
class NumberItem : public QTableWidgetItem
{
public:
    NumberItem(const QString &text, double value)
        : QTableWidgetItem(text)
    {
        setData(SortRole, value);
        setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    }

    bool operator<(const QTableWidgetItem &other) const override
    {
        return data(SortRole).toDouble() < other.data(SortRole).toDouble();
    }
};

QString humanSize(qint64 bytes)
{
    return QLocale().formattedDataSize(bytes, 1, QLocale::DataSizeTraditionalFormat);
}

/** The resolution as placed, as one number when both axes agree. */
QString dpiText(const ImageEdit::ImageUse &use)
{
    const int across = int(std::lround(use.effectiveDpiX));
    const int down = int(std::lround(use.effectiveDpiY));
    if (across <= 0 || down <= 0) {
        return i18nc("@item effective resolution that could not be worked out", "unknown");
    }
    // Only worth two numbers when the placement genuinely stretched the pixels.
    if (std::abs(across - down) * 20 > std::max(across, down)) {
        return u"%1×%2"_s.arg(across).arg(down);
    }
    return QString::number(across);
}

/** What is unusual about this picture, in a word each. */
QStringList notesFor(const ImageEdit::ImageUse &use)
{
    QStringList notes;
    if (use.hasAlpha) {
        notes += i18nc("@item this picture carries a soft mask, so parts of it are transparent", "soft mask");
    }
    if (use.isMask) {
        notes += i18nc("@item a stencil that takes its colour from the page", "stencil");
    }
    if (!use.decodable) {
        notes += i18nc("@item the pixels are stored in a form this tool cannot open", "cannot open");
    }
    return notes;
}

/** A resource name a file system will accept, e.g. /Im0 as Im0. */
QString fileStem(const QString &resourceName)
{
    QString stem;
    for (const QChar letter : resourceName) {
        stem += letter.isLetterOrNumber() || letter == u'-' || letter == u'_' ? letter : u'_';
    }
    while (stem.startsWith(u'_')) {
        stem.remove(0, 1);
    }
    return stem.isEmpty() ? u"image"_s : stem;
}

/** Before and after, because the size is usually what the request was about. */
QString sizeChange(const QString &input, const QString &output)
{
    const qint64 before = QFileInfo(input).size();
    const qint64 after = QFileInfo(output).size();
    if (before <= 0 || after <= 0) {
        return {};
    }
    if (after < before) {
        return i18n("%1 → %2 (%3% smaller)", humanSize(before), humanSize(after),
                    QString::number((before - after) * 100 / before));
    }
    return i18n("%1 → %2", humanSize(before), humanSize(after));
}

/**
 * Every picture in the document, and what can be done to one of them.
 *
 * The table is the tool and the tabs behind it are where its numbers lead. A
 * document's pictures go wrong in four ways (far more pixels than the size
 * they print at needs, pixels that are themselves bad, the wrong picture in the
 * frame, or one picture stored five times), so there are four tabs, and the
 * effective resolution column is what tells the user which of the four
 * conversations to have.
 *
 * Everything here addresses a picture the way a content stream does, by page
 * and resource name, which is why picking a row comes first. "The photograph on
 * page four" is not a question a PDF can answer, and a dialog that pretended
 * otherwise would edit the wrong object on the first day a page held two.
 */
class ImagesDialog : public QDialog
{
public:
    ImagesDialog(Document *document, const QString &pdf, QWidget *parent);

private:
    QWidget *buildInventory();
    QWidget *buildSmaller();
    QWidget *buildPixels();
    QWidget *buildTidy();

    void read();
    void fill();
    void followSelection();

    /** The picture the table has picked, or null. Never held across a read(). */
    const ImageEdit::ImageUse *picked() const;

    void extractPicked();
    void extractEverything();
    void replacePicked();
    void rotatePicked();
    void removePicked();
    void runRecompress();
    void runAdjust();
    void runCrop();
    void runDeduplicate();

    /**
     * The pair of lists recompress() reads in step, from the scope box.
     *
     * Both left empty is its whole-document mode, where a picture it cannot
     * open is skipped rather than failing the run. Returns false when the user
     * asked for something that is not there, having said so.
     */
    bool chosenPictures(QVector<int> *pages, QStringList *names, int *skipped);

    Document *m_document = nullptr;
    QString m_pdf;
    QVector<ImageEdit::ImageUse> m_uses;

    QTableWidget *m_table = nullptr;
    QLabel *m_totals = nullptr;
    QLabel *m_picked = nullptr;
    QLabel *m_pickedPixels = nullptr;
    QCheckBox *m_keepAspect = nullptr;
    QDoubleSpinBox *m_angle = nullptr;
    QVector<QPushButton *> m_needPicture;
    QVector<QPushButton *> m_needPixels;

    QComboBox *m_scope = nullptr;
    QLineEdit *m_pages = nullptr;
    QSpinBox *m_targetDpi = nullptr;
    QSpinBox *m_quality = nullptr;
    QCheckBox *m_lossless = nullptr;
    QCheckBox *m_grey = nullptr;
    QCheckBox *m_force = nullptr;

    QDoubleSpinBox *m_brightness = nullptr;
    QDoubleSpinBox *m_contrast = nullptr;
    QDoubleSpinBox *m_gamma = nullptr;
    QCheckBox *m_sharpen = nullptr;
    QCheckBox *m_despeckle = nullptr;
    QCheckBox *m_autoLevels = nullptr;
    QCheckBox *m_toGrey = nullptr;
    QCheckBox *m_toBlackWhite = nullptr;
    QSpinBox *m_threshold = nullptr;

    QDoubleSpinBox *m_keepX = nullptr;
    QDoubleSpinBox *m_keepY = nullptr;
    QDoubleSpinBox *m_keepWidth = nullptr;
    QDoubleSpinBox *m_keepHeight = nullptr;
};

ImagesDialog::ImagesDialog(Document *document, const QString &pdf, QWidget *parent)
    : QDialog(parent)
    , m_document(document)
    , m_pdf(pdf)
{
    setWindowTitle(i18nc("@title:window", "Pictures"));
    resize(1080, 800);

    auto *tabs = new QTabWidget(this);
    tabs->addTab(buildInventory(), i18nc("@title:tab", "What Is in Here"));
    tabs->addTab(buildSmaller(), i18nc("@title:tab", "Make Them Smaller"));
    tabs->addTab(buildPixels(), i18nc("@title:tab", "Work on the Pixels"));
    tabs->addTab(buildTidy(), i18nc("@title:tab", "Tidying Up"));

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(tabs, 1);
    layout->addWidget(buttons);

    read();
}

// ── What is in here ───────────────────────────────────────────────────────

QWidget *ImagesDialog::buildInventory()
{
    auto *page = new QWidget(this);

    m_table = new QTableWidget(0, 10, page);
    m_table->setHorizontalHeaderLabels(
        { i18nc("@title:column page number", "Page"),
          i18nc("@title:column the /Im0 name a content stream uses", "Name"),
          i18nc("@title:column effective resolution, pixels per inch as placed", "dpi"),
          i18nc("@title:column the stored pixel size", "Pixels"),
          i18nc("@title:column how large it is drawn", "On page"),
          i18nc("@title:column the colour space the pixels are in", "Colour"),
          i18nc("@title:column how the pixels are compressed", "Format"),
          i18nc("@title:column bytes it occupies in the file", "Stored"),
          i18nc("@title:column how often the document draws this same picture", "Drawn"),
          i18nc("@title:column anything unusual about this picture", "Notes") });
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setAlternatingRowColors(true);
    m_table->setSortingEnabled(true);
    m_table->verticalHeader()->hide();
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setStretchLastSection(true);
    // Six of the ten columns are numbers to be compared down the column, and a
    // column of numbers only reads as one when every digit is the same width.
    m_table->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));

    m_totals = new QLabel(page);
    m_totals->setWordWrap(true);

    auto *chosen = new QGroupBox(i18nc("@title:group", "The picture you have picked"), page);
    m_picked = new QLabel(chosen);
    m_picked->setWordWrap(true);

    auto *extract = new QPushButton(QIcon::fromTheme(u"document-save-as"_s), i18nc("@action:button", "Write It Out…"),
                                    chosen);
    extract->setToolTip(i18nc("@info:tooltip",
                              "A JPEG asked for as .jpg is copied out compressed, byte for byte: the one "
                              "extraction that loses nothing at all."));
    auto *replace = new QPushButton(QIcon::fromTheme(u"edit-image-face-show"_s),
                                    i18nc("@action:button", "Put Another in Its Place…"), chosen);
    auto *remove = new QPushButton(QIcon::fromTheme(u"edit-delete"_s), i18nc("@action:button", "Take It Off the Page"),
                                   chosen);

    m_angle = new QDoubleSpinBox(chosen);
    m_angle->setRange(-360.0, 360.0);
    m_angle->setDecimals(1);
    m_angle->setSingleStep(90.0);
    m_angle->setValue(90.0);
    m_angle->setSuffix(i18nc("@item degree suffix in a spin box", " °"));
    auto *turn =
        new QPushButton(QIcon::fromTheme(u"object-rotate-right"_s), i18nc("@action:button", "Turn It"), chosen);
    turn->setToolTip(i18nc("@info:tooltip",
                           "A turn is a matrix in the content stream, so not one stored byte of the picture is "
                           "rewritten: a fax or a JPEG 2000 turns as well as a JPEG, and any angle works."));

    m_keepAspect =
        new QCheckBox(i18nc("@option:check", "Fit a replacement inside the old place rather than stretch it"), chosen);
    m_keepAspect->setChecked(true);

    auto *actions = new QHBoxLayout;
    actions->addWidget(extract);
    actions->addWidget(replace);
    actions->addWidget(remove);
    actions->addStretch(1);
    actions->addWidget(m_angle);
    actions->addWidget(turn);

    auto *chosenLayout = new QVBoxLayout(chosen);
    chosenLayout->addWidget(m_picked);
    chosenLayout->addLayout(actions);
    chosenLayout->addWidget(m_keepAspect);

    auto *again = new QPushButton(QIcon::fromTheme(u"view-refresh"_s), i18nc("@action:button", "Read Again"), page);
    auto *all = new QPushButton(QIcon::fromTheme(u"document-save-all"_s),
                                i18nc("@action:button", "Write Every Picture Out…"), page);

    auto *bottom = new QHBoxLayout;
    bottom->addWidget(again);
    bottom->addWidget(all);
    bottom->addStretch(1);

    auto *layout = new QVBoxLayout(page);
    layout->addWidget(m_table, 1);
    layout->addWidget(m_totals);
    layout->addWidget(chosen);
    layout->addLayout(bottom);

    m_needPicture = { extract, replace, remove, turn };

    connect(m_table, &QTableWidget::itemSelectionChanged, this, [this] {
        followSelection();
    });
    connect(again, &QPushButton::clicked, this, [this] {
        read();
    });
    connect(all, &QPushButton::clicked, this, [this] {
        extractEverything();
    });
    connect(extract, &QPushButton::clicked, this, [this] {
        extractPicked();
    });
    connect(replace, &QPushButton::clicked, this, [this] {
        replacePicked();
    });
    connect(remove, &QPushButton::clicked, this, [this] {
        removePicked();
    });
    connect(turn, &QPushButton::clicked, this, [this] {
        rotatePicked();
    });
    return page;
}

void ImagesDialog::read()
{
    QApplication::setOverrideCursor(Qt::WaitCursor);
    QString error;
    m_uses = ImageEdit::read(m_pdf, &error);
    QApplication::restoreOverrideCursor();

    if (!error.isEmpty()) {
        m_uses.clear();
        KMessageBox::error(this, error, windowTitle());
    }
    fill();
}

void ImagesDialog::fill()
{
    // Sorting has to be off while the rows go in, or each new row is moved
    // under the one being written and the columns end up interleaved.
    m_table->setSortingEnabled(false);
    m_table->clearContents();
    m_table->setRowCount(int(m_uses.size()));

    qint64 total = 0;
    qint64 lavishBytes = 0;
    int lavishCount = 0;
    for (int row = 0; row < m_uses.size(); ++row) {
        const ImageEdit::ImageUse &use = m_uses.at(row);
        total += use.storedBytes;
        const double dpi = std::max(use.effectiveDpiX, use.effectiveDpiY);
        const bool lavish = dpi > lavishDpi;
        if (lavish) {
            lavishBytes += use.storedBytes;
            ++lavishCount;
        }

        auto *number = new NumberItem(QLocale().toString(use.page + 1), use.page);
        number->setData(IndexRole, row);
        m_table->setItem(row, 0, number);
        m_table->setItem(row, 1, new QTableWidgetItem(use.resourceName));

        auto *resolution = new NumberItem(dpiText(use), dpi);
        if (lavish) {
            QFont bold = resolution->font();
            bold.setBold(true);
            resolution->setFont(bold);
            resolution->setToolTip(i18nc("@info:tooltip", "Finer than %1 dpi, which is more than any printer will "
                                                          "show. Bringing it down is free size.",
                                         QLocale().toString(int(lavishDpi))));
        }
        m_table->setItem(row, 2, resolution);

        m_table->setItem(row, 3,
                         new NumberItem(u"%1×%2"_s.arg(use.pixelSize.width()).arg(use.pixelSize.height()),
                                        double(use.pixelSize.width()) * use.pixelSize.height()));
        m_table->setItem(row, 4,
                         new NumberItem(i18nc("@item size on the page, in millimetres", "%1×%2 mm",
                                              QString::number(use.placement.width() * 25.4 / 72.0, 'f', 0),
                                              QString::number(use.placement.height() * 25.4 / 72.0, 'f', 0)),
                                        use.placement.width() * use.placement.height()));
        m_table->setItem(row, 5,
                         new QTableWidgetItem(i18nc("@item a colour space and its bit depth", "%1 %2", use.colourSpace,
                                                    QLocale().toString(use.bitsPerComponent))));
        m_table->setItem(row, 6, new QTableWidgetItem(use.encoding));
        m_table->setItem(row, 7, new NumberItem(humanSize(use.storedBytes), double(use.storedBytes)));
        m_table->setItem(row, 8, new NumberItem(QLocale().toString(use.drawnTimes), use.drawnTimes));
        m_table->setItem(row, 9, new QTableWidgetItem(notesFor(use).join(u", "_s)));
    }
    m_table->setSortingEnabled(true);

    if (m_uses.isEmpty()) {
        m_totals->setText(i18n("No page of this document draws a picture."));
    } else {
        QStringList lines { i18np("One picture, %2 in all.", "%1 pictures, %2 in all.", int(m_uses.size()),
                                  humanSize(total)) };
        if (lavishCount > 0) {
            lines += i18np("%1 of them sits above %2 dpi and holds %3, more than any printer will show. The second "
                           "tab brings them down.",
                           "%1 of them sit above %2 dpi and hold %3, more than any printer will show. The second tab "
                           "brings them down.",
                           lavishCount, QLocale().toString(int(lavishDpi)), humanSize(lavishBytes));
        }
        m_totals->setText(lines.join(u' '));
    }

    followSelection();
}

const ImageEdit::ImageUse *ImagesDialog::picked() const
{
    const int row = m_table->currentRow();
    if (row < 0 || !m_table->item(row, 0) || !m_table->item(row, 0)->isSelected()) {
        return nullptr;
    }
    const int index = m_table->item(row, 0)->data(IndexRole).toInt();
    return index >= 0 && index < m_uses.size() ? &m_uses.at(index) : nullptr;
}

void ImagesDialog::followSelection()
{
    const ImageEdit::ImageUse *use = picked();

    // The pixel operations are the ones that need to open the stream, so a fax
    // or a JPEG 2000 picture disables those and leaves turning and replacing
    // alone, since neither of those ever decodes anything.
    for (QPushButton *button : std::as_const(m_needPicture)) {
        button->setEnabled(use != nullptr);
    }
    for (QPushButton *button : std::as_const(m_needPixels)) {
        button->setEnabled(use != nullptr && use->decodable && !use->isMask);
    }
    m_angle->setEnabled(use != nullptr);

    const QString what = use
        ? i18nc("@info which picture the per-picture actions will work on", "%1 on page %2: %3×%4 pixels at %5 dpi, "
                                                                            "%6 as %7",
                use->resourceName, QLocale().toString(use->page + 1), QLocale().toString(use->pixelSize.width()),
                QLocale().toString(use->pixelSize.height()), dpiText(*use), humanSize(use->storedBytes), use->encoding)
        : i18n("Nothing is picked. Choose a row in the table above.");
    m_picked->setText(what);

    if (!m_pickedPixels) {
        return;
    }
    if (use && !use->decodable) {
        m_pickedPixels->setText(i18n("%1 is stored in a form this cannot open, so its pixels cannot be worked on. "
                                     "Turning it, replacing it and taking it off the page all still work.",
                                     use->resourceName));
    } else if (use && use->isMask) {
        m_pickedPixels->setText(i18n("%1 is a stencil: its one-bit samples choose where the page's own fill colour "
                                     "lands, so it has no brightness or contrast of its own to change.",
                                     use->resourceName));
    } else {
        m_pickedPixels->setText(what);
    }
}

void ImagesDialog::extractPicked()
{
    const ImageEdit::ImageUse *use = picked();
    if (!use) {
        return;
    }
    const int page = use->page;
    const QString name = use->resourceName;

    // A stored JPEG is offered as .jpg because that extraction is a copy rather
    // than a decode; anything else has to go through Qt and png keeps it exact.
    const QString ending = use->encoding == u"DCTDecode"_s ? u"jpg"_s : u"png"_s;
    const QFileInfo file(m_pdf);
    const QString suggested = file.absolutePath() + u'/'
        + u"%1-p%2-%3.%4"_s.arg(file.completeBaseName(), QString::number(page + 1), fileStem(name), ending);

    const QString target =
        QFileDialog::getSaveFileName(this, i18nc("@title:window", "Write the Picture Out"), suggested,
                                     i18n("Pictures (*.png *.jpg *.jpeg *.tif *.webp);;All files (*)"));
    if (target.isEmpty()) {
        return;
    }

    QApplication::setOverrideCursor(Qt::WaitCursor);
    QString error;
    const bool ok = ImageEdit::extract(m_pdf, page, name, target, &error);
    QApplication::restoreOverrideCursor();

    if (!ok) {
        KMessageBox::error(this, error.isEmpty() ? i18n("That picture could not be written out.") : error,
                           windowTitle());
        return;
    }
    KMessageBox::information(this, i18n("Wrote %1, %2.", QFileInfo(target).fileName(),
                                        humanSize(QFileInfo(target).size())),
                             windowTitle());
}

void ImagesDialog::extractEverything()
{
    if (m_uses.isEmpty()) {
        return;
    }
    const QString folder = QFileDialog::getExistingDirectory(this, i18nc("@title:window", "Where the Pictures Go"),
                                                             QFileInfo(m_pdf).absolutePath());
    if (folder.isEmpty()) {
        return;
    }

    const QString base = QFileInfo(m_pdf).completeBaseName();
    QStringList written;
    int skipped = 0;
    qint64 bytes = 0;

    QApplication::setOverrideCursor(Qt::WaitCursor);
    for (const ImageEdit::ImageUse &use : std::as_const(m_uses)) {
        if (use.isMask || !use.decodable) {
            // Picked on its own it would earn a refusal by name; in a sweep it
            // is a note at the end, so one fax does not stop the other twelve.
            ++skipped;
            continue;
        }
        const QString ending = use.encoding == u"DCTDecode"_s ? u"jpg"_s : u"png"_s;
        const QString target = QDir(folder).filePath(
            u"%1-p%2-%3.%4"_s.arg(base, QString::number(use.page + 1), fileStem(use.resourceName), ending));
        QString error;
        if (!ImageEdit::extract(m_pdf, use.page, use.resourceName, target, &error)) {
            ++skipped;
            continue;
        }
        bytes += QFileInfo(target).size();
        written += QFileInfo(target).fileName();
    }
    QApplication::restoreOverrideCursor();

    QStringList report { i18ncp("@info", "Wrote one picture, %2 in all.", "Wrote %1 pictures, %2 in all.",
                                int(written.size()), humanSize(bytes)) };
    if (skipped > 0) {
        report += i18np("Left one picture in the document: its pixels are stored in a form this cannot open.",
                        "Left %1 pictures in the document: their pixels are stored in a form this cannot open.",
                        skipped);
    }
    report += QString();
    report += written;
    showReport(this, windowTitle(), report, true);
}

void ImagesDialog::replacePicked()
{
    const ImageEdit::ImageUse *use = picked();
    if (!use) {
        return;
    }
    const int page = use->page;
    const QString name = use->resourceName;

    const QString source = QFileDialog::getOpenFileName(this, i18nc("@title:window", "The Picture to Put In"),
                                                        QFileInfo(m_pdf).absolutePath(),
                                                        i18n("Pictures (*.png *.jpg *.jpeg *.tif *.tiff *.webp *.bmp);;"
                                                             "All files (*)"));
    if (source.isEmpty()) {
        return;
    }
    const QImage replacement(source);
    if (replacement.isNull()) {
        KMessageBox::error(this, i18n("“%1” could not be read as a picture.", QFileInfo(source).fileName()),
                           windowTitle());
        return;
    }

    const bool keepAspect = m_keepAspect->isChecked();
    runProducing(m_document, this, windowTitle(), u"-edited.pdf"_s,
                 [this, page, name, replacement, keepAspect](const QString &out, QStringList *summary, QString *error) {
                     QApplication::setOverrideCursor(Qt::WaitCursor);
                     const bool ok = ImageEdit::replace(m_pdf, out, page, name, replacement, keepAspect, error);
                     QApplication::restoreOverrideCursor();
                     if (!ok) {
                         return false;
                     }
                     *summary += i18n("Page %1 now draws the new picture, in the place %2 had.",
                                      QLocale().toString(page + 1), name);
                     *summary += keepAspect
                         ? i18n("It was fitted inside that place and centred, so its proportions are its own.")
                         : i18n("It was stretched to fill that place exactly.");
                     if (const QString change = sizeChange(m_pdf, out); !change.isEmpty()) {
                         *summary += change;
                     }
                     return true;
                 });
}

void ImagesDialog::rotatePicked()
{
    const ImageEdit::ImageUse *use = picked();
    if (!use) {
        return;
    }
    const int page = use->page;
    const QString name = use->resourceName;
    const double degrees = m_angle->value();

    runProducing(m_document, this, windowTitle(), u"-turned.pdf"_s,
                 [this, page, name, degrees](const QString &out, QStringList *summary, QString *error) {
                     ImageEdit::RotateReport report;
                     QApplication::setOverrideCursor(Qt::WaitCursor);
                     const bool ok = ImageEdit::rotate(m_pdf, out, page, name, degrees, &report, error);
                     QApplication::restoreOverrideCursor();
                     if (!ok) {
                         return false;
                     }
                     *summary += i18np("Turned %2 on page %3 by %4°, in one place.",
                                       "Turned %2 on page %3 by %4°, in %1 places.", report.placementsTurned, name,
                                       QLocale().toString(page + 1), QLocale().toString(degrees, 'f', 1));
                     if (report.pixelsUntouched) {
                         *summary += i18n("Not one stored byte of the picture was rewritten.");
                     }
                     if (report.placementsRefused > 0) {
                         *summary += i18np("One placement was left alone: its own matrix cannot be reversed, so no "
                                           "turn could be worked out for it.",
                                           "%1 placements were left alone: their own matrices cannot be reversed, so "
                                           "no turn could be worked out for them.",
                                           report.placementsRefused);
                     }
                     *summary += report.notes;
                     return true;
                 });
}

void ImagesDialog::removePicked()
{
    const ImageEdit::ImageUse *use = picked();
    if (!use) {
        return;
    }
    const int page = use->page;
    const QString name = use->resourceName;

    if (KMessageBox::questionTwoActions(
            this,
            i18n("Page %1 will stop drawing %2. The stored bytes go when nothing else in the document points at them.",
                 QLocale().toString(page + 1), name),
            windowTitle(), KGuiItem(i18nc("@action:button", "Take It Off the Page")), KStandardGuiItem::cancel())
        != KMessageBox::PrimaryAction) {
        return;
    }

    runProducing(m_document, this, windowTitle(), u"-edited.pdf"_s,
                 [this, page, name](const QString &out, QStringList *summary, QString *error) {
                     QApplication::setOverrideCursor(Qt::WaitCursor);
                     const bool ok = ImageEdit::remove(m_pdf, out, page, name, error);
                     QApplication::restoreOverrideCursor();
                     if (!ok) {
                         return false;
                     }
                     *summary += i18n("Page %1 no longer draws %2.", QLocale().toString(page + 1), name);
                     if (const QString change = sizeChange(m_pdf, out); !change.isEmpty()) {
                         *summary += change;
                     }
                     return true;
                 });
}

// ── Make them smaller ─────────────────────────────────────────────────────

QWidget *ImagesDialog::buildSmaller()
{
    auto *page = new QWidget(this);

    m_scope = new QComboBox(page);
    m_scope->addItem(i18nc("@item:inlistbox which pictures to work on", "Every picture in the document"),
                     int(Scope::Everything));
    m_scope->addItem(i18nc("@item:inlistbox which pictures to work on", "The pictures on these pages"),
                     int(Scope::Pages));
    m_scope->addItem(i18nc("@item:inlistbox which pictures to work on", "Only the picture picked in the first tab"),
                     int(Scope::Picked));

    m_pages = new QLineEdit(page);
    m_pages->setPlaceholderText(i18n("1-4, 8, 12-"));
    m_pages->setEnabled(false);

    m_targetDpi = new QSpinBox(page);
    m_targetDpi->setRange(0, 1200);
    m_targetDpi->setSingleStep(25);
    m_targetDpi->setValue(150);
    m_targetDpi->setSuffix(i18nc("@item dots-per-inch suffix in a spin box", " dpi"));
    m_targetDpi->setSpecialValueText(i18nc("@item:spinbox no downsampling at all", "keep every pixel"));
    m_targetDpi->setToolTip(i18nc("@info:tooltip",
                                  "Anything finer than this comes down to it. 150 is right for reading on screen, "
                                  "300 for printing. Nothing is ever enlarged."));

    m_quality = new QSpinBox(page);
    m_quality->setRange(1, 100);
    m_quality->setValue(85);
    m_quality->setSuffix(i18nc("@item percent suffix in a spin box", " %"));

    m_lossless = new QCheckBox(i18nc("@option:check", "Re-pack the pixels rather than re-encode them as JPEG"), page);
    m_lossless->setToolTip(i18nc("@info:tooltip",
                                 "Loses nothing at all, and saves far less. Right for a screenshot or a diagram, "
                                 "wrong for a photograph."));

    m_grey = new QCheckBox(i18nc("@option:check", "Turn the pictures grey on the way through"), page);

    m_force = new QCheckBox(i18nc("@option:check", "Re-encode even where the resolution is already right"), page);
    m_force->setToolTip(i18nc("@info:tooltip",
                              "Off, a picture that needs nothing is left byte for byte alone. On, the quality is "
                              "applied whatever the resolution says, which is the answer to “smaller without losing "
                              "resolution” and costs a generation of JPEG loss every time."));

    auto *form = new QFormLayout;
    form->addRow(i18nc("@label:listbox", "Work on:"), m_scope);
    form->addRow(i18nc("@label:textbox", "Pages:"), m_pages);
    form->addRow(i18nc("@label:spinbox", "Bring down to:"), m_targetDpi);
    form->addRow(i18nc("@label:spinbox", "JPEG quality:"), m_quality);
    form->addRow(QString(), m_lossless);
    form->addRow(QString(), m_grey);
    form->addRow(QString(), m_force);

    auto *note = new QLabel(i18n("A picture drawn on several pages is re-encoded once, and sized for the largest place "
                                 "it is drawn anywhere: downsampling for a thumbnail would ruin the full-page copy of "
                                 "the same object. What is reported is the bytes that left the picture streams, so a "
                                 "run with nothing to do reports exactly zero."),
                            page);
    note->setWordWrap(true);

    auto *run = new QPushButton(QIcon::fromTheme(u"zoom-out"_s), i18nc("@action:button", "Make Them Smaller"), page);

    auto *layout = new QVBoxLayout(page);
    layout->addLayout(form);
    layout->addWidget(note);
    layout->addStretch(1);
    layout->addWidget(run);

    const auto followScope = [this] {
        m_pages->setEnabled(m_scope->currentData().toInt() == int(Scope::Pages));
        m_quality->setEnabled(!m_lossless->isChecked());
    };
    connect(m_scope, &QComboBox::currentIndexChanged, this, followScope);
    connect(m_lossless, &QCheckBox::toggled, this, followScope);
    connect(run, &QPushButton::clicked, this, [this] {
        runRecompress();
    });
    followScope();
    return page;
}

bool ImagesDialog::chosenPictures(QVector<int> *pages, QStringList *names, int *skipped)
{
    const auto scope = static_cast<Scope>(m_scope->currentData().toInt());
    if (scope == Scope::Everything) {
        return true; // both lists empty is the engine's whole-document mode
    }

    if (scope == Scope::Picked) {
        const ImageEdit::ImageUse *use = picked();
        if (!use) {
            KMessageBox::information(this, i18n("No picture is picked. Choose a row in the first tab, or work on the "
                                                "whole document."),
                                     windowTitle());
            return false;
        }
        // Asked for by name, an unopenable picture earns a refusal from the
        // engine rather than being quietly dropped, which is the right answer
        // to a request this specific.
        pages->append(use->page);
        names->append(use->resourceName);
        return true;
    }

    QString error;
    const QVector<int> wanted = PageRange::parse(m_pages->text().trimmed(),
                                                 m_document ? m_document->pageCount() : 0, &error);
    if (!error.isEmpty()) {
        KMessageBox::error(this, error, windowTitle());
        return false;
    }

    for (const ImageEdit::ImageUse &use : std::as_const(m_uses)) {
        if (!wanted.contains(use.page)) {
            continue;
        }
        if (use.isMask || !use.decodable) {
            // "These pages" is the same kind of request as "the whole file", so
            // anything unopenable is dropped here and counted for the report.
            ++*skipped;
            continue;
        }
        pages->append(use.page);
        names->append(use.resourceName);
    }
    if (pages->isEmpty()) {
        KMessageBox::information(this, i18n("Those pages hold no picture whose pixels this tool can open."),
                                 windowTitle());
        return false;
    }
    return true;
}

void ImagesDialog::runRecompress()
{
    QVector<int> pages;
    QStringList names;
    int skipped = 0;
    if (!chosenPictures(&pages, &names, &skipped)) {
        return;
    }

    ImageEdit::Recompress options;
    options.targetDpi = m_targetDpi->value();
    options.jpegQuality = m_quality->value();
    options.toGrayscale = m_grey->isChecked();
    options.lossless = m_lossless->isChecked();
    options.forceReencode = m_force->isChecked();

    if (options.targetDpi <= 0.0 && !options.forceReencode && !options.toGrayscale) {
        KMessageBox::information(this, i18n("With every pixel kept and no re-encoding forced, there is nothing for "
                                            "this to do. Give it a resolution to come down to, or tick the last box."),
                                 windowTitle());
        return;
    }

    runProducing(m_document, this, windowTitle(), u"-smaller.pdf"_s,
                 [this, pages, names, options, skipped](const QString &out, QStringList *summary, QString *error) {
                     qint64 saved = 0;
                     QApplication::setOverrideCursor(Qt::WaitCursor);
                     const bool ok = ImageEdit::recompress(m_pdf, out, pages, names, options, &saved, error);
                     QApplication::restoreOverrideCursor();
                     if (!ok) {
                         return false;
                     }
                     *summary += saved > 0
                         ? i18n("%1 left the picture streams.", humanSize(saved))
                         : i18n("No picture needed re-encoding, so the result is a copy of the original. A lower "
                                "resolution or quality would give it something to work with.");
                     if (skipped > 0) {
                         *summary += i18np("Left one picture alone: its pixels are stored in a form this cannot open.",
                                           "Left %1 pictures alone: their pixels are stored in a form this cannot "
                                           "open.",
                                           skipped);
                     }
                     if (const QString change = sizeChange(m_pdf, out); !change.isEmpty()) {
                         *summary += change;
                     }
                     return true;
                 });
}

// ── Work on the pixels ────────────────────────────────────────────────────

QWidget *ImagesDialog::buildPixels()
{
    auto *page = new QWidget(this);

    m_pickedPixels = new QLabel(page);
    m_pickedPixels->setWordWrap(true);

    auto *adjustBox = new QGroupBox(i18nc("@title:group", "The values in the pixels"), page);

    m_brightness = new QDoubleSpinBox(adjustBox);
    m_brightness->setRange(-1.0, 1.0);
    m_brightness->setDecimals(2);
    m_brightness->setSingleStep(0.05);

    m_contrast = new QDoubleSpinBox(adjustBox);
    m_contrast->setRange(-1.0, 1.0);
    m_contrast->setDecimals(2);
    m_contrast->setSingleStep(0.05);

    m_gamma = new QDoubleSpinBox(adjustBox);
    m_gamma->setRange(0.05, 5.0);
    m_gamma->setDecimals(2);
    m_gamma->setSingleStep(0.05);
    m_gamma->setValue(1.0);
    m_gamma->setToolTip(i18nc("@info:tooltip", "Above one lifts the midtones, below one lowers them."));

    m_sharpen = new QCheckBox(i18nc("@option:check", "Sharpen"), adjustBox);
    m_despeckle = new QCheckBox(i18nc("@option:check", "Take the specks out of a scan"), adjustBox);
    m_autoLevels = new QCheckBox(i18nc("@option:check", "Spread the tones over the full range"), adjustBox);
    m_toGrey = new QCheckBox(i18nc("@option:check", "Reduce to grey"), adjustBox);
    m_toBlackWhite = new QCheckBox(i18nc("@option:check", "Reduce to black and white"), adjustBox);

    m_threshold = new QSpinBox(adjustBox);
    m_threshold->setRange(0, 255);
    m_threshold->setValue(128);
    m_threshold->setEnabled(false);
    m_threshold->setToolTip(i18nc("@info:tooltip", "Above this level a pixel becomes white, below it black."));

    auto *adjustForm = new QFormLayout;
    adjustForm->addRow(i18nc("@label:spinbox", "Brightness:"), m_brightness);
    adjustForm->addRow(i18nc("@label:spinbox", "Contrast:"), m_contrast);
    adjustForm->addRow(i18nc("@label:spinbox", "Gamma:"), m_gamma);
    adjustForm->addRow(QString(), m_sharpen);
    adjustForm->addRow(QString(), m_despeckle);
    adjustForm->addRow(QString(), m_autoLevels);
    adjustForm->addRow(QString(), m_toGrey);
    adjustForm->addRow(QString(), m_toBlackWhite);
    adjustForm->addRow(i18nc("@label:spinbox", "Black-and-white threshold:"), m_threshold);

    auto *adjustNote = new QLabel(i18n("The picture keeps its place on the page and its number of pixels; only the "
                                       "values change. One shared with another page is copied first, so that page "
                                       "keeps what it had."),
                                  adjustBox);
    adjustNote->setWordWrap(true);

    auto *adjustButton = new QPushButton(QIcon::fromTheme(u"tools-wizard"_s),
                                         i18nc("@action:button", "Work Over the Pixels"), adjustBox);

    auto *adjustLayout = new QVBoxLayout(adjustBox);
    adjustLayout->addLayout(adjustForm);
    adjustLayout->addWidget(adjustNote);
    adjustLayout->addWidget(adjustButton);

    auto *cropBox = new QGroupBox(i18nc("@title:group", "Cut pixels away for good"), page);

    const auto fraction = [cropBox](double value) {
        auto *box = new QDoubleSpinBox(cropBox);
        box->setRange(0.0, 1.0);
        box->setDecimals(3);
        box->setSingleStep(0.05);
        box->setValue(value);
        return box;
    };
    m_keepX = fraction(0.0);
    m_keepY = fraction(0.0);
    m_keepWidth = fraction(1.0);
    m_keepHeight = fraction(1.0);

    auto *cropRow = new QHBoxLayout;
    cropRow->addWidget(new QLabel(i18nc("@label:spinbox distance from the left edge", "From the left:"), cropBox));
    cropRow->addWidget(m_keepX);
    cropRow->addWidget(new QLabel(i18nc("@label:spinbox distance from the top edge", "From the top:"), cropBox));
    cropRow->addWidget(m_keepY);
    cropRow->addWidget(new QLabel(i18nc("@label:spinbox", "Width:"), cropBox));
    cropRow->addWidget(m_keepWidth);
    cropRow->addWidget(new QLabel(i18nc("@label:spinbox", "Height:"), cropBox));
    cropRow->addWidget(m_keepHeight);
    cropRow->addStretch(1);

    auto *cropNote = new QLabel(i18n("The part to keep, as fractions of the stored picture measured from its top-left "
                                     "corner. What is cut away leaves the file, and what is kept stays exactly where "
                                     "it was on the page rather than being stretched over the old area."),
                                cropBox);
    cropNote->setWordWrap(true);

    auto *cropButton = new QPushButton(QIcon::fromTheme(u"transform-crop"_s),
                                       i18nc("@action:button", "Crop the Picture"), cropBox);

    auto *cropLayout = new QVBoxLayout(cropBox);
    cropLayout->addLayout(cropRow);
    cropLayout->addWidget(cropNote);
    cropLayout->addWidget(cropButton);

    auto *layout = new QVBoxLayout(page);
    layout->addWidget(m_pickedPixels);
    layout->addWidget(adjustBox);
    layout->addWidget(cropBox);
    layout->addStretch(1);

    m_needPixels = { adjustButton, cropButton };

    connect(m_toBlackWhite, &QCheckBox::toggled, m_threshold, &QSpinBox::setEnabled);
    connect(adjustButton, &QPushButton::clicked, this, [this] {
        runAdjust();
    });
    connect(cropButton, &QPushButton::clicked, this, [this] {
        runCrop();
    });
    return page;
}

void ImagesDialog::runAdjust()
{
    const ImageEdit::ImageUse *use = picked();
    if (!use) {
        return;
    }
    const int page = use->page;
    const QString name = use->resourceName;

    ImageEdit::Adjust options;
    options.brightness = m_brightness->value();
    options.contrast = m_contrast->value();
    options.gamma = m_gamma->value();
    options.sharpen = m_sharpen->isChecked();
    options.despeckle = m_despeckle->isChecked();
    options.autoLevels = m_autoLevels->isChecked();
    options.toGrayscale = m_toGrey->isChecked();
    options.toBlackWhite = m_toBlackWhite->isChecked();
    options.threshold = m_threshold->value();

    const bool somethingToDo = options.sharpen || options.despeckle || options.autoLevels || options.toGrayscale
        || options.toBlackWhite || !qFuzzyIsNull(options.brightness) || !qFuzzyIsNull(options.contrast)
        || !qFuzzyCompare(options.gamma, 1.0);
    if (!somethingToDo) {
        KMessageBox::information(this, i18n("Nothing is set to change, so the picture would come out as it went in."),
                                 windowTitle());
        return;
    }

    runProducing(m_document, this, windowTitle(), u"-adjusted.pdf"_s,
                 [this, page, name, options](const QString &out, QStringList *summary, QString *error) {
                     QApplication::setOverrideCursor(Qt::WaitCursor);
                     const bool ok = ImageEdit::adjust(m_pdf, out, page, name, options, error);
                     QApplication::restoreOverrideCursor();
                     if (!ok) {
                         return false;
                     }
                     *summary += i18n("Worked over the pixels of %1 on page %2; it keeps its place and its size.", name,
                                      QLocale().toString(page + 1));
                     // Sharpening a scan can leave it larger than it was, so
                     // both numbers are worth saying even here.
                     if (const QString change = sizeChange(m_pdf, out); !change.isEmpty()) {
                         *summary += change;
                     }
                     return true;
                 });
}

void ImagesDialog::runCrop()
{
    const ImageEdit::ImageUse *use = picked();
    if (!use) {
        return;
    }
    const int page = use->page;
    const QString name = use->resourceName;
    const QRectF keep(m_keepX->value(), m_keepY->value(), m_keepWidth->value(), m_keepHeight->value());

    // A hair over one is what a hand-typed 0.333 + 0.334 + 0.333 comes to; a
    // long way over it means someone has read the boxes as pixels.
    if (keep.width() <= 0.0 || keep.height() <= 0.0 || keep.x() + keep.width() > 1.001
        || keep.y() + keep.height() > 1.001) {
        KMessageBox::information(this, i18n("The part to keep has to lie inside the picture, as fractions between 0 "
                                            "and 1."),
                                 windowTitle());
        return;
    }
    if (qFuzzyCompare(keep.width(), 1.0) && qFuzzyCompare(keep.height(), 1.0)) {
        KMessageBox::information(this, i18n("That keeps the whole picture, so there is nothing to cut away."),
                                 windowTitle());
        return;
    }

    runProducing(m_document, this, windowTitle(), u"-cropped.pdf"_s,
                 [this, page, name, keep](const QString &out, QStringList *summary, QString *error) {
                     QApplication::setOverrideCursor(Qt::WaitCursor);
                     const bool ok = ImageEdit::crop(m_pdf, out, page, name, keep, error);
                     QApplication::restoreOverrideCursor();
                     if (!ok) {
                         return false;
                     }
                     *summary += i18n("Cropped %1 on page %2. The part cut away has left the file.", name,
                                      QLocale().toString(page + 1));
                     if (const QString change = sizeChange(m_pdf, out); !change.isEmpty()) {
                         *summary += change;
                     }
                     return true;
                 });
}

// ── Tidying up ────────────────────────────────────────────────────────────

QWidget *ImagesDialog::buildTidy()
{
    auto *page = new QWidget(this);

    auto *dupeBox = new QGroupBox(i18nc("@title:group", "Pictures the file stores twice"), page);
    auto *dupeNote = new QLabel(i18n("A logo pasted onto forty pages is often forty copies of the same bytes. "
                                     "Identical pictures are merged into one object, and every page that drew a copy "
                                     "draws the survivor instead, so the pages come out looking exactly as they did."),
                                dupeBox);
    dupeNote->setWordWrap(true);
    auto *runDupes = new QPushButton(QIcon::fromTheme(u"edit-duplicate"_s),
                                     i18nc("@action:button", "Merge the Duplicates"), dupeBox);

    auto *dupeLayout = new QVBoxLayout(dupeBox);
    dupeLayout->addWidget(dupeNote);
    dupeLayout->addWidget(runDupes);

    auto *limitsBox = new QGroupBox(i18nc("@title:group", "What working on pictures cannot promise"), page);
    QStringList limits;
    for (const QString &limit : ImageEdit::limitations()) {
        limits += u"• "_s + limit;
    }
    auto *limitsLabel = new QLabel(limits.join(u'\n'), limitsBox);
    limitsLabel->setWordWrap(true);
    limitsLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);

    auto *limitsLayout = new QVBoxLayout(limitsBox);
    limitsLayout->addWidget(limitsLabel);

    auto *layout = new QVBoxLayout(page);
    layout->addWidget(dupeBox);
    layout->addWidget(limitsBox);
    layout->addStretch(1);

    connect(runDupes, &QPushButton::clicked, this, [this] {
        runDeduplicate();
    });
    return page;
}

void ImagesDialog::runDeduplicate()
{
    runProducing(m_document, this, windowTitle(), u"-deduped.pdf"_s,
                 [this](const QString &out, QStringList *summary, QString *error) {
                     int merged = 0;
                     qint64 saved = 0;
                     QApplication::setOverrideCursor(Qt::WaitCursor);
                     const bool ok = ImageEdit::deduplicate(m_pdf, out, &merged, &saved, error);
                     QApplication::restoreOverrideCursor();
                     if (!ok) {
                         return false;
                     }
                     if (merged == 0) {
                         *summary += i18n("No picture is stored more than once, so the result is a copy of the "
                                          "original.");
                         return true;
                     }
                     *summary += i18np("Merged one duplicate into the picture it repeated, freeing %2.",
                                       "Merged %1 duplicates into the pictures they repeated, freeing %2.", merged,
                                       humanSize(saved));
                     if (const QString change = sizeChange(m_pdf, out); !change.isEmpty()) {
                         *summary += change;
                     }
                     return true;
                 });
}

} // namespace

void showImages(Document *document, QWidget *parent)
{
    const QString pdf = savedPath(document, parent);
    if (pdf.isEmpty()) {
        return;
    }
    ImagesDialog(document, pdf, parent).exec();
}

} // namespace ps::tools
