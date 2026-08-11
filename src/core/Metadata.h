/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#pragma once

#include <QDateTime>
#include <QString>

class QPDF;

namespace ps {

/**
 * What a PDF says about itself, and how to make it stop saying it.
 *
 * The reading half is ordinary. The stripping half is the point: a document
 * that has been through an office suite typically names the author, the
 * machine, the software and every editing session, and people send those
 * files to strangers without knowing. Since the whole argument for this
 * application is that documents stay private, being able to empty that out is
 * not a nicety.
 */
class Metadata
{
public:
    struct Fields {
        QString title;
        QString author;
        QString subject;
        QString keywords;
        QString creator; //!< the application the document was written in
        QString producer; //!< the library that wrote the file
        QDateTime created;
        QDateTime modified;

        bool isEmpty() const;
    };

    struct StripOptions {
        /** Title, author, dates and the rest of /Info, plus the XMP packet. */
        bool documentInfo = true;

        /** Document-level and annotation JavaScript. */
        bool javaScript = true;

        /** Files carried along inside the PDF. */
        bool embeddedFiles = true;
    };

    struct Findings {
        bool hasDocumentInfo = false;
        bool hasXmp = false;
        bool hasJavaScript = false;
        int embeddedFileCount = 0;

        bool anythingToStrip() const { return hasDocumentInfo || hasXmp || hasJavaScript || embeddedFileCount > 0; }
    };

    static bool read(const QString &path, Fields *fields, QString *error);

    static bool write(const QString &inputPath, const QString &outputPath, const Fields &fields, QString *error);

    /** Reports what is in there without changing anything. */
    static bool inspect(const QString &path, Findings *findings, QString *error);

    static bool strip(const QString &inputPath, const QString &outputPath, const StripOptions &options, QString *error);

    /**
     * Writes @p fields into an already-open document.
     *
     * Used by the writer, which assembles output from scratch and would
     * otherwise drop the document's metadata on every save without saying so.
     */
    static void applyTo(QPDF &pdf, const Fields &fields);

    /** Reads the fields from an already-open document. */
    static Fields readFrom(QPDF &pdf);
};

} // namespace ps
