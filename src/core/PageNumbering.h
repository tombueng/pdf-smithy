/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#pragma once

#include "Overlay.h"

#include <QColor>
#include <QString>
#include <QVector>

namespace ps {

/**
 * Page numbers, headers, footers and Bates stamping.
 *
 * All the same operation underneath (a short line of text in a corner that
 * counts up), so they share one implementation rather than three that drift
 * apart.
 *
 * Bates numbering matters more than it looks: legal and audit work needs every
 * page of a bundle to carry a unique, sequential, quotable identifier, and
 * doing that by hand across a four-hundred-page disclosure is how mistakes
 * happen.
 */
class PageNumbering
{
public:
    struct Options {
        /**
         * The text to stamp, with placeholders:
         *
         *   {page}   the running number
         *   {pages}  how many pages the run covers
         *   {bates}  the Bates number, prefix and zero padding applied
         *   {file}   the document's file name
         *   {date}   today, in the user's short format
         */
        QString text = QStringLiteral("{page} / {pages}");

        Overlay::Anchor anchor = Overlay::Anchor::BottomCentre;
        double fontSize = 10.0;
        QColor colour = QColor(60, 60, 60);
        double marginPoints = 28.0;

        /** What the first stamped page is called. */
        int startNumber = 1;

        /** Prefix in front of the Bates number, e.g. "ACME-". */
        QString batesPrefix;

        /** Zero padding for {bates}; six is the usual convention. */
        int batesDigits = 6;

        /** Substituted for {file}. */
        QString fileName;
    };

    /** Fills the placeholders for one page, so callers can show a preview. */
    static QString render(const Options &options, int indexInRun, int totalInRun);

    /** Stamps @p pages of @p inputPdf, in the order given. */
    static bool apply(const QString &inputPdf, const QString &outputPdf, const QVector<int> &pages,
                      const Options &options, QString *error);
};

} // namespace ps
