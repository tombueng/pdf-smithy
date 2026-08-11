/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#pragma once

#include "DocumentWriter.h"

#include <QString>
#include <QStringList>

namespace ps {

class Document;

/**
 * Breaks one document into several files.
 *
 * Splitting is not one operation but four questions people actually ask:
 * "every page separately", "these ranges", "chunks of N pages", "pieces small
 * enough to email". Each is a mode here rather than a separate feature.
 */
class Splitter
{
public:
    enum class Mode {
        EveryNPages, //!< fixed-size chunks; N = 1 gives one file per page
        Ranges, //!< one output per comma-separated group, e.g. "1-3, 4-9"
        MaxFileSize, //!< as many pages as fit under a byte ceiling
        Bookmarks, //!< a new file wherever a top-level bookmark starts
    };

    struct Options {
        Mode mode = Mode::EveryNPages;

        int pagesPerFile = 1;

        /** For Mode::Ranges: each comma group becomes one file. */
        QString ranges;

        /** For Mode::MaxFileSize, in bytes. */
        qint64 maxBytes = 5 * 1024 * 1024;

        /**
         * Output template containing one %1 placeholder for the running
         * number, e.g. "/home/tom/contract-%1.pdf". Numbers are zero-padded
         * to the width the result needs, so files sort correctly.
         */
        QString outputTemplate;

        DocumentWriter::Options writer;

        /**
         * For Mode::Bookmarks: the file the outline is read from.
         *
         * Needed because the page list may span several sources, and only a
         * file on disk has an outline to read.
         */
        QString bookmarkSource;

        /** Use the bookmark titles as file names rather than numbering them. */
        bool nameFilesAfterBookmarks = true;
    };

    struct Result {
        bool success = false;
        QStringList files;
        QString error;
    };

    /** A top-level bookmark: where it points, and what it is called. */
    struct Chapter {
        int firstPage = 0; //!< zero-based
        QString title;
    };

    /**
     * The document's top-level outline, in page order.
     *
     * Exposed so the dialog can show what the split would produce before
     * anyone commits to writing twenty files.
     */
    static QVector<Chapter> chaptersOf(const QString &pdfPath);

    static Result split(const Document &document, const Options &options);

    /**
     * Fills in the placeholder without writing anything, used by the dialog
     * to show what the resulting file names will look like.
     */
    static QString expandTemplate(const QString &outputTemplate, int number, int totalCount);
};

} // namespace ps
