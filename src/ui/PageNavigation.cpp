/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "PageNavigation.h"

#include <KLocalizedString>

#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QSpinBox>
#include <QToolButton>

using namespace Qt::Literals::StringLiterals;

namespace ps {

QHBoxLayout *pageNavigation(QWidget *parent, QSpinBox *pageBox, int pageCount)
{
    auto *previous = new QToolButton(parent);
    previous->setIcon(QIcon::fromTheme(u"go-previous"_s));
    previous->setToolTip(i18nc("@info:tooltip", "The page before this one"));
    previous->setAutoRepeat(true);

    auto *next = new QToolButton(parent);
    next->setIcon(QIcon::fromTheme(u"go-next"_s));
    next->setToolTip(i18nc("@info:tooltip", "The page after this one"));
    next->setAutoRepeat(true);

    pageBox->setRange(1, qMax(1, pageCount));
    pageBox->setPrefix(i18nc("@label:spinbox prefix before a page number", "Page "));

    auto *total = new QLabel(i18nc("@info total page count", "of %1", QLocale().toString(pageCount)), parent);

    auto *row = new QHBoxLayout;
    row->setContentsMargins(0, 0, 0, 0);
    row->addWidget(previous);
    row->addWidget(pageBox);
    row->addWidget(next);
    row->addWidget(total);
    row->addStretch(1);

    QObject::connect(previous, &QToolButton::clicked, pageBox, [pageBox] { pageBox->setValue(pageBox->value() - 1); });
    QObject::connect(next, &QToolButton::clicked, pageBox, [pageBox] { pageBox->setValue(pageBox->value() + 1); });

    // Greyed out at the ends rather than silently doing nothing, so that a
    // document of one page does not offer two buttons that never work.
    const auto follow = [previous, next, pageBox] {
        previous->setEnabled(pageBox->value() > pageBox->minimum());
        next->setEnabled(pageBox->value() < pageBox->maximum());
    };
    QObject::connect(pageBox, &QSpinBox::valueChanged, pageBox, follow);
    follow();

    return row;
}

} // namespace ps
