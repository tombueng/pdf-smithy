/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#include "Splitter.h"
#include "PdfFile.h"

#include "Document.h"
#include "PageRange.h"

#include <qpdf/QPDF.hh>
#include <qpdf/QPDFOutlineDocumentHelper.hh>
#include <qpdf/QPDFOutlineObjectHelper.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>

#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>

#include <KLocalizedString>

#include <algorithm>
#include <cmath>
#include <map>

namespace ps {

namespace {

int digitsFor(int count)
{
    return count < 10 ? 1 : static_cast<int>(std::floor(std::log10(count))) + 1;
}

} // namespace

QVector<Splitter::Chapter> Splitter::chaptersOf(const QString &pdfPath)
{
    QVector<Chapter> chapters;

    try {
        QPDF pdf;
        PdfFile::open(pdf, pdfPath);

        // A map from page object to index, so a destination can be turned into
        // a page number without searching the tree for every bookmark.
        std::map<QPDFObjGen, int> pageIndex;
        const std::vector<QPDFPageObjectHelper> pages = QPDFPageDocumentHelper(pdf).getAllPages();
        for (size_t i = 0; i < pages.size(); ++i) {
            pageIndex[pages[i].getObjectHandle().getObjGen()] = static_cast<int>(i);
        }

        QPDFOutlineDocumentHelper outlines(pdf);
        for (QPDFOutlineObjectHelper &entry : outlines.getTopLevelOutlines()) {
            QPDFObjectHandle destination = entry.getDestPage();
            if (destination.isNull()) {
                continue;
            }
            const auto it = pageIndex.find(destination.getObjGen());
            if (it == pageIndex.end()) {
                continue;
            }

            Chapter chapter;
            chapter.firstPage = it->second;
            chapter.title = QString::fromStdString(entry.getTitle()).simplified();
            chapters.append(chapter);
        }
    } catch (const std::exception &) {
        return {};
    }

    std::sort(chapters.begin(), chapters.end(),
              [](const Chapter &a, const Chapter &b) { return a.firstPage < b.firstPage; });

    // Two bookmarks on the same page would otherwise produce an empty file
    // between them.
    chapters.erase(std::unique(chapters.begin(), chapters.end(),
                               [](const Chapter &a, const Chapter &b) { return a.firstPage == b.firstPage; }),
                   chapters.end());
    return chapters;
}

QString Splitter::expandTemplate(const QString &outputTemplate, int number, int totalCount)
{
    const int width = digitsFor(std::max(1, totalCount));
    const QString padded = QStringLiteral("%1").arg(number, width, 10, QLatin1Char('0'));

    if (outputTemplate.contains(QLatin1String("%1"))) {
        return QString(outputTemplate).replace(QLatin1String("%1"), padded);
    }

    // No placeholder given: put the number before the extension rather than
    // silently overwriting the same file for every chunk.
    const QFileInfo info(outputTemplate);
    const QString suffix = info.completeSuffix();
    const QString base = suffix.isEmpty() ? outputTemplate : outputTemplate.chopped(suffix.size() + 1);
    return suffix.isEmpty() ? QStringLiteral("%1-%2").arg(base, padded)
                            : QStringLiteral("%1-%2.%3").arg(base, padded, suffix);
}

Splitter::Result Splitter::split(const Document &document, const Options &options)
{
    Result result;

    if (document.pageCount() == 0) {
        result.error = i18n("The document has no pages to split.");
        return result;
    }
    if (options.outputTemplate.isEmpty()) {
        result.error = i18n("No output file name was given.");
        return result;
    }

    QVector<QVector<int>> groups;
    QStringList titles;

    switch (options.mode) {
    case Mode::EveryNPages: {
        const int step = std::max(1, options.pagesPerFile);
        for (int start = 0; start < document.pageCount(); start += step) {
            QVector<int> group;
            for (int i = start; i < std::min(start + step, document.pageCount()); ++i) {
                group.append(i);
            }
            groups.append(group);
        }
        break;
    }

    case Mode::Ranges: {
        // Each comma group becomes its own file, so "1-3, 7-9" yields two
        // documents rather than one document of six pages.
        const QStringList parts = options.ranges.split(QLatin1Char(','), Qt::SkipEmptyParts);
        if (parts.isEmpty()) {
            result.error = i18n("No page ranges were given.");
            return result;
        }
        for (const QString &part : parts) {
            QString error;
            const QVector<int> pages = PageRange::parse(part, document.pageCount(), &error);
            if (pages.isEmpty()) {
                result.error = error;
                return result;
            }
            groups.append(pages);
        }
        break;
    }

    case Mode::Bookmarks: {
        const QVector<Chapter> chapters = chaptersOf(options.bookmarkSource);
        if (chapters.isEmpty()) {
            result.error = i18n("This document has no bookmarks to split on.");
            return result;
        }

        for (int i = 0; i < chapters.size(); ++i) {
            const int from = chapters.at(i).firstPage;
            const int to = (i + 1 < chapters.size()) ? chapters.at(i + 1).firstPage : document.pageCount();

            QVector<int> group;
            for (int page = from; page < to && page < document.pageCount(); ++page) {
                group.append(page);
            }
            if (!group.isEmpty()) {
                groups.append(group);
                titles.append(chapters.at(i).title);
            }
        }
        break;
    }

    case Mode::MaxFileSize: {
        if (options.maxBytes <= 0) {
            result.error = i18n("The maximum file size must be greater than zero.");
            return result;
        }

        // Grow a chunk one page at a time and write it out to see how big it
        // really got. Estimating from page count is unreliable, because one scanned
        // photo page can outweigh fifty pages of text.
        QVector<int> current;
        for (int page = 0; page < document.pageCount(); ++page) {
            QVector<int> candidate = current;
            candidate.append(page);

            const QString probePath = options.outputTemplate + QStringLiteral(".probe");
            QString error;
            if (!DocumentWriter::writeSelection(document, candidate, probePath, options.writer, &error)) {
                result.error = error;
                return result;
            }
            const qint64 size = QFileInfo(probePath).size();
            QFile::remove(probePath);

            if (size > options.maxBytes && !current.isEmpty()) {
                groups.append(current);
                current = { page };
            } else {
                current = candidate;
            }
        }
        if (!current.isEmpty()) {
            groups.append(current);
        }
        break;
    }
    }

    if (groups.isEmpty()) {
        result.error = i18n("The settings produce no files.");
        return result;
    }

    for (int i = 0; i < groups.size(); ++i) {
        QString path = expandTemplate(options.outputTemplate, i + 1, groups.size());

        // "03 - Kündigungsfristen.pdf" beats "part-03.pdf" for a document that
        // told us what its chapters are called.
        if (options.nameFilesAfterBookmarks && i < titles.size() && !titles.at(i).isEmpty()) {
            QString safe = titles.at(i);
            safe.replace(QRegularExpression(QStringLiteral("[/\\\\:*?\"<>|]")), QStringLiteral("-"));
            safe.truncate(80);
            const QFileInfo info(path);
            path = info.absolutePath() + QLatin1Char('/')
                + QStringLiteral("%1 - %2.%3")
                      .arg(i + 1, digitsFor(groups.size()), 10, QLatin1Char('0'))
                      .arg(safe.trimmed(), info.suffix().isEmpty() ? QStringLiteral("pdf") : info.suffix());
        }

        QString error;
        if (!DocumentWriter::writeSelection(document, groups.at(i), path, options.writer, &error)) {
            result.error = error;
            // Everything written so far is reported, so the caller can tell the
            // user what already exists on disk rather than leaving them guessing.
            return result;
        }
        result.files.append(path);
    }

    result.success = true;
    return result;
}

} // namespace ps
