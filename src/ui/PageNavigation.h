/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#pragma once

class QHBoxLayout;
class QSpinBox;
class QWidget;

namespace ps {

/**
 * The strip that says which page is being worked on and moves between them.
 *
 * Written once because it was written seven times, and six of those had the
 * same fault: the forward button was created and then never put into the
 * layout, so Qt left it sitting in the top-left corner of the page view with
 * the page image drawn over it. A widget with a parent and no layout is placed
 * at the origin and nothing warns about it, which is precisely the kind of
 * mistake that copying a block of setup code produces and that having one copy
 * prevents.
 *
 * @p pageBox is wired up here: the buttons step it, and they disable
 * themselves at the ends. The caller connects its @c valueChanged to whatever
 * turning the page means for them.
 */
QHBoxLayout *pageNavigation(QWidget *parent, QSpinBox *pageBox, int pageCount);

} // namespace ps
