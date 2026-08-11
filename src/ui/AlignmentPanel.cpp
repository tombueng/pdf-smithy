/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "AlignmentPanel.h"

#include <KLocalizedString>

#include <QGridLayout>
#include <QIcon>
#include <QToolButton>

#include <utility>

using namespace Qt::Literals::StringLiterals;

namespace ps {

AlignmentPanel::AlignmentPanel(QWidget *parent)
    : QWidget(parent)
    , m_grid(new QGridLayout(this))
{
    m_grid->setContentsMargins(0, 0, 0, 0);
    m_grid->setSpacing(2);

    // Read across, one axis per row: everything sideways first, everything up
    // and down beneath it, sizes last. Spacing sits at the end of its own row
    // rather than in a group of its own, because it answers the same question
    // about the same axis as the three lines before it.
    addButton(0, 0, u"align-horizontal-left"_s, i18nc("@action:button", "Align Left"),
              i18nc("@info:tooltip", "Line the left edges up with the leftmost."), 2,
              [](const QVector<QRectF> &boxes) {
                  return Alignment::align(boxes, Alignment::Edge::Left);
              });
    addButton(0, 1, u"align-horizontal-center"_s, i18nc("@action:button line the boxes up on a shared upright line", "Align Centre"),
              i18nc("@info:tooltip", "Line the centres up on one upright line, halfway across the selection."), 2,
              [](const QVector<QRectF> &boxes) {
                  return Alignment::align(boxes, Alignment::Edge::HorizontalCentre);
              });
    addButton(0, 2, u"align-horizontal-right"_s, i18nc("@action:button", "Align Right"),
              i18nc("@info:tooltip", "Line the right edges up with the rightmost."), 2,
              [](const QVector<QRectF> &boxes) {
                  return Alignment::align(boxes, Alignment::Edge::Right);
              });
    addButton(0, 3, u"distribute-horizontal"_s, i18nc("@action:button", "Space Out Sideways"),
              i18nc("@info:tooltip", "Put equal gaps between them from left to right. The outermost two stay put."), 3,
              [](const QVector<QRectF> &boxes) {
                  return Alignment::distribute(boxes, Alignment::Spread::Horizontally);
              });

    addButton(1, 0, u"align-vertical-top"_s, i18nc("@action:button", "Align Top"),
              i18nc("@info:tooltip", "Line the top edges up with the highest."), 2,
              [](const QVector<QRectF> &boxes) {
                  return Alignment::align(boxes, Alignment::Edge::Top);
              });
    addButton(1, 1, u"align-vertical-center"_s, i18nc("@action:button line the boxes up on a shared level line", "Align Middle"),
              i18nc("@info:tooltip", "Line the centres up on one level line, halfway up the selection."), 2,
              [](const QVector<QRectF> &boxes) {
                  return Alignment::align(boxes, Alignment::Edge::VerticalMiddle);
              });
    addButton(1, 2, u"align-vertical-bottom"_s, i18nc("@action:button", "Align Bottom"),
              i18nc("@info:tooltip", "Line the bottom edges up with the lowest."), 2,
              [](const QVector<QRectF> &boxes) {
                  return Alignment::align(boxes, Alignment::Edge::Bottom);
              });
    addButton(1, 3, u"distribute-vertical"_s, i18nc("@action:button", "Space Out Upright"),
              i18nc("@info:tooltip", "Put equal gaps between them from bottom to top. The outermost two stay put."), 3,
              [](const QVector<QRectF> &boxes) {
                  return Alignment::distribute(boxes, Alignment::Spread::Vertically);
              });

    // No icon in any standard theme stands for these three, and an invented one
    // would have to be learned before it said anything, so they are written out.
    addButton(2, 0, QString(), i18nc("@action:button", "Same Width"),
              i18nc("@info:tooltip", "Give everything the width of the widest, keeping each top left corner."), 2,
              [](const QVector<QRectF> &boxes) {
                  return Alignment::matchSize(boxes, Alignment::Match::Width);
              });
    addButton(2, 1, QString(), i18nc("@action:button", "Same Height"),
              i18nc("@info:tooltip", "Give everything the height of the tallest, keeping each top left corner."), 2,
              [](const QVector<QRectF> &boxes) {
                  return Alignment::matchSize(boxes, Alignment::Match::Height);
              });
    addButton(2, 2, QString(), i18nc("@action:button", "Same Size"),
              i18nc("@info:tooltip",
                    "Give everything the width of the widest and the height of the tallest, keeping each top left "
                    "corner."),
              2, [](const QVector<QRectF> &boxes) {
                  return Alignment::matchSize(boxes, Alignment::Match::Both);
              });

    // The buttons keep their own width; the room left over goes to the right of
    // them, so a wide properties panel does not stretch eleven buttons across it.
    m_grid->setColumnStretch(m_grid->columnCount(), 1);

    // Nothing is selected until somebody says otherwise, which is the state that
    // greys every button and explains itself.
    refreshButtons();
}

void AlignmentPanel::setBoxes(const QVector<QRectF> &boxes)
{
    m_boxes = boxes;
    refreshButtons();
}

QVector<QRectF> AlignmentPanel::boxes() const
{
    return m_boxes;
}

void AlignmentPanel::addButton(int row, int column, const QString &iconName, const QString &text, const QString &tip,
                               qsizetype needs, const Action &act)
{
    auto *button = new QToolButton(this);
    button->setText(text);

    // The label is set whatever happens, and shown whenever the theme has no
    // such icon. Icon themes outside KDE are missing more of these than they
    // look, and a button that draws nothing at all is one nobody can press on
    // purpose. It is also what a screen reader announces.
    if (QIcon::hasThemeIcon(iconName)) {
        button->setIcon(QIcon::fromTheme(iconName));
        button->setToolButtonStyle(Qt::ToolButtonIconOnly);
    } else {
        button->setToolButtonStyle(Qt::ToolButtonTextOnly);
    }

    button->setAutoRaise(true);

    // Reachable by Tab, unlike a toolbar's buttons: this sits inside a panel of
    // spin boxes that are all reached that way, and stopping at the edge of the
    // alignment row would strand anyone working from the keyboard.
    button->setFocusPolicy(Qt::StrongFocus);

    connect(button, &QToolButton::clicked, this, [this, act] {
        apply(act(m_boxes));
    });

    m_grid->addWidget(button, row, column);
    m_buttons.append(Button { button, needs, tip });
}

void AlignmentPanel::apply(const QVector<QRectF> &boxes)
{
    // Kept before it is announced, so that pressing align-left and then
    // space-out works from what the first press produced. Whoever is listening
    // is free to hand back something else (snapped to a grid, or refused for
    // some of them), and that later answer replaces this one.
    m_boxes = boxes;
    Q_EMIT changed(m_boxes);
}

void AlignmentPanel::refreshButtons()
{
    for (const Button &button : std::as_const(m_buttons)) {
        const bool can = m_boxes.size() >= button.needs;
        button.widget->setEnabled(can);
        if (can) {
            button.widget->setToolTip(button.tip);
            continue;
        }
        // Why, not just no: the button is greyed out at the very moment somebody
        // is wondering what it would have done, and "choose one more" is the
        // whole answer.
        button.widget->setToolTip(button.needs >= 3
                                      ? i18nc("@info:tooltip why a button cannot be pressed",
                                              "Choose at least three things. Spacing out moves what lies between the "
                                              "outermost two, and two things have nothing between them.")
                                      : i18nc("@info:tooltip why a button cannot be pressed",
                                              "Choose at least two things. One is already lined up with itself."));
    }
}

} // namespace ps
