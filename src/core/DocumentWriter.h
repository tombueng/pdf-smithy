/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#pragma once

#include "Encryption.h"

#include <QString>
#include <QVector>

namespace ps {

class Document;

/**
 * Assembles a Document's page list into a real PDF file.
 *
 * This is the only place where PDF objects are created, and it is deliberately
 * dumb: it copies pages verbatim from their sources and applies rotation. No
 * re-rendering, no re-compression, no quality loss. Reordering a scanned
 * contract and saving it must produce the same image bytes that went in.
 */
class DocumentWriter
{
public:
    struct Options {
        /** Web-optimised layout: slower to write, faster to open remotely. */
        bool linearize = false;

        /** Pack objects into object streams. Smaller files, PDF 1.5+. */
        bool objectStreams = true;

        /** Compress content streams that are not already compressed. */
        bool compressStreams = true;

        /**
         * Derive the file ID from the content instead of the clock, so that
         * saving the same document twice yields byte-identical files. Makes
         * the test suite meaningful and version control diffs sane.
         */
        bool deterministicId = true;

        /**
         * Drop annotation actions and attachments while writing.
         *
         * Assembling output from scratch already leaves document-level
         * JavaScript and embedded files behind, because nothing in the page
         * tree points at them. What survives is what hangs off the pages
         * themselves, and this removes that too.
         */
        /**
         * Carry the table of contents across.
         *
         * On by default and rarely worth turning off; it exists so that
         * extracting a handful of pages for processing does not drag a whole
         * book's navigation along with them.
         */
        bool keepOutline = true;

        bool stripInteractivity = false;

        /**
         * Keep the document locked when it is written out.
         *
         * A document assembled from scratch carries no encryption of its own,
         * so without this every save quietly unlocks a file the user had
         * locked: they set a password, went on working, pressed save, and the
         * protection was gone with nothing said. An empty user password means
         * no encryption, which is the ordinary case.
         */
        QString userPassword;
        QString ownerPassword;
        Encryption::Permissions permissions;
    };

    /** Writes every page. */
    static bool write(const Document &document, const QString &path, const Options &options, QString *error);

    /**
     * Writes only @p pageIndexes, in the order given. This is the engine
     * behind extract, split and "export selection".
     */
    static bool writeSelection(const Document &document, const QVector<int> &pageIndexes, const QString &path,
                               const Options &options, QString *error);
};

} // namespace ps
