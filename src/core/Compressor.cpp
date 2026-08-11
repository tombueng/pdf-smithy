/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#include "Compressor.h"
#include "PdfFile.h"

#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryFile>

#include <KLocalizedString>

#include <algorithm>
#include <cstdio>

#include <qpdf/QPDF.hh>
#include <qpdf/QPDFWriter.hh>

namespace ps {

namespace {

constexpr int GhostscriptTimeoutMs = 15 * 60 * 1000;

/** Rewrites structure only: object streams, flate, duplicate objects merged. */
bool compressLossless(const QString &input, const QString &output, const Compressor::Options &options, QString *error)
{
    try {
        QPDF pdf;
        PdfFile::open(pdf, input);

        QPDFWriter writer(pdf);
        writer.setOutputFilename(QFile::encodeName(output).constData());
        writer.setObjectStreamMode(qpdf_o_generate);
        writer.setCompressStreams(true);
        // Re-deflating already-deflated streams is off by default because it
        // is slow; here it is exactly the point.
        writer.setRecompressFlate(true);
        writer.setDecodeLevel(qpdf_dl_generalized);
        writer.setLinearization(options.linearize);
        writer.setDeterministicID(true);
        writer.write();
    } catch (const std::exception &e) {
        if (error) {
            *error = QString::fromUtf8(e.what());
        }
        return false;
    }
    return true;
}

QStringList ghostscriptArguments(const QString &output, const Compressor::Options &options)
{
    const int dpi = options.imageDpi > 0 ? options.imageDpi : Compressor::dpiForLevel(options.level);
    // Line art needs far more resolution than photographs before it looks
    // ragged, so monochrome images are downsampled much more gently.
    const int monoDpi = std::max(300, dpi * 2);

    QStringList arguments {
        QStringLiteral("-sDEVICE=pdfwrite"),
        QStringLiteral("-dCompatibilityLevel=1.7"),
        QStringLiteral("-dNOPAUSE"),
        QStringLiteral("-dBATCH"),
        QStringLiteral("-dQUIET"),
        QStringLiteral("-dSAFER"),
        QStringLiteral("-dDetectDuplicateImages=true"),
        QStringLiteral("-dCompressFonts=true"),
        QStringLiteral("-dSubsetFonts=true"),

        QStringLiteral("-dDownsampleColorImages=true"),
        QStringLiteral("-dColorImageDownsampleType=/Bicubic"),
        QStringLiteral("-dColorImageResolution=%1").arg(dpi),

        QStringLiteral("-dDownsampleGrayImages=true"),
        QStringLiteral("-dGrayImageDownsampleType=/Bicubic"),
        QStringLiteral("-dGrayImageResolution=%1").arg(dpi),

        QStringLiteral("-dDownsampleMonoImages=true"),
        QStringLiteral("-dMonoImageDownsampleType=/Subsample"),
        QStringLiteral("-dMonoImageResolution=%1").arg(monoDpi),

        QStringLiteral("-dJPEGQ=%1").arg(std::clamp(options.jpegQuality, 1, 100)),
    };

    if (options.grayscale) {
        arguments << QStringLiteral("-sColorConversionStrategy=Gray")
                  << QStringLiteral("-dProcessColorModel=/DeviceGray") << QStringLiteral("-dOverrideICC=true");
    }
    if (options.linearize) {
        arguments << QStringLiteral("-dFastWebView=true");
    }

    arguments << QStringLiteral("-sOutputFile=%1").arg(output);
    return arguments;
}

} // namespace

QString Compressor::ghostscriptPath()
{
    return QStandardPaths::findExecutable(QStringLiteral("gs"));
}

bool Compressor::isGhostscriptAvailable()
{
    return !ghostscriptPath().isEmpty();
}

int Compressor::dpiForLevel(Level level)
{
    switch (level) {
    case Level::Lossless:
        return 0;
    case Level::Balanced:
        return 150;
    case Level::Strong:
        return 110;
    case Level::Extreme:
        return 72;
    }
    return 150;
}

QString Compressor::levelName(Level level)
{
    switch (level) {
    case Level::Lossless:
        return i18nc("@item compression level", "Lossless (nothing is re-encoded)");
    case Level::Balanced:
        return i18nc("@item compression level", "Balanced (good for reading and printing)");
    case Level::Strong:
        return i18nc("@item compression level", "Strong (good for email)");
    case Level::Extreme:
        return i18nc("@item compression level", "Smallest (visibly softer images)");
    }
    return {};
}

bool Compressor::estimate(const QString &inputPath, const QVector<int> &pageIndexes,
                          const std::function<QString(int)> &sampleExtractor, const Options &options, Estimate *result,
                          QString *error)
{
    if (!result || pageIndexes.isEmpty() || !sampleExtractor) {
        return false;
    }

    Estimate local;
    local.originalBytes = QFileInfo(inputPath).size();

    // Spread across the document rather than taking the first few: the cover
    // page of a scan is rarely representative of what is inside it.
    QVector<int> sample;
    const int wanted = std::min(4, static_cast<int>(pageIndexes.size()));
    for (int i = 0; i < wanted; ++i) {
        sample.append(pageIndexes.at(i * pageIndexes.size() / wanted));
    }

    qint64 before = 0;
    qint64 after = 0;

    for (const int page : sample) {
        const QString single = sampleExtractor(page);
        if (single.isEmpty()) {
            continue;
        }
        const qint64 originalSize = QFileInfo(single).size();
        const QString smaller = single + QStringLiteral(".small");

        Report report;
        if (!compress(single, smaller, options, &report, error)) {
            QFile::remove(single);
            return false;
        }

        before += originalSize;
        after += QFileInfo(smaller).size();
        ++local.pagesSampled;

        QFile::remove(single);
        QFile::remove(smaller);
    }

    if (local.pagesSampled == 0 || before <= 0) {
        if (error) {
            *error = i18n("The sample pages could not be measured.");
        }
        return false;
    }

    const double ratio = static_cast<double>(after) / before;
    local.expectedBytes = static_cast<qint64>(local.originalBytes * ratio);
    // Four pages out of four hundred is a hint, not a measurement; say so.
    local.reliable = local.pagesSampled >= 3;

    *result = local;
    return true;
}

bool Compressor::compress(const QString &inputPath, const QString &outputPath, const Options &options, Report *report,
                          QString *error)
{
    const QFileInfo inputInfo(inputPath);
    if (!inputInfo.exists()) {
        if (error) {
            *error = i18n("“%1” does not exist.", inputPath);
        }
        return false;
    }

    Report local;
    local.originalBytes = inputInfo.size();

    // Work into a temporary next to the destination so a failure never leaves a
    // half-written file where the user expects their document.
    QTemporaryFile temp(QFileInfo(outputPath).absolutePath() + QLatin1String("/.pdf-smithy-zip-XXXXXX.pdf"));
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

    if (options.level == Level::Lossless) {
        if (!compressLossless(inputPath, tempPath, options, error)) {
            cleanUp();
            return false;
        }
    } else {
        const QString gs = ghostscriptPath();
        if (gs.isEmpty()) {
            cleanUp();
            if (error) {
                *error = i18n("Ghostscript is not installed, so images cannot be downsampled. "
                              "Install the “ghostscript” package, or use lossless compression.");
            }
            return false;
        }

        QProcess process;
        process.start(gs, ghostscriptArguments(tempPath, options) << inputPath);
        if (!process.waitForStarted(10000)) {
            cleanUp();
            if (error) {
                *error = i18n("Ghostscript could not be started.");
            }
            return false;
        }
        if (!process.waitForFinished(GhostscriptTimeoutMs)) {
            process.kill();
            process.waitForFinished(2000);
            cleanUp();
            if (error) {
                *error = i18n("Compression took too long and was stopped.");
            }
            return false;
        }
        if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
            const QString details = QString::fromLocal8Bit(process.readAllStandardError()).trimmed();
            cleanUp();
            if (error) {
                *error = details.isEmpty() ? i18n("Ghostscript reported an error.") : details;
            }
            return false;
        }
        local.usedGhostscript = true;
    }

    const qint64 producedSize = QFileInfo(tempPath).size();
    if (producedSize <= 0) {
        cleanUp();
        if (error) {
            *error = i18n("Compression produced an empty file.");
        }
        return false;
    }

    // Compression that makes the file bigger is worse than useless. Hand back
    // the original instead and let the report show a saving of zero.
    if (producedSize >= local.originalBytes && inputPath != outputPath) {
        cleanUp();
        QFile::remove(outputPath);
        if (!QFile::copy(inputPath, outputPath)) {
            if (error) {
                *error = i18n("Could not write “%1”.", outputPath);
            }
            return false;
        }
        local.compressedBytes = local.originalBytes;
        local.outcome = Outcome::AlreadyOptimal;
        if (report) {
            *report = local;
        }
        return true;
    }

    QFile::setPermissions(tempPath, QFile::ReadOwner | QFile::WriteOwner | QFile::ReadGroup | QFile::ReadOther);

    if (::rename(QFile::encodeName(tempPath).constData(), QFile::encodeName(outputPath).constData()) != 0) {
        cleanUp();
        if (error) {
            *error = i18n("Could not replace “%1”.", outputPath);
        }
        return false;
    }

    local.compressedBytes = QFileInfo(outputPath).size();
    // A saving of a few kilobytes on a twelve-megabyte scan is not a result,
    // it is rounding. Saying so plainly is more useful than printing "0 %".
    if (local.savedPercent() < MeaningfulSavingPercent) {
        local.outcome = Outcome::AlreadyOptimal;
    }
    if (report) {
        *report = local;
    }
    return true;
}

} // namespace ps
