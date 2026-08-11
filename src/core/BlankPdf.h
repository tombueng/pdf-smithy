/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#pragma once

#include <QSizeF>
#include <QString>

namespace ps {

/**
 * Empty paper, written as a PDF file.
 *
 * Everything the editor does starts from a file: a Document holds Sources, and
 * a page is a reference into one of them. So "start a new document" and "add a
 * blank page" are not special cases in the page list; they are ordinary files
 * that happen to have nothing drawn on them. That is what makes them undoable,
 * saveable and mergeable for free, and it is why this writes a file rather than
 * synthesising a page inside Document.
 *
 * The size is given in points and taken as the reader sees it: a landscape
 * sheet is a wide size, not a tall one that has been turned.
 */
class BlankPdf
{
public:
    /** A4 upright, which is what "a sheet of paper" means nearly everywhere. */
    static QSizeF defaultSize() { return QSizeF(595.276, 841.89); }

    /**
     * Writes @p pageCount empty pages of @p sizePoints to @p path.
     *
     * Each page carries a content stream rather than none at all. An empty
     * stream costs a few bytes and spares every reader downstream a null check,
     * including this program's own, which reads a page's operators to find
     * out what it draws.
     */
    static bool write(const QString &path, int pageCount, const QSizeF &sizePoints, QString *error);
};

} // namespace ps
