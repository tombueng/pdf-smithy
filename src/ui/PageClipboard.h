/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#pragma once

#include "core/PageObjects.h"

#include <QSizeF>
#include <QString>
#include <QStringList>
#include <QVector>

class QMimeData;

namespace ps {

/**
 * A piece of a page on its way to and from the clipboard.
 *
 * ## Why a copy is a description rather than a duplicate
 *
 * PageComposer rewrites the instruction stream a page already has; it cannot be
 * asked to draw one of those instructions a second time somewhere else. So a
 * copied object cannot travel as "object 14 of page 3": the only thing that can
 * be pasted is NewContent, which the composer knows how to write from nothing.
 * Everything here is therefore a **reconstruction**: the words of a line, the
 * pixels of a picture, the corners of a rectangle.
 *
 * ## What could not be reconstructed travels too
 *
 * A vector drawing or a form XObject has no NewContent that stands for it, and
 * the honest answer is to say so, but at the paste, where the user is looking
 * for the thing to appear, rather than at the copy, where nothing has visibly
 * gone wrong yet. Parcel::missing carries those sentences across the clipboard
 * for exactly that reason.
 *
 * ## Two ends of the same trip
 *
 * The private type is a byte stream with its own magic and version, because the
 * two ends of a clipboard round trip are two processes that need not have been
 * started from the same build. A `text/plain` twin always goes alongside it, and
 * a lone picture goes as `image/png` as well, so that what is copied out of a
 * page is worth something to every other program on the machine.
 */
namespace PageClipboard {

/** The private type, namespaced so that only this program answers to it. */
QString objectsMimeType();

/** One copy: what can be made again, and what cannot. */
struct Parcel {
    /**
     * Ready for PageComposer, except for NewContent::page, which the paste sets:
     * a copy belongs to whichever page it is being put on, not to the one it
     * came from.
     */
    QVector<NewContent> items;

    /** One sentence each, naming what the copy could not carry and why. */
    QStringList missing;

    bool isEmpty() const { return items.isEmpty() && missing.isEmpty(); }
};

/** Everything @p parcel needs to survive the trip, with its plain-text twin. */
QMimeData *pack(const Parcel &parcel);

bool holdsObjects(const QMimeData *data);

/** The private type, read back. An empty parcel when @p data does not carry it. */
Parcel unpack(const QMimeData *data);

/**
 * A picture or plain text from another program, as content a page could take.
 *
 * @p pageSizePoints is what the arriving content is sized and placed against: a
 * picture larger than the paper is fitted to it rather than pasted mostly off
 * the edge, which is how a screenshot usually arrives.
 */
Parcel fromForeign(const QMimeData *data, const QSizeF &pageSizePoints);

/** What stands for @p parcel where only text can go. */
QString asPlainText(const Parcel &parcel);

} // namespace PageClipboard

} // namespace ps
