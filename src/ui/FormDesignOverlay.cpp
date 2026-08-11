/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "FormDesignOverlay.h"

#include "AlignmentPanel.h"
#include "core/Core14Widths.h"
#include "core/Document.h"
#include "core/DocumentWriter.h"
#include "core/PdfFile.h"
#include "render/PopplerBackend.h"

#include <KLocalizedString>

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFontMetricsF>
#include <QFormLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QLineF>
#include <QLocale>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>
#include <QtConcurrentRun>

#include <qpdf/QPDF.hh>
#include <qpdf/QPDFObjectHandle.hh>

#include <algorithm>
#include <cmath>

using namespace Qt::Literals::StringLiterals;

namespace ps {

namespace {

using BuilderKind = FormBuilder::Field::Kind;

/** How near a handle counts as being on it, and how big it is drawn, in pixels. */
constexpr double HandleReach = 7.0;
constexpr double HandleSize = 8.0;

/** No field may be dragged smaller than this, or it can never be clicked again. */
constexpr double MinimumSizePoints = 6.0;

/** Under this, a drag was a click that wobbled and a new field takes its own size. */
constexpr double DragSlopPoints = 2.0;

/** Below this the name written inside a field would be unreadable, so it is left off. */
constexpr double CaptionFloorPixels = 9.0;

/**
 * How long the page has to be left alone before it is rendered as it will be saved.
 *
 * The same third of a second TextOverlay waits after the last keystroke, and for
 * the same reason: the render costs about a tenth of one, so it follows the
 * gesture rather than the mouse.
 */
constexpr int SettleMs = 300;

/** One measurement, written the way a page is measured: points, one decimal. */
QString points(double value)
{
    return QLocale().toString(value, 'f', 1);
}

// ── A field's own appearance, as FormBuilder writes it ────────────────────
//
// Everything from here to paintFace() is the screen's half of an agreement with
// FormBuilder: the same padding, the same fitted size, the same baseline, the
// same inset border, measured against the same advance widths out of
// Core14Widths.h. The point is not that the numbers are good ones, but that
// they are *its* numbers, so what the user drags round the page and what the
// file says afterwards are one picture rather than two guesses at it.

/** The resource names Acrobat has used for the standard fonts since 1996. */
struct FontAlias {
    const char *resource;
    const char *base;
};

constexpr FontAlias fontAliases[] = {
    { "Helv", "Helvetica" },
    { "HeBo", "Helvetica-Bold" },
    { "HeOb", "Helvetica-Oblique" },
    { "HeBO", "Helvetica-BoldOblique" },
    { "TiRo", "Times-Roman" },
    { "TiBo", "Times-Bold" },
    { "TiIt", "Times-Italic" },
    { "TiBI", "Times-BoldItalic" },
    { "Cour", "Courier" },
    { "CoBo", "Courier-Bold" },
    { "CoOb", "Courier-Oblique" },
    { "CoBO", "Courier-BoldOblique" },
    { "Symb", "Symbol" },
    { "ZaDb", "ZapfDingbats" },
};

QString baseFontOf(const QString &resource)
{
    for (const FontAlias &alias : fontAliases) {
        if (resource == QLatin1String(alias.resource)) {
            return QString::fromLatin1(alias.base);
        }
    }
    return resource;
}

/** How wide @p text sets, in points, by the widths every reader is required to know. */
double textWidthOf(const QString &text, const QString &baseFont, double size)
{
    const quint16 *widths = nullptr;
    int average = 500;
    for (const Core14::Entry &entry : Core14::table) {
        if (baseFont == QLatin1String(entry.name)) {
            widths = entry.widths;
            average = entry.averageWidth;
            break;
        }
    }

    double thousandths = 0.0;
    for (const QChar &character : text) {
        const int code = character.unicode();
        thousandths += widths && code >= Core14::firstCode && code <= Core14::lastCode
            ? widths[code - Core14::firstCode]
            : average;
    }
    return thousandths * size / 1000.0;
}

double paddingOf(const FormBuilder::Field &spec)
{
    return 1.0 + std::max(0.0, spec.borderWidth);
}

/** The size the text is set at, which is not always the size that was asked for. */
double effectiveSizeOf(const FormBuilder::Field &spec, double w, double h, const QString &text, const QString &baseFont)
{
    if (spec.fontSize > 0.0) {
        return spec.fontSize;
    }
    // `0 Tf` in /DA is the legal way of saying "reader, you fit it"; this is the
    // same answer FormBuilder gives to the same question.
    double size = spec.multiline ? 11.0 : std::clamp(h * 0.62, 4.0, 12.0);
    if (!spec.multiline && !text.isEmpty()) {
        const double available = std::max(1.0, w - 2.0 * paddingOf(spec));
        const double unitWidth = textWidthOf(text, baseFont, 1.0);
        if (unitWidth > 0.0 && size * unitWidth > available) {
            size = std::max(4.0, available / unitWidth);
        }
    }
    return size;
}

/**
 * The closest face this machine has to one of the standard fourteen.
 *
 * Asked for by the name the specification uses rather than by a family that
 * happens to be installed, because that is the name the rasteriser under the
 * page is going to resolve too. On a desktop with fontconfig both of them land
 * on the same substitute, which is what makes the letters on the screen and the
 * letters in the file the same letters.
 */
QFont faceFor(const QString &baseFont, double pixels)
{
    QFont font(baseFont.startsWith(QLatin1String("Times"))           ? u"Times"_s
                   : baseFont.startsWith(QLatin1String("Courier"))   ? u"Courier"_s
                   : baseFont.startsWith(QLatin1String("Helvetica")) ? u"Helvetica"_s
                                                                     : baseFont);
    font.setStyleHint(baseFont.startsWith(QLatin1String("Times"))         ? QFont::Serif
                          : baseFont.startsWith(QLatin1String("Courier")) ? QFont::TypeWriter
                                                                          : QFont::SansSerif);
    font.setBold(baseFont.contains(QLatin1String("Bold")));
    font.setItalic(baseFont.contains(QLatin1String("Italic")) || baseFont.contains(QLatin1String("Oblique")));
    font.setPixelSize(std::max(1, int(std::lround(pixels))));
    return font;
}

/** The two triangles that make a button look raised, or pressed. */
void paintBevel(QPainter &painter, const QRectF &box, double inset, const QColor &light, const QColor &dark)
{
    const double d = inset;
    // Written in the page's own space in FormBuilder, where y runs up; here y
    // runs down, so the pair swap over and the raised edge stays at the top.
    const QPolygonF top({ QPointF(box.left() + d, box.bottom() - d), QPointF(box.left() + d, box.top() + d),
                          QPointF(box.right() - d, box.top() + d), QPointF(box.right() - 2 * d, box.top() + 2 * d),
                          QPointF(box.left() + 2 * d, box.top() + 2 * d),
                          QPointF(box.left() + 2 * d, box.bottom() - 2 * d) });
    const QPolygonF bottom(
        { QPointF(box.right() - d, box.top() + d), QPointF(box.right() - d, box.bottom() - d),
          QPointF(box.left() + d, box.bottom() - d), QPointF(box.left() + 2 * d, box.bottom() - 2 * d),
          QPointF(box.right() - 2 * d, box.bottom() - 2 * d), QPointF(box.right() - 2 * d, box.top() + 2 * d) });

    painter.setPen(Qt::NoPen);
    painter.setBrush(light);
    painter.drawPolygon(top);
    painter.setBrush(dark);
    painter.drawPolygon(bottom);
}

/**
 * One hue per kind of field.
 *
 * Fixed rather than taken from the palette: these are seven categories that
 * have to stay apart from each other and from the paper underneath them, which
 * a theme's two or three accent colours cannot promise.
 */
QColor tintOf(BuilderKind kind)
{
    switch (kind) {
    case BuilderKind::Text:
        return { 60, 120, 205 };
    case BuilderKind::Checkbox:
        return { 40, 155, 110 };
    case BuilderKind::Radio:
        return { 45, 165, 165 };
    case BuilderKind::Dropdown:
        return { 205, 130, 45 };
    case BuilderKind::ListBox:
        return { 190, 105, 60 };
    case BuilderKind::PushButton:
        return { 145, 95, 190 };
    case BuilderKind::Signature:
        return { 200, 85, 140 };
    }
    return { 120, 120, 120 };
}

/**
 * How big a field arrives when it is clicked rather than dragged out.
 *
 * A line of type at eleven points needs about eighteen to sit in, and a tick
 * box is square because every paper form's tick box is.
 */
QSizeF defaultSizeOf(BuilderKind kind)
{
    switch (kind) {
    case BuilderKind::Text:
    case BuilderKind::Dropdown:
        return { 150.0, 18.0 };
    case BuilderKind::Checkbox:
    case BuilderKind::Radio:
        return { 12.0, 12.0 };
    case BuilderKind::ListBox:
        return { 150.0, 56.0 };
    case BuilderKind::PushButton:
        return { 84.0, 22.0 };
    case BuilderKind::Signature:
        return { 180.0, 44.0 };
    }
    return { 150.0, 18.0 };
}

/**
 * What a new field of each kind is called before it is named properly.
 *
 * Deliberately not translated: this becomes the field's name in the file, which
 * is what a spreadsheet column, an import file and any script in the document
 * address it by. A name that changed with the user's language would make a form
 * built in German unfillable from a file exported in English.
 */
QString stemFor(BuilderKind kind)
{
    switch (kind) {
    case BuilderKind::Text:
        return u"Text"_s;
    case BuilderKind::Checkbox:
        return u"TickBox"_s;
    case BuilderKind::Radio:
        return u"Choice"_s;
    case BuilderKind::Dropdown:
        return u"DropDown"_s;
    case BuilderKind::ListBox:
        return u"List"_s;
    case BuilderKind::PushButton:
        return u"Button"_s;
    case BuilderKind::Signature:
        return u"Signature"_s;
    }
    return u"Field"_s;
}

BuilderKind builderKindOf(FormDesignOverlay::Kind kind)
{
    switch (kind) {
    case FormDesignOverlay::Kind::Checkbox:
        return BuilderKind::Checkbox;
    case FormDesignOverlay::Kind::Radio:
        return BuilderKind::Radio;
    case FormDesignOverlay::Kind::Dropdown:
        return BuilderKind::Dropdown;
    case FormDesignOverlay::Kind::ListBox:
        return BuilderKind::ListBox;
    case FormDesignOverlay::Kind::PushButton:
        return BuilderKind::PushButton;
    case FormDesignOverlay::Kind::Signature:
        return BuilderKind::Signature;
    case FormDesignOverlay::Kind::None:
    case FormDesignOverlay::Kind::Text:
        break;
    }
    return BuilderKind::Text;
}

/**
 * What the document says a field is, as the engine describes the same thing.
 *
 * ps::Forms reports a drop-down and an open list as one kind, because both are
 * a choice field and the flag that separates them is not among what it reads
 * back. A field of that kind therefore arrives here as a drop-down, and the
 * panel says so and offers the other.
 */
BuilderKind builderKindOf(FormField::Kind kind)
{
    switch (kind) {
    case FormField::Kind::Checkbox:
        return BuilderKind::Checkbox;
    case FormField::Kind::Radio:
        return BuilderKind::Radio;
    case FormField::Kind::Choice:
        return BuilderKind::Dropdown;
    case FormField::Kind::Button:
        return BuilderKind::PushButton;
    case FormField::Kind::Signature:
        return BuilderKind::Signature;
    case FormField::Kind::Text:
    case FormField::Kind::Unknown:
        break;
    }
    return BuilderKind::Text;
}

/** The name the engine reads back for a border style, which it matches in lower case. */
QString borderStyleName(FormField::BorderStyle style)
{
    switch (style) {
    case FormField::BorderStyle::Dashed:
        return u"dashed"_s;
    case FormField::BorderStyle::Beveled:
        return u"beveled"_s;
    case FormField::BorderStyle::Inset:
        return u"inset"_s;
    case FormField::BorderStyle::Underline:
        return u"underline"_s;
    case FormField::BorderStyle::Solid:
        break;
    }
    return u"solid"_s;
}

/** The five styles in the order the panel's list offers them. */
const QStringList &borderStyleNames()
{
    static const QStringList names { u"solid"_s, u"dashed"_s, u"beveled"_s, u"inset"_s, u"underline"_s };
    return names;
}

/** The fourteen fonts every reader already has, which are the only ones on offer. */
struct StandardFont {
    const char16_t *resource;
    const char16_t *name;
};

const StandardFont standardFonts[] = {
    { u"Helv", u"Helvetica" },
    { u"HeBo", u"Helvetica Bold" },
    { u"HeOb", u"Helvetica Oblique" },
    { u"HeBO", u"Helvetica Bold Oblique" },
    { u"TiRo", u"Times Roman" },
    { u"TiBo", u"Times Bold" },
    { u"TiIt", u"Times Italic" },
    { u"TiBI", u"Times Bold Italic" },
    { u"Cour", u"Courier" },
    { u"CoBo", u"Courier Bold" },
    { u"CoOb", u"Courier Oblique" },
    { u"CoBO", u"Courier Bold Oblique" },
    { u"Symb", u"Symbol" },
    { u"ZaDb", u"Zapf Dingbats" },
};

/** The document's field, described completely, as `updateFields()` demands. */
FormBuilder::Field toSpec(const FormField &field)
{
    FormBuilder::Field spec;
    spec.kind = builderKindOf(field.kind);
    spec.name = field.name;
    spec.label = field.label;
    spec.page = field.page;
    spec.rect = field.rect.normalized();
    spec.required = field.required;
    spec.readOnly = field.readOnly;
    spec.multiline = field.multiline;
    spec.comb = field.comb;
    spec.maxLength = field.maxLength > 0 ? field.maxLength : 0;

    if (spec.kind == BuilderKind::Checkbox || spec.kind == BuilderKind::Radio) {
        // For a tick box ps::Forms reports the names the widget's own appearance
        // answers to, not a list of choices, and the first of them is the one
        // that means "ticked".
        spec.onState = field.options.value(0, u"/Yes"_s);
    } else {
        spec.options = field.options;
    }
    if (spec.kind == BuilderKind::Radio) {
        spec.radioGroup = field.name;
    }

    // Carried through rather than left empty: the engine writes /V from this,
    // so a field changed for its border would otherwise lose its answer.
    spec.defaultValue = field.value;

    spec.fontName = field.fontResource.isEmpty() ? u"Helv"_s : field.fontResource;
    spec.fontSize = field.fontSize;
    spec.textColour = field.textColour.isValid() ? field.textColour : QColor(Qt::black);
    spec.backgroundColour = field.background;
    spec.borderColour = field.border;
    spec.borderWidth = field.borderWidth;
    spec.borderStyle = borderStyleName(field.borderStyle);
    spec.alignment = field.alignment == 1 ? Qt::AlignHCenter : field.alignment == 2 ? Qt::AlignRight : Qt::AlignLeft;
    return spec;
}

QString colourText(const QColor &colour)
{
    return colour.isValid() ? colour.name() : i18nc("@item no colour is set", "none");
}

QString xfaMessage()
{
    return i18nc("@info why no field can be added to this document",
                 "This document carries an XFA form, which is a second and incompatible description of the same "
                 "fields. Nothing can be added to it or changed in it here, because the result would behave "
                 "differently in every reader.");
}

QString unmovableMessage(int count)
{
    return i18ncp("@info a field whose box the engine will not move",
                  "One field has more than one button sharing its name, and such a field keeps the places its "
                  "author gave it. Its box cannot be moved or resized from here.",
                  "%1 fields have more than one button sharing their name, and such a field keeps the places its "
                  "author gave it. Their boxes cannot be moved or resized from here.",
                  count);
}

QString emptyNameMessage()
{
    return i18nc("@info", "A field needs a name.");
}

QString takenNameMessage(const QString &name)
{
    return i18nc("@info %1 is a field name", "This document already has a field called “%1”.", name);
}

QString groupMessage(const QString &from, const QString &to)
{
    return i18nc("@info %1 and %2 are field names",
                 "A field can be renamed but not moved into another group, so “%1” cannot become “%2”. Anything in "
                 "the document referring to it by name would be left pointing at nothing.",
                 from, to);
}

QString scriptsMessage()
{
    return i18n("The four scripts are shown as the document wrote them and are kept exactly as they are. They "
                "cannot be edited here: the engine writes only the formatting, validation and calculation actions "
                "it generates itself, and hand-written script is not something it can promise to write back.");
}

QString unreadFlagsMessage()
{
    return i18n("A password box, a list that takes several answers and a drop-down that also accepts typing are "
                "not among what is read back from a form, so these three start unticked whatever the document "
                "says. Changing anything at all about a field rewrites all of its flags, so tick them again here "
                "if the field had them.");
}

QString listKindMessage()
{
    return i18n("The form says this field offers a choice but not whether it drops down or stays open, so it is "
                "shown as a drop-down. Set it to a list if that is what it was.");
}

QString waitingMessage()
{
    return i18n("Nothing here is written until the document is saved.");
}

/** A button showing the colour it holds, with a way of holding none. */
class Swatch : public QWidget
{
public:
    Swatch(const QColor &initial, bool clearable, QWidget *parent, const std::function<void(const QColor &)> &picked)
        : QWidget(parent)
        , m_colour(initial)
    {
        auto *row = new QHBoxLayout(this);
        row->setContentsMargins(0, 0, 0, 0);
        m_button = new QPushButton(this);
        row->addWidget(m_button, 1);

        connect(m_button, &QPushButton::clicked, this, [this, picked] {
            const QColor chosen = QColorDialog::getColor(m_colour.isValid() ? m_colour : QColor(Qt::black), this,
                                                         i18nc("@title:window", "Pick a Colour"));
            if (chosen.isValid()) {
                m_colour = chosen;
                refresh();
                picked(chosen);
            }
        });

        if (clearable) {
            // A form that states no background and a form that states white are
            // different documents, so there has to be a way back to neither.
            auto *none = new QToolButton(this);
            none->setText(i18nc("@action:button take the colour off again", "None"));
            none->setToolTip(i18nc("@info:tooltip", "Leave the document saying nothing about this colour."));
            row->addWidget(none);
            connect(none, &QToolButton::clicked, this, [this, picked] {
                m_colour = QColor();
                refresh();
                picked(QColor());
            });
        }
        refresh();
    }

private:
    void refresh()
    {
        m_button->setText(colourText(m_colour));
        if (!m_colour.isValid()) {
            m_button->setStyleSheet(QString());
            return;
        }
        // Black text on a dark swatch is unreadable, so the label follows the
        // swatch rather than the theme.
        const QString ink = m_colour.lightnessF() > 0.55 ? u"#101010"_s : u"#f0f0f0"_s;
        m_button->setStyleSheet(u"background-color: %1; color: %2;"_s.arg(m_colour.name(), ink));
    }

    QColor m_colour;
    QPushButton *m_button = nullptr;
};

QDoubleSpinBox *measure(QWidget *parent, double from, double to)
{
    auto *box = new QDoubleSpinBox(parent);
    box->setRange(from, to);
    box->setDecimals(1);
    box->setSuffix(i18nc("@item point suffix in a spin box", " pt"));
    box->setKeyboardTracking(false); // Or every digit typed on the way to 120 is a change of its own.
    return box;
}

/** A text view for something reported and never rewritten. */
QPlainTextEdit *readOnlyScript(QWidget *parent, const QString &text)
{
    auto *view = new QPlainTextEdit(text, parent);
    view->setReadOnly(true);
    view->setLineWrapMode(QPlainTextEdit::NoWrap);
    view->setMaximumHeight(96);
    QFont typewriter = view->font();
    typewriter.setFamily(u"monospace"_s);
    view->setFont(typewriter);
    return view;
}

} // namespace

// ── What the properties panel shows ───────────────────────────────────────

/**
 * What is chosen, as the panel shows it.
 *
 * One field gets every attribute it carries, because authoring a form is mostly
 * setting them; several get the ones that mean the same thing said about all of
 * them at once, with the alignment buttons on top, since lining a column of
 * boxes up is what a multiple choice is nearly always for.
 */
class FormDesignOverlay::Details : public Inspectable
{
public:
    explicit Details(FormDesignOverlay *overlay)
        : m_overlay(overlay)
    {
    }

    /** Called whenever the choice, or what is waiting on it, has changed. */
    void show(int handle, int chosenCount)
    {
        m_handle = handle;
        m_chosen = chosenCount;
        m_spec = m_overlay->specOf(handle);
        pushGeometry();
    }

    QString kindName() const override;
    QString description() const override;
    QWidget *buildEditor(QWidget *parent) override;

private:
    /** Puts the numbers back after a drag has moved what they describe. */
    void pushGeometry();

    /** The kind every chosen field has, when they agree; @p same says whether. */
    BuilderKind commonKind(bool *same) const;

    void addGeometry(QWidget *box, QFormLayout *form);
    void addIdentity(QWidget *box, QFormLayout *form);
    void addRules(QWidget *box, QFormLayout *form, BuilderKind kind);
    void addStyling(QWidget *box, QFormLayout *form);
    void addScripts(QWidget *box, QFormLayout *form);

    FormDesignOverlay *m_overlay;
    FormBuilder::Field m_spec;
    int m_handle = 0;
    int m_chosen = 0;

    // The panel is built fresh on every change of choice and may be gone at any
    // moment, so each of these is held weakly and checked before it is used.
    QPointer<QDoubleSpinBox> m_x;
    QPointer<QDoubleSpinBox> m_y;
    QPointer<QDoubleSpinBox> m_width;
    QPointer<QDoubleSpinBox> m_height;
    QPointer<AlignmentPanel> m_alignment;
    QPointer<QLabel> m_summary;
};

QString FormDesignOverlay::Details::kindName() const
{
    return i18nc("@title the kind of thing shown in the properties panel", "Form field");
}

QString FormDesignOverlay::Details::description() const
{
    const QString what = m_overlay->nameOf(m_handle);
    if (m_chosen > 1) {
        return i18ncp("@info what is chosen, when it is more than one thing", "%2 and one more field",
                      "%2 and %1 more fields", m_chosen - 1, what);
    }
    return what;
}

BuilderKind FormDesignOverlay::Details::commonKind(bool *same) const
{
    const QVector<int> chosen = m_overlay->chosenHandles();
    bool agreed = !chosen.isEmpty();
    BuilderKind kind = m_spec.kind;
    for (const int handle : chosen) {
        if (m_overlay->specOf(handle).kind != kind) {
            agreed = false;
            break;
        }
    }
    if (same) {
        *same = agreed;
    }
    return kind;
}

QWidget *FormDesignOverlay::Details::buildEditor(QWidget *parent)
{
    auto *box = new QWidget(parent);
    auto *column = new QVBoxLayout(box);
    column->setContentsMargins(0, 0, 0, 0);

    // The panel built for the previous choice may still be alive for a moment
    // after this one is built, and nothing here may write into it.
    m_alignment = nullptr;
    m_summary = nullptr;
    m_x = nullptr;
    m_y = nullptr;
    m_width = nullptr;
    m_height = nullptr;

    // Above everything else: with more than one field chosen, lining them up is
    // what is wanted first, and a form is a column of boxes that have to agree.
    if (m_chosen > 1) {
        m_alignment = new AlignmentPanel(box);
        QObject::connect(m_alignment, &AlignmentPanel::changed, box,
                         [this](const QVector<QRectF> &boxes) { m_overlay->placeChosen(boxes); });
        column->addWidget(m_alignment);

        m_summary = new QLabel(box);
        m_summary->setWordWrap(true);
        column->addWidget(m_summary);
    }

    auto *form = new QFormLayout;
    form->setContentsMargins(0, 0, 0, 0);
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    column->addLayout(form);

    bool sameKind = false;
    const BuilderKind kind = commonKind(&sameKind);

    if (m_chosen <= 1) {
        addGeometry(box, form);
        addIdentity(box, form);
    }
    if (sameKind) {
        addRules(box, form, kind);
    }
    addStyling(box, form);
    if (m_chosen <= 1) {
        addScripts(box, form);
    }

    auto *note = new QLabel(waitingMessage(), box);
    note->setWordWrap(true);
    form->addRow(note);

    pushGeometry();
    return box;
}

void FormDesignOverlay::Details::addGeometry(QWidget *box, QFormLayout *form)
{
    const int handle = m_handle;
    const bool movable = m_overlay->isMovable(handle);

    m_x = measure(box, -20000, 20000);
    m_y = measure(box, -20000, 20000);
    m_width = measure(box, MinimumSizePoints, 20000);
    m_height = measure(box, MinimumSizePoints, 20000);
    for (QDoubleSpinBox *spin : { m_x.data(), m_y.data(), m_width.data(), m_height.data() }) {
        spin->setEnabled(movable);
    }
    m_x->setToolTip(i18nc("@info:tooltip", "The bottom left corner, measured from the bottom left of the page."));
    m_y->setToolTip(m_x->toolTip());

    auto *place = new QHBoxLayout;
    place->addWidget(m_x);
    place->addWidget(m_y);
    auto *size = new QHBoxLayout;
    size->addWidget(m_width);
    size->addWidget(m_height);
    form->addRow(i18nc("@label:spinbox where a field sits on the page", "Place:"), place);
    form->addRow(i18nc("@label:spinbox how wide and how tall a field is", "Size:"), size);

    // The overlay outlives the panel, so capturing it is safe; the panel is the
    // context object, so the connection dies with the widgets rather than
    // writing into a spin box that has already been destroyed.
    const auto reshape = [this, handle](int part, double value) {
        QRectF now = m_overlay->rectOf(handle).normalized();
        if (now.isNull()) {
            return;
        }
        switch (part) {
        case 0:
            now.moveLeft(value);
            break;
        case 1:
            // In points the smaller y is the bottom edge, which is what a page
            // is measured from, and moveTop() is what moves it.
            now.moveTop(value);
            break;
        case 2:
            now.setWidth(std::max(MinimumSizePoints, value));
            break;
        default:
            // Held by its bottom left, so setting a width and then a height does
            // not walk the field across the page.
            now.setHeight(std::max(MinimumSizePoints, value));
            break;
        }
        m_overlay->moveOne(handle, now);
    };
    QObject::connect(m_x, &QDoubleSpinBox::valueChanged, box, [reshape](double value) { reshape(0, value); });
    QObject::connect(m_y, &QDoubleSpinBox::valueChanged, box, [reshape](double value) { reshape(1, value); });
    QObject::connect(m_width, &QDoubleSpinBox::valueChanged, box, [reshape](double value) { reshape(2, value); });
    QObject::connect(m_height, &QDoubleSpinBox::valueChanged, box, [reshape](double value) { reshape(3, value); });

    if (!movable) {
        auto *stuck = new QLabel(unmovableMessage(1), box);
        stuck->setWordWrap(true);
        form->addRow(stuck);
    }
}

void FormDesignOverlay::Details::addIdentity(QWidget *box, QFormLayout *form)
{
    auto *name = new QLineEdit(m_spec.name, box);
    name->setToolTip(i18nc("@info:tooltip",
                           "The name filling addresses. A full stop makes a group: “Address.Street” and "
                           "“Address.Town” become two fields under one heading."));
    form->addRow(i18nc("@label:textbox the name the document gives the field", "Name:"), name);
    // On finishing rather than on every keystroke: a half-typed name would be
    // refused as a clash with itself on the way to the name that is wanted.
    QObject::connect(name, &QLineEdit::editingFinished, box, [this, name] {
        m_overlay->renameChosen(name->text());
        // A refused rename must not be left on screen looking as though it took.
        name->setText(m_overlay->specOf(m_handle).name);
    });

    auto *label = new QLineEdit(m_spec.label, box);
    label->setToolTip(i18nc("@info:tooltip", "What a reader shows beside the field, and what a push button says."));
    form->addRow(i18nc("@label:textbox what a person reads next to the field", "Label:"), label);
    QObject::connect(label, &QLineEdit::textEdited, box, [this](const QString &text) {
        m_overlay->changeChosen([text](FormBuilder::Field &spec) { spec.label = text; });
    });
}

void FormDesignOverlay::Details::addRules(QWidget *box, QFormLayout *form, BuilderKind kind)
{
    auto *kinds = new QComboBox(box);
    for (int at = int(FormDesignOverlay::Kind::Text); at <= int(FormDesignOverlay::Kind::Signature); ++at) {
        const auto one = static_cast<FormDesignOverlay::Kind>(at);
        kinds->addItem(FormDesignOverlay::describe(one), int(builderKindOf(one)));
    }
    kinds->setCurrentIndex(kinds->findData(int(kind)));
    form->addRow(i18nc("@label:listbox text, tick box, list and so on", "Kind:"), kinds);
    QObject::connect(kinds, &QComboBox::currentIndexChanged, box, [this, kinds](int at) {
        const auto wanted = static_cast<BuilderKind>(kinds->itemData(at).toInt());
        m_overlay->changeChosen([wanted](FormBuilder::Field &spec) { spec.kind = wanted; });
    });
    if (kind == BuilderKind::Dropdown || kind == BuilderKind::ListBox) {
        auto *which = new QLabel(listKindMessage(), box);
        which->setWordWrap(true);
        form->addRow(which);
    }

    const auto tick = [this, box, form](const QString &caption, bool on, const QString &tip,
                                        const std::function<void(FormBuilder::Field &, bool)> &set) {
        auto *check = new QCheckBox(caption, box);
        check->setChecked(on);
        if (!tip.isEmpty()) {
            check->setToolTip(tip);
        }
        form->addRow(check);
        QObject::connect(check, &QCheckBox::toggled, box, [this, set](bool now) {
            m_overlay->changeChosen([set, now](FormBuilder::Field &spec) { set(spec, now); });
        });
        return check;
    };

    tick(i18nc("@option:check the form insists on an answer", "Required"), m_spec.required, QString(),
         [](FormBuilder::Field &spec, bool on) { spec.required = on; });
    tick(i18nc("@option:check the field may be read but not filled in", "Read-only"), m_spec.readOnly, QString(),
         [](FormBuilder::Field &spec, bool on) { spec.readOnly = on; });

    if (kind == BuilderKind::Text) {
        tick(i18nc("@option:check the field takes more than one line", "Several lines"), m_spec.multiline, QString(),
             [](FormBuilder::Field &spec, bool on) { spec.multiline = on; });
        tick(i18nc("@option:check what is typed is hidden", "Hidden as it is typed"), m_spec.password,
             i18nc("@info:tooltip", "Not read back from the document; see the note below."),
             [](FormBuilder::Field &spec, bool on) { spec.password = on; });

        auto *limit = new QSpinBox(box);
        limit->setRange(0, 10000);
        limit->setSpecialValueText(i18nc("@item no limit on how much may be typed", "no limit"));
        limit->setValue(m_spec.maxLength);
        limit->setKeyboardTracking(false);
        form->addRow(i18nc("@label:spinbox the greatest number of characters the field will take", "Character limit:"),
                     limit);
        QObject::connect(limit, &QSpinBox::valueChanged, box, [this](int value) {
            m_overlay->changeChosen([value](FormBuilder::Field &spec) { spec.maxLength = value; });
        });

        const QString cells = i18nc("@option:check one character per cell, as on a paper form", "One box per letter");
        QCheckBox *comb
            = tick(cells, m_spec.comb,
                   i18nc("@info:tooltip", "Needs a limit on how much may be typed, and only works on a single line."),
                   [](FormBuilder::Field &spec, bool on) { spec.comb = on; });
        comb->setEnabled(m_spec.maxLength > 0 && !m_spec.multiline);
    }

    if (kind == BuilderKind::Dropdown) {
        tick(i18nc("@option:check the list also takes an answer of the reader's own", "Also accepts typing"),
             m_spec.editable, i18nc("@info:tooltip", "Not read back from the document; see the note below."),
             [](FormBuilder::Field &spec, bool on) { spec.editable = on; });
    }
    if (kind == BuilderKind::ListBox) {
        tick(i18nc("@option:check several of the choices may be picked at once", "Several may be picked"),
             m_spec.multiSelect, i18nc("@info:tooltip", "Not read back from the document; see the note below."),
             [](FormBuilder::Field &spec, bool on) { spec.multiSelect = on; });
    }

    if (kind == BuilderKind::Dropdown || kind == BuilderKind::ListBox) {
        auto *choices = new QPlainTextEdit(m_spec.options.join(u'\n'), box);
        choices->setMaximumHeight(96);
        choices->setToolTip(i18nc("@info:tooltip", "One choice on each line."));
        form->addRow(i18nc("@label:textbox what a list offers", "Choices:"), choices);
        QObject::connect(choices, &QPlainTextEdit::textChanged, box, [this, choices] {
            const QStringList lines = choices->toPlainText().split(u'\n', Qt::SkipEmptyParts);
            m_overlay->changeChosen([lines](FormBuilder::Field &spec) { spec.options = lines; });
        });
    }

    if (kind == BuilderKind::Checkbox || kind == BuilderKind::Radio) {
        auto *state = new QLineEdit(m_spec.onState, box);
        state->setToolTip(i18nc("@info:tooltip",
                                "The name this button's own drawing answers to when it is ticked. Every button of "
                                "one group needs a different one."));
        form->addRow(i18nc("@label:textbox the name a ticked box stores", "Ticked state:"), state);
        QObject::connect(state, &QLineEdit::textEdited, box, [this](const QString &text) {
            m_overlay->changeChosen([text](FormBuilder::Field &spec) { spec.onState = text; });
        });
    }

    if (kind != BuilderKind::PushButton && kind != BuilderKind::Signature) {
        auto *value = new QLineEdit(m_spec.defaultValue, box);
        value->setToolTip(i18nc("@info:tooltip", "What the field holds before anybody fills it in."));
        form->addRow(i18nc("@label:textbox the answer the field starts with", "Value:"), value);
        QObject::connect(value, &QLineEdit::textEdited, box, [this](const QString &text) {
            m_overlay->changeChosen([text](FormBuilder::Field &spec) { spec.defaultValue = text; });
        });
    }

    auto *unread = new QLabel(unreadFlagsMessage(), box);
    unread->setWordWrap(true);
    form->addRow(unread);
}

void FormDesignOverlay::Details::addStyling(QWidget *box, QFormLayout *form)
{
    auto *fonts = new QComboBox(box);
    for (const StandardFont &font : standardFonts) {
        fonts->addItem(QString::fromUtf16(font.name), QString::fromUtf16(font.resource));
    }
    int at = fonts->findData(m_spec.fontName);
    if (at < 0) {
        // A form set in a font of the author's own is believed rather than
        // quietly given Helvetica, so its name is offered back as it stands.
        fonts->addItem(m_spec.fontName, m_spec.fontName);
        at = fonts->count() - 1;
    }
    fonts->setCurrentIndex(at);

    auto *size = measure(box, 0, 400);
    size->setSpecialValueText(i18nc("@item the reader fits the text to the box", "fit"));
    size->setValue(m_spec.fontSize);
    size->setToolTip(i18nc("@info:tooltip", "Zero leaves the size to whoever draws the field."));

    auto *type = new QHBoxLayout;
    type->addWidget(fonts, 1);
    type->addWidget(size);
    form->addRow(i18nc("@label:listbox the font a field's text is set in", "Font:"), type);
    QObject::connect(fonts, &QComboBox::currentIndexChanged, box, [this, fonts](int index) {
        const QString resource = fonts->itemData(index).toString();
        m_overlay->changeChosen([resource](FormBuilder::Field &spec) { spec.fontName = resource; });
    });
    QObject::connect(size, &QDoubleSpinBox::valueChanged, box, [this](double value) {
        m_overlay->changeChosen([value](FormBuilder::Field &spec) { spec.fontSize = value; });
    });

    form->addRow(i18nc("@label:chooser the colour of the letters", "Ink:"),
                 new Swatch(m_spec.textColour, false, box, [this](const QColor &colour) {
                     m_overlay->changeChosen([colour](FormBuilder::Field &spec) { spec.textColour = colour; });
                 }));
    form->addRow(i18nc("@label:chooser the fill behind the field", "Background:"),
                 new Swatch(m_spec.backgroundColour, true, box, [this](const QColor &colour) {
                     m_overlay->changeChosen([colour](FormBuilder::Field &spec) { spec.backgroundColour = colour; });
                 }));

    auto *width = measure(box, 0, 24);
    width->setDecimals(2);
    width->setValue(m_spec.borderWidth);
    auto *style = new QComboBox(box);
    style->addItem(i18nc("@item:inlistbox border style", "Solid"), borderStyleNames().at(0));
    style->addItem(i18nc("@item:inlistbox border style", "Dashed"), borderStyleNames().at(1));
    style->addItem(i18nc("@item:inlistbox border style, a raised edge", "Raised"), borderStyleNames().at(2));
    style->addItem(i18nc("@item:inlistbox border style, a sunken edge", "Sunken"), borderStyleNames().at(3));
    style->addItem(i18nc("@item:inlistbox border style, a rule along the bottom", "Underline"),
                   borderStyleNames().at(4));
    style->setCurrentIndex(std::max(0, int(borderStyleNames().indexOf(m_spec.borderStyle.toLower()))));

    form->addRow(i18nc("@label:chooser the colour of the field's border", "Border:"),
                 new Swatch(m_spec.borderColour, true, box, [this](const QColor &colour) {
                     m_overlay->changeChosen([colour](FormBuilder::Field &spec) { spec.borderColour = colour; });
                 }));
    auto *edge = new QHBoxLayout;
    edge->addWidget(style, 1);
    edge->addWidget(width);
    form->addRow(i18nc("@label:listbox how the border is drawn and how thick", "Border style:"), edge);
    QObject::connect(width, &QDoubleSpinBox::valueChanged, box, [this](double value) {
        m_overlay->changeChosen([value](FormBuilder::Field &spec) { spec.borderWidth = value; });
    });
    QObject::connect(style, &QComboBox::currentIndexChanged, box, [this, style](int index) {
        const QString name = style->itemData(index).toString();
        m_overlay->changeChosen([name](FormBuilder::Field &spec) { spec.borderStyle = name; });
    });

    auto *across = new QComboBox(box);
    across->addItem(i18nc("@item:inlistbox where the text sits in the box", "Left"), int(Qt::AlignLeft));
    across->addItem(i18nc("@item:inlistbox where the text sits in the box", "Centred"), int(Qt::AlignHCenter));
    across->addItem(i18nc("@item:inlistbox where the text sits in the box", "Right"), int(Qt::AlignRight));
    across->setCurrentIndex(m_spec.alignment.testFlag(Qt::AlignHCenter)     ? 1
                                : m_spec.alignment.testFlag(Qt::AlignRight) ? 2
                                                                            : 0);
    form->addRow(i18nc("@label:listbox where the answer sits in the box", "Text:"), across);
    QObject::connect(across, &QComboBox::currentIndexChanged, box, [this, across](int index) {
        const auto wanted = static_cast<Qt::Alignment>(across->itemData(index).toInt());
        m_overlay->changeChosen([wanted](FormBuilder::Field &spec) { spec.alignment = wanted; });
    });
}

void FormDesignOverlay::Details::addScripts(QWidget *box, QFormLayout *form)
{
    if (isDraft(m_handle)) {
        return; // A field that is not in the document yet has nothing attached to it.
    }
    const int index = fieldIndex(m_handle);
    if (index < 0 || index >= m_overlay->m_fields.size()) {
        return;
    }
    const FormField &field = m_overlay->m_fields.at(index);
    if (field.formatScript.isEmpty() && field.keystrokeScript.isEmpty() && field.validateScript.isEmpty()
        && field.calculateScript.isEmpty()) {
        return; // Four empty boxes would say only that this form uses no scripts.
    }

    const auto addScript = [box, form](const QString &caption, const QString &text) {
        if (!text.isEmpty()) {
            form->addRow(caption, readOnlyScript(box, text));
        }
    };
    addScript(i18nc("@label:textbox the script that reformats a value", "Formatting:"), field.formatScript);
    addScript(i18nc("@label:textbox the script that runs on every keystroke", "Keystroke:"), field.keystrokeScript);
    addScript(i18nc("@label:textbox the script that checks a value", "Validation:"), field.validateScript);
    addScript(i18nc("@label:textbox the script that works a value out", "Calculation:"), field.calculateScript);

    auto *note = new QLabel(scriptsMessage(), box);
    note->setWordWrap(true);
    form->addRow(note);
}

void FormDesignOverlay::Details::pushGeometry()
{
    const QRectF bounds = m_overlay->rectOf(m_handle).normalized();

    // A box being typed into is never written back to: the numbers arrive again
    // on every drag, and setValue() under a cursor eats what the user is halfway
    // through saying.
    const auto put = [](QDoubleSpinBox *spin, double value) {
        if (!spin || spin->hasFocus()) {
            return;
        }
        QSignalBlocker blocker(spin);
        spin->setValue(value);
    };
    put(m_x, bounds.left());
    put(m_y, bounds.top()); // In points the smaller y is the bottom edge, which is what is measured from.
    put(m_width, bounds.width());
    put(m_height, bounds.height());

    if (m_alignment) {
        // Blocked while they go in: what arrives is the panel's own last answer
        // coming back, and letting it out again as a fresh question would align
        // an already aligned column a second time.
        QSignalBlocker blocker(m_alignment);
        m_alignment->setBoxes(m_overlay->alignableBoxes());
    }
    if (m_summary) {
        const QRectF round = m_overlay->choiceBounds();
        m_summary->setText(i18ncp("@info how many fields are chosen, and the box round all of them",
                                  "One field chosen · %2 × %3 pt", "%1 fields chosen · %2 × %3 pt", m_chosen,
                                  points(round.width()), points(round.height())));
    }
}

// ── Setting up ────────────────────────────────────────────────────────────

FormDesignOverlay::FormDesignOverlay(PageView *view, QObject *parent)
    : QObject(parent)
    , m_view(view)
{
    if (!m_view) {
        return;
    }
    m_view->addOverlay(this);

    // Key presses go to the view, which is what has the focus; the Overlay
    // interface has no hook for them, and an arrow key has to reach the field
    // that was just clicked rather than scroll the page out from under it.
    m_view->installEventFilter(this);

    connect(m_view, &PageView::modeChanged, this, [this](PageView::Mode mode) {
        // Handles on a page that can no longer be changed would be an offer the
        // view is not making any more, and an armed tool that cannot draw is a
        // pointer promising something it will not do.
        if (mode != PageView::Mode::Edit) {
            clearChoice();
            setKindToPlace(Kind::None);
        }
        updateOmission();
    });

    // The reader has arrived at another sheet, and the settled render is a
    // picture of the one they have left.
    connect(m_view, &PageView::currentPageChanged, this, [this](int row) { scheduleLiveRender(row); });

    // A render made at one size is a picture of a page at that size, and
    // stretching it would show a softer field than the ones beside it. Asked for
    // again rather than left to the drawing, and after the same wait, so that a
    // run of wheel notches costs one render and not ten.
    connect(m_view, &PageView::zoomChanged, this, [this] {
        scheduleLiveRender(m_liveRow >= 0 ? m_liveRow : m_view ? m_view->currentPage() : -1);
    });

    // Queued, because a window changing what it is for takes every layer off and
    // puts back the ones the new mode is about, so the list passes through
    // states nobody means, and answering each of them would throw the renders
    // away twice on the way to the arrangement that was actually asked for.
    connect(m_view, &PageView::overlaysChanged, this, &FormDesignOverlay::updateOmission, Qt::QueuedConnection);

    // Every gesture and every change made from the panel ends in this, so it is
    // the one place the clock has to be restarted from.
    connect(this, &FormDesignOverlay::editsChanged, this, [this] {
        scheduleLiveRender(m_drag != Drag::None ? m_dragRow
                               : m_page >= 0    ? m_page
                               : m_view         ? m_view->currentPage()
                                                : -1);
    });
}

FormDesignOverlay::~FormDesignOverlay()
{
    // A render in flight holds a rasteriser of its own and writes into the
    // scratch directory this is about to take away with it.
    if (m_liveWatcher) {
        m_liveWatcher->waitForFinished();
    }
    if (m_view) {
        // The pages go back to being drawn whole. Left as they are, a window
        // that took this layer off would show a form with no fields at all.
        m_view->setOmittedFromRenders(RenderBackend::Request::Omit::Nothing);
        m_view->removeEventFilter(this);
        m_view->removeOverlay(this);
    }
}

QString FormDesignOverlay::describe(Kind kind)
{
    switch (kind) {
    case Kind::None:
        return i18nc("@item no field is armed; dragging picks instead", "Pick");
    case Kind::Text:
        return i18nc("@item form field kind", "Text");
    case Kind::Checkbox:
        return i18nc("@item form field kind", "Tick box");
    case Kind::Radio:
        return i18nc("@item form field kind, one button of a group", "Choice of one");
    case Kind::Dropdown:
        return i18nc("@item form field kind", "Drop-down");
    case Kind::ListBox:
        return i18nc("@item form field kind", "List");
    case Kind::PushButton:
        return i18nc("@item form field kind", "Button");
    case Kind::Signature:
        return i18nc("@item form field kind", "Signature");
    }
    return i18nc("@item form field kind", "Text");
}

void FormDesignOverlay::setSource(const QString &pdfPath)
{
    if (m_source == pdfPath) {
        return;
    }
    m_source = pdfPath;

    // Whether a document refuses to be authored is a fact about one file, and so
    // is everything waiting: the work names fields this document has.
    m_xfa = Xfa::Unknown;
    const bool had = hasPendingWork();
    m_changed.clear();
    m_removed.clear();
    m_drafts.clear();
    m_radioGroup.clear();
    clearChoice();
    if (had) {
        Q_EMIT editsChanged();
    }
    repaint();
}

void FormDesignOverlay::setDocument(Document *document)
{
    m_document = document;
    m_rows.setDocument(document);
    if (document) {
        // Moving a page is a removal followed by an insertion, so between the
        // two signals the page is briefly nowhere at all. Answering that state
        // would throw away a field the user had only dragged somewhere else, so
        // both are queued: whichever arrives first is answered once the list has
        // settled, and the second finds nothing left to do.
        connect(document, &Document::pagesInserted, this, &FormDesignOverlay::rebaseRows, Qt::QueuedConnection);
        connect(document, &Document::pagesRemoved, this, &FormDesignOverlay::rebaseRows, Qt::QueuedConnection);

        // A row that changed in place kept its number, so nothing can be lost
        // by it and it is answered at once.
        connect(document, &Document::pagesChanged, this, &FormDesignOverlay::refreshRows);
    }
    rebuildRows();
}

void FormDesignOverlay::setFields(const QVector<FormField> &fields)
{
    const bool had = hasPendingWork();
    m_fields = fields;

    // Set here and not worked out later: the form has just been read out of the
    // document's own file, which is the one instant at which page eight of that
    // file and row eight of the view are the same sheet.
    m_rowOfPage.clear();
    m_turnAtRead.clear();
    m_rows.resync();
    for (const FormField &field : m_fields) {
        // Every widget's page and not only the field's own: a group of radio
        // buttons and a reference number repeated at the head of every sheet
        // both put boxes on pages the field itself does not name, and those
        // boxes have to be drawn on the right row like any other.
        for (const FormField::Widget &widget : field.widgets) {
            if (widget.page >= 0) {
                m_rowOfPage.insert(widget.page, widget.page);
                // And the one instant at which the turn the boxes were measured
                // against can still be asked about.
                m_turnAtRead.insert(widget.page, m_rows.rotationOf(widget.page));
            }
        }
        if (field.page >= 0) {
            m_rowOfPage.insert(field.page, field.page);
            m_turnAtRead.insert(field.page, m_rows.rotationOf(field.page));
        }
    }

    // Everything waiting was addressed to the fields that have just been
    // replaced, by index and by name, so none of it means anything now.
    m_changed.clear();
    m_removed.clear();
    m_drafts.clear();
    m_radioGroup.clear();
    clearChoice();
    rebuildRows();

    // A different form, so the render in hand is a picture of another document's
    // fields. Whether the pages have to be drawn without theirs at all is a
    // question this has just changed the answer to.
    dropLiveRender();
    updateOmission();
    if (had) {
        Q_EMIT editsChanged();
    }
}

void FormDesignOverlay::rebuildRows()
{
    m_byRow.clear();
    m_extrasByRow.clear();
    for (int at = 0; at < m_fields.size(); ++at) {
        const FormField &field = m_fields.at(at);

        // The boxes of the field that are not the box it is dragged by. Worked
        // out here rather than while painting, because a page is repainted on
        // every pixel the mouse moves and the answer only changes when the rows
        // do. The one that *is* dragged is skipped: @ref handlesOn already
        // draws it, and drawing it twice would double every border.
        bool primaryFound = false;
        for (int w = 0; w < field.widgets.size(); ++w) {
            const FormField::Widget &widget = field.widgets.at(w);
            if (widget.page < 0 || widget.rect.isEmpty()) {
                continue;
            }
            if (!primaryFound && widget.page == field.page && widget.rect == field.rect) {
                primaryFound = true;
                continue;
            }
            const int row = m_rowOfPage.value(widget.page, widget.page);
            if (row >= 0) {
                m_extrasByRow[row].append({ at, w });
            }
        }

        // A field with no widget has nowhere to be drawn and nothing to drag; it
        // is still in the form and is left exactly as the document has it.
        if (field.page < 0 || field.rect.isEmpty()) {
            continue;
        }
        const int row = rowOf(handleOfField(at));
        if (row >= 0) {
            m_byRow[row].append(at);
        }
    }
    repaint();
}

void FormDesignOverlay::rebaseRows()
{
    carryRows(m_rows.follow());
}

void FormDesignOverlay::refreshRows()
{
    carryRows(m_rows.followInPlace());
}

void FormDesignOverlay::carryRows(const Rebase &change)
{
    if (change.isIdentity()) {
        return;
    }

    // The render is a picture of one row, and the rows have just moved: which
    // sheet it stands for is a question there is no longer an answer to.
    dropLiveRender();

    // A field whose page has been taken out of the document cannot be written
    // anywhere: the engine is handed the document row by row, and there is no
    // row left that holds it. Dropping the work quietly would leave the user
    // believing a change they can no longer see was still going to happen.
    int lost = 0;

    // Before m_rowOfPage moves, because how far a waiting box has to be turned
    // is a fact about the row it was last measured at, and once the map has
    // moved there is nothing left to ask which row that was.
    QHash<int, FormBuilder::Field> changed;
    changed.reserve(m_changed.size());
    for (auto waiting = m_changed.cbegin(); waiting != m_changed.cend(); ++waiting) {
        const int was = rowOf(handleOfField(waiting.key()));
        const int row = was < 0 ? -1 : change.nowAt(was);
        if (row < 0) {
            ++lost;
            continue;
        }
        FormBuilder::Field spec = waiting.value();
        spec.rect = pageTurn(change.turnOf(was), m_rows.sizeOf(row)).mapRect(spec.rect.normalized());
        changed.insert(waiting.key(), spec);
    }
    m_changed = changed;

    for (auto page = m_rowOfPage.begin(); page != m_rowOfPage.end(); ++page) {
        page.value() = page.value() >= 0 ? change.nowAt(page.value()) : -1;
    }

    QVector<Draft> kept;
    kept.reserve(m_drafts.size());
    for (Draft &draft : m_drafts) {
        const int row = change.nowAt(draft.spec.page);
        if (row < 0) {
            ++lost;
            continue;
        }
        // Turning the sheet a box was drawn on turns the box with it: it is
        // stuck to that piece of paper, not to the corner of the screen.
        draft.spec.rect
            = pageTurn(change.turnOf(draft.spec.page), m_rows.sizeOf(row)).mapRect(draft.spec.rect.normalized());
        draft.spec.page = row;
        kept.append(draft);
    }
    m_drafts = kept;

    rebuildRows();

    if (lost > 0) {
        clearChoice();
        Q_EMIT refused(i18ncp("@info %1 is a number of form fields",
                              "One field was waiting on a page that has been taken out of the document, so it has "
                              "been dropped.",
                              "%1 fields were waiting on pages that have been taken out of the document, so they "
                              "have been dropped.",
                              lost));
        Q_EMIT editsChanged();
    }
}

int FormDesignOverlay::rowOf(int handle) const
{
    if (isDraft(handle)) {
        for (const Draft &draft : m_drafts) {
            if (draft.id == -handle) {
                return draft.spec.page;
            }
        }
        return -1;
    }
    const int index = fieldIndex(handle);
    if (index < 0 || index >= m_fields.size() || m_fields.at(index).page < 0) {
        return -1;
    }
    // Absent means the fields arrived without a document to convert through, in
    // which case the file's own numbering is all there is.
    return m_rowOfPage.value(m_fields.at(index).page, m_fields.at(index).page);
}

bool FormDesignOverlay::appliesTo(PageView::Mode mode) const
{
    return mode == PageView::Mode::Edit;
}

// ── What is on the page ───────────────────────────────────────────────────

QVector<int> FormDesignOverlay::handlesOn(int row) const
{
    QVector<int> handles;
    const auto here = m_byRow.constFind(row);
    if (here != m_byRow.constEnd()) {
        for (const int index : here.value()) {
            handles.append(handleOfField(index));
        }
    }
    // Drafts last, so that a field just drawn over an existing one is the one a
    // click on the overlap picks: it is the one the user is looking at.
    for (const Draft &draft : m_drafts) {
        if (draft.spec.page == row) {
            handles.append(-draft.id);
        }
    }
    return handles;
}

QVector<FormDesignOverlay::Extra> FormDesignOverlay::extrasOn(int row) const
{
    return m_extrasByRow.value(row);
}

QTransform FormDesignOverlay::turnOf(int index) const
{
    if (index < 0 || index >= m_fields.size()) {
        return {};
    }
    return turnAt(rowOf(handleOfField(index)), m_fields.at(index).page);
}

QTransform FormDesignOverlay::turnAt(int row, int page) const
{
    const int since = normalizeRotation(m_rows.rotationOf(row) - m_turnAtRead.value(page));

    // Answered before the page is measured, because measuring one is a question
    // for the rasteriser and on a document nobody has turned this is every call.
    return since == 0 ? QTransform() : pageTurn(since, m_rows.sizeOf(row));
}

FormBuilder::Field FormDesignOverlay::documentSpec(int index) const
{
    FormBuilder::Field spec = toSpec(m_fields.at(index));
    spec.rect = turnOf(index).mapRect(spec.rect);
    return spec;
}

FormBuilder::Field FormDesignOverlay::specOf(int handle) const
{
    if (isDraft(handle)) {
        for (const Draft &draft : m_drafts) {
            if (draft.id == -handle) {
                return draft.spec;
            }
        }
        return {};
    }
    const int index = fieldIndex(handle);
    if (index < 0 || index >= m_fields.size()) {
        return {};
    }
    const auto waiting = m_changed.constFind(index);
    return waiting == m_changed.constEnd() ? documentSpec(index) : waiting.value();
}

FormBuilder::Field *FormDesignOverlay::editableSpec(int handle)
{
    if (isDraft(handle)) {
        for (Draft &draft : m_drafts) {
            if (draft.id == -handle) {
                return &draft.spec;
            }
        }
        return nullptr;
    }
    const int index = fieldIndex(handle);
    if (index < 0 || index >= m_fields.size() || m_removed.contains(index)) {
        return nullptr;
    }
    const auto waiting = m_changed.find(index);
    if (waiting != m_changed.end()) {
        return &waiting.value();
    }
    // The whole field is copied on the first touch, because the engine takes a
    // complete description and would apply every default of one that was not.
    return &m_changed.insert(index, documentSpec(index)).value();
}

QRectF FormDesignOverlay::rectOf(int handle) const
{
    return specOf(handle).rect.normalized();
}

QString FormDesignOverlay::nameOf(int handle) const
{
    const FormBuilder::Field spec = specOf(handle);
    return spec.name.isEmpty() ? i18nc("@info a field with no name yet", "unnamed field") : spec.name;
}

bool FormDesignOverlay::isRemoved(int handle) const
{
    return !isDraft(handle) && m_removed.contains(fieldIndex(handle));
}

bool FormDesignOverlay::isMovable(int handle) const
{
    if (isDraft(handle)) {
        return true; // Nothing has been written for it yet, so its box is wholly ours.
    }
    const int index = fieldIndex(handle);
    if (index < 0 || index >= m_fields.size()) {
        return false;
    }
    const FormField &field = m_fields.at(index);
    // The engine moves a field's box only when the field has exactly one button
    // to move; a group of them keeps the places its author chose. What is read
    // back does not count them, and the one kind that always has several is the
    // group of buttons that share a name.
    return field.page >= 0 && !field.rect.isEmpty() && field.kind != FormField::Kind::Radio;
}

QVector<int> FormDesignOverlay::chosenHandles() const
{
    QVector<int> handles(m_chosen.constBegin(), m_chosen.constEnd());
    std::sort(handles.begin(), handles.end());
    return handles;
}

QVector<int> FormDesignOverlay::movableChosen() const
{
    QVector<int> movable;
    for (const int handle : chosenHandles()) {
        if (isMovable(handle) && !isRemoved(handle)) {
            movable.append(handle);
        }
    }
    return movable;
}

QVector<QRectF> FormDesignOverlay::alignableBoxes() const
{
    QVector<QRectF> boxes;
    // As the page now stands, so that the buttons line up what the user can see
    // rather than what the file will go on saying until the work is written.
    for (const int handle : movableChosen()) {
        boxes.append(rectOf(handle));
    }
    return boxes;
}

QRectF FormDesignOverlay::choiceBounds() const
{
    QRectF box;
    for (const int handle : chosenHandles()) {
        const QRectF bounds = rectOf(handle);
        if (bounds.isEmpty()) {
            continue;
        }
        box = box.isNull() ? bounds : box.united(bounds);
    }
    return box;
}

// ── Drawing ───────────────────────────────────────────────────────────────

QPointF FormDesignOverlay::gripCentre(const QRectF &box, Grip grip)
{
    // Pixels, so top is the top of the screen; the page's own y runs the other
    // way and is nobody's business here.
    const double midX = box.center().x();
    const double midY = box.center().y();
    switch (grip) {
    case Grip::TopLeft:
        return box.topLeft();
    case Grip::Top:
        return { midX, box.top() };
    case Grip::TopRight:
        return box.topRight();
    case Grip::Right:
        return { box.right(), midY };
    case Grip::BottomRight:
        return box.bottomRight();
    case Grip::Bottom:
        return { midX, box.bottom() };
    case Grip::BottomLeft:
        return box.bottomLeft();
    case Grip::Left:
        return { box.left(), midY };
    case Grip::None:
        break;
    }
    return {};
}

Qt::CursorShape FormDesignOverlay::cursorForGrip(Grip grip)
{
    switch (grip) {
    case Grip::TopLeft:
    case Grip::BottomRight:
        return Qt::SizeFDiagCursor;
    case Grip::TopRight:
    case Grip::BottomLeft:
        return Qt::SizeBDiagCursor;
    case Grip::Left:
    case Grip::Right:
        return Qt::SizeHorCursor;
    case Grip::Top:
    case Grip::Bottom:
        return Qt::SizeVerCursor;
    case Grip::None:
        break;
    }
    return Qt::ArrowCursor;
}

void FormDesignOverlay::paint(QPainter &painter, int row, const QRect &pageRect)
{
    Q_UNUSED(pageRect)
    if (!m_view) {
        return;
    }

    // The painter arrives in the viewport's own coordinates, which is what
    // fromPoints() answers in, so nothing here has to find the page's corner.
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QColor highlight = m_view->palette().color(QPalette::Highlight);

    QFont caption = m_view->font();
    caption.setPointSizeF(std::max(6.0, caption.pointSizeF() - 1.0));
    const QFontMetricsF metrics(caption);

    // Worked out once for the whole page: every field asks the same question of
    // it, and paint() runs again on every pixel the mouse moves.
    const QTransform live = m_drag == Drag::None ? QTransform() : liveTransform();

    // ── the fields themselves ─────────────────────────────────────────────
    //
    // Drawn here because the page underneath was asked for without them, so
    // there is exactly one of each and it is wherever the gesture has put it.
    // The extra boxes of a field that has several go first and plainly: they
    // cannot be dragged, so they carry no scaffolding, but they are on the page
    // and would simply vanish if nobody drew them.
    for (const Extra &extra : extrasOn(row)) {
        const FormField &field = m_fields.at(extra.field);
        const FormField::Widget &widget = field.widgets.at(extra.widget);
        const FormBuilder::Field spec = specOf(handleOfField(extra.field));
        const QRectF box = m_view->fromPoints(row, turnAt(row, widget.page).mapRect(widget.rect.normalized()));
        if (box.isEmpty() || isRemoved(handleOfField(extra.field))) {
            continue;
        }
        const bool worked = row == m_page && m_chosen.contains(handleOfField(extra.field));
        if (!(m_liveCurrent || !worked) || !paintLive(painter, row, box)) {
            paintFace(painter, spec, box, isMarked(spec, widget.onState));
        }
        QColor tint = tintOf(spec.kind);
        tint.setAlpha(80);
        painter.setPen(QPen(tint, 1.0));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(box);
    }

    const QVector<int> handles = handlesOn(row);
    for (const int handle : handles) {
        const FormBuilder::Field spec = specOf(handle);
        const bool chosen = row == m_page && m_chosen.contains(handle);
        const bool moving = chosen && m_drag != Drag::None && isMovable(handle);
        const QRectF bounds
            = chosen && isMovable(handle) ? live.mapRect(spec.rect.normalized()) : spec.rect.normalized();
        const QRectF box = m_view->fromPoints(row, bounds);
        if (box.isEmpty()) {
            continue;
        }
        const bool doomed = isRemoved(handle);
        const bool waiting = isDraft(handle);
        const bool hovered = row == m_hoverPage && handle == m_hover;

        // A field waiting to be taken off gets no face: after saving it is not
        // there, and the cross below says so. Otherwise the settled render of
        // the page as it will be written, where there is one that still stands
        // for this box, and the drawing of it where there is not, or while the
        // hand is still moving it and no render could keep up.
        const bool settled = m_liveCurrent || !chosen;
        if (!doomed && (moving || !settled || !paintLive(painter, row, box))) {
            paintFace(painter, spec, box, isMarked(spec, spec.onState));
        }

        QColor tint = doomed ? QColor(200, 60, 60) : tintOf(spec.kind);
        const Qt::PenStyle line = doomed || waiting ? Qt::DashLine : Qt::SolidLine;

        painter.setPen(Qt::NoPen);
        if (chosen) {
            QColor wash = highlight;
            wash.setAlpha(45);
            painter.setBrush(wash);
            painter.drawRect(box);
            painter.setPen(QPen(highlight, 2.0, line));
        } else if (hovered) {
            QColor wash = tint;
            wash.setAlpha(28);
            painter.setBrush(wash);
            painter.drawRect(box);
            painter.setPen(QPen(tint, 1.5, line));
        } else {
            // Faint until it is pointed at: a solid boundary round every field
            // would bury the form under its own scaffolding, and the page below
            // already draws whatever borders the fields themselves carry.
            tint.setAlpha(doomed || waiting ? 170 : 80);
            painter.setPen(QPen(tint, waiting ? 1.5 : 1.0, line));
        }

        painter.setBrush(Qt::NoBrush);
        painter.drawRect(box);
        if (doomed) {
            painter.drawLine(box.topLeft(), box.bottomRight());
            painter.drawLine(box.bottomLeft(), box.topRight());
            continue;
        }

        // The name on a tag above the box rather than inside it. Three fields
        // called "Date" are what a form author spends their afternoon telling
        // apart, so the name has to be there; but the box already carries the
        // field's own appearance, and a name written into the same rectangle
        // landed on top of the answer somebody had typed.
        // And only for the field under the pointer or in hand. A name on every
        // field at once covers the printed captions the form was laid out
        // around, and a designer wants to know what one field is called at a
        // time, which is the one they are pointing at.
        if ((chosen || hovered) && box.width() >= 3.0 * CaptionFloorPixels) {
            painter.setFont(caption);
            const QString said = spec.name.isEmpty() ? nameOf(handle) : spec.name;
            const QString shown = metrics.elidedText(said, Qt::ElideMiddle, box.width());
            const double tall = metrics.height();

            // Above by preference, below when the field sits at the top of the
            // sheet. Either way it is outside the box and cannot collide with
            // what the field says.
            QRectF tag(box.left(), box.top() - tall - 1.0, metrics.horizontalAdvance(shown) + 6.0, tall);
            if (tag.top() < 0.0) {
                tag.moveTop(box.bottom() + 1.0);
            }

            QColor ink = chosen ? highlight : tintOf(spec.kind);
            ink.setAlpha(chosen || hovered ? 235 : 170);
            QColor ground = ink;
            ground.setAlpha(chosen || hovered ? 46 : 26);
            painter.setPen(Qt::NoPen);
            painter.setBrush(ground);
            painter.drawRect(tag);
            painter.setBrush(Qt::NoBrush);
            painter.setPen(ink);
            painter.drawText(tag.adjusted(3.0, 0.0, -3.0, 0.0), Qt::AlignLeft | Qt::AlignVCenter, shown);
        }
    }

    // ── the handles ───────────────────────────────────────────────────────
    if (row == m_page && !m_chosen.isEmpty()) {
        QRectF bounds = choiceBounds();
        if (m_drag != Drag::None) {
            bounds = liveTransform().mapRect(bounds);
        }
        if (!bounds.isNull()) {
            const QRectF box = m_view->fromPoints(row, bounds);
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(highlight, 1.0, Qt::DashLine));
            painter.drawRect(box);

            painter.setPen(QPen(highlight.darker(150), 1.0));
            painter.setBrush(m_view->palette().color(QPalette::Base));
            // Eight, and no ninth to turn it by: a widget's rectangle is stated
            // as two corners and cannot be written at an angle, so a turn handle
            // would be an offer the file format does not accept.
            for (int grip = int(Grip::TopLeft); grip <= int(Grip::Left); ++grip) {
                const QPointF centre = gripCentre(box, static_cast<Grip>(grip));
                painter.drawRect(
                    QRectF(centre.x() - HandleSize / 2.0, centre.y() - HandleSize / 2.0, HandleSize, HandleSize));
            }
        }
    }

    // ── the band, and the field being drawn out ───────────────────────────
    if ((m_drag == Drag::Band || m_drag == Drag::Create) && row == m_dragRow) {
        const QRectF box = m_view->fromPoints(row, QRectF(m_from, m_to).normalized());
        QColor wash = m_drag == Drag::Create ? tintOf(builderKindOf(m_kind)) : highlight;
        painter.setPen(QPen(wash, m_drag == Drag::Create ? 1.5 : 1.0, Qt::DashLine));
        wash.setAlpha(35);
        painter.setBrush(wash);
        painter.drawRect(box);
    }
}

bool FormDesignOverlay::isMarked(const FormBuilder::Field &spec, const QString &onState)
{
    if (spec.kind != BuilderKind::Checkbox && spec.kind != BuilderKind::Radio) {
        return false;
    }
    // The value and the state name are the same thing written two ways: one
    // carries the leading slash of a PDF name and the other often does not, and
    // a comparison that insists on it shows every ticked box empty.
    const auto bare = [](const QString &name) { return name.startsWith(QLatin1Char('/')) ? name.mid(1) : name; };
    const QString on = bare(onState.isEmpty() ? spec.onState : onState);
    return !on.isEmpty() && bare(spec.defaultValue).compare(on, Qt::CaseInsensitive) == 0;
}

void FormDesignOverlay::paintFace(QPainter &painter, const FormBuilder::Field &spec, const QRectF &box,
                                  bool marked) const
{
    const double zoom = m_view ? m_view->zoom() : 1.0;
    if (zoom <= 0.0 || box.isEmpty()) {
        return;
    }

    // The box as the engine will measure it: points, the way /Rect states it.
    const double w = box.width() / zoom;
    const double h = box.height() / zoom;
    const bool circular = spec.kind == BuilderKind::Radio;
    const double borderWidth = std::max(0.0, spec.borderWidth);
    const double pen = borderWidth * zoom;

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setClipRect(box.adjusted(-1.0, -1.0, 1.0, 1.0), Qt::IntersectClip);

    if (spec.backgroundColour.isValid()) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(spec.backgroundColour);
        circular ? painter.drawEllipse(box) : painter.drawRect(box);
    }

    const QString style = spec.borderStyle.trimmed().toLower();
    if (spec.borderColour.isValid() && pen > 0.0) {
        QPen stroke(spec.borderColour, pen);
        if (style == QLatin1String("dashed")) {
            // The engine writes [3w 2w] as the dash, in points; a pen counts its
            // pattern in multiples of its own width, which is those same numbers
            // divided by it.
            stroke.setDashPattern({ 3.0, 2.0 });
        }
        painter.setPen(stroke);
        painter.setBrush(Qt::NoBrush);
        if (style == QLatin1String("underline")) {
            // A line under the writing, not a box round it.
            const double y = box.bottom() - pen / 2.0;
            painter.drawLine(QPointF(box.left(), y), QPointF(box.right(), y));
        } else if (circular) {
            painter.drawEllipse(box.adjusted(pen / 2.0, pen / 2.0, -pen / 2.0, -pen / 2.0));
        } else {
            painter.drawRect(box.adjusted(pen / 2.0, pen / 2.0, -pen / 2.0, -pen / 2.0));
        }
        if (!circular && (style == QLatin1String("beveled") || style == QLatin1String("inset"))) {
            const bool raised = style == QLatin1String("beveled");
            paintBevel(painter, box, pen, raised ? QColor(Qt::white) : QColor(128, 128, 128),
                       raised ? QColor(128, 128, 128) : QColor(Qt::white));
        }
    }

    const QColor ink = spec.textColour.isValid() ? spec.textColour : QColor(Qt::black);
    const QString baseFont = baseFontOf(spec.fontName);

    if (spec.kind == BuilderKind::Checkbox || spec.kind == BuilderKind::Radio) {
        if (marked) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(ink);
            if (circular) {
                // The engine fills a disc of this radius; the number is its.
                const double radius = std::min(box.width(), box.height()) * 0.22;
                painter.drawEllipse(box.center(), radius, radius);
            } else {
                // The engine sets character 4 of ZapfDingbats, which is a tick.
                // Drawn as a stroke rather than as that glyph, because a machine
                // without the face would otherwise show a box or a 4 where the
                // file will show a tick. This is the one mark on a field whose
                // shape is this program's rather than the document's, and the
                // settled render puts the document's own back.
                const QRectF inner
                    = box.adjusted(box.width() * 0.2, box.height() * 0.2, -box.width() * 0.2, -box.height() * 0.2);
                painter.setBrush(Qt::NoBrush);
                painter.setPen(
                    QPen(ink, std::max(1.0, inner.height() * 0.22), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                painter.drawPolyline(QPolygonF({ QPointF(inner.left(), inner.center().y()),
                                                 QPointF(inner.left() + inner.width() * 0.35, inner.bottom()),
                                                 QPointF(inner.right(), inner.top()) }));
            }
        }
        painter.restore();
        return;
    }

    // A push button says its caption; everything else says its value. Both are
    // measured against the standard widths rather than against this desktop's
    // metrics, so the text sits where the file will put it even where the face
    // that draws it is a substitute.
    const bool button = spec.kind == BuilderKind::PushButton;
    const QString text = button ? (spec.label.isEmpty() ? spec.name : spec.label) : spec.defaultValue;
    if (text.isEmpty()) {
        painter.restore();
        return;
    }

    const double size = effectiveSizeOf(spec, w, h, text, baseFont);
    painter.setPen(ink);
    painter.setBrush(Qt::NoBrush);
    painter.setFont(faceFor(baseFont, size * zoom));

    if (button) {
        const double drawn = textWidthOf(text, baseFont, size);
        painter.drawText(QPointF(box.left() + (w - drawn) / 2.0 * zoom, box.bottom() - (h - size * 0.72) / 2.0 * zoom),
                         text);
        painter.restore();
        return;
    }

    const double pad = paddingOf(spec);
    const QStringList lines = spec.multiline ? text.split(QLatin1Char('\n')) : QStringList { text };
    double y = spec.multiline ? h - pad - size : (h - size) / 2.0 + size * 0.22;
    for (const QString &line : lines) {
        if (y < -size) {
            break;
        }
        double x = pad;
        if (spec.comb && spec.maxLength > 0) {
            // One character per cell, which is what makes a row of little boxes
            // line up with the letters somebody types into them.
            const double cell = w / double(spec.maxLength);
            for (int i = 0; i < line.size() && i < spec.maxLength; ++i) {
                const QString one = line.mid(i, 1);
                const double centre = cell * (double(i) + 0.5) - textWidthOf(one, baseFont, size) / 2.0;
                painter.drawText(QPointF(box.left() + centre * zoom, box.bottom() - y * zoom), one);
            }
            y -= size * 1.16;
            continue;
        }
        const double drawn = textWidthOf(line, baseFont, size);
        if (spec.alignment.testFlag(Qt::AlignHCenter)) {
            x = (w - drawn) / 2.0;
        } else if (spec.alignment.testFlag(Qt::AlignRight)) {
            x = w - pad - drawn;
        }
        painter.drawText(QPointF(box.left() + x * zoom, box.bottom() - y * zoom), line);
        y -= size * 1.16;
    }
    painter.restore();
}

// ── What is shown is the file ─────────────────────────────────────────────

void FormDesignOverlay::updateOmission()
{
    if (!m_view) {
        return;
    }
    // Only where there is something to leave off, and only while this is the
    // layer drawing it. A document with no form has no widgets to hide, and a
    // mode that edits the text of a form takes this layer away without leaving
    // the mode: a page rendered without its fields then would simply have none.
    const bool wanted = m_view->mode() == PageView::Mode::Edit && !m_fields.isEmpty() && m_view->hasOverlay(this);
    m_view->setOmittedFromRenders(wanted ? RenderBackend::Request::Omit::FormFields
                                         : RenderBackend::Request::Omit::Nothing);
    if (wanted) {
        scheduleLiveRender(m_view->currentPage());
    } else {
        m_settle.stop();
        m_settleRow = -1;
        dropLiveRender();
    }
}

void FormDesignOverlay::scheduleLiveRender(int row)
{
    // Nothing to settle for a layer that has been taken off the view: it draws
    // nothing, so a render made for it would be a render nobody looks at.
    if (!m_view || row < 0 || m_view->mode() != PageView::Mode::Edit || !m_view->hasOverlay(this)) {
        return;
    }
    m_settleRow = row;

    // The render in hand is left up rather than thrown away, because it is a
    // true picture of every field except the ones being worked on, and those
    // are drawn from their own description while the work is going on. Taking it
    // down would make the whole page flicker each time one box was nudged.
    m_liveCurrent = false;
    if (row != m_liveRow) {
        dropLiveRender();
    }
    m_settle.start(SettleMs, this);
}

void FormDesignOverlay::dropLiveRender()
{
    if (m_live.isNull() && m_liveRow < 0) {
        return;
    }
    m_live = QImage();
    m_liveRow = -1;
    m_liveZoom = 0.0;
    m_liveCurrent = false;
}

void FormDesignOverlay::timerEvent(QTimerEvent *event)
{
    if (event->timerId() == m_settle.timerId()) {
        m_settle.stop();
        startLiveRender();
        return;
    }
    QObject::timerEvent(event);
}

void FormDesignOverlay::startLiveRender()
{
    if (!m_view || !m_document || !m_scratch.isValid() || m_settleRow < 0) {
        return;
    }
    const int row = m_settleRow;
    const QRect where = m_view->pageRect(row);
    if (where.isEmpty()) {
        return;
    }

    // The page on its own, which is a few kilobytes against a document's
    // several megabytes. Its fields come with it (QPDF's form helper carries
    // them across), and the turn the organiser gave the row is written into its
    // /Rotate, which is exactly the space the boxes waiting on it are measured
    // in.
    const quint64 generation = ++m_liveWanted;
    for (const QString &stale :
         QDir(m_scratch.path()).entryList({ u"page-%1-*.pdf"_s.arg(generation - 1) }, QDir::Files)) {
        QFile::remove(m_scratch.filePath(stale));
    }
    const QString extracted = m_scratch.filePath(u"page-%1-0.pdf"_s.arg(generation));
    if (!DocumentWriter::writeSelection(*m_document, { row }, extracted, {}, nullptr)) {
        return;
    }

    // Named by their page, because the page it is a copy of holds exactly one
    // sheet and the engine numbers from there.
    const Work work = collect(row);
    QStringList steps;
    for (int step = 1; step <= 4; ++step) {
        steps << m_scratch.filePath(u"page-%1-%2.pdf"_s.arg(generation).arg(step));
    }
    const int widthPx = std::max(1, int(std::lround(where.width() * m_view->devicePixelRatioF())));

    if (!m_liveWatcher) {
        m_liveWatcher = new QFutureWatcher<QImage>(this);
        connect(m_liveWatcher, &QFutureWatcher<QImage>::finished, this, &FormDesignOverlay::liveRenderArrived);
    }

    m_liveRow = row;
    m_liveZoom = m_view->zoom();
    m_livePage = where.size();

    // Setting a new future lets go of the one before it: a render started for a
    // box that has since been dragged somewhere else finishes into nothing,
    // which costs less than making the thread that has to keep drawing wait.
    m_liveWatcher->setFuture(QtConcurrent::run([extracted, steps, work, widthPx] {
        // The four calls in the order the window applies them, each reading one
        // file and writing another, so that the page rendered below is the page
        // the document would have if it were saved this instant.
        QString from = extracted;
        const auto step = [&from, &steps](int index, auto &&apply) {
            const QString to = steps.at(index);
            if (!apply(from, to)) {
                return false;
            }
            from = to;
            return true;
        };

        // Each call is asked how many fields it actually touched, and anything
        // short of what it was given gives up rather than rendering. A page that
        // came out of the document without its form would take every change
        // silently and produce a picture of the fields where they *were*, which
        // is the very thing this exists to stop, and would be far worse than the
        // drawing it is meant to replace.
        if (!work.removed.isEmpty() && !step(0, [&work](const QString &in, const QString &out) {
                int removed = 0;
                return FormBuilder::removeFields(in, out, work.removed, &removed, nullptr)
                    && removed == work.removed.size();
            })) {
            return QImage();
        }
        for (const Rename &rename : work.renamed) {
            if (!step(1, [&rename](const QString &in, const QString &out) {
                    return FormBuilder::renameField(in, out, rename.from, rename.to, nullptr);
                })) {
                return QImage();
            }
        }
        if (!work.changed.isEmpty() && !step(2, [&work](const QString &in, const QString &out) {
                int updated = 0;
                return FormBuilder::updateFields(in, out, work.changed, &updated, nullptr)
                    && updated == work.changed.size();
            })) {
            return QImage();
        }
        if (!work.added.isEmpty() && !step(3, [&work](const QString &in, const QString &out) {
                int added = 0;
                return FormBuilder::addFields(in, out, work.added, &added, nullptr) && added == work.added.size();
            })) {
            return QImage();
        }

        // The rasteriser is opened and let go inside the worker, so nothing here
        // shares a handle with the renders the view has in flight, and this one
        // is asked for the page *with* its fields, which is the whole point of
        // making it.
        PopplerBackend backend;
        if (!backend.addDocument(1, from, nullptr)) {
            return QImage();
        }
        return backend.renderPage(1, 0, widthPx);
    }));
}

void FormDesignOverlay::liveRenderArrived()
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
    // Unless something has been changed since it was asked for, in which case
    // it is already a picture of the page as it was and another one is coming.
    m_liveCurrent = !m_settle.isActive();
    repaint();
}

bool FormDesignOverlay::paintLive(QPainter &painter, int row, const QRectF &box)
{
    if (m_live.isNull() || row != m_liveRow || !m_view) {
        return false;
    }
    const QRect where = m_view->pageRect(row);
    // A zoom or a re-flow since the render was asked for makes it a picture of a
    // page at another size, and stretching it would show a softer field than the
    // ones beside it, which reads as a mistake rather than as a wait.
    if (where.size() != m_livePage || !qFuzzyCompare(m_view->zoom(), m_liveZoom)) {
        return false;
    }
    const double scale = double(m_live.width()) / double(where.width());
    if (std::abs(m_live.height() - where.height() * scale) > 2.0) {
        return false;
    }

    // The field's own patch of that render and nothing else: everything outside
    // it is what the page already draws, and blitting the lot would put a second
    // copy of the page over every other overlay.
    const QRectF onPage = box.translated(-where.topLeft());
    const QRectF source(onPage.left() * scale, onPage.top() * scale, onPage.width() * scale, onPage.height() * scale);
    if (!m_live.rect().contains(source.toAlignedRect())) {
        return false;
    }
    painter.drawImage(box, m_live, source);
    return true;
}

// ── The mouse ─────────────────────────────────────────────────────────────

FormDesignOverlay::Grip FormDesignOverlay::gripAt(int row, const QPointF &pagePoint) const
{
    if (!m_view || row != m_page || m_chosen.isEmpty()) {
        return Grip::None;
    }
    const QRectF bounds = choiceBounds();
    if (bounds.isNull()) {
        return Grip::None;
    }

    // Handles are the same size at every zoom, so which one is being reached for
    // is a question about pixels rather than about points.
    const QRectF box = m_view->fromPoints(row, bounds);
    const QPointF where = m_view->fromPoints(row, pagePoint);
    for (int grip = int(Grip::TopLeft); grip <= int(Grip::Left); ++grip) {
        if (QLineF(where, gripCentre(box, static_cast<Grip>(grip))).length() <= HandleReach) {
            return static_cast<Grip>(grip);
        }
    }
    return Grip::None;
}

QTransform FormDesignOverlay::liveTransform() const
{
    const QPointF delta = m_to - m_from;
    if (m_drag == Drag::Move) {
        return QTransform::fromTranslate(delta.x(), delta.y());
    }
    if (m_drag != Drag::Resize || m_startBox.width() <= 0.0 || m_startBox.height() <= 0.0) {
        return {};
    }

    const bool left = m_grip == Grip::TopLeft || m_grip == Grip::Left || m_grip == Grip::BottomLeft;
    const bool right = m_grip == Grip::TopRight || m_grip == Grip::Right || m_grip == Grip::BottomRight;
    const bool up = m_grip == Grip::TopLeft || m_grip == Grip::Top || m_grip == Grip::TopRight;
    const bool down = m_grip == Grip::BottomLeft || m_grip == Grip::Bottom || m_grip == Grip::BottomRight;

    // The far side of the box stays where it is, which is what makes a handle
    // feel attached to the field rather than to the pointer. Points run up the
    // page, so the handles along the top edge move the rectangle's larger y.
    QPointF anchor = m_startBox.center();
    double across = 1.0;
    double downwards = 1.0;
    if (left) {
        anchor.setX(m_startBox.right());
        across = (m_startBox.right() - (m_startBox.left() + delta.x())) / m_startBox.width();
    } else if (right) {
        anchor.setX(m_startBox.left());
        across = (m_startBox.right() + delta.x() - m_startBox.left()) / m_startBox.width();
    }
    if (up) {
        anchor.setY(m_startBox.top());
        downwards = (m_startBox.bottom() + delta.y() - m_startBox.top()) / m_startBox.height();
    } else if (down) {
        anchor.setY(m_startBox.bottom());
        downwards = (m_startBox.bottom() - (m_startBox.top() + delta.y())) / m_startBox.height();
    }

    // Dragging a handle past the far edge would turn the field inside out, which
    // is never what the hand that did it meant.
    across = std::max(across, MinimumSizePoints / m_startBox.width());
    downwards = std::max(downwards, MinimumSizePoints / m_startBox.height());

    if ((left || right) && (up || down) && QGuiApplication::keyboardModifiers().testFlag(Qt::ShiftModifier)) {
        // The pointer rarely lands exactly on the diagonal; following the mean
        // of the two keeps the proportions without the box snapping to one axis.
        const double both = (across + downwards) / 2.0;
        across = both;
        downwards = both;
    }

    return QTransform::fromTranslate(-anchor.x(), -anchor.y()) * QTransform::fromScale(across, downwards)
        * QTransform::fromTranslate(anchor.x(), anchor.y());
}

int FormDesignOverlay::fieldAt(int row, const QPointF &pagePoint) const
{
    const QVector<int> handles = handlesOn(row);
    // Back to front, so a field lying over another is the one a click on the
    // overlap picks: the same order they were drawn in.
    for (auto at = handles.crbegin(); at != handles.crend(); ++at) {
        if (isRemoved(*at)) {
            continue; // Nothing left to do with it; the click goes to whatever is underneath.
        }
        if (rectOf(*at).contains(pagePoint)) {
            return *at;
        }
    }
    return 0;
}

bool FormDesignOverlay::press(int row, const QPointF &pagePoint, Qt::MouseButtons buttons)
{
    if (!m_view || row < 0 || !buttons.testFlag(Qt::LeftButton)) {
        return false;
    }

    // The interface hands over the buttons but not the modifiers, and Ctrl is
    // what makes a click add to the choice rather than replace it.
    const bool add = QGuiApplication::keyboardModifiers().testFlag(Qt::ControlModifier);

    m_from = pagePoint;
    m_to = pagePoint;
    m_dragRow = row;
    m_drag = Drag::None;

    const Grip grip = gripAt(row, pagePoint);
    if (grip != Grip::None) {
        const int stuck = int(m_chosen.size() - movableChosen().size());
        if (movableChosen().isEmpty()) {
            Q_EMIT refused(unmovableMessage(int(m_chosen.size())));
            return true;
        }
        if (stuck > 0) {
            Q_EMIT refused(unmovableMessage(stuck));
        }
        m_grip = grip;
        m_startBox = choiceBounds();
        m_drag = Drag::Resize;
        return true;
    }

    const int hit = fieldAt(row, pagePoint);
    if (hit != 0) {
        if (add) {
            choose(row, { hit }, true);
        } else if (row != m_page || !m_chosen.contains(hit)) {
            choose(row, { hit }, false);
        } else if (m_current != hit) {
            // Pressing something already chosen keeps the whole choice, so that
            // a column of five can be dragged without picking them again.
            m_current = hit;
            Q_EMIT choiceChanged();
        }

        if (movableChosen().isEmpty()) {
            Q_EMIT refused(unmovableMessage(int(m_chosen.size())));
        } else {
            m_drag = Drag::Move;
            m_startBox = choiceBounds();
        }
        repaint();
        return true;
    }

    // Bare paper. With a kind armed the drag draws that field; without one it
    // bands a choice, but only on a page that has fields on it, because a document
    // with no form must not take the band away from the layer underneath.
    if (m_kind != Kind::None) {
        if (refusesAuthoring()) {
            setKindToPlace(Kind::None);
            return true;
        }
        m_drag = Drag::Create;
        if (!m_chosen.isEmpty()) {
            clearChoice();
        }
        repaint();
        return true;
    }
    if (handlesOn(row).isEmpty()) {
        return false;
    }

    m_drag = Drag::Band;
    if (!add) {
        clearChoice();
    }
    repaint();
    return true;
}

bool FormDesignOverlay::move(int row, const QPointF &pagePoint, Qt::MouseButtons buttons)
{
    Q_UNUSED(row)
    Q_UNUSED(buttons)
    if (m_drag == Drag::None) {
        return false;
    }
    m_to = pagePoint;

    // The numbers in the panel follow the drag rather than waiting for it to
    // end, because "how big is it now" is the question the drag is asking.
    refreshDetails();
    repaint();
    return true;
}

bool FormDesignOverlay::release(int row, const QPointF &pagePoint)
{
    Q_UNUSED(row)
    if (m_drag == Drag::None) {
        return false;
    }
    m_to = pagePoint;
    const Drag drag = m_drag;
    const QTransform gesture = liveTransform();
    m_drag = Drag::None;
    m_grip = Grip::None;

    if (drag == Drag::Create) {
        createField(m_dragRow, QRectF(m_from, m_to));
        repaint();
        return true;
    }

    if (drag == Drag::Band) {
        const QRectF band = QRectF(m_from, m_to).normalized();
        if (band.width() > 2.0 && band.height() > 2.0) {
            QVector<int> caught;
            for (const int handle : handlesOn(m_dragRow)) {
                if (!isRemoved(handle) && band.contains(rectOf(handle))) {
                    caught.append(handle);
                }
            }
            choose(m_dragRow, caught, QGuiApplication::keyboardModifiers().testFlag(Qt::ControlModifier));
        }
        repaint();
        return true;
    }

    const QPointF shift = m_to - m_from;
    const bool worthIt = drag != Drag::Move || qAbs(shift.x()) > DragSlopPoints || qAbs(shift.y()) > DragSlopPoints;
    if (worthIt && !gesture.isIdentity()) {
        for (const int handle : movableChosen()) {
            setRectOf(handle, gesture.mapRect(rectOf(handle)));
        }
        Q_EMIT editsChanged();
        refreshDetails();
    }
    repaint();
    return true;
}

Qt::CursorShape FormDesignOverlay::cursor(int row, const QPointF &pagePoint)
{
    if (!m_view) {
        return Qt::ArrowCursor;
    }
    if (m_kind != Kind::None) {
        return Qt::CrossCursor; // A tool is armed, and the next drag draws rather than picks.
    }

    const Grip grip = gripAt(row, pagePoint);
    if (grip != Grip::None) {
        return cursorForGrip(grip);
    }

    const int hit = fieldAt(row, pagePoint);
    setHover(row, hit);
    if (hit == 0) {
        return Qt::ArrowCursor;
    }
    if (!isMovable(hit)) {
        return Qt::PointingHandCursor; // Its properties are still the author's to change.
    }
    return row == m_page && m_chosen.contains(hit) ? Qt::SizeAllCursor : Qt::OpenHandCursor;
}

// ── The keyboard ──────────────────────────────────────────────────────────

bool FormDesignOverlay::eventFilter(QObject *watched, QEvent *event)
{
    if (watched != m_view || !m_view || m_view->mode() != PageView::Mode::Edit) {
        return QObject::eventFilter(watched, event);
    }
    if (m_chosen.isEmpty() && m_kind == Kind::None) {
        return QObject::eventFilter(watched, event);
    }

    // The window binds Delete to deleting whole pages, and a shortcut is matched
    // before the key ever reaches the widget. While a field is chosen, Delete
    // means that field; accepting the override sends the key on as a key and
    // leaves the pages alone.
    if (event->type() == QEvent::ShortcutOverride) {
        auto *pressed = static_cast<QKeyEvent *>(event);
        if (!m_chosen.isEmpty() && (pressed->key() == Qt::Key_Delete || pressed->key() == Qt::Key_Backspace)) {
            pressed->accept();
            return true;
        }
        return QObject::eventFilter(watched, event);
    }
    if (event->type() != QEvent::KeyPress) {
        return QObject::eventFilter(watched, event);
    }

    auto *key = static_cast<QKeyEvent *>(event);

    if (key->key() == Qt::Key_Escape) {
        // The armed tool goes first: Escape means "stop doing what you are about
        // to do" before it means "forget what I picked".
        if (m_kind != Kind::None) {
            setKindToPlace(Kind::None);
        } else {
            clearChoice();
        }
        return true;
    }
    if (m_chosen.isEmpty()) {
        return QObject::eventFilter(watched, event);
    }

    // A point at a time, ten with Shift: the numbers people reach for when
    // something is nearly in the right place.
    const double step = key->modifiers().testFlag(Qt::ShiftModifier) ? 10.0 : 1.0;
    switch (key->key()) {
    case Qt::Key_Left:
        nudge(-step, 0);
        return true;
    case Qt::Key_Right:
        nudge(step, 0);
        return true;
    case Qt::Key_Up:
        nudge(0, step);
        return true;
    case Qt::Key_Down:
        nudge(0, -step);
        return true;
    case Qt::Key_Delete:
    case Qt::Key_Backspace:
        removeChosen();
        return true;
    default:
        break;
    }
    return QObject::eventFilter(watched, event);
}

// ── The choice ────────────────────────────────────────────────────────────

void FormDesignOverlay::choose(int row, const QVector<int> &handles, bool add)
{
    const int wasPage = m_page;
    const QSet<int> was = m_chosen;

    // A choice lives on one page: a gesture means one thing in one page's
    // coordinates, and lining up boxes on two pages at once means nothing.
    if (row != m_page || !add) {
        m_chosen.clear();
        m_page = row;
    }

    for (const int handle : handles) {
        if (add && m_chosen.contains(handle)) {
            m_chosen.remove(handle);
            continue;
        }
        m_chosen.insert(handle);
        m_current = handle;
    }

    if (m_chosen.isEmpty()) {
        m_current = 0;
    } else if (!m_chosen.contains(m_current)) {
        m_current = *m_chosen.constBegin();
    }

    if (m_page != wasPage || m_chosen != was) {
        repaint();
        Q_EMIT choiceChanged();
    }
}

void FormDesignOverlay::clearChoice()
{
    if (m_page < 0 && m_chosen.isEmpty()) {
        return;
    }
    m_chosen.clear();
    m_page = -1;
    m_current = 0;
    m_drag = Drag::None;
    repaint();
    Q_EMIT choiceChanged();
}

void FormDesignOverlay::setHover(int row, int handle)
{
    if (m_hoverPage == row && m_hover == handle) {
        return;
    }
    m_hoverPage = row;
    m_hover = handle;
    repaint();
}

Inspectable *FormDesignOverlay::inspected()
{
    if (m_current == 0 || !m_chosen.contains(m_current)) {
        return nullptr;
    }
    if (!m_details) {
        m_details = std::make_unique<Details>(this);
    }
    refreshDetails();
    return m_details.get();
}

void FormDesignOverlay::refreshDetails()
{
    if (!m_details || m_current == 0) {
        return;
    }
    m_details->show(m_current, int(m_chosen.size()));
}

void FormDesignOverlay::repaint()
{
    if (m_view) {
        m_view->viewport()->update();
    }
}

// ── Arming a kind, and drawing one out ────────────────────────────────────

void FormDesignOverlay::setKindToPlace(Kind kind)
{
    if (m_kind == kind) {
        return;
    }
    if (kind != Kind::None && refusesAuthoring()) {
        // Said now rather than when the box has been drawn: a tool that can
        // never draw anything should not come up armed. The signal goes out even
        // though nothing changed here, because the button that asked for it is
        // showing itself as pressed and has to be put back.
        m_kind = Kind::None;
        Q_EMIT kindToPlaceChanged(m_kind);
        return;
    }

    // Buttons drawn one after another belong to one group: that is what "yes,
    // no, maybe" is. Leaving the kind starts the next group afresh.
    if (kind != Kind::Radio) {
        m_radioGroup.clear();
    }
    m_kind = kind;
    repaint();
    Q_EMIT kindToPlaceChanged(m_kind);
}

void FormDesignOverlay::startNewRadioGroup()
{
    m_radioGroup.clear();
}

QString FormDesignOverlay::uniqueName(const QString &stem) const
{
    QSet<QString> taken;
    for (const FormField &field : m_fields) {
        taken.insert(field.name);
    }
    for (auto waiting = m_changed.constBegin(); waiting != m_changed.constEnd(); ++waiting) {
        taken.insert(waiting->name);
    }
    for (const Draft &draft : m_drafts) {
        taken.insert(draft.spec.name);
    }

    int at = 1;
    QString name = stem + QString::number(at);
    while (taken.contains(name)) {
        name = stem + QString::number(++at);
    }
    return name;
}

void FormDesignOverlay::createField(int row, const QRectF &rect)
{
    if (m_kind == Kind::None || row < 0 || refusesAuthoring()) {
        return;
    }

    const BuilderKind kind = builderKindOf(m_kind);
    const QSizeF wanted = defaultSizeOf(kind);
    QRectF box = rect.normalized();

    // A click rather than a drag still places a field, at the size that kind
    // usually wants, with its top left corner where the pointer was. On the page
    // the larger y is the top, so that corner is the rectangle's bottom edge.
    if (box.width() < DragSlopPoints) {
        box.setWidth(wanted.width());
    }
    if (box.height() < DragSlopPoints) {
        box.setTop(box.bottom() - wanted.height());
    }
    box.setWidth(std::max(MinimumSizePoints, box.width()));
    box.setTop(std::min(box.top(), box.bottom() - MinimumSizePoints));

    Draft draft;
    draft.id = m_nextDraft++;
    draft.spec.kind = kind;
    // The row, because that is what the engine will be handed. It is moved
    // by rebaseRows() from here on, so that the field is written onto the sheet
    // it was drawn on however far the organiser rearranges the document first.
    draft.spec.page = row;
    draft.spec.rect = box;

    if (kind == BuilderKind::Radio) {
        // One field with several widgets, which is the only arrangement in which
        // the buttons take it in turns rather than toggling as one.
        if (m_radioGroup.isEmpty()) {
            m_radioGroup = uniqueName(stemFor(kind));
        }
        draft.spec.name = m_radioGroup;
        draft.spec.radioGroup = m_radioGroup;
        int already = 0;
        for (const Draft &other : m_drafts) {
            if (other.spec.radioGroup == m_radioGroup) {
                ++already;
            }
        }
        draft.spec.onState = u"/Option%1"_s.arg(already + 1);
    } else {
        draft.spec.name = uniqueName(stemFor(kind));
    }

    m_drafts.append(draft);
    choose(row, { -draft.id }, false);
    Q_EMIT editsChanged();
    repaint();
}

bool FormDesignOverlay::refusesAuthoring()
{
    if (m_xfa == Xfa::Unknown) {
        // The engine has no way of being asked "would you refuse this document",
        // and its answer only arrives once a whole file has been written. The
        // one entry that decides it is read here instead, so that the refusal
        // reaches the user while their hand is still on the mouse.
        m_xfa = Xfa::Absent;
        if (!m_source.isEmpty()) {
            try {
                QPDF pdf;
                PdfFile::open(pdf, m_source);
                QPDFObjectHandle acroForm = pdf.getRoot().getKey("/AcroForm");
                if (acroForm.isDictionary() && !acroForm.getKey("/XFA").isNull()) {
                    m_xfa = Xfa::Present;
                }
            } catch (const std::exception &) {
                // A file that will not open here will not open for the engine
                // either, which refuses it by name and in better words at the
                // moment the work is applied.
            }
        }
    }
    if (m_xfa != Xfa::Present) {
        return false;
    }
    Q_EMIT refused(xfaMessage());
    return true;
}

// ── Changing what is chosen ───────────────────────────────────────────────

void FormDesignOverlay::setRectOf(int handle, const QRectF &rect)
{
    FormBuilder::Field *spec = editableSpec(handle);
    if (!spec) {
        return;
    }
    QRectF box = rect.normalized();
    box.setWidth(std::max(MinimumSizePoints, box.width()));
    box.setHeight(std::max(MinimumSizePoints, box.height()));
    spec->rect = box;
}

void FormDesignOverlay::moveOne(int handle, const QRectF &rect)
{
    if (isRemoved(handle) || !isMovable(handle)) {
        return;
    }
    const QRectF was = rectOf(handle);
    setRectOf(handle, rect);
    if (rectOf(handle) == was) {
        return;
    }
    Q_EMIT editsChanged();
    refreshDetails();
    repaint();
}

void FormDesignOverlay::changeChosen(const std::function<void(FormBuilder::Field &)> &change)
{
    bool did = false;
    for (const int handle : chosenHandles()) {
        if (isRemoved(handle)) {
            continue;
        }
        if (FormBuilder::Field *spec = editableSpec(handle)) {
            change(*spec);
            did = true;
        }
    }
    if (!did) {
        return;
    }
    Q_EMIT editsChanged();
    refreshDetails();
    repaint();
}

void FormDesignOverlay::renameChosen(const QString &name)
{
    if (m_chosen.size() != 1 || m_current == 0) {
        return; // A name belongs to one field; several at once would be one name for all of them.
    }
    const QString wanted = name.trimmed();
    if (wanted.isEmpty()) {
        Q_EMIT refused(emptyNameMessage());
        return;
    }
    const QString had = specOf(m_current).name;
    if (wanted == had) {
        return;
    }

    // Against every name in the document and every name waiting, because both
    // will be in the file by the time the work is applied.
    for (int at = 0; at < m_fields.size(); ++at) {
        const int handle = handleOfField(at);
        if (handle != m_current && !m_removed.contains(at) && specOf(handle).name == wanted) {
            Q_EMIT refused(takenNameMessage(wanted));
            return;
        }
    }
    for (const Draft &draft : m_drafts) {
        if (-draft.id != m_current && draft.spec.name == wanted && draft.spec.radioGroup != had) {
            Q_EMIT refused(takenNameMessage(wanted));
            return;
        }
    }

    if (!isDraft(m_current)) {
        // The engine renames a field in place and will not carry it into another
        // group, because whatever refers to it by name would be left pointing at
        // nothing. Asked here, the answer arrives before the work is collected.
        const QString original = m_fields.at(fieldIndex(m_current)).name;
        const QStringList was = original.split(u'.', Qt::SkipEmptyParts);
        const QStringList now = wanted.split(u'.', Qt::SkipEmptyParts);
        if (was.size() != now.size() || was.mid(0, was.size() - 1) != now.mid(0, now.size() - 1)) {
            Q_EMIT refused(groupMessage(original, wanted));
            return;
        }
    }

    // A group of buttons is one field under one name, so renaming any of them
    // renames all of them; leaving them apart would make each button a field of
    // its own and they would stop taking it in turns.
    const bool group = isDraft(m_current) && !specOf(m_current).radioGroup.isEmpty();
    if (group) {
        const QString was = specOf(m_current).radioGroup;
        for (Draft &draft : m_drafts) {
            if (draft.spec.radioGroup == was) {
                draft.spec.radioGroup = wanted;
                draft.spec.name = wanted;
            }
        }
        if (m_radioGroup == was) {
            m_radioGroup = wanted;
        }
    } else if (FormBuilder::Field *spec = editableSpec(m_current)) {
        spec->name = wanted;
        if (spec->kind == BuilderKind::Radio) {
            spec->radioGroup = wanted;
        }
    }

    Q_EMIT editsChanged();
    refreshDetails();
    Q_EMIT choiceChanged(); // The heading names the field, so it has to be rebuilt.
    repaint();
}

void FormDesignOverlay::nudge(double acrossPoints, double upPoints)
{
    const QVector<int> movable = movableChosen();
    const int stuck = int(m_chosen.size() - movable.size());
    if (stuck > 0) {
        Q_EMIT refused(unmovableMessage(stuck));
    }
    if (movable.isEmpty()) {
        return;
    }
    for (const int handle : movable) {
        setRectOf(handle, rectOf(handle).translated(acrossPoints, upPoints));
    }
    Q_EMIT editsChanged();
    refreshDetails();
    repaint();
}

void FormDesignOverlay::placeChosen(const QVector<QRectF> &boxes)
{
    const QVector<int> movable = movableChosen();
    if (boxes.size() != movable.size()) {
        // The choice changed under the panel between the buttons being drawn and
        // one of them being pressed, so these boxes name fields that are no
        // longer the ones chosen.
        return;
    }

    // Said before anything moves, and said even when nothing can: "four of the
    // five lined up" is a question the user would otherwise have to answer by
    // staring at the page.
    const int stuck = int(m_chosen.size() - movable.size());
    if (stuck > 0) {
        Q_EMIT refused(unmovableMessage(stuck));
    }

    bool did = false;
    for (int at = 0; at < movable.size(); ++at) {
        const QRectF to = boxes.at(at).normalized();
        const QRectF from = rectOf(movable.at(at));
        // Only what actually has somewhere to go: a field already on the line
        // would otherwise collect a waiting change that moves it nowhere.
        if (qAbs(from.x() - to.x()) < 0.001 && qAbs(from.y() - to.y()) < 0.001
            && qAbs(from.width() - to.width()) < 0.001 && qAbs(from.height() - to.height()) < 0.001) {
            continue;
        }
        setRectOf(movable.at(at), to);
        did = true;
    }
    if (!did) {
        return;
    }
    Q_EMIT editsChanged();
    refreshDetails();
    repaint();
}

void FormDesignOverlay::alignChosen(Alignment::Edge edge)
{
    placeChosen(Alignment::align(alignableBoxes(), edge));
}

void FormDesignOverlay::distributeChosen(Alignment::Spread spread)
{
    placeChosen(Alignment::distribute(alignableBoxes(), spread));
}

void FormDesignOverlay::matchChosenSize(Alignment::Match what)
{
    placeChosen(Alignment::matchSize(alignableBoxes(), what));
}

void FormDesignOverlay::removeChosen()
{
    const QVector<int> chosen = chosenHandles();
    if (chosen.isEmpty()) {
        return;
    }

    bool did = false;
    for (const int handle : chosen) {
        if (isDraft(handle)) {
            // Never written, so there is nothing to wait for: it simply goes.
            for (int at = 0; at < m_drafts.size(); ++at) {
                if (m_drafts.at(at).id == -handle) {
                    m_drafts.remove(at);
                    did = true;
                    break;
                }
            }
            continue;
        }
        const int index = fieldIndex(handle);
        if (index < 0 || index >= m_fields.size() || m_removed.contains(index)) {
            continue;
        }
        // Waiting rather than done, and whatever was waiting on the field goes
        // with it: changing a field the same pass is about to take off the form
        // would be work addressed to nothing.
        m_removed.insert(index);
        m_changed.remove(index);
        did = true;
    }
    if (!did) {
        return;
    }
    clearChoice();
    Q_EMIT editsChanged();
    repaint();
}

void FormDesignOverlay::takeBackAll()
{
    if (!hasPendingWork()) {
        return;
    }
    m_changed.clear();
    m_removed.clear();
    m_drafts.clear();
    m_radioGroup.clear();
    clearChoice();
    // The render stands for work that has just been withdrawn, so it goes rather
    // than showing the fields where they were about to be.
    dropLiveRender();
    Q_EMIT editsChanged();
    repaint();
}

// ── Collecting the work ───────────────────────────────────────────────────

FormDesignOverlay::Work FormDesignOverlay::pendingWork() const
{
    return collect(-1);
}

FormDesignOverlay::Work FormDesignOverlay::workOn(int row) const
{
    return row < 0 ? Work() : collect(row);
}

FormDesignOverlay::Work FormDesignOverlay::collect(int onlyRow) const
{
    Work work;

    // First, because these are the names the file still has: nothing has been
    // renamed at the point the window makes this call.
    QVector<int> removed(m_removed.constBegin(), m_removed.constEnd());
    std::sort(removed.begin(), removed.end());
    for (const int index : std::as_const(removed)) {
        if (index < 0 || index >= m_fields.size()) {
            continue;
        }
        if (onlyRow >= 0 && rowOf(handleOfField(index)) != onlyRow) {
            continue;
        }
        work.removed.append(m_fields.at(index).name);
    }

    QVector<int> touched(m_changed.keyBegin(), m_changed.keyEnd());
    std::sort(touched.begin(), touched.end());
    for (const int index : std::as_const(touched)) {
        if (index < 0 || index >= m_fields.size() || m_removed.contains(index)) {
            continue;
        }
        FormBuilder::Field spec = m_changed.value(index);

        // toSpec() copied the page number the form's own file gave the field,
        // which is the one number the engine must not be handed: it is given the
        // document row by row, so a field described by its page in the original
        // would be rewritten onto whichever page has since moved into that slot.
        const int row = rowOf(handleOfField(index));
        if (row < 0 || (onlyRow >= 0 && row != onlyRow)) {
            continue;
        }
        spec.page = onlyRow >= 0 ? 0 : row;

        const QString original = m_fields.at(index).name;
        if (spec.name != original) {
            work.renamed.append({ original, spec.name });
        }
        // The description carries the name the field has after the renaming, so
        // that the one call that changes everything else can still find it.
        if (spec.kind == BuilderKind::Radio) {
            spec.radioGroup = spec.name;
        }
        work.changed.append(spec);
    }

    for (const Draft &draft : m_drafts) {
        if (onlyRow >= 0 && draft.spec.page != onlyRow) {
            continue;
        }
        FormBuilder::Field spec = draft.spec;
        if (onlyRow >= 0) {
            spec.page = 0;
        }
        work.added.append(spec);
    }
    return work;
}

bool FormDesignOverlay::hasPendingWork() const
{
    return !m_drafts.isEmpty() || !m_removed.isEmpty() || !m_changed.isEmpty();
}

} // namespace ps
