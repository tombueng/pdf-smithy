/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

namespace ps {

class RenderBackend;

/**
 * Turns scans into documents: text recognition and straightening.
 *
 * The guiding rule is that **the page itself is never re-rendered**. A scanned
 * contract that goes in at 600 dpi comes out with the very same image bytes.
 *
 * - **Text recognition** rasterises each page only to feed Tesseract, then
 *   writes the recognised words back as an *invisible* text layer (render mode
 *   3) positioned over the words in the picture. The page looks identical and
 *   suddenly selects, copies, searches and indexes.
 * - **Straightening** measures the skew with Leptonica and corrects it with a
 *   transformation matrix wrapped around the existing content stream. No
 *   resampling, so nothing softens.
 *
 * Tesseract and Leptonica are linked directly, both permissively licensed, so
 * this stays inside the MIT core.
 */
class ScanProcessor : public QObject
{
    Q_OBJECT

public:
    struct Options {
        /** Tesseract language codes, e.g. {"deu", "eng"}. */
        QStringList languages { QStringLiteral("deu"), QStringLiteral("eng") };

        /** Resolution the page is rasterised at for recognition only. */
        int dpi = 300;

        /** Add the invisible text layer. */
        bool recognizeText = true;

        /** Correct page skew in the output. */
        bool straighten = true;

        /**
         * Skip pages that already contain real text. Running OCR over a
         * digitally generated page adds a second, slightly wrong text layer,
         * the classic way to ruin a searchable document.
         */
        bool skipPagesWithText = true;

        /** Ignore words Tesseract is less sure about than this, in percent. */
        int minimumConfidence = 40;

        /**
         * Discard specks smaller than this many pixels before recognition.
         *
         * Applied to the image Tesseract sees, never to the page itself:
         * cleaning up the output would mean re-encoding it, and this project
         * does not re-encode pages.
         */
        int despeckle = 0;

        /**
         * Even out uneven lighting before recognition.
         *
         * A page photographed rather than scanned is bright in the middle and
         * dim at the edges; Tesseract reads the dim part far worse. Like
         * despeckling, this touches only the recognition image.
         */
        bool evenOutLighting = false;
    };

    struct Report {
        int pagesProcessed = 0;
        int pagesSkipped = 0;
        int wordsRecognized = 0;
        int pagesStraightened = 0;
        double largestSkewAngle = 0.0;
    };

    explicit ScanProcessor(QObject *parent = nullptr);
    ~ScanProcessor() override;

    /** False when the build has no OCR support compiled in. */
    static bool isAvailable();

    /** Language codes Tesseract has data files for, e.g. {"deu", "eng", "osd"}. */
    static QStringList availableLanguages();

    /** Human-readable name for a Tesseract language code. */
    static QString languageName(const QString &code);

    /**
     * Reads @p inputPath, processes every page, writes @p outputPath.
     *
     * @p backend supplies the page images and must outlive the call. Progress
     * is reported per page; cancel() makes the run stop at the next page
     * boundary and leaves the output file untouched.
     */
    bool process(const QString &inputPath, const QString &outputPath, RenderBackend *backend, const Options &options,
                 Report *report, QString *error);

public Q_SLOTS:
    void cancel();

Q_SIGNALS:
    void progress(int page, int pageCount);

private:
    class Private;
    std::unique_ptr<Private> d;
};

} // namespace ps
