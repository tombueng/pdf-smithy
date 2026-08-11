/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#pragma once

#include <QString>
#include <QVector>

namespace ps {

/**
 * Parses and formats the page-range notation people already know from print
 * dialogs: "1-4, 8, 12-", "last", "odd", "even", "3-1" for reverse order.
 *
 * Shared by the GUI and the command line so that both accept exactly the same
 * syntax, because a documented format is worth more than two convenient ones.
 */
namespace PageRange {

/**
 * Turns @p text into zero-based page indexes against a document of
 * @p pageCount pages.
 *
 * Order and repetition are preserved: "3,1,3" yields {2, 0, 2}, because
 * "extract pages 3, 1 and 3 again" is a legitimate request. Returns an empty
 * vector and sets @p error when the text cannot be understood; an empty input
 * means "all pages".
 */
QVector<int> parse(const QString &text, int pageCount, QString *error = nullptr);

/** Renders indexes back to the compact notation, e.g. {0,1,2,5} to "1-3, 6". */
QString format(const QVector<int> &indexes);

/**
 * The words for the odd pages, the even pages and the final page in the
 * language the program is running in.
 *
 * Captions that invite people to type a keyword must take it from here rather
 * than spell it out, because then the caption and the parser cannot drift
 * apart: a translated word is only ever promised where it is also accepted.
 * The English words are accepted in every language whatever these return, so
 * the handbook and existing scripts keep working.
 */
QString oddWord();
QString evenWord();
QString lastWord();

/** True when @p text is syntactically valid for a document of that length. */
bool isValid(const QString &text, int pageCount);

} // namespace PageRange

} // namespace ps
