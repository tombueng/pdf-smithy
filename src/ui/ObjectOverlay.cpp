/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "ObjectOverlay.h"

#include "AlignmentPanel.h"
#include "core/Document.h"
#include "core/DocumentWriter.h"
#include "core/ImageEdit.h"
#include "render/PopplerBackend.h"

#include <KLocalizedString>

#include <QClipboard>
#include <QColorDialog>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFormLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineF>
#include <QLocale>
#include <QMimeData>
#include <QPainter>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTemporaryFile>
#include <QTimerEvent>
#include <QVBoxLayout>
#include <QWidget>
#include <QtConcurrentRun>
#include <QtMath>

#include <algorithm>
#include <cmath>

using namespace Qt::Literals::StringLiterals;

namespace ps {

namespace {

/** How near a handle counts as being on it, and how big it is drawn, in pixels. */
constexpr double HandleReach = 7.0;
constexpr double HandleSize = 8.0;

/** How far above the box the turn handle floats, in pixels. */
constexpr double TurnArm = 26.0;

/** Nothing may be dragged smaller than this, or it can never be caught again. */
constexpr double MinimumSizePoints = 2.0;

/** Under this, a drag was a click that wobbled. */
constexpr double DragSlopPoints = 0.4;

/**
 * Under this, two boundaries are the same one, in points.
 *
 * Aligning a row leaves the objects that were already on the line alone, and
 * "already" has to allow for the tenth of a point that a scale and its inverse
 * leave behind. Otherwise every object in the selection collects a waiting
 * change and the document is dirty for nothing.
 */
constexpr double BoxSlopPoints = 0.001;

/**
 * How far a pasted copy lands from the thing it was copied from, in points.
 *
 * Far enough to be seen as a second object rather than as a paste that did
 * nothing, near enough that it is obviously the same thing.
 */
constexpr double PasteStepPoints = 12.0;

/** The character PageObjects puts where it could not read a byte as a letter. */
constexpr char16_t Unreadable = u'·';

/** How far the composer steps down for each line of text after the first. */
constexpr double LineStep = 1.35;

/**
 * How long a choice has to stand still before it is worth lifting, in ms.
 *
 * Long enough that adding five objects to a choice one Ctrl-click at a time is
 * one preparation rather than five, short enough that it is spent inside the
 * press that started the drag.
 */
constexpr int LiftSettleMs = 40;

/**
 * How far past its own boundary an object's ink is looked for, in points.
 *
 * A boundary is measured from the operators rather than from the ink, and the
 * two are not the same to the pixel: a stroke is centred on its path, a glyph
 * overshoots the box its font metrics promise, and clipping is followed only as
 * far as a rectangle. Cheap to allow for, and what it costs when it is not
 * needed is a border of page that is copied back exactly where it came from.
 */
constexpr double LiftMarginPoints = 3.0;

/**
 * The most memory one lift may hold, in bytes.
 *
 * There is only ever one, because there is only ever one choice, so this is the
 * whole of what the arrangement costs. A whole page at reading size is about
 * eleven megabytes of it, so the ceiling is only ever reached by choosing most
 * of a page at a large zoom, and what it does then is render the lift smaller
 * and stretch it. That is the trade the gesture is already making: a bitmap
 * being turned is soft, and saving the page is what makes it exact.
 */
constexpr qint64 LiftBudgetBytes = 64LL * 1024 * 1024;

/** Both pictures of a lifted pixel: the ink and the page under it. */
constexpr int LiftBytesPerPixel = 8;

/** Whether two waiting changes would draw the same pixels, wherever they put them. */
bool sameLook(const PageEdit &a, const PageEdit &b)
{
    return a.kind == b.kind && a.fill == b.fill && a.stroke == b.stroke
        && qFuzzyCompare(1.0 + a.lineWidth, 1.0 + b.lineWidth);
}

/** Whether two waiting changes are the same change, place included. */
bool sameEdit(const PageEdit &a, const PageEdit &b)
{
    return sameLook(a, b) && a.transform == b.transform;
}

/** What @p change does to an object's shape, with where it puts it left out. */
QTransform shapeOf(const QTransform &change)
{
    return { change.m11(), change.m12(), change.m21(), change.m22(), 0.0, 0.0 };
}

/**
 * Whether ink made for @p had would still be ink of @p want.
 *
 * Deletion is deliberately not a difference. Something waiting to be taken off
 * the page already has its hole cut, and the ink is simply not put back, so
 * making the pictures again would be work whose whole result is what is on the
 * screen already. Taking the deletion back is a difference, because the ink then
 * has to come from somewhere.
 */
bool stillInk(const PageEdit &want, const PageEdit &had)
{
    return want.kind == PageEdit::Kind::Delete || sameLook(want, had);
}

/**
 * The ink, from the same objects drawn over white and over black.
 *
 * Over black a pixel is coverage times colour, which is a premultiplied pixel
 * already; over white the leftover white is added to it. So the difference
 * between the two says how much white was left, which is how much of the pixel
 * the object did not cover. See ObjectOverlay::startLift.
 */
QImage inkBetween(const QImage &overWhite, const QImage &overBlack)
{
    if (overWhite.isNull() || overWhite.size() != overBlack.size()) {
        return {};
    }
    const QImage white = overWhite.convertToFormat(QImage::Format_RGB32);
    const QImage black = overBlack.convertToFormat(QImage::Format_RGB32);

    QImage ink(white.size(), QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < ink.height(); ++y) {
        const auto *pale = reinterpret_cast<const QRgb *>(white.constScanLine(y));
        const auto *dark = reinterpret_cast<const QRgb *>(black.constScanLine(y));
        auto *out = reinterpret_cast<QRgb *>(ink.scanLine(y));
        for (int x = 0; x < ink.width(); ++x) {
            // The mean of the three, because the rasteriser rounds each channel
            // on its own and a single channel would carry that rounding into
            // the coverage of every pixel of every edge.
            const int through = (qRed(pale[x]) - qRed(dark[x]) + qGreen(pale[x]) - qGreen(dark[x]) + qBlue(pale[x])
                                 - qBlue(dark[x]) + 1)
                / 3;
            const int covered = qBound(0, 255 - through, 255);

            // Premultiplied means no channel may exceed the coverage; rounding
            // can put one a step over, and Qt draws that as a colour nobody
            // asked for.
            out[x] = qRgba(std::min(qRed(dark[x]), covered), std::min(qGreen(dark[x]), covered),
                           std::min(qBlue(dark[x]), covered), covered);
        }
    }
    return ink;
}

/** One measurement, written the way a page is measured: points, one decimal. */
QString points(double value)
{
    return QLocale().toString(value, 'f', 1);
}

/**
 * One hue per kind, the same six the objects tool uses.
 *
 * Fixed rather than taken from the palette: these are six categories that have
 * to stay apart from each other and from the paper underneath them, which a
 * theme's two or three accent colours cannot promise.
 */
QColor tintOf(PageObject::Kind kind)
{
    switch (kind) {
    case PageObject::Kind::Text:
        return { 60, 120, 205 };
    case PageObject::Kind::Image:
    case PageObject::Kind::InlineImage:
        return { 40, 155, 110 };
    case PageObject::Kind::Path:
        return { 205, 130, 45 };
    case PageObject::Kind::Drawing:
        return { 145, 95, 190 };
    case PageObject::Kind::Shading:
        return { 200, 85, 140 };
    }
    return { 120, 120, 120 };
}

QString colourText(const QColor &colour)
{
    return colour.isValid() ? colour.name() : i18nc("@item no colour is set", "none");
}

/**
 * The four corners of @p box, in the order they are joined up.
 *
 * A boundary is drawn as four corners rather than as a rectangle because a
 * turned object no longer has an upright one. The box round a turned rectangle
 * is bigger than the rectangle and a different shape, so drawing that instead
 * shows a turn as a stretch, which is what it looked like.
 */
QPolygonF cornersOf(const QRectF &box)
{
    const QRectF area = box.normalized();
    return QPolygonF() << area.topLeft() << area.topRight() << area.bottomRight() << area.bottomLeft();
}

/** Near enough that moving one onto the other would change nothing. */
bool sameBox(const QRectF &one, const QRectF &other)
{
    return qAbs(one.x() - other.x()) < BoxSlopPoints && qAbs(one.y() - other.y()) < BoxSlopPoints
        && qAbs(one.width() - other.width()) < BoxSlopPoints && qAbs(one.height() - other.height()) < BoxSlopPoints;
}

/**
 * The matrix that carries @p from onto @p to, both normalised.
 *
 * A translation while the size holds, a translation and a scale when it does
 * not. The scale is taken about the corner the rectangle is being put at rather
 * than about its middle, so that lining a column up on its left edge and then
 * giving it all one width leaves the edge where the first command put it.
 *
 * A rectangle with no width or no height (a hairline rule is one) is only
 * moved along that axis, because there is nothing to scale up from zero.
 */
QTransform placement(const QRectF &from, const QRectF &to)
{
    const double across = from.width() > 0.0 ? to.width() / from.width() : 1.0;
    const double up = from.height() > 0.0 ? to.height() / from.height() : 1.0;
    if (qAbs(across - 1.0) < BoxSlopPoints && qAbs(up - 1.0) < BoxSlopPoints) {
        return QTransform::fromTranslate(to.x() - from.x(), to.y() - from.y());
    }
    return QTransform::fromTranslate(-from.x(), -from.y()) * QTransform::fromScale(across, up)
        * QTransform::fromTranslate(to.x(), to.y());
}

QString unmovableMessage(int count)
{
    return i18ncp("@info an object the engine cannot transform at all",
                  "One object cannot be moved: its placement is flattened into the page and cannot be reversed "
                  "arithmetically.",
                  "%1 objects cannot be moved: their placement is flattened into the page and cannot be reversed "
                  "arithmetically.",
                  count);
}

QString oneChangeMessage()
{
    return i18nc("@info why one waiting change has taken the place of another",
                 "The page is rewritten in a single pass, so an object carries one change at a time. What was "
                 "waiting on it has been dropped.");
}

QString cannotRebuildMessage(PageObject::Kind kind)
{
    return i18nc("@info %1 is the kind of object, such as Shape or Drawing",
                 "%1 was left out of the copy. The engine writes text, pictures and simple shapes onto a page; it "
                 "cannot build drawing operators it did not write itself, so there is nothing to paste it from.",
                 PageObjects::describe(kind));
}

QString substituteFontMessage()
{
    return i18nc("@info what a pasted line of text loses",
                 "Copied text is set again in a standard font. The page's own font stays with the letters that "
                 "were already on it, so the copy will not match them exactly.");
}

QString unreadableTextMessage()
{
    return i18nc("@info characters that could not be read back off the page",
                 "Some characters could not be read back from the page and were left out of the copy. This happens "
                 "where a font gives no way of telling which letter a code stands for.");
}

QString orderMessage()
{
    return i18nc("@info why the stacking order cannot be changed",
                 "What lies on top of what cannot be changed yet. It would mean moving whole blocks of drawing "
                 "operators, which is only safe when each block is properly enclosed, and on a real page they "
                 "often are not.");
}

/** What a pasted thing is called before it has a resource name of its own. */
QString pastedName(NewContent::Kind kind)
{
    switch (kind) {
    case NewContent::Kind::Text:
        return i18nc("@item something pasted and not yet written to the file", "pasted text");
    case NewContent::Kind::Image:
        return i18nc("@item something pasted and not yet written to the file", "pasted picture");
    case NewContent::Kind::Rectangle:
    case NewContent::Kind::Ellipse:
    case NewContent::Kind::Line:
        break;
    }
    return i18nc("@item something pasted and not yet written to the file", "pasted shape");
}

/**
 * How much taller a pasted thing now stands than it was pasted, or 1.
 *
 * Type size is the one property that a NewContent carries beside its box rather
 * than inside it, so pulling the box taller has to be passed on to it by hand. A
 * line of text the file already holds is wrapped in the scale and really does
 * grow; one that kept its size after being stretched would be the two kinds of
 * object drifting apart again.
 */
double stretchOf(const QRectF &pasted, const QRectF &now)
{
    const double was = pasted.normalized().height();
    return was > 0.0 ? now.normalized().height() / was : 1.0;
}

/** The box round @p content where it now stands: its own box, turned as it is. */
QRectF whereItStands(const NewContent &content)
{
    return content.placement.mapRect(content.rect.normalized()).normalized();
}

/**
 * Whether a rectangle on its own can say what @p gesture did.
 *
 * A move and a stretch, and nothing else. Qt calls half a turn a negative scale
 * rather than a rotation, which is true of the matrix and false of the picture:
 * a box mapped through it comes back as the same box, so a turn let through here
 * would be a turn silently thrown away, and half a turn is exactly the one that
 * every other check agrees with, being its own inverse.
 */
bool aBoxCanSayIt(const QTransform &gesture)
{
    return gesture.type() <= QTransform::TxScale && gesture.m11() > 0.0 && gesture.m22() > 0.0;
}

/**
 * @p content moved, scaled or turned by @p gesture, in display points.
 *
 * Something waiting to be written is a box and a matrix, and a gesture goes into
 * whichever of the two can say it. A move or a stretch made on something still
 * standing upright is the box's to carry: that is what a box is for, and the
 * type size of a line of text follows the box it is set in. Anything else is the
 * matrix's: a turn folded into a box instead is no turn at all, only a picture
 * squared up again and grown to the size of the box that was round it.
 *
 * Both ways round put the content in exactly the same place. What differs is
 * only which half of the record says so, and the gesture is always folded into
 * the copy as it was pasted, so which half never depends on the order the
 * gestures were made in.
 */
NewContent contentUnder(const NewContent &content, const QTransform &gesture)
{
    NewContent moved = content;
    if (gesture.isIdentity()) {
        return moved;
    }
    if (content.placement.isIdentity() && aBoxCanSayIt(gesture)) {
        const QRectF now = gesture.mapRect(content.rect.normalized()).normalized();
        moved.fontSize *= stretchOf(content.rect, now);
        moved.rect = now;
        return moved;
    }
    moved.placement = content.placement * gesture;
    return moved;
}

/** A button that shows the colour it holds and picks a new one when pressed. */
class Swatch : public QPushButton
{
public:
    Swatch(const QColor &initial, QWidget *parent, const std::function<void(const QColor &)> &picked)
        : QPushButton(parent)
        , m_colour(initial)
    {
        setAutoFillBackground(true);
        refresh();
        connect(this, &QPushButton::clicked, this, [this, picked] {
            const QColor chosen = QColorDialog::getColor(m_colour.isValid() ? m_colour : QColor(Qt::black), this,
                                                         i18nc("@title:window", "Pick a Colour"));
            if (chosen.isValid()) {
                m_colour = chosen;
                refresh();
                picked(chosen);
            }
        });
    }

private:
    void refresh()
    {
        setText(colourText(m_colour));
        if (!m_colour.isValid()) {
            setStyleSheet(QString());
            return;
        }
        // Black text on a dark swatch is unreadable, so the label follows the
        // swatch rather than the theme.
        const QString ink = m_colour.lightnessF() > 0.55 ? u"#101010"_s : u"#f0f0f0"_s;
        setStyleSheet(u"background-color: %1; color: %2;"_s.arg(m_colour.name(), ink));
    }

    QColor m_colour;
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

} // namespace

// ── What the properties panel shows ───────────────────────────────────────

/**
 * The chosen object's attributes, as the panel shows them.
 *
 * What the engine can change is a control and is written through as it is
 * changed; what it cannot is a label. An editable font field would be a promise
 * the composer cannot keep: it wraps the operators that are already there
 * rather than rewriting them, so the letters keep the font the file gives them.
 */
class ObjectOverlay::Details : public Inspectable
{
public:
    explicit Details(ObjectOverlay *overlay)
        : m_overlay(overlay)
    {
    }

    /** Called whenever the object, or what is waiting on it, has changed. */
    void show(const PageObject &object, int chosenCount, int objectsOnPage)
    {
        m_object = object;
        m_chosen = chosenCount;
        m_onPage = objectsOnPage;
        pushGeometry();
    }

    QString kindName() const override { return PageObjects::describe(m_object.kind); }

    QString description() const override;
    QWidget *buildEditor(QWidget *parent) override;

private:
    /** Puts the numbers back after a drag has moved what they describe. */
    void pushGeometry();

    /** The chosen object with everything waiting on it already applied. */
    PageObject m_object;
    int m_chosen = 0;
    int m_onPage = 0;

    ObjectOverlay *m_overlay;

    // The panel is built fresh on every change of choice and may be gone at any
    // moment, so each of these is held weakly and checked before it is used.
    QPointer<QDoubleSpinBox> m_x;
    QPointer<QDoubleSpinBox> m_y;
    QPointer<QDoubleSpinBox> m_width;
    QPointer<QDoubleSpinBox> m_height;
    QPointer<QLabel> m_resolution;
    QPointer<AlignmentPanel> m_alignment;
    QPointer<QLabel> m_summary;
};

QString ObjectOverlay::Details::description() const
{
    QString what;
    switch (m_object.kind) {
    case PageObject::Kind::Text: {
        const QString said = m_object.text.simplified();
        what = i18nc("@info what a text object says", "“%1”", said.size() > 42 ? said.left(41) + u'…' : said);
        break;
    }
    case PageObject::Kind::Image:
    case PageObject::Kind::InlineImage:
    case PageObject::Kind::Drawing:
    case PageObject::Kind::Shading:
    case PageObject::Kind::Path:
        // A pasted thing arrives with the name it is called by already on it,
        // because "object -3" is a number nobody has any use for.
        what = m_object.resourceName.isEmpty()
            ? i18nc("@info an object with nothing to name it by", "object %1", m_object.index)
            : m_object.resourceName;
        break;
    }

    if (m_chosen > 1) {
        return i18ncp("@info what is chosen, when it is more than one thing", "%2 and one more object",
                      "%2 and %1 more objects", m_chosen - 1, what);
    }
    return what;
}

QWidget *ObjectOverlay::Details::buildEditor(QWidget *parent)
{
    auto *box = new QWidget(parent);
    auto *column = new QVBoxLayout(box);
    column->setContentsMargins(0, 0, 0, 0);

    // The panel built for the previous choice may still be alive for a moment
    // after this one is built, and nothing here may write into it.
    m_alignment = nullptr;
    m_summary = nullptr;

    // Above the numbers rather than below them: with more than one thing chosen,
    // lining them up is what is wanted first, and the four spin boxes underneath
    // describe only whichever of them was pointed at last.
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
    column->addLayout(form);

    const int index = m_object.index;
    const bool movable = m_object.placement.isInvertible();

    // ── where it sits and how big it is ───────────────────────────────────
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
    form->addRow(i18nc("@label:spinbox where an object sits on the page", "Place:"), place);
    form->addRow(i18nc("@label:spinbox how wide and how tall an object is", "Size:"), size);

    // The overlay outlives the panel, so capturing it is safe; the panel is the
    // context object, so the connection dies with the widgets rather than
    // writing into a spin box that has already been destroyed.
    QObject::connect(m_x, &QDoubleSpinBox::valueChanged, box, [this, index](double value) {
        const double delta = value - m_object.bounds.normalized().left();
        if (qAbs(delta) > 0.001) {
            m_overlay->applyGesture([delta](const QRectF &) { return QTransform::fromTranslate(delta, 0); }, { index });
        }
    });
    QObject::connect(m_y, &QDoubleSpinBox::valueChanged, box, [this, index](double value) {
        const double delta = value - m_object.bounds.normalized().top();
        if (qAbs(delta) > 0.001) {
            m_overlay->applyGesture([delta](const QRectF &) { return QTransform::fromTranslate(0, delta); }, { index });
        }
    });

    // Growing holds the corner the two numbers above name, so that setting a
    // width and then a height does not walk the object across the page.
    const auto stretch = [this, index](bool across, double wanted) {
        const QRectF now = m_object.bounds.normalized();
        const double have = across ? now.width() : now.height();
        if (have <= 0.0 || qAbs(wanted - have) < 0.001) {
            return;
        }
        const double factor = wanted / have;
        const QPointF anchor(now.left(), now.top());
        m_overlay->applyGesture(
            [factor, across, anchor](const QRectF &) {
                return QTransform::fromTranslate(-anchor.x(), -anchor.y())
                    * QTransform::fromScale(across ? factor : 1.0, across ? 1.0 : factor)
                    * QTransform::fromTranslate(anchor.x(), anchor.y());
            },
            { index });
    };
    QObject::connect(m_width, &QDoubleSpinBox::valueChanged, box, [stretch](double value) { stretch(true, value); });
    QObject::connect(m_height, &QDoubleSpinBox::valueChanged, box, [stretch](double value) { stretch(false, value); });

    if (!movable) {
        auto *stuck = new QLabel(unmovableMessage(1), box);
        stuck->setWordWrap(true);
        form->addRow(stuck);
    }

    // ── colours ───────────────────────────────────────────────────────────
    const bool shape = m_object.kind == PageObject::Kind::Path;
    const bool letters = m_object.kind == PageObject::Kind::Text;

    if (shape || letters) {
        auto *fill = new Swatch(m_object.fill, box,
                                [this](const QColor &colour) { m_overlay->restyleChosen(colour, QColor(), -1.0); });
        form->addRow(letters ? i18nc("@label:chooser the colour of the letters", "Ink:")
                             : i18nc("@label:chooser the colour inside a shape", "Fill:"),
                     fill);

        if (shape) {
            auto *width = measure(box, 0, 100);
            width->setDecimals(2);
            width->setValue(m_object.lineWidth);
            auto *stroke = new Swatch(m_object.stroke, box, [this](const QColor &colour) {
                m_overlay->restyleChosen(QColor(), colour, -1.0);
            });

            auto *outline = new QHBoxLayout;
            outline->addWidget(stroke, 1);
            outline->addWidget(width);
            form->addRow(i18nc("@label:chooser the colour and width of a shape's outline", "Outline:"), outline);

            QObject::connect(width, &QDoubleSpinBox::valueChanged, box,
                             [this](double value) { m_overlay->restyleChosen(QColor(), QColor(), value); });
        }
    } else {
        auto *plain = new QLabel(i18n("A picture carries its own colours and takes no notice of one set here."), box);
        plain->setWordWrap(true);
        form->addRow(plain);
    }

    // ── what only this kind of object has ─────────────────────────────────
    if (letters) {
        form->addRow(i18nc("@label the font a text object is set in", "Font:"),
                     new QLabel(i18nc("@info a font resource and the size it is used at", "%1 · %2 pt",
                                      m_object.fontResource.isEmpty() ? i18nc("@item the object names no font", "none")
                                                                      : m_object.fontResource,
                                      points(m_object.fontSize)),
                                box));
    }
    if (m_object.kind == PageObject::Kind::Image || m_object.kind == PageObject::Kind::InlineImage) {
        m_resolution = new QLabel(box);
        form->addRow(i18nc("@label how many pixels a picture holds", "Pixels:"), m_resolution);
    }

    form->addRow(i18nc("@label where an object comes in the page's painting order", "Order:"),
                 new QLabel(i18nc("@info paint order, and that it is not ours to change yet",
                                  "%1 of %2 from the bottom · cannot be changed yet", m_object.layer + 1, m_onPage),
                            box));

    auto *note = new QLabel(i18n("Nothing here is written until the document is saved. An object carries one change "
                                 "at a time, so a colour takes the place of a move that was waiting on it."),
                            box);
    note->setWordWrap(true);
    form->addRow(note);

    pushGeometry();
    return box;
}

void ObjectOverlay::Details::pushGeometry()
{
    const QRectF bounds = m_object.bounds.normalized();

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
        // an already aligned row a second time.
        QSignalBlocker blocker(m_alignment);
        m_alignment->setBoxes(m_overlay->alignableBoxes());
    }
    if (m_summary) {
        const QRectF round = m_overlay->choiceBounds();
        m_summary->setText(i18ncp("@info how many objects are chosen, and the box round all of them",
                                  "One object chosen · %2 × %3 pt", "%1 objects chosen · %2 × %3 pt", m_chosen,
                                  points(round.width()), points(round.height())));
    }

    if (m_resolution) {
        // How densely the pixels land follows the size on the page, so a picture
        // dragged smaller says so here while it is being dragged.
        const double across = bounds.width() > 0.0 ? m_object.pixelSize.width() / bounds.width() * 72.0 : 0.0;
        const double down = bounds.height() > 0.0 ? m_object.pixelSize.height() / bounds.height() * 72.0 : 0.0;
        m_resolution->setText(i18nc("@info a picture's pixels and how densely they land on the page",
                                    "%1 × %2 px · %3 × %4 dpi", m_object.pixelSize.width(), m_object.pixelSize.height(),
                                    QLocale().toString(qRound(across)), QLocale().toString(qRound(down))));
    }
}

// ── Setting up ────────────────────────────────────────────────────────────

ObjectOverlay::ObjectOverlay(PageView *view, QObject *parent)
    : QObject(parent)
    , m_view(view)
{
    if (!m_view) {
        return;
    }
    m_view->addOverlay(this);

    // Key presses go to the view, which is what has the focus; the Overlay
    // interface has no hook for them, and an arrow key has to reach what was
    // just clicked rather than scroll the page out from under it.
    m_view->installEventFilter(this);

    connect(m_view, &PageView::modeChanged, this, [this](PageView::Mode mode) {
        // Handles on a page that can no longer be changed would be an offer the
        // view is not making any more.
        if (mode != PageView::Mode::Edit) {
            clearChoice();
        }
    });

    // The two signals this emits itself are exactly the two things a lift is
    // about: what is chosen, and what is waiting on it. Answering its own
    // signals rather than calling reviewLift() from a dozen places is what keeps
    // a gesture nobody thought of from leaving stale pixels on the page.
    connect(this, &ObjectOverlay::choiceChanged, this, &ObjectOverlay::reviewLift);
    connect(this, &ObjectOverlay::editsChanged, this, &ObjectOverlay::reviewLift);

    // A zoom does not make the pixels wrong, only soft: they stay up, stretched,
    // and sharper ones are asked for. Which is why this is reviewLift() and not
    // dropLift().
    connect(m_view, &PageView::zoomChanged, this, &ObjectOverlay::reviewLift);
}

ObjectOverlay::~ObjectOverlay()
{
    if (m_view) {
        m_view->removeEventFilter(this);
        m_view->removeOverlay(this);
    }

    // A preparation still in flight writes into the scratch directory this owns
    // and hands its answer back by queued call, so it has to be finished with
    // before either of those goes away.
    if (m_liftWatcher) {
        m_liftWatcher->waitForFinished();
    }
}

void ObjectOverlay::setDocument(Document *document)
{
    m_document = document;
    m_rows.setDocument(document);
    if (document) {
        // Everything waiting names a row, and a row is a position in a list the
        // organiser rearranges. Following it is what lets a picture dragged on
        // page ten survive somebody deleting page one.
        //
        // Queued, because moving a page is a removal followed by an insertion:
        // between the two signals the page is nowhere at all, and answering that
        // state would announce as lost what the user only dragged elsewhere.
        connect(document, &Document::pagesInserted, this, &ObjectOverlay::rebaseRows, Qt::QueuedConnection);
        connect(document, &Document::pagesRemoved, this, &ObjectOverlay::rebaseRows, Qt::QueuedConnection);

        // A row that changed in place kept its number: it was turned, or an
        // operation put a freshly written file behind it. Both change what is
        // drawn on it, and neither can lose anything, so it is answered at once.
        connect(document, &Document::pagesChanged, this, &ObjectOverlay::refreshRows);
    }
    m_objects.clear();
    dropLift();
    repaint();
}

void ObjectOverlay::rebaseRows()
{
    carryRows(m_rows.follow());
}

void ObjectOverlay::refreshRows()
{
    carryRows(m_rows.followInPlace());
}

void ObjectOverlay::carryRows(const Rebase &change)
{
    if (change.isIdentity()) {
        return;
    }

    QHash<int, QVector<PageObject>> objects;
    QHash<int, QHash<int, PageEdit>> pending;
    int lost = 0;

    for (auto page = m_objects.constBegin(); page != m_objects.constEnd(); ++page) {
        const int row = change.nowAt(page.key());
        // A row pointed at a freshly written file has different operators on it
        // and different numbers for them, so what was read means nothing now.
        if (row < 0 || change.rewritten(page.key())) {
            continue;
        }
        QVector<PageObject> here = page.value();
        const QTransform turn = pageTurn(change.turnOf(page.key()), m_rows.sizeOf(row));
        if (!turn.isIdentity()) {
            for (PageObject &object : here) {
                object.bounds = turn.mapRect(object.bounds);
                object.outline = turn.map(object.outline);
                object.placement = object.placement * turn;
            }
        }
        objects.insert(row, here);
    }

    for (auto page = m_pending.constBegin(); page != m_pending.constEnd(); ++page) {
        if (page->isEmpty()) {
            continue;
        }
        const int row = change.nowAt(page.key());
        if (row < 0) {
            lost += int(page->size());
            continue;
        }
        QHash<int, PageEdit> here = page.value();
        for (PageEdit &edit : here) {
            edit.page = row;
        }
        pending.insert(row, here);
    }

    QVector<Insertion> insertions;
    insertions.reserve(m_insertions.size());
    for (Insertion item : m_insertions) {
        const int was = item.content.page;
        const int row = change.nowAt(was);
        if (row < 0) {
            ++lost;
            continue;
        }

        // A copy is measured against the sheet as it was drawn when it landed,
        // so a sheet the organiser has since turned has to take it round. A
        // page pointed at a freshly written file is not the same problem: what
        // was pasted is not in that file either, and goes on waiting.
        const QTransform turn = pageTurn(change.turnOf(was), m_rows.sizeOf(row));
        if (!turn.isIdentity()) {
            // The drag made on it is in the coordinates the sheet had then, so
            // it is folded into the copy before the sheet goes round and the
            // waiting change is dropped rather than read in the new ones. The
            // sheet's turn goes in the same way a turn of the copy's own would:
            // what was pasted onto the paper goes round with the paper.
            item.content = contentUnder(insertionAsWritten(item), turn);
            const auto here = pending.find(row);
            if (here != pending.end() && here->value(item.handle).kind != PageEdit::Kind::Delete) {
                here->remove(item.handle);
            }
        }
        item.content.page = row;
        insertions.append(item);
    }

    const bool had = hasPendingEdits() || hasPendingInsertions();
    m_objects = objects;
    // The lift was taken out of the file the rows pointed at a moment ago, so
    // it is pixels of a page that may not even be in the document any more.
    dropLift();
    m_pending = pending;
    m_insertions = insertions;

    // The choice is a row and a set of object numbers on it. The numbers are
    // still that page's own, so the choice travels with the page and is only
    // given up when the page itself is gone.
    const int chosenNow = m_page >= 0 ? change.nowAt(m_page) : -1;
    if (m_page >= 0 && chosenNow < 0) {
        clearChoice();
    } else {
        m_page = chosenNow;
    }
    m_hoverPage = m_hoverPage >= 0 ? change.nowAt(m_hoverPage) : -1;
    if (m_hoverPage < 0) {
        m_hover = -1;
    }
    m_drag = Drag::None;
    m_grip = Grip::None;
    m_dragRow = m_dragRow >= 0 ? change.nowAt(m_dragRow) : -1;

    if (lost > 0) {
        Q_EMIT refused(i18ncp("@info %1 is a number of waiting changes to a page",
                              "One change was waiting on a page that has been taken out of the document, so it has "
                              "been dropped.",
                              "%1 changes were waiting on pages that have been taken out of the document, so they "
                              "have been dropped.",
                              lost));
    }
    if (had != (hasPendingEdits() || hasPendingInsertions())) {
        Q_EMIT editsChanged();
    }
    refreshDetails();
    repaint();
}

void ObjectOverlay::setSource(const QString &pdfPath)
{
    if (m_source == pdfPath) {
        return;
    }
    m_source = pdfPath;

    // An object number counts painting operations in one particular file, so
    // nothing read from the old one, and nothing waiting against it, means
    // anything now.
    m_objects.clear();
    dropLift();
    const bool had = hasPendingEdits() || hasPendingInsertions();
    m_pending.clear();

    // A pasted copy names its page by row, and a new file behind those rows is
    // a different page under every one of them.
    m_insertions.clear();
    m_pasteWalk = 0;
    clearChoice();
    if (had) {
        Q_EMIT editsChanged();
    }
    repaint();
}

void ObjectOverlay::reread()
{
    m_objects.clear();

    // The page behind the row has been written again, so both halves of the
    // lift are pictures of a file that no longer says what they say.
    dropLift();
    reviewLift();
    repaint();
}

bool ObjectOverlay::appliesTo(PageView::Mode mode) const
{
    return mode == PageView::Mode::Edit;
}

// ── What is on the page ───────────────────────────────────────────────────

const QVector<PageObject> &ObjectOverlay::readOf(int row)
{
    const auto cached = m_objects.constFind(row);
    if (cached != m_objects.constEnd()) {
        return cached.value();
    }
    if (row < 0) {
        return *m_objects.insert(row, {});
    }

    // Which file, and which page inside it. A row is neither of those: on a
    // document merged from two files, row three is page three of one file only
    // by accident, and after a page is deleted not even that.
    const SourcePage where = m_rows.pageOf(row);
    const QString file = where.isValid() ? m_rows.fileOf(where) : m_source;
    const int page = where.isValid() ? where.pageInFile() : row;
    if (file.isEmpty()) {
        return *m_objects.insert(row, {});
    }

    QString error;
    QVector<PageObject> found = PageObjects::read(file, page, &error);

    // Read as the file has the page, drawn as the organiser has since turned it.
    const QTransform turn = pageTurn(m_rows.rotationOf(row), m_rows.sizeOf(row));
    if (!turn.isIdentity()) {
        for (PageObject &object : found) {
            object.bounds = turn.mapRect(object.bounds);
            object.outline = turn.map(object.outline);
            object.placement = object.placement * turn;
        }
    }

    if (!error.isEmpty()) {
        // The first read of a page happens inside paint(); anything the window
        // puts on screen from there would be painted over by the paint that
        // asked for it.
        QMetaObject::invokeMethod(this, [this, error] { Q_EMIT refused(error); }, Qt::QueuedConnection);
    }
    return *m_objects.insert(row, found);
}

PageObject ObjectOverlay::objectOf(const Insertion &insertion, int layer)
{
    const NewContent &content = insertion.content;

    // Its own box is where the content is laid out; where that box has been put
    // is what the placement says. The boundary is round the two together, which
    // is why turning something pasted moves its handles with it.
    const QRectF box = content.rect.normalized();

    PageObject object;
    object.index = insertion.handle;
    object.page = content.page;
    object.bounds = whereItStands(content);
    object.layer = layer;

    // The matrix that would have to be reversed to move it, which is the same
    // matrix the composer will write. A turn is always reversible, so something
    // pasted can always be moved, unlike an object flattened into a page.
    object.placement = content.placement;
    object.resourceName = pastedName(content.kind);

    switch (content.kind) {
    case NewContent::Kind::Text:
        object.kind = PageObject::Kind::Text;
        object.text = content.text;
        object.fontResource = content.fontFamily;
        object.fontSize = content.fontSize;
        object.fill = content.fill;
        break;

    case NewContent::Kind::Image:
        object.kind = PageObject::Kind::Image;
        object.pixelSize = content.image.size();
        break;

    case NewContent::Kind::Rectangle:
    case NewContent::Kind::Ellipse:
    case NewContent::Kind::Line:
        object.kind = PageObject::Kind::Path;
        object.fill = content.fill;
        object.stroke = content.stroke;
        object.lineWidth = content.lineWidth;
        // The shape rather than its box, because that is what a hit test takes a
        // Path by: a pasted diagonal rule is caught along the rule instead of
        // anywhere inside the rectangle that happens to contain it. Drawn in the
        // box and then put through the placement, so a turned rule is caught
        // along where it now lies rather than where it was pasted.
        if (content.kind == NewContent::Kind::Ellipse) {
            object.outline.addEllipse(box);
        } else if (content.kind == NewContent::Kind::Line) {
            // The same diagonal the composer draws: from the box's left edge at
            // its greater y across to its right edge at its lesser one.
            object.outline.moveTo(box.left(), box.bottom());
            object.outline.lineTo(box.right(), box.top());
        } else {
            object.outline.addRect(box);
        }
        object.outline = content.placement.map(object.outline);
        break;
    }
    return object;
}

QVector<PageObject> ObjectOverlay::objectsOf(int row)
{
    QVector<PageObject> objects = readOf(row);
    if (m_insertions.isEmpty()) {
        return objects;
    }

    // After the page's own operators, because that is where the composer puts
    // them: what was pasted lies on top of what was already there, and both the
    // hit test and the painter take the last thing in the list to be the topmost.
    int layer = int(objects.size());
    for (const Insertion &insertion : m_insertions) {
        if (insertion.content.page != row) {
            continue;
        }
        // Deleting something pasted takes it off the page outright rather than
        // leaving the crossed-out ghost a deleted operator leaves. There is no
        // operator here to cross out: it was never written, and a mark that no
        // save could ever clear would sit on the page for good. The change is
        // still only waiting, so takeBack() brings it back.
        if (waitingOn(row, insertion.handle).kind == PageEdit::Kind::Delete) {
            continue;
        }
        objects.append(objectOf(insertion, layer++));
    }
    return objects;
}

const ObjectOverlay::Insertion *ObjectOverlay::insertionFor(int handle) const
{
    for (const Insertion &insertion : m_insertions) {
        if (insertion.handle == handle) {
            return &insertion;
        }
    }
    return nullptr;
}

PageEdit ObjectOverlay::waitingOn(int page, int object) const
{
    const auto here = m_pending.constFind(page);
    return here == m_pending.constEnd() ? PageEdit() : here->value(object);
}

NewContent ObjectOverlay::insertionAsWritten(const Insertion &insertion) const
{
    NewContent content = insertion.content;
    const PageEdit edit = waitingOn(content.page, insertion.handle);

    if (edit.kind == PageEdit::Kind::Transform) {
        content = contentUnder(content, edit.transform);
    } else if (edit.kind == PageEdit::Kind::Restyle) {
        if (edit.fill.isValid()) {
            content.fill = edit.fill;
        }
        if (edit.stroke.isValid()) {
            content.stroke = edit.stroke;
        }
        if (edit.lineWidth >= 0.0) {
            content.lineWidth = edit.lineWidth;
        }
    }
    return content;
}

bool ObjectOverlay::choiceHoldsPaste() const
{
    for (const int index : std::as_const(m_chosen)) {
        if (isPasted(index)) {
            return true;
        }
    }
    return false;
}

QVector<PageObject> ObjectOverlay::shownOf(int row)
{
    const QVector<PageObject> objects = objectsOf(row);
    const QHash<int, PageEdit> here = m_pending.value(row);
    if (here.isEmpty()) {
        return objects;
    }

    QVector<PageObject> shown = objects;
    for (PageObject &object : shown) {
        const auto waiting = here.constFind(object.index);
        if (waiting != here.constEnd() && waiting->kind == PageEdit::Kind::Transform) {
            object.bounds = waiting->transform.mapRect(object.bounds);
            object.outline = waiting->transform.map(object.outline);
        }
    }
    return shown;
}

PageObject ObjectOverlay::shownObject(int row, int index)
{
    const QVector<PageObject> objects = shownOf(row);
    for (const PageObject &object : objects) {
        if (object.index == index) {
            return object;
        }
    }
    return {};
}

QRectF ObjectOverlay::choiceBounds()
{
    QRectF box;
    if (m_page < 0 || m_chosen.isEmpty()) {
        return box;
    }
    const QVector<PageObject> objects = shownOf(m_page);
    for (const PageObject &object : objects) {
        if (!m_chosen.contains(object.index) || object.bounds.isEmpty()) {
            continue;
        }
        const QRectF bounds = object.bounds.normalized();
        box = box.isNull() ? bounds : box.united(bounds);
    }
    return box;
}

// ── Drawing ───────────────────────────────────────────────────────────────

QPointF ObjectOverlay::gripCentre(const QRectF &box, Grip grip)
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
    case Grip::Turn:
        return { midX, box.top() - TurnArm };
    case Grip::None:
        break;
    }
    return {};
}

Qt::CursorShape ObjectOverlay::cursorForGrip(Grip grip)
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
    case Grip::Turn:
        return Qt::CrossCursor; // Qt has no pointer that says "turn"; the cross at least says "not a move".
    case Grip::None:
        break;
    }
    return Qt::ArrowCursor;
}

void ObjectOverlay::paint(QPainter &painter, int row, const QRect &pageRect)
{
    Q_UNUSED(pageRect)
    if (!m_view) {
        return;
    }

    // The painter arrives in the viewport's own coordinates, which is what
    // fromPoints() answers in, so nothing here has to find the page's corner.
    const QHash<int, PageEdit> here = m_pending.value(row);
    const bool ours = row == m_page;
    const QTransform live = ours ? liveTransform() : QTransform();
    const QTransform pixels = toPixels(row);

    // The page as the file draws it, with what is waiting on each object folded
    // in below one at a time. Not folded in here, because the box round a turned
    // object is not the turned object: a boundary has to be drawn as four
    // corners that went round, and only the matrix knows where they went.
    const QVector<PageObject> objects = objectsOf(row);

    painter.setRenderHint(QPainter::Antialiasing, true);
    const QColor highlight = m_view->palette().color(QPalette::Highlight);

    // Under the boundaries, so that the wash and the handles are on top of what
    // they belong to.
    paintInsertions(painter, row, objects, live);
    paintLift(painter, row, live);

    for (const PageObject &object : objects) {
        QRectF bounds = object.bounds.normalized();
        if (bounds.isEmpty()) {
            continue;
        }
        const bool chosen = ours && m_chosen.contains(object.index);

        // Never so thin that it disappears: a hairline rule is a real object and
        // has to be visible as one before anyone can click it. Widened in points
        // rather than in pixels, since it is about to go through a matrix.
        const double thin = 3.0 / std::max(0.05, m_view->zoom());
        if (bounds.width() < thin) {
            bounds.adjust(-thin / 2.0, 0, thin / 2.0, 0);
        }
        if (bounds.height() < thin) {
            bounds.adjust(0, -thin / 2.0, 0, thin / 2.0);
        }
        const QPolygonF frame = pixels.map(standingOn(row, object.index, live).map(cornersOf(bounds)));

        const auto waiting = here.constFind(object.index);
        const bool doomed = waiting != here.constEnd() && waiting->kind == PageEdit::Kind::Delete;
        const bool hovered = row == m_hoverPage && object.index == m_hover;

        // Dashed says "waiting", which is true of everything pasted as well as
        // of everything marked for removal: neither is in the file yet.
        const Qt::PenStyle line = doomed || isPasted(object.index) ? Qt::DashLine : Qt::SolidLine;

        QColor tint = doomed ? QColor(200, 60, 60) : tintOf(object.kind);
        painter.setPen(Qt::NoPen);
        if (chosen) {
            QColor wash = highlight;
            wash.setAlpha(45);
            painter.setBrush(wash);
            painter.drawPolygon(frame);
            painter.setPen(QPen(highlight, 2.0, line));
        } else if (hovered) {
            QColor wash = tint;
            wash.setAlpha(28);
            painter.setBrush(wash);
            painter.drawPolygon(frame);
            painter.setPen(QPen(tint, 1.5, line));
        } else {
            // Faint until it is pointed at: a solid boundary round every drawing
            // operation would bury a busy page under its own scaffolding.
            tint.setAlpha(doomed ? 170 : 70);
            painter.setPen(QPen(tint, 1.0, line));
        }

        painter.setBrush(Qt::NoBrush);
        painter.drawPolygon(frame);
        if (doomed) {
            painter.drawLine(frame.at(0), frame.at(2));
            painter.drawLine(frame.at(1), frame.at(3));
        }
    }

    // ── the handles ───────────────────────────────────────────────────────
    if (ours && !m_chosen.isEmpty()) {
        const QRectF bounds = live.mapRect(choiceBounds());
        if (!bounds.isNull()) {
            const QRectF box = m_view->fromPoints(row, bounds);
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(highlight, 1.0, Qt::DashLine));
            painter.drawRect(box);
            painter.drawLine(QPointF(box.center().x(), box.top()), gripCentre(box, Grip::Turn));

            painter.setPen(QPen(highlight.darker(150), 1.0));
            painter.setBrush(m_view->palette().color(QPalette::Base));
            for (int grip = int(Grip::TopLeft); grip <= int(Grip::Turn); ++grip) {
                const QPointF centre = gripCentre(box, static_cast<Grip>(grip));
                if (static_cast<Grip>(grip) == Grip::Turn) {
                    painter.drawEllipse(centre, HandleSize / 2.0, HandleSize / 2.0);
                } else {
                    painter.drawRect(
                        QRectF(centre.x() - HandleSize / 2.0, centre.y() - HandleSize / 2.0, HandleSize, HandleSize));
                }
            }
        }
    }

    // ── the band ──────────────────────────────────────────────────────────
    if (m_drag == Drag::Band && row == m_dragRow) {
        QColor wash = highlight;
        wash.setAlpha(35);
        painter.setPen(QPen(highlight, 1.0, Qt::DashLine));
        painter.setBrush(wash);
        painter.drawRect(m_view->fromPoints(row, QRectF(m_from, m_to).normalized()));
    }
}

QTransform ObjectOverlay::standingOn(int row, int index, const QTransform &live) const
{
    const PageEdit edit = waitingOn(row, index);
    const QTransform waiting = edit.kind == PageEdit::Kind::Transform ? edit.transform : QTransform();

    // The drag in progress is on top of whatever was already waiting, and only
    // on what is chosen: it is the gesture being made to those, not a state the
    // page is in.
    return row == m_page && m_chosen.contains(index) ? waiting * live : waiting;
}

// ── The page taken apart, so a gesture is a blit ───────────────────────────

void ObjectOverlay::paintLift(QPainter &painter, int row, const QTransform &live)
{
    if (!m_view || m_lift.row != row || m_lift.patch.isNull()) {
        return;
    }

    // Nothing at all while everything still stands where the file draws it. The
    // sheet underneath is the truth in that case, and putting a copy of it back
    // over itself can only ever be a way of getting it slightly wrong.
    bool stirred = m_lift.corrects;
    for (const Lift::Ink &ink : std::as_const(m_lift.inks)) {
        for (const int object : ink.objects) {
            stirred = stirred || waitingOn(row, object).kind == PageEdit::Kind::Delete
                || !standingOn(row, object, live).isIdentity();
        }
    }
    if (!stirred) {
        return;
    }

    // The hole first and every object afterwards, or the second object's hole
    // would be punched through the first object's new place. The page itself
    // rather than paper white: what was drawn behind the object comes back,
    // which is the whole reason this is a render and not a fill.
    painter.drawImage(m_view->fromPoints(row, m_lift.where), m_lift.patch);

    const QTransform pixels = toPixels(row);
    bool invertible = false;
    const QTransform back = pixels.inverted(&invertible);
    for (const Lift::Ink &ink : std::as_const(m_lift.inks)) {
        if (ink.objects.isEmpty() || waitingOn(row, ink.objects.constFirst()).kind == PageEdit::Kind::Delete) {
            continue; // Taken off the page: the hole is covered and nothing goes back.
        }

        // Every object under one ink shares its waiting change, which is what
        // let them share a picture in the first place, so any of them answers
        // for all of them.
        //
        // The gesture in progress is a different question, and one picture
        // cannot answer it two ways: where some of a group has since been let go
        // of, the drag is left off all of it, so the boundaries move on their own
        // until the gesture settles and the group is split properly. It takes a
        // deliberate Ctrl-click to get into that state.
        bool everyone = row == m_page;
        for (const int object : ink.objects) {
            everyone = everyone && m_chosen.contains(object);
        }
        const PageEdit waiting = waitingOn(row, ink.objects.constFirst());
        const QTransform stands = waiting.kind == PageEdit::Kind::Transform ? waiting.transform : QTransform();

        // Whatever is already in the pixels comes off first, so what is left is
        // the movement since, which is usually nothing.
        bool undone = false;
        const QTransform undo = ink.baked.inverted(&undone);
        const QTransform total = (undone ? undo : QTransform()) * (everyone ? stands * live : stands);
        painter.save();
        if (invertible && !total.isIdentity()) {
            // The same conjugation the composer does to reach page space: the
            // ink is drawn in the box the file gives it and the matrix waiting
            // on it carries the box wherever it has gone.
            painter.setTransform(back * total * pixels, true);
        }
        painter.drawImage(m_view->fromPoints(row, ink.where), ink.pixels);
        painter.restore();
    }
}

int ObjectOverlay::liftWidthOf(int row) const
{
    if (!m_view || row < 0) {
        return 0;
    }
    const QRect where = m_view->pageRect(row);
    return where.isEmpty() ? 0 : int(std::lround(where.width() * m_view->devicePixelRatioF()));
}

QRect ObjectOverlay::tileFor(int row, const QRectF &wanted) const
{
    if (!m_view) {
        return {};
    }
    const QRect where = m_view->pageRect(row);
    const double ratio = m_view->devicePixelRatioF();
    if (where.isEmpty() || ratio <= 0.0) {
        return {};
    }

    // Asked of the view rather than worked out from the zoom, because there is
    // one conversion between points and pixels in this program. What comes back
    // is in the viewport's coordinates, so the page's own corner comes off it,
    // and then it is in the pixels the rasteriser counts in.
    const QRectF box = m_view->fromPoints(row, wanted).translated(-where.topLeft());
    const QRectF device(box.x() * ratio, box.y() * ratio, box.width() * ratio, box.height() * ratio);
    const int height = int(std::lround(where.height() * ratio));
    return device.toAlignedRect().intersected(QRect(0, 0, liftWidthOf(row), std::max(1, height)));
}

ObjectOverlay::LiftAsk ObjectOverlay::liftWanted() const
{
    LiftAsk ask;

    // The row the choice is on, and where there is no choice the row the pixels
    // in hand are of. That second half is what lets something dragged and then
    // let go of stay where it was put: the sheet underneath still draws it in
    // the old place, and only this puts it right.
    const int row = m_page >= 0 ? m_page : m_lift.row;
    if (!m_view || row < 0) {
        return ask;
    }

    // Chosen, or carrying a waiting change, and drawn by the file either way.
    // Something pasted is not in the sheet underneath at all: there is no hole
    // to cover and no ink to lift, and its pixels are already in hand, which is
    // what paintInsertions draws.
    const QHash<int, PageEdit> here = m_pending.value(row);
    QSet<int> wanted;
    if (row == m_page) {
        for (const int index : m_chosen) {
            wanted.insert(index);
        }
    }
    for (auto edit = here.constBegin(); edit != here.constEnd(); ++edit) {
        wanted.insert(edit.key());
    }

    QVector<int> objects;
    for (const int index : std::as_const(wanted)) {
        if (!isPasted(index)) {
            objects.append(index);
        }
    }
    if (objects.isEmpty()) {
        return ask;
    }
    std::sort(objects.begin(), objects.end());

    ask.row = row;
    ask.widthPx = liftWidthOf(row);
    ask.objects = objects;
    ask.groups.reserve(objects.size());

    // Grouped by the waiting change alone, and not by whether the object is
    // chosen: letting go of one of two objects dragged together would otherwise
    // split a group that is still perfectly well carried by one picture, and
    // splitting a group throws the pixels away.
    for (int at = 0; at < objects.size(); ++at) {
        const PageEdit edit = waitingOn(row, objects.at(at));
        int with = at;
        for (int earlier = 0; earlier < at; ++earlier) {
            if (sameEdit(waitingOn(row, objects.at(earlier)), edit)) {
                with = ask.groups.at(earlier);
                break;
            }
        }
        ask.groups.append(with);
        if (with == at) {
            ask.looks.append(edit);
            ask.shapes.append(edit.kind == PageEdit::Kind::Transform ? shapeOf(edit.transform) : QTransform());
        }
    }
    return ask;
}

bool ObjectOverlay::isLifted(int row) const
{
    return m_lift.row == row && !m_lift.patch.isNull() && !m_lift.inks.isEmpty();
}

bool ObjectOverlay::isLifting() const
{
    return m_liftSettle.isActive() || m_liftBusy;
}

void ObjectOverlay::dropLift()
{
    m_liftSettle.stop();
    m_liftAsk = LiftAsk();
    if (m_lift.row < 0) {
        return;
    }
    m_lift = Lift();
    repaint();
}

bool ObjectOverlay::liftHolds() const
{
    for (const Lift::Ink &ink : std::as_const(m_lift.inks)) {
        if (ink.objects.isEmpty()) {
            return false;
        }
        const PageEdit head = waitingOn(m_lift.row, ink.objects.constFirst());

        // Taken off the page is not a difference: the hole is cut and the ink is
        // simply not put back. Recoloured is, because the pixels are the colour
        // the object used to be.
        if (head.kind != PageEdit::Kind::Delete && !sameLook(head, ink.under)) {
            return false;
        }
        for (const int object : ink.objects) {
            if (!sameEdit(waitingOn(m_lift.row, object), head)) {
                return false;
            }
        }
    }
    return true;
}

void ObjectOverlay::reviewLift()
{
    if (!m_view) {
        return;
    }
    const LiftAsk want = liftWanted();
    if (want.row < 0) {
        dropLift();
        return;
    }

    // Thrown away only where it has become a lie. A better one being available
    // is not a reason: the pixels would be off the page for as long as the new
    // ones took to arrive, and the reader would watch the object jump back to
    // where the file still draws it and then forward again.
    if (m_lift.row >= 0 && (m_lift.row != want.row || !liftHolds())) {
        m_lift = Lift();
        repaint();
    }

    const bool asked = want.row == m_liftAsk.row && want.widthPx == m_liftAsk.widthPx
        && want.objects == m_liftAsk.objects && want.groups == m_liftAsk.groups && want.shapes == m_liftAsk.shapes
        && want.looks.size() == m_liftAsk.looks.size()
        && std::equal(want.looks.cbegin(), want.looks.cend(), m_liftAsk.looks.cbegin(), stillInk);
    if (asked) {
        return;
    }
    m_liftAsk = want;
    m_liftSettle.start(LiftSettleMs, this);
}

void ObjectOverlay::timerEvent(QTimerEvent *event)
{
    if (event->timerId() == m_liftSettle.timerId()) {
        m_liftSettle.stop();
        startLift();
        return;
    }
    QObject::timerEvent(event);
}

void ObjectOverlay::startLift()
{
    const LiftAsk ask = m_liftAsk;
    if (!m_view || ask.row < 0 || ask.widthPx <= 0 || !m_scratch.isValid()) {
        return;
    }

    LiftJob job;
    job.row = ask.row;
    job.widthPx = ask.widthPx;
    job.scratch = m_scratch.path();
    job.run = ++m_liftRun;

    // The run before last is finished with. Not the one before this: a choice
    // changed twice in quick succession leaves the first run still reading its
    // own copies, and taking a file out from under a rasteriser is a render that
    // fails for no reason anybody could act on.
    if (job.run >= 2) {
        for (const QString &stale : QDir(job.scratch).entryList({ u"lift-%1-*.pdf"_s.arg(job.run - 2) }, QDir::Files)) {
            QFile::remove(m_scratch.filePath(stale));
        }
    }

    // The page on its own, which is a few kilobytes against the document's
    // several megabytes and takes about a millisecond to write. Without a
    // document to ask there is no way to cut one page out, so the file the
    // source names is composed whole instead; that path is only ever taken by a
    // caller that set a file and no document.
    const SourcePage where = m_rows.pageOf(ask.row);
    if (m_document) {
        job.file = m_scratch.filePath(u"lift-%1-page.pdf"_s.arg(job.run));
        job.page = 0;
        if (!DocumentWriter::writeSelection(*m_document, { ask.row }, job.file, {}, nullptr)) {
            return;
        }
    } else {
        job.file = where.isValid() ? m_rows.fileOf(where) : m_source;
        job.page = where.isValid() ? where.pageInFile() : ask.row;
    }
    if (job.file.isEmpty()) {
        return;
    }

    const QVector<PageObject> onPage = readOf(ask.row);
    QHash<int, QRectF> bounds;
    for (const PageObject &object : onPage) {
        bounds.insert(object.index, object.bounds.normalized());
    }

    // A group is lifted when anything in it is chosen, because that is what is
    // about to be dragged. Everything else waiting on the page goes into the
    // patch instead, at the place it now stands: one render puts any number of
    // them right, and they are not moving, so ink of their own would be a
    // picture nobody was going to carry anywhere.
    // LiftAsk::groups names a group by the place of its first object; the looks
    // are in the order those first appear, so this is the one conversion between
    // the two, done once.
    QHash<int, int> ordinalOf;
    for (int at = 0; at < ask.objects.size(); ++at) {
        if (ask.groups.at(at) == at) {
            ordinalOf.insert(at, int(ordinalOf.size()));
        }
    }

    QVector<bool> lifting(ask.looks.size(), false);
    for (int at = 0; at < ask.objects.size(); ++at) {
        if (m_chosen.contains(ask.objects.at(at))) {
            lifting[ordinalOf.value(ask.groups.at(at))] = true;
        }
    }

    QRectF hole;
    for (int at = 0; at < ask.objects.size(); ++at) {
        const int index = ask.objects.at(at);
        const QRectF box = bounds.value(index);
        const int group = ordinalOf.value(ask.groups.at(at));

        if (lifting.at(group)) {
            // Cut out of the page, so that the ink can be put back wherever the
            // gesture has carried it.
            PageEdit gone;
            gone.kind = PageEdit::Kind::Delete;
            gone.page = job.page;
            gone.object = index;
            job.hide.append(gone);
            if (!box.isEmpty()) {
                hole = hole.isNull() ? box : hole.united(box);
            }
            continue;
        }

        // Written into the page as it will be saved. The patch has to reach both
        // where the file draws it and where it now stands, or the sheet
        // underneath goes on showing it in the old place beside the new one.
        PageEdit put = ask.looks.at(group);
        put.page = job.page;
        put.object = index;
        job.hide.append(put);
        job.corrects = true;
        if (box.isEmpty()) {
            continue;
        }
        const QRectF now = put.kind == PageEdit::Kind::Transform ? put.transform.mapRect(box) : box;
        hole = hole.isNull() ? box.united(now) : hole.united(box).united(now);
    }
    if (hole.isNull() || job.hide.isEmpty()) {
        return;
    }
    hole = hole.marginsAdded(QMarginsF(LiftMarginPoints, LiftMarginPoints, LiftMarginPoints, LiftMarginPoints));
    job.patchTile = tileFor(ask.row, hole);
    if (job.patchTile.isEmpty()) {
        return;
    }

    for (int at = 0; at < ask.objects.size(); ++at) {
        if (ask.groups.at(at) != at) {
            continue; // Not the first of its group; it is added to that one below.
        }
        const int group = ordinalOf.value(at);
        const PageEdit &look = ask.looks.at(group);
        if (!lifting.at(group) || look.kind == PageEdit::Kind::Delete) {
            continue; // Left in the patch, or taken off the page altogether.
        }

        LiftJob::Group made;
        QRectF box;
        for (int also = at; also < ask.objects.size(); ++also) {
            if (ask.groups.at(also) != at) {
                continue;
            }
            const int index = ask.objects.at(also);
            made.objects.append(index);
            const QRectF one = bounds.value(index);
            if (!one.isEmpty()) {
                box = box.isNull() ? one : box.united(one);
            }
        }
        if (box.isNull()) {
            continue;
        }

        // A turn or a stretch goes into the page the ink is drawn from, so that
        // the settled object is drawn at its own size and angle rather than
        // being a bitmap put through them. Read here rather than taken off the
        // ask, which deliberately forgets the movement and so is a gesture or
        // two out of date by now.
        const PageEdit standing = waitingOn(ask.row, made.objects.constFirst());
        if (standing.kind == PageEdit::Kind::Transform && !shapeOf(standing.transform).isIdentity()
            && standing.transform.isInvertible()) {
            made.bake = standing.transform;
            box = made.bake.mapRect(box);
        }
        box = box.marginsAdded(QMarginsF(LiftMarginPoints, LiftMarginPoints, LiftMarginPoints, LiftMarginPoints));
        made.tile = tileFor(ask.row, box);
        if (made.tile.isEmpty()) {
            continue;
        }

        // Everything the group does not hold, left off the page. What is left is
        // the group as the page draws it, clipping, blending and all: only the
        // painting operators go, so every piece of state that shaped them stays.
        const QSet<int> kept(made.objects.constBegin(), made.objects.constEnd());
        for (const PageObject &object : onPage) {
            if (kept.contains(object.index)) {
                continue;
            }
            PageEdit gone;
            gone.kind = PageEdit::Kind::Delete;
            gone.page = job.page;
            gone.object = object.index;
            made.alone.append(gone);
        }

        // A recolouring is baked in, because it changes what the ink looks like,
        // and so is a turn or a stretch. A plain move is not: it only says where
        // the ink goes, and where it goes is read afresh on every frame.
        //
        // Never both at once, and that is the overlay's own rule rather than
        // luck: one waiting change per object, so a recolouring takes the place
        // of a move and says so.
        if (look.kind == PageEdit::Kind::Restyle || !made.bake.isIdentity()) {
            for (const int index : std::as_const(made.objects)) {
                PageEdit put = look.kind == PageEdit::Kind::Restyle ? look : PageEdit();
                put.kind = look.kind == PageEdit::Kind::Restyle ? PageEdit::Kind::Restyle : PageEdit::Kind::Transform;
                put.transform = made.bake;
                put.page = job.page;
                put.object = index;
                made.alone.append(put);
            }
        }
        made.under = look;
        job.groups.append(made);
    }
    if (job.groups.isEmpty() && !job.corrects) {
        return;
    }

    // Under the ceiling, by rendering smaller and stretching. Only ever reached
    // by choosing most of a page at a large zoom, which is where a bitmap is
    // going to be soft anyway.
    qint64 pixels = qint64(job.patchTile.width()) * job.patchTile.height();
    for (const LiftJob::Group &group : std::as_const(job.groups)) {
        pixels += qint64(group.tile.width()) * group.tile.height();
    }
    if (pixels * LiftBytesPerPixel > LiftBudgetBytes) {
        const double wanted = std::sqrt(double(LiftBudgetBytes) / double(pixels * LiftBytesPerPixel));
        job.widthPx = std::max(1, int(std::lround(job.widthPx * wanted)));

        // Taken from the widths rather than from @p wanted, which rounding has
        // just moved away from: every tile has to be measured against the page
        // the renders are actually made at, or the pictures land a pixel off.
        const double shrink = double(job.widthPx) / double(ask.widthPx);
        const auto smaller = [shrink](const QRect &tile) {
            return QRectF(tile.x() * shrink, tile.y() * shrink, std::max(1.0, tile.width() * shrink),
                          std::max(1.0, tile.height() * shrink))
                .toAlignedRect();
        };
        job.patchTile = smaller(job.patchTile);
        for (LiftJob::Group &group : job.groups) {
            group.tile = smaller(group.tile);
        }
    }

    // Where each picture belongs, taken back out of the tile rather than out of
    // the rectangle it was made from: a tile is whole pixels, and a picture drawn
    // over the box it was rounded up from would sit a fraction of a pixel off,
    // which along the edge of a patch is a visible seam.
    const double ratio = m_view->devicePixelRatioF() * double(job.widthPx) / double(ask.widthPx);
    const QRect page = m_view->pageRect(ask.row);
    const auto inPoints = [&](const QRect &tile) {
        const auto onScreen = [&](double x, double y) {
            return m_view->toPoints(ask.row, QPointF(x / ratio + page.x(), y / ratio + page.y()));
        };
        return QRectF(onScreen(tile.x(), tile.y()), onScreen(tile.right() + 1.0, tile.bottom() + 1.0)).normalized();
    };
    job.patchWhere = inPoints(job.patchTile);
    for (LiftJob::Group &group : job.groups) {
        group.where = inPoints(group.tile);
    }

    if (!m_liftWatcher) {
        m_liftWatcher = new QFutureWatcher<Lift>(this);
        connect(m_liftWatcher, &QFutureWatcher<Lift>::finished, this, &ObjectOverlay::liftArrived);
    }

    // Setting a new future lets go of the one before it, so a preparation
    // started for a choice that has since changed finishes into nothing. That
    // costs less than making the thread that has to keep drawing wait for it.
    m_liftBusy = true;
    m_liftWatcher->setFuture(QtConcurrent::run(&ObjectOverlay::liftFrom, job));
}

ObjectOverlay::Lift ObjectOverlay::liftFrom(const LiftJob &job)
{
    const QDir scratch(job.scratch);
    const auto path = [&](const QString &what) { return scratch.filePath(u"lift-%1-%2.pdf"_s.arg(job.run).arg(what)); };

    PageComposer::Report report;
    PageComposer::Options plain;
    const QString without = path(u"without"_s);
    if (!PageComposer::apply(job.file, without, job.hide, {}, plain, &report, nullptr)) {
        return {};
    }

    // The rasteriser is opened and let go inside the worker, so nothing here
    // shares a handle with the renders the view has in flight.
    PopplerBackend backend;
    if (!backend.addDocument(1, without, nullptr)) {
        return {};
    }

    RenderBackend::Request ask;
    ask.widthPx = job.widthPx;
    ask.tile = job.patchTile;

    Lift lift;
    lift.patch = backend.render(1, job.page, ask);
    if (lift.patch.isNull()) {
        return {};
    }
    lift.row = job.row;
    lift.run = job.run;
    lift.where = job.patchWhere;
    lift.corrects = job.corrects;

    PageComposer::Options overWhite;
    overWhite.backdrop = QColor(Qt::white);
    overWhite.annotations = false;
    PageComposer::Options overBlack = overWhite;
    overBlack.backdrop = QColor(Qt::black);

    for (int at = 0; at < job.groups.size(); ++at) {
        const LiftJob::Group &group = job.groups.at(at);
        const QString pale = path(u"%1-white"_s.arg(at));
        const QString dark = path(u"%1-black"_s.arg(at));
        if (!PageComposer::apply(job.file, pale, group.alone, {}, overWhite, &report, nullptr)
            || !PageComposer::apply(job.file, dark, group.alone, {}, overBlack, &report, nullptr)) {
            continue;
        }

        // A handle of its own per file, given out whether or not the file opens,
        // so that a group that failed cannot leave its number pointing at
        // somebody else's page.
        const int paleHandle = 2 + at * 2;
        const int darkHandle = paleHandle + 1;
        if (!backend.addDocument(paleHandle, pale, nullptr) || !backend.addDocument(darkHandle, dark, nullptr)) {
            continue;
        }

        ask.tile = group.tile;
        Lift::Ink ink;
        ink.objects = group.objects;
        ink.where = group.where;
        ink.baked = group.bake;
        ink.under = group.under;
        ink.pixels = inkBetween(backend.render(paleHandle, job.page, ask), backend.render(darkHandle, job.page, ask));
        if (!ink.pixels.isNull()) {
            lift.inks.append(ink);
        }
    }
    // A lift that holds nothing but the patch is still worth having: it is the
    // page put right where something was moved and then let go of.
    return lift.inks.isEmpty() && !lift.corrects ? Lift() : lift;
}

void ObjectOverlay::liftArrived()
{
    m_liftBusy = false;
    if (!m_liftWatcher) {
        return;
    }
    const Lift made = m_liftWatcher->result();

    // The choice may have moved on while this was being drawn. Answering with
    // pixels of what used to be chosen would put ink on the page where the file
    // has none.
    if (made.row < 0 || made.run != m_liftRun || made.row != m_liftAsk.row) {
        return;
    }
    m_lift = made;
    repaint();
}

QTransform ObjectOverlay::toPixels(int row) const
{
    if (!m_view) {
        return {};
    }
    // Asked of the view rather than rebuilt from the zoom and the page's corner:
    // there is one conversion between points and pixels in this program and a
    // second copy of the arithmetic is how the sign of a turn goes wrong. Three
    // probes pin an affine mapping completely.
    const QPointF origin = m_view->fromPoints(row, QPointF(0.0, 0.0));
    const QPointF across = m_view->fromPoints(row, QPointF(1.0, 0.0)) - origin;
    const QPointF up = m_view->fromPoints(row, QPointF(0.0, 1.0)) - origin;
    return { across.x(), across.y(), up.x(), up.y(), origin.x(), origin.y() };
}

void ObjectOverlay::paintInsertions(QPainter &painter, int row, const QVector<PageObject> &shown,
                                    const QTransform &live)
{
    if (!m_view || m_insertions.isEmpty()) {
        return;
    }

    for (const PageObject &object : shown) {
        if (!isPasted(object.index)) {
            continue;
        }
        const Insertion *insertion = insertionFor(object.index);
        if (!insertion) {
            continue;
        }

        // As it now stands, the drag in progress included, and worked out the
        // same way the composer will work it out at saving time: what is drawn
        // here and what ends up in the file are then the same shape by
        // construction rather than by two pieces of arithmetic agreeing.
        NewContent item = insertionAsWritten(*insertion);
        if (row == m_page && m_chosen.contains(object.index)) {
            item = contentUnder(item, live);
        }

        const QRectF rect = item.rect.normalized();
        const QRectF box = m_view->fromPoints(row, rect);
        if (box.isEmpty()) {
            continue;
        }

        // Drawn here rather than written and re-rendered: the file still says
        // nothing about this, and the whole point of a pending change is that
        // the page is not touched until the window commits it.
        // Not clipped to its own box: the composer lets a line of text run past
        // the box it was measured for, and a preview that cut it off would
        // promise a tidier page than the one that gets written.
        painter.save();

        // Everything below places itself in the upright box the content was
        // measured in (the box the composer writes it in too), so the turn is
        // put on the painter instead. In pixels rather than in points, which is
        // the same conjugation the composer does to reach page space.
        if (!item.placement.isIdentity()) {
            const QTransform pixels = toPixels(row);
            bool invertible = false;
            const QTransform back = pixels.inverted(&invertible);
            if (invertible) {
                painter.setTransform(back * item.placement * pixels, true);
            }
        }

        switch (item.kind) {
        case NewContent::Kind::Text: {
            const double size = item.fontSize;
            QFont font = m_view->font();
            font.setPixelSize(std::max(1, int(std::lround(size * m_view->zoom()))));
            painter.setFont(font);
            painter.setPen(item.fill.isValid() ? item.fill : QColor(Qt::black));

            // The same baselines the composer will use, so what is seen now is
            // where the letters land in the file afterwards.
            double baseline = rect.bottom() - size;
            const QStringList lines = item.text.split(u'\n');
            for (const QString &line : lines) {
                painter.drawText(m_view->fromPoints(row, QPointF(rect.left(), baseline)), line);
                baseline -= size * LineStep;
            }
            break;
        }
        case NewContent::Kind::Image:
            painter.drawImage(box, item.image);
            break;
        case NewContent::Kind::Rectangle:
        case NewContent::Kind::Ellipse:
        case NewContent::Kind::Line: {
            painter.setBrush(item.fill.isValid() ? QBrush(item.fill) : QBrush(Qt::NoBrush));
            painter.setPen(item.stroke.isValid() ? QPen(item.stroke, std::max(1.0, item.lineWidth * m_view->zoom()))
                                                 : QPen(Qt::NoPen));
            if (item.kind == NewContent::Kind::Ellipse) {
                painter.drawEllipse(box);
            } else if (item.kind == NewContent::Kind::Line) {
                painter.drawLine(box.topLeft(), box.bottomRight());
            } else {
                painter.drawRect(box);
            }
            break;
        }
        }
        painter.restore();
    }
}

// ── The mouse ─────────────────────────────────────────────────────────────

ObjectOverlay::Grip ObjectOverlay::gripAt(int row, const QPointF &pagePoint)
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
    for (int grip = int(Grip::TopLeft); grip <= int(Grip::Turn); ++grip) {
        if (QLineF(where, gripCentre(box, static_cast<Grip>(grip))).length() <= HandleReach) {
            return static_cast<Grip>(grip);
        }
    }
    return Grip::None;
}

QTransform ObjectOverlay::liveTransform() const
{
    return m_drag == Drag::None ? QTransform() : transformFor(m_drag, m_to);
}

QTransform ObjectOverlay::transformFor(Drag drag, const QPointF &to) const
{
    const QPointF delta = to - m_from;
    if (drag == Drag::Move) {
        return QTransform::fromTranslate(delta.x(), delta.y());
    }
    if (m_startBox.width() <= 0.0 || m_startBox.height() <= 0.0) {
        return {};
    }

    if (drag == Drag::Turn) {
        const QPointF pivot = m_startBox.center();
        const double was = std::atan2(m_from.y() - pivot.y(), m_from.x() - pivot.x());
        const double now = std::atan2(to.y() - pivot.y(), to.x() - pivot.x());
        double angle = qRadiansToDegrees(now - was);
        if (QGuiApplication::keyboardModifiers().testFlag(Qt::ShiftModifier)) {
            angle = std::round(angle / 15.0) * 15.0; // The angles people mean when they say "a bit round".
        }
        // Built on an identity matrix so there is no doubt which side of the
        // multiplication the rotation lands on. Page points run up, so this
        // turns counter-clockwise.
        QTransform rotation;
        rotation.rotate(angle);
        return QTransform::fromTranslate(-pivot.x(), -pivot.y()) * rotation
            * QTransform::fromTranslate(pivot.x(), pivot.y());
    }

    if (drag != Drag::Resize) {
        return {};
    }

    const bool left = m_grip == Grip::TopLeft || m_grip == Grip::Left || m_grip == Grip::BottomLeft;
    const bool right = m_grip == Grip::TopRight || m_grip == Grip::Right || m_grip == Grip::BottomRight;
    const bool up = m_grip == Grip::TopLeft || m_grip == Grip::Top || m_grip == Grip::TopRight;
    const bool down = m_grip == Grip::BottomLeft || m_grip == Grip::Bottom || m_grip == Grip::BottomRight;

    // The far side of the box stays where it is, which is what makes a handle
    // feel attached to the object rather than to the pointer. Points run up the
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

    // Dragging a handle past the far edge would turn the object inside out,
    // which is never what the hand that did it meant.
    across = std::max(across, MinimumSizePoints / m_startBox.width());
    downwards = std::max(downwards, MinimumSizePoints / m_startBox.height());

    // A corner holds the proportions and an edge does not. That is the division
    // every drawing program on the desktop has taught the hand: the corner is
    // reached for to make a picture bigger, the middle of an edge to make it a
    // different shape. Shift frees the corner for the times when the proportions
    // really are what has to change.
    if ((left || right) && (up || down) && !QGuiApplication::keyboardModifiers().testFlag(Qt::ShiftModifier)) {
        // The pointer rarely lands exactly on the diagonal; following the mean
        // of the two keeps the proportions without the box snapping to one axis.
        const double both = (across + downwards) / 2.0;
        across = both;
        downwards = both;
    }

    return QTransform::fromTranslate(-anchor.x(), -anchor.y()) * QTransform::fromScale(across, downwards)
        * QTransform::fromTranslate(anchor.x(), anchor.y());
}

bool ObjectOverlay::press(int row, const QPointF &pagePoint, Qt::MouseButtons buttons)
{
    if (!m_view || row < 0 || !buttons.testFlag(Qt::LeftButton)) {
        return false;
    }
    const QVector<PageObject> objects = shownOf(row);
    if (objects.isEmpty()) {
        return false; // Nothing has been read for this page; let another overlay have the press.
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
        if (movableChosen().isEmpty()) {
            Q_EMIT refused(unmovableMessage(m_chosen.size()));
            return true;
        }
        m_grip = grip;
        m_startBox = choiceBounds();
        m_drag = grip == Grip::Turn ? Drag::Turn : Drag::Resize;
        return true;
    }

    const int hit = PageObjects::hitTest(objects, pagePoint);
    if (hit == NoObject) {
        // Let go of the old choice first: clearing one gives up whatever gesture
        // was in progress, so a band declared before that call is cancelled by
        // it and the drag draws nothing.
        if (!add) {
            clearChoice();
        }
        m_drag = Drag::Band;
        repaint();
        return true;
    }

    if (add) {
        choose(row, { hit }, true);
    } else if (row != m_page || !m_chosen.contains(hit)) {
        choose(row, { hit }, false);
    } else if (m_current != hit) {
        // Pressing something already chosen keeps the whole choice, so that a
        // group of five can be dragged without picking them again one by one.
        m_current = hit;
        Q_EMIT choiceChanged();
    }

    if (!m_chosen.isEmpty()) {
        if (movableChosen().isEmpty()) {
            Q_EMIT refused(unmovableMessage(m_chosen.size()));
        } else {
            m_drag = Drag::Move;
            m_startBox = choiceBounds();
        }
    }
    repaint();
    return true;
}

bool ObjectOverlay::move(int row, const QPointF &pagePoint, Qt::MouseButtons buttons)
{
    Q_UNUSED(row)
    Q_UNUSED(buttons)
    if (m_drag == Drag::None) {
        return false;
    }
    m_to = pagePoint;

    // The numbers in the panel follow the drag rather than waiting for it to
    // end, because "how big is it now" is the question being asked by the drag.
    refreshDetails();
    repaint();
    return true;
}

bool ObjectOverlay::release(int row, const QPointF &pagePoint)
{
    Q_UNUSED(row)
    if (m_drag == Drag::None) {
        return false;
    }
    m_to = pagePoint;
    const Drag drag = m_drag;
    const QTransform gesture = transformFor(drag, m_to);
    m_drag = Drag::None;
    m_grip = Grip::None;

    if (drag == Drag::Band) {
        const QRectF band = QRectF(m_from, m_to).normalized();
        if (band.width() > 2.0 && band.height() > 2.0) {
            choose(m_dragRow, PageObjects::within(shownOf(m_dragRow), band),
                   QGuiApplication::keyboardModifiers().testFlag(Qt::ControlModifier));
        }
        repaint();
        return true;
    }

    const QPointF shift = m_to - m_from;
    const bool worthIt = drag != Drag::Move || qAbs(shift.x()) > DragSlopPoints || qAbs(shift.y()) > DragSlopPoints;
    if (worthIt && !gesture.isIdentity()) {
        applyGesture([gesture](const QRectF &) { return gesture; });
    }
    repaint();
    return true;
}

Qt::CursorShape ObjectOverlay::cursor(int row, const QPointF &pagePoint)
{
    if (!m_view) {
        return Qt::ArrowCursor;
    }
    const Grip grip = gripAt(row, pagePoint);
    if (grip != Grip::None) {
        return cursorForGrip(grip);
    }

    // Asking the engine rather than the boxes: a thin diagonal line is caught by
    // its own shape, and a rectangle the size of the page is not what was meant.
    const int hit = PageObjects::hitTest(shownOf(row), pagePoint);
    setHover(row, hit);
    if (hit == NoObject) {
        return Qt::ArrowCursor;
    }
    return row == m_page && m_chosen.contains(hit) ? Qt::SizeAllCursor : Qt::OpenHandCursor;
}

// ── The keyboard ──────────────────────────────────────────────────────────

bool ObjectOverlay::eventFilter(QObject *watched, QEvent *event)
{
    if (watched != m_view || !m_view || m_view->mode() != PageView::Mode::Edit || m_chosen.isEmpty()) {
        return QObject::eventFilter(watched, event);
    }

    // The window binds Delete to deleting whole pages, and a shortcut is matched
    // before the key ever reaches the widget. While something on the page is
    // chosen, Delete means that thing; accepting the override sends the key on
    // as a key and leaves the pages alone.
    if (event->type() == QEvent::ShortcutOverride) {
        auto *pressed = static_cast<QKeyEvent *>(event);
        if (pressed->key() == Qt::Key_Delete || pressed->key() == Qt::Key_Backspace) {
            pressed->accept();
            return true;
        }
        return QObject::eventFilter(watched, event);
    }
    if (event->type() != QEvent::KeyPress) {
        return QObject::eventFilter(watched, event);
    }

    auto *key = static_cast<QKeyEvent *>(event);
    const Qt::KeyboardModifiers modifiers = key->modifiers();

    if (modifiers.testFlag(Qt::ControlModifier) && modifiers.testFlag(Qt::ShiftModifier)) {
        if (key->key() == Qt::Key_Up) {
            raiseChosen();
            return true;
        }
        if (key->key() == Qt::Key_Down) {
            lowerChosen();
            return true;
        }
    }

    // A point at a time, ten with Shift: the numbers people reach for when
    // something is nearly in the right place.
    const double step = modifiers.testFlag(Qt::ShiftModifier) ? 10.0 : 1.0;
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
    case Qt::Key_Escape:
        clearChoice();
        return true;
    default:
        break;
    }
    return QObject::eventFilter(watched, event);
}

// ── The choice ────────────────────────────────────────────────────────────

QVector<int> ObjectOverlay::chosenObjects() const
{
    QVector<int> objects(m_chosen.constBegin(), m_chosen.constEnd());
    std::sort(objects.begin(), objects.end());
    return objects;
}

void ObjectOverlay::choose(int row, const QVector<int> &objects, bool add)
{
    const int wasPage = m_page;
    const QSet<int> was = m_chosen;

    // A choice lives on one page: the edits are keyed by page, and a gesture
    // means one thing in one page's coordinates.
    if (row != m_page || !add) {
        m_chosen.clear();
        m_page = row;
    }

    for (const int index : objects) {
        if (add && m_chosen.contains(index)) {
            m_chosen.remove(index);
            continue;
        }
        m_chosen.insert(index);
        m_current = index;
    }

    if (m_chosen.isEmpty()) {
        m_current = NoObject;
    } else if (!m_chosen.contains(m_current)) {
        m_current = *m_chosen.constBegin();
    }

    if (m_page != wasPage || m_chosen != was) {
        repaint();
        Q_EMIT choiceChanged();
    }
}

void ObjectOverlay::clearChoice()
{
    if (m_page < 0 && m_chosen.isEmpty()) {
        return;
    }
    m_chosen.clear();
    m_page = -1;
    m_current = NoObject;
    m_drag = Drag::None;
    repaint();
    Q_EMIT choiceChanged();
}

void ObjectOverlay::setHover(int row, int index)
{
    if (m_hoverPage == row && m_hover == index) {
        return;
    }
    m_hoverPage = row;
    m_hover = index;
    repaint();
}

Inspectable *ObjectOverlay::inspected()
{
    // Whether there is one at all is the membership question alone: an object
    // number below zero is a perfectly good pasted object, not an empty choice.
    // What it is not is something the page has stopped drawing: a pasted thing
    // that has since been deleted is still chosen and no longer there.
    if (m_page < 0 || !m_chosen.contains(m_current) || shownObject(m_page, m_current).index != m_current) {
        return nullptr;
    }
    if (!m_details) {
        m_details = std::make_unique<Details>(this);
    }
    refreshDetails();
    return m_details.get();
}

void ObjectOverlay::refreshDetails()
{
    if (!m_details || m_page < 0 || !m_chosen.contains(m_current)) {
        return;
    }
    m_details->show(shownObject(m_page, m_current), m_chosen.size(), int(objectsOf(m_page).size()));
}

void ObjectOverlay::repaint()
{
    if (m_view) {
        m_view->viewport()->update();
    }
}

// ── The clipboard ─────────────────────────────────────────────────────────

QImage ObjectOverlay::pixelsOf(int row, const PageObject &object, QString *why)
{
    const auto refuse = [why](const QString &reason) {
        if (why) {
            *why = reason;
        }
        return QImage();
    };

    if (object.kind == PageObject::Kind::InlineImage || object.resourceName.isEmpty()) {
        return refuse(i18nc("@info why an inline picture cannot be copied",
                            "An inline picture cannot be copied: its pixels are written into the page's own "
                            "instructions rather than into an object that can be lifted out of them."));
    }

    // The file this row's page really lives in, which is the file the resource
    // name means something in, not whichever file the document was last saved
    // as, and not the same file for every row of a merged document.
    const SourcePage where = m_rows.pageOf(row);
    const QString holder = where.isValid() ? m_rows.fileOf(where) : m_source;
    const int page = where.isValid() ? where.pageInFile() : object.page;
    if (holder.isEmpty()) {
        return refuse(i18nc("@info", "The file behind this page is not available, so its pictures cannot be read."));
    }

    // Taken out of the file rather than off the screen: what the page draws is
    // the stored picture, and a copy cut out of the rendered view would carry
    // whatever zoom it happened to be at and whatever was drawn behind it.
    QTemporaryFile file(QDir::tempPath() + u"/pdf-smithy-copy-XXXXXX.png"_s);
    if (!file.open()) {
        return refuse(i18nc("@info there is nowhere to write a temporary file",
                            "A picture could not be copied: this machine has nowhere to put it while it is being "
                            "read out of the file."));
    }
    const QString path = file.fileName();
    file.close();

    QString error;
    if (!ImageEdit::extract(holder, page, object.resourceName, path, &error)) {
        return refuse(i18nc("@info %1 names a picture, %2 says what went wrong", "%1 could not be copied: %2",
                            object.resourceName, error));
    }

    const QImage image(path);
    if (image.isNull()) {
        return refuse(i18nc("@info %1 names a picture",
                            "%1 came out of the file in a form this program cannot read back.", object.resourceName));
    }
    return image;
}

PageClipboard::Parcel ObjectOverlay::parcelOfChoice()
{
    PageClipboard::Parcel parcel;
    if (m_page < 0 || m_chosen.isEmpty()) {
        return parcel;
    }

    bool substituted = false;
    bool unreadable = false;

    // As the page now stands: an object already dragged somewhere else is copied
    // from where the user can see it, not from where the file still has it.
    const QVector<PageObject> objects = shownOf(m_page);
    for (const PageObject &object : objects) {
        if (!m_chosen.contains(object.index)) {
            continue;
        }
        const QRectF bounds = object.bounds.normalized();

        // What was pasted is already content in the shape the composer takes, so
        // the copy is the thing itself. Reading it back off the page would mean
        // reading a page it has not been written to yet.
        if (isPasted(object.index)) {
            if (const Insertion *insertion = insertionFor(object.index)) {
                parcel.items.append(insertionAsWritten(*insertion));
            }
            continue;
        }

        switch (object.kind) {
        case PageObject::Kind::Text: {
            QString said = object.text;
            const bool lost = said.contains(QChar(Unreadable));
            said.remove(QChar(Unreadable));
            if (said.trimmed().isEmpty()) {
                unreadable = true;
                break;
            }
            unreadable = unreadable || lost;

            NewContent item;
            item.kind = NewContent::Kind::Text;
            item.rect = bounds;
            item.text = said;
            // A page may set its type through the text matrix alone; the box the
            // letters came out in still says how tall they are.
            item.fontSize = object.fontSize > 0.0 ? object.fontSize : bounds.height() / 1.2;
            item.fill = object.fill.isValid() ? object.fill : QColor(Qt::black);
            parcel.items.append(item);
            substituted = true;
            break;
        }

        case PageObject::Kind::Image:
        case PageObject::Kind::InlineImage: {
            QString why;
            const QImage pixels = pixelsOf(m_page, object, &why);
            if (pixels.isNull()) {
                parcel.missing.append(why);
                break;
            }
            NewContent item;
            item.kind = NewContent::Kind::Image;
            item.rect = bounds;
            item.image = pixels;
            parcel.items.append(item);
            break;
        }

        case PageObject::Kind::Path:
        case PageObject::Kind::Drawing:
        case PageObject::Kind::Shading:
            // The outline this program holds is an approximation good enough to
            // click on. Writing it back out would put a different shape on the
            // page and call it a copy.
            parcel.missing.append(cannotRebuildMessage(object.kind));
            break;
        }
    }

    if (substituted) {
        parcel.missing.append(substituteFontMessage());
    }
    if (unreadable) {
        parcel.missing.append(unreadableTextMessage());
    }
    return parcel;
}

bool ObjectOverlay::putChoiceOnClipboard(QStringList *missing)
{
    const PageClipboard::Parcel parcel = parcelOfChoice();
    if (parcel.isEmpty()) {
        return false;
    }
    // Packed even when it holds nothing but refusals: the paste is where the
    // user finds out what did not come, and it has to have something to say.
    QGuiApplication::clipboard()->setMimeData(PageClipboard::pack(parcel));
    if (missing) {
        *missing = parcel.missing;
    }
    m_pasteWalk = 0;
    return true;
}

bool ObjectOverlay::copy()
{
    return putChoiceOnClipboard(nullptr);
}

bool ObjectOverlay::cut()
{
    QStringList missing;
    if (!putChoiceOnClipboard(&missing)) {
        return false;
    }
    // Said now rather than at the paste: what the clipboard could not carry is
    // about to be taken off the page, and the two halves of that belong
    // together. The removal is only waiting, so it can still be taken back.
    for (const QString &one : std::as_const(missing)) {
        Q_EMIT refused(one);
    }
    removeChosen();
    return true;
}

bool ObjectOverlay::paste(const QMimeData *data, int row)
{
    if (!m_view || !data) {
        return false;
    }
    // The page the reader is looking at, because that is the one they mean;
    // the chosen object's page only when the view has no answer.
    const int page = row >= 0 ? row : m_page;
    if (page < 0) {
        return false;
    }

    // Our own parcel first. Falling back rather than failing when it will not
    // read covers the one case a private type has to survive: a copy made by a
    // build that writes the parcel differently still has its plain-text twin.
    PageClipboard::Parcel parcel = PageClipboard::unpack(data);
    if (parcel.isEmpty()) {
        parcel = PageClipboard::fromForeign(data, m_view->pageSizePoints(page));
    }
    if (parcel.isEmpty()) {
        return false; // Not ours and not something a page can hold; let the view say so.
    }

    if (!parcel.items.isEmpty()) {
        const QSizeF size = m_view->pageSizePoints(page);
        const QRectF paper(0.0, 0.0, size.width(), size.height());

        // Each paste steps further from the one before it. A walk that has
        // carried the copy off the paper starts again at the first step, which
        // is better than pasting somewhere nobody can see.
        ++m_pasteWalk;
        double step = PasteStepPoints * m_pasteWalk;
        for (const NewContent &item : parcel.items) {
            if (!paper.intersects(whereItStands(item).translated(step, -step))) {
                m_pasteWalk = 1;
                step = PasteStepPoints;
                break;
            }
        }

        QVector<int> landed;
        landed.reserve(parcel.items.size());
        for (const NewContent &item : parcel.items) {
            Insertion placed;
            // Stepped the way every other gesture moves it, so that a copy that
            // was turned before it was copied steps across the page rather than
            // along its own edge.
            placed.content = contentUnder(item, QTransform::fromTranslate(step, -step));
            placed.content.page = page;

            // Never handed out twice, even across a save that clears the list:
            // a number given again could be named by a change that outlived the
            // thing it was made on.
            placed.handle = m_nextHandle--;
            m_insertions.append(placed);
            landed.append(placed.handle);
        }

        // Chosen the moment it lands, handles and all. What has just been put on
        // the page is what the hand is reaching for, and a paste that arrives
        // without a grip on it is a paste that looks stuck.
        choose(page, landed, false);
        Q_EMIT editsChanged();
    }

    // The refusals travelled with the copy so that they could be said here,
    // where the user is looking for the thing to appear.
    for (const QString &one : std::as_const(parcel.missing)) {
        Q_EMIT refused(one);
    }

    repaint();
    return true;
}

// ── Collecting the changes ────────────────────────────────────────────────

QVector<int> ObjectOverlay::movableChosen()
{
    QVector<int> movable;
    if (m_page < 0) {
        return movable;
    }
    const QVector<PageObject> objects = objectsOf(m_page);
    for (const PageObject &object : objects) {
        if (m_chosen.contains(object.index) && object.placement.isInvertible()) {
            movable.append(object.index);
        }
    }
    return movable;
}

QVector<QRectF> ObjectOverlay::alignableBoxes()
{
    QVector<QRectF> boxes;
    if (m_page < 0 || m_chosen.isEmpty()) {
        return boxes;
    }

    // As the page now stands, so that the buttons line up what the user can see
    // rather than what the file will go on saying until the changes are written.
    const QVector<PageObject> shown = shownOf(m_page);
    for (const PageObject &object : shown) {
        if (m_chosen.contains(object.index) && object.placement.isInvertible()) {
            boxes.append(object.bounds.normalized());
        }
    }
    return boxes;
}

void ObjectOverlay::applyGesture(const std::function<QTransform(const QRectF &now)> &gesture)
{
    applyGesture(gesture, chosenObjects());
}

void ObjectOverlay::applyGesture(const std::function<QTransform(const QRectF &now)> &gesture,
                                 const QVector<int> &objects)
{
    applyGestures([&gesture](int, const QRectF &now) { return gesture(now); }, objects);
}

void ObjectOverlay::applyGestures(const std::function<QTransform(int index, const QRectF &now)> &gesture,
                                  const QVector<int> &objects)
{
    if (m_page < 0 || objects.isEmpty()) {
        return;
    }

    // Held by value: the cache is written to while a page is being read, and a
    // reference into it would not survive that.
    const QVector<PageObject> onPage = objectsOf(m_page);
    QHash<int, PageEdit> &here = m_pending[m_page];
    int stuck = 0;
    int displaced = 0;
    bool did = false;

    for (const int index : objects) {
        const PageObject *object = nullptr;
        for (const PageObject &candidate : onPage) {
            if (candidate.index == index) {
                object = &candidate;
                break;
            }
        }
        if (!object) {
            continue;
        }
        // The composer has to invert the matrix in force when the object was
        // drawn; asked now, the answer arrives while the mouse is still on it.
        if (!object->placement.isInvertible()) {
            ++stuck;
            continue;
        }

        PageEdit &edit = here[index];
        if (edit.kind != PageEdit::Kind::Transform) {
            if (edit.kind == PageEdit::Kind::Restyle || edit.kind == PageEdit::Kind::Delete) {
                ++displaced;
            }
            edit = PageEdit();
        }
        edit.kind = PageEdit::Kind::Transform;
        edit.page = m_page;
        edit.object = index;
        edit.transform = edit.transform * gesture(index, edit.transform.mapRect(object->bounds));
        did = true;
    }

    if (stuck > 0) {
        Q_EMIT refused(unmovableMessage(stuck));
    }
    if (displaced > 0) {
        Q_EMIT refused(oneChangeMessage());
    }
    if (!did) {
        return;
    }

    Q_EMIT editsChanged();
    refreshDetails();
    repaint();
}

void ObjectOverlay::restyleChosen(const QColor &fill, const QColor &stroke, double lineWidth)
{
    const QVector<int> chosen = chosenObjects();
    if (m_page < 0 || chosen.isEmpty()) {
        return;
    }

    QHash<int, PageEdit> &here = m_pending[m_page];
    int displaced = 0;
    for (const int index : chosen) {
        PageEdit &edit = here[index];
        // Colours set one after another belong to the same waiting change, or
        // choosing an outline would throw away the fill chosen a moment before.
        if (edit.kind != PageEdit::Kind::Restyle) {
            if (edit.kind == PageEdit::Kind::Transform && !edit.transform.isIdentity()) {
                ++displaced;
            }
            edit = PageEdit();
        }
        edit.kind = PageEdit::Kind::Restyle;
        edit.page = m_page;
        edit.object = index;
        if (fill.isValid()) {
            edit.fill = fill;
        }
        if (stroke.isValid()) {
            edit.stroke = stroke;
        }
        if (lineWidth >= 0.0) {
            edit.lineWidth = lineWidth;
        }
    }

    if (displaced > 0) {
        Q_EMIT refused(oneChangeMessage());
    }
    Q_EMIT editsChanged();
    repaint();
}

void ObjectOverlay::nudge(double acrossPoints, double upPoints)
{
    applyGesture(
        [acrossPoints, upPoints](const QRectF &) { return QTransform::fromTranslate(acrossPoints, upPoints); });
}

void ObjectOverlay::placeChosen(const QVector<QRectF> &boxes)
{
    const QVector<int> alignable = movableChosen();
    if (m_page < 0 || boxes.size() != alignable.size()) {
        // The choice changed under the panel between the buttons being drawn and
        // one of them being pressed, so these boundaries name objects that are
        // no longer the ones chosen.
        return;
    }

    // Said before anything moves, and said even when nothing can: "four of the
    // five lined up" is a question the user would otherwise have to answer by
    // staring at the page.
    const int stuck = m_chosen.size() - alignable.size();
    if (stuck > 0) {
        Q_EMIT refused(unmovableMessage(stuck));
    }

    // Only what actually has somewhere to go: an object already on the line
    // would otherwise collect a waiting change that moves it nowhere.
    const QVector<QRectF> now = alignableBoxes();
    QHash<int, QRectF> wanted;
    QVector<int> moving;
    for (int at = 0; at < alignable.size() && at < now.size(); ++at) {
        const QRectF to = boxes.at(at).normalized();
        if (sameBox(now.at(at), to)) {
            continue;
        }
        wanted.insert(alignable.at(at), to);
        moving.append(alignable.at(at));
    }
    if (moving.isEmpty()) {
        return;
    }

    // The boundary handed in is where the object is to end up, so the matrix is
    // whatever carries it from where it now stands onto that. Folded into what
    // was already waiting, exactly as a drag is: one change per object is what
    // the composer allows, and it has to hold every gesture made so far.
    applyGestures(
        [&wanted](int index, const QRectF &from) { return placement(from.normalized(), wanted.value(index)); }, moving);
}

void ObjectOverlay::alignChosen(Alignment::Edge edge)
{
    placeChosen(Alignment::align(alignableBoxes(), edge));
}

void ObjectOverlay::distributeChosen(Alignment::Spread spread)
{
    placeChosen(Alignment::distribute(alignableBoxes(), spread));
}

void ObjectOverlay::matchChosenSize(Alignment::Match what)
{
    placeChosen(Alignment::matchSize(alignableBoxes(), what));
}

void ObjectOverlay::removeChosen()
{
    const QVector<int> chosen = chosenObjects();
    if (m_page < 0 || chosen.isEmpty()) {
        return;
    }

    // Waiting rather than done: the page is rewritten in one pass at saving
    // time, and taking an object out now would renumber everything after it
    // under the choice the user is still working with.
    QHash<int, PageEdit> &here = m_pending[m_page];
    for (const int index : chosen) {
        PageEdit edit;
        edit.kind = PageEdit::Kind::Delete;
        edit.page = m_page;
        edit.object = index;
        here.insert(index, edit);
    }
    Q_EMIT editsChanged();
    repaint();
}

void ObjectOverlay::takeBack()
{
    if (m_page < 0 || !m_pending.contains(m_page)) {
        return;
    }
    QHash<int, PageEdit> &here = m_pending[m_page];
    bool dropped = false;
    for (const int index : chosenObjects()) {
        dropped = here.remove(index) > 0 || dropped;
    }
    if (!dropped) {
        return;
    }
    if (here.isEmpty()) {
        m_pending.remove(m_page);
    }
    Q_EMIT editsChanged();
    refreshDetails();
    repaint();
}

void ObjectOverlay::takeBackAll()
{
    if (m_pending.isEmpty() && m_insertions.isEmpty()) {
        return;
    }
    const bool held = choiceHoldsPaste();
    m_pending.clear();
    m_insertions.clear();
    m_pasteWalk = 0;

    // What was pasted has gone from the list, so a choice naming it names
    // nothing at all and its handles would stand round an empty box.
    if (held) {
        clearChoice();
    }
    Q_EMIT editsChanged();
    refreshDetails();
    repaint();
}

void ObjectOverlay::raiseChosen()
{
    Q_EMIT refused(orderMessage());
}

void ObjectOverlay::lowerChosen()
{
    Q_EMIT refused(orderMessage());
}

QVector<PageEdit> ObjectOverlay::pendingEdits() const
{
    QVector<PageEdit> edits;
    for (auto page = m_pending.constBegin(); page != m_pending.constEnd(); ++page) {
        for (auto edit = page->constBegin(); edit != page->constEnd(); ++edit) {
            // A change made to something pasted names no operator on the page,
            // so it never travels as an edit. It goes into the insertion itself,
            // in pendingInsertions().
            if (!isPasted(edit->object)) {
                edits.append(*edit);
            }
        }
    }
    return edits;
}

bool ObjectOverlay::hasPendingEdits() const
{
    for (auto page = m_pending.constBegin(); page != m_pending.constEnd(); ++page) {
        for (auto edit = page->constBegin(); edit != page->constEnd(); ++edit) {
            if (!isPasted(edit->object)) {
                return true;
            }
        }
    }
    return false;
}

QVector<NewContent> ObjectOverlay::pendingInsertions() const
{
    QVector<NewContent> items;
    items.reserve(m_insertions.size());
    for (const Insertion &insertion : m_insertions) {
        // Something pasted and then deleted was never written, so it simply does
        // not go: there is nothing on the page for a deletion to take away.
        if (waitingOn(insertion.content.page, insertion.handle).kind == PageEdit::Kind::Delete) {
            continue;
        }
        items.append(insertionAsWritten(insertion));
    }
    return items;
}

bool ObjectOverlay::hasPendingInsertions() const
{
    // Asked without building the list, because a picture's pixels are copied
    // with it and this is answered on every change.
    for (const Insertion &insertion : m_insertions) {
        if (waitingOn(insertion.content.page, insertion.handle).kind != PageEdit::Kind::Delete) {
            return true;
        }
    }
    return false;
}

} // namespace ps
