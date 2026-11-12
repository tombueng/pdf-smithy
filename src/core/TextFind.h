/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#pragma once

#include <QString>
#include <QVector>

#include <Qt>

namespace ps {

/**
 * Looking for a word across a whole document.
 *
 * The text a page draws is read the same way editing reads it — out of the
 * content stream, with QPDF — so a search finds exactly what the editor could
 * put a cursor in. A scanned page carries no such text and simply yields no
 * matches; recognising it first is a separate step.
 *
 * It answers which pages a term is on and how often, with a little of the
 * surrounding text, which is what a result list is built from.
 */
namespace TextFind {

/** One page a term was found on. */
struct Hit {
    int page = 0; //!< zero-based
    int count = 0; //!< how many times the term appears on it
    QString snippet; //!< a little text around the first match, for a result list
};

/**
 * Every page of @p pdf that holds @p term, in page order.
 *
 * Case is folded unless @p cs is Qt::CaseSensitive. An empty @p term matches
 * nothing. On a document that will not open, the result is empty and @p error,
 * when not null, says why.
 */
QVector<Hit> search(const QString &pdf, const QString &term, Qt::CaseSensitivity cs, QString *error);

} // namespace TextFind

} // namespace ps
