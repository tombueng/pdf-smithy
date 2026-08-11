/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#include "ScanProcessor.h"

#include "PdfGeometry.h"
#include "PdfFile.h"

#include "RenderBackend.h"

#include <QAtomicInt>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QScopeGuard>
#include <QTemporaryFile>

#include <KLocalizedString>

#include <qpdf/QPDF.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>
#include <qpdf/QPDFWriter.hh>

#include <leptonica/allheaders.h>
#include <tesseract/baseapi.h>
#include <tesseract/resultiterator.h>

#include <cmath>
#include <cstdio>
#include <memory>

namespace ps {

namespace {

/** Private render-backend slot; Document only ever hands out ids from zero up. */
constexpr int ScratchSourceId = -4711;

/** Resource name for the invisible OCR font. Unlikely to collide. */
constexpr const char *OcrFontKey = "/PsOcrFont";

/** Below this the page is straight enough that correcting it only adds risk. */
constexpr double MinimumCorrectableSkew = 0.1;

/** Above this it is not skew any more, it is a page fed in sideways. */
constexpr double MaximumCorrectableSkew = 15.0;

// ── Leptonica helpers ─────────────────────────────────────────────────────

struct PixDeleter {
    void operator()(PIX *pix) const
    {
        if (pix) {
            pixDestroy(&pix);
        }
    }
};
using PixPtr = std::unique_ptr<PIX, PixDeleter>;

PixPtr toPix(const QImage &image)
{
    const QImage source = image.convertToFormat(QImage::Format_RGBA8888);
    if (source.isNull()) {
        return {};
    }

    PixPtr pix(pixCreate(source.width(), source.height(), 32));
    if (!pix) {
        return {};
    }
    pixSetSpp(pix.get(), 3);

    const int wpl = pixGetWpl(pix.get());
    l_uint32 *data = pixGetData(pix.get());

    for (int y = 0; y < source.height(); ++y) {
        const uchar *line = source.constScanLine(y);
        l_uint32 *row = data + static_cast<ptrdiff_t>(y) * wpl;
        for (int x = 0; x < source.width(); ++x) {
            composeRGBPixel(line[4 * x], line[4 * x + 1], line[4 * x + 2], &row[x]);
        }
    }
    return pix;
}

/**
 * Measures page skew in degrees.
 *
 * Positive means the page needs turning counter-clockwise to come straight,
 * matching the sign Leptonica's own deskew uses. Returns false when no
 * confident answer could be found, which is the normal outcome for a page that
 * is mostly picture.
 */
bool measureSkew(PIX *pix, double *angleOut)
{
    PixPtr gray(pixConvertRGBToGray(pix, 0.0f, 0.0f, 0.0f));
    if (!gray) {
        return false;
    }
    PixPtr binary(pixConvertTo1(gray.get(), 130));
    if (!binary) {
        return false;
    }

    l_float32 angle = 0.0f;
    l_float32 confidence = 0.0f;
    // pixFindSkew measures without also producing a corrected image, which is
    // what we want: the correction is applied as a transform, not as pixels.
    if (pixFindSkew(binary.get(), &angle, &confidence) != 0) {
        return false;
    }

    // Leptonica's own threshold for "this measurement means something".
    if (confidence < 3.0f) {
        return false;
    }
    *angleOut = static_cast<double>(angle);
    return true;
}

// ── PDF content stream helpers ────────────────────────────────────────────

std::string escapePdfString(const QString &text)
{
    // WinAnsiEncoding covers Latin-1, which is what German and English need.
    // Characters outside it are dropped rather than written as mojibake that
    // would corrupt a copy-and-paste.
    const QByteArray latin1 = text.toLatin1();

    std::string out;
    out.reserve(static_cast<size_t>(latin1.size()) + 8);
    for (const char rawByte : latin1) {
        const unsigned char byte = static_cast<unsigned char>(rawByte);
        if (byte == '(' || byte == ')' || byte == '\\') {
            out.push_back('\\');
            out.push_back(static_cast<char>(byte));
        } else if (byte < 32 || byte == 127) {
            continue;
        } else {
            out.push_back(static_cast<char>(byte));
        }
    }
    return out;
}

/**
 * Formats a PDF number.
 *
 * Deliberately *not* snprintf("%f") or std::to_string: both honour the C
 * locale, which Qt initialises from the environment. On a German system that
 * turns every coordinate into "12,3400" and produces a content stream no
 * viewer can parse. QByteArray::number always uses the C locale.
 */
std::string formatNumber(double value)
{
    return QByteArray::number(value, 'f', 4).toStdString();
}

/**
 * The matrix that maps displayed coordinates back into unrotated page space.
 *
 * Poppler hands out images with /Rotate already applied, so word boxes arrive
 * in display space while content streams are written in page space. Without
 * this, the text layer of any rotated scan lands somewhere off the page.
 */
std::string displayToPageMatrix(int rotate, double pageWidth, double pageHeight)
{
    switch (((rotate % 360) + 360) % 360) {
    case 90:
        return "0 1 -1 0 " + formatNumber(pageWidth) + " 0 cm\n";
    case 180:
        return "-1 0 0 -1 " + formatNumber(pageWidth) + " " + formatNumber(pageHeight) + " cm\n";
    case 270:
        return "0 -1 1 0 0 " + formatNumber(pageHeight) + " cm\n";
    default:
        return "1 0 0 1 0 0 cm\n";
    }
}

/** Rotation by @p degrees about the centre of a page, as a cm operator. */
std::string rotateAboutCentreMatrix(double degrees, double width, double height)
{
    const double radians = degrees * M_PI / 180.0;
    const double c = std::cos(radians);
    const double s = std::sin(radians);
    const double cx = width / 2.0;
    const double cy = height / 2.0;

    return formatNumber(c) + " " + formatNumber(s) + " " + formatNumber(-s) + " " + formatNumber(c) + " "
        + formatNumber(cx - cx * c + cy * s) + " " + formatNumber(cy - cx * s - cy * c) + " cm\n";
}

double boxNumber(QPDFObjectHandle box, int index, double fallback)
{
    if (!box.isArray() || box.getArrayNItems() != 4) {
        return fallback;
    }
    QPDFObjectHandle item = box.getArrayItem(index);
    return PdfGeometry::numericValue(item, fallback);
}

} // namespace

// ══════════════════════════════════════════════════════════════════════════

class ScanProcessor::Private
{
public:
    QAtomicInt cancelled { 0 };
};

ScanProcessor::ScanProcessor(QObject *parent)
    : QObject(parent)
    , d(std::make_unique<Private>())
{
}

ScanProcessor::~ScanProcessor() = default;

void ScanProcessor::cancel()
{
    d->cancelled.storeRelaxed(1);
}

bool ScanProcessor::isAvailable()
{
    return !availableLanguages().isEmpty();
}

QStringList ScanProcessor::availableLanguages()
{
    QStringList languages;

    tesseract::TessBaseAPI api;
    // Initialising needs *a* language; whichever exists will do, since we only
    // want the list afterwards.
    for (const char *probe : { "eng", "deu", "osd" }) {
        if (api.Init(nullptr, probe) == 0) {
            std::vector<std::string> found;
            api.GetAvailableLanguagesAsVector(&found);
            for (const std::string &code : found) {
                languages.append(QString::fromStdString(code));
            }
            api.End();
            break;
        }
    }

    languages.removeAll(QStringLiteral("osd")); // orientation data, not a language
    languages.sort();
    return languages;
}

QString ScanProcessor::languageName(const QString &code)
{
    static const QHash<QString, QString> names {
        { QStringLiteral("deu"), i18nc("@item language", "German") },
        { QStringLiteral("eng"), i18nc("@item language", "English") },
        { QStringLiteral("fra"), i18nc("@item language", "French") },
        { QStringLiteral("spa"), i18nc("@item language", "Spanish") },
        { QStringLiteral("ita"), i18nc("@item language", "Italian") },
        { QStringLiteral("nld"), i18nc("@item language", "Dutch") },
        { QStringLiteral("por"), i18nc("@item language", "Portuguese") },
        { QStringLiteral("pol"), i18nc("@item language", "Polish") },
        { QStringLiteral("rus"), i18nc("@item language", "Russian") },
        { QStringLiteral("tur"), i18nc("@item language", "Turkish") },
    };
    return names.value(code, code);
}

bool ScanProcessor::process(const QString &inputPath, const QString &outputPath, RenderBackend *backend,
                            const Options &options, Report *report, QString *error)
{
    d->cancelled.storeRelaxed(0);

    if (!backend) {
        if (error) {
            *error = i18n("No renderer is available for reading the pages.");
        }
        return false;
    }
    if (!options.recognizeText && !options.straighten) {
        if (error) {
            *error = i18n("Neither text recognition nor straightening was requested.");
        }
        return false;
    }

    Report local;

    // ── Tesseract ─────────────────────────────────────────────────────────
    tesseract::TessBaseAPI api;
    if (options.recognizeText) {
        const QByteArray languages = options.languages.join(QLatin1Char('+')).toUtf8();
        if (api.Init(nullptr, languages.constData(), tesseract::OEM_LSTM_ONLY) != 0) {
            if (error) {
                *error = i18n("Text recognition could not be started for “%1”. "
                              "Install the matching tesseract-ocr language package.",
                              options.languages.join(QStringLiteral(", ")));
            }
            return false;
        }
        api.SetPageSegMode(tesseract::PSM_AUTO);
    }

    if (!backend->addDocument(ScratchSourceId, inputPath, error)) {
        return false;
    }
    const auto releaseBackend = qScopeGuard([backend] { backend->removeDocument(ScratchSourceId); });

    QTemporaryFile temp(QFileInfo(outputPath).absolutePath() + QLatin1String("/.pdf-smithy-ocr-XXXXXX.pdf"));
    temp.setAutoRemove(false);
    if (!temp.open()) {
        if (error) {
            *error = i18n("Cannot write to “%1”.", QFileInfo(outputPath).absolutePath());
        }
        return false;
    }
    const QString tempPath = temp.fileName();
    temp.close();
    const auto cleanUp = [&tempPath] { QFile::remove(tempPath); };

    try {
        QPDF pdf;
        PdfFile::open(pdf, inputPath);
        std::vector<QPDFPageObjectHelper> pages = QPDFPageDocumentHelper(pdf).getAllPages();
        const int pageCount = static_cast<int>(pages.size());

        QPDFObjectHandle ocrFont;

        for (int index = 0; index < pageCount; ++index) {
            if (d->cancelled.loadRelaxed()) {
                cleanUp();
                if (error) {
                    *error = i18n("Cancelled.");
                }
                return false;
            }
            Q_EMIT progress(index + 1, pageCount);

            QPDFPageObjectHelper &page = pages[static_cast<size_t>(index)];

            // Pages that already carry real text are left alone: a second,
            // slightly different text layer is how searchable PDFs get ruined.
            if (options.skipPagesWithText && options.recognizeText) {
                const QString existing = backend->extractText(ScratchSourceId, index).trimmed();
                if (existing.size() > 40) {
                    ++local.pagesSkipped;
                    continue;
                }
            }

            const QPDFObjectHandle mediaBox = page.getMediaBox(true);
            const double boxLeft = boxNumber(mediaBox, 0, 0.0);
            const double boxBottom = boxNumber(mediaBox, 1, 0.0);
            const double pageWidth = boxNumber(mediaBox, 2, 612.0) - boxLeft;
            const double pageHeight = boxNumber(mediaBox, 3, 792.0) - boxBottom;
            if (pageWidth <= 0.0 || pageHeight <= 0.0) {
                continue;
            }

            QPDFObjectHandle rotateHandle = page.getAttribute("/Rotate", false);
            const int rotate = rotateHandle.isInteger() ? rotateHandle.getIntValueAsInt() : 0;
            const bool swapped = (((rotate % 360) + 360) % 360) == 90 || (((rotate % 360) + 360) % 360) == 270;
            const double visibleWidth = swapped ? pageHeight : pageWidth;
            const double visibleHeight = swapped ? pageWidth : pageHeight;

            const int targetPixels = static_cast<int>(std::lround(visibleWidth * options.dpi / 72.0));
            const QImage image = backend->renderPage(ScratchSourceId, index, targetPixels);
            if (image.isNull()) {
                continue;
            }

            PixPtr pix = toPix(image);
            if (!pix) {
                continue;
            }

            // ── Straightening ─────────────────────────────────────────────
            double skew = 0.0;
            const bool haveSkew = measureSkew(pix.get(), &skew);
            const bool correctable
                = haveSkew && std::abs(skew) >= MinimumCorrectableSkew && std::abs(skew) <= MaximumCorrectableSkew;
            const bool straightenPage = options.straighten && correctable;

            if (!options.recognizeText && !straightenPage) {
                ++local.pagesProcessed;
                continue;
            }

            // Recognition runs on a straightened copy whether or not the page
            // itself is being corrected, because crooked lines cost accuracy.
            // pixDeskew rather than a hand-rolled rotation: letting Leptonica
            // both measure and correct removes any chance of getting its sign
            // convention backwards.
            PixPtr forOcr;
            if (correctable) {
                forOcr.reset(pixDeskew(pix.get(), 4));
            }
            if (!forOcr) {
                forOcr.reset(pixClone(pix.get()));
            }

            // Uneven lighting first: despeckling a photograph with a dark
            // corner would otherwise throw away real letters along with dirt.
            if (options.evenOutLighting) {
                PixPtr gray(pixConvertRGBToGray(forOcr.get(), 0.0f, 0.0f, 0.0f));
                if (gray) {
                    PixPtr evened(pixBackgroundNormSimple(gray.get(), nullptr, nullptr));
                    if (evened) {
                        forOcr = std::move(evened);
                    }
                }
            }

            if (options.despeckle > 0) {
                PixPtr gray(pixConvertRGBToGray(forOcr.get(), 0.0f, 0.0f, 0.0f));
                PixPtr binary(gray ? pixConvertTo1(gray.get(), 130) : nullptr);
                if (binary) {
                    PixPtr cleaned(pixSelectBySize(binary.get(), options.despeckle, options.despeckle, 8,
                                                   L_SELECT_IF_EITHER, L_SELECT_IF_GTE, nullptr));
                    if (cleaned) {
                        forOcr = std::move(cleaned);
                    }
                }
            }

            int wordsOnPage = 0;
            std::string text;

            if (options.recognizeText) {
                api.SetImage(forOcr.get());
                api.SetSourceResolution(options.dpi);
                if (api.Recognize(nullptr) != 0) {
                    continue;
                }

                const int imageWidth = pixGetWidth(forOcr.get());
                const int imageHeight = pixGetHeight(forOcr.get());
                if (imageWidth <= 0 || imageHeight <= 0) {
                    continue;
                }
                const double scaleX = visibleWidth / imageWidth;
                const double scaleY = visibleHeight / imageHeight;

                text.reserve(4096);
                text += "q\n";
                text += displayToPageMatrix(rotate, pageWidth, pageHeight);
                // Render mode 3 is "draw nothing": the words are selectable,
                // searchable and copyable, but completely invisible.
                text += "BT\n3 Tr\n";

                std::unique_ptr<tesseract::ResultIterator> iterator(api.GetIterator());
                if (iterator) {
                    constexpr tesseract::PageIteratorLevel level = tesseract::RIL_WORD;
                    do {
                        if (iterator->Empty(level)) {
                            continue;
                        }
                        if (iterator->Confidence(level) < options.minimumConfidence) {
                            continue;
                        }

                        std::unique_ptr<char[]> word(iterator->GetUTF8Text(level));
                        if (!word) {
                            continue;
                        }
                        const QString wordText = QString::fromUtf8(word.get()).trimmed();
                        if (wordText.isEmpty()) {
                            continue;
                        }

                        int left = 0;
                        int top = 0;
                        int right = 0;
                        int bottom = 0;
                        if (!iterator->BoundingBox(level, &left, &top, &right, &bottom)) {
                            continue;
                        }
                        if (right <= left || bottom <= top) {
                            continue;
                        }

                        const double x = boxLeft + left * scaleX;
                        // Image coordinates run downwards, PDF coordinates upwards.
                        const double y = boxBottom + visibleHeight - bottom * scaleY;
                        const double boxWidth = (right - left) * scaleX;
                        const double fontSize = std::max(1.0, (bottom - top) * scaleY);

                        // Helvetica averages roughly half an em per character;
                        // stretching to the measured box keeps the selection
                        // rectangle on top of the word it belongs to.
                        const double naturalWidth = 0.5 * fontSize * wordText.size();
                        const double stretch
                            = naturalWidth > 0.0 ? std::clamp(100.0 * boxWidth / naturalWidth, 10.0, 1000.0) : 100.0;

                        text += std::string(OcrFontKey) + " " + formatNumber(fontSize) + " Tf\n";
                        text += formatNumber(stretch) + " Tz\n";
                        text += "1 0 0 1 " + formatNumber(x) + " " + formatNumber(y) + " Tm\n";
                        text += "(" + escapePdfString(wordText) + ") Tj\n";
                        ++wordsOnPage;
                    } while (iterator->Next(level));
                }

                text += "ET\nQ\n";
            } // options.recognizeText

            // Whatever the page contents did to the graphics state, they did it
            // without cleaning up: Ghostscript's scanner output, for instance,
            // leaves a 612x792 scaling matrix in force. Since `cm` concatenates
            // rather than replaces, anything appended afterwards inherits it.
            // Wrapping the original content in q/Q hands us the identity matrix
            // we assumed we had, and doubles as the place to hang the skew
            // correction.
            if (straightenPage || wordsOnPage > 0) {
                std::string prefix = "q\n";
                if (straightenPage) {
                    // Negated: the measured angle describes how the page is
                    // tilted, so correcting it means turning the other way.
                    prefix += rotateAboutCentreMatrix(-skew, pageWidth, pageHeight);
                }
                page.addPageContents(QPDFObjectHandle::newStream(&pdf, prefix), true);
                page.addPageContents(QPDFObjectHandle::newStream(&pdf, std::string("Q\n")), false);

                if (straightenPage) {
                    ++local.pagesStraightened;
                    local.largestSkewAngle = std::max(local.largestSkewAngle, std::abs(skew));
                }
            }

            if (wordsOnPage > 0) {
                if (!ocrFont.isInitialized() || ocrFont.isNull()) {
                    ocrFont = pdf.makeIndirectObject(QPDFObjectHandle::parse(
                        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>"));
                }

                QPDFObjectHandle resources = page.getAttribute("/Resources", true);
                if (!resources.isDictionary()) {
                    resources = QPDFObjectHandle::newDictionary();
                    page.getObjectHandle().replaceKey("/Resources", resources);
                }
                QPDFObjectHandle fonts = resources.getKey("/Font");
                if (!fonts.isDictionary()) {
                    fonts = QPDFObjectHandle::newDictionary();
                    resources.replaceKey("/Font", fonts);
                }
                fonts.replaceKey(OcrFontKey, ocrFont);

                page.addPageContents(QPDFObjectHandle::newStream(&pdf, text), false);
                local.wordsRecognized += wordsOnPage;
            }

            ++local.pagesProcessed;
        }

        QPDFWriter writer(pdf);
        writer.setOutputFilename(QFile::encodeName(tempPath).constData());
        writer.setObjectStreamMode(qpdf_o_generate);
        writer.setCompressStreams(true);
        writer.setDeterministicID(true);
        writer.write();
    } catch (const std::exception &e) {
        cleanUp();
        if (error) {
            *error = QString::fromUtf8(e.what());
        }
        return false;
    }

    if (options.recognizeText) {
        api.End();
    }

    QFile::setPermissions(tempPath, QFile::ReadOwner | QFile::WriteOwner | QFile::ReadGroup | QFile::ReadOther);
    if (::rename(QFile::encodeName(tempPath).constData(), QFile::encodeName(outputPath).constData()) != 0) {
        cleanUp();
        if (error) {
            *error = i18n("Could not write “%1”.", outputPath);
        }
        return false;
    }

    if (report) {
        *report = local;
    }
    return true;
}

} // namespace ps
