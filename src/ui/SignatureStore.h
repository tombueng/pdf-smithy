/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#pragma once

#include <QImage>
#include <QString>
#include <QVector>

namespace ps {

/**
 * The signatures the user has captured, kept between sessions.
 *
 * Signing a contract is a repeated act, so the signature is captured once and
 * then reused, which is exactly what makes it worth storing rather than
 * asking for a file every time.
 *
 * Files live under the application's data directory as PNGs with alpha. They
 * never leave the machine, and they are deliberately *not* put in the config
 * file: a signature is an image, and images belong in files where the user can
 * find, back up and delete them.
 */
class SignatureStore
{
public:
    struct Entry {
        QString id; //!< file base name, stable across sessions
        QString name; //!< what the user called it
        QImage image; //!< with alpha
    };

    /** Directory the signatures live in, created if needed. */
    static QString directory();

    static QVector<Entry> load();

    /** Stores @p image under @p name and returns its id, or empty on failure. */
    static QString save(const QImage &image, const QString &name);

    static bool remove(const QString &id);

    /**
     * Turns the paper around a scanned signature transparent.
     *
     * A photographed or scanned signature arrives as dark strokes on a light
     * background. Stamped as-is it covers the document with a white box, which
     * looks obviously pasted-on. Pixels brighter than @p threshold become fully
     * transparent and the ones just below it partly so, which keeps the stroke
     * edges soft instead of jagged.
     */
    static QImage removeBackground(const QImage &source, int threshold = 200);

    /** Crops away fully transparent margins so the stamp is all signature. */
    static QImage trim(const QImage &source);
};

} // namespace ps
