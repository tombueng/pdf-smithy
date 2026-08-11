/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later

    The command-line companion. Same engine as the window, no window.

    Everything the graphical application can do to a document, a script must be
    able to do too; that is a design rule of this project, not a bonus. The
    commands here are thin: they parse arguments and hand straight over to
    ps_core.
*/
#include "config.h"

#include "Commands.h"

#include "core/Annotation.h"
#include "core/Archival.h"
#include "core/Outline.h"
#include "core/Compare.h"
#include "core/Compressor.h"
#include "core/Document.h"
#include "core/DocumentWriter.h"
#include "core/Encryption.h"
#include "core/Forms.h"
#include "core/ImageIO.h"
#include "core/Metadata.h"
#include "core/Overlay.h"
#include "core/PageCrop.h"
#include "core/PageNumbering.h"
#include "core/NUp.h"
#include "core/PageRange.h"
#include "core/Source.h"
#include "core/Redaction.h"
#include "core/Splitter.h"
#include "core/TextEdit.h"
#include "render/PopplerBackend.h"

#ifdef PS_WITH_OCR
#include "core/ScanProcessor.h"
#endif

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QColor>
#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QJsonValue>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QTextStream>

#include <algorithm>
#include <clocale>

#include <KAboutData>
#include <KLocalizedString>

namespace {

// out(), err(), fail(), humanSize() and the exit codes now live in Common.cpp,
// so that a command group in its own translation unit reports failure exactly
// the way the commands here do.
using namespace ps::cli;

/** Loads @p paths into one document, appending in the order given. */
bool loadAll(ps::Document &document, const QStringList &paths, QString *error)
{
    for (const QString &path : paths) {
        if (!QFileInfo::exists(path)) {
            *error = i18n("“%1” does not exist.", path);
            return false;
        }
        if (!document.importFile(path, -1, error)) {
            return false;
        }
    }
    return true;
}

// ── Commands ──────────────────────────────────────────────────────────────

int commandInfo(const QStringList &arguments, bool asJson)
{
    if (arguments.isEmpty()) {
        return fail(i18n("info needs a file."), UsageError);
    }

    ps::PopplerBackend backend;
    ps::Document document;
    document.setRenderBackend(&backend);

    QString error;
    if (!document.open(arguments.constFirst(), &error)) {
        return fail(error);
    }

    const QFileInfo info(arguments.constFirst());

    if (asJson) {
        QJsonArray pages;
        for (int i = 0; i < document.pageCount(); ++i) {
            const QSizeF size = document.pageSizePoints(i);
            pages.append(QJsonObject { { QStringLiteral("number"), i + 1 },
                                       { QStringLiteral("widthPt"), size.width() },
                                       { QStringLiteral("heightPt"), size.height() } });
        }
        const QJsonObject root { { QStringLiteral("file"), info.absoluteFilePath() },
                                 { QStringLiteral("bytes"), info.size() },
                                 { QStringLiteral("pageCount"), document.pageCount() },
                                 { QStringLiteral("pages"), pages } };
        out() << QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Indented));
        return Success;
    }

    out() << i18n("File:  %1", info.fileName()) << Qt::endl;
    out() << i18n("Size:  %1", humanSize(info.size())) << Qt::endl;
    out() << i18np("Pages: %1 page", "Pages: %1 pages", document.pageCount()) << Qt::endl;

    const ps::Source *source = document.source(0);
    if (source && !source->openWarning().isEmpty()) {
        out() << source->openWarning() << Qt::endl;
    }

    if (document.pageCount() > 0) {
        const QSizeF first = document.pageSizePoints(0);
        out() << i18n("First page: %1 × %2 mm", QString::number(first.width() * 25.4 / 72.0, 'f', 1),
                      QString::number(first.height() * 25.4 / 72.0, 'f', 1))
              << Qt::endl;
    }
    return Success;
}

int commandMerge(const QStringList &inputs, const QString &output)
{
    if (inputs.size() < 2) {
        return fail(i18n("merge needs at least two files."), UsageError);
    }
    if (output.isEmpty()) {
        return fail(i18n("merge needs an output file (-o)."), UsageError);
    }

    ps::PopplerBackend backend;
    ps::Document document;
    document.setRenderBackend(&backend);

    QString error;
    if (!loadAll(document, inputs, &error)) {
        return fail(error);
    }
    if (!ps::DocumentWriter::write(document, output, {}, &error)) {
        return fail(error, OutputError);
    }

    out() << i18np("Merged %1 file into %2.", "Merged %1 files into %2.", inputs.size(), output) << Qt::endl;
    out() << i18np("%1 page total.", "%1 pages total.", document.pageCount()) << Qt::endl;
    return Success;
}

int commandPages(const QStringList &arguments, const QString &output, const QString &keep, const QString &rotate)
{
    if (arguments.isEmpty()) {
        return fail(i18n("pages needs a file."), UsageError);
    }
    if (output.isEmpty()) {
        return fail(i18n("pages needs an output file (-o)."), UsageError);
    }

    ps::PopplerBackend backend;
    ps::Document document;
    document.setRenderBackend(&backend);

    QString error;
    if (!document.open(arguments.constFirst(), &error)) {
        return fail(error);
    }

    QVector<int> selection = ps::PageRange::parse(keep, document.pageCount(), &error);
    if (selection.isEmpty()) {
        return fail(error, UsageError);
    }

    // --rotate takes "PAGES:DEGREES" pairs, e.g. "1-3:90,7:180".
    if (!rotate.isEmpty()) {
        const QStringList clauses = rotate.split(QLatin1Char(','), Qt::SkipEmptyParts);
        for (const QString &clause : clauses) {
            const int colon = clause.lastIndexOf(QLatin1Char(':'));
            if (colon < 0) {
                return fail(i18n("“%1” should look like PAGES:DEGREES, for example 1-3:90.", clause), UsageError);
            }
            bool ok = false;
            const int degrees = clause.mid(colon + 1).trimmed().toInt(&ok);
            if (!ok) {
                return fail(i18n("“%1” is not a rotation in degrees.", clause.mid(colon + 1)), UsageError);
            }
            const QVector<int> targets = ps::PageRange::parse(clause.left(colon), document.pageCount(), &error);
            if (targets.isEmpty()) {
                return fail(error, UsageError);
            }
            document.applyRotation(targets, degrees);
        }
    }

    if (!ps::DocumentWriter::writeSelection(document, selection, output, {}, &error)) {
        return fail(error, OutputError);
    }

    out() << i18np("Wrote %1 page to %2.", "Wrote %1 pages to %2.", selection.size(), output) << Qt::endl;
    return Success;
}

int commandSplit(const QStringList &arguments, const QString &output, int every, const QString &ranges,
                 const QString &maxSize, bool byBookmarks)
{
    if (arguments.isEmpty()) {
        return fail(i18n("split needs a file."), UsageError);
    }

    ps::PopplerBackend backend;
    ps::Document document;
    document.setRenderBackend(&backend);

    QString error;
    if (!document.open(arguments.constFirst(), &error)) {
        return fail(error);
    }

    ps::Splitter::Options options;
    options.outputTemplate = output.isEmpty() ? QFileInfo(arguments.constFirst()).absolutePath() + QLatin1Char('/')
            + QFileInfo(arguments.constFirst()).completeBaseName() + QStringLiteral("-%1.pdf")
                                              : output;

    if (!ranges.isEmpty()) {
        options.mode = ps::Splitter::Mode::Ranges;
        options.ranges = ranges;
    } else if (byBookmarks) {
        options.mode = ps::Splitter::Mode::Bookmarks;
        options.bookmarkSource = arguments.constFirst();

        const QVector<ps::Splitter::Chapter> chapters = ps::Splitter::chaptersOf(arguments.constFirst());
        if (chapters.isEmpty()) {
            return fail(i18n("This document has no bookmarks to split on."));
        }
        out() << i18ncp("@info", "Found %1 bookmark:", "Found %1 bookmarks:", chapters.size()) << Qt::endl;
        for (const ps::Splitter::Chapter &chapter : chapters) {
            out() << QStringLiteral("  %1  %2").arg(chapter.firstPage + 1, 4).arg(chapter.title) << Qt::endl;
        }
    } else if (!maxSize.isEmpty()) {
        options.mode = ps::Splitter::Mode::MaxFileSize;
        bool ok = false;
        qint64 bytes = 0;
        const QString trimmed = maxSize.trimmed().toLower();
        if (trimmed.endsWith(QLatin1Char('m'))) {
            bytes = static_cast<qint64>(trimmed.chopped(1).toDouble(&ok) * 1024 * 1024);
        } else if (trimmed.endsWith(QLatin1Char('k'))) {
            bytes = static_cast<qint64>(trimmed.chopped(1).toDouble(&ok) * 1024);
        } else {
            bytes = trimmed.toLongLong(&ok);
        }
        if (!ok || bytes <= 0) {
            return fail(i18n("“%1” is not a size. Try 5M or 500k.", maxSize), UsageError);
        }
        options.maxBytes = bytes;
    } else {
        options.mode = ps::Splitter::Mode::EveryNPages;
        options.pagesPerFile = std::max(1, every);
    }

    const ps::Splitter::Result result = ps::Splitter::split(document, options);
    if (!result.success) {
        return fail(result.error, OutputError);
    }

    for (const QString &file : result.files) {
        out() << QStringLiteral("%1  (%2)").arg(file, humanSize(QFileInfo(file).size())) << Qt::endl;
    }
    out() << i18np("Wrote %1 file.", "Wrote %1 files.", result.files.size()) << Qt::endl;
    return Success;
}

/** The compression level names, in English and in German. */
bool parseCompressionLevel(const QString &level, ps::Compressor::Options *options)
{
    static const QHash<QString, ps::Compressor::Level> levels {
        { QStringLiteral("lossless"), ps::Compressor::Level::Lossless },
        { QStringLiteral("verlustfrei"), ps::Compressor::Level::Lossless },
        { QStringLiteral("balanced"), ps::Compressor::Level::Balanced },
        { QStringLiteral("ausgewogen"), ps::Compressor::Level::Balanced },
        { QStringLiteral("strong"), ps::Compressor::Level::Strong },
        { QStringLiteral("stark"), ps::Compressor::Level::Strong },
        { QStringLiteral("extreme"), ps::Compressor::Level::Extreme },
        { QStringLiteral("klein"), ps::Compressor::Level::Extreme },
    };
    if (level.isEmpty()) {
        return true;
    }
    if (!levels.contains(level.toLower())) {
        return false;
    }
    options->level = levels.value(level.toLower());
    return true;
}

int commandCompress(const QStringList &arguments, const QString &output, const QString &level, bool grayscale)
{
    if (arguments.isEmpty()) {
        return fail(i18n("compress needs a file."), UsageError);
    }
    if (output.isEmpty()) {
        return fail(i18n("compress needs an output file (-o)."), UsageError);
    }

    ps::Compressor::Options options;
    options.grayscale = grayscale;

    if (!parseCompressionLevel(level, &options)) {
        return fail(i18n("Unknown level “%1”. Use lossless, balanced, strong or extreme.", level), UsageError);
    }

    ps::Compressor::Report report;
    QString error;
    if (!ps::Compressor::compress(arguments.constFirst(), output, options, &report, &error)) {
        return fail(error, OutputError);
    }

    // "0 % smaller" is a number, not an answer. Scans are very often already
    // stored as JPEG or JPEG 2000 below the target resolution, and saying that
    // out loud saves the user from trying every level in turn.
    if (report.outcome == ps::Compressor::Outcome::AlreadyOptimal) {
        out() << i18n("%1, already as small as it gets.", humanSize(report.originalBytes)) << Qt::endl;
        if (options.level == ps::Compressor::Level::Lossless) {
            out() << i18n("The structure is already packed. Try --level balanced to shrink the images too.")
                  << Qt::endl;
        } else if (options.level != ps::Compressor::Level::Extreme) {
            out() << i18n("The images already sit below this level's resolution, so there is nothing to downsample.")
                  << Qt::endl;
        } else {
            out() << i18n("The images are stored more efficiently than re-encoding them could manage.") << Qt::endl;
        }
        return Success;
    }

    out() << i18n("%1 → %2 (%3% smaller)", humanSize(report.originalBytes), humanSize(report.compressedBytes),
                  QString::number(report.savedPercent()))
          << Qt::endl;
    return Success;
}

/** Maps the words people type to the nine anchor positions. */
bool parseAnchor(const QString &text, ps::Overlay::Anchor *anchor)
{
    static const QHash<QString, ps::Overlay::Anchor> names {
        { QStringLiteral("top-left"), ps::Overlay::Anchor::TopLeft },
        { QStringLiteral("oben-links"), ps::Overlay::Anchor::TopLeft },
        { QStringLiteral("top-centre"), ps::Overlay::Anchor::TopCentre },
        { QStringLiteral("top-center"), ps::Overlay::Anchor::TopCentre },
        { QStringLiteral("oben-mitte"), ps::Overlay::Anchor::TopCentre },
        { QStringLiteral("top-right"), ps::Overlay::Anchor::TopRight },
        { QStringLiteral("oben-rechts"), ps::Overlay::Anchor::TopRight },
        { QStringLiteral("left"), ps::Overlay::Anchor::CentreLeft },
        { QStringLiteral("links"), ps::Overlay::Anchor::CentreLeft },
        { QStringLiteral("centre"), ps::Overlay::Anchor::Centre },
        { QStringLiteral("center"), ps::Overlay::Anchor::Centre },
        { QStringLiteral("mitte"), ps::Overlay::Anchor::Centre },
        { QStringLiteral("right"), ps::Overlay::Anchor::CentreRight },
        { QStringLiteral("rechts"), ps::Overlay::Anchor::CentreRight },
        { QStringLiteral("bottom-left"), ps::Overlay::Anchor::BottomLeft },
        { QStringLiteral("unten-links"), ps::Overlay::Anchor::BottomLeft },
        { QStringLiteral("bottom-centre"), ps::Overlay::Anchor::BottomCentre },
        { QStringLiteral("bottom-center"), ps::Overlay::Anchor::BottomCentre },
        { QStringLiteral("unten-mitte"), ps::Overlay::Anchor::BottomCentre },
        { QStringLiteral("bottom-right"), ps::Overlay::Anchor::BottomRight },
        { QStringLiteral("unten-rechts"), ps::Overlay::Anchor::BottomRight },
    };

    const auto it = names.constFind(text.trimmed().toLower());
    if (it == names.constEnd()) {
        return false;
    }
    *anchor = *it;
    return true;
}

/** Resolves a page range against the document, without loading it twice. */
QVector<int> resolvePages(const QString &input, const QString &range, QString *error)
{
    ps::PopplerBackend backend;
    ps::Document document;
    document.setRenderBackend(&backend);
    if (!document.open(input, error)) {
        return {};
    }
    return ps::PageRange::parse(range, document.pageCount(), error);
}

int commandSign(const QStringList &arguments, const QString &output, const QString &imagePath, const QString &range,
                const QString &where, double size, double margin)
{
    if (arguments.isEmpty()) {
        return fail(i18n("sign needs a file."), UsageError);
    }
    if (output.isEmpty()) {
        return fail(i18n("sign needs an output file (-o)."), UsageError);
    }
    if (imagePath.isEmpty()) {
        return fail(i18n("sign needs a signature image (--image)."), UsageError);
    }

    QImage image(imagePath);
    if (image.isNull()) {
        return fail(i18n("“%1” could not be read as an image.", imagePath));
    }

    ps::Overlay::Anchor anchor = ps::Overlay::Anchor::BottomRight;
    if (!where.isEmpty() && !parseAnchor(where, &anchor)) {
        return fail(i18n("“%1” is not a position. Try bottom-right, centre, top-left and so on.", where), UsageError);
    }

    QString error;
    const QVector<int> pages = resolvePages(arguments.constFirst(), range, &error);
    if (pages.isEmpty()) {
        return fail(error);
    }

    if (!ps::Overlay::stampImageOnPages(arguments.constFirst(), output, pages, image, anchor, size, 0.0, 1.0, margin,
                                        &error)) {
        return fail(error, OutputError);
    }

    out() << i18ncp("@info", "Signed %1 page.", "Signed %1 pages.", pages.size()) << Qt::endl;
    return Success;
}

int commandWatermark(const QStringList &arguments, const QString &output, const QString &text, const QString &range,
                     int angle, int opacity, const QString &colour, bool outline)
{
    if (arguments.isEmpty()) {
        return fail(i18n("watermark needs a file."), UsageError);
    }
    if (output.isEmpty()) {
        return fail(i18n("watermark needs an output file (-o)."), UsageError);
    }

    ps::Overlay::TextStamp stamp;
    stamp.text = text.isEmpty() ? i18nc("@item default watermark", "DRAFT") : text;
    stamp.rotation = angle;
    stamp.opacity = std::clamp(opacity, 1, 100) / 100.0;
    stamp.outlineOnly = outline;
    stamp.fontSize = 64;

    if (!colour.isEmpty()) {
        const QColor parsed(colour);
        if (!parsed.isValid()) {
            return fail(i18n("“%1” is not a colour. Try a name or #rrggbb.", colour), UsageError);
        }
        stamp.colour = parsed;
    }

    QString error;
    const QVector<int> pages = resolvePages(arguments.constFirst(), range, &error);
    if (pages.isEmpty()) {
        return fail(error);
    }

    if (!ps::Overlay::stampText(arguments.constFirst(), output, pages, stamp, &error)) {
        return fail(error, OutputError);
    }

    out() << i18ncp("@info", "Watermarked %1 page.", "Watermarked %1 pages.", pages.size()) << Qt::endl;
    return Success;
}

int commandFromImages(const QStringList &images, const QString &output, const QString &pageSize, int quality)
{
    if (images.isEmpty()) {
        return fail(i18n("from-images needs at least one image."), UsageError);
    }
    if (output.isEmpty()) {
        return fail(i18n("from-images needs an output file (-o)."), UsageError);
    }

    ps::ImageIO::ImportOptions options;
    options.jpegQuality = std::clamp(quality, 0, 100);

    if (!pageSize.isEmpty()) {
        static const QHash<QString, QSizeF> sizes {
            { QStringLiteral("a4"), QSizeF(595, 842) },     { QStringLiteral("a3"), QSizeF(842, 1191) },
            { QStringLiteral("a5"), QSizeF(420, 595) },     { QStringLiteral("letter"), QSizeF(612, 792) },
            { QStringLiteral("legal"), QSizeF(612, 1008) },
        };
        const auto it = sizes.constFind(pageSize.toLower());
        if (it == sizes.constEnd()) {
            return fail(i18n("“%1” is not a page size. Try a4, a3, a5, letter or legal.", pageSize), UsageError);
        }
        options.pageSizePoints = *it;
        options.marginPoints = 18;
    }

    QString error;
    if (!ps::ImageIO::imagesToPdf(images, output, options, &error)) {
        return fail(error, OutputError);
    }

    out() << i18ncp("@info", "Built a document from %1 image.", "Built a document from %1 images.", images.size())
          << Qt::endl;
    return Success;
}

int commandExportImages(const QStringList &arguments, const QString &output, const QString &range, int dpi,
                        const QString &format)
{
    if (arguments.isEmpty()) {
        return fail(i18n("export-images needs a file."), UsageError);
    }

    ps::PopplerBackend backend;
    ps::Document document;
    document.setRenderBackend(&backend);

    QString error;
    if (!document.open(arguments.constFirst(), &error)) {
        return fail(error);
    }

    const QVector<int> pages = ps::PageRange::parse(range, document.pageCount(), &error);
    if (pages.isEmpty()) {
        return fail(error, UsageError);
    }

    ps::ImageIO::ExportOptions options;
    options.dpi = dpi > 0 ? dpi : 150;
    if (!format.isEmpty()) {
        options.format = format.toLower();
    }

    const QString target = output.isEmpty() ? QFileInfo(arguments.constFirst()).absolutePath() + QLatin1Char('/')
            + QFileInfo(arguments.constFirst()).completeBaseName() + QStringLiteral("-%1.") + options.format
                                            : output;

    // The pages carry the rotation the document gave them, so an export of a
    // sideways scan comes out the way it reads.
    QVector<int> rotations;
    QVector<int> sourcePages;
    rotations.reserve(pages.size());
    sourcePages.reserve(pages.size());
    for (const int row : pages) {
        sourcePages.append(document.pageAt(row).sourcePage);
        rotations.append(document.pageAt(row).rotation);
    }

    QStringList written;
    if (!ps::ImageIO::pagesToImages(&backend, 0, sourcePages, rotations, target, options, &written, &error)) {
        return fail(error, OutputError);
    }

    for (const QString &file : written) {
        out() << file << Qt::endl;
    }
    out() << i18ncp("@info", "Wrote %1 image.", "Wrote %1 images.", written.size()) << Qt::endl;
    return Success;
}

int commandMeta(const QStringList &arguments, const QString &output, bool sanitize, bool asJson)
{
    if (arguments.isEmpty()) {
        return fail(i18n("meta needs a file."), UsageError);
    }
    const QString input = arguments.constFirst();

    if (sanitize) {
        if (output.isEmpty()) {
            return fail(i18n("Sanitising needs an output file (-o)."), UsageError);
        }
        ps::Metadata::Findings before;
        QString error;
        if (!ps::Metadata::inspect(input, &before, &error)) {
            return fail(error);
        }
        if (!ps::Metadata::strip(input, output, ps::Metadata::StripOptions {}, &error)) {
            return fail(error, OutputError);
        }

        out() << i18n("Removed:") << Qt::endl;
        out() << QStringLiteral("  %1 %2").arg(before.hasDocumentInfo || before.hasXmp ? QStringLiteral("✓")
                                                                                       : QStringLiteral("✗"),
                                               i18n("document information and XMP"))
              << Qt::endl;
        out() << QStringLiteral("  %1 %2").arg(before.hasJavaScript ? QStringLiteral("✓") : QStringLiteral("✗"),
                                               i18n("JavaScript"))
              << Qt::endl;
        out() << QStringLiteral("  %1 %2").arg(before.embeddedFileCount > 0 ? QStringLiteral("✓") : QStringLiteral("✗"),
                                               i18np("%1 embedded file", "%1 embedded files", before.embeddedFileCount))
              << Qt::endl;
        return Success;
    }

    ps::Metadata::Fields fields;
    QString error;
    if (!ps::Metadata::read(input, &fields, &error)) {
        return fail(error);
    }

    if (asJson) {
        const QJsonObject root { { QStringLiteral("title"), fields.title },
                                 { QStringLiteral("author"), fields.author },
                                 { QStringLiteral("subject"), fields.subject },
                                 { QStringLiteral("keywords"), fields.keywords },
                                 { QStringLiteral("creator"), fields.creator },
                                 { QStringLiteral("producer"), fields.producer },
                                 { QStringLiteral("created"), fields.created.toString(Qt::ISODate) },
                                 { QStringLiteral("modified"), fields.modified.toString(Qt::ISODate) } };
        out() << QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Indented));
        return Success;
    }

    const auto line = [](const QString &label, const QString &value) {
        if (!value.isEmpty()) {
            out() << QStringLiteral("%1 %2").arg(label.leftJustified(12), value) << Qt::endl;
        }
    };
    line(i18n("Title:"), fields.title);
    line(i18n("Author:"), fields.author);
    line(i18n("Subject:"), fields.subject);
    line(i18n("Keywords:"), fields.keywords);
    line(i18n("Creator:"), fields.creator);
    line(i18n("Producer:"), fields.producer);
    line(i18n("Created:"), fields.created.toString(Qt::ISODate));
    if (fields.isEmpty()) {
        out() << i18n("The document carries no metadata.") << Qt::endl;
    }
    return Success;
}

int commandProtect(const QStringList &arguments, const QString &output, const QString &password, bool removeIt,
                   const QString &openWith, bool noPrinting, bool noCopying)
{
    if (arguments.isEmpty()) {
        return fail(i18n("protect needs a file."), UsageError);
    }
    if (output.isEmpty()) {
        return fail(i18n("protect needs an output file (-o)."), UsageError);
    }
    const QString input = arguments.constFirst();

    QString error;
    if (removeIt) {
        if (!ps::Encryption::decrypt(input, output, openWith, &error)) {
            return fail(error, OutputError);
        }
        out() << i18n("The password has been removed.") << Qt::endl;
        return Success;
    }

    if (password.isEmpty()) {
        return fail(i18n("protect needs a password (--password), or --remove."), UsageError);
    }

    ps::Encryption::Permissions permissions;
    permissions.printing = noPrinting ? ps::Encryption::Printing::Forbidden : ps::Encryption::Printing::Allowed;
    permissions.allowExtractText = !noCopying;

    if (!ps::Encryption::encrypt(input, output, password, QString(), permissions, openWith, &error)) {
        return fail(error, OutputError);
    }

    out() << i18n("Locked with AES-256.") << Qt::endl;
    if (noPrinting || noCopying) {
        // Said plainly, because permission flags are a request rather than a
        // lock and pretending otherwise would be misleading.
        out() << i18n("Note: printing and copying restrictions are advisory. "
                      "Any reader may ignore them; the password is the real protection.")
              << Qt::endl;
    }
    return Success;
}

int commandNumber(const QStringList &arguments, const QString &output, const QString &range, const QString &text,
                  const QString &where, double fontSize, const QString &batesPrefix, int batesDigits, int startNumber)
{
    if (arguments.isEmpty()) {
        return fail(i18n("number needs a file."), UsageError);
    }
    if (output.isEmpty()) {
        return fail(i18n("number needs an output file (-o)."), UsageError);
    }

    ps::PageNumbering::Options options;
    if (!text.isEmpty()) {
        options.text = text;
    }
    if (fontSize > 0) {
        options.fontSize = fontSize;
    }
    options.batesPrefix = batesPrefix;
    if (batesDigits >= 0) {
        options.batesDigits = batesDigits;
    }
    if (startNumber > 0) {
        options.startNumber = startNumber;
    }
    options.fileName = QFileInfo(arguments.constFirst()).fileName();

    if (!where.isEmpty() && !parseAnchor(where, &options.anchor)) {
        return fail(i18n("“%1” is not a position. Try bottom-right, centre, top-left and so on.", where), UsageError);
    }

    QString error;
    const QVector<int> pages = resolvePages(arguments.constFirst(), range, &error);
    if (pages.isEmpty()) {
        return fail(error);
    }

    if (!ps::PageNumbering::apply(arguments.constFirst(), output, pages, options, &error)) {
        return fail(error, OutputError);
    }

    out() << i18ncp("@info", "Numbered %1 page.", "Numbered %1 pages.", pages.size()) << Qt::endl;
    out() << i18n("First: %1    Last: %2", ps::PageNumbering::render(options, 0, pages.size()),
                  ps::PageNumbering::render(options, pages.size() - 1, pages.size()))
          << Qt::endl;
    return Success;
}

int commandCrop(const QStringList &arguments, const QString &output, const QString &range, bool automatic, bool reset,
                double left, double right, double top, double bottom)
{
    if (arguments.isEmpty()) {
        return fail(i18n("crop needs a file."), UsageError);
    }
    if (output.isEmpty()) {
        return fail(i18n("crop needs an output file (-o)."), UsageError);
    }
    const QString input = arguments.constFirst();

    ps::PopplerBackend backend;
    ps::Document document;
    document.setRenderBackend(&backend);

    QString error;
    if (!document.open(input, &error)) {
        return fail(error);
    }

    const QVector<int> pages = ps::PageRange::parse(range, document.pageCount(), &error);
    if (pages.isEmpty()) {
        return fail(error, UsageError);
    }

    if (reset) {
        if (!ps::PageCrop::reset(input, output, pages, &error)) {
            return fail(error, OutputError);
        }
        out() << i18ncp("@info", "Removed the crop from %1 page.", "Removed the crop from %1 pages.", pages.size())
              << Qt::endl;
        return Success;
    }

    ps::PageCrop::Margins margins;
    if (automatic) {
        // The safest trim across every listed page, so the result stays one
        // consistent size.
        margins = ps::PageCrop::detectCommon(&backend, 0, pages);
        if (margins.isEmpty()) {
            out() << i18n("Nothing to trim: these pages have no empty border in common.") << Qt::endl;
            return Success;
        }
        out() << i18n("Trimming  left %1  right %2  top %3  bottom %4  (points)", QString::number(margins.left, 'f', 1),
                      QString::number(margins.right, 'f', 1), QString::number(margins.top, 'f', 1),
                      QString::number(margins.bottom, 'f', 1))
              << Qt::endl;
    } else {
        margins.left = left;
        margins.right = right;
        margins.top = top;
        margins.bottom = bottom;
        if (margins.isEmpty()) {
            return fail(i18n("Give margins with --left, --right, --top and --bottom, or use --auto."), UsageError);
        }
    }

    if (!ps::PageCrop::crop(input, output, pages, margins, &error)) {
        return fail(error, OutputError);
    }

    out() << i18ncp("@info", "Cropped %1 page.", "Cropped %1 pages.", pages.size()) << Qt::endl;
    return Success;
}

namespace {

/** "PAGE:X,Y,WIDTH,HEIGHT" or "PAGE:X,Y", in display points. */
bool parsePlacement(const QString &spec, int wanted, int *page, double *values)
{
    const QStringList halves = spec.split(QLatin1Char(':'));
    if (halves.size() != 2) {
        return false;
    }
    bool ok = false;
    *page = halves.constFirst().toInt(&ok) - 1;
    if (!ok || *page < 0) {
        return false;
    }
    const QStringList numbers = halves.constLast().split(QLatin1Char(','));
    if (numbers.size() != wanted) {
        return false;
    }
    for (int i = 0; i < wanted; ++i) {
        values[i] = numbers.at(i).toDouble(&ok);
        if (!ok) {
            return false;
        }
    }
    return true;
}

QString describe(ps::Annotation::Type type)
{
    switch (type) {
    case ps::Annotation::Type::Highlight:
        return i18nc("@item annotation kind", "Highlight");
    case ps::Annotation::Type::Underline:
        return i18nc("@item annotation kind", "Underline");
    case ps::Annotation::Type::StrikeOut:
        return i18nc("@item annotation kind", "Strikethrough");
    case ps::Annotation::Type::Squiggly:
        return i18nc("@item annotation kind", "Squiggly underline");
    case ps::Annotation::Type::Ink:
        return i18nc("@item annotation kind", "Freehand");
    case ps::Annotation::Type::Square:
        return i18nc("@item annotation kind", "Rectangle");
    case ps::Annotation::Type::Circle:
        return i18nc("@item annotation kind", "Ellipse");
    case ps::Annotation::Type::Line:
        return i18nc("@item annotation kind", "Line");
    case ps::Annotation::Type::FreeText:
        return i18nc("@item annotation kind", "Text box");
    case ps::Annotation::Type::Note:
        return i18nc("@item annotation kind", "Note");
    }
    return {};
}

} // namespace

/** A stable token for scripts, which must not follow the interface language. */
QString changeToken(ps::Compare::Change change)
{
    switch (change) {
    case ps::Compare::Change::Same:
        return QStringLiteral("same");
    case ps::Compare::Change::TextChanged:
        return QStringLiteral("textChanged");
    case ps::Compare::Change::VisualOnly:
        return QStringLiteral("visualOnly");
    case ps::Compare::Change::Added:
        return QStringLiteral("added");
    case ps::Compare::Change::Removed:
        return QStringLiteral("removed");
    }
    return QStringLiteral("same");
}

int commandArchive(const QStringList &arguments, const QString &output, const QString &level, const QString &icc,
                   const QString &title, const QString &author)
{
    if (arguments.isEmpty()) {
        return fail(i18n("archive needs a file."), UsageError);
    }
    if (output.isEmpty()) {
        return fail(i18n("archive needs an output file (-o)."), UsageError);
    }
    if (!ps::Archival::isAvailable()) {
        return fail(i18n("Ghostscript is not installed, and it does the conversion. Install the “ghostscript” "
                         "package and try again."));
    }

    ps::Archival::Options options;
    static const QHash<QString, ps::Archival::Level> levels {
        { QStringLiteral("1b"), ps::Archival::Level::PdfA1b },
        { QStringLiteral("2b"), ps::Archival::Level::PdfA2b },
        { QStringLiteral("3b"), ps::Archival::Level::PdfA3b },
    };
    if (!level.isEmpty()) {
        if (!levels.contains(level.toLower())) {
            return fail(i18n("“%1” is not a level. Use 1b, 2b or 3b.", level), UsageError);
        }
        options.level = levels.value(level.toLower());
    }
    options.iccProfilePath = icc;
    options.title = title;
    options.author = author;

    ps::Archival::Report report;
    QString error;
    if (!ps::Archival::convert(arguments.constFirst(), output, options, &report, &error)) {
        return fail(error, OutputError);
    }

    out() << i18n("Wrote %1 as %2.", output, ps::Archival::describe(options.level)) << Qt::endl;
    if (!report.changes.isEmpty()) {
        out() << i18nc("@title heading before a list", "Changed:") << Qt::endl;
        for (const QString &change : report.changes) {
            out() << QStringLiteral("  · ") << change << Qt::endl;
        }
    }
    for (const QString &warning : report.warnings) {
        err() << i18n("Note: %1", warning) << Qt::endl;
    }
    return Success;
}

int commandArchiveCheck(const QStringList &arguments)
{
    if (arguments.isEmpty()) {
        return fail(i18n("archive-check needs a file."), UsageError);
    }

    QString error;
    const ps::Archival::Findings findings = ps::Archival::inspect(arguments.constFirst(), &error);
    if (!error.isEmpty()) {
        return fail(error);
    }

    out() << (findings.claimedLevel.isEmpty() ? i18n("This file does not claim to be PDF/A.")
                                              : i18n("This file says it is %1.", findings.claimedLevel))
          << Qt::endl;

    for (const QString &problem : findings.problems) {
        out() << QStringLiteral("  · ") << problem << Qt::endl;
    }
    if (findings.problems.isEmpty()) {
        out() << i18n("Nothing found that would stop it being archival.") << Qt::endl;
    }

    // Printed, not optional: the exit code below would otherwise read as a
    // verdict from a real validator, which this is not.
    out() << Qt::endl;
    for (const QString &limit : ps::Archival::inspectionLimitations()) {
        out() << QStringLiteral("  ") << limit << Qt::endl;
    }

    return findings.looksArchival ? Success : DifferencesFound;
}

int commandCompare(const QStringList &arguments, const QString &output, bool asJson, int tolerance, bool keepWhitespace,
                   double dpi, const QString &visualise)
{
    if (arguments.size() < 2) {
        return fail(i18n("compare needs two files."), UsageError);
    }

    ps::Compare::Options options;
    options.ignoreWhitespace = !keepWhitespace;
    options.pixelTolerance = qBound(0, tolerance, 255);
    if (dpi > 0) {
        options.dpi = dpi;
    }

    ps::PopplerBackend backend;
    QString error;

    if (!visualise.isEmpty()) {
        if (output.isEmpty()) {
            return fail(i18n("--visualise needs an output image file (-o)."), UsageError);
        }
        bool ok = false;
        const int page = visualise.toInt(&ok) - 1;
        if (!ok || page < 0) {
            return fail(i18n("“%1” is not a page number.", visualise), UsageError);
        }
        const QImage sheet = ps::Compare::visualise(&backend, arguments.at(0), arguments.at(1), page, page, options);
        if (sheet.isNull() || !sheet.save(output)) {
            return fail(i18n("The comparison picture could not be made."), OutputError);
        }
        out() << i18n("Wrote the comparison of page %1 to %2.", page + 1, output) << Qt::endl;
        return Success;
    }

    const ps::Compare::Report report = ps::Compare::run(&backend, arguments.at(0), arguments.at(1), options, &error);
    if (!error.isEmpty()) {
        return fail(error);
    }

    if (asJson) {
        QJsonArray pages;
        for (const ps::Compare::PageResult &page : report.pages) {
            pages.append(QJsonObject {
                { QStringLiteral("leftPage"), page.leftPage < 0 ? QJsonValue() : QJsonValue(page.leftPage + 1) },
                { QStringLiteral("rightPage"), page.rightPage < 0 ? QJsonValue() : QJsonValue(page.rightPage + 1) },
                { QStringLiteral("change"), changeToken(page.change) },
                { QStringLiteral("pixelDifference"), page.pixelDifference },
                { QStringLiteral("removedWords"), QJsonArray::fromStringList(page.removedWords) },
                { QStringLiteral("addedWords"), QJsonArray::fromStringList(page.addedWords) } });
        }
        const QJsonObject root { { QStringLiteral("identical"), report.identical },
                                 { QStringLiteral("changedPages"), report.changedPages },
                                 { QStringLiteral("pages"), pages } };
        out() << QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Indented));
        return report.identical ? Success : DifferencesFound;
    }

    if (report.identical) {
        out() << i18n("The two documents are the same.") << Qt::endl;
        return Success;
    }

    for (const ps::Compare::PageResult &page : report.pages) {
        if (page.change == ps::Compare::Change::Same) {
            continue; // Silence for what has not changed, as diff does.
        }
        const int number = (page.leftPage >= 0 ? page.leftPage : page.rightPage) + 1;
        out() << i18nc("@info one changed page: number and what changed", "Page %1: %2", number,
                       ps::Compare::describe(page.change));
        if (page.change == ps::Compare::Change::VisualOnly && page.pixelDifference > 0) {
            out() << i18nc("@info how much of a page looks different", " (%1% of the page)",
                           QString::number(page.pixelDifference * 100.0, 'f', 1));
        }
        out() << Qt::endl;

        for (const QString &word : page.removedWords) {
            out() << QStringLiteral("  \u2212 ") << word << Qt::endl;
        }
        for (const QString &word : page.addedWords) {
            out() << QStringLiteral("  + ") << word << Qt::endl;
        }
    }

    out() << i18np("%1 page changed, out of %2.", "%1 pages changed, out of %2.", report.changedPages,
                   report.pages.size())
          << Qt::endl;
    return DifferencesFound;
}

int commandBatch(const QStringList &arguments, const QString &outDir, const QString &operation, const QString &level,
                 const QString &languages, bool straighten)
{
    if (arguments.isEmpty()) {
        return fail(i18n("batch needs at least one file."), UsageError);
    }
    if (outDir.isEmpty()) {
        return fail(i18n("batch needs a folder to write into (--out-dir)."), UsageError);
    }

    QDir target(outDir);
    if (!target.exists() && !QDir().mkpath(outDir)) {
        return fail(i18n("“%1” could not be created.", outDir), OutputError);
    }

    enum class Job { Compress, Sanitise, FlattenComments, FlattenForm, Ocr };
    Job job = Job::Compress;
    if (operation == QLatin1String("compress")) {
        job = Job::Compress;
    } else if (operation == QLatin1String("sanitize") || operation == QLatin1String("sanitise")) {
        job = Job::Sanitise;
    } else if (operation == QLatin1String("flatten-comments")) {
        job = Job::FlattenComments;
    } else if (operation == QLatin1String("flatten-form")) {
        job = Job::FlattenForm;
    } else if (operation == QLatin1String("ocr")) {
        job = Job::Ocr;
    } else {
        return fail(i18n("“%1” is not a batch operation. Choose compress, sanitize, flatten-comments, flatten-form "
                         "or ocr.",
                         operation),
                    UsageError);
    }

#ifndef PS_WITH_OCR
    if (job == Job::Ocr) {
        return fail(i18n("This build has no text recognition."), UsageError);
    }
#endif

    ps::Compressor::Options compression;
    if (job == Job::Compress && !parseCompressionLevel(level, &compression)) {
        return fail(i18n("“%1” is not a compression level.", level), UsageError);
    }

    // Sequential on purpose. Ghostscript and Tesseract each take a core or more
    // of their own, so running four files at once on a laptop makes the whole
    // machine unusable and finishes no sooner.
    int done = 0;
    int failed = 0;
    qint64 before = 0;
    qint64 after = 0;

    for (int i = 0; i < arguments.size(); ++i) {
        const QString input = arguments.at(i);
        const QFileInfo info(input);
        if (!info.exists()) {
            err() << i18n("Skipped %1: it does not exist.", input) << Qt::endl;
            ++failed;
            continue;
        }
        const QString output = target.filePath(info.fileName());
        if (QFileInfo(output).absoluteFilePath() == info.absoluteFilePath()) {
            err() << i18n("Skipped %1: that would overwrite the original.", input) << Qt::endl;
            ++failed;
            continue;
        }

        err() << QStringLiteral("\r")
              << i18nc("@info:progress", "%1 of %2: %3", i + 1, arguments.size(), info.fileName())
              << QStringLiteral("          ");
        err().flush();

        QString error;
        bool ok = false;
        switch (job) {
        case Job::Compress: {
            ps::Compressor::Report report;
            ok = ps::Compressor::compress(input, output, compression, &report, &error);
            break;
        }
        case Job::Sanitise: {
            ps::Metadata::StripOptions strip;
            ok = ps::Metadata::strip(input, output, strip, &error);
            break;
        }
        case Job::FlattenComments: {
            int count = 0;
            ok = ps::Annotations::flatten(input, output, {}, &count, &error);
            break;
        }
        case Job::FlattenForm: {
            int count = 0;
            ok = ps::Forms::flatten(input, output, &count, &error);
            break;
        }
        case Job::Ocr:
#ifdef PS_WITH_OCR
        {
            ps::PopplerBackend backend;
            ps::ScanProcessor processor;
            ps::ScanProcessor::Options options;
            options.straighten = straighten;
            options.skipPagesWithText = true;
            if (!languages.isEmpty()) {
                options.languages = languages.split(QLatin1Char('+'), Qt::SkipEmptyParts);
            }
            ps::ScanProcessor::Report ocrReport;
            ok = processor.process(input, output, &backend, options, &ocrReport, &error);
        }
#endif
        break;
        }

        if (ok) {
            ++done;
            before += info.size();
            after += QFileInfo(output).size();
        } else {
            ++failed;
            err() << QStringLiteral("\r") << i18n("Failed on %1: %2", info.fileName(), error) << Qt::endl;
        }
    }

    // Wipe the progress line and flush it, or it lands in the middle of the
    // summary when stdout and stderr are the same terminal.
    err() << QStringLiteral("\r") << QString(70, QLatin1Char(' ')) << QStringLiteral("\r");
    err().flush();
    out() << i18np("Wrote %1 file into %2.", "Wrote %1 files into %2.", done, outDir) << Qt::endl;
    if (failed > 0) {
        out() << i18np("%1 file was left out; see the notes above.", "%1 files were left out; see the notes above.",
                       failed)
              << Qt::endl;
    }
    if (job == Job::Compress && done > 0 && before > 0) {
        out() << i18n("Together: %1 → %2 (%3% smaller)", humanSize(before), humanSize(after),
                      QString::number(100.0 - 100.0 * double(after) / double(before), 'f', 1))
              << Qt::endl;
    }
    return failed == 0 ? Success : InputError;
}

int commandText(const QStringList &arguments, const QString &output, int listPage, bool asJson,
                const QStringList &changes)
{
    if (arguments.isEmpty()) {
        return fail(i18n("text needs a file."), UsageError);
    }
    const QString input = arguments.constFirst();
    QString error;

    if (changes.isEmpty()) {
        const int page = listPage > 0 ? listPage - 1 : 0;
        const QVector<ps::TextEdit::Run> runs = ps::TextEdit::runsOn(input, page, &error);
        if (!error.isEmpty()) {
            return fail(error);
        }
        if (runs.isEmpty()) {
            out() << i18n("Page %1 holds no text this can change. A scanned page needs text recognition first.",
                          page + 1)
                  << Qt::endl;
            return Success;
        }

        if (asJson) {
            QJsonArray array;
            for (const ps::TextEdit::Run &run : runs) {
                array.append(QJsonObject { { QStringLiteral("page"), run.page + 1 },
                                           { QStringLiteral("index"), run.index },
                                           { QStringLiteral("text"), run.text },
                                           { QStringLiteral("font"), run.fontResource },
                                           { QStringLiteral("size"), run.fontSize },
                                           { QStringLiteral("editable"), run.editable } });
            }
            out() << QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Indented));
            return Success;
        }

        for (const ps::TextEdit::Run &run : runs) {
            out() << QStringLiteral("%1").arg(run.index, 4) << QStringLiteral("  ")
                  << (run.editable ? QStringLiteral("  ") : QStringLiteral("· ")) << run.text << Qt::endl;
        }
        out() << i18np("%1 line of text on page %2. The number in front is what --replace takes.",
                       "%1 lines of text on page %2. The number in front is what --replace takes.", runs.size(),
                       page + 1)
              << Qt::endl;
        out() << i18n("Lines marked · use a font this cannot write into.") << Qt::endl;
        return Success;
    }

    if (output.isEmpty()) {
        return fail(i18n("Changing text needs an output file (-o)."), UsageError);
    }

    QVector<ps::TextEdit::Replacement> replacements;
    for (const QString &change : changes) {
        // PAGE:INDEX=TEXT, split so the replacement may contain colons and
        // equals signs of its own.
        const int equals = change.indexOf(QLatin1Char('='));
        const int colon = change.indexOf(QLatin1Char(':'));
        bool ok = equals > 0 && colon > 0 && colon < equals;
        ps::TextEdit::Replacement replacement;
        if (ok) {
            const int page = change.left(colon).toInt(&ok);
            const int index = ok ? change.mid(colon + 1, equals - colon - 1).toInt(&ok) : 0;
            replacement.page = page - 1;
            replacement.index = index;
            replacement.text = change.mid(equals + 1);
            ok = ok && page >= 1 && index >= 0;
        }
        if (!ok) {
            return fail(i18n("“%1” is not a replacement. The form is PAGE:LINE=New text, and the line number is the "
                             "one --list shows.",
                             change),
                        UsageError);
        }
        replacements.append(replacement);
    }

    ps::TextEdit::Report report;
    if (!ps::TextEdit::apply(input, output, replacements, {}, &report, &error)) {
        return fail(error, OutputError);
    }

    out() << i18np("Corrected %1 line.", "Corrected %1 lines.", report.replaced) << Qt::endl;
    if (report.widthAdjustment > 0.5) {
        out() << i18n("Squeezed by up to %1% to keep the text inside its space.",
                      QString::number(report.widthAdjustment, 'f', 0))
              << Qt::endl;
    }
    for (const QString &note : report.refusals + report.overflowed) {
        err() << i18n("Note: %1", note) << Qt::endl;
    }
    return report.refusals.isEmpty() ? Success : InputError;
}

int commandForm(const QStringList &arguments, const QString &output, bool asJson, bool flatten,
                const QStringList &settings)
{
    if (arguments.isEmpty()) {
        return fail(i18n("form needs a file."), UsageError);
    }
    const QString input = arguments.constFirst();
    QString error;

    if (flatten) {
        if (output.isEmpty()) {
            return fail(i18n("form --flatten needs an output file (-o)."), UsageError);
        }
        int count = 0;
        if (!ps::Forms::flatten(input, output, &count, &error)) {
            return fail(error, OutputError);
        }
        out() << i18np("Made %1 field permanent. The form can no longer be filled in.",
                       "Made %1 fields permanent. The form can no longer be filled in.", count)
              << Qt::endl;
        return Success;
    }

    if (settings.isEmpty()) {
        const QVector<ps::FormField> fields = ps::Forms::read(input, &error);
        if (!error.isEmpty()) {
            return fail(error);
        }
        if (fields.isEmpty()) {
            out() << i18n("This document has no form fields.") << Qt::endl;
            return Success;
        }

        if (asJson) {
            QJsonArray array;
            for (const ps::FormField &field : fields) {
                QJsonObject object { { QStringLiteral("name"), field.name },
                                     { QStringLiteral("label"), field.label },
                                     { QStringLiteral("kind"), ps::Forms::describe(field.kind) },
                                     { QStringLiteral("value"), field.value },
                                     { QStringLiteral("page"), field.page + 1 },
                                     { QStringLiteral("readOnly"), field.readOnly },
                                     { QStringLiteral("required"), field.required } };
                if (!field.options.isEmpty()) {
                    object.insert(QStringLiteral("options"), QJsonArray::fromStringList(field.options));
                }
                array.append(object);
            }
            out() << QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Indented));
            return Success;
        }

        for (const ps::FormField &field : fields) {
            out() << i18nc("@info one form field: page, kind, name", "Page %1  %2  %3", field.page + 1,
                           ps::Forms::describe(field.kind), field.name);
            if (field.required) {
                out() << i18nc("@info marks a form field that must be filled in", "  (required)");
            }
            if (field.readOnly) {
                out() << i18nc("@info marks a form field that cannot be filled in", "  (locked)");
            }
            out() << Qt::endl;
            if (!field.value.isEmpty()) {
                out() << QStringLiteral("      = ") << field.value << Qt::endl;
            }
            if (!field.options.isEmpty()) {
                out() << QStringLiteral("      ") << i18n("choices: %1", field.options.join(QStringLiteral(", ")))
                      << Qt::endl;
            }
        }
        return Success;
    }

    if (output.isEmpty()) {
        return fail(i18n("Filling a form needs an output file (-o)."), UsageError);
    }

    QHash<QString, QString> values;
    for (const QString &setting : settings) {
        // NAME=VALUE, split at the first equals so values may contain one.
        const int equals = setting.indexOf(QLatin1Char('='));
        if (equals <= 0) {
            return fail(i18n("“%1” is not a field setting. The form is NAME=VALUE.", setting), UsageError);
        }
        values.insert(setting.left(equals), setting.mid(equals + 1));
    }

    int count = 0;
    QStringList notes;
    if (!ps::Forms::fill(input, output, values, &count, &notes, &error)) {
        return fail(error, OutputError);
    }

    out() << i18np("Filled in %1 field.", "Filled in %1 fields.", count) << Qt::endl;
    for (const QString &note : notes) {
        err() << i18n("Note: %1", note) << Qt::endl;
    }
    return notes.isEmpty() ? Success : InputError;
}

int commandOutline(const QStringList &arguments, const QString &output, bool asJson, bool removeAll,
                   const QStringList &additions, const QString &fromJson)
{
    if (arguments.isEmpty()) {
        return fail(i18n("outline needs a file."), UsageError);
    }
    const QString input = arguments.constFirst();
    QString error;

    QVector<ps::OutlineItem> items = ps::Outline::read(input, &error);
    if (!error.isEmpty()) {
        return fail(error);
    }

    const bool changing = removeAll || !additions.isEmpty() || !fromJson.isEmpty();
    if (!changing) {
        if (asJson) {
            // Nested, so the shape of the tree survives a round trip through a
            // script rather than being flattened into guesswork.
            const std::function<QJsonArray(const QVector<ps::OutlineItem> &)> encode
                = [&](const QVector<ps::OutlineItem> &level) {
                      QJsonArray array;
                      for (const ps::OutlineItem &item : level) {
                          QJsonObject object { { QStringLiteral("title"), item.title },
                                               { QStringLiteral("page"), item.page + 1 } };
                          if (!item.children.isEmpty()) {
                              object.insert(QStringLiteral("children"), encode(item.children));
                          }
                          array.append(object);
                      }
                      return array;
                  };
            out() << QString::fromUtf8(QJsonDocument(encode(items)).toJson(QJsonDocument::Indented));
            return Success;
        }

        if (items.isEmpty()) {
            out() << i18n("This document has no table of contents.") << Qt::endl;
            return Success;
        }
        for (const auto &entry : ps::Outline::flatten(items)) {
            out() << QString(entry.second * 2, QLatin1Char(' ')) << QStringLiteral("%1").arg(entry.first->page + 1, 5)
                  << QStringLiteral("  ") << entry.first->title << Qt::endl;
        }
        out() << i18np("%1 entry.", "%1 entries.", ps::Outline::count(items)) << Qt::endl;
        return Success;
    }

    if (output.isEmpty()) {
        return fail(i18n("Changing the table of contents needs an output file (-o)."), UsageError);
    }

    if (removeAll) {
        items.clear();
    }

    if (!fromJson.isEmpty()) {
        QFile file(fromJson);
        if (!file.open(QIODevice::ReadOnly)) {
            return fail(i18n("“%1” could not be read.", fromJson));
        }
        QJsonParseError parseError;
        const QJsonDocument json = QJsonDocument::fromJson(file.readAll(), &parseError);
        if (parseError.error != QJsonParseError::NoError || !json.isArray()) {
            return fail(i18n("“%1” is not a list of bookmarks: %2", fromJson, parseError.errorString()), UsageError);
        }

        const std::function<QVector<ps::OutlineItem>(const QJsonArray &)> decode = [&](const QJsonArray &array) {
            QVector<ps::OutlineItem> level;
            for (const QJsonValue &value : array) {
                const QJsonObject object = value.toObject();
                ps::OutlineItem item;
                item.title = object.value(QStringLiteral("title")).toString();
                item.page = object.value(QStringLiteral("page")).toInt(1) - 1;
                item.children = decode(object.value(QStringLiteral("children")).toArray());
                if (!item.title.isEmpty()) {
                    level.append(item);
                }
            }
            return level;
        };
        items = decode(json.array());
    }

    for (const QString &spec : additions) {
        // "PAGE:Title", with the first colon separating them so titles may
        // contain colons of their own.
        const int colon = spec.indexOf(QLatin1Char(':'));
        bool ok = colon > 0;
        const int page = ok ? spec.left(colon).toInt(&ok) : 0;
        const QString title = ok ? spec.mid(colon + 1).trimmed() : QString();
        if (!ok || page < 1 || title.isEmpty()) {
            return fail(i18n("“%1” is not a bookmark. The form is PAGE:Title, for example 3:Chapter one.", spec),
                        UsageError);
        }
        ps::OutlineItem item;
        item.title = title;
        item.page = page - 1;
        items.append(item);
    }

    if (!ps::Outline::write(input, output, items, &error)) {
        return fail(error, OutputError);
    }

    out() << i18np("Wrote a table of contents with %1 entry.", "Wrote a table of contents with %1 entries.",
                   ps::Outline::count(items))
          << Qt::endl;
    return Success;
}

int commandAnnotate(const QStringList &arguments, const QString &output, bool list, bool asJson, bool removeAll,
                    bool flatten, const QStringList &highlights, const QStringList &notes, const QString &text,
                    const QString &colourName, const QString &range, const QString &author, const QString &exportPath,
                    const QString &importPath)
{
    if (arguments.isEmpty()) {
        return fail(i18n("annotate needs a file."), UsageError);
    }
    const QString input = arguments.constFirst();
    QString error;

    if (list) {
        const QVector<ps::Annotation> found = ps::Annotations::read(input, {}, &error);
        if (!error.isEmpty()) {
            return fail(error);
        }
        if (asJson) {
            QJsonArray items;
            for (const ps::Annotation &annotation : found) {
                items.append(QJsonObject {
                    { QStringLiteral("page"), annotation.page + 1 },
                    { QStringLiteral("kind"), describe(annotation.type) },
                    { QStringLiteral("author"), annotation.author },
                    { QStringLiteral("contents"), annotation.contents },
                    { QStringLiteral("id"), annotation.identifier },
                });
            }
            out() << QString::fromUtf8(QJsonDocument(items).toJson(QJsonDocument::Indented));
            return Success;
        }
        if (found.isEmpty()) {
            out() << i18n("No comments in this document.") << Qt::endl;
            return Success;
        }
        for (const ps::Annotation &annotation : found) {
            out() << i18nc("@info one listed annotation: page, kind, author", "Page %1  %2  %3", annotation.page + 1,
                           describe(annotation.type),
                           annotation.author.isEmpty() ? i18nc("@item:shell an annotation that names no author", "none")
                                                       : annotation.author)
                  << Qt::endl;
            if (!annotation.contents.isEmpty()) {
                out() << QStringLiteral("    ") << annotation.contents << Qt::endl;
            }
        }
        return Success;
    }

    if (!exportPath.isEmpty()) {
        if (!ps::Annotations::exportXfdf(input, exportPath, {}, QFileInfo(input).fileName(), &error)) {
            return fail(error, OutputError);
        }
        out() << i18n("Wrote the comments to %1. The document itself is not in it.", exportPath) << Qt::endl;
        return Success;
    }

    if (output.isEmpty()) {
        return fail(i18n("annotate needs an output file (-o)."), UsageError);
    }

    if (!importPath.isEmpty()) {
        int count = 0;
        QStringList warnings;
        if (!ps::Annotations::importXfdf(input, importPath, output, &count, &warnings, &error)) {
            return fail(error, OutputError);
        }
        out() << i18np("Brought in %1 comment.", "Brought in %1 comments.", count) << Qt::endl;
        for (const QString &warning : warnings) {
            err() << i18n("Note: %1", warning) << Qt::endl;
        }
        return Success;
    }

    if (flatten) {
        int count = 0;
        QVector<int> pages;
        if (!range.isEmpty()) {
            ps::PopplerBackend backend;
            ps::Document document;
            document.setRenderBackend(&backend);
            if (!document.open(input, &error)) {
                return fail(error);
            }
            pages = ps::PageRange::parse(range, document.pageCount(), &error);
            if (pages.isEmpty()) {
                return fail(error, UsageError);
            }
        }
        if (!ps::Annotations::flatten(input, output, pages, &count, &error)) {
            return fail(error, OutputError);
        }
        out() << i18np("Drew %1 comment into the pages; it is part of them now.",
                       "Drew %1 comments into the pages; they are part of them now.", count)
              << Qt::endl;
        return Success;
    }

    if (removeAll) {
        int count = 0;
        if (!ps::Annotations::remove(input, output, {}, {}, &count, &error)) {
            return fail(error, OutputError);
        }
        out() << i18np("Removed %1 comment. Form fields were left alone.",
                       "Removed %1 comments. Form fields were left alone.", count)
              << Qt::endl;
        return Success;
    }

    QVector<ps::Annotation> additions;
    // Comments without a name are hard to answer; the login name is what every
    // other tool defaults to.
    const QString who = author.isEmpty() ? QString::fromLocal8Bit(qgetenv("USER")) : author;
    const QColor colour = colourName.isEmpty() ? QColor(255, 212, 0) : QColor(colourName);
    if (!colour.isValid()) {
        return fail(i18n("“%1” is not a colour.", colourName), UsageError);
    }

    for (const QString &spec : highlights) {
        int page = 0;
        double values[4] = { 0, 0, 0, 0 };
        if (!parsePlacement(spec, 4, &page, values) || values[2] <= 0 || values[3] <= 0) {
            return fail(i18n("“%1” is not an area. The form is PAGE:X,Y,WIDTH,HEIGHT, in points from the "
                             "bottom-left corner of the page as it is shown.",
                             spec),
                        UsageError);
        }
        ps::Annotation annotation;
        annotation.type = ps::Annotation::Type::Highlight;
        annotation.page = page;
        annotation.quads = { QRectF(values[0], values[1], values[2], values[3]) };
        annotation.colour = colour;
        annotation.contents = text;
        annotation.author = who;
        additions.append(annotation);
    }

    for (const QString &spec : notes) {
        int page = 0;
        double values[2] = { 0, 0 };
        if (!parsePlacement(spec, 2, &page, values)) {
            return fail(i18n("“%1” is not a place. The form is PAGE:X,Y, in points from the bottom-left corner of "
                             "the page as it is shown.",
                             spec),
                        UsageError);
        }
        ps::Annotation annotation;
        annotation.type = ps::Annotation::Type::Note;
        annotation.page = page;
        annotation.rect = QRectF(values[0], values[1], 20, 20);
        annotation.colour = colour;
        annotation.contents = text;
        annotation.author = who;
        additions.append(annotation);
    }

    if (additions.isEmpty()) {
        return fail(i18n("Say what to do: --highlight, --note, --clear, --flatten or --list."), UsageError);
    }
    if (!ps::Annotations::add(input, output, additions, &error)) {
        return fail(error, OutputError);
    }

    out() << i18np("Added %1 comment.", "Added %1 comments.", additions.size()) << Qt::endl;
    return Success;
}

int commandNUp(const QStringList &arguments, const QString &output, int perSheet, const QString &grid, double margin,
               double gutter, bool borders, bool columnsFirst)
{
    if (arguments.isEmpty()) {
        return fail(i18n("nup needs a file."), UsageError);
    }
    if (output.isEmpty()) {
        return fail(i18n("nup needs an output file (-o)."), UsageError);
    }

    ps::NUp::Options options;
    options.marginPoints = margin;
    options.gutterPoints = gutter;
    options.drawBorders = borders;
    options.columnsFirst = columnsFirst;

    if (!grid.isEmpty()) {
        const QStringList parts = grid.split(QLatin1Char('x'), Qt::SkipEmptyParts);
        bool ok = parts.size() == 2;
        if (ok) {
            options.columns = parts.at(0).toInt(&ok);
        }
        if (ok) {
            options.rows = parts.at(1).toInt(&ok);
        }
        if (!ok || options.columns < 1 || options.rows < 1) {
            return fail(i18n("“%1” is not a grid. The form is COLUMNSxROWS, for example 2x2.", grid), UsageError);
        }
    } else {
        // Without an explicit grid, guess from the first page which way round
        // the sheet wants to be.
        ps::PopplerBackend backend;
        bool landscape = false;
        if (backend.addDocument(1, arguments.constFirst(), nullptr)) {
            const QSizeF first = backend.pageSizePoints(1, 0);
            landscape = first.width() > first.height();
        }
        const QSize chosen = ps::NUp::gridFor(perSheet, landscape);
        options.columns = chosen.width();
        options.rows = chosen.height();
    }

    QString error;
    if (!ps::NUp::apply(arguments.constFirst(), output, options, &error)) {
        return fail(error, OutputError);
    }

    ps::PopplerBackend result;
    int sheets = 0;
    if (result.addDocument(2, output, nullptr)) {
        ps::Document written;
        written.setRenderBackend(&result);
        if (written.open(output, nullptr)) {
            sheets = written.pageCount();
        }
    }

    out() << i18n("Arranged %1 per sheet, %2 across and %3 down.", options.columns * options.rows, options.columns,
                  options.rows)
          << Qt::endl;
    out() << i18np("Wrote %1 sheet to %2.", "Wrote %1 sheets to %2.", sheets, output) << Qt::endl;
    return Success;
}

int commandRedact(const QStringList &arguments, const QString &output, const QStringList &areaSpecs, bool keepVisible)
{
    if (arguments.isEmpty()) {
        return fail(i18n("redact needs a file."), UsageError);
    }
    if (output.isEmpty()) {
        return fail(i18n("redact needs an output file (-o)."), UsageError);
    }
    if (areaSpecs.isEmpty()) {
        return fail(i18n("Give at least one area, e.g. --area 1:72,700,120,16"), UsageError);
    }
    const QString input = arguments.constFirst();

    QVector<ps::Redaction::Area> areas;
    for (const QString &spec : areaSpecs) {
        const QStringList halves = spec.split(QLatin1Char(':'));
        const QStringList numbers = halves.constLast().split(QLatin1Char(','));
        bool ok = halves.size() == 2 && numbers.size() == 4;
        ps::Redaction::Area area;
        if (ok) {
            const int page = halves.constFirst().toInt(&ok);
            double values[4] = { 0, 0, 0, 0 };
            for (int i = 0; ok && i < 4; ++i) {
                values[i] = numbers.at(i).toDouble(&ok);
            }
            area.page = page - 1;
            area.rect = QRectF(values[0], values[1], values[2], values[3]);
            ok = ok && page >= 1 && values[2] > 0 && values[3] > 0;
        }
        if (!ok) {
            return fail(i18n("“%1” is not an area. The form is PAGE:X,Y,WIDTH,HEIGHT, in points from the "
                             "bottom-left corner of the page as it is shown.",
                             spec),
                        UsageError);
        }
        areas.append(area);
    }

    ps::PopplerBackend backend;
    QString error;
    if (!backend.addDocument(1, input, &error)) {
        return fail(error);
    }

    ps::Redaction::Options options;
    options.paintBox = !keepVisible;
    // A picture in a format nothing here can decode leaves a choice between
    // destroying the page and not redacting it. Rendering is the way out, and
    // on the command line the renderer is right here.
    options.pageRasteriser = [&backend](int page, double dpi) {
        const QSizeF size = backend.pageSizePoints(1, page);
        return backend.renderPage(1, page, qRound(size.width() * dpi / 72.0));
    };

    ps::Redaction::Report report;
    if (!ps::Redaction::apply(input, output, areas, options, &report, &error)) {
        return fail(error, OutputError);
    }

    out() << i18ncp("@info", "Redacted %1 area.", "Redacted %1 areas.", report.areasApplied) << Qt::endl;
    // Each noun counts itself: a single line with four hard-coded plurals reads
    // wrong for a count of one in every language there is.
    const QString characters = i18ncp("@item one entry in the list of what a redaction removed", "%1 character",
                                      "%1 characters", report.glyphsRemoved);
    const QString pictures = i18ncp("@item one entry in the list of what a redaction removed", "%1 picture",
                                    "%1 pictures", report.imagesRemoved);
    const QString drawings = i18ncp("@item one entry in the list of what a redaction removed", "%1 drawing",
                                    "%1 drawings", report.formsRemoved);
    const QString annotations = i18ncp("@item one entry in the list of what a redaction removed", "%1 annotation",
                                       "%1 annotations", report.annotationsRemoved);
    out() << i18nc("@info each argument is already a count with its own noun, such as “3 pictures”",
                   "Removed %1, %2, %3, %4", characters, pictures, drawings, annotations)
          << Qt::endl;
    if (report.imagesEdited > 0) {
        out() << i18ncp("@info", "Edited %1 picture pixel by pixel.", "Edited %1 pictures pixel by pixel.",
                        report.imagesEdited)
              << Qt::endl;
    }
    if (!report.pagesRasterised.isEmpty()) {
        QStringList numbers;
        for (int page : report.pagesRasterised) {
            numbers << QString::number(page + 1);
        }
        out() << i18ncp("@info %2 is a comma-separated list of page numbers",
                        "Flattened one page to an image, losing its selectable text: page %2",
                        "Flattened %1 pages to an image, losing their selectable text: pages %2",
                        report.pagesRasterised.size(), numbers.join(QStringLiteral(", ")))
              << Qt::endl;
    }
    for (const QString &warning : report.warnings) {
        err() << i18n("Note: %1", warning) << Qt::endl;
    }
    return Success;
}

#ifdef PS_WITH_OCR
int commandOcr(const QStringList &arguments, const QString &output, const QString &languages, bool straighten,
               bool force)
{
    if (arguments.isEmpty()) {
        return fail(i18n("ocr needs a file."), UsageError);
    }
    if (output.isEmpty()) {
        return fail(i18n("ocr needs an output file (-o)."), UsageError);
    }

    ps::PopplerBackend backend;
    ps::ScanProcessor processor;

    ps::ScanProcessor::Options options;
    options.straighten = straighten;
    options.skipPagesWithText = !force;
    if (!languages.isEmpty()) {
        options.languages = languages.split(QLatin1Char('+'), Qt::SkipEmptyParts);
    }

    QObject::connect(&processor, &ps::ScanProcessor::progress, [](int page, int count) {
        err() << QStringLiteral("\r") << i18n("Page %1 of %2…", page, count);
        err().flush();
    });

    ps::ScanProcessor::Report report;
    QString error;
    const bool ok = processor.process(arguments.constFirst(), output, &backend, options, &report, &error);
    err() << QStringLiteral("\r") << Qt::endl;

    if (!ok) {
        return fail(error, OutputError);
    }

    out() << i18np("Recognised %1 word.", "Recognised %1 words.", report.wordsRecognized) << Qt::endl;
    if (report.pagesSkipped > 0) {
        out() << i18np("Skipped %1 page that already had text.", "Skipped %1 pages that already had text.",
                       report.pagesSkipped)
              << Qt::endl;
    }
    if (report.pagesStraightened > 0) {
        out() << i18np("Straightened %1 page, worst tilt %2°.", "Straightened %1 pages, worst tilt %2°.",
                       report.pagesStraightened, QString::number(report.largestSkewAngle, 'f', 1))
              << Qt::endl;
    }
    return Success;
}
#endif

/** Every area of the command line, in the order their help is printed. */
QVector<ps::cli::Group> commandGroups()
{
    return { ps::cli::preflightGroup(), ps::cli::colourGroup(), ps::cli::fontGroup(),
             ps::cli::imageGroup(),     ps::cli::layoutGroup(), ps::cli::formGroup(),
             ps::cli::typesetGroup(),   ps::cli::objectGroup(), ps::cli::convertGroup() };
}

/**
 * The verbs the areas contribute, in the same shape as the built-in list.
 *
 * They belong in that one list rather than in a second section of their own:
 * docs/pdf-smithy-cli.1 is generated by reading this help back out of the
 * program, and it stops at the first blank line, so a command in a second
 * block would work, be documented nowhere, and nobody would notice until a
 * user went looking for it in the manual.
 *
 * A group writes each line as its usage, a tab, and what it does; the columns
 * are laid out here so that eight separately written files cannot drift apart.
 */
QString groupHelp()
{
    QStringList lines;
    for (const ps::cli::Group &group : commandGroups()) {
        if (group.help) {
            lines += group.help();
        }
    }
    lines.sort();

    QString text;
    for (QString line : std::as_const(lines)) {
        line = line.trimmed();
        if (line.isEmpty()) {
            continue;
        }
        // Split on a tab rather than on the run of spaces between the columns: a
        // group that pads its usage column to a fixed width leaves no run of
        // spaces at all once the usage is long enough, and that one line would
        // otherwise lose its description silently.
        const qsizetype gap = line.indexOf(u'\t');
        const QString left = gap < 0 ? line : line.left(gap).trimmed();
        const QString right = gap < 0 ? QString() : line.mid(gap + 1).trimmed();
        text += QStringLiteral("  ") + left.leftJustified(34, u' ') + QStringLiteral("  ") + right + u'\n';
    }
    return text;
}

} // namespace

int main(int argc, char **argv)
{
    // PDF is written in the C locale and QPDF parses it with strtod, so a
    // German or French system would read every fractional number in every
    // document as zero. Our own reads go through PdfGeometry::numericValue and
    // are safe either way; QPDF's internal arithmetic (page placement,
    // matrices) is not, so the process is put into the C locale for numbers
    // only. Qt formats through QLocale and is unaffected.
    std::setlocale(LC_NUMERIC, "C");

    QCoreApplication application(argc, argv);
    KLocalizedString::setApplicationDomain(PS_NAME);

    KAboutData about(QStringLiteral(PS_NAME "-cli"), QStringLiteral(PS_DISPLAY " (command line)"),
                     QStringLiteral(PS_VERSION), i18n("Edit PDF documents from the command line."),
                     KAboutLicense::GPL_V3, i18n("© 2026 Tom Bueng"));
    KAboutData::setApplicationData(about);

    QCommandLineParser parser;
    parser.setApplicationDescription(
        i18n("PDF Smithy (command line)\n"
             "\n"
             "Commands:\n"
             "  info      FILE                      Show page count, size and dimensions\n"
             "  merge     FILE...  -o OUT           Glue documents together\n"
             "  split     FILE     [-o TEMPLATE]    Break a document apart\n"
             "  pages     FILE     -o OUT           Select, reorder and rotate pages\n"
             "  compress  FILE     -o OUT           Make a document smaller\n"
             "  ocr       FILE     -o OUT           Make a scan searchable\n"
             "  sign      FILE     -o OUT           Stamp a signature image onto pages\n"
             "  watermark FILE     -o OUT           Lay text across pages\n"
             "  meta      FILE                      Show metadata; --sanitize -o OUT strips it\n"
             "  protect   FILE     -o OUT           Set or remove a password\n"
             "  from-images IMAGE... -o OUT         Build a document from pictures\n"
             "  export-images FILE                  Render pages to image files\n"
             "  number    FILE     -o OUT           Page numbers, headers and Bates stamping\n"
             "  crop      FILE     -o OUT           Trim the empty border off pages\n"
             "  redact    FILE     -o OUT           Take content out of an area for good\n"
             "  nup       FILE     -o OUT           Put several pages on one sheet\n"
             "  annotate  FILE                      List, add, remove or flatten comments\n"
             "  outline   FILE                      Show or rewrite the table of contents\n"
             "  form      FILE                      List, fill in or flatten form fields\n"
             "  text      FILE                      Show the text of a page, or correct it\n"
             "  batch     FILE...  --out-dir DIR    Do one thing to many files\n"
             "  compare   LEFT RIGHT                Show what changed between two documents\n"
             "  archive   FILE     -o OUT           Convert to PDF/A for long-term keeping\n"
             "  archive-check FILE                  Look for what stops a file being archival\n")
        + groupHelp()
        + i18n("\n"
               "Examples:\n"
               "  pdf-smithy-cli merge a.pdf b.pdf -o both.pdf\n"
               "  pdf-smithy-cli split invoices.pdf --every 1\n"
               "  pdf-smithy-cli pages report.pdf --keep 1-3,7 --rotate 4:90 -o short.pdf\n"
               "  pdf-smithy-cli compress scan.pdf --level strong -o small.pdf\n"
               "  pdf-smithy-cli ocr scan.pdf --languages deu+eng --straighten -o searchable.pdf\n"
               "  pdf-smithy-cli sign contract.pdf --image signature.png --pages last -o signed.pdf\n"
               "  pdf-smithy-cli watermark offer.pdf --text DRAFT --opacity 20 -o marked.pdf\n"
               "  pdf-smithy-cli meta payslip.pdf --sanitize -o clean.pdf\n"
               "  pdf-smithy-cli protect contract.pdf --password geheim -o locked.pdf\n"
               "  pdf-smithy-cli from-images scan*.png --page-size a4 -o scans.pdf\n"
               "  pdf-smithy-cli export-images report.pdf --pages 1-3 --dpi 300\n"
               "  pdf-smithy-cli number report.pdf --text \"{page} / {pages}\" -o numbered.pdf\n"
               "  pdf-smithy-cli number bundle.pdf --text {bates} --bates ACME- -o stamped.pdf\n"
               "  pdf-smithy-cli crop scan.pdf --auto -o trimmed.pdf\n"
               "  pdf-smithy-cli redact case.pdf --area 1:72,700,180,16 -o public.pdf\n"
               "  pdf-smithy-cli nup slides.pdf --per-sheet 4 --borders -o handout.pdf\n"
               "  pdf-smithy-cli annotate report.pdf --list\n"
               "  pdf-smithy-cli outline manual.pdf --json > toc.json\n"
               "  pdf-smithy-cli form application.pdf --list\n"
               "  pdf-smithy-cli text letter.pdf --page 1\n"
               "  pdf-smithy-cli batch scans/*.pdf --op ocr --out-dir searchable/\n"
               "  pdf-smithy-cli compare contract-v1.pdf contract-v2.pdf\n"
               "  pdf-smithy-cli archive invoice.pdf --level 2b -o invoice-archival.pdf\n"
               "  pdf-smithy-cli to-pdfx cover.pdf --pdfx-level x4 -o cover-print.pdf\n"
               "  pdf-smithy-cli tables report.pdf --pages 3-5\n"
               "  pdf-smithy-cli to-svg poster.pdf --pages 1-4 -o poster.svg\n"
               "  pdf-smithy-cli linearise handbook.pdf -o handbook-web.pdf\n"
               "  pdf-smithy-cli set-version old.pdf --pdf-version 1.7 -o modern.pdf\n"
               "  pdf-smithy-cli text letter.pdf --replace \"1:4=Sehr geehrte Frau Meier\" -o fixed.pdf\n"
               "  pdf-smithy-cli form application.pdf --set Name=Bueng --set Ort=Bern -o filled.pdf\n"
               "  pdf-smithy-cli annotate report.pdf --export-xfdf comments.xfdf\n"
               "  pdf-smithy-cli annotate report.pdf --note 2:120,600 --text \"Check this\" -o marked.pdf\n"
               "  pdf-smithy-cli split handbook.pdf --bookmarks\n"
               "\n"
               "Duplicating pages needs no separate command: a range may repeat a page,\n"
               "so \"pages FILE --keep 1,1,2-4\" writes page 1 twice."));

    parser.addPositionalArgument(QStringLiteral("command"), i18n("The command to run."));
    parser.addHelpOption();
    parser.addVersionOption();

    const QCommandLineOption outputOption({ QStringLiteral("o"), QStringLiteral("output") },
                                          i18n("Where to write the result."), QStringLiteral("file"));
    const QCommandLineOption jsonOption(QStringLiteral("json"), i18n("Machine-readable output."));
    const QCommandLineOption keepOption(QStringLiteral("keep"), i18n("Pages to keep, e.g. 1-3,7,9-last."),
                                        QStringLiteral("range"));
    const QCommandLineOption rotateOption(QStringLiteral("rotate"), i18n("Rotate pages, e.g. 1-3:90,7:180."),
                                          QStringLiteral("spec"));
    const QCommandLineOption everyOption(QStringLiteral("every"), i18n("Split into chunks of this many pages."),
                                         QStringLiteral("n"), QStringLiteral("1"));
    const QCommandLineOption rangesOption(
        QStringLiteral("ranges"), i18n("Split into one file per range, e.g. 1-3,4-9."), QStringLiteral("ranges"));
    const QCommandLineOption maxSizeOption(
        QStringLiteral("max-size"), i18n("Split into pieces no larger than this, e.g. 5M."), QStringLiteral("size"));
    const QCommandLineOption levelOption(
        QStringLiteral("level"), i18n("Compression: lossless, balanced, strong or extreme."), QStringLiteral("level"));
    const QCommandLineOption grayOption(QStringLiteral("grayscale"), i18n("Convert to grayscale while compressing."));
    const QCommandLineOption languagesOption(QStringLiteral("languages"), i18n("OCR languages, e.g. deu+eng."),
                                             QStringLiteral("codes"));
    const QCommandLineOption straightenOption(QStringLiteral("straighten"), i18n("Correct crooked scans."));
    const QCommandLineOption imageOption(QStringLiteral("image"), i18n("Signature image to stamp."),
                                         QStringLiteral("file"));
    const QCommandLineOption atOption(QStringLiteral("at"), i18n("Where to put it: bottom-right, centre, top-left…"),
                                      QStringLiteral("position"), QStringLiteral("bottom-right"));
    const QCommandLineOption sizeOption(QStringLiteral("size"), i18n("Width as a fraction of the page, e.g. 0.3."),
                                        QStringLiteral("fraction"), QStringLiteral("0.3"));
    // Points, here and everywhere else this name appears. It used to mean
    // millimetres to "typeset" and points to "sign", which is the same word for
    // two lengths nearly three times apart; typeset now spells its own margins
    // --margin-mm and turns this one down rather than reading it.
    const QCommandLineOption marginOption(QStringLiteral("margin"), i18n("Distance from the page edge, in points."),
                                          QStringLiteral("points"), QStringLiteral("36"));
    const QCommandLineOption textOption(QStringLiteral("text"), i18n("Watermark text."), QStringLiteral("text"));
    const QCommandLineOption angleOption(QStringLiteral("angle"), i18n("Watermark angle in degrees."),
                                         QStringLiteral("degrees"), QStringLiteral("45"));
    const QCommandLineOption opacityOption(QStringLiteral("opacity"), i18n("Watermark strength, 1 to 100 percent."),
                                           QStringLiteral("percent"), QStringLiteral("22"));
    const QCommandLineOption colourOption(QStringLiteral("colour"), i18n("Watermark colour, a name or #rrggbb."),
                                          QStringLiteral("colour"));
    const QCommandLineOption outlineOption(QStringLiteral("outline"),
                                           i18n("Draw the watermark as an outline, keeping the page readable."));
    const QCommandLineOption pagesOption(QStringLiteral("pages"), i18n("Which pages, e.g. 1-3,last. Default: all."),
                                         QStringLiteral("range"));
    const QCommandLineOption pageSizeOption(
        QStringLiteral("page-size"), i18n("Fit images into a4, a3, a5, letter or legal."), QStringLiteral("size"));
    const QCommandLineOption dpiOption(QStringLiteral("dpi"), i18n("Resolution for image export, in dots per inch."),
                                       QStringLiteral("number"), QStringLiteral("150"));
    const QCommandLineOption formatOption(QStringLiteral("format"), i18n("Image format: png, jpeg or tiff."),
                                          QStringLiteral("name"), QStringLiteral("png"));
    const QCommandLineOption qualityOption(QStringLiteral("quality"),
                                           i18n("JPEG quality when importing, 0 keeps the pixels."),
                                           QStringLiteral("number"), QStringLiteral("85"));
    const QCommandLineOption sanitizeOption(QStringLiteral("sanitize"),
                                            i18n("Strip metadata, JavaScript and embedded files."));
    const QCommandLineOption passwordOption(QStringLiteral("password"), i18n("Password to set."),
                                            QStringLiteral("text"));
    const QCommandLineOption openWithOption(QStringLiteral("open-with"),
                                            i18n("Password the input already has, if any."), QStringLiteral("text"));
    const QCommandLineOption removeOption(QStringLiteral("remove"),
                                          i18n("Remove the password instead of setting one."));
    const QCommandLineOption noPrintOption(QStringLiteral("no-printing"), i18n("Ask readers not to print."));
    const QCommandLineOption noCopyOption(QStringLiteral("no-copying"), i18n("Ask readers not to copy text."));
    const QCommandLineOption bookmarksOption(QStringLiteral("bookmarks"), i18n("Split wherever a bookmark starts."));
    const QCommandLineOption iccOption(
        QStringLiteral("icc-profile"),
        i18n("A colour profile to embed: sRGB for “archive”, a CMYK printing condition for “to-pdfx”. Leave out to "
             "look for one on the system."),
        QStringLiteral("file"));
    const QCommandLineOption titleOption(QStringLiteral("title"), i18n("Document title to write."),
                                         QStringLiteral("text"));
    const QCommandLineOption authorNameOption(QStringLiteral("author-name"), i18n("Document author to write."),
                                              QStringLiteral("text"));
    const QCommandLineOption toleranceOption(
        QStringLiteral("tolerance"),
        i18n("How different two pixels may be before they count as changed, in levels of brightness out of 255. "
             "On “colour replace” it is a distance in colour between 0 and 1."),
        QStringLiteral("0-255"), QStringLiteral("12"));
    const QCommandLineOption keepWhitespaceOption(QStringLiteral("keep-whitespace"),
                                                  i18n("Treat a different line break as a change."));
    const QCommandLineOption visualiseOption(QStringLiteral("visualise"),
                                             i18n("Write a side-by-side picture of one page, with -o."),
                                             QStringLiteral("page"));
    const QCommandLineOption outDirOption(QStringLiteral("out-dir"), i18n("Folder to write the finished files into."),
                                          QStringLiteral("folder"));
    const QCommandLineOption operationOption(QStringLiteral("op"),
                                             i18n("What to do to each file: compress, sanitize, flatten-comments, "
                                                  "flatten-form or ocr."),
                                             QStringLiteral("name"), QStringLiteral("compress"));
    const QCommandLineOption replaceOption(QStringLiteral("replace"),
                                           i18n("Replace a line, as PAGE:LINE=New text. Repeat for more."),
                                           QStringLiteral("spec"));
    const QCommandLineOption linePageOption(QStringLiteral("page"), i18n("Which page to list the text of."),
                                            QStringLiteral("number"), QStringLiteral("1"));
    const QCommandLineOption setFieldOption(
        QStringLiteral("set"), i18n("Fill in a field, as NAME=VALUE. Repeat for more."), QStringLiteral("pair"));
    const QCommandLineOption bookmarkOption(
        QStringLiteral("add"), i18n("Add a bookmark, as PAGE:Title. Repeat for more."), QStringLiteral("spec"));
    const QCommandLineOption fromJsonOption(
        QStringLiteral("from-json"), i18n("Replace the table of contents from a JSON file, as --json writes it."),
        QStringLiteral("file"));
    const QCommandLineOption exportXfdfOption(QStringLiteral("export-xfdf"),
                                              i18n("Write the comments to an XFDF file, without the document."),
                                              QStringLiteral("file"));
    const QCommandLineOption importXfdfOption(
        QStringLiteral("import-xfdf"), i18n("Read comments from an XFDF file and add them."), QStringLiteral("file"));
    const QCommandLineOption listOption(QStringLiteral("list"), i18n("Show the comments instead of changing them."));
    const QCommandLineOption highlightOption(QStringLiteral("highlight"),
                                             i18n("Highlight an area, as PAGE:X,Y,WIDTH,HEIGHT. Repeat for more."),
                                             QStringLiteral("spec"));
    const QCommandLineOption noteOption(QStringLiteral("note"), i18n("Put a note marker at PAGE:X,Y. Repeat for more."),
                                        QStringLiteral("spec"));
    // Not --remove: that already means "remove the password" on protect, and
    // one flag with two meanings is how someone deletes the wrong thing.
    const QCommandLineOption clearOption(QStringLiteral("clear"),
                                         i18n("Take every comment out, leaving form fields alone."));
    const QCommandLineOption authorOption(QStringLiteral("author"), i18n("Name to put on the comment."),
                                          QStringLiteral("name"));
    const QCommandLineOption flattenOption(QStringLiteral("flatten"),
                                           i18n("Draw the comments into the pages and remove them."));
    const QCommandLineOption perSheetOption(QStringLiteral("per-sheet"),
                                            i18n("How many pages go on one sheet, e.g. 2, 4 or 9."),
                                            QStringLiteral("number"), QStringLiteral("4"));
    const QCommandLineOption gridOption(QStringLiteral("grid"), i18n("Exact arrangement as COLUMNSxROWS, e.g. 2x3."),
                                        QStringLiteral("spec"));
    const QCommandLineOption gutterOption(QStringLiteral("gutter"), i18n("Gap between the slots, in points."),
                                          QStringLiteral("points"), QStringLiteral("8"));
    const QCommandLineOption marginOption2(QStringLiteral("sheet-margin"),
                                           i18n("Empty border around the sheet, in points."), QStringLiteral("points"),
                                           QStringLiteral("14"));
    const QCommandLineOption bordersOption(QStringLiteral("borders"), i18n("Draw a hairline around each slot."));
    const QCommandLineOption downFirstOption(QStringLiteral("down-first"), i18n("Fill downwards before moving right."));
    const QCommandLineOption areaOption(QStringLiteral("area"),
                                        i18n("Area to redact, as PAGE:X,Y,WIDTH,HEIGHT in points from the "
                                             "bottom-left corner. Repeat for more."),
                                        QStringLiteral("spec"));
    const QCommandLineOption keepVisibleOption(QStringLiteral("no-box"),
                                               i18n("Leave the gap open instead of painting it black."));
    const QCommandLineOption autoOption(QStringLiteral("auto"), i18n("Find the empty border and trim it."));
    const QCommandLineOption resetOption(QStringLiteral("reset"), i18n("Remove the crop instead of setting one."));
    const QCommandLineOption leftOption(QStringLiteral("left"), i18n("Points to trim from the left."),
                                        QStringLiteral("points"), QStringLiteral("0"));
    const QCommandLineOption rightOption(QStringLiteral("right"), i18n("Points to trim from the right."),
                                         QStringLiteral("points"), QStringLiteral("0"));
    const QCommandLineOption topOption(QStringLiteral("top"), i18n("Points to trim from the top."),
                                       QStringLiteral("points"), QStringLiteral("0"));
    const QCommandLineOption bottomOption(QStringLiteral("bottom"), i18n("Points to trim from the bottom."),
                                          QStringLiteral("points"), QStringLiteral("0"));
    const QCommandLineOption batesOption(QStringLiteral("bates"), i18n("Prefix for Bates numbering, e.g. ACME-."),
                                         QStringLiteral("prefix"));
    const QCommandLineOption digitsOption(QStringLiteral("digits"), i18n("Zero padding for the Bates number."),
                                          QStringLiteral("number"), QStringLiteral("6"));
    const QCommandLineOption startOption(QStringLiteral("start"), i18n("Number the first stamped page as this."),
                                         QStringLiteral("number"), QStringLiteral("1"));
    const QCommandLineOption fontSizeOption(QStringLiteral("font-size"), i18n("Text size in points."),
                                            QStringLiteral("points"), QStringLiteral("10"));

    const QCommandLineOption forceOption(QStringLiteral("force"),
                                         i18n("Recognise text even on pages that already have some."));

    parser.addOptions({ outputOption,      jsonOption,       keepOption,       rotateOption,
                        everyOption,       rangesOption,     maxSizeOption,    levelOption,
                        grayOption,        languagesOption,  straightenOption, forceOption,
                        imageOption,       atOption,         sizeOption,       marginOption,
                        textOption,        angleOption,      opacityOption,    colourOption,
                        outlineOption,     pagesOption,      pageSizeOption,   dpiOption,
                        formatOption,      qualityOption,    sanitizeOption,   passwordOption,
                        openWithOption,    removeOption,     noPrintOption,    noCopyOption,
                        bookmarksOption,   autoOption,       resetOption,      leftOption,
                        rightOption,       topOption,        bottomOption,     batesOption,
                        digitsOption,      startOption,      fontSizeOption,   areaOption,
                        keepVisibleOption, perSheetOption,   gridOption,       gutterOption,
                        marginOption2,     bordersOption,    downFirstOption,  listOption,
                        highlightOption,   noteOption,       clearOption,      authorOption,
                        flattenOption,     exportXfdfOption, importXfdfOption, bookmarkOption,
                        fromJsonOption,    setFieldOption,   replaceOption,    linePageOption,
                        outDirOption,      operationOption,  toleranceOption,  keepWhitespaceOption,
                        visualiseOption,   iccOption,        titleOption,      authorNameOption });

    // The editing areas each register the options only they use, before the
    // line is parsed. Kept out of the list above deliberately: an option that
    // belongs to one command should be declared next to it, not in a table that
    // every area has to edit.
    const QVector<ps::cli::Group> groups = commandGroups();
    for (const ps::cli::Group &group : groups) {
        if (group.options) {
            group.options(parser);
        }
    }

    parser.process(application);

    QStringList arguments = parser.positionalArguments();
    if (arguments.isEmpty()) {
        err() << parser.helpText();
        return UsageError;
    }

    const QString command = arguments.takeFirst().toLower();
    const QString output = parser.value(outputOption);

    if (command == QLatin1String("info")) {
        return commandInfo(arguments, parser.isSet(jsonOption));
    }
    if (command == QLatin1String("merge")) {
        return commandMerge(arguments, output);
    }
    if (command == QLatin1String("pages")) {
        return commandPages(arguments, output, parser.value(keepOption), parser.value(rotateOption));
    }
    if (command == QLatin1String("split")) {
        return commandSplit(arguments, output, parser.value(everyOption).toInt(), parser.value(rangesOption),
                            parser.value(maxSizeOption), parser.isSet(bookmarksOption));
    }
    if (command == QLatin1String("compress")) {
        return commandCompress(arguments, output, parser.value(levelOption), parser.isSet(grayOption));
    }
    if (command == QLatin1String("number")) {
        return commandNumber(arguments, output, parser.value(pagesOption), parser.value(textOption),
                             parser.value(atOption), parser.value(fontSizeOption).toDouble(), parser.value(batesOption),
                             parser.value(digitsOption).toInt(), parser.value(startOption).toInt());
    }
    if (command == QLatin1String("crop")) {
        return commandCrop(arguments, output, parser.value(pagesOption), parser.isSet(autoOption),
                           parser.isSet(resetOption), parser.value(leftOption).toDouble(),
                           parser.value(rightOption).toDouble(), parser.value(topOption).toDouble(),
                           parser.value(bottomOption).toDouble());
    }
    if (command == QLatin1String("archive")) {
        return commandArchive(arguments, output, parser.value(levelOption), parser.value(iccOption),
                              parser.value(titleOption), parser.value(authorNameOption));
    }
    if (command == QLatin1String("archive-check")) {
        return commandArchiveCheck(arguments);
    }
    if (command == QLatin1String("compare")) {
        return commandCompare(arguments, output, parser.isSet(jsonOption), parser.value(toleranceOption).toInt(),
                              parser.isSet(keepWhitespaceOption), parser.value(dpiOption).toDouble(),
                              parser.value(visualiseOption));
    }
    if (command == QLatin1String("batch")) {
        return commandBatch(arguments, parser.value(outDirOption), parser.value(operationOption),
                            parser.value(levelOption), parser.value(languagesOption), parser.isSet(straightenOption));
    }
    if (command == QLatin1String("text")) {
        return commandText(arguments, output, parser.value(linePageOption).toInt(), parser.isSet(jsonOption),
                           parser.values(replaceOption));
    }
    if (command == QLatin1String("form")) {
        return commandForm(arguments, output, parser.isSet(jsonOption), parser.isSet(flattenOption),
                           parser.values(setFieldOption));
    }
    if (command == QLatin1String("outline")) {
        return commandOutline(arguments, output, parser.isSet(jsonOption), parser.isSet(clearOption),
                              parser.values(bookmarkOption), parser.value(fromJsonOption));
    }
    if (command == QLatin1String("annotate")) {
        return commandAnnotate(arguments, output, parser.isSet(listOption), parser.isSet(jsonOption),
                               parser.isSet(clearOption), parser.isSet(flattenOption), parser.values(highlightOption),
                               parser.values(noteOption), parser.value(textOption), parser.value(colourOption),
                               parser.value(pagesOption), parser.value(authorOption), parser.value(exportXfdfOption),
                               parser.value(importXfdfOption));
    }
    if (command == QLatin1String("nup")) {
        return commandNUp(arguments, output, parser.value(perSheetOption).toInt(), parser.value(gridOption),
                          parser.value(marginOption2).toDouble(), parser.value(gutterOption).toDouble(),
                          parser.isSet(bordersOption), parser.isSet(downFirstOption));
    }
    if (command == QLatin1String("redact")) {
        return commandRedact(arguments, output, parser.values(areaOption), parser.isSet(keepVisibleOption));
    }
    if (command == QLatin1String("from-images")) {
        return commandFromImages(arguments, output, parser.value(pageSizeOption), parser.value(qualityOption).toInt());
    }
    if (command == QLatin1String("export-images")) {
        return commandExportImages(arguments, output, parser.value(pagesOption), parser.value(dpiOption).toInt(),
                                   parser.value(formatOption));
    }
    if (command == QLatin1String("meta")) {
        return commandMeta(arguments, output, parser.isSet(sanitizeOption), parser.isSet(jsonOption));
    }
    if (command == QLatin1String("protect")) {
        return commandProtect(arguments, output, parser.value(passwordOption), parser.isSet(removeOption),
                              parser.value(openWithOption), parser.isSet(noPrintOption), parser.isSet(noCopyOption));
    }
    if (command == QLatin1String("sign")) {
        return commandSign(arguments, output, parser.value(imageOption), parser.value(pagesOption),
                           parser.value(atOption), parser.value(sizeOption).toDouble(),
                           parser.value(marginOption).toDouble());
    }
    if (command == QLatin1String("watermark")) {
        return commandWatermark(arguments, output, parser.value(textOption), parser.value(pagesOption),
                                parser.value(angleOption).toInt(), parser.value(opacityOption).toInt(),
                                parser.value(colourOption), parser.isSet(outlineOption));
    }
    if (command == QLatin1String("ocr")) {
#ifdef PS_WITH_OCR
        return commandOcr(arguments, output, parser.value(languagesOption), parser.isSet(straightenOption),
                          parser.isSet(forceOption));
#else
        return fail(i18n("This build has no OCR support."), UsageError);
#endif
    }

    for (const ps::cli::Group &group : groups) {
        if (!group.run) {
            continue;
        }
        if (const std::optional<int> handled = group.run(command, arguments, parser)) {
            return *handled;
        }
    }

    err() << i18n("Unknown command “%1”.", command) << Qt::endl << Qt::endl << parser.helpText();
    return UsageError;
}
