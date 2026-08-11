/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "DocumentProperties.h"

#include "DocumentCommands.h"
#include "InspectorDock.h"
#include "core/Document.h"
#include "core/FontInventory.h"
#include "core/PageObjects.h"
#include "core/PageRange.h"
#include "core/PdfGeometry.h"
#include "core/RenderBackend.h"
#include "core/Source.h"

#include <qpdf/QPDF.hh>
#include <qpdf/QPDFEmbeddedFileDocumentHelper.hh>
#include <qpdf/QPDFObjGen.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDFPageObjectHelper.hh>

#include <KFormat>
#include <KLocalizedString>

#include <QAbstractItemView>
#include <QDateTimeEdit>
#include <QEvent>
#include <QFileInfo>
#include <QFont>
#include <QFormLayout>
#include <QHash>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QSet>
#include <QTimer>
#include <QUndoStack>
#include <QVBoxLayout>
#include <QWidget>
#include <QtConcurrentRun>

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <utility>

using namespace Qt::Literals::StringLiterals;

namespace ps {

namespace {

// ── Measuring, in the units the reader thinks in ──────────────────────────

constexpr double kPointsPerMillimetre = 72.0 / 25.4;

bool metricHere()
{
    return QLocale().measurementSystem() == QLocale::MetricSystem;
}

/** Width by height, through QLocale so a German desktop reads 210,0 × 297,0 mm. */
QString extent(const QSizeF &points)
{
    if (points.isEmpty()) {
        return {};
    }
    if (metricHere()) {
        return i18nc("@info the width and height of a page, in millimetres", "%1 × %2 mm",
                     QLocale().toString(points.width() / kPointsPerMillimetre, 'f', 1),
                     QLocale().toString(points.height() / kPointsPerMillimetre, 'f', 1));
    }
    return i18nc("@info the width and height of a page, in inches", "%1 × %2 in",
                 QLocale().toString(points.width() / 72.0, 'f', 2), QLocale().toString(points.height() / 72.0, 'f', 2));
}

/** The same size in the unit the file itself is written in, for the tooltip. */
QString extentInPoints(const QSizeF &points)
{
    return i18nc("@info the width and height of a page, in PostScript points", "%1 × %2 pt",
                 QLocale().toString(points.width(), 'f', 2), QLocale().toString(points.height(), 'f', 2));
}

/**
 * The paper this size is, when it is one people have a name for.
 *
 * The table is small and local on purpose. The layout tool carries one of its
 * own because it has to *offer* sizes; this only has to recognise the handful
 * anybody would look for, and a shared table would have to live in ps_core,
 * which is not this change's to move.
 */
QString paperName(const QSizeF &points)
{
    struct Paper {
        const char *name;
        double width;
        double height;
    };
    static constexpr Paper kPapers[] = {
        { "A3", 297.0, 420.0 },    { "A4", 210.0, 297.0 },      { "A5", 148.0, 210.0 },
        { "A6", 105.0, 148.0 },    { "B5", 176.0, 250.0 },      { "Letter", 215.9, 279.4 },
        { "Legal", 215.9, 355.6 }, { "Tabloid", 279.4, 431.8 }, { "Executive", 184.15, 266.7 },
    };

    const double width = points.width() / kPointsPerMillimetre;
    const double height = points.height() / kPointsPerMillimetre;
    // A millimetre and a half of slack: real documents are trimmed, resaved and
    // rounded, and an A4 page that measures 209.9 mm is still A4 to its owner.
    constexpr double kSlack = 1.5;

    for (const Paper &paper : kPapers) {
        const QString name = QString::fromLatin1(paper.name);
        if (std::abs(width - paper.width) < kSlack && std::abs(height - paper.height) < kSlack) {
            return name;
        }
        if (std::abs(width - paper.height) < kSlack && std::abs(height - paper.width) < kSlack) {
            return i18nc("@info a named paper size turned on its side, e.g. \"A4 sideways\"", "%1 sideways", name);
        }
    }
    return {};
}

QString formatBytes(qint64 bytes)
{
    return KFormat().formatByteSize(double(bytes));
}

// ── Reading one page out of a file that is already open ───────────────────

/** A box entry as a rectangle, or an invalid one when the page does not carry it. */
QRectF boxRect(QPDFObjectHandle box)
{
    if (!box.isArray() || box.getArrayNItems() != 4) {
        return {};
    }
    const double missing = std::numeric_limits<double>::quiet_NaN();
    double edge[4];
    for (int i = 0; i < 4; ++i) {
        // Never getNumericValue(): it goes through strtod and reads every
        // fractional number as zero wherever the decimal separator is a comma.
        edge[i] = PdfGeometry::boxValue(box, i, missing);
        if (std::isnan(edge[i])) {
            return {};
        }
    }
    const QRectF rect(qMin(edge[0], edge[2]), qMin(edge[1], edge[3]), qAbs(edge[2] - edge[0]), qAbs(edge[3] - edge[1]));
    return rect.isValid() ? rect : QRectF();
}

/**
 * What the file spends on one page, from the objects the page points at.
 *
 * /Parent is deliberately never followed. It leads to the page tree and from
 * there to every other page, which is how the first version of this reported
 * the entire 17 MB of a magazine as the weight of each of its 180 pages.
 *
 * Resources two pages share are counted for both, because there is no honest
 * way to give a shared font to one of the four pages that use it. So the pages
 * add up to a little more than the document (18.2 MB against a 17.3 MB file on
 * that magazine, six per cent over), and the number answers the question people
 * actually ask of it, which is which page is the heavy one.
 *
 * Stream bodies are counted from /Length rather than by fetching the data: the
 * two agreed to the byte on every page measured, and one of them reads the file.
 */
qint64 weighPage(QPDFObjectHandle object, std::set<QPDFObjGen> &seen)
{
    if (object.isIndirect()) {
        if (seen.count(object.getObjGen()) > 0) {
            return 0;
        }
        seen.insert(object.getObjGen());
    }

    qint64 total = 0;
    if (object.isStream()) {
        QPDFObjectHandle dictionary = object.getDict();
        const QPDFObjectHandle length = dictionary.getKey("/Length");
        if (length.isInteger()) {
            total += qint64(length.getIntValue());
        }
        total += weighPage(dictionary, seen);
    } else if (object.isDictionary()) {
        for (const auto &entry : object.getDictAsMap()) {
            if (entry.first == "/Parent") {
                continue;
            }
            total += qint64(entry.first.size()) + 2;
            total += weighPage(entry.second, seen);
        }
    } else if (object.isArray()) {
        for (const auto &item : object.getArrayAsVector()) {
            total += weighPage(item, seen);
        }
    } else {
        total += qint64(object.unparse().size());
    }
    return total;
}

PageFacts readPage(const Document *document, const PageRows &rows, int row)
{
    PageFacts facts;
    facts.row = row;
    facts.page = rows.pageOf(row);
    facts.file = rows.fileOf(facts.page);
    if (!document || !facts.page.isValid()) {
        return facts;
    }
    Source *source = document->source(facts.page.sourceId());
    if (!source) {
        return facts;
    }

    try {
        QPDFPageObjectHelper page = source->page(facts.page.pageInFile());
        QPDFObjectHandle handle = page.getObjectHandle();
        if (!handle.isDictionary()) {
            return facts;
        }

        // /MediaBox and /CropBox are inherited down the page tree and the other
        // three are not, which is why they are not read the same way. Absent
        // boxes stay absent rather than being filled in with what the
        // specification says they fall back to: "this page has no /TrimBox" and
        // "its /TrimBox happens to equal its /CropBox" are different documents
        // as far as a press is concerned.
        facts.boxes.media = boxRect(page.getAttribute("/MediaBox", false));
        facts.boxes.crop = boxRect(page.getAttribute("/CropBox", false));
        facts.boxes.bleed = boxRect(handle.getKey("/BleedBox"));
        facts.boxes.trim = boxRect(handle.getKey("/TrimBox"));
        facts.boxes.art = boxRect(handle.getKey("/ArtBox"));

        // What the reader sees is both turns at once: the one the file was
        // written with and the one the organiser has since added.
        facts.rotation = normalizeRotation(PdfGeometry::rotationOf(page) + rows.rotationOf(row));

        const QRectF visible = facts.boxes.crop.isValid() ? facts.boxes.crop : facts.boxes.media;
        facts.sizePoints = visible.size();
        if (facts.rotation % 180 != 0) {
            facts.sizePoints.transpose();
        }

        const QPDFObjectHandle annotations = handle.getKey("/Annots");
        if (annotations.isArray()) {
            for (int i = 0; i < annotations.getArrayNItems(); ++i) {
                const QPDFObjectHandle annotation = annotations.getArrayItem(i);
                if (!annotation.isDictionary()) {
                    continue;
                }
                const QPDFObjectHandle subtype = annotation.getKey("/Subtype");
                const std::string kind = subtype.isName() ? subtype.getName() : std::string();
                if (kind == "/Widget") {
                    ++facts.fields;
                } else if (kind == "/Link") {
                    ++facts.links;
                } else {
                    ++facts.comments;
                }
            }
        }

        std::set<QPDFObjGen> seen;
        facts.bytes = weighPage(handle, seen);
    } catch (const std::exception &) {
        // A page QPDF cannot read is a page with nothing to say about itself,
        // which is a far better panel than no window at all.
    }
    return facts;
}

// ── Reading what the document says about itself ───────────────────────────

/** Everything the document panel shows that costs nothing to find out. */
struct DocumentFacts {
    QString path; //!< where it will be written, or where it came from
    bool saved = false; //!< false while it lives only in this session
    qint64 fileBytes = 0;
    int pages = 0;
    int sources = 0;
    QString version;
    bool encrypted = false;
    QStringList forbidden; //!< what the protection asks readers not to do
    int bookmarks = 0;
    int formFields = 0;
    int attachments = 0;
    bool javaScript = false;
};

DocumentFacts readDocument(const Document *document)
{
    DocumentFacts facts;
    if (!document) {
        return facts;
    }
    facts.pages = document->pageCount();
    facts.sources = document->sourceCount();
    facts.bookmarks = int(document->outline().size());
    facts.path = document->filePath();
    facts.saved = !facts.path.isEmpty();

    // The file behind the first page, which is the one a merged document is
    // still mostly made of, and the only one whose version and protection can
    // be stated without averaging several files into a fiction.
    const SourcePage first
        = facts.pages > 0 ? SourcePage(document->pageAt(0).sourceId, document->pageAt(0).sourcePage) : SourcePage();
    Source *source = document->source(first.isValid() ? first.sourceId() : 0);
    if (!facts.saved && source) {
        facts.path = source->path();
    }
    if (!facts.path.isEmpty()) {
        facts.fileBytes = QFileInfo(facts.path).size();
    }
    if (!source) {
        return facts;
    }

    try {
        QPDF &pdf = source->qpdf();
        facts.version = QString::fromStdString(pdf.getPDFVersion());
        facts.encrypted = pdf.isEncrypted();
        if (facts.encrypted) {
            if (!pdf.allowPrintHighRes()) {
                facts.forbidden << i18nc("@item something a protected document asks readers not to do", "printing");
            }
            if (!pdf.allowExtractAll()) {
                facts.forbidden << i18nc("@item something a protected document asks readers not to do",
                                         "copying text out");
            }
            if (!pdf.allowModifyOther()) {
                facts.forbidden << i18nc("@item something a protected document asks readers not to do", "changes");
            }
            if (!pdf.allowModifyAnnotation()) {
                facts.forbidden << i18nc("@item something a protected document asks readers not to do", "comments");
            }
            if (!pdf.allowModifyForm()) {
                facts.forbidden << i18nc("@item something a protected document asks readers not to do",
                                         "filling in the form");
            }
            if (!pdf.allowModifyAssembly()) {
                facts.forbidden << i18nc("@item something a protected document asks readers not to do",
                                         "rearranging the pages");
            }
            if (!pdf.allowAccessibility()) {
                facts.forbidden << i18nc("@item something a protected document asks readers not to do",
                                         "reading aloud");
            }
        }

        QPDFObjectHandle root = pdf.getRoot();
        const QPDFObjectHandle form = root.getKey("/AcroForm");
        if (form.isDictionary()) {
            const QPDFObjectHandle fields = form.getKey("/Fields");
            if (fields.isArray()) {
                facts.formFields = fields.getArrayNItems();
            }
        }
        const QPDFObjectHandle names = root.getKey("/Names");
        facts.javaScript = names.isDictionary() && names.getKey("/JavaScript").isDictionary();

        // The shared helper rather than one of our own: building one validates
        // the whole EmbeddedFiles name tree, and this is read on every rebuild
        // of the panel.
        QPDFEmbeddedFileDocumentHelper &attachments = QPDFEmbeddedFileDocumentHelper::get(pdf);
        if (attachments.hasEmbeddedFiles()) {
            facts.attachments = int(attachments.getEmbeddedFiles().size());
        }
    } catch (const std::exception &) {
    }
    return facts;
}

// ── The dear half, off the GUI thread ─────────────────────────────────────

/** A file's fonts, indexed by the page that uses them. */
struct Inventory {
    QVector<FontUse> fonts;
    QHash<int, QVector<int>> byPage;
};

/**
 * Reads @p pages through. Runs on a worker thread and touches no widget.
 *
 * @p readThrough is false for the document's own panel, which wants the font
 * inventory and nothing else: that is one read per file however many pages
 * there are, while laying out the text of 180 pages is a second and parsing
 * their content streams is ten.
 */
DeepFacts survey(RenderBackend *backend, const QVector<SurveyPage> &pages, bool readThrough)
{
    DeepFacts facts;
    facts.pages = int(pages.size());

    QHash<QString, Inventory> inventories;
    QSet<QString> families;
    QSet<QString> loose;
    static const QRegularExpression gap(u"\\s+"_s);

    for (const SurveyPage &page : pages) {
        if (page.file.isEmpty()) {
            continue;
        }

        auto found = inventories.find(page.file);
        if (found == inventories.end()) {
            Inventory inventory;
            QString error;
            inventory.fonts = FontInventory::read(page.file, &error);
            // Turned round once per file rather than searched once per page:
            // a magazine has 864 font objects and 180 pages, and asking each
            // font whether it is on each page is thirty million comparisons.
            for (int i = 0; i < inventory.fonts.size(); ++i) {
                for (const int used : inventory.fonts.at(i).pages) {
                    inventory.byPage[used].append(i);
                }
            }
            found = inventories.insert(page.file, inventory);
        }

        for (const int index : found->byPage.value(page.pageInFile)) {
            const FontUse &font = found->fonts.at(index);
            const QString family = font.family.isEmpty() ? font.baseFont : font.family;
            if (family.isEmpty()) {
                continue;
            }
            families.insert(family);
            if (!font.embedded) {
                loose.insert(family);
            }
        }

        if (!readThrough) {
            continue;
        }

        if (backend) {
            const QString text = backend->extractText(page.sourceId, page.pageInFile);
            facts.words += int(text.split(gap, Qt::SkipEmptyParts).size());
            for (const QChar letter : text) {
                if (!letter.isSpace()) {
                    ++facts.characters;
                }
            }
        }

        QString error;
        const QVector<PageObject> drawn = PageObjects::read(page.file, page.pageInFile, &error);
        if (drawn.isEmpty() && !error.isEmpty() && facts.trouble.isEmpty()) {
            facts.trouble = error;
        }
        for (const PageObject &object : drawn) {
            switch (object.kind) {
            case PageObject::Kind::Text:
                ++facts.textRuns;
                break;
            case PageObject::Kind::Image:
            case PageObject::Kind::InlineImage:
                ++facts.pictures;
                break;
            case PageObject::Kind::Path:
            case PageObject::Kind::Drawing:
            case PageObject::Kind::Shading:
                ++facts.shapes;
                break;
            }
        }
    }

    QStringList names = loose.values();
    names.sort(Qt::CaseInsensitive);
    facts.fontFamilies = int(families.size());
    facts.notEmbedded = names;
    facts.done = true;
    return facts;
}

// ── The shape both answers are built out of ───────────────────────────────

/**
 * Sections of caption-and-value rows in one scrolling column.
 *
 * Not QGroupBox: a dock this narrow spends a quarter of its width on frames,
 * and the sections are here to be skimmed past rather than to be looked at.
 */
class Sheet
{
public:
    explicit Sheet(QWidget *parent)
        : m_body(new QWidget(parent))
        , m_column(new QVBoxLayout(m_body))
    {
        m_column->setContentsMargins(0, 0, 0, 0);
    }

    QWidget *widget() const { return m_body; }

    void section(const QString &title)
    {
        if (!title.isEmpty()) {
            auto *heading = new QLabel(title, m_body);
            QFont bold = heading->font();
            bold.setBold(true);
            heading->setFont(bold);
            if (m_form) {
                m_column->addSpacing(10);
            }
            m_column->addWidget(heading);
        }
        m_form = new QFormLayout;
        m_form->setContentsMargins(0, 0, 0, 0);
        m_form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
        m_column->addLayout(m_form);
    }

    QLabel *row(const QString &caption, const QString &value)
    {
        auto *label = new QLabel(value, m_body);
        label->setWordWrap(true);
        label->setTextFormat(Qt::PlainText);
        // A size, a font's name or a path is something people retype elsewhere.
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        field(caption, label);
        return label;
    }

    void field(const QString &caption, QWidget *widget)
    {
        if (!m_form) {
            section(QString());
        }
        m_form->addRow(caption, widget);
    }

    /** A whole-width sentence rather than a row, for what has no caption. */
    QLabel *note(const QString &text)
    {
        auto *label = new QLabel(text, m_body);
        label->setWordWrap(true);
        label->setTextFormat(Qt::PlainText);
        label->setForegroundRole(QPalette::PlaceholderText);
        m_column->addWidget(label);
        return label;
    }

    void add(QWidget *widget) { m_column->addWidget(widget); }

    void finish() { m_column->addStretch(1); }

private:
    QWidget *m_body;
    QVBoxLayout *m_column;
    QFormLayout *m_form = nullptr;
};

/**
 * True when every chosen page answered the same thing.
 *
 * A bool rather than "the answer, or nothing": pages that all lack a /TrimBox
 * all answer nothing, and that is agreement; the earlier shape of this reported
 * five boxes as differing on a document whose pages were identical.
 */
bool allAgree(const QStringList &answers)
{
    for (const QString &answer : answers) {
        if (answer != answers.constFirst()) {
            return false;
        }
    }
    return true;
}

QString differs()
{
    return i18nc("@info a property whose value is not the same on every chosen page", "differs between pages");
}

QString stillCounting()
{
    return i18nc("@info a value that is still being worked out", "counting…");
}

QString turn(int degrees)
{
    switch (normalizeRotation(degrees)) {
    case 90:
        return i18nc("@info how far a page has been turned", "90° clockwise");
    case 180:
        return i18nc("@info how far a page has been turned", "upside down");
    case 270:
        return i18nc("@info how far a page has been turned", "90° anticlockwise");
    default:
        return i18nc("@info a page that has not been turned", "upright");
    }
}

} // namespace

// ══ The document ══════════════════════════════════════════════════════════

/**
 * What the document says about itself, and what it is made of.
 *
 * The four fields at the top are editable because they are the reason anybody
 * opens this panel: a title and an author are what a document is filed under
 * everywhere outside this program. They are pushed as SetMetadataCommand, the
 * same command the Document Properties dialog pushes, so that a change made
 * here and a change made there are one thing to undo rather than two.
 */
class DocumentProperties::AboutDocument : public Inspectable
{
public:
    explicit AboutDocument(DocumentProperties *owner)
        : m_owner(owner)
    {
    }

    QString kindName() const override
    {
        return i18nc("@title the kind of thing the properties panel is showing", "Document");
    }

    QString description() const override
    {
        return m_owner->m_document ? m_owner->m_document->displayName() : QString();
    }

    QWidget *buildEditor(QWidget *parent) override;

    /** Puts the counted fonts in, the panel having been built long before. */
    void counted(const DeepFacts &facts)
    {
        if (m_fonts) {
            m_fonts->setText(fontLine(facts));
        }
    }

private:
    QString fontLine(const DeepFacts &facts) const;

    DocumentProperties *m_owner;
    QPointer<QLabel> m_fonts;
};

QString DocumentProperties::AboutDocument::fontLine(const DeepFacts &facts) const
{
    if (!facts.done) {
        return stillCounting();
    }
    if (facts.fontFamilies == 0) {
        return i18nc("@info the document draws no text at all", "None");
    }
    if (facts.notEmbedded.isEmpty()) {
        return i18ncp("@info how many typefaces a document uses, all of them carried inside it",
                      "%1 typeface, embedded", "%1 typefaces, all embedded", facts.fontFamilies);
    }
    // The ones that are missing are named, because that is the list somebody has
    // to act on: a count alone leaves them opening the font tool to find out
    // which of forty typefaces will be substituted at the far end.
    return i18nc("@info typefaces the document names but does not carry the glyphs of", "%1. Not embedded: %2",
                 i18ncp("@item a count of typefaces", "%1 typeface", "%1 typefaces", facts.fontFamilies),
                 facts.notEmbedded.join(i18nc("@item separator in a list of names", ", ")));
}

QWidget *DocumentProperties::AboutDocument::buildEditor(QWidget *parent)
{
    // The panel built for the previous answer can still be alive for a moment
    // after this one, and nothing here may write into it.
    m_fonts = nullptr;

    Sheet sheet(parent);
    Document *document = m_owner->m_document;
    if (!document) {
        return sheet.widget();
    }
    const DocumentFacts facts = readDocument(document);

    // ── what it says about itself ─────────────────────────────────────────

    const auto text = [this, &sheet, document](const QString &caption, const QString &value,
                                               QString Metadata::Fields::*member, const QString &placeholder) {
        auto *edit = new QLineEdit(value, sheet.widget());
        if (!placeholder.isEmpty()) {
            edit->setPlaceholderText(placeholder);
        }
        // On finishing rather than on every keystroke: one undo step per title
        // is what Ctrl+Z is expected to walk back, not one per letter.
        QObject::connect(edit, &QLineEdit::editingFinished, sheet.widget(), [this, edit, document, member] {
            Metadata::Fields fields = document->metadata();
            const QString typed = edit->text().trimmed();
            if (fields.*member == typed) {
                return;
            }
            fields.*member = typed;
            m_owner->apply(fields);
        });
        sheet.field(caption, edit);
        return edit;
    };

    const Metadata::Fields fields = document->metadata();
    sheet.section(QString());
    text(i18nc("@label:textbox", "Title:"), fields.title, &Metadata::Fields::title, QString());
    text(i18nc("@label:textbox", "Author:"), fields.author, &Metadata::Fields::author, QString());
    text(i18nc("@label:textbox", "Subject:"), fields.subject, &Metadata::Fields::subject, QString());
    text(i18nc("@label:textbox", "Keywords:"), fields.keywords, &Metadata::Fields::keywords,
         i18nc("@info:placeholder", "Separated by commas"));
    sheet.note(i18n("Emptying a field removes it from the document entirely."));

    // ── the file ──────────────────────────────────────────────────────────

    sheet.section(i18nc("@title:group the file the document is or will become", "File"));
    sheet.row(i18nc("@label how many pages a document has", "Pages:"), QLocale().toString(facts.pages));
    if (facts.sources > 1) {
        sheet.row(i18nc("@label a document put together out of several files", "Assembled from:"),
                  i18ncp("@info", "%1 file", "%1 files", facts.sources));
    }
    if (facts.fileBytes > 0) {
        QLabel *size = sheet.row(i18nc("@label how big the file is on disk", "Size:"), formatBytes(facts.fileBytes));
        if (!facts.saved) {
            size->setToolTip(
                i18nc("@info:tooltip", "The file this document was opened from. What a save writes will differ."));
        }
    }
    if (!facts.version.isEmpty()) {
        sheet.row(i18nc("@label which revision of the PDF specification the file is written to", "PDF version:"),
                  facts.version);
    }
    if (!facts.path.isEmpty()) {
        QLabel *where
            = sheet.row(facts.saved ? i18nc("@label where the document is kept", "Location:")
                                    : i18nc("@label where an unsaved document was opened from", "Opened from:"),
                        QFileInfo(facts.path).absolutePath());
        where->setToolTip(facts.path);
    }
    if (!facts.saved) {
        sheet.note(i18n("This document has not been saved yet."));
    }

    // ── what it carries ───────────────────────────────────────────────────

    sheet.section(i18nc("@title:group what a document carries besides its pages", "Contents"));
    const QString none = i18nc("@info a document carries none of this", "None");
    sheet.row(i18nc("@label the table of contents", "Bookmarks:"),
              facts.bookmarks > 0 ? QLocale().toString(facts.bookmarks) : none);
    sheet.row(i18nc("@label fields to be filled in", "Form fields:"),
              facts.formFields > 0 ? i18ncp("@info", "%1 field", "%1 fields", facts.formFields) : none);
    sheet.row(i18nc("@label files carried inside the document", "Attachments:"),
              facts.attachments > 0 ? QLocale().toString(facts.attachments) : none);
    if (facts.javaScript) {
        // Only ever shown when it is there. It is the one item on this list that
        // is a reason to do something, and a row reading "None" every time is
        // how a warning stops being read.
        QLabel *script = sheet.row(i18nc("@label programs the document runs when it is opened", "JavaScript:"),
                                   i18nc("@info the document carries JavaScript", "Yes"));
        script->setToolTip(i18nc("@info:tooltip", "Clean Up under the Document menu removes it."));
    }
    m_fonts = sheet.row(i18nc("@label the typefaces a document uses", "Fonts:"), fontLine(m_owner->deepFacts()));
    m_fonts->setToolTip(i18nc("@info:tooltip",
                              "A typeface that is not embedded is drawn with whatever the reading program has "
                              "instead, which may not be the same shape or the same width."));

    // ── protection ────────────────────────────────────────────────────────

    if (facts.encrypted) {
        sheet.section(i18nc("@title:group the password and permissions on a document", "Protection"));
        sheet.row(i18nc("@label whether the file is encrypted", "Encrypted:"),
                  i18nc("@info the file needs a password to be read", "Yes"));
        sheet.row(i18nc("@label what a protected document asks readers not to do", "Not allowed:"),
                  facts.forbidden.isEmpty() ? i18nc("@info the protection restricts nothing", "Nothing in particular")
                                            : facts.forbidden.join(i18nc("@item separator in a list", ", ")));
        sheet.note(i18n("Permissions are a request to the reading program, not a lock. Any reader may ignore them."));
    }

    // ── where it came from ────────────────────────────────────────────────

    sheet.section(i18nc("@title:group what wrote the document and when", "Origin"));
    text(i18nc("@label:textbox the application the document was written in", "Created with:"), fields.creator,
         &Metadata::Fields::creator, QString());
    QLineEdit *producer = text(i18nc("@label:textbox the library that wrote the file", "Written by:"), fields.producer,
                               &Metadata::Fields::producer, QString());
    producer->setToolTip(i18nc("@info:tooltip",
                               "The software that wrote the file. Kept as it was rather than overwritten, so that "
                               "saving does not announce which tool you used."));

    const auto stamp = [this, &sheet, document](const QString &caption, const QDateTime &value,
                                                QDateTime Metadata::Fields::*member) {
        auto *edit = new QDateTimeEdit(sheet.widget());
        edit->setCalendarPopup(true);
        edit->setDisplayFormat(QLocale().dateTimeFormat(QLocale::ShortFormat));
        // A document that records no date must be able to go on recording none,
        // so the bottom of the range means "not set" here exactly as it does in
        // the dialog. Without it, merely looking at a file would date it.
        edit->setSpecialValueText(i18nc("@item no date set", "not recorded"));
        edit->setMinimumDateTime(QDateTime::fromSecsSinceEpoch(0));
        edit->setDateTime(value.isValid() ? value : edit->minimumDateTime());
        QObject::connect(edit, &QDateTimeEdit::editingFinished, sheet.widget(), [this, edit, document, member] {
            Metadata::Fields fields = document->metadata();
            const QDateTime typed = edit->dateTime() > edit->minimumDateTime() ? edit->dateTime() : QDateTime();
            if (fields.*member == typed) {
                return;
            }
            fields.*member = typed;
            m_owner->apply(fields);
        });
        sheet.field(caption, edit);
    };
    stamp(i18nc("@label:textbox", "Created:"), fields.created, &Metadata::Fields::created);
    stamp(i18nc("@label:textbox", "Last changed:"), fields.modified, &Metadata::Fields::modified);

    sheet.finish();
    return sheet.widget();
}

// ══ The chosen pages ══════════════════════════════════════════════════════

/**
 * The pages picked out in the organiser: what they measure, cost and carry.
 *
 * The rule for several pages at once is that a number is either true of all of
 * them or is not shown as a number at all. A panel that answered "A4" for a
 * selection containing one A3 sheet would be worse than one that answered
 * nothing, because it would be believed.
 */
class DocumentProperties::AboutPages : public Inspectable
{
public:
    explicit AboutPages(DocumentProperties *owner)
        : m_owner(owner)
    {
    }

    QString kindName() const override
    {
        // Written out rather than plural-formed: the heading carries no number
        // of its own (description() has the range), and a plural form with
        // nothing to count is a message translators cannot fill in.
        return m_owner->pageFacts().size() == 1
            ? i18nc("@title the kind of thing the properties panel is showing", "Page")
            : i18nc("@title the kind of thing the properties panel is showing", "Pages");
    }

    QString description() const override { return PageRange::format(m_owner->chosenRows()); }

    QWidget *buildEditor(QWidget *parent) override;

    /** Puts the counted numbers in, the panel having been built long before. */
    void counted(const DeepFacts &facts);

private:
    DocumentProperties *m_owner;
    QPointer<QLabel> m_words;
    QPointer<QLabel> m_drawn;
    QPointer<QLabel> m_fonts;
    QPointer<QPushButton> m_count;
};

void DocumentProperties::AboutPages::counted(const DeepFacts &facts)
{
    if (m_count) {
        m_count->setVisible(!facts.done && !m_owner->isCounting());
        m_count->setEnabled(!m_owner->isCounting());
    }
    const QString waiting = m_owner->isCounting() ? stillCounting() : QString();
    // Each number is a message of its own and joined afterwards. One message
    // carrying three of them can only be given one plural form, and a language
    // that counts differently from English would have to pick which of the three
    // it agreed with.
    const QString comma = i18nc("@item separator in a list of counts", ", ");
    if (m_words) {
        const QStringList parts {
            i18ncp("@item how many words are on the chosen pages", "%1 word", "%1 words", facts.words),
            i18ncp("@item letters, not counting spaces or line breaks", "%1 character", "%1 characters",
                   facts.characters),
        };
        m_words->setText(facts.done ? parts.join(comma) : waiting);
    }
    if (m_drawn) {
        const QStringList parts {
            i18ncp("@item text-showing operations on a page", "%1 text run", "%1 text runs", facts.textRuns),
            i18ncp("@item pictures drawn on a page", "%1 picture", "%1 pictures", facts.pictures),
            i18ncp("@item filled or stroked artwork on a page", "%1 shape", "%1 shapes", facts.shapes),
        };
        m_drawn->setText(facts.done ? parts.join(comma) : waiting);
    }
    if (m_fonts) {
        if (!facts.done) {
            m_fonts->setText(waiting);
        } else if (facts.fontFamilies == 0) {
            m_fonts->setText(i18nc("@info the pages draw no text at all", "None"));
        } else if (facts.notEmbedded.isEmpty()) {
            m_fonts->setText(i18ncp("@info typefaces used, all carried inside the document", "%1 typeface, embedded",
                                    "%1 typefaces, all embedded", facts.fontFamilies));
        } else {
            m_fonts->setText(
                i18nc("@info typefaces used, some of them not carried inside the document", "%1. Not embedded: %2",
                      i18ncp("@item a count of typefaces", "%1 typeface", "%1 typefaces", facts.fontFamilies),
                      facts.notEmbedded.join(i18nc("@item separator in a list of names", ", "))));
        }
    }
}

QWidget *DocumentProperties::AboutPages::buildEditor(QWidget *parent)
{
    m_words = nullptr;
    m_drawn = nullptr;
    m_fonts = nullptr;
    m_count = nullptr;

    Sheet sheet(parent);
    const QVector<PageFacts> &pages = m_owner->pageFacts();
    if (pages.isEmpty()) {
        return sheet.widget();
    }
    const bool single = pages.size() == 1;

    // ── the paper ─────────────────────────────────────────────────────────

    sheet.section(QString());
    QStringList sizes;
    QStringList turns;
    qint64 bytes = 0;
    int comments = 0;
    int links = 0;
    int fields = 0;
    const PageFacts *heaviest = &pages.first();
    for (const PageFacts &page : pages) {
        sizes << extent(page.sizePoints);
        turns << turn(page.rotation);
        bytes += page.bytes;
        comments += page.comments;
        links += page.links;
        fields += page.fields;
        if (page.bytes > heaviest->bytes) {
            heaviest = &page;
        }
    }

    const bool oneSize = allAgree(sizes);
    QLabel *size = sheet.row(i18nc("@label how big the paper is", "Size:"), oneSize ? sizes.constFirst() : differs());
    if (oneSize) {
        size->setToolTip(extentInPoints(pages.constFirst().sizePoints));
        const QString paper = paperName(pages.constFirst().sizePoints);
        if (!paper.isEmpty()) {
            sheet.row(i18nc("@label the name of a standard paper size, such as A4", "Paper:"), paper);
        }
    }

    sheet.row(i18nc("@label how far the page has been turned", "Turned:"),
              allAgree(turns) ? turns.constFirst() : differs());

    // ── what it weighs ────────────────────────────────────────────────────

    QLabel *weight
        = sheet.row(i18nc("@label how much of the file one page accounts for", "Weight:"), formatBytes(bytes));
    weight->setToolTip(i18nc("@info:tooltip",
                             "What the file spends on this page: its own drawing instructions plus the pictures and "
                             "fonts it uses. Something two pages share is counted for both, so the pages add up to a "
                             "little more than the file."));
    if (!single) {
        sheet.row(i18nc("@label which of the chosen pages costs the most", "Heaviest page:"),
                  i18nc("@info a page number and what it weighs", "%1, %2", QLocale().toString(heaviest->row + 1),
                        formatBytes(heaviest->bytes)));
    }

    // ── what is on it ─────────────────────────────────────────────────────

    sheet.section(i18nc("@title:group what has been put on the pages", "On the page"));
    const QString none = i18nc("@info the pages carry none of this", "None");
    sheet.row(i18nc("@label notes and marks made on the page", "Comments:"),
              comments > 0 ? QLocale().toString(comments) : none);
    sheet.row(i18nc("@label boxes to be filled in", "Form fields:"), fields > 0 ? QLocale().toString(fields) : none);
    sheet.row(i18nc("@label places on the page that can be clicked", "Links:"),
              links > 0 ? QLocale().toString(links) : none);

    m_words = sheet.row(i18nc("@label how much text is on the pages", "Text:"), QString());
    m_words->setToolTip(i18nc("@info:tooltip", "Characters do not count spaces or line breaks."));
    m_drawn = sheet.row(i18nc("@label what the page draws", "Drawn:"), QString());
    m_fonts = sheet.row(i18nc("@label the typefaces the pages use", "Fonts:"), QString());

    auto *count = new QPushButton(i18ncp("@action:button read the pages through and count what is on them",
                                         "Count what is on %1 page", "Count what is on %1 pages", int(pages.size())),
                                  sheet.widget());
    count->setToolTip(i18nc("@info:tooltip",
                            "Counting means reading every one of these pages through, which takes about a tenth of a "
                            "second per page. It happens in the background; the document stays usable."));
    QObject::connect(count, &QPushButton::clicked, sheet.widget(), [this] { m_owner->startCounting(); });
    sheet.add(count);
    m_count = count;

    // ── the boxes ─────────────────────────────────────────────────────────
    //
    // Last, and only where every chosen page agrees: four of the five are
    // invisible to everyone who is not sending the document to a press, and the
    // one that is not (the paper) has already been said at the top.

    sheet.section(i18nc("@title:group the five rectangles a PDF page carries", "Boxes"));
    const auto boxRow = [&sheet, &pages](const QString &caption, QRectF PageLayout::Boxes::*which) {
        QStringList measured;
        for (const PageFacts &page : pages) {
            const QRectF box = page.boxes.*which;
            measured << (box.isValid() ? extent(box.size()) : QString());
        }
        if (!allAgree(measured)) {
            sheet.row(caption, differs());
            return;
        }
        // An absent box is stated as absent rather than filled in with what the
        // specification says it falls back to; a prepress check needs the two
        // told apart.
        sheet.row(caption,
                  measured.constFirst().isEmpty() ? i18nc("@info the page does not carry this box at all", "Not set")
                                                  : measured.constFirst());
    };
    boxRow(i18nc("@label the sheet of paper itself", "MediaBox:"), &PageLayout::Boxes::media);
    boxRow(i18nc("@label the part of the sheet a reader is shown", "CropBox:"), &PageLayout::Boxes::crop);
    boxRow(i18nc("@label how far the ink runs past the cut", "BleedBox:"), &PageLayout::Boxes::bleed);
    boxRow(i18nc("@label where the finished page is cut", "TrimBox:"), &PageLayout::Boxes::trim);
    boxRow(i18nc("@label the meaningful content of the page", "ArtBox:"), &PageLayout::Boxes::art);

    // ── where it came from ────────────────────────────────────────────────
    //
    // Only when the document is more than one file. A page's number inside its
    // own file is not its number here, and saying so about a document that has
    // never been merged is noise.

    if (m_owner->m_document && m_owner->m_document->sourceCount() > 1) {
        sheet.section(i18nc("@title:group which file a page came out of", "Source"));
        QStringList files;
        for (const PageFacts &page : pages) {
            files << QFileInfo(page.file).fileName();
        }
        const QSet<QString> distinct(files.begin(), files.end());
        sheet.row(i18nc("@label the file a page was read from", "File:"),
                  allAgree(files) ? files.constFirst()
                                  : i18nc("@info pages taken from more than one file", "%1 different files",
                                          QLocale().toString(int(distinct.size()))));
        if (single) {
            sheet.row(i18nc("@label a page's number inside the file it came from, which is not its number here",
                            "Page in that file:"),
                      QLocale().toString(pages.first().page.pageInFile() + 1));
        }
    }

    counted(m_owner->deepFacts());
    sheet.finish();
    return sheet.widget();
}

// ══ Choosing between the two, and fetching what they need ═════════════════

DocumentProperties::DocumentProperties(Document *document, QAbstractItemView *pages, InspectorDock *panel,
                                       QObject *parent)
    : QObject(parent)
    , m_document(document)
    , m_pages(pages)
    , m_panel(panel)
    , m_aboutDocument(std::make_unique<AboutDocument>(this))
    , m_aboutPages(std::make_unique<AboutPages>(this))
    , m_settle(new QTimer(this))
{
    m_settle->setSingleShot(true);
    // A rubber band dragged across the organiser emits a selection change per
    // cell it touches, and re-reading 180 pages costs some fifty milliseconds.
    // Waiting for the selection to stand still is the difference between a grid
    // that drags smoothly and one that catches.
    m_settle->setInterval(100);
    connect(m_settle, &QTimer::timeout, this, &DocumentProperties::refresh);

    if (m_document) {
        m_pageRows.setDocument(m_document);
        connect(m_document, &Document::pagesInserted, this, &DocumentProperties::schedule);
        connect(m_document, &Document::pagesRemoved, this, &DocumentProperties::schedule);
        connect(m_document, &Document::pagesChanged, this, &DocumentProperties::schedule);
        connect(m_document, &Document::outlineChanged, this, &DocumentProperties::schedule);
        connect(m_document, &Document::filePathChanged, this, &DocumentProperties::schedule);
        connect(m_document, &Document::sourceAdded, this, &DocumentProperties::schedule);
        // Undoing a title has to put the old title back in the box it was typed
        // in. Straight to the panel rather than through schedule(), and as a
        // plain refresh rather than as refreshFrom(): the document saying
        // something different about itself is not the user clicking on it, and
        // must not take the panel away from the comment they have selected.
        //
        // Typing is safe from it. The panel skips a rebuild while the focus is
        // inside the editor, and a field the focus is not in is a field nobody
        // is halfway through a word in, which also means Ctrl+Z pressed inside
        // one of these boxes goes to the box, never to the document.
        connect(m_document, &Document::metadataChanged, this, [this] {
            if (m_panel) {
                m_panel->refresh();
            }
        });
        connect(m_document, &Document::wasReset, this, [this] {
            m_pageRows.setDocument(m_document);
            schedule();
        });
    }

    if (m_pages) {
        m_pages->installEventFilter(this);
        watchSelection();
    }

    connect(&m_counting, &QFutureWatcher<DeepFacts>::finished, this, [this] {
        const int answered = std::exchange(m_countingFor, -1);
        // An answer worked out for pages the user has since moved off is not an
        // answer about the pages in front of them. Dropped rather than shown,
        // and asked for again if the pages now chosen still want one.
        if (answered == m_generation) {
            m_deep = m_counting.result();
            m_aboutDocument->counted(m_deep);
            m_aboutPages->counted(m_deep);
            Q_EMIT countingChanged();
            return;
        }
        if (m_countWanted) {
            // Queued: setting a new future from inside the old one's own
            // finished signal is asking the watcher to change under itself.
            QMetaObject::invokeMethod(this, &DocumentProperties::startCounting, Qt::QueuedConnection);
        }
    });

    if (m_panel) {
        m_panel->addSource(this, this);
    }
    refresh();
}

DocumentProperties::~DocumentProperties()
{
    // The worker holds the render backend and the source files, both of which
    // the window is about to take down. One second of waiting at teardown is
    // the price of not reading them after they are gone.
    if (m_counting.isRunning()) {
        m_counting.waitForFinished();
    }
}

int DocumentProperties::automaticLimit()
{
    return 16;
}

bool DocumentProperties::isCounting() const
{
    return m_counting.isRunning();
}

Inspectable *DocumentProperties::inspected()
{
    if (!m_document || m_document->pageCount() == 0) {
        return nullptr;
    }
    return m_rows.isEmpty() ? static_cast<Inspectable *>(m_aboutDocument.get())
                            : static_cast<Inspectable *>(m_aboutPages.get());
}

void DocumentProperties::apply(const Metadata::Fields &fields)
{
    if (!m_document || !m_document->undoStack()) {
        return;
    }
    m_document->undoStack()->push(new SetMetadataCommand(m_document, fields));
}

bool DocumentProperties::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_pages && (event->type() == QEvent::Show || event->type() == QEvent::Hide)) {
        // Leaving the organiser leaves its rows selected, and a set of rows
        // nobody can see must not go on claiming the panel from the document
        // they went back to reading.
        watchSelection();
        schedule();
    }
    return QObject::eventFilter(watched, event);
}

void DocumentProperties::watchSelection()
{
    disconnect(m_selectionWatch);
    if (!m_pages || !m_pages->selectionModel()) {
        return;
    }
    m_selectionWatch = connect(m_pages->selectionModel(), &QItemSelectionModel::selectionChanged, this,
                               &DocumentProperties::schedule);
}

void DocumentProperties::schedule()
{
    m_settle->start();
}

void DocumentProperties::refresh()
{
    QVector<int> chosen;
    // Only while the organiser is on screen; see the note in eventFilter().
    if (m_pages && m_pages->isVisible() && m_pages->selectionModel()) {
        const QModelIndexList picked = m_pages->selectionModel()->selectedIndexes();
        for (const QModelIndex &index : picked) {
            if (index.column() == 0) {
                chosen.append(index.row());
            }
        }
        std::sort(chosen.begin(), chosen.end());
        chosen.erase(std::unique(chosen.begin(), chosen.end()), chosen.end());
    }

    const bool wasDocument = m_rows.isEmpty();
    m_rows = chosen;
    m_facts.clear();
    m_facts.reserve(chosen.size());
    for (const int row : std::as_const(chosen)) {
        m_facts.append(readPage(m_document, m_pageRows, row));
    }

    // Only when the pages to be read through are different ones. Turning a
    // sheet, renaming the file or going in and out of the organiser all bring us
    // here, and none of them changes a word on any page. Restarting the count
    // for those would throw away a finished answer and set a worker going again
    // every time somebody pressed the rotate button.
    const QByteArray key = countKey();
    if (key != m_countKey) {
        m_countKey = key;
        ++m_generation;
        m_deep = DeepFacts();
        // The document's own panel wants the font inventory and nothing else,
        // which is one read per file however many pages there are. The pages'
        // panel wants the text and the drawing too, and that is dear enough per
        // page to be worth asking about past a certain number.
        m_countWanted
            = m_document && m_document->pageCount() > 0 && (chosen.isEmpty() || chosen.size() <= automaticLimit());
        if (m_countWanted) {
            startCounting();
        }
    }

    if (m_panel) {
        // Named rather than left to be guessed, exactly as the window names the
        // overlay that took the click: the answer can be the same object as
        // last time with only its contents moved on.
        const bool nowDocument = m_rows.isEmpty();
        if (wasDocument != nowDocument || !m_rows.isEmpty()) {
            m_panel->refreshFrom(this);
        } else {
            m_panel->refresh();
        }
    }
}

/**
 * The pages a count would be over, as bytes that can be compared.
 *
 * Deliberately the pages *inside their files* rather than the rows: two rows
 * showing one duplicated page have one answer between them, and a row that has
 * merely slid up the list has not changed what is printed on it.
 */
QVector<SurveyPage> DocumentProperties::wantedPages() const
{
    QVector<SurveyPage> wanted;
    if (!m_document) {
        return wanted;
    }
    if (!m_rows.isEmpty()) {
        wanted.reserve(m_facts.size());
        for (const PageFacts &page : m_facts) {
            if (page.isValid()) {
                wanted.append({ page.page.sourceId(), page.page.pageInFile(), page.file });
            }
        }
        return wanted;
    }
    // Every page of the document, so that a font used only on page 90 is
    // counted; the inventory itself is read once per file regardless.
    wanted.reserve(m_document->pageCount());
    for (int row = 0; row < m_document->pageCount(); ++row) {
        const SourcePage page = m_pageRows.pageOf(row);
        if (page.isValid()) {
            wanted.append({ page.sourceId(), page.pageInFile(), m_pageRows.fileOf(page) });
        }
    }
    return wanted;
}

QByteArray DocumentProperties::countKey() const
{
    QByteArray key = m_rows.isEmpty() ? "document" : "pages";
    for (const SurveyPage &page : wantedPages()) {
        key += ' ' + QByteArray::number(page.sourceId) + ':' + QByteArray::number(page.pageInFile);
    }
    return key;
}

void DocumentProperties::startCounting()
{
    if (!m_document) {
        return;
    }
    m_countWanted = true;
    if (m_counting.isRunning()) {
        // One at a time. The count in flight is for pages that have since been
        // replaced, and its finished handler starts this one over.
        return;
    }

    const QVector<SurveyPage> wanted = wantedPages();
    const bool readThrough = !m_rows.isEmpty();
    if (wanted.isEmpty()) {
        return;
    }

    RenderBackend *backend = m_document->renderBackend();
    m_countingFor = m_generation;
    m_counting.setFuture(
        QtConcurrent::run([backend, wanted, readThrough] { return survey(backend, wanted, readThrough); }));
    m_aboutDocument->counted(m_deep);
    m_aboutPages->counted(m_deep);
}

} // namespace ps
