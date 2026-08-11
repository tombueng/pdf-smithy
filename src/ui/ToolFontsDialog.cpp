/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "ToolDialogs.h"

#include "ToolSupport.h"
#include "core/Document.h"
#include "core/FontEmbedder.h"
#include "core/FontInventory.h"
#include "core/PageRange.h"

#include <KLocalizedString>
#include <KMessageBox>

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>

using namespace Qt::Literals::StringLiterals;

namespace ps::tools {

namespace {

/** "/TrueType" and "/WinAnsiEncoding" read better in a column without the slash. */
QString bare(const QString &name)
{
    return name.startsWith(u'/') ? name.mid(1) : name;
}

QString tick(bool yes)
{
    return yes ? u"✓"_s : u"✗"_s;
}

QString sizeOf(qint64 bytes)
{
    return bytes > 0 ? QLocale().formattedDataSize(bytes) : i18nc("@item no font file, so no size to give", "none");
}

/** How a line of the report names the font it is about. */
QString describe(const QString &baseFont, const QStringList &resourceNames, const QVector<int> &pages)
{
    const QString name = baseFont.isEmpty() ? i18nc("@item a font with no /BaseFont", "unnamed") : baseFont;
    if (pages.isEmpty()) {
        return i18nc("@item a font and the names the pages call it by", "%1 (%2)", name, resourceNames.join(u", "_s));
    }
    return i18ncp("@item a font, the names a page calls it by, and where it is used", "%2 (%3, page %4)",
                  "%2 (%3, pages %4)", pages.size(), name, resourceNames.join(u", "_s), PageRange::format(pages));
}

QString describe(const FontUse &use)
{
    return describe(use.baseFont, { use.resourceName }, use.pages);
}

/**
 * What became of every font a substitution looked at, changed or not.
 *
 * The ones it could not touch matter more than the ones it could: a document
 * that gained a face for three of its four missing fonts still cannot be sent
 * anywhere, and the fourth is the only line in the report worth reading.
 */
QStringList describe(const QVector<FontEmbedder::Substitution> &substitutions)
{
    QStringList lines;
    for (const FontEmbedder::Substitution &one : substitutions) {
        const QString what = describe(one.baseFont, one.resourceNames, one.pages);
        if (one.family.isEmpty()) {
            lines += i18nc("@item a font that was left as it was", "%1: left alone.", what);
        } else {
            lines += i18nc("@item a font, the face put in its place, and what that cost",
                           "%1 → %2: %3 character codes kept, %4 glyphs, %5.", what, one.family, one.codes,
                           one.glyphCount, sizeOf(one.embeddedBytes));
            if (one.widthGap > 0) {
                lines += u"    "_s
                    + i18nc("@item how far a substitute's own widths differ from the ones the page states",
                            "The widths differ by up to %1/1000 em. Nothing moves, because the document's own widths "
                            "are kept, but the letters do not quite fill the room reserved for them.",
                            one.widthGap);
            }
        }
        for (const QString &warning : one.warnings) {
            lines += u"    "_s + warning;
        }
    }
    return lines;
}

/** A cell that sorts by the number it carries rather than by the text it shows. */
class NumericItem : public QTableWidgetItem
{
public:
    NumericItem(const QString &text, qint64 value)
        : QTableWidgetItem(text)
        , m_value(value)
    {
        setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    }

    bool operator<(const QTableWidgetItem &other) const override
    {
        const auto *theirs = dynamic_cast<const NumericItem *>(&other);
        return theirs ? m_value < theirs->m_value : QTableWidgetItem::operator<(other);
    }

private:
    qint64 m_value = 0;
};

/**
 * A box to name a font family in.
 *
 * Editable rather than a fixed list, because what looks the font up is
 * Fontconfig and what fills the list is Qt: an alias Fontconfig resolves
 * ("sans-serif", or a family installed since this program started) is a
 * perfectly good answer that the list does not contain.
 */
QComboBox *familyBox(QWidget *parent, const QString &placeholder)
{
    auto *box = new QComboBox(parent);
    box->setEditable(true);
    box->addItem(QString());
    box->addItems(QFontDatabase::families());
    box->setCurrentIndex(0);
    box->lineEdit()->setPlaceholderText(placeholder);
    return box;
}

/**
 * Everything the fonts in a document can be asked or told, in one window.
 *
 * The questions people actually have about a PDF are nearly all font questions
 * in disguise (why is this eleven megabytes, why can nobody copy the text, why
 * does the printer say Helvetica is missing, why does the same typeface appear
 * four times), so the answers belong in one place rather than scattered across
 * four menus. The first tab is the reading; the rest are the repairs that
 * follow from it, each writing a *new* file and saying what it actually did to
 * it rather than what it was asked to do.
 */
class FontsDialog : public QDialog
{
public:
    FontsDialog(Document *document, const QString &pdf, QWidget *parent);

private:
    QWidget *buildOverview();
    QWidget *buildRepairs();
    QWidget *buildSize();
    QWidget *buildReplace();
    QWidget *buildAdd();

    void inspect();
    void showCharacters();
    void showLimitations();
    void locateChosen();

    void runEmbedMissing();
    void runToUnicode();
    void runMerge();
    void runSubset();
    void runStrip();
    void runReplace();
    void runAdd();

    /** The document's own fonts by /BaseFont, one entry each, for the choosers. */
    QStringList documentFonts(bool embeddedOnly) const;

    /** The font the "Add a Font" tab describes. */
    FontEmbedder::Request requested() const;

    /** Zero-based pages from the range box, or empty for all of them. */
    QVector<int> chosenPages();

    /** "1.2 MiB → 840 KiB", for the tools whose whole point is the second number. */
    QString sizeChange(const QString &output) const;

    Document *m_document = nullptr;
    QString m_pdf;
    QVector<FontUse> m_uses;

    QTableWidget *m_table = nullptr;
    QPlainTextEdit *m_notes = nullptr;
    QPushButton *m_characters = nullptr;

    QComboBox *m_substitute = nullptr;

    QListWidget *m_embedded = nullptr;
    QCheckBox *m_everyFont = nullptr;

    QComboBox *m_replaceOld = nullptr;
    QComboBox *m_replaceNew = nullptr;

    QComboBox *m_addFamily = nullptr;
    QCheckBox *m_bold = nullptr;
    QCheckBox *m_italic = nullptr;
    QLineEdit *m_text = nullptr;
    QLineEdit *m_pages = nullptr;
    QLabel *m_found = nullptr;
    QLabel *m_wanted = nullptr;

    /** Whether the font on this system has been looked for yet; see the constructor. */
    bool m_located = false;
};

FontsDialog::FontsDialog(Document *document, const QString &pdf, QWidget *parent)
    : QDialog(parent)
    , m_document(document)
    , m_pdf(pdf)
{
    setWindowTitle(i18nc("@title:window", "Fonts"));
    resize(1060, 800);

    auto *tabs = new QTabWidget(this);
    QWidget *add = buildAdd();
    tabs->addTab(buildOverview(), i18nc("@title:tab", "What Is in Here"));
    tabs->addTab(buildRepairs(), i18nc("@title:tab", "Repairs"));
    tabs->addTab(buildSize(), i18nc("@title:tab", "Size"));
    tabs->addTab(buildReplace(), i18nc("@title:tab", "Replace a Font"));
    tabs->addTab(add, i18nc("@title:tab", "Add a Font"));

    // Looking a font up on this system runs fc-match and, where that finds
    // nothing, reads the name table of every font file installed. That waits
    // until somebody goes to the tab that wants the answer, rather than being
    // paid for on every opening of this window.
    connect(tabs, &QTabWidget::currentChanged, this, [this, tabs, add](int index) {
        if (tabs->widget(index) == add && !m_located) {
            locateChosen();
        }
    });

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(tabs, 1);
    layout->addWidget(buttons);

    inspect();
}

// ── What is in here ───────────────────────────────────────────────────────

QWidget *FontsDialog::buildOverview()
{
    auto *page = new QWidget(this);

    m_table = new QTableWidget(0, 10, page);
    m_table->setHorizontalHeaderLabels(
        { i18nc("@title:column the /Fx name a page calls a font by", "Name"), i18nc("@title:column", "Font"),
          i18nc("@title:column what kind of font", "Type"), i18nc("@title:column short for embedded", "Emb"),
          i18nc("@title:column short for subset", "Sub"),
          i18nc("@title:column short for “has a /ToUnicode map”", "Uni"), i18nc("@title:column", "Encoding"),
          i18nc("@title:column how many characters can be addressed", "Chars"),
          i18nc("@title:column size of the embedded font programme", "Size"), i18nc("@title:column", "Pages") });
    m_table->horizontalHeaderItem(3)->setToolTip(i18nc("@info:tooltip", "The glyphs travel with the document."));
    m_table->horizontalHeaderItem(4)->setToolTip(i18nc("@info:tooltip", "Only part of the font is here."));
    m_table->horizontalHeaderItem(5)->setToolTip(
        i18nc("@info:tooltip", "Its text can be copied out, searched and read aloud."));
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_table->verticalHeader()->hide();
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSortingEnabled(true);

    m_notes = new QPlainTextEdit(page);
    m_notes->setReadOnly(true);

    auto *again = new QPushButton(QIcon::fromTheme(u"view-refresh"_s), i18nc("@action:button", "Read Again"), page);
    m_characters
        = new QPushButton(QIcon::fromTheme(u"character-set"_s), i18nc("@action:button", "Which Characters"), page);
    m_characters->setToolTip(i18nc("@info:tooltip",
                                   "What the selected font lets this document address. Text edited in that font can "
                                   "use these characters and no others, unless a font is added."));
    m_characters->setEnabled(false);
    auto *limits = new QPushButton(QIcon::fromTheme(u"dialog-information"_s),
                                   i18nc("@action:button", "What This Cannot Promise"), page);

    auto *row = new QHBoxLayout;
    row->addWidget(again);
    row->addWidget(m_characters);
    row->addWidget(limits);
    row->addStretch(1);

    auto *legend = new QLabel(i18nc("@info legend for the three tick columns",
                                    "Emb = the glyphs travel with the document · Sub = only part of the font is "
                                    "here · Uni = its text can be copied"),
                              page);
    legend->setWordWrap(true);

    auto *layout = new QVBoxLayout(page);
    layout->addWidget(m_table, 3);
    layout->addWidget(legend);
    layout->addWidget(m_notes, 2);
    layout->addLayout(row);

    connect(again, &QPushButton::clicked, this, [this] { inspect(); });
    connect(m_characters, &QPushButton::clicked, this, [this] { showCharacters(); });
    connect(limits, &QPushButton::clicked, this, [this] { showLimitations(); });
    connect(m_table, &QTableWidget::itemSelectionChanged, this,
            [this] { m_characters->setEnabled(m_table->currentRow() >= 0); });
    return page;
}

void FontsDialog::inspect()
{
    QApplication::setOverrideCursor(Qt::WaitCursor);
    QString error;
    m_uses = FontInventory::read(m_pdf, &error);
    QApplication::restoreOverrideCursor();

    if (!error.isEmpty()) {
        m_table->setRowCount(0);
        m_notes->setPlainText(error);
        return;
    }

    // Filling a sorted table row by row would move the rows about underneath the
    // filling, which is how one font ends up with another font's page list.
    m_table->setSortingEnabled(false);
    m_table->setRowCount(m_uses.size());
    for (int row = 0; row < m_uses.size(); ++row) {
        const FontUse &use = m_uses.at(row);
        const auto centred = [](QTableWidgetItem *item) {
            item->setTextAlignment(Qt::AlignCenter);
            return item;
        };

        m_table->setItem(row, 0, new QTableWidgetItem(use.resourceName));
        auto *name = new QTableWidgetItem(use.baseFont);
        if (!use.licence.isEmpty()) {
            name->setToolTip(i18nc("@info:tooltip what a font's own licence bits say", "Licence: %1", use.licence));
        }
        m_table->setItem(row, 1, name);
        m_table->setItem(row, 2, new QTableWidgetItem(bare(use.subtype)));
        m_table->setItem(row, 3, centred(new QTableWidgetItem(tick(use.embedded))));
        m_table->setItem(row, 4, centred(new QTableWidgetItem(tick(use.subset))));
        m_table->setItem(row, 5, centred(new QTableWidgetItem(tick(use.hasToUnicode))));
        m_table->setItem(row, 6,
                         new QTableWidgetItem(use.encoding.isEmpty()
                                                  ? i18nc("@item the font names no encoding of its own", "none")
                                                  : bare(use.encoding)));
        m_table->setItem(row, 7, new NumericItem(QLocale().toString(use.coverage.size()), use.coverage.size()));
        m_table->setItem(row, 8, new NumericItem(sizeOf(use.fileSize), use.fileSize));
        m_table->setItem(row, 9, new QTableWidgetItem(PageRange::format(use.pages)));
    }
    m_table->setSortingEnabled(true);
    m_table->resizeColumnsToContents();
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);

    QStringList lines;
    if (m_uses.isEmpty()) {
        lines += i18n("No page in this document uses a font. A scan is all picture, and has none until its text has "
                      "been recognised.");
    } else {
        int embedded = 0;
        int noUnicode = 0;
        qint64 total = 0;
        for (const FontUse &use : std::as_const(m_uses)) {
            embedded += use.embedded ? 1 : 0;
            noUnicode += use.hasToUnicode ? 0 : 1;
            total += use.fileSize;
        }

        lines += i18np("One font.", "%1 fonts.", m_uses.size());
        if (embedded < m_uses.size()) {
            lines += i18np("One of them is not embedded, so it depends on the reader having that face.",
                           "%1 of them are not embedded, so they depend on the reader having those faces.",
                           m_uses.size() - embedded);
        }
        if (noUnicode > 0) {
            lines += i18np("One has no /ToUnicode map, so its text cannot be copied out or searched.",
                           "%1 have no /ToUnicode map, so their text cannot be copied out or searched.", noUnicode);
        }
        if (total > 0) {
            lines += i18n("The embedded font programmes come to %1 of a %2 file.", sizeOf(total),
                          sizeOf(QFileInfo(m_pdf).size()));
        }

        QStringList problems;
        for (const FontUse &use : std::as_const(m_uses)) {
            if (!use.embedded) {
                problems += i18nc("@info",
                                  "%1 is not embedded, so the reader will substitute another face and the "
                                  "lines will shift.",
                                  describe(use));
            }
            if (!use.hasToUnicode) {
                problems += i18nc("@info",
                                  "%1 has no /ToUnicode map, so its text cannot be copied, searched or read "
                                  "aloud.",
                                  describe(use));
            }
            if (use.coverage.isEmpty()) {
                problems += i18nc("@info",
                                  "%1 says nothing anywhere about what its character codes mean, so its "
                                  "text cannot be edited.",
                                  describe(use));
            }
        }
        lines += QString();
        if (problems.isEmpty()) {
            lines += i18n("Nothing wrong with the fonts in this document.");
        } else {
            lines += i18np("One thing to put right:", "%1 things to put right:", problems.size());
            for (const QString &problem : std::as_const(problems)) {
                lines += u"  • "_s + problem;
            }
        }

        QStringList licences;
        for (const FontUse &use : std::as_const(m_uses)) {
            if (!use.licence.isEmpty()) {
                licences += i18nc("@item a font and what its licence bits say", "%1: %2", use.family, use.licence);
            }
        }
        licences.removeDuplicates();
        if (!licences.isEmpty()) {
            lines += QString();
            lines += i18nc("@title heading before a list", "Licences, as the fonts themselves state them:");
            for (const QString &licence : std::as_const(licences)) {
                lines += u"  • "_s + licence;
            }
        }
    }
    m_notes->setPlainText(lines.join(u'\n'));

    // The chooser and the strip list follow what the document actually carries,
    // so they are filled from the reading rather than guessed at when the tabs
    // are built.
    const QStringList all = documentFonts(false);
    const QString wasChosen = m_replaceOld->currentText();
    m_replaceOld->clear();
    m_replaceOld->addItems(all);
    m_replaceOld->setCurrentText(all.contains(wasChosen) ? wasChosen : QString());

    m_embedded->clear();
    const QStringList carried = documentFonts(true);
    for (const QString &font : carried) {
        auto *item = new QListWidgetItem(font, m_embedded);
        item->setFlags((item->flags() | Qt::ItemIsUserCheckable) & ~Qt::ItemIsEditable);
        item->setCheckState(Qt::Unchecked);
    }
    m_embedded->setEnabled(!m_everyFont->isChecked());
}

void FontsDialog::showCharacters()
{
    const int row = m_table->currentRow();
    if (row < 0 || !m_table->item(row, 0)) {
        return;
    }

    // The table sorts, so a row number is not the reading's index. The /Fx name
    // and the /BaseFont together are what find the entry again.
    const QString resourceName = m_table->item(row, 0)->text();
    const QString baseFont = m_table->item(row, 1)->text();
    const auto found = std::find_if(m_uses.cbegin(), m_uses.cend(), [&](const FontUse &use) {
        return use.resourceName == resourceName && use.baseFont == baseFont;
    });
    if (found == m_uses.cend()) {
        return;
    }

    QVector<QChar> characters(found->coverage.cbegin(), found->coverage.cend());
    std::sort(characters.begin(), characters.end(),
              [](QChar left, QChar right) { return left.unicode() < right.unicode(); });

    QStringList lines { describe(*found), QString() };
    if (characters.isEmpty()) {
        lines += i18n("This font says nothing about what its character codes mean, so nothing set in it can be "
                      "edited. Adding a font is the way round that.");
    } else {
        lines += i18np("One character can be addressed:", "%1 characters can be addressed:", characters.size());
        lines += QString();

        QString run;
        for (const QChar &character : std::as_const(characters)) {
            // A space or a control character in a grid of glyphs reads as a
            // missing entry rather than as itself.
            run += character.isPrint() && !character.isSpace() ? character : u'·';
            if (run.size() == 48) {
                lines += run;
                run.clear();
            }
        }
        if (!run.isEmpty()) {
            lines += run;
        }

        lines += QString();
        lines += i18n("Read from the /ToUnicode map where there is one and from the encoding otherwise, so the font "
                      "this came from may well hold more. What decides whether text can be changed is what the "
                      "document can address, which is this.");
    }
    showReport(this, i18nc("@title:window", "Characters"), lines, true);
}

void FontsDialog::showLimitations()
{
    QStringList lines { i18nc("@title heading before a list", "Reading and repairing what a document carries:") };
    const QStringList inventory = FontInventory::limitations();
    for (const QString &limit : inventory) {
        lines += u"  • "_s + limit;
    }

    lines += QString();
    lines += i18nc("@title heading before a list", "Embedding a font from this system:");
    const QStringList embedder = FontEmbedder::limitations();
    for (const QString &limit : embedder) {
        lines += u"  • "_s + limit;
    }
    showReport(this, i18nc("@title:window", "What the Font Tools Cannot Promise"), lines);
}

QStringList FontsDialog::documentFonts(bool embeddedOnly) const
{
    QStringList names;
    for (const FontUse &use : m_uses) {
        if (embeddedOnly && !use.embedded) {
            continue;
        }
        if (!use.baseFont.isEmpty() && !names.contains(use.baseFont)) {
            names += use.baseFont;
        }
    }
    return names;
}

QString FontsDialog::sizeChange(const QString &output) const
{
    return i18nc("@info the file's size before and after", "%1 → %2", sizeOf(QFileInfo(m_pdf).size()),
                 sizeOf(QFileInfo(output).size()));
}

// ── Repairs ───────────────────────────────────────────────────────────────

QWidget *FontsDialog::buildRepairs()
{
    auto *page = new QWidget(this);

    auto *missingBox = new QGroupBox(i18nc("@title:group", "Fonts the document is missing"), page);
    m_substitute = familyBox(missingBox, i18n("The closest match on this system"));
    auto *missingForm = new QFormLayout;
    missingForm->addRow(i18nc("@label:listbox", "Put in their place:"), m_substitute);
    auto *missingNote = new QLabel(i18n("The font object the pages already point at is the one that gains the "
                                        "glyphs, so no page is rewritten: every character code keeps the meaning it "
                                        "had and every width the document states is kept, which is what stops the "
                                        "text reflowing. Left empty, a metrically compatible face is chosen from "
                                        "what the document asks for: Liberation Sans for Helvetica, and so on."),
                                   missingBox);
    missingNote->setWordWrap(true);
    auto *embedMissing = new QPushButton(QIcon::fromTheme(u"font-enable"_s),
                                         i18nc("@action:button", "Embed the Missing Faces"), missingBox);
    auto *missingLayout = new QVBoxLayout(missingBox);
    missingLayout->addLayout(missingForm);
    missingLayout->addWidget(missingNote);
    missingLayout->addWidget(embedMissing);

    auto *unicodeBox = new QGroupBox(i18nc("@title:group", "Text that cannot be copied"), page);
    auto *unicodeNote = new QLabel(i18n("A /ToUnicode map is worked out from the font's own encoding (a "
                                        "/Differences array saying /eacute means the code beside it draws an é) "
                                        "and written back into the file, so that every other program can read the "
                                        "text out too."),
                                   unicodeBox);
    unicodeNote->setWordWrap(true);
    auto *unicode = new QPushButton(QIcon::fromTheme(u"edit-select-text"_s),
                                    i18nc("@action:button", "Make the Text Copyable"), unicodeBox);
    auto *unicodeLayout = new QVBoxLayout(unicodeBox);
    unicodeLayout->addWidget(unicodeNote);
    unicodeLayout->addWidget(unicode);

    auto *mergeBox = new QGroupBox(i18nc("@title:group", "The same font several times over"), page);
    auto *mergeNote = new QLabel(i18n("What produces duplicates is merging documents: three chapters, each carrying "
                                      "its own subset of the same typeface, become one file carrying three. Subsets "
                                      "that are the same programme with the same encoding become one object every "
                                      "page points at; ones that genuinely hold different glyphs are left alone."),
                                 mergeBox);
    mergeNote->setWordWrap(true);
    auto *merge
        = new QPushButton(QIcon::fromTheme(u"merge"_s), i18nc("@action:button", "Join the Duplicates"), mergeBox);
    auto *mergeLayout = new QVBoxLayout(mergeBox);
    mergeLayout->addWidget(mergeNote);
    mergeLayout->addWidget(merge);

    auto *layout = new QVBoxLayout(page);
    layout->addWidget(missingBox);
    layout->addWidget(unicodeBox);
    layout->addWidget(mergeBox);
    layout->addStretch(1);

    connect(embedMissing, &QPushButton::clicked, this, [this] { runEmbedMissing(); });
    connect(unicode, &QPushButton::clicked, this, [this] { runToUnicode(); });
    connect(merge, &QPushButton::clicked, this, [this] { runMerge(); });
    return page;
}

void FontsDialog::runEmbedMissing()
{
    const bool anyMissing
        = std::any_of(m_uses.cbegin(), m_uses.cend(), [](const FontUse &use) { return !use.embedded; });
    if (!m_uses.isEmpty() && !anyMissing) {
        KMessageBox::information(this,
                                 i18n("Every font this document uses already travels with it, so there is "
                                      "nothing to embed."),
                                 windowTitle());
        return;
    }

    const QString family = m_substitute->currentText().trimmed();
    runProducing(m_document, this, windowTitle(), u"-embedded.pdf"_s,
                 [this, family](const QString &out, QStringList *summary, QString *error) {
                     QVector<FontEmbedder::Substitution> substitutions;
                     QApplication::setOverrideCursor(Qt::WaitCursor);
                     const bool ok = FontEmbedder::embedMissing(m_pdf, out, family, &substitutions, error);
                     QApplication::restoreOverrideCursor();
                     if (!ok) {
                         // Every font it could not deal with carries its own
                         // reason. Without them the message says only that
                         // nothing happened, which is the half that does not
                         // tell anyone what to do next.
                         const QStringList reasons = describe(substitutions);
                         if (!reasons.isEmpty()) {
                             *error += u"\n\n"_s + reasons.join(u'\n');
                         }
                         return false;
                     }
                     *summary += describe(substitutions).join(u'\n');
                     *summary += i18n("The pages were not rewritten and did not need to be: the font objects they "
                                      "already point at are the ones that gained the glyphs.");
                     *summary += sizeChange(out);
                     return true;
                 });
}

void FontsDialog::runToUnicode()
{
    const bool anyWithout
        = std::any_of(m_uses.cbegin(), m_uses.cend(), [](const FontUse &use) { return !use.hasToUnicode; });
    if (!m_uses.isEmpty() && !anyWithout) {
        KMessageBox::information(this, i18n("Every font in this document already says what its characters mean."),
                                 windowTitle());
        return;
    }

    runProducing(m_document, this, windowTitle(), u"-copyable.pdf"_s,
                 [this](const QString &out, QStringList *summary, QString *error) {
                     int added = 0;
                     QApplication::setOverrideCursor(Qt::WaitCursor);
                     const bool ok = FontInventory::addToUnicode(m_pdf, out, &added, error);
                     QApplication::restoreOverrideCursor();
                     if (!ok) {
                         return false;
                     }
                     *summary += i18np("Wrote a /ToUnicode map for one font.", "Wrote a /ToUnicode map for %1 fonts.",
                                       added);
                     *summary += i18n("Their text can be copied and searched now, by this program and by any other.");
                     return true;
                 });
}

void FontsDialog::runMerge()
{
    runProducing(m_document, this, windowTitle(), u"-fonts-merged.pdf"_s,
                 [this](const QString &out, QStringList *summary, QString *error) {
                     int merged = 0;
                     QApplication::setOverrideCursor(Qt::WaitCursor);
                     const bool ok = FontInventory::mergeSubsets(m_pdf, out, &merged, error);
                     QApplication::restoreOverrideCursor();
                     if (!ok) {
                         return false;
                     }
                     *summary += i18np("Reduced one duplicate font to the one the pages now share.",
                                       "Reduced %1 duplicate fonts to the ones the pages now share.", merged);
                     *summary += sizeChange(out);
                     return true;
                 });
}

// ── Size ──────────────────────────────────────────────────────────────────

QWidget *FontsDialog::buildSize()
{
    auto *page = new QWidget(this);

    auto *subsetBox = new QGroupBox(i18nc("@title:group", "Whole fonts where a few letters would do"), page);
    auto *subsetNote = new QLabel(i18n("The commonest reason a two-page letter weighs a megabyte. What the pages "
                                       "actually draw is measured from the content streams (every page, every "
                                       "form, every annotation) and the rest of each embedded font is left behind. "
                                       "Nothing on any page changes: the encoding, the widths and the character "
                                       "codes are untouched, and only the glyph programme is rewritten. A font whose "
                                       "codes cannot be resolved to glyphs is left exactly as it was and named in "
                                       "the report."),
                                  subsetBox);
    subsetNote->setWordWrap(true);
    auto *subset = new QPushButton(QIcon::fromTheme(u"edit-cut"_s),
                                   i18nc("@action:button", "Cut the Fonts Down to What Is Used"), subsetBox);
    auto *subsetLayout = new QVBoxLayout(subsetBox);
    subsetLayout->addWidget(subsetNote);
    subsetLayout->addWidget(subset);

    auto *stripBox = new QGroupBox(i18nc("@title:group", "Fonts every machine has anyway"), page);
    m_embedded = new QListWidget(stripBox);
    m_everyFont = new QCheckBox(i18nc("@option:check", "Every embedded font"), stripBox);
    auto *stripNote = new QLabel(i18n("The glyphs go and the reference stays. The text is still there and still "
                                      "selectable; what goes is the guarantee that it will look the same elsewhere, "
                                      "so this is a trade rather than an improvement."),
                                 stripBox);
    stripNote->setWordWrap(true);
    auto *strip = new QPushButton(QIcon::fromTheme(u"edit-delete"_s), i18nc("@action:button", "Throw the Glyphs Away"),
                                  stripBox);
    auto *stripLayout = new QVBoxLayout(stripBox);
    stripLayout->addWidget(m_embedded, 1);
    stripLayout->addWidget(m_everyFont);
    stripLayout->addWidget(stripNote);
    stripLayout->addWidget(strip);

    auto *layout = new QVBoxLayout(page);
    layout->addWidget(subsetBox);
    layout->addWidget(stripBox, 1);

    connect(subset, &QPushButton::clicked, this, [this] { runSubset(); });
    connect(strip, &QPushButton::clicked, this, [this] { runStrip(); });
    connect(m_everyFont, &QCheckBox::toggled, this, [this](bool every) { m_embedded->setEnabled(!every); });
    return page;
}

void FontsDialog::runSubset()
{
    runProducing(m_document, this, windowTitle(), u"-fonts-subset.pdf"_s,
                 [this](const QString &out, QStringList *summary, QString *error) {
                     QVector<SubsetReduction> reduced;
                     QStringList notes;
                     QApplication::setOverrideCursor(Qt::WaitCursor);
                     const bool ok = FontInventory::subsetEmbedded(m_pdf, out, &reduced, &notes, error);
                     QApplication::restoreOverrideCursor();
                     if (!ok) {
                         return false;
                     }

                     QStringList lines;
                     for (const SubsetReduction &one : std::as_const(reduced)) {
                         lines += i18nc("@item a font, its glyph count before and after, and its size before and "
                                        "after",
                                        "%1: %2 glyphs down to %3, %4 down to %5.",
                                        describe(one.baseFont, one.resourceNames, {}), one.glyphsBefore,
                                        one.glyphsAfter, sizeOf(one.bytesBefore), sizeOf(one.bytesAfter));
                     }
                     *summary += lines.isEmpty() ? i18n("No embedded font could be cut down any further.")
                                                 : lines.join(u'\n');
                     if (!notes.isEmpty()) {
                         *summary += i18n("Left exactly as they were:") + u"\n  • "_s + notes.join(u"\n  • "_s);
                     }
                     *summary += sizeChange(out);
                     return true;
                 });
}

void FontsDialog::runStrip()
{
    QStringList chosen;
    if (!m_everyFont->isChecked()) {
        for (int row = 0; row < m_embedded->count(); ++row) {
            if (m_embedded->item(row)->checkState() == Qt::Checked) {
                chosen += m_embedded->item(row)->text();
            }
        }
        if (chosen.isEmpty()) {
            KMessageBox::information(this,
                                     i18n("Tick the fonts whose glyphs are to go, or ask for every embedded "
                                          "one."),
                                     windowTitle());
            return;
        }
    }

    runProducing(m_document, this, windowTitle(), u"-unembedded.pdf"_s,
                 [this, chosen](const QString &out, QStringList *summary, QString *error) {
                     int removed = 0;
                     QApplication::setOverrideCursor(Qt::WaitCursor);
                     const bool ok = FontInventory::removeEmbedding(m_pdf, out, chosen, &removed, error);
                     QApplication::restoreOverrideCursor();
                     if (!ok) {
                         return false;
                     }
                     *summary
                         += i18np("Threw the glyphs of one font away.", "Threw the glyphs of %1 fonts away.", removed);
                     *summary += sizeChange(out);
                     *summary += i18n("The text is still there and still selectable. What it looks like is now up to "
                                      "whatever face the reader puts in its place.");
                     return true;
                 });
}

// ── Replace a font ────────────────────────────────────────────────────────

QWidget *FontsDialog::buildReplace()
{
    auto *page = new QWidget(this);

    m_replaceOld = new QComboBox(page);
    m_replaceOld->setEditable(true);
    m_replaceOld->lineEdit()->setPlaceholderText(i18n("A font this document uses"));

    m_replaceNew = familyBox(page, i18n("A family installed on this system"));

    auto *form = new QFormLayout;
    form->addRow(i18nc("@label:listbox", "Replace:"), m_replaceOld);
    form->addRow(i18nc("@label:listbox", "With:"), m_replaceNew);

    auto *note = new QLabel(i18n("The font object the pages point at is given the new face, so the codes on the page "
                                 "keep their meaning and the widths the document states are kept, so nothing moves. "
                                 "What the new face cannot draw is reported rather than left as a blank somebody "
                                 "finds later. A composite font, the kind used for Chinese and by most modern "
                                 "producers, cannot be replaced this way and is named instead of guessed at."),
                            page);
    note->setWordWrap(true);

    auto *run = new QPushButton(QIcon::fromTheme(u"edit-find-replace"_s), i18nc("@action:button", "Replace"), page);

    auto *layout = new QVBoxLayout(page);
    layout->addLayout(form);
    layout->addWidget(note);
    layout->addStretch(1);
    layout->addWidget(run);

    connect(run, &QPushButton::clicked, this, [this] { runReplace(); });
    return page;
}

void FontsDialog::runReplace()
{
    const QString oldFont = m_replaceOld->currentText().trimmed();
    const QString newFamily = m_replaceNew->currentText().trimmed();
    if (oldFont.isEmpty() || newFamily.isEmpty()) {
        KMessageBox::information(this,
                                 i18n("Replacing a font needs both the font to replace and the family to put "
                                      "in its place."),
                                 windowTitle());
        return;
    }

    runProducing(m_document, this, windowTitle(), u"-font-replaced.pdf"_s,
                 [this, oldFont, newFamily](const QString &out, QStringList *summary, QString *error) {
                     QVector<FontEmbedder::Substitution> substitutions;
                     QApplication::setOverrideCursor(Qt::WaitCursor);
                     const bool ok = FontEmbedder::replaceFont(m_pdf, out, oldFont, newFamily, &substitutions, error);
                     QApplication::restoreOverrideCursor();
                     if (!ok) {
                         const QStringList reasons = describe(substitutions);
                         if (!reasons.isEmpty()) {
                             *error += u"\n\n"_s + reasons.join(u'\n');
                         }
                         return false;
                     }
                     *summary += describe(substitutions).join(u'\n');
                     *summary += sizeChange(out);
                     return true;
                 });
}

// ── Add a font ────────────────────────────────────────────────────────────

QWidget *FontsDialog::buildAdd()
{
    auto *page = new QWidget(this);

    m_addFamily = familyBox(page, i18n("A family installed on this system"));
    m_addFamily->setCurrentText(u"DejaVu Sans"_s);

    m_bold = new QCheckBox(i18nc("@option:check a cut of a font family", "Bold"), page);
    m_italic = new QCheckBox(i18nc("@option:check a cut of a font family", "Italic"), page);
    auto *cut = new QHBoxLayout;
    cut->addWidget(m_bold);
    cut->addWidget(m_italic);
    cut->addStretch(1);

    m_text = new QLineEdit(page);
    m_text->setPlaceholderText(i18n("The characters to put in, e.g. Müller"));

    m_pages = new QLineEdit(page);
    m_pages->setPlaceholderText(i18n("All pages. Or: 1-4, 8, 12-"));

    auto *form = new QFormLayout;
    form->addRow(i18nc("@label:listbox", "Family:"), m_addFamily);
    form->addRow(i18nc("@label which cut of a font family", "Cut:"), cut);
    form->addRow(i18nc("@label:textbox", "Characters:"), m_text);
    form->addRow(i18nc("@label:textbox", "Pages:"), m_pages);

    m_found = new QLabel(i18n("Choose a family to see where this system keeps it."), page);
    m_found->setWordWrap(true);
    m_found->setTextInteractionFlags(Qt::TextSelectableByMouse);

    m_wanted = new QLabel(page);
    m_wanted->setWordWrap(true);

    auto *note = new QLabel(i18n("This is what lifts the one real limit on editing text: a document that never "
                                 "contained an umlaut cannot be made to say “Müller” with the font it carries. Only "
                                 "the characters asked for go in, cut out of the font file itself, which is what "
                                 "keeps a nine-character correction from adding three quarters of a megabyte. The "
                                 "text already on those pages goes on using the fonts it always did: this adds a "
                                 "resource to write with, not a replacement for what is there."),
                            page);
    note->setWordWrap(true);

    auto *run = new QPushButton(QIcon::fromTheme(u"list-add-font"_s), i18nc("@action:button", "Add the Font"), page);

    auto *layout = new QVBoxLayout(page);
    layout->addLayout(form);
    layout->addWidget(m_found);
    layout->addWidget(m_wanted);
    layout->addWidget(note);
    layout->addStretch(1);
    layout->addWidget(run);

    const auto countCharacters = [this] {
        const int wanted = requested().characters.size();
        m_wanted->setText(wanted == 0 ? i18n("No characters have been asked for yet, so there is nothing to embed.")
                                      : i18np("One character goes in.", "%1 characters go in.", wanted));
    };

    // On a family the user has settled on rather than on every keystroke, for
    // the reason given where the tabs are built.
    connect(m_addFamily, &QComboBox::activated, this, [this] { locateChosen(); });
    connect(m_addFamily->lineEdit(), &QLineEdit::editingFinished, this, [this] { locateChosen(); });
    connect(m_bold, &QCheckBox::toggled, this, [this] { locateChosen(); });
    connect(m_italic, &QCheckBox::toggled, this, [this] { locateChosen(); });
    connect(m_text, &QLineEdit::textChanged, this, countCharacters);
    countCharacters();

    connect(run, &QPushButton::clicked, this, [this] { runAdd(); });
    return page;
}

FontEmbedder::Request FontsDialog::requested() const
{
    FontEmbedder::Request request;
    request.family = m_addFamily->currentText().trimmed();
    request.bold = m_bold->isChecked();
    request.italic = m_italic->isChecked();
    const QString characters = m_text->text();
    for (const QChar &character : characters) {
        request.characters.insert(character);
    }
    return request;
}

void FontsDialog::locateChosen()
{
    m_located = true;

    const FontEmbedder::Request request = requested();
    if (request.family.isEmpty()) {
        m_found->setText(i18n("Name a family to embed."));
        return;
    }

    QApplication::setOverrideCursor(Qt::WaitCursor);
    const QString path = FontEmbedder::locate(request);
    const bool usable = !path.isEmpty() && FontEmbedder::isAvailable(request);
    QApplication::restoreOverrideCursor();

    if (path.isEmpty()) {
        m_found->setText(i18n("No font called “%1” is installed on this system.", request.family));
    } else if (!usable) {
        // Worth saying rather than leaving to the attempt: a Type 1 or a bitmap
        // font is a perfectly good font that this cannot cut down, and the file
        // name is what makes that obvious.
        m_found->setText(i18n("%1 is not a TrueType or OpenType file, and only those can be cut down and "
                              "embedded.",
                              path));
    } else {
        m_found->setText(i18nc("@info where this system keeps the chosen font", "Found: %1", path));
    }
}

QVector<int> FontsDialog::chosenPages()
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

void FontsDialog::runAdd()
{
    const FontEmbedder::Request request = requested();
    if (request.family.isEmpty()) {
        KMessageBox::information(this, i18n("Name the family to embed."), windowTitle());
        return;
    }
    if (request.characters.isEmpty()) {
        KMessageBox::information(this,
                                 i18n("Name the characters to put in. Only those go in, which is what keeps "
                                      "the document from growing by a whole font."),
                                 windowTitle());
        return;
    }
    if (!FontEmbedder::isAvailable(request)) {
        KMessageBox::error(
            this, i18n("No font called “%1” that could be embedded is installed on this system.", request.family),
            windowTitle());
        return;
    }

    const QVector<int> pages = chosenPages();
    if (pages.size() == 1 && pages.first() == -1) {
        return;
    }

    runProducing(m_document, this, windowTitle(), u"-font-added.pdf"_s,
                 [this, request, pages](const QString &out, QStringList *summary, QString *error) {
                     QVector<FontEmbedder::Result> results;
                     QApplication::setOverrideCursor(Qt::WaitCursor);
                     const bool ok = FontEmbedder::embed(m_pdf, out, request, pages, &results, error);
                     QApplication::restoreOverrideCursor();
                     if (!ok) {
                         return false;
                     }

                     // Every page gets its own copy of the font, because a
                     // page's resources are its own; the results come back in
                     // the order the pages were worked through in, which is the
                     // order they are named here.
                     QVector<int> touched = pages;
                     if (touched.isEmpty()) {
                         for (int page = 0; page < results.size(); ++page) {
                             touched.append(page);
                         }
                     }

                     QStringList lines;
                     QStringList warnings;
                     for (int i = 0; i < results.size() && i < touched.size(); ++i) {
                         const FontEmbedder::Result &result = results.at(i);
                         lines += i18nc("@item the page, the /Fx name the font got there, and what went in",
                                        "Page %1: added as %2, %3 glyphs, %4.", touched.at(i) + 1, result.resourceName,
                                        result.glyphCount, sizeOf(result.embeddedBytes));
                         warnings += result.warnings;
                     }
                     // The same licence or the same missing character on forty
                     // pages is one thing to know, not forty.
                     warnings.removeDuplicates();

                     *summary += lines.join(u'\n');
                     if (!warnings.isEmpty()) {
                         *summary += warnings.join(u'\n');
                     }
                     *summary += i18n("The text already on those pages still uses the fonts it always did. The new "
                                      "one is a resource to write with, not a replacement for what is there.");
                     return true;
                 });
}

} // namespace

void showFonts(Document *document, QWidget *parent)
{
    const QString pdf = savedPath(document, parent);
    if (pdf.isEmpty()) {
        return;
    }
    FontsDialog(document, pdf, parent).exec();
}

} // namespace ps::tools
