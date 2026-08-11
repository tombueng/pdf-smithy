/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#pragma once

#include <QString>
#include <QStringList>

namespace ps {

/**
 * PDF/A: the shape a document has to be in before an archive will take it.
 *
 * The standard is mostly a list of things a file may not do: no encryption, no
 * scripts, no external dependencies, and above all no font it does not carry
 * itself, because a document that borrows Helvetica from the reader's machine
 * stops looking like itself the moment that machine is gone. Fixing all of that
 * means rewriting the document, which Ghostscript already does well.
 *
 * Ghostscript is run as a separate process and never linked, exactly as
 * Compressor does it, so its AGPL obligation stays outside this codebase and
 * the dependency stays optional.
 *
 * The other half is inspection. Note what it is *not* called: a real PDF/A
 * validator is a substantial piece of software (veraPDF is the reference one),
 * and a few hundred lines of QPDF calls cannot honestly claim the word
 * "validate". What is here finds the handful of faults that account for most
 * rejections and says so in those terms.
 */
class Archival
{
public:
    enum class Level {
        PdfA1b, //!< the oldest and strictest; no transparency, no layers
        PdfA2b, //!< what most archives ask for today
        PdfA3b, //!< as A-2, but a file may be carried along inside the document
    };

    struct Options {
        Level level = Level::PdfA2b;

        /** An sRGB ICC profile to embed. Empty means look for one on the system. */
        QString iccProfilePath;

        QString title;
        QString author;
    };

    struct Report {
        bool converted = false;

        /** What the conversion had to change, in words a user can act on. */
        QStringList changes;

        /** What could not be fixed and may still fail a strict check. */
        QStringList warnings;
    };

    /** Whether Ghostscript, which does the conversion, is installed. */
    static bool isAvailable();

    static bool convert(const QString &inputPdf, const QString &outputPdf, const Options &options, Report *report,
                        QString *error);

    struct Findings {
        /** The level the file claims in its XMP metadata, or empty. */
        QString claimedLevel;

        QStringList problems;

        bool looksArchival = false;
    };

    /**
     * Inspects a file for the things that stop it being archival.
     *
     * Deliberately not called "validate": a real PDF/A validator is a large
     * piece of software (veraPDF) and pretending otherwise would be dishonest.
     * This finds the common, actionable problems and says that is what it does.
     */
    static Findings inspect(const QString &pdf, QString *error);

    static QString describe(Level level);

    /** Plain-language notes about what the inspection cannot promise. */
    static QStringList inspectionLimitations();
};

} // namespace ps
