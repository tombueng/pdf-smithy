/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#pragma once

#include <QImage>

class QPDF;
class QPDFObjectHandle;

namespace ps {

/**
 * Turning a QImage into something a PDF can draw.
 *
 * Shared by the overlay engine and by image import, which would otherwise each
 * grow their own copy of the same fiddly business with soft masks.
 */
namespace PdfImage {

/**
 * Embeds @p image as an image XObject, with an /SMask when it has alpha.
 *
 * Pixel data goes in uncompressed and QPDFWriter deflates it on the way out,
 * which keeps this free of any codec knowledge. Returns a null handle when the
 * image cannot be converted.
 */
QPDFObjectHandle embed(QPDF &pdf, const QImage &image);

/**
 * Embeds @p image as a JPEG, passing the compressed bytes straight through.
 *
 * Worth it for photographs and scans: re-deflating already-lossy pixels makes
 * a much larger file than simply carrying the JPEG across. Falls back to
 * embed() when the image has transparency, which JPEG cannot carry.
 */
QPDFObjectHandle embedAsJpeg(QPDF &pdf, const QImage &image, int quality);

} // namespace PdfImage

} // namespace ps
