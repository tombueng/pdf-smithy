/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#pragma once

#include <QColor>
#include <QString>

namespace ps {

/**
 * Smithy ember — the one warm accent the window marks a live thing with: the
 * mode you are in, the button under the pointer, the scroll handle you are
 * about to grab. Written down once here rather than typed into a dozen
 * stylesheet strings, so the whole program can be re-themed from this line.
 *
 * Deliberately warm but not loud, and always laid on translucent (see
 * accentRgba) so it tints whatever palette the desktop theme hands us instead
 * of fighting it.
 */
inline QColor accentColour()
{
    return QColor(214, 124, 42);
}

/**
 * The accent as a CSS `rgba(…)` literal at @p alpha (0..1), ready to drop into
 * a Qt stylesheet. The translucency is the point: the colour underneath shows
 * through, so the same accent reads right on a light theme and a dark one.
 */
inline QString accentRgba(double alpha)
{
    const QColor c = accentColour();
    return QStringLiteral("rgba(%1,%2,%3,%4)").arg(c.red()).arg(c.green()).arg(c.blue()).arg(alpha, 0, 'f', 2);
}

} // namespace ps
