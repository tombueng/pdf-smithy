/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#pragma once

#include "FormBuilder.h"

#include <qpdf/QPDFObjectHandle.hh>

class QPDF;

namespace ps {

/**
 * Draws @p widget's `/AP` `/N` so that @p spec.defaultValue shows, in the style
 * @p spec carries, on a page turned by @p rotate degrees.
 *
 * ps::FormBuilder draws every value it writes, because a field with no
 * appearance is invisible in about half the readers in use. Filling in a value
 * has exactly that need, and it cannot be met by asking the library to
 * regenerate the appearance: the QPDF the distributions ship (12.2) leaves a
 * merged field/widget untouched, so a filled value drawn only by it never
 * appears. The value is therefore drawn here instead, with the same engine
 * ps::FormBuilder uses, so that what fills a field looks like what built it and
 * does not depend on which QPDF happens to be installed.
 *
 * @p spec.rect is read for its width and height only; the widget keeps the
 * `/Rect` it already has. Fields that hold no drawable value (a push button, a
 * signature) are not for this; it is meant for text and choice fields.
 */
void drawFieldValueAppearance(QPDF &pdf, QPDFObjectHandle widget, const FormBuilder::Field &spec, int rotate);

} // namespace ps
