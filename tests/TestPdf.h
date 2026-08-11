/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#pragma once

#include <QMap>
#include <QSizeF>
#include <QString>

namespace ps::test {

/**
 * Builds a valid PDF at @p path with @p pageCount pages, each stamped with its
 * own number.
 *
 * Fixtures are generated rather than committed: the suite stays deterministic,
 * the repository stays free of third-party documents, and a test that needs a
 * 2000-page file can simply ask for one.
 */
bool writeSamplePdf(const QString &path, int pageCount, const QSizeF &sizePoints = QSizeF(612, 792));

/** Same, but every page carries the given /Rotate value. */
bool writeRotatedPdf(const QString &path, int pageCount, int rotate);

/**
 * A page densely filled with text lines, which is what skew detection needs to
 * work with: a single headline gives it nothing to measure.
 */
bool writeTextHeavyPdf(const QString &path, int pageCount, double skewDegrees = 0.0);

/**
 * Rasterises @p input into an image-only PDF via Ghostscript, i.e. a
 * stand-in for a scan. Returns false when Ghostscript is unavailable.
 */
bool rasterizePdf(const QString &input, const QString &output, int dpi = 200);

/** True when Ghostscript is installed. */
bool haveGhostscript();

/**
 * A document with a top-level outline.
 *
 * @p chapters maps a zero-based page index to the bookmark title that points
 * at it. Needed to test splitting on bookmarks, which cannot be checked with a
 * document that has none.
 */
bool writeBookmarkedPdf(const QString &path, int pageCount, const QMap<int, QString> &chapters);

/** Deliberately malformed bytes; the application must survive these. */
bool writeBrokenPdf(const QString &path);

/** Page count of @p path according to QPDF, or -1 when unreadable. */
int pageCountOf(const QString &path);

/** The /Rotate value of one page, or -1 when unreadable. */
int rotationOf(const QString &path, int page);

/** Extracted text of one page's content stream, for identifying pages. */
QString contentOf(const QString &path, int page);

/**
 * Puts a form field on a page.
 *
 * Form fields are annotations too, which is exactly why anything that removes
 * annotations has to be careful. Tests need one to prove they are.
 */
bool addWidgetAnnotation(const QString &input, const QString &output, int page);

/** Whether that field is still there. */
bool hasWidgetAnnotation(const QString &path, int page);

/**
 * A document whose first page carries a link to @p target.
 *
 * Internal links are the other half of navigation, and the interesting case is
 * what happens to one when its destination page is deleted.
 */
bool writeLinkedPdf(const QString &path, int pageCount, int target);

/** How many /Link annotations survive in @p path, and where the first one points. */
int countLinks(const QString &path, int *firstTargetPage);

/**
 * A one-page form with the awkward cases in it.
 *
 * A required text field, a multi-line one, a tick box whose "on" state is /Ja
 * rather than /Yes, a drop-down, and a field the form locks. Every one of those
 * has caught a bug here.
 */
bool writeFormPdf(const QString &path);

} // namespace ps::test
