/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#pragma once

#include <QDateTime>
#include <QString>

class QPDF;

namespace ps {

/**
 * Opening documents without shouting about them.
 *
 * QPDF is admirably tolerant of malformed files (it recovers and carries on),
 * but it says so on stderr, once per object. A scan with an old generator's
 * habit of writing a bare carriage return after `stream` produces a warning per
 * page, and a 150-page book buries the actual result under 150 lines of noise
 * about something nobody can act on.
 *
 * Suppressing them and offering a single sentence instead is the honest middle:
 * the user still learns the file is not quite right, without being made to
 * scroll past the evidence.
 */
namespace PdfFile {

/** Opens @p path into @p pdf with per-object warnings held back. */
void open(QPDF &pdf, const QString &path);

/** The same, for a document that needs a password to open. */
void open(QPDF &pdf, const QString &path, const QString &password);

/** One sentence about what QPDF had to work around, or empty when it was clean. */
QString warningSummary(QPDF &pdf);

/**
 * A PDF date string, which looks like `D:20260728103000+02'00'`.
 *
 * Shared because metadata, annotations and anything else that stamps a time
 * have to agree on the format; two nearly-identical parsers is how one of them
 * quietly starts producing something no reader accepts.
 */
QString formatDate(const QDateTime &stamp);

/** The reverse, tolerant of the shortened forms real documents contain. */
QDateTime parseDate(const QString &raw);

} // namespace PdfFile

} // namespace ps
