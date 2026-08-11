/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#pragma once

#include <QRectF>
#include <QTransform>

#include <string>

class QPDFObjectHandle;
class QPDFPageObjectHelper;

namespace ps {

/**
 * The small pile of arithmetic that every "draw something onto a page"
 * operation needs, in one place.
 *
 * Two traps live here, and both have already cost a day between them:
 *
 * 1. **Numbers must not follow the locale.** snprintf("%f") and
 *    std::to_string honour LC_NUMERIC, which Qt initialises from the
 *    environment, so on a German system they emit "12,3400" and produce a
 *    content stream no viewer can read.
 * 2. **The graphics state leaks.** `cm` concatenates rather than replaces, and
 *    page content routinely ends without restoring what it changed. Anything
 *    appended afterwards inherits it, so existing content has to be wrapped in
 *    q/Q before anything new is drawn on top.
 */
namespace PdfGeometry {

/** A PDF number, always with a full stop, whatever the user's locale. */
std::string number(double value);

/**
 * A number out of the file, read without consulting the locale.
 *
 * The mirror of number(), and needed for the same reason. QPDF's
 * getNumericValue() goes through strtod, which honours LC_NUMERIC: on a German
 * system every fractional number in every PDF reads back as zero, because
 * parsing stops at the full stop. An A4 page becomes 595 × 841 instead of
 * 595.276 × 841.89, a colour of [1 0.83 0] becomes pure red, and a form's
 * matrix becomes the identity. Nothing warns; it simply reads wrong.
 *
 * Taking the real's own text and handing it to QByteArray::toDouble, which is
 * always C-locale, sidesteps the whole question.
 */
double numericValue(QPDFObjectHandle item, double fallback);

/**
 * Wraps a page's existing content in q/Q so that appended streams start from
 * the identity matrix, optionally prepending @p extraPrefix inside the q.
 *
 * Call this once per page before appending anything.
 */
void isolateExistingContent(QPDFPageObjectHelper &page, QPDFObjectHandle document, const std::string &extraPrefix = {});

/**
 * The matrix mapping displayed coordinates back into unrotated page space.
 *
 * Renderers hand out images with /Rotate already applied, so anything measured
 * on a rendered page arrives in display space while content streams are written
 * in page space.
 */
std::string displayToPageMatrix(int rotate, double pageWidth, double pageHeight);

/**
 * The same mapping as a transform, for measuring rather than drawing.
 *
 * @p pageWidth and @p pageHeight are the *unrotated* page dimensions, as they
 * are for displayToPageMatrix().
 */
QTransform displayToPageTransform(int rotate, double pageWidth, double pageHeight);

/** Rotation by @p degrees about the centre of a page, as a `cm` operator. */
std::string rotateAboutCentre(double degrees, double width, double height);

/**
 * Maps the unit square onto @p target, turned by @p rotationDegrees about its
 * own centre. Form XObjects and images are drawn in [0,1]², so this is how they
 * get somewhere useful.
 */
std::string placementMatrix(const QRectF &target, double rotationDegrees);

/**
 * Reads a box entry, falling back when it is missing or malformed.
 *
 * Deliberately insists on exactly four entries: a /Rect or /MediaBox with any
 * other count is broken, and reading it anyway would put content somewhere
 * arbitrary. For arrays that are not boxes (colours, quad points, ink paths),
 * use numberAt(), which has no such expectation.
 */
double boxValue(QPDFObjectHandle box, int index, double fallback);

/** Reads one number out of an array of any length. */
double numberAt(QPDFObjectHandle array, int index, double fallback);

/** Page size in points, ignoring /Rotate. */
QRectF mediaBoxOf(QPDFPageObjectHelper &page);

/** The /Rotate of a page, normalised to 0/90/180/270. */
int rotationOf(QPDFPageObjectHelper &page);

/**
 * Encodes @p text for a PDF string in WinAnsi, escaping what has to be escaped.
 *
 * WinAnsi rather than Latin-1, and the difference is not academic: the en dash
 * a German watermark sets between two words, the euro sign, and the curly
 * quotes every word processor produces all live in 0x80 to 0x9F, which Latin-1
 * leaves empty. Qt's toLatin1() turns each of them into a question mark, so a
 * watermark that read “Entwurf” and “nicht freigegeben” came out with a
 * question mark where the dash between them should have been.
 *
 * Lives here rather than in one of the callers because three of them need it
 * (printer's marks, watermarks and signature stamps) and two of them had their
 * own version, only one of which was right.
 *
 * Anything WinAnsi genuinely cannot hold does become a question mark: a visibly
 * wrong character is better than a silently shortened line.
 *
 * @returns the escaped bytes, without the surrounding parentheses.
 */
std::string winAnsi(const QString &text);

/** The same, wrapped in the parentheses of a PDF literal string. */
std::string winAnsiLiteral(const QString &text);

} // namespace PdfGeometry

} // namespace ps
