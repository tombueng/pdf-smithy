/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "ToolDialogs.h"

#include "MainWindow.h"
#include "ToolSupport.h"
#include "core/Document.h"
#include "core/Typeset.h"
#include "render/PopplerBackend.h"

#include <KLocalizedString>
#include <KMessageBox>
#include <KStandardGuiItem>

#include <QApplication>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSizeF>
#include <QSpinBox>
#include <QSplitter>
#include <QStringDecoder>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTextDocument>
#include <QVBoxLayout>
#include <QVector>

#include <memory>

using namespace Qt::Literals::StringLiterals;

namespace ps::tools {

namespace {

constexpr double MillimetresToPoints = 72.0 / 25.4;

/**
 * The window @p widget lives in, so the finished document can be opened in it.
 *
 * ToolSupport has the same three lines and keeps them to itself, because
 * runProducing is the only caller it needs. This tool cannot use runProducing:
 * that helper insists on saving the open document first and then writes beside
 * it, which is right for the seven tools that *edit* a PDF and wrong here.
 * Typeset makes a document out of text, so there is nothing to save, often
 * nothing open at all, and the result belongs beside the text rather than
 * beside whatever the window happened to be showing.
 */
MainWindow *windowOf(QWidget *widget)
{
    for (QWidget *step = widget; step; step = step->parentWidget()) {
        if (auto *window = qobject_cast<MainWindow *>(step)) {
            return window;
        }
    }
    return nullptr;
}

/** Paper this dialog offers by name, measured the way a PDF measures it. */
struct Paper {
    QString name;
    QSizeF points;
};

QVector<Paper> papers()
{
    return {
        { i18nc("@item:inlistbox paper size", "A3"), QSizeF(841.89, 1190.55) },
        { i18nc("@item:inlistbox paper size", "A4"), QSizeF(595.276, 841.89) },
        { i18nc("@item:inlistbox paper size", "A5"), QSizeF(419.528, 595.276) },
        { i18nc("@item:inlistbox paper size", "Letter"), QSizeF(612.0, 792.0) },
        { i18nc("@item:inlistbox paper size", "Legal"), QSizeF(612.0, 1008.0) },
    };
}

/**
 * UTF-8, falling back to Latin-1 for bytes that are not.
 *
 * The same two-step Typeset::fromTextFile uses. Letting QTextStream pick the
 * local eight-bit codec instead is how a German text file reaches the page with
 * two wrong glyphs wherever it had an umlaut.
 */
QString decodeUtf8(const QByteArray &bytes)
{
    QStringDecoder decoder(QStringConverter::Utf8);
    const QString text = decoder(bytes);
    return decoder.hasError() ? QString::fromLatin1(bytes) : text;
}

/** What a run produced, as the lines a report or a message box shows. */
QStringList describe(const Typeset::Report &report)
{
    QStringList lines;
    lines += i18np("One page.", "%1 pages.", report.pages);
    lines += i18np("One paragraph.", "%1 paragraphs.", report.paragraphs);
    if (!report.overflows.isEmpty()) {
        lines += i18n("What a reader would otherwise notice before the author does:");
        for (const QString &note : report.overflows) {
            lines += u"  • "_s + note;
        }
    }
    return lines;
}

/** Wraps a settings tab so that a short window scrolls instead of clipping. */
QWidget *scrolled(QWidget *inner)
{
    auto *area = new QScrollArea(inner->parentWidget());
    area->setWidget(inner);
    area->setWidgetResizable(true);
    area->setFrameShape(QFrame::NoFrame);
    return area;
}

/**
 * Text on the left, the settings that decide how it is set on the right.
 *
 * The odd one out among the editing tools: every other one reads the open
 * document and writes a changed copy of it, while this one makes a document
 * where there was none. So an empty window is not an error here, nothing has to
 * be saved before the button works, and the finished PDF goes wherever the user
 * says rather than beside an input that may not exist.
 *
 * The preview is the real engine writing a real PDF into a temporary folder and
 * Poppler rendering it, not an approximation drawn with Qt's own layout, which
 * would be wrong in exactly the places that matter: where the lines break and
 * where the pages do.
 *
 * Two units are on show and they are kept apart on a rule: a distance across
 * the paper is in millimetres, a distance inside the type is in points. Every
 * spin box says which in its suffix, because a margin of 20 and a leading of 20
 * are not the same length and nothing about the number says so.
 */
class TypesetDialog : public QDialog
{
public:
    TypesetDialog(Document *document, QWidget *parent);

private:
    QWidget *buildSource();
    QWidget *buildPaper();
    QWidget *buildType();
    QWidget *buildPreview();

    /** Everything the settings say, in the form the engine wants it. */
    Typeset::Document settings() const;

    bool markdown() const;
    bool compose(const QString &output, Typeset::Report *report, QString *error) const;

    void loadFile();
    void refreshPreview();
    void showPreviewPage();
    void updateSummary();
    void paintColourButton();
    void run();

    QString startingFolder() const;
    QString suggestedOutput() const;

    Document *m_document = nullptr;

    /** Where the text came from, when it came from a file. Names the output. */
    QString m_sourcePath;

    QTabWidget *m_tabs = nullptr;
    QPlainTextEdit *m_text = nullptr;
    QComboBox *m_reading = nullptr;
    QLabel *m_summary = nullptr;

    QComboBox *m_paper = nullptr;
    QDoubleSpinBox *m_width = nullptr;
    QDoubleSpinBox *m_height = nullptr;
    QDoubleSpinBox *m_marginTop = nullptr;
    QDoubleSpinBox *m_marginBottom = nullptr;
    QDoubleSpinBox *m_marginLeft = nullptr;
    QDoubleSpinBox *m_marginRight = nullptr;
    QSpinBox *m_columns = nullptr;
    QDoubleSpinBox *m_gutter = nullptr;
    QLineEdit *m_header = nullptr;
    QLineEdit *m_footer = nullptr;
    QDoubleSpinBox *m_headerSize = nullptr;
    QLineEdit *m_title = nullptr;
    QLineEdit *m_author = nullptr;

    QComboBox *m_family = nullptr;
    QDoubleSpinBox *m_fontSize = nullptr;
    QDoubleSpinBox *m_leading = nullptr;
    QPushButton *m_colour = nullptr;
    QColor m_ink = Qt::black;
    QCheckBox *m_bold = nullptr;
    QCheckBox *m_italic = nullptr;
    QComboBox *m_alignment = nullptr;
    QDoubleSpinBox *m_spaceBefore = nullptr;
    QDoubleSpinBox *m_spaceAfter = nullptr;
    QDoubleSpinBox *m_indentFirst = nullptr;
    QDoubleSpinBox *m_indentLeft = nullptr;
    QDoubleSpinBox *m_indentRight = nullptr;
    QCheckBox *m_hyphenate = nullptr;
    QComboBox *m_language = nullptr;

    QScrollArea *m_preview = nullptr;
    QLabel *m_sheet = nullptr;
    QSpinBox *m_previewPage = nullptr;
    QPlainTextEdit *m_notes = nullptr;
    QTemporaryDir m_scratch;
    std::unique_ptr<PopplerBackend> m_backend;
    int m_previewPages = 0;

    /** Set while the paper list fills the two size boxes, so they do not answer back. */
    bool m_choosingPaper = false;
};

TypesetDialog::TypesetDialog(Document *document, QWidget *parent)
    : QDialog(parent)
    , m_document(document)
{
    setWindowTitle(i18nc("@title:window", "Set Text as Pages"));
    resize(1180, 820);

    // The tabs are built in this order because each one only connects its
    // signals after it has set its own starting values, and the summary those
    // signals refresh reads widgets from all three.
    m_tabs = new QTabWidget(this);
    m_tabs->addTab(scrolled(buildPaper()), i18nc("@title:tab", "Paper"));
    m_tabs->addTab(scrolled(buildType()), i18nc("@title:tab", "Type"));
    const int preview = m_tabs->addTab(buildPreview(), i18nc("@title:tab", "Preview"));

    auto *split = new QSplitter(Qt::Horizontal, this);
    split->addWidget(buildSource());
    split->addWidget(m_tabs);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 4);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    auto *limits = buttons->addButton(i18nc("@action:button", "What This Cannot Do"), QDialogButtonBox::ActionRole);
    auto *make = buttons->addButton(i18nc("@action:button", "Set the Pages…"), QDialogButtonBox::ActionRole);
    make->setIcon(QIcon::fromTheme(u"document-export"_s));
    make->setDefault(true);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(split, 1);
    layout->addWidget(buttons);

    // Set afresh whenever the tab comes up, rather than left as it was: a
    // preview that shows the settings from two changes ago is worse than none,
    // because it looks like an answer.
    connect(m_tabs, &QTabWidget::currentChanged, this, [this, preview](int index) {
        if (index == preview) {
            refreshPreview();
        }
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(limits, &QPushButton::clicked, this, [this] {
        showReport(this, i18nc("@title:window", "What Setting Text Cannot Do"), Typeset::limitations());
    });
    connect(make, &QPushButton::clicked, this, [this] { run(); });

    updateSummary();
}

// ── The text ──────────────────────────────────────────────────────────────

QWidget *TypesetDialog::buildSource()
{
    auto *page = new QWidget(this);

    auto *load
        = new QPushButton(QIcon::fromTheme(u"document-open"_s), i18nc("@action:button", "Load from a File…"), page);

    m_reading = new QComboBox(page);
    m_reading->addItem(i18nc("@item:inlistbox how the text is read", "Plain text"), false);
    m_reading->addItem(i18nc("@item:inlistbox how the text is read", "Markdown"), true);
    m_reading->setToolTip(i18nc("@info:tooltip",
                                "Plain text: a blank line starts a paragraph and nothing else is interpreted. "
                                "Markdown: headings, lists, quotations, code, tables, bold and italic as well."));

    auto *row = new QHBoxLayout;
    row->addWidget(load);
    row->addWidget(new QLabel(i18nc("@label:listbox", "Read as:"), page));
    row->addWidget(m_reading);
    row->addStretch(1);

    m_text = new QPlainTextEdit(page);
    m_text->setPlaceholderText(i18n("Type or paste the text here, or load a file. A blank line starts a new "
                                    "paragraph."));
    m_text->setTabChangesFocus(true);

    m_summary = new QLabel(page);
    m_summary->setWordWrap(true);

    auto *layout = new QVBoxLayout(page);
    layout->addLayout(row);
    layout->addWidget(m_text, 1);
    layout->addWidget(m_summary);

    connect(load, &QPushButton::clicked, this, [this] { loadFile(); });
    connect(m_reading, &QComboBox::currentIndexChanged, this, [this] { updateSummary(); });
    connect(m_text, &QPlainTextEdit::textChanged, this, [this] { updateSummary(); });
    return page;
}

void TypesetDialog::loadFile()
{
    const QString path
        = QFileDialog::getOpenFileName(this, i18nc("@title:window", "Open a Text File"), startingFolder(),
                                       i18n("Text and Markdown (*.txt *.text *.md *.markdown *.mdown *.mkd);;"
                                            "All files (*)"));
    if (path.isEmpty()) {
        return;
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        KMessageBox::error(this, i18n("“%1” could not be read.", path), windowTitle());
        return;
    }

    // A text editor holding tens of megabytes stops answering the keyboard, and
    // the user would blame the program rather than the file they chose.
    constexpr qint64 Comfortable = 16 * 1024 * 1024;
    if (file.size() > Comfortable
        && KMessageBox::questionTwoActions(
               this,
               i18n("That file is %1. The editor will be slow with that much text in it. Load it anyway?",
                    QLocale().formattedDataSize(file.size())),
               windowTitle(), KGuiItem(i18nc("@action:button", "Load It")), KStandardGuiItem::cancel())
            != KMessageBox::PrimaryAction) {
        return;
    }

    m_text->setPlainText(decodeUtf8(file.readAll()));
    m_sourcePath = path;

    // Typeset's own rule for a file handed to it by name, kept here so that
    // opening a .md and pressing the button does what the command line does.
    static const QStringList markdownSuffixes { u"md"_s, u"markdown"_s, u"mdown"_s, u"mkd"_s };
    if (markdownSuffixes.contains(QFileInfo(path).suffix().toLower())) {
        m_reading->setCurrentIndex(m_reading->findData(true));
    }
    updateSummary();
}

// ── Paper ─────────────────────────────────────────────────────────────────

QWidget *TypesetDialog::buildPaper()
{
    auto *page = new QWidget(this);
    const Typeset::Document defaults;

    const auto millimetres = [page](double points, double most) {
        auto *box = new QDoubleSpinBox(page);
        box->setRange(0.0, most);
        box->setDecimals(1);
        box->setSingleStep(1.0);
        box->setSuffix(i18nc("@item millimetre suffix in a spin box", " mm"));
        box->setValue(points / MillimetresToPoints);
        return box;
    };

    m_paper = new QComboBox(page);
    const QVector<Paper> known = papers();
    for (const Paper &paper : known) {
        m_paper->addItem(paper.name, QVariant::fromValue(paper.points));
    }
    m_paper->addItem(i18nc("@item:inlistbox paper size", "Custom"), QVariant::fromValue(QSizeF()));
    for (int i = 0; i < known.size(); ++i) {
        // The engine already has an opinion about the default paper; the dialog
        // agrees with it rather than holding a second one that could drift.
        if (known.at(i).points == defaults.pageSize) {
            m_paper->setCurrentIndex(i);
        }
    }

    auto *turn
        = new QPushButton(QIcon::fromTheme(u"object-rotate-right"_s), i18nc("@action:button", "Turn the Paper"), page);
    turn->setToolTip(i18nc("@info:tooltip", "Swaps the width and the height, which is what landscape is."));

    m_width = millimetres(defaults.pageSize.width(), 2000.0);
    m_height = millimetres(defaults.pageSize.height(), 2000.0);

    m_marginTop = millimetres(defaults.marginTop, 200.0);
    m_marginBottom = millimetres(defaults.marginBottom, 200.0);
    m_marginLeft = millimetres(defaults.marginLeft, 200.0);
    m_marginRight = millimetres(defaults.marginRight, 200.0);

    m_columns = new QSpinBox(page);
    m_columns->setRange(1, 12);
    m_columns->setValue(defaults.columns);

    m_gutter = millimetres(defaults.columnGap, 100.0);
    m_gutter->setToolTip(i18nc("@info:tooltip", "The space between two columns of text."));

    m_header = new QLineEdit(page);
    m_footer = new QLineEdit(page);
    const QString placeholders = i18nc("@info:tooltip",
                                       "{page}, {pages}, {title} and {date} are filled in. The line is centred and "
                                       "holds one line of text.");
    m_header->setToolTip(placeholders);
    m_footer->setToolTip(placeholders);
    m_footer->setPlaceholderText(i18n("For example: {title} · {page} of {pages}"));

    m_headerSize = new QDoubleSpinBox(page);
    m_headerSize->setRange(4.0, 48.0);
    m_headerSize->setDecimals(1);
    m_headerSize->setSingleStep(0.5);
    m_headerSize->setSuffix(i18nc("@item point suffix in a spin box", " pt"));
    m_headerSize->setValue(defaults.headerSize);

    m_title = new QLineEdit(page);
    m_title->setToolTip(i18nc("@info:tooltip",
                              "Written into the document's properties, and put in wherever a "
                              "running head says {title}."));
    m_author = new QLineEdit(page);

    auto *paperRow = new QHBoxLayout;
    paperRow->addWidget(m_paper, 1);
    paperRow->addWidget(turn);

    auto *sheet = new QGroupBox(i18nc("@title:group", "Sheet"), page);
    auto *sheetForm = new QFormLayout(sheet);
    sheetForm->addRow(i18nc("@label:listbox", "Paper:"), paperRow);
    sheetForm->addRow(i18nc("@label:spinbox", "Width:"), m_width);
    sheetForm->addRow(i18nc("@label:spinbox", "Height:"), m_height);

    auto *margins = new QGroupBox(i18nc("@title:group", "Margins"), page);
    auto *marginForm = new QFormLayout(margins);
    marginForm->addRow(i18nc("@label:spinbox", "Top:"), m_marginTop);
    marginForm->addRow(i18nc("@label:spinbox", "Bottom:"), m_marginBottom);
    marginForm->addRow(i18nc("@label:spinbox", "Left:"), m_marginLeft);
    marginForm->addRow(i18nc("@label:spinbox", "Right:"), m_marginRight);

    auto *columns = new QGroupBox(i18nc("@title:group", "Columns"), page);
    auto *columnForm = new QFormLayout(columns);
    columnForm->addRow(i18nc("@label:spinbox", "Columns:"), m_columns);
    columnForm->addRow(i18nc("@label:spinbox", "Gap between them:"), m_gutter);

    auto *running = new QGroupBox(i18nc("@title:group", "Running head and foot"), page);
    auto *runningForm = new QFormLayout(running);
    runningForm->addRow(i18nc("@label:textbox", "Head:"), m_header);
    runningForm->addRow(i18nc("@label:textbox", "Foot:"), m_footer);
    runningForm->addRow(i18nc("@label:spinbox", "Size:"), m_headerSize);

    auto *properties = new QGroupBox(i18nc("@title:group", "The document itself"), page);
    auto *propertyForm = new QFormLayout(properties);
    propertyForm->addRow(i18nc("@label:textbox", "Title:"), m_title);
    propertyForm->addRow(i18nc("@label:textbox", "Author:"), m_author);

    auto *layout = new QVBoxLayout(page);
    layout->addWidget(sheet);
    layout->addWidget(margins);
    layout->addWidget(columns);
    layout->addWidget(running);
    layout->addWidget(properties);
    layout->addStretch(1);

    // Typing a size by hand is choosing a custom sheet, so the list says so
    // instead of going on claiming A4 for something that is not A4 any more.
    const auto becomeCustom = [this] {
        if (!m_choosingPaper) {
            m_paper->setCurrentIndex(m_paper->count() - 1);
        }
        updateSummary();
    };

    connect(m_paper, &QComboBox::currentIndexChanged, this, [this] {
        const QSizeF chosen = m_paper->currentData().toSizeF();
        if (chosen.isEmpty()) {
            return; // "Custom" carries no size; the two boxes already hold it.
        }
        m_choosingPaper = true;
        m_width->setValue(chosen.width() / MillimetresToPoints);
        m_height->setValue(chosen.height() / MillimetresToPoints);
        m_choosingPaper = false;
        updateSummary();
    });
    connect(m_width, &QDoubleSpinBox::valueChanged, this, becomeCustom);
    connect(m_height, &QDoubleSpinBox::valueChanged, this, becomeCustom);
    connect(turn, &QPushButton::clicked, this, [this] {
        const double wide = m_width->value();
        m_width->setValue(m_height->value());
        m_height->setValue(wide);
    });
    connect(m_columns, &QSpinBox::valueChanged, this, [this] { updateSummary(); });
    return page;
}

// ── Type ──────────────────────────────────────────────────────────────────

QWidget *TypesetDialog::buildType()
{
    auto *page = new QWidget(this);
    const Typeset::Document defaults;

    const auto points = [page](double value, double least, double most) {
        auto *box = new QDoubleSpinBox(page);
        box->setRange(least, most);
        box->setDecimals(1);
        box->setSingleStep(0.5);
        box->setSuffix(i18nc("@item point suffix in a spin box", " pt"));
        box->setValue(value);
        return box;
    };

    m_family = new QComboBox(page);
    m_family->addItem(i18nc("@item:inlistbox font", "Helvetica"), u"Helvetica"_s);
    m_family->addItem(i18nc("@item:inlistbox font", "Times"), u"Times"_s);
    m_family->addItem(i18nc("@item:inlistbox font", "Courier"), u"Courier"_s);
    m_family->setCurrentIndex(qMax(0, m_family->findData(defaults.body.family)));

    m_fontSize = points(defaults.body.fontSize, 4.0, 96.0);

    m_leading = points(defaults.body.leading, 0.0, 200.0);
    m_leading->setSpecialValueText(i18nc("@item:valuesuffix leading that follows the type size", "automatic"));
    m_leading->setToolTip(i18nc("@info:tooltip",
                                "The distance from one baseline to the next. Left automatic it is 1.35 times the "
                                "type size, which is what most text wants."));

    m_colour = new QPushButton(page);
    m_colour->setMinimumWidth(90);
    m_ink = defaults.body.colour;
    paintColourButton();

    m_bold = new QCheckBox(i18nc("@option:check", "Bold"), page);
    m_bold->setChecked(defaults.body.bold);
    m_italic = new QCheckBox(i18nc("@option:check", "Italic"), page);
    m_italic->setChecked(defaults.body.italic);

    m_alignment = new QComboBox(page);
    m_alignment->addItem(i18nc("@item:inlistbox how text is set", "Ranged left"), int(Qt::AlignLeft));
    m_alignment->addItem(i18nc("@item:inlistbox how text is set", "Centred"), int(Qt::AlignHCenter));
    m_alignment->addItem(i18nc("@item:inlistbox how text is set", "Ranged right"), int(Qt::AlignRight));
    m_alignment->addItem(i18nc("@item:inlistbox how text is set", "Justified"), int(Qt::AlignJustify));
    m_alignment->setToolTip(i18nc("@info:tooltip",
                                  "Justifying stretches the spaces between words and leaves the last line of each "
                                  "paragraph alone."));

    m_spaceBefore = points(defaults.body.spaceBefore, 0.0, 200.0);
    m_spaceAfter = points(defaults.body.spaceAfter, 0.0, 200.0);
    m_indentFirst = points(defaults.body.indentFirst, -200.0, 200.0);
    m_indentFirst->setToolTip(i18nc("@info:tooltip",
                                    "Indenting the first line or leaving a space between paragraphs both mark where "
                                    "one paragraph ends. Doing both marks it twice."));
    m_indentLeft = points(defaults.body.indentLeft, 0.0, 400.0);
    m_indentRight = points(defaults.body.indentRight, 0.0, 400.0);

    m_hyphenate = new QCheckBox(i18nc("@option:check", "Break long words at the end of a line"), page);
    m_hyphenate->setChecked(defaults.hyphenate);

    m_language = new QComboBox(page);
    m_language->setEditable(true);
    m_language->addItems({ u"en"_s, u"de"_s, u"fr"_s, u"es"_s, u"it"_s, u"nl"_s, u"pt"_s, u"da"_s, u"sv"_s });
    m_language->setCurrentText(defaults.language);
    m_language->setToolTip(i18nc("@info:tooltip",
                                 "Decides how much of a word has to stay behind when it is broken: German keeps two "
                                 "letters, everything else keeps three."));
    m_language->setEnabled(m_hyphenate->isChecked());

    auto *fonts = new QLabel(i18n("Three of the fourteen standard fonts: Helvetica has no serifs, Times has them, "
                                  "Courier is monospaced. None of them is embedded, so the file stays a few "
                                  "kilobytes and every reader on earth can show it. The price is the alphabet: "
                                  "western Europe and no further."),
                             page);
    fonts->setWordWrap(true);

    auto *headings = new QLabel(i18n("Headings are not set here. A Markdown heading takes the body's font, larger "
                                     "and bold, with the space above and below that goes with its size, which is "
                                     "what keeps a document set in Times from getting Helvetica headings."),
                                page);
    headings->setWordWrap(true);

    auto *bodyBox = new QGroupBox(i18nc("@title:group", "Body"), page);
    auto *bodyForm = new QFormLayout(bodyBox);
    bodyForm->addRow(i18nc("@label:listbox", "Font:"), m_family);
    bodyForm->addRow(i18nc("@label:spinbox", "Size:"), m_fontSize);
    bodyForm->addRow(i18nc("@label:spinbox", "Leading:"), m_leading);
    bodyForm->addRow(i18nc("@label:chooser", "Colour:"), m_colour);
    bodyForm->addRow(QString(), m_bold);
    bodyForm->addRow(QString(), m_italic);
    bodyForm->addRow(i18nc("@label:listbox", "Set:"), m_alignment);
    bodyForm->addRow(fonts);
    bodyForm->addRow(headings);

    auto *paragraphs = new QGroupBox(i18nc("@title:group", "Paragraphs"), page);
    auto *paragraphForm = new QFormLayout(paragraphs);
    paragraphForm->addRow(i18nc("@label:spinbox", "Space above:"), m_spaceBefore);
    paragraphForm->addRow(i18nc("@label:spinbox", "Space below:"), m_spaceAfter);
    paragraphForm->addRow(i18nc("@label:spinbox", "First line indent:"), m_indentFirst);
    paragraphForm->addRow(i18nc("@label:spinbox", "Indent from the left:"), m_indentLeft);
    paragraphForm->addRow(i18nc("@label:spinbox", "Indent from the right:"), m_indentRight);

    auto *hyphenation = new QGroupBox(i18nc("@title:group", "Hyphenation"), page);
    auto *hyphenationForm = new QFormLayout(hyphenation);
    hyphenationForm->addRow(m_hyphenate);
    hyphenationForm->addRow(i18nc("@label:listbox", "Language:"), m_language);

    auto *layout = new QVBoxLayout(page);
    layout->addWidget(bodyBox);
    layout->addWidget(paragraphs);
    layout->addWidget(hyphenation);
    layout->addStretch(1);

    connect(m_colour, &QPushButton::clicked, this, [this] {
        const QColor picked = QColorDialog::getColor(m_ink, this, i18nc("@title:window", "Colour of the Text"));
        if (picked.isValid()) {
            m_ink = picked;
            paintColourButton();
        }
    });
    connect(m_hyphenate, &QCheckBox::toggled, this, [this](bool on) {
        m_language->setEnabled(on);
        updateSummary();
    });
    connect(m_family, &QComboBox::currentIndexChanged, this, [this] { updateSummary(); });
    connect(m_fontSize, &QDoubleSpinBox::valueChanged, this, [this] { updateSummary(); });
    connect(m_leading, &QDoubleSpinBox::valueChanged, this, [this] { updateSummary(); });
    return page;
}

void TypesetDialog::paintColourButton()
{
    m_colour->setText(m_ink.name());
    // Black text on a dark swatch is unreadable, so the label follows the
    // swatch rather than the theme.
    const QString label = m_ink.lightnessF() > 0.55 ? u"#101010"_s : u"#f0f0f0"_s;
    m_colour->setStyleSheet(u"background-color: %1; color: %2;"_s.arg(m_ink.name(), label));
}

// ── Preview ───────────────────────────────────────────────────────────────

QWidget *TypesetDialog::buildPreview()
{
    auto *page = new QWidget(this);

    auto *again = new QPushButton(QIcon::fromTheme(u"view-refresh"_s), i18nc("@action:button", "Set It Again"), page);

    m_previewPage = new QSpinBox(page);
    m_previewPage->setRange(1, 1);
    m_previewPage->setPrefix(i18nc("@item prefix in a page spin box", "Page "));

    auto *row = new QHBoxLayout;
    row->addWidget(again);
    row->addWidget(m_previewPage);
    row->addStretch(1);

    m_sheet = new QLabel(page);
    m_sheet->setAlignment(Qt::AlignCenter);
    m_sheet->setWordWrap(true);
    m_sheet->setText(i18n("The same engine that writes the finished document writes this, so what is shown here is "
                          "where the lines and the pages really break."));

    m_preview = new QScrollArea(page);
    m_preview->setWidget(m_sheet);
    m_preview->setWidgetResizable(true);
    m_preview->setAlignment(Qt::AlignCenter);

    m_notes = new QPlainTextEdit(page);
    m_notes->setReadOnly(true);
    m_notes->setMaximumHeight(150);

    auto *layout = new QVBoxLayout(page);
    layout->addLayout(row);
    layout->addWidget(m_preview, 1);
    layout->addWidget(m_notes);

    connect(again, &QPushButton::clicked, this, [this] { refreshPreview(); });
    connect(m_previewPage, &QSpinBox::valueChanged, this, [this] { showPreviewPage(); });
    return page;
}

void TypesetDialog::refreshPreview()
{
    const auto say = [this](const QString &text) {
        m_sheet->setMinimumSize(0, 0);
        m_sheet->setWordWrap(true);
        m_sheet->setText(text);
    };

    if (m_text->document()->isEmpty()) {
        m_previewPages = 0;
        m_previewPage->setRange(1, 1);
        m_notes->clear();
        say(i18n("There is no text yet, so there is nothing to show."));
        return;
    }
    if (!m_scratch.isValid()) {
        m_notes->clear();
        say(i18n("A temporary folder for the preview could not be made. Setting the pages themselves still works."));
        return;
    }

    // Closed before it is written over. Poppler keeps the file open, and a page
    // rendered out of a half-rewritten file is worse than no page at all.
    if (m_backend) {
        m_backend->removeDocument(0);
    }
    const QString path = m_scratch.filePath(u"preview.pdf"_s);

    QApplication::setOverrideCursor(Qt::WaitCursor);
    Typeset::Report report;
    QString error;
    const bool composed = compose(path, &report, &error);
    QApplication::restoreOverrideCursor();

    if (!composed) {
        m_previewPages = 0;
        m_notes->clear();
        say(error.isEmpty() ? i18n("The text could not be set.") : error);
        return;
    }

    if (!m_backend) {
        m_backend = std::make_unique<PopplerBackend>();
    }
    QString opening;
    if (!m_backend->addDocument(0, path, &opening)) {
        m_previewPages = 0;
        m_notes->setPlainText(describe(report).join(u'\n'));
        say(opening.isEmpty() ? i18n("The preview could not be read back.") : opening);
        return;
    }

    m_previewPages = report.pages;
    m_previewPage->setRange(1, qMax(1, report.pages));
    m_notes->setPlainText(describe(report).join(u'\n'));
    showPreviewPage();
}

void TypesetDialog::showPreviewPage()
{
    if (!m_backend || m_previewPages <= 0) {
        return;
    }

    // Rendered at the width it is shown at rather than at a fixed size, so the
    // type in the preview is as sharp as the screen allows.
    const int width = qMax(360, m_preview->viewport()->width() - 24);

    QApplication::setOverrideCursor(Qt::WaitCursor);
    const QImage sheet = m_backend->renderPage(0, m_previewPage->value() - 1, width);
    QApplication::restoreOverrideCursor();

    if (sheet.isNull()) {
        m_sheet->setMinimumSize(0, 0);
        m_sheet->setWordWrap(true);
        m_sheet->setText(i18n("That page could not be rendered."));
        return;
    }

    // A minimum size rather than a bare pixmap: without it the scroll area
    // squeezes the label down and a long page has no way to scroll.
    m_sheet->setWordWrap(false);
    m_sheet->setPixmap(QPixmap::fromImage(sheet));
    m_sheet->setMinimumSize(sheet.size());
}

// ── Setting the pages ─────────────────────────────────────────────────────

bool TypesetDialog::markdown() const
{
    return m_reading->currentData().toBool();
}

Typeset::Document TypesetDialog::settings() const
{
    Typeset::Document document;
    document.pageSize = QSizeF(m_width->value() * MillimetresToPoints, m_height->value() * MillimetresToPoints);
    document.marginTop = m_marginTop->value() * MillimetresToPoints;
    document.marginBottom = m_marginBottom->value() * MillimetresToPoints;
    document.marginLeft = m_marginLeft->value() * MillimetresToPoints;
    document.marginRight = m_marginRight->value() * MillimetresToPoints;
    document.columns = m_columns->value();
    document.columnGap = m_gutter->value() * MillimetresToPoints;

    document.header = m_header->text();
    document.footer = m_footer->text();
    document.headerSize = m_headerSize->value();
    document.title = m_title->text();
    document.author = m_author->text();

    document.body.family = m_family->currentData().toString();
    document.body.fontSize = m_fontSize->value();
    document.body.leading = m_leading->value();
    document.body.colour = m_ink;
    document.body.bold = m_bold->isChecked();
    document.body.italic = m_italic->isChecked();
    document.body.alignment = Qt::Alignment(static_cast<Qt::AlignmentFlag>(m_alignment->currentData().toInt()));
    document.body.spaceBefore = m_spaceBefore->value();
    document.body.spaceAfter = m_spaceAfter->value();
    document.body.indentFirst = m_indentFirst->value();
    document.body.indentLeft = m_indentLeft->value();
    document.body.indentRight = m_indentRight->value();

    document.hyphenate = m_hyphenate->isChecked();
    document.language = m_language->currentText().trimmed();

    // headings is deliberately left empty: the engine derives every level from
    // the body, and a dialog that filled the six of them would have to repeat
    // that derivation and would then be the place it went stale.
    return document;
}

bool TypesetDialog::compose(const QString &output, Typeset::Report *report, QString *error) const
{
    const QString source = m_text->toPlainText();
    return markdown() ? Typeset::fromMarkdown(source, output, settings(), report, error)
                      : Typeset::fromPlainText(source, output, settings(), report, error);
}

QString TypesetDialog::startingFolder() const
{
    if (!m_sourcePath.isEmpty()) {
        return QFileInfo(m_sourcePath).absolutePath();
    }
    if (m_document && !m_document->filePath().isEmpty()) {
        return QFileInfo(m_document->filePath()).absolutePath();
    }
    return QDir::homePath();
}

QString TypesetDialog::suggestedOutput() const
{
    // Text loaded from a file names the PDF, because that is the answer the
    // user would have typed anyway.
    if (!m_sourcePath.isEmpty()) {
        const QFileInfo source(m_sourcePath);
        return source.absolutePath() + u'/' + source.completeBaseName() + u".pdf"_s;
    }

    // A title with a separator in it would name a folder that does not exist,
    // so it loses the separator before the chooser ever sees it.
    QString name = m_title->text().trimmed().replace(u'/', u'-');
    if (name.isEmpty()) {
        name = i18nc("@item default file name for pages made from typed text", "typeset");
    }
    return startingFolder() + u'/' + name + u".pdf"_s;
}

void TypesetDialog::run()
{
    if (m_text->toPlainText().trimmed().isEmpty()) {
        KMessageBox::information(this, i18n("There is no text to set. Type some, or load a file."), windowTitle());
        return;
    }

    const QString output = QFileDialog::getSaveFileName(this, i18nc("@title:window", "Where the Pages Should Go"),
                                                        suggestedOutput(), i18n("PDF documents (*.pdf)"));
    if (output.isEmpty()) {
        return;
    }

    QApplication::setOverrideCursor(Qt::WaitCursor);
    Typeset::Report report;
    QString error;
    const bool composed = compose(output, &report, &error);
    QApplication::restoreOverrideCursor();

    if (!composed) {
        KMessageBox::error(this, error.isEmpty() ? i18n("The text could not be set.") : error, windowTitle());
        return;
    }

    QStringList lines = describe(report);
    lines += i18n("Written to %1.", QFileInfo(output).fileName());

    MainWindow *window = windowOf(this);
    if (!window) {
        showReport(this, windowTitle(), lines);
        return;
    }

    // Offered rather than done, the same as every other tool that produces a
    // new document: quietly replacing what someone has open is the kind of
    // helpfulness that loses work.
    if (KMessageBox::questionTwoActions(this, lines.join(u'\n'), windowTitle(),
                                        KGuiItem(i18nc("@action:button", "Open the Result")),
                                        KGuiItem(i18nc("@action:button", "Leave It on Disk")))
        == KMessageBox::PrimaryAction) {
        window->openFile(output);
    }
}

void TypesetDialog::updateSummary()
{
    // The header says a leading of zero means 1.35 times the size, so this can
    // say the number rather than showing a nought and leaving it to be guessed.
    const double leading = m_leading->value() > 0.0 ? m_leading->value() : m_fontSize->value() * 1.35;

    QStringList parts;
    parts += markdown() ? i18nc("@info part of a one-line summary", "read as Markdown")
                        : i18nc("@info part of a one-line summary", "read as plain text");
    parts += i18nc("@info part of a one-line summary: name, width and height of the paper", "%1, %2 × %3 mm",
                   m_paper->currentText(), QLocale().toString(m_width->value(), 'f', 0),
                   QLocale().toString(m_height->value(), 'f', 0));
    parts += i18ncp("@info part of a one-line summary", "one column", "%1 columns", m_columns->value());
    parts += i18nc("@info part of a one-line summary: font, size and leading", "%1 at %2 pt on %3 pt",
                   m_family->currentText(), QLocale().toString(m_fontSize->value(), 'f', 1),
                   QLocale().toString(leading, 'f', 1));
    parts += m_hyphenate->isChecked() ? i18nc("@info part of a one-line summary", "hyphenated")
                                      : i18nc("@info part of a one-line summary", "not hyphenated");

    // characterCount() counts the block terminator the document always carries,
    // and toPlainText() would copy the whole text on every keystroke to say the
    // same thing.
    const int characters = qMax(0, m_text->document()->characterCount() - 1);

    m_summary->setText(parts.join(u" · "_s) + u'\n'
                       + i18np("One character of text.", "%1 characters of text.", characters));
}

} // namespace

void showTypeset(Document *document, QWidget *parent)
{
    // No savedPath() here, unlike every other tool in this family. This one
    // reads the text in front of it rather than the file on disk, so an empty
    // window and an unsaved document are both perfectly good starting points.
    TypesetDialog(document, parent).exec();
}

} // namespace ps::tools
