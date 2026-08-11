/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "TextOverlay.h"

#include "core/Document.h"
#include "core/DocumentWriter.h"
#include "core/FontEmbedder.h"
#include "core/Source.h"
#include "render/PopplerBackend.h"

#include <KLocalizedString>

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QDir>
#include <QFocusEvent>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QPushButton>
#include <QRawFont>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QStyle>
#include <QStyleOptionFrame>
#include <QTimerEvent>
#include <QToolButton>
#include <QWidget>
#include <QtConcurrentRun>

#include <algorithm>
#include <cmath>

using namespace Qt::Literals::StringLiterals;

namespace ps {

namespace {

/** How far past a run's own box a click still counts as hitting it, in points. */
constexpr double Slack = 1.5;

/** The paper under a line of text, which is what a replacement is drawn on. */
constexpr QColor Paper = QColor(255, 255, 255);

/** A run that can be typed into, one that cannot, and one that has been. */
constexpr QColor EditableOutline = QColor(120, 160, 220, 130);
constexpr QColor RefusedOutline = QColor(190, 190, 190);
constexpr QColor ChangedOutline = QColor(40, 150, 90);

/** Where a line that grew now lies over ground that was not its own. */
constexpr QColor Crowded = QColor(220, 130, 40, 70);
constexpr QColor CrowdedOutline = QColor(190, 100, 20);

/**
 * The paper under the caret while the line is over its neighbour.
 *
 * The editor is a widget, so it is painted over everything this overlay draws
 * and no wash laid on the page beneath would show through it. Colouring its own
 * paper is how the line being typed says the same thing.
 */
constexpr QColor CrowdedPaper = QColor(255, 240, 224);

/**
 * How narrow letters may be squeezed into a box that is being kept.
 *
 * The same fraction the engine is told through TextEdit::Options, from one
 * place, so that the narrowing on screen cannot drift away from the narrowing
 * in the file.
 */
constexpr double MinimumSqueeze = 0.6;

/**
 * How much of two runs' heights must line up before they count as one line.
 *
 * A superscript and the word it hangs off overlap a little and are not in each
 * other's way; a word and the word after it on the same line overlap almost
 * entirely.
 */
constexpr double SameLineShare = 0.5;

/** How far a line may reach past its own box before it counts as grown, in points. */
constexpr double GrowthTolerance = 1.0;

/**
 * How long the typing has to stop before the page is drawn from the file.
 *
 * Long enough that it does not happen between two keystrokes of ordinary
 * typing, short enough that somebody who has stopped to look at what they wrote
 * is looking at the truth by the time they have focused on it.
 */
constexpr int SettleMs = 300;

/** The margin every QLineEdit keeps at each side of its text, inside the style's. */
constexpr int EditorSideMargin = 2;

/**
 * The tail of a PostScript font name that says which cut it is.
 *
 * "Graphik-Semibold" and "Graphik-RegularItalic" are cuts of Graphik, and a
 * machine that has Graphik has all of them; asking Fontconfig for the whole
 * name finds nothing and asking it for the family finds the face.
 */
QString familyWithoutCut(const QString &name)
{
    static const QRegularExpression cut(
        u"[-,_ ]?(Thin|Extra ?Light|Ultra ?Light|Light|Book|Regular|Normal|Roman|Medium|Semi ?Bold|Demi ?Bold|Demi|"
        u"Extra ?Bold|Ultra ?Bold|Bold|Black|Heavy|Italic|Oblique|Cond(ensed)?|Ext(ended)?|MT|PS|Std|Pro)+$"_s,
        QRegularExpression::CaseInsensitiveOption);

    // Repeatedly, because a name carries its cut in pieces: "MyriadPro-BoldCond"
    // is Myriad, and one pass off the end leaves "MyriadPro-Bold".
    QString family = name;
    for (int pass = 0; pass < 6; ++pass) {
        const QString shorter = QString(family).remove(cut);
        if (shorter == family || shorter.isEmpty()) {
            break;
        }
        family = shorter;
    }
    return family.trimmed();
}

/**
 * Faces on this system worth trying for a run, closest first.
 *
 * The document's own name first, because a machine that has the typeface should
 * use it; then the family without its cut; then a metrically compatible stand-in
 * for the standard fourteen, which is what makes a Helvetica document come out
 * the right length rather than merely the right shape.
 */
QStringList substitutesFor(const TextEdit::Run &run)
{
    QStringList families;
    if (!run.fontFamily.isEmpty()) {
        families << run.fontFamily;
        const QString bare = familyWithoutCut(run.fontFamily);
        if (!bare.isEmpty() && bare != run.fontFamily) {
            families << bare;
        }
    }

    const QString plain = run.fontFamily.toLower().remove(u'-').remove(u' ');
    if (plain.contains(u"helvetica"_s) || plain.contains(u"arial"_s)) {
        families << u"Liberation Sans"_s << u"Nimbus Sans"_s;
    } else if (plain.contains(u"times"_s)) {
        families << u"Liberation Serif"_s << u"Nimbus Roman"_s;
    } else if (plain.contains(u"courier"_s)) {
        families << u"Liberation Mono"_s << u"Nimbus Mono PS"_s;
    }

    // Fontconfig answers these three, and they are what says "any serif" to a
    // machine whose fonts nobody here can know the names of.
    families << (run.fixedPitch ? u"monospace"_s : run.serif ? u"serif"_s : u"sans-serif"_s);
    return families;
}

/** A page of one file, as a key for what was worked out about it. */
QString pageKey(const QString &path, int page)
{
    return path + u'\n' + QString::number(page);
}

QString shortened(const QString &text)
{
    const QString one = text.simplified();
    return one.size() <= 70 ? one : one.left(69) + QChar(0x2026);
}

/** A value the panel shows but cannot yet change, still worth copying out of. */
QLabel *readOnlyValue(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return label;
}

/** A swatch of one colour, for a button that opens the colour picker. */
QIcon swatch(const QColor &colour)
{
    QPixmap pixmap(16, 16);
    pixmap.fill(colour);
    QPainter painter(&pixmap);
    painter.setPen(QPen(QColor(0, 0, 0, 90), 1));
    painter.drawRect(0, 0, 15, 15);
    return QIcon(pixmap);
}

/**
 * What the properties panel says about the line the caret is in, and what it
 * lets be changed about it.
 *
 * Holds a copy of the run rather than a pointer back into the overlay, and
 * reaches the overlay through a guarded pointer: the panel is rebuilt whenever
 * the caret moves, and the widget it built before may outlive the object that
 * built it by as long as the dock takes to delete it.
 *
 * ## Why the face is a choice and not a text field
 *
 * The page names a face like "Graphik-Semibold", which is a cut and not a family
 * and is not what this machine calls anything it has. Putting that name into a
 * font chooser would make it snap to some other family and read as though the
 * user had asked for that. So what the page says is shown as a fact, and asking
 * for something else is a separate, deliberate choice, which is also the truth
 * of it, because a different face has to be found, cut down and carried in the
 * document, and that is not the same act as reading one.
 */
class RunProperties : public Inspectable
{
public:
    RunProperties(TextOverlay *overlay, int row, int index, const TextEdit::Run &run, const QString &typed,
                  TextOverlay::Fitting fitting)
        : m_overlay(overlay)
        , m_row(row)
        , m_index(index)
        , m_run(run)
        , m_typed(typed)
        , m_fitting(fitting)
    {
    }

    QString kindName() const override
    {
        return i18nc("@title the kind of thing the properties panel is describing", "Text");
    }

    QString description() const override { return shortened(m_typed.isEmpty() ? m_run.text : m_typed); }

    QWidget *buildEditor(QWidget *parent) override
    {
        auto *widget = new QWidget(parent);
        auto *form = new QFormLayout(widget);
        form->setContentsMargins(0, 0, 0, 0);

        form->addRow(i18nc("@label:textbox the face the page sets this text in", "Set in:"),
                     readOnlyValue(m_run.fontFamily.isEmpty()
                                       ? i18nc("@info the page names no font for this text", "not named by the page")
                                       : m_run.fontFamily,
                                   widget));

        form->addRow(i18nc("@label:textbox the name the page gives a font, such as /F1", "Named:"),
                     readOnlyValue(m_run.fontResource.isEmpty()
                                       ? i18nc("@info the page names no font for this text", "not named by the page")
                                       : m_run.fontResource
                                           + (m_run.embedded ? i18nc("@item:intext the glyphs travel with the document",
                                                                     ", carried by the document")
                                                             : i18nc("@item:intext the glyphs are not in the document",
                                                                     ", not carried by the document")),
                                   widget));

        buildTypeControls(form, widget);

        form->addRow(i18nc("@label:textbox which page the text is on", "Page:"),
                     readOnlyValue(QLocale().toString(m_row + 1), widget));

        // The padlock beside the caret is where this is changed, but the caret
        // is only in one line at a time and the panel is where somebody looks
        // to find out what a line is going to do.
        form->addRow(i18nc("@label how the text's box behaves as it is typed into", "Box:"),
                     readOnlyValue(m_fitting == TextOverlay::Fitting::KeepBox
                                       ? i18nc("@item:intext the box is kept and the type narrowed into it",
                                               "kept; the type is narrowed to fit")
                                       : i18nc("@item:intext the box follows the text", "grows with the text"),
                                   widget));

        const QRectF box = m_run.rect.normalized();
        form->addRow(i18nc("@label:textbox where on the page the text sits", "Position:"),
                     readOnlyValue(i18nc("@item:intext a position and a size in points, from the bottom left",
                                         "%1, %2 pt · %3 × %4 pt", QLocale().toString(box.left(), 'f', 0),
                                         QLocale().toString(box.top(), 'f', 0), QLocale().toString(box.width(), 'f', 0),
                                         QLocale().toString(box.height(), 'f', 0)),
                                   widget));

        if (!m_typed.isEmpty() && m_typed != m_run.text) {
            auto *was = readOnlyValue(shortened(m_run.text), widget);
            was->setWordWrap(true);
            form->addRow(i18nc("@label:textbox what the line said before it was typed into", "Was:"), was);
        }

        if (!m_run.editable && !m_run.limitation.isEmpty()) {
            auto *why = new QLabel(m_run.limitation, widget);
            why->setWordWrap(true);
            form->addRow(why);
        }

        return widget;
    }

private:
    /**
     * The controls that change how the line is set.
     *
     * All of them write through the moment they are touched, because the widget
     * they live in is destroyed the instant the caret moves and anything held
     * back until an "apply" would be lost with it.
     */
    void buildTypeControls(QFormLayout *form, QWidget *widget)
    {
        const TextEdit::Format wanted = m_overlay ? m_overlay->formatOf(m_row, m_index) : TextEdit::Format();
        const double size = m_run.scaledSize > 0.0 ? m_run.scaledSize : m_run.fontSize;

        auto *face = new QComboBox(widget);
        face->addItem(i18nc("@item:inlistbox keep the typeface the page already uses", "the page's own face"),
                      QString());
        for (const QString &family : QFontDatabase::families()) {
            face->addItem(family, family);
        }
        face->setCurrentIndex(std::max(0, face->findData(wanted.family)));
        face->setEnabled(m_run.editable);
        form->addRow(i18nc("@label:listbox the typeface to set the line in", "Typeface:"), face);

        auto *points = new QDoubleSpinBox(widget);
        points->setDecimals(1);
        points->setRange(1.0, 600.0);
        points->setSingleStep(0.5);
        points->setSuffix(i18nc("@item:valuesuffix PostScript points, after a number", " pt"));
        points->setValue(wanted.size > 0.0 ? wanted.size : size);
        points->setEnabled(m_run.editable);
        form->addRow(i18nc("@label:spinbox size of the type", "Size:"), points);

        auto *cuts = new QWidget(widget);
        auto *cutsRow = new QHBoxLayout(cuts);
        cutsRow->setContentsMargins(0, 0, 0, 0);
        auto *bold = new QCheckBox(i18nc("@option:check weight of the type", "Bold"), cuts);
        auto *italic = new QCheckBox(i18nc("@option:check slant of the type", "Italic"), cuts);
        bold->setChecked(wanted.family.isEmpty() ? m_run.fontWeight >= 600 : wanted.bold);
        italic->setChecked(wanted.family.isEmpty() ? m_run.italic : wanted.italic);
        cutsRow->addWidget(bold);
        cutsRow->addWidget(italic);
        cutsRow->addStretch();
        form->addRow(cuts);

        // Only with a face chosen, because a cut is a different font programme
        // and the page's own is the one it carries: there is no bold of it to
        // reach for. Saying so with a disabled box beats a checkbox that does
        // nothing.
        const auto followFace = [bold, italic, face] {
            const bool chosen = !face->currentData().toString().isEmpty();
            bold->setEnabled(chosen);
            italic->setEnabled(chosen);
            bold->setToolTip(chosen ? QString()
                                    : i18nc("@info:tooltip",
                                            "The document carries one cut of its own face, so a bold or italic of it "
                                            "has to come from a typeface chosen above."));
            italic->setToolTip(bold->toolTip());
        };
        followFace();

        auto *colour = new QPushButton(widget);
        const QColor shown = wanted.colour.isValid() ? wanted.colour : m_run.colour;
        colour->setIcon(swatch(shown));
        colour->setText(shown.name(QColor::HexRgb));
        colour->setEnabled(m_run.editable);
        form->addRow(i18nc("@label:listbox colour of the type", "Colour:"), colour);

        auto *reset = new QPushButton(i18nc("@action:button", "Set as the page does"), widget);
        reset->setEnabled(!wanted.isEmpty());
        form->addRow(reset);

        // Captured by value, so that a widget outliving the object that built it
        // still knows which line on which page it is about.
        const QPointer<TextOverlay> overlay = m_overlay;
        const int row = m_row;
        const int index = m_index;
        const double own = size;

        const auto push = [overlay, row, index, face, points, bold, italic, colour, reset, own] {
            if (!overlay) {
                return;
            }
            TextEdit::Format format;
            format.family = face->currentData().toString();
            format.bold = bold->isChecked() && !format.family.isEmpty();
            format.italic = italic->isChecked() && !format.family.isEmpty();
            // Only when it differs: a size the user never touched must not turn
            // into a `Tf` of its own in the file.
            if (std::abs(points->value() - own) > 0.05) {
                format.size = points->value();
            }
            const QColor chosen = QColor(colour->text());
            if (chosen.isValid() && chosen != overlay->pageRunOf(row, index).colour) {
                format.colour = chosen;
            }
            overlay->setFormat(row, index, format);
            reset->setEnabled(!format.isEmpty());
        };

        QObject::connect(face, &QComboBox::currentIndexChanged, widget, [followFace, push] {
            followFace();
            push();
        });
        QObject::connect(points, &QDoubleSpinBox::valueChanged, widget, push);
        QObject::connect(bold, &QCheckBox::toggled, widget, push);
        QObject::connect(italic, &QCheckBox::toggled, widget, push);
        QObject::connect(colour, &QPushButton::clicked, widget, [colour, push, widget] {
            const QColor picked
                = QColorDialog::getColor(QColor(colour->text()), widget, i18nc("@title:window", "Colour of this line"));
            if (!picked.isValid()) {
                return;
            }
            colour->setIcon(swatch(picked));
            colour->setText(picked.name(QColor::HexRgb));
            push();
        });
        QObject::connect(reset, &QPushButton::clicked, widget,
                         [overlay, row, index, face, points, bold, italic, colour, own, reset] {
                             if (!overlay) {
                                 return;
                             }
                             const QSignalBlocker one(face);
                             const QSignalBlocker two(points);
                             const QSignalBlocker three(bold);
                             const QSignalBlocker four(italic);
                             face->setCurrentIndex(0);
                             points->setValue(own);
                             const TextEdit::Run page = overlay->pageRunOf(row, index);
                             bold->setChecked(page.fontWeight >= 600);
                             italic->setChecked(page.italic);
                             colour->setIcon(swatch(page.colour));
                             colour->setText(page.colour.name(QColor::HexRgb));
                             overlay->setFormat(row, index, {});
                             reset->setEnabled(false);
                         });
    }

    QPointer<TextOverlay> m_overlay;
    int m_row;
    int m_index;
    TextEdit::Run m_run;
    QString m_typed;
    TextOverlay::Fitting m_fitting;
};

/**
 * Turns runs read from a source page into the space the view draws in.
 *
 * The engine reads a page as its own file has it; the view shows the page as
 * the organiser has since turned it. Without this, every box on a page someone
 * straightened would sit at ninety degrees to the words it belongs to.
 */
void applyRotation(QVector<TextEdit::Run> &runs, int rotation, const QSizeF &shown)
{
    const QTransform turn = pageTurn(rotation, shown);
    if (turn.isIdentity()) {
        return;
    }
    for (TextEdit::Run &run : runs) {
        run.rect = turn.mapRect(run.rect.normalized());
    }
}

} // namespace

TextOverlay::TextOverlay(PageView *view, Document *document, QObject *parent)
    : QObject(parent)
    , m_view(view)
    , m_document(document)
{
    if (m_view) {
        m_view->viewport()->installEventFilter(this);

        // The editor is a widget over the page rather than part of it, so
        // everything that moves the page under it has to move it too.
        connect(m_view->verticalScrollBar(), &QScrollBar::valueChanged, this, &TextOverlay::placeEditor);
        connect(m_view->horizontalScrollBar(), &QScrollBar::valueChanged, this, &TextOverlay::placeEditor);
        connect(m_view, &PageView::zoomChanged, this, [this] {
            placeEditor();
            // A render made for another size would be stretched, and softer
            // letters in the middle of a sharp page read as a mistake rather
            // than as a wait. So it is asked for again at the new size.
            scheduleLiveRender();
        });
        connect(m_view, &PageView::layoutChanged, this, &TextOverlay::placeEditor);
        connect(m_view, &PageView::modeChanged, this, [this](PageView::Mode mode) {
            if (mode != PageView::Mode::Edit) {
                finishEditing();
            }
        });
    }

    if (m_document) {
        m_rows.setDocument(m_document);

        // A replacement names a line by where it falls in one page's list of
        // instructions, and names the page by its row. Only the second half
        // moves when the organiser rearranges the document, and it moves in a way
        // that can be worked out, so the typing goes with its page instead of
        // being thrown away for it.
        //
        // Queued, because moving a page is a removal followed by an insertion:
        // between the two signals the page is nowhere at all, and answering that
        // state would announce as lost what the user only dragged elsewhere.
        connect(m_document, &Document::pagesInserted, this, &TextOverlay::rebaseRows, Qt::QueuedConnection);
        connect(m_document, &Document::pagesRemoved, this, &TextOverlay::rebaseRows, Qt::QueuedConnection);

        // A row that changed in place kept its number, so nothing can be lost by
        // it and it is answered at once.
        connect(m_document, &Document::pagesChanged, this, &TextOverlay::refreshRows);

        // A reset is a different document, and nothing typed about the old one
        // means anything about it.
        connect(m_document, &Document::wasReset, this, &TextOverlay::reload);
    }
}

TextOverlay::~TextOverlay()
{
    if (m_view) {
        m_view->removeOverlay(this);
    }
    // The worker writes into the scratch directory this owns, so it has to be
    // finished with before that directory goes.
    if (m_liveWatcher) {
        m_liveWatcher->waitForFinished();
    }
    delete m_editor;
    delete m_boxButton;
    delete m_note;
}

// ── What it is asked to do ────────────────────────────────────────────────

bool TextOverlay::appliesTo(PageView::Mode mode) const
{
    return mode == PageView::Mode::Edit;
}

Inspectable *TextOverlay::inspected()
{
    return m_inspected.get();
}

bool TextOverlay::hasEdits() const
{
    // Type set differently counts: a heading somebody has only made bigger is
    // as much an unsaved change as a corrected word, and a window that asks
    // "save?" for one and not the other loses work.
    return !m_edits.isEmpty() || !m_formats.isEmpty();
}

QVector<TextEdit::Replacement> TextOverlay::replacements() const
{
    return replacements(Fitting::Grow) + replacements(Fitting::KeepBox);
}

QVector<TextEdit::Replacement> TextOverlay::replacements(Fitting fitting) const
{
    // Every run somebody has said anything about, whether that was the words or
    // the type: the engine takes both in one operation, and a line that is only
    // to be set larger still has to name the words it keeps.
    QHash<int, QSet<int>> touched;
    for (auto page = m_edits.constBegin(); page != m_edits.constEnd(); ++page) {
        for (auto typed = page.value().constBegin(); typed != page.value().constEnd(); ++typed) {
            touched[page.key()].insert(typed.key());
        }
    }
    for (auto page = m_formats.constBegin(); page != m_formats.constEnd(); ++page) {
        for (auto set = page.value().constBegin(); set != page.value().constEnd(); ++set) {
            touched[page.key()].insert(set.key());
        }
    }

    QVector<TextEdit::Replacement> some;
    for (auto page = touched.constBegin(); page != touched.constEnd(); ++page) {
        const QVector<TextEdit::Run> runs = m_runs.value(page.key());
        const QSet<int> kept = m_keptBoxes.value(page.key());
        for (const int index : page.value()) {
            if (index < 0 || index >= runs.size()) {
                continue;
            }
            if (kept.contains(index) != (fitting == Fitting::KeepBox)) {
                continue;
            }
            TextEdit::Replacement replacement;
            // The engine addresses a run by its place in the page's instruction
            // stream, which is not its place in this list: a page that draws a
            // line of spaces has an operation the list leaves out.
            replacement.page = page.key();
            replacement.index = runs.at(index).index;
            replacement.text = m_edits.value(page.key()).value(index, runs.at(index).text);
            replacement.format = m_formats.value(page.key()).value(index);
            some.append(replacement);
        }
    }
    return some;
}

TextEdit::Options TextOverlay::optionsFor(Fitting fitting)
{
    TextEdit::Options options;
    // Fitting a replacement into the space its original occupied is precisely
    // what keeping the box means, and turning it off is precisely what letting
    // the box grow means: the engine has the switch, and this is the one place
    // that decides which way it is thrown.
    options.fitWidth = fitting == Fitting::KeepBox;
    options.minimumSqueeze = MinimumSqueeze;
    return options;
}

TextOverlay::Fitting TextOverlay::fittingOf(int row, int index) const
{
    return m_keptBoxes.value(row).contains(index) ? Fitting::KeepBox : Fitting::Grow;
}

// ── The page ──────────────────────────────────────────────────────────────

const QVector<TextEdit::Run> &TextOverlay::runsOf(int row)
{
    const auto cached = m_runs.constFind(row);
    if (cached != m_runs.constEnd()) {
        return cached.value();
    }

    QVector<TextEdit::Run> found;
    if (m_document && row >= 0 && row < m_document->pageCount()) {
        const PageRef ref = m_document->pageAt(row);
        if (const Source *source = m_document->source(ref.sourceId)) {
            found = TextEdit::runsOn(source->path(), ref.sourcePage, nullptr);
            applyRotation(found, ref.rotation, m_document->pageSizePoints(row));
        }
    }
    return *m_runs.insert(row, found);
}

QSet<QChar> TextOverlay::coverageFor(int row, const QString &fontResource)
{
    if (!m_document || fontResource.isEmpty() || row < 0 || row >= m_document->pageCount()) {
        return {};
    }
    const PageRef ref = m_document->pageAt(row);
    const Source *source = m_document->source(ref.sourceId);
    if (!source) {
        return {};
    }

    // Read once per file: this walks every font object in the document, which
    // is far too much work to repeat for each character somebody types.
    auto cached = m_fonts.constFind(source->path());
    if (cached == m_fonts.constEnd()) {
        cached = m_fonts.insert(source->path(), FontInventory::read(source->path(), nullptr));
    }
    const QVector<FontUse> &fonts = cached.value();

    // The same "/F1" is a different font on a different page, so the page has
    // to agree as well as the name.
    for (const FontUse &font : fonts) {
        if (font.resourceName == fontResource && font.pages.contains(ref.sourcePage)) {
            return font.coverage;
        }
    }
    return {};
}

int TextOverlay::runAt(int row, const QPointF &pagePoint)
{
    const QVector<TextEdit::Run> &runs = runsOf(row);
    // Backwards, so the run drawn last (the one on top) is the one hit.
    for (int i = int(runs.size()) - 1; i >= 0; --i) {
        if (runs.at(i).rect.normalized().adjusted(-Slack, -Slack, Slack, Slack).contains(pagePoint)) {
            return i;
        }
    }
    return -1;
}

// ── Drawing ───────────────────────────────────────────────────────────────

double TextOverlay::sizeOf(const TextEdit::Run &run)
{
    if (run.scaledSize > 0.0) {
        return run.scaledSize;
    }
    // A page may set its type through the text matrix and leave the font size
    // at nothing; the box it drew into still says how tall the letters came out,
    // and the box is a fifth taller than the type by construction.
    return run.fontSize > 0.0 ? run.fontSize : run.rect.normalized().height() / 1.2;
}

double TextOverlay::baselineOf(const TextEdit::Run &run)
{
    // The box runs from a quarter of the type size below the baseline upwards,
    // so that is where the letters sit. Putting them there rather than in the
    // middle of the box is the difference between type that stands on the page's
    // own line and type that floats a pixel or two above it.
    return run.rect.normalized().top() + 0.25 * sizeOf(run);
}

void TextOverlay::readFaces(int row)
{
    if (!m_document || row < 0 || row >= m_document->pageCount()) {
        return;
    }
    const PageRef ref = m_document->pageAt(row);
    const Source *source = m_document->source(ref.sourceId);
    if (!source) {
        return;
    }
    const QString key = pageKey(source->path(), ref.sourcePage);
    if (m_facesRead.contains(key)) {
        return;
    }
    m_facesRead.insert(key);

    const QHash<QString, QByteArray> programmes
        = FontInventory::embeddedProgrammes(source->path(), ref.sourcePage, nullptr);
    for (auto programme = programmes.constBegin(); programme != programmes.constEnd(); ++programme) {
        // Asked before it is registered, because this answers the family name
        // without putting a second face of that name into the database.
        const QRawFont face(programme.value(), 12.0);
        if (!face.isValid() || face.familyName().isEmpty()) {
            // A Type 1 or a symbolic programme Qt will not read. Nothing is
            // lost that a substitute cannot cover.
            continue;
        }
        const QString family = face.familyName();
        if (!m_claimed.contains(family)) {
            if (QFontDatabase::addApplicationFontFromData(programme.value()) < 0) {
                continue;
            }
            m_claimed.insert(family);
        }
        m_faces.insert(key + programme.key(), family);
    }
}

QString TextOverlay::faceFor(int row, const TextEdit::Run &run)
{
    if (!m_document || !run.embedded || run.fontResource.isEmpty() || row < 0 || row >= m_document->pageCount()) {
        return {};
    }
    readFaces(row);
    const PageRef ref = m_document->pageAt(row);
    const Source *source = m_document->source(ref.sourceId);
    return source ? m_faces.value(pageKey(source->path(), ref.sourcePage) + run.fontResource) : QString();
}

TextEdit::Run TextOverlay::styledRun(int row, int index)
{
    const QVector<TextEdit::Run> &runs = runsOf(row);
    if (index < 0 || index >= runs.size()) {
        return {};
    }
    TextEdit::Run run = runs.at(index);
    const TextEdit::Format wanted = m_formats.value(row).value(index);
    if (!wanted.family.isEmpty()) {
        run.fontFamily = wanted.family;
        run.fontWeight = wanted.bold ? 700 : 400;
        run.italic = wanted.italic;
        // Not the page's face any more, so its embedded glyphs are not the ones
        // to draw with: the chosen family is looked for on this system, exactly
        // as the engine will look for it when the file is written.
        run.embedded = false;
    }
    if (wanted.size > 0.0) {
        run.scaledSize = wanted.size;
    }
    if (wanted.colour.isValid()) {
        run.colour = wanted.colour;
    }
    return run;
}

QFont TextOverlay::fontOf(int row, int index)
{
    return fontFor(row, styledRun(row, index));
}

TextEdit::Format TextOverlay::formatOf(int row, int index) const
{
    return m_formats.value(row).value(index);
}

TextEdit::Run TextOverlay::pageRunOf(int row, int index)
{
    const QVector<TextEdit::Run> &runs = runsOf(row);
    return index >= 0 && index < runs.size() ? runs.at(index) : TextEdit::Run();
}

void TextOverlay::setFormat(int row, int index, const TextEdit::Format &format)
{
    const QVector<TextEdit::Run> &runs = runsOf(row);
    if (index < 0 || index >= runs.size()) {
        return;
    }

    TextEdit::Format wanted = format;
    if (!wanted.family.isEmpty()) {
        FontEmbedder::Request request;
        request.family = wanted.family;
        request.bold = wanted.bold;
        request.italic = wanted.italic;
        if (!FontEmbedder::isAvailable(request)) {
            // Refused here rather than at the far end of a save: a face that
            // cannot be cut down and carried is a page of empty boxes, and
            // finding that out in the finished file is finding it out too late.
            Q_EMIT refused(i18nc("@info %1 is a font family the user chose",
                                 "“%1” cannot be put into a PDF from this system, so the text keeps the face it "
                                 "has. Only fonts in TrueType or OpenType form can travel with a document.",
                                 wanted.family));
            wanted.family.clear();
        }
    }

    if (wanted.isEmpty()) {
        const auto page = m_formats.find(row);
        if (page == m_formats.end() || page->remove(index) == 0) {
            return;
        }
        if (page->isEmpty()) {
            m_formats.erase(page);
        }
    } else {
        if (m_formats.value(row).value(index).family == wanted.family
            && qFuzzyCompare(m_formats.value(row).value(index).size + 1.0, wanted.size + 1.0)
            && m_formats.value(row).value(index).colour == wanted.colour
            && m_formats.value(row).value(index).bold == wanted.bold
            && m_formats.value(row).value(index).italic == wanted.italic) {
            return;
        }
        m_formats[row].insert(index, wanted);
    }

    // The editor is the line, so it changes with it; and the page under it has
    // to be drawn again, because the box the line takes has just moved.
    if (row == m_editRow && index == m_editRun) {
        QPalette palette = m_editor->palette();
        palette.setColor(QPalette::Text, styledRun(row, index).colour);
        m_editor->setPalette(palette);
        placeEditor();
        updateAdvice();
    }
    scheduleLiveRender();
    Q_EMIT editsChanged();
    if (m_view) {
        m_view->viewport()->update();
    }
}

QFont TextOverlay::fontFor(int row, const TextEdit::Run &run)
{
    QFont font;
    const QString embedded = faceFor(row, run);
    if (!embedded.isEmpty()) {
        // The document's own glyphs. The cut is the face (an italic subset is
        // an upright face called "…-Italic" as far as Qt is concerned), so
        // asking for italic on top of it would have Qt slant it a second time.
        font.setFamilies({ embedded });
    } else {
        font.setFamilies(substitutesFor(run));
        font.setStyleHint(run.fixedPitch ? QFont::TypeWriter : run.serif ? QFont::Serif : QFont::SansSerif);
        font.setWeight(QFont::Weight(std::clamp(run.fontWeight, 1, 1000)));
        font.setItalic(run.italic);
    }

    const double zoom = m_view && m_view->zoom() > 0.0 ? m_view->zoom() : 1.0;
    const double pixels = std::max(0.5, sizeOf(run) * zoom);

    // QFont counts pixels in whole numbers only, and body type at an ordinary
    // zoom rounded to the nearest pixel is out by up to a twentieth, which is
    // the difference between type that matches the page and type that nearly
    // does. A point size against the screen's own resolution is the only way to
    // ask Qt for a fractional height.
    const double dpi = m_view ? std::max(1.0, double(m_view->logicalDpiY())) : 96.0;
    font.setPointSizeF(pixels * 72.0 / dpi);

    // Hinting rounds every glyph's advance to a whole pixel, and a line of forty
    // of them ends up several pixels from where the page puts it. A rasteriser
    // drawing the same page does not round, so neither does this: the letters
    // come out a shade softer and land where the page's letters are.
    font.setHintingPreference(QFont::PreferNoHinting);

    if (!qFuzzyCompare(run.horizontalScale, 1.0) && run.horizontalScale > 0.0) {
        font.setStretch(std::clamp(int(std::lround(run.horizontalScale * 100.0)), 1, 4000));
    }
    // The page's own tracking, in the pixels it comes to on screen. Without it a
    // line set loose measures short here and the caret drifts along it.
    double tracking = run.charSpacing * run.horizontalScale * zoom;
    if (!qFuzzyIsNull(run.wordSpacing)) {
        font.setWordSpacing(run.wordSpacing * run.horizontalScale * zoom);
    }

    // What the page's own widths make of this line against what the face on this
    // machine makes of it. A subset carries the same outlines but a document
    // states its own `/Widths`, and a substitute agrees with neither, so the
    // difference is spread over the letters, and the line ends where the page
    // ends it. Beyond a sixth of the type size the two faces are too unalike for
    // tracking to rescue, and stretching it that far would look worse than being
    // wrong at the end of the line.
    if (!run.text.isEmpty() && run.rect.normalized().width() > 0.0) {
        QFont measuring = font;
        measuring.setLetterSpacing(QFont::AbsoluteSpacing, tracking);
        const double natural = QFontMetricsF(measuring).horizontalAdvance(run.text);
        const double wanted = run.rect.normalized().width() * zoom;
        const double perLetter = (wanted - natural) / double(run.text.size());
        if (natural > 0.0 && std::abs(perLetter) < pixels / 6.0) {
            tracking += perLetter;
        }
    }
    if (!qFuzzyIsNull(tracking)) {
        font.setLetterSpacing(QFont::AbsoluteSpacing, tracking);
    }
    return font;
}

int TextOverlay::caretAt(int row, int index, double pageX)
{
    const QVector<TextEdit::Run> &runs = runsOf(row);
    if (index < 0 || index >= runs.size()) {
        return 0;
    }
    const TextEdit::Run &run = runs.at(index);
    const QString text = m_edits.value(row).value(index, run.text);
    if (text.isEmpty()) {
        return 0;
    }

    const double zoom = m_view && m_view->zoom() > 0.0 ? m_view->zoom() : 1.0;
    const double wanted = (pageX - run.rect.normalized().left()) * zoom;
    const QFontMetricsF metrics(fontFor(row, styledRun(row, index)));

    // The nearest boundary between two letters, which is what a caret is and
    // what every text field on the machine does with a click.
    int nearest = 0;
    double closest = std::abs(wanted);
    for (int upTo = 1; upTo <= int(text.size()); ++upTo) {
        const double distance = std::abs(wanted - metrics.horizontalAdvance(text.left(upTo)));
        if (distance < closest) {
            closest = distance;
            nearest = upTo;
        }
    }
    return nearest;
}

QFont TextOverlay::typeFor(int row, const TextEdit::Run &run, const QString &text, double available, Fitting fitting)
{
    QFont font = fontFor(row, run);
    if (fitting != Fitting::KeepBox) {
        // A line that has grown is longer, not thinner. The engine is told the
        // same through TextEdit::Options::fitWidth, so this is what the file
        // will say too.
        return font;
    }

    // Keeping the box is the engine's own fitting, and it stops narrowing at
    // MinimumSqueeze and lets whatever is still too long reach past. Showing
    // the line fitting neatly and then writing one that does not is the trap
    // this preview exists to close.
    const double natural = QFontMetricsF(font).horizontalAdvance(text);
    if (natural > available && available > 0.0) {
        const int floor = int(std::lround(MinimumSqueeze * 100.0));
        font.setStretch(std::max(floor, int(std::lround(available / natural * 100.0))));
    }
    return font;
}

QRectF TextOverlay::reachOf(int row, int index, const QString &text)
{
    const QVector<TextEdit::Run> &runs = runsOf(row);
    if (index < 0 || index >= runs.size() || !m_view) {
        return {};
    }
    QRectF box = runs.at(index).rect.normalized();

    // Measured with the very font that will be drawn, zoom and rounding and
    // all, then brought back to points: a reach worked out from other metrics
    // than the preview uses would mark collisions the eye cannot see.
    const double zoom = m_view->zoom() > 0.0 ? m_view->zoom() : 1.0;
    const QFont type = typeFor(row, styledRun(row, index), text, box.width() * zoom, fittingOf(row, index));
    box.setWidth(std::max(box.width(), QFontMetricsF(type).horizontalAdvance(text) / zoom));
    return box;
}

TextOverlay::Encroachment TextOverlay::encroachmentOf(int row, int index, const QRectF &reach)
{
    const QVector<TextEdit::Run> &runs = runsOf(row);
    if (index < 0 || index >= runs.size() || reach.isEmpty()) {
        return {};
    }
    const QRectF own = runs.at(index).rect.normalized();
    if (reach.right() <= own.right() + GrowthTolerance) {
        return {};
    }
    Encroachment result;
    result.over = QRectF(own.right(), reach.top(), reach.right() - own.right(), reach.height());

    // The nearest run to the right on the same line, because that is the one it
    // reaches first, and how far right the rest of the page's text goes, which
    // is as much of a text area as a file without paragraphs can be asked for.
    int hit = -1;
    bool haveOthers = false;
    double textRight = 0.0;
    for (int i = 0; i < runs.size(); ++i) {
        const QRectF other = runs.at(i).rect.normalized();
        if (i == index || other.isEmpty()) {
            continue;
        }
        textRight = haveOthers ? std::max(textRight, other.right()) : other.right();
        haveOthers = true;

        const double shared = std::min(own.bottom(), other.bottom()) - std::max(own.top(), other.top());
        if (shared <= SameLineShare * std::min(own.height(), other.height())) {
            continue;
        }
        if (other.left() < own.right() || other.left() >= reach.right()) {
            continue;
        }
        if (hit < 0 || other.left() < runs.at(hit).rect.normalized().left()) {
            hit = i;
        }
    }

    const QSizeF page = m_document ? m_document->pageSizePoints(row) : QSizeF();
    if (!page.isEmpty() && reach.right() > page.width()) {
        result.message = i18n("This line now reaches past the edge of the page, where nothing is printed.");
    } else if (hit >= 0) {
        result.message = i18nc("@info:status %1 is the text the edited line has grown over",
                               "This line now lies over “%1”. A PDF page has no paragraphs, so nothing beside it "
                               "moves out of the way.",
                               shortened(runs.at(hit).text));
    } else if (haveOthers && textRight >= own.right() - GrowthTolerance
               && reach.right() > textRight + GrowthTolerance) {
        // Only where the rest of the page had already drawn a boundary at least
        // as far right as this line. A run that was the widest thing on the page
        // to begin with has no margin to cross, and saying it had would be a
        // warning on the first letter of every title anybody corrects.
        result.message = i18n("This line now reaches into the margin, past where the rest of the page's text stops.");
    }
    return result;
}

void TextOverlay::paintEncroachment(QPainter &painter, int row, int index, const QString &text)
{
    if (!m_view) {
        return;
    }
    const Encroachment taken = encroachmentOf(row, index, reachOf(row, index, text));
    if (taken.message.isEmpty()) {
        return;
    }
    const QRectF box = m_view->fromPoints(row, taken.over);
    painter.fillRect(box, Crowded);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(CrowdedOutline, 1));
    painter.drawRect(box);
}

QColor TextOverlay::paperUnder(int row, const TextEdit::Run &run) const
{
    // Never from inside paint(): grabbing a widget paints it, and painting it
    // would come straight back here. It is sampled when the caret arrives and
    // kept, which is also the only moment the page under the line can still be
    // seen.
    if (!m_view || m_view->viewport()->paintingActive()) {
        return Paper;
    }
    // Sampled from the page rather than assumed: a magazine puts half its text
    // on tinted panels, and covering one of those with white is exactly the
    // "an input box appeared" the user complained about. Taken from just left of
    // the run, where the paper is whatever the line stands on.
    const QRectF box = m_view->fromPoints(row, run.rect.normalized());
    const QRect strip(int(std::floor(box.left())) - 4, int(std::floor(box.top())), 3,
                      std::max(1, int(std::ceil(box.height()))));
    if (!m_view->viewport()->rect().contains(strip)) {
        return Paper;
    }

    const QImage sample = m_view->viewport()->grab(strip).toImage();
    if (sample.isNull()) {
        return Paper;
    }

    // The commonest colour in the strip, because a sample of one pixel lands on
    // a rule or the edge of a letter often enough to matter.
    QHash<QRgb, int> seen;
    for (int y = 0; y < sample.height(); ++y) {
        for (int x = 0; x < sample.width(); ++x) {
            ++seen[sample.pixel(x, y)];
        }
    }
    QRgb best = Paper.rgb();
    int most = 0;
    for (auto colour = seen.constBegin(); colour != seen.constEnd(); ++colour) {
        if (colour.value() > most) {
            most = colour.value();
            best = colour.key();
        }
    }
    return QColor(best);
}

// ── What is shown is the file ─────────────────────────────────────────────

void TextOverlay::scheduleLiveRender()
{
    const int row = m_editRow >= 0 ? m_editRow : m_liveRow >= 0 ? m_liveRow : m_settleRow;
    if (row < 0 || (m_edits.value(row).isEmpty() && m_formats.value(row).isEmpty())) {
        return;
    }
    m_settleRow = row;

    // The render in hand is a picture of what the page said a keystroke ago, so
    // it goes at once rather than staying up until its replacement arrives.
    dropLiveRender();
    m_settle.start(SettleMs, this);
}

void TextOverlay::dropLiveRender()
{
    if (m_live.isNull() && m_liveRow < 0) {
        return;
    }
    m_live = QImage();
    m_liveRow = -1;
    m_liveZoom = 0.0;
}

void TextOverlay::timerEvent(QTimerEvent *event)
{
    if (event->timerId() == m_settle.timerId()) {
        m_settle.stop();
        startLiveRender();
        return;
    }
    QObject::timerEvent(event);
}

void TextOverlay::startLiveRender()
{
    if (!m_view || !m_document || !m_scratch.isValid() || m_settleRow < 0) {
        return;
    }
    const int row = m_settleRow;
    const QRect where = m_view->pageRect(row);
    if (where.isEmpty()) {
        return;
    }

    // Both fittings apart, because they are opposite settings of one engine
    // option: handing them to a single call would write one of the two groups
    // the way the page never showed it.
    const QVector<TextEdit::Run> &runs = runsOf(row);
    const QHash<int, QString> typed = m_edits.value(row);
    const QHash<int, TextEdit::Format> restyled = m_formats.value(row);
    QSet<int> touched(typed.keyBegin(), typed.keyEnd());
    for (auto set = restyled.constBegin(); set != restyled.constEnd(); ++set) {
        touched.insert(set.key());
    }

    QVector<TextEdit::Replacement> growing;
    QVector<TextEdit::Replacement> kept;
    for (const int index : std::as_const(touched)) {
        if (index < 0 || index >= runs.size()) {
            continue;
        }
        // Page zero: the file this is applied to holds that one page and
        // nothing else.
        const TextEdit::Replacement replacement { 0, runs.at(index).index, typed.value(index, runs.at(index).text),
                                                  restyled.value(index) };
        (fittingOf(row, index) == Fitting::KeepBox ? kept : growing).append(replacement);
    }
    if (growing.isEmpty() && kept.isEmpty()) {
        return;
    }

    // The page on its own, which is a few kilobytes against the document's
    // several megabytes, and takes about a millisecond to write.
    const quint64 generation = ++m_liveWanted;

    // The one before it is finished with (a render takes a tenth of a second
    // and these are a third of a second apart), and a session's worth of
    // one-page copies would otherwise sit in the temporary directory until the
    // program closed.
    for (const QString &stale :
         QDir(m_scratch.path()).entryList({ u"page-%1*.pdf"_s.arg(generation - 1) }, QDir::Files)) {
        QFile::remove(m_scratch.filePath(stale));
    }
    const QString extracted = m_scratch.filePath(u"page-%1.pdf"_s.arg(generation));
    if (!DocumentWriter::writeSelection(*m_document, { row }, extracted, {}, nullptr)) {
        return;
    }
    const QString grown = m_scratch.filePath(u"page-%1-grown.pdf"_s.arg(generation));
    const QString fitted = m_scratch.filePath(u"page-%1-fitted.pdf"_s.arg(generation));
    const int widthPx = std::max(1, int(std::lround(where.width() * m_view->devicePixelRatioF())));

    if (!m_liveWatcher) {
        m_liveWatcher = new QFutureWatcher<QImage>(this);
        connect(m_liveWatcher, &QFutureWatcher<QImage>::finished, this, &TextOverlay::liveRenderArrived);
    }

    m_liveRow = row;
    m_liveZoom = m_view->zoom();
    m_livePage = where.size();

    // Setting a new future lets go of the one before it: a render started for a
    // word that has since been typed over finishes into nothing, which costs
    // less than making the thread that has to keep drawing wait for it.
    m_liveWatcher->setFuture(QtConcurrent::run([extracted, grown, fitted, growing, kept, widthPx] {
        QString from = extracted;
        if (!growing.isEmpty()) {
            if (!TextEdit::apply(from, grown, growing, optionsFor(Fitting::Grow), nullptr, nullptr)) {
                return QImage();
            }
            from = grown;
        }
        if (!kept.isEmpty()) {
            if (!TextEdit::apply(from, fitted, kept, optionsFor(Fitting::KeepBox), nullptr, nullptr)) {
                return QImage();
            }
            from = fitted;
        }

        // The rasteriser is opened and let go inside the worker, so nothing here
        // shares a handle with the renders the view has in flight.
        PopplerBackend backend;
        if (!backend.addDocument(1, from, nullptr)) {
            return QImage();
        }
        return backend.renderPage(1, 0, widthPx);
    }));
}

void TextOverlay::liveRenderArrived()
{
    if (!m_liveWatcher) {
        return;
    }
    const QImage rendered = m_liveWatcher->result();
    if (rendered.isNull() || m_liveRow < 0) {
        dropLiveRender();
        return;
    }
    m_live = rendered;
    if (m_view) {
        m_view->viewport()->update();
    }
}

bool TextOverlay::paintLive(QPainter &painter, int row, int index, const QRectF &reach)
{
    if (m_live.isNull() || row != m_liveRow || !m_view) {
        return false;
    }
    const QRect where = m_view->pageRect(row);
    // A zoom or a re-flow since the render was asked for makes it a picture of
    // a page at another size, and stretching it would show softer letters than
    // the ones beside it, which reads as a mistake rather than as a wait.
    if (where.size() != m_livePage || !qFuzzyCompare(m_view->zoom(), m_liveZoom)) {
        return false;
    }
    // And a render whose shape is not this page's shape is a picture of
    // something else, whatever its width says: blitting it would put a band of
    // the wrong part of the page over the line.
    const double scale = double(m_live.width()) / double(where.width());
    if (std::abs(m_live.height() - where.height() * scale) > 2.0) {
        return false;
    }

    // The run's own patch of that render, and nothing else: everything outside
    // it is what the page already draws, and blitting the lot would put a
    // second copy of the page over every other overlay.
    const QRectF box = reach.adjusted(-1.0, -1.0, 1.0, 1.0);
    const QRectF onPage = box.translated(-where.topLeft());
    const QRectF source(onPage.left() * scale, onPage.top() * scale, onPage.width() * scale, onPage.height() * scale);
    if (!m_live.rect().contains(source.toAlignedRect())) {
        return false;
    }

    painter.drawImage(box, m_live, source);
    const QVector<TextEdit::Run> &runs = runsOf(row);
    paintEncroachment(painter, row, index,
                      m_edits.value(row).value(index, index < runs.size() ? runs.at(index).text : QString()));
    return true;
}

void TextOverlay::paint(QPainter &painter, int row, const QRect &pageRect)
{
    Q_UNUSED(pageRect)
    const QVector<TextEdit::Run> &runs = runsOf(row);
    if (runs.isEmpty() || !m_view) {
        return;
    }
    const QHash<int, QString> typed = m_edits.value(row);

    for (int i = 0; i < runs.size(); ++i) {
        const TextEdit::Run &run = runs.at(i);
        const QRectF box = m_view->fromPoints(row, run.rect.normalized());
        if (box.isEmpty()) {
            continue;
        }
        // The line being typed into is covered by the editor widget itself,
        // which draws the caret and the text as they are entered. It has no
        // frame of its own, so the edge that says where the caret is has to be
        // drawn round it here.
        if (row == m_editRow && i == m_editRun) {
            if (m_editor && m_editor->isVisible()) {
                // What the growing line has taken from the page is drawn under
                // the caret while it is being typed, which is the moment it
                // costs something rather than the moment the file is opened
                // again.
                paintEncroachment(painter, row, i, m_editor->text());
                painter.setBrush(Qt::NoBrush);
                painter.setPen(m_crowded ? QPen(CrowdedOutline, 2)
                                         : QPen(m_view->palette().color(QPalette::Highlight), 1));
                painter.drawRect(QRectF(m_editor->geometry()).adjusted(-1.0, -1.0, 1.0, 1.0));
            }
            continue;
        }

        // A line somebody has only made larger has changed as surely as one they
        // have corrected, and has to be shown having changed.
        const auto changed = typed.constFind(i);
        const bool restyled = m_formats.value(row).contains(i);
        QRectF reach = box;
        if (changed != typed.constEnd() || restyled) {
            const QString text = typed.value(i, run.text);
            const TextEdit::Run styled = styledRun(row, i);
            const QFont font = typeFor(row, styled, text, box.width(), fittingOf(row, i));
            reach.setWidth(std::max(box.width(), QFontMetricsF(font).horizontalAdvance(text)));

            // A render of the page as edited, where one has settled: then what
            // is on the screen is the file rather than a drawing of it, which is
            // the only way "will it really look like this" has a true answer.
            // The drawing below is what stands in while the typing is still
            // going on, and for a page the rasteriser could not give.
            if (!paintLive(painter, row, i, reach)) {
                // Paper over the run's own box and no further: what the line has
                // grown across is still there in the file, and hiding it under
                // white would make the collision look like empty space.
                painter.fillRect(box.adjusted(-1.0, -1.0, 1.0, 1.0), m_papers.value(row).value(i, Paper));
                paintEncroachment(painter, row, i, text);

                // Invisible type (the recognised text under a scan) is drawn
                // by the page as nothing at all, and drawing it here would put
                // black letters across the picture of the paper.
                if (run.renderMode != 3 && run.renderMode != 7) {
                    painter.setFont(font);
                    painter.setPen(styled.colour);
                    // On the page's own baseline, which is where the letters
                    // stood before and where they will stand again: the size may
                    // have changed, but a `Tf` does not move a line.
                    painter.drawText(m_view->fromPoints(row, QPointF(run.rect.normalized().left(), baselineOf(run))),
                                     text);
                }
            }
        }

        painter.setBrush(Qt::NoBrush);
        if (!run.editable) {
            painter.setPen(QPen(RefusedOutline, 1, Qt::DotLine));
        } else if (changed != typed.constEnd() || restyled) {
            painter.setPen(QPen(ChangedOutline, 1));
        } else {
            painter.setPen(QPen(EditableOutline, 1));
        }
        // Round what the line has become rather than round where it began, so
        // that the outline is the answer to "how much room does this take now".
        painter.drawRect(reach.adjusted(-1.0, -1.0, 1.0, 1.0));
    }
}

// ── The mouse ─────────────────────────────────────────────────────────────

bool TextOverlay::press(int row, const QPointF &pagePoint, Qt::MouseButtons buttons)
{
    if (!(buttons & Qt::LeftButton) || !m_view) {
        return false;
    }
    hideNote();

    const int index = runAt(row, pagePoint);
    if (index < 0) {
        // Clicking off a line keeps what was typed into it, which is what
        // clicking out of a cell does in every table anyone has used.
        finishEditing();
        return false;
    }

    const TextEdit::Run run = runsOf(row).at(index);
    if (!run.editable) {
        finishEditing();
        // The refusal belongs here, before a word has been typed into a line
        // that was never going to take it.
        const QString why = run.limitation.isEmpty()
            ? i18n("This line cannot be changed: the page does not say enough about the font it uses.")
            : run.limitation;
        showNote(why, m_view->fromPoints(row, run.rect.normalized()).toAlignedRect());
        return true;
    }

    beginEdit(row, index, pagePoint);
    return true;
}

Qt::CursorShape TextOverlay::cursor(int row, const QPointF &pagePoint)
{
    const int index = runAt(row, pagePoint);
    if (index < 0) {
        return Qt::ArrowCursor;
    }

    // Whether a run's font can be written into is settled when the page is
    // read, so the refusal is known before the pointer arrives, and the pointer
    // is where somebody looks for it. Saving the reason for the click would be
    // making them ask a question this already has the answer to.
    return runsOf(row).at(index).editable ? Qt::IBeamCursor : Qt::ForbiddenCursor;
}

// ── The clipboard, while a line is being typed into ───────────────────────

bool TextOverlay::copy()
{
    // The editor's own handling, not the page's: inside a line, Ctrl+C means the
    // letters that are selected, and doing nothing when none are is what every
    // other text field on the machine does.
    if (m_editRow < 0 || !m_editor) {
        return false;
    }
    m_editor->copy();
    return true;
}

bool TextOverlay::cut()
{
    if (m_editRow < 0 || !m_editor) {
        return false;
    }
    // QLineEdit reports a cut as text the user edited, so what is left of the
    // line is recorded exactly as if it had been typed.
    m_editor->cut();
    return true;
}

bool TextOverlay::paste(const QMimeData *data, int row)
{
    Q_UNUSED(data)
    Q_UNUSED(row)
    if (m_editRow < 0 || !m_editor) {
        return false;
    }
    // Asked of the clipboard again rather than of @p data: a paste into a line
    // is plain text, whatever else the clipboard may be carrying alongside it.
    m_editor->paste();
    return true;
}

// ── Editing in place ──────────────────────────────────────────────────────

void TextOverlay::ensureWidgets()
{
    if (m_editor || !m_view) {
        return;
    }

    m_editor = new QLineEdit(m_view->viewport());
    m_editor->setFrame(false);
    m_editor->setTextMargins(0, 0, 0, 0);
    m_editor->setAutoFillBackground(true);

    // Paper under it and ink on it, because it stands in for a line of the
    // page: a themed input box in the middle of a document would announce
    // itself as a widget, which is the impression this exists to avoid.
    QPalette palette = m_editor->palette();
    palette.setColor(QPalette::Base, Paper);
    palette.setColor(QPalette::Text, Qt::black);
    m_editor->setPalette(palette);
    m_editor->hide();
    m_editor->installEventFilter(this);
    connect(m_editor, &QLineEdit::textEdited, this, &TextOverlay::recordTyped);

    m_boxButton = new QToolButton(m_view->viewport());
    m_boxButton->setCheckable(true);
    m_boxButton->setAutoRaise(true);
    // It must not take the caret out of the line it is about, so it never takes
    // the focus: a click on it is a statement about the line, not a move away
    // from it.
    m_boxButton->setFocusPolicy(Qt::NoFocus);
    m_boxButton->hide();
    connect(m_boxButton, &QToolButton::toggled, this,
            [this](bool kept) { setFitting(kept ? Fitting::KeepBox : Fitting::Grow); });

    m_note = new QLabel(m_view->viewport());
    m_note->setWordWrap(true);
    m_note->setFrameShape(QFrame::StyledPanel);
    m_note->setMargin(4);
    m_note->setAutoFillBackground(true);
    QPalette notePalette = m_note->palette();
    notePalette.setColor(QPalette::Window, notePalette.color(QPalette::ToolTipBase));
    notePalette.setColor(QPalette::WindowText, notePalette.color(QPalette::ToolTipText));
    m_note->setPalette(notePalette);
    m_note->hide();
}

void TextOverlay::beginEdit(int row, int index, const QPointF &pagePoint)
{
    if (row == m_editRow && index == m_editRun) {
        // Already in this line: the click only moves the caret, exactly as a
        // second click inside any text box does.
        if (m_editor) {
            m_editor->setCursorPosition(caretAt(row, index, pagePoint.x()));
        }
        return;
    }
    finishEditing();

    const QVector<TextEdit::Run> &runs = runsOf(row);
    if (index < 0 || index >= runs.size() || !runs.at(index).editable || !m_view) {
        return;
    }
    const TextEdit::Run run = runs.at(index);

    ensureWidgets();

    // Sampled before the widget is put over the page, because that is the last
    // moment the page under it can still be seen. Text on a tinted panel (half
    // a magazine) used to be covered in white, which is precisely the "an input
    // box appeared" this is here to stop.
    m_paper = paperUnder(row, run);
    m_papers[row].insert(index, m_paper);
    QPalette palette = m_editor->palette();
    palette.setColor(QPalette::Base, m_paper);
    palette.setColor(QPalette::Text, run.colour);
    m_editor->setPalette(palette);

    m_editRow = row;
    m_editRun = index;

    {
        // Filling the editor is not the user typing, and recording it as such
        // would mark an untouched line as changed.
        QSignalBlocker blocker(m_editor);
        m_editor->setText(m_edits.value(row).value(index, run.text));
    }
    m_editor->show();
    updateBoxButton();
    placeEditor();
    m_editor->setFocus(Qt::MouseFocusReason);

    // The caret lands where the page was clicked rather than at the end of the
    // line, which is the difference between typing on a page and filling in a
    // box that happens to sit on one. Measured against the run's own font from
    // the run's own left edge: asking the widget would make the answer depend
    // on where the widget sits and how its style insets its text.
    m_editor->setCursorPosition(caretAt(row, index, pagePoint.x()));

    updateAdvice();

    m_inspected = std::make_unique<RunProperties>(this, row, index, run, m_edits.value(row).value(index),
                                                  fittingOf(row, index));
    Q_EMIT inspectionChanged();
    m_view->viewport()->update();
}

void TextOverlay::endEdit(bool keepTyping)
{
    if (m_editRow < 0 || m_leaving) {
        return;
    }
    m_leaving = true;

    const int row = m_editRow;
    const int index = m_editRun;
    m_editRow = -1;
    m_editRun = -1;

    if (!keepTyping) {
        const auto page = m_edits.find(row);
        if (page != m_edits.end() && page->remove(index) > 0) {
            if (page->isEmpty()) {
                m_edits.erase(page);
            }
            Q_EMIT editsChanged();
        }
    }

    if (!keepTyping) {
        // Taking the words back takes the choice about the box back with them:
        // it was a decision about an edit that no longer exists.
        const auto kept = m_keptBoxes.find(row);
        if (kept != m_keptBoxes.end() && kept->remove(index) && kept->isEmpty()) {
            m_keptBoxes.erase(kept);
        }
    }

    if (m_editor) {
        m_editor->hide();
        m_editor->clear();
        if (m_crowded) {
            // Put the paper back, or the next line clicked into starts out
            // wearing a warning that belongs to this one.
            QPalette palette = m_editor->palette();
            palette.setColor(QPalette::Base, m_paper.isValid() ? m_paper : Paper);
            m_editor->setPalette(palette);
        }
    }
    if (m_boxButton) {
        m_boxButton->hide();
    }
    m_crowded = false;
    hideNote();

    if (m_settle.isActive()) {
        // Leaving the line is as clear a statement that the typing has stopped
        // as a pause is, and this is the moment the user is most likely to look
        // at what they have done.
        m_settle.stop();
        startLiveRender();
    } else if (m_edits.value(row).isEmpty()) {
        // Nothing left on this page to show, so what was rendered for it is a
        // picture of an edit that has been taken back.
        if (row == m_liveRow) {
            dropLiveRender();
        }
    }

    m_inspected.reset();
    Q_EMIT inspectionChanged();
    if (m_view) {
        m_view->viewport()->update();
    }
    m_leaving = false;
}

void TextOverlay::finishEditing()
{
    endEdit(true);
}

void TextOverlay::stepToNeighbour(int direction)
{
    if (m_editRow < 0) {
        return;
    }
    const int row = m_editRow;
    const int from = m_editRun;
    const QVector<TextEdit::Run> &runs = runsOf(row);

    // Round the page rather than stopping at its last line: tabbing is how
    // somebody checks a whole page, and a caret that dies at the bottom makes
    // them reach for the mouse again.
    for (int step = 1; step <= runs.size(); ++step) {
        const int next = ((from + direction * step) % runs.size() + runs.size()) % runs.size();
        if (runs.at(next).editable) {
            const QRectF box = runs.at(next).rect.normalized();
            finishEditing();
            beginEdit(row, next, box.center());
            return;
        }
    }
}

void TextOverlay::recordTyped(const QString &text)
{
    if (m_editRow < 0) {
        return;
    }
    const QVector<TextEdit::Run> &runs = runsOf(m_editRow);
    if (m_editRun < 0 || m_editRun >= runs.size()) {
        return;
    }

    // Kept on every keystroke rather than on leaving the line, so that text
    // somebody typed is in hand whenever they reach for Save.
    if (text == runs.at(m_editRun).text) {
        const auto page = m_edits.find(m_editRow);
        if (page != m_edits.end()) {
            page->remove(m_editRun);
            if (page->isEmpty()) {
                m_edits.erase(page);
            }
        }
    } else {
        m_edits[m_editRow].insert(m_editRun, text);
    }

    // Placed before it is judged: how far the line now reaches is measured from
    // the box it has just been given, not from the one it had a keystroke ago.
    placeEditor();
    updateAdvice();
    scheduleLiveRender();
    Q_EMIT editsChanged();

    // The line's own outline and whatever it has grown across are drawn on the
    // page under the editor, and neither follows the widget on its own.
    if (m_view) {
        m_view->viewport()->update();
    }
}

QString TextOverlay::characterRefusal(const TextEdit::Run &run, const QString &text)
{
    const QSet<QChar> coverage = coverageFor(m_editRow, run.fontResource);
    if (coverage.isEmpty()) {
        // Nothing is known about what the font holds, and a guess dressed up as
        // a warning would be worse than saying nothing.
        return {};
    }

    QStringList missing;
    for (const QChar &character : text) {
        // What the line already draws is on the page, whatever the font's own
        // tables were able to say about it.
        if (coverage.contains(character) || run.text.contains(character)) {
            continue;
        }
        const QString one(character);
        if (!missing.contains(one)) {
            missing.append(one);
        }
    }

    if (missing.isEmpty()) {
        return {};
    }
    return i18nc("@info:status %1 is a list of characters",
                 "This page's font does not contain %1, so the line will be left as it is.", missing.join(u", "_s));
}

void TextOverlay::updateAdvice()
{
    if (m_editRow < 0 || !m_editor) {
        return;
    }
    const QVector<TextEdit::Run> &runs = runsOf(m_editRow);
    if (m_editRun < 0 || m_editRun >= runs.size()) {
        return;
    }
    const TextEdit::Run run = runs.at(m_editRun);
    const QString text = m_editor->text();

    const Encroachment taken = encroachmentOf(m_editRow, m_editRun, reachOf(m_editRow, m_editRun, text));
    const bool crowded = !taken.message.isEmpty();
    if (crowded != m_crowded) {
        // The paper the caret sits on turns as the line starts covering
        // something else, which is the one signal that cannot be missed while
        // somebody is looking at what they are typing.
        m_crowded = crowded;
        QPalette palette = m_editor->palette();
        palette.setColor(QPalette::Base, m_crowded ? CrowdedPaper : (m_paper.isValid() ? m_paper : Paper));
        m_editor->setPalette(palette);
        if (m_view) {
            m_view->viewport()->update();
        }
    }

    // One line, and the font's refusal takes it: a line that cannot be written
    // at all is a bigger thing to know than a line that will sit too close to
    // its neighbour.
    const QString refusal = characterRefusal(run, text);
    const QString advice = refusal.isEmpty() ? taken.message : refusal;
    if (advice.isEmpty()) {
        hideNote();
        return;
    }
    showNote(advice, m_editor->geometry());
}

// ── Where the editor sits ─────────────────────────────────────────────────

void TextOverlay::placeEditor()
{
    if (m_editRow < 0 || !m_editor || !m_view) {
        return;
    }
    const QVector<TextEdit::Run> &runs = runsOf(m_editRow);
    if (m_editRun < 0 || m_editRun >= runs.size()) {
        return;
    }
    const TextEdit::Run &run = runs.at(m_editRun);

    const QRectF natural = m_view->fromPoints(m_editRow, run.rect.normalized());
    const QFont font = typeFor(m_editRow, styledRun(m_editRow, m_editRun), m_editor->text(), natural.width(),
                               fittingOf(m_editRow, m_editRun));
    m_editor->setFont(font);

    // Where a QLineEdit puts its text inside itself: the style's own inset, plus
    // the two pixels every one of them keeps at each side. Asked of the style
    // rather than assumed, and taken off the geometry, so that the letters in
    // the widget land on the letters the page drew rather than a few pixels
    // right of and below them, which is what made a click land two characters
    // from where it was aimed.
    QStyleOptionFrame option;
    option.initFrom(m_editor);
    option.rect = QRect(0, 0, 400, 100);
    option.lineWidth = 0;
    option.midLineWidth = 0;
    option.features = QStyleOptionFrame::None;
    const QRect inside = m_editor->style()->subElementRect(QStyle::SE_LineEditContents, &option, m_editor);
    const int padLeft = inside.left() + EditorSideMargin;
    const int padTop = inside.top();
    const int padBottom = 100 - inside.bottom() - 1;

    const QFontMetrics metrics(font);
    const QPointF baseline = m_view->fromPoints(m_editRow, QPointF(run.rect.normalized().left(), baselineOf(run)));

    // Exactly the font's own height inside, because a QLineEdit centres its one
    // line in whatever room it is given and only a snug fit makes that centring
    // land on the baseline rather than near it.
    QRect box(int(std::lround(baseline.x())) - padLeft, int(std::lround(baseline.y())) - metrics.ascent() - padTop,
              int(std::ceil(natural.width())) + 2 * padLeft, metrics.height() + padTop + padBottom);

    // The whole point: an added word makes the line longer at the size the page
    // sets it, and the editor is the line, so it takes the room the saved page
    // will take. A kept box only reaches past once narrowing has done all the
    // engine will let it do. Either way nothing is hidden behind a text field
    // that has quietly started scrolling.
    const double wanted = QFontMetricsF(font).horizontalAdvance(m_editor->text()) + 2.0 * padLeft + 4.0;
    box.setWidth(std::max(box.width(), int(std::ceil(wanted))));

    const int knob = std::clamp(box.height(), 14, 22);

    // Held inside the window rather than inside the page: a line that has grown
    // off the paper is exactly the one somebody has to be able to read back and
    // shorten again.
    const int limit = m_view->viewport()->width() - knob - 8;
    if (box.right() > limit) {
        box.setRight(std::max(box.left() + knob, limit));
    }

    m_editor->setGeometry(box);
    m_editor->setVisible(m_view->viewport()->rect().intersects(box));

    if (m_boxButton) {
        m_boxButton->setIconSize(QSize(knob - 4, knob - 4));
        m_boxButton->setGeometry(box.right() + 4, box.center().y() - knob / 2, knob, knob);
        m_boxButton->setVisible(m_editor->isVisible());
        m_boxButton->raise();
    }
    if (m_note && !m_note->isHidden()) {
        showNote(m_note->text(), box);
    }
}

void TextOverlay::updateBoxButton()
{
    if (!m_boxButton) {
        return;
    }
    const bool kept = m_editRow >= 0 && fittingOf(m_editRow, m_editRun) == Fitting::KeepBox;

    // Set here rather than in ensureWidgets() because it is not decoration: the
    // padlock is the whole statement of which of the two things this line is
    // going to do when it is written.
    QSignalBlocker blocker(m_boxButton);
    m_boxButton->setChecked(kept);
    m_boxButton->setIcon(QIcon::fromTheme(kept ? u"object-locked"_s : u"object-unlocked"_s));
    m_boxButton->setToolTip(kept ? i18nc("@info:tooltip",
                                         "This line keeps its box: the type is narrowed until the text fits inside it. "
                                         "Click to let the box grow with the text instead.")
                                 : i18nc("@info:tooltip",
                                         "This line's box grows with the text, at the size the page sets. "
                                         "Click to keep the box as it is and narrow the type into it instead."));
    m_boxButton->setAccessibleName(kept ? i18nc("@action:button", "Box kept, type narrowed to fit")
                                        : i18nc("@action:button", "Box grows with the text"));
}

void TextOverlay::setFitting(Fitting fitting)
{
    if (m_editRow < 0 || m_editRun < 0) {
        return;
    }
    if (fitting == Fitting::KeepBox) {
        m_keptBoxes[m_editRow].insert(m_editRun);
    } else {
        const auto page = m_keptBoxes.find(m_editRow);
        if (page != m_keptBoxes.end() && page->remove(m_editRun) && page->isEmpty()) {
            m_keptBoxes.erase(page);
        }
    }

    updateBoxButton();
    placeEditor();
    updateAdvice();

    const QVector<TextEdit::Run> &runs = runsOf(m_editRow);
    if (m_editRun < runs.size()) {
        m_inspected = std::make_unique<RunProperties>(this, m_editRow, m_editRun, runs.at(m_editRun),
                                                      m_edits.value(m_editRow).value(m_editRun), fitting);
        Q_EMIT inspectionChanged();
    }

    // Only when there is something typed to save: the switch decides how a
    // replacement is written, so changing it changes the pending edit, but on an
    // untouched line it has changed nothing anybody could save.
    if (m_edits.value(m_editRow).contains(m_editRun)) {
        Q_EMIT editsChanged();
    }
    if (m_view) {
        m_view->viewport()->update();
    }
}

void TextOverlay::showNote(const QString &text, const QRect &anchor)
{
    if (text.isEmpty() || !m_view) {
        hideNote();
        return;
    }
    ensureWidgets();

    const QRect viewport = m_view->viewport()->rect();
    const int width = std::min(420, std::max(120, viewport.width() - 16));
    m_note->setText(text);
    m_note->resize(width, m_note->heightForWidth(width));

    // Under the line it is about, unless that would put it off the bottom of
    // the window, where nobody would read it.
    QRect box = m_note->geometry();
    box.moveTopLeft(QPoint(anchor.left(), anchor.bottom() + 6));
    if (box.bottom() > viewport.bottom()) {
        box.moveBottom(anchor.top() - 6);
    }
    box.moveLeft(
        std::clamp(box.left(), viewport.left() + 8, std::max(viewport.left() + 8, viewport.right() - width - 8)));
    box.moveTop(std::max(viewport.top() + 8, box.top()));
    m_note->setGeometry(box);
    m_note->show();
    m_note->raise();
}

void TextOverlay::hideNote()
{
    if (m_note) {
        m_note->hide();
    }
}

// ── Keys, and the events the view does not send ───────────────────────────

bool TextOverlay::eventFilter(QObject *watched, QEvent *event)
{
    if (m_view && watched == m_view->viewport() && event->type() == QEvent::Resize) {
        // Resizing re-centres the pages, and the editor is not one of the
        // things the view moves when it does.
        placeEditor();
        return false;
    }

    if (m_editor && watched == m_editor) {
        if (event->type() == QEvent::KeyPress) {
            auto *key = static_cast<QKeyEvent *>(event);
            switch (key->key()) {
            case Qt::Key_Escape:
                endEdit(false);
                return true;
            case Qt::Key_Return:
            case Qt::Key_Enter:
                finishEditing();
                return true;
            case Qt::Key_Tab:
                stepToNeighbour(1);
                return true;
            case Qt::Key_Backtab:
                stepToNeighbour(-1);
                return true;
            default:
                break;
            }
        } else if (event->type() == QEvent::FocusOut) {
            // Switching to another window leaves the caret where it was; a
            // click elsewhere in this one ends the edit, keeping what was typed.
            const Qt::FocusReason reason = static_cast<QFocusEvent *>(event)->reason();
            if (reason != Qt::ActiveWindowFocusReason && reason != Qt::PopupFocusReason) {
                finishEditing();
            }
        }
    }

    return QObject::eventFilter(watched, event);
}

// ── Starting again ────────────────────────────────────────────────────────

void TextOverlay::discardEdits()
{
    endEdit(false);
    // Before the early return: a render may be up for a page whose corrections
    // were taken back while the caret was somewhere else entirely, and leaving
    // it there would go on showing an edit that no longer exists.
    m_settle.stop();
    m_settleRow = -1;
    dropLiveRender();

    if (m_edits.isEmpty() && m_keptBoxes.isEmpty() && m_formats.isEmpty()) {
        return;
    }
    const bool had = !m_edits.isEmpty() || !m_formats.isEmpty();
    m_edits.clear();
    m_formats.clear();
    m_keptBoxes.clear();
    if (had) {
        Q_EMIT editsChanged();
    }
    if (m_view) {
        m_view->viewport()->update();
    }
}

void TextOverlay::reload()
{
    endEdit(false);
    m_settle.stop();
    m_settleRow = -1;
    dropLiveRender();

    m_runs.clear();
    m_fonts.clear();
    m_rows.setDocument(m_document);

    const bool had = !m_edits.isEmpty() || !m_formats.isEmpty();
    m_edits.clear();
    m_formats.clear();
    m_keptBoxes.clear();
    if (had) {
        Q_EMIT editsChanged();
    }
    if (m_view) {
        m_view->viewport()->update();
    }
}

void TextOverlay::rebaseRows()
{
    carryRows(m_rows.follow());
}

void TextOverlay::refreshRows()
{
    carryRows(m_rows.followInPlace());
}

void TextOverlay::carryRows(const Rebase &change)
{
    if (change.isIdentity()) {
        return;
    }

    // Before anything is re-keyed, so that what is in the box under the caret is
    // already among the edits being carried across rather than in a widget
    // pointing at a row that is about to mean something else.
    endEdit(true);

    QHash<int, QVector<TextEdit::Run>> runs;
    for (auto page = m_runs.constBegin(); page != m_runs.constEnd(); ++page) {
        const int row = change.nowAt(page.key());
        if (row < 0) {
            continue;
        }
        // Dropped only for a row now standing for a different file's page, and
        // only when nothing was typed into it: what was typed is addressed by
        // its place in *this* list, so throwing the list away throws the typing
        // away with it.
        if (change.rewritten(page.key()) && m_edits.value(page.key()).isEmpty()) {
            continue;
        }
        QVector<TextEdit::Run> here = page.value();
        applyRotation(here, change.turnOf(page.key()), m_document ? m_document->pageSizePoints(row) : QSizeF());
        runs.insert(row, here);
    }

    // A run's place in its page's instruction stream is a fact about the page,
    // and the organiser does not rewrite pages, so only the row has to move.
    QHash<int, QHash<int, QString>> edits;
    int lost = 0;
    for (auto page = m_edits.constBegin(); page != m_edits.constEnd(); ++page) {
        const int row = change.nowAt(page.key());
        if (row < 0) {
            lost += int(page.value().size());
            continue;
        }
        edits.insert(row, page.value());
    }

    QHash<int, QHash<int, TextEdit::Format>> formats;
    for (auto page = m_formats.constBegin(); page != m_formats.constEnd(); ++page) {
        const int row = change.nowAt(page.key());
        if (row >= 0) {
            formats.insert(row, page.value());
        }
    }

    QHash<int, QSet<int>> kept;
    for (auto page = m_keptBoxes.constBegin(); page != m_keptBoxes.constEnd(); ++page) {
        const int row = change.nowAt(page.key());
        if (row >= 0) {
            kept.insert(row, page.value());
        }
    }

    const bool had = !m_edits.isEmpty();
    m_runs = runs;
    m_edits = edits;
    m_formats = formats;
    m_keptBoxes = kept;

    // Every row number the render was made against has just changed meaning.
    m_settle.stop();
    m_settleRow = -1;
    dropLiveRender();

    if (lost > 0) {
        Q_EMIT refused(i18ncp("@info %1 is a number of corrections the user had typed",
                              "One correction was on a page that has been taken out of the document, so it is gone.",
                              "%1 corrections were on pages that have been taken out of the document, so they are "
                              "gone.",
                              lost));
    }
    if (had != !m_edits.isEmpty()) {
        Q_EMIT editsChanged();
    }
    if (m_view) {
        m_view->viewport()->update();
    }
}

} // namespace ps
