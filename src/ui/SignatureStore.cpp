/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "SignatureStore.h"

#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QUuid>

#include <algorithm>

namespace ps {

QString SignatureStore::directory()
{
    const QString path
        = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QLatin1String("/signatures");
    QDir().mkpath(path);
    return path;
}

QVector<SignatureStore::Entry> SignatureStore::load()
{
    QVector<Entry> entries;

    QDir dir(directory());
    const QStringList files = dir.entryList({ QStringLiteral("*.png") }, QDir::Files, QDir::Time);
    entries.reserve(files.size());

    for (const QString &file : files) {
        QImage image;
        if (!image.load(dir.filePath(file))) {
            continue;
        }
        Entry entry;
        entry.id = QFileInfo(file).completeBaseName();
        // The visible name is stored in the PNG itself, so renaming never gets
        // out of step with the file it describes.
        entry.name = image.text(QStringLiteral("Title"));
        if (entry.name.isEmpty()) {
            entry.name = entry.id;
        }
        entry.image = image;
        entries.append(entry);
    }

    return entries;
}

QString SignatureStore::save(const QImage &image, const QString &name)
{
    if (image.isNull()) {
        return {};
    }

    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces).left(12);
    QImage stored = image.convertToFormat(QImage::Format_ARGB32);
    stored.setText(QStringLiteral("Title"), name);

    if (!stored.save(QDir(directory()).filePath(id + QLatin1String(".png")), "PNG")) {
        return {};
    }
    return id;
}

bool SignatureStore::remove(const QString &id)
{
    if (id.isEmpty()) {
        return false;
    }
    return QFile::remove(QDir(directory()).filePath(id + QLatin1String(".png")));
}

QImage SignatureStore::removeBackground(const QImage &source, int threshold)
{
    if (source.isNull()) {
        return {};
    }

    QImage result = source.convertToFormat(QImage::Format_ARGB32);
    const int cutoff = std::clamp(threshold, 1, 254);

    // Everything above the cutoff is paper, everything well below it is ink,
    // and the band in between fades, which is what stops the stroke edges
    // turning into a staircase.
    const int softness = 60;

    for (int y = 0; y < result.height(); ++y) {
        auto *line = reinterpret_cast<QRgb *>(result.scanLine(y));
        for (int x = 0; x < result.width(); ++x) {
            const QRgb pixel = line[x];
            const int luminance = qGray(pixel);

            int alpha = 255;
            if (luminance >= cutoff) {
                alpha = 0;
            } else if (luminance > cutoff - softness) {
                alpha = 255 * (cutoff - luminance) / softness;
            }

            // Keep the ink's own colour, only its coverage changes.
            line[x] = qRgba(qRed(pixel), qGreen(pixel), qBlue(pixel), std::min(alpha, qAlpha(pixel)));
        }
    }

    return result;
}

QImage SignatureStore::trim(const QImage &source)
{
    if (source.isNull()) {
        return {};
    }

    const QImage image = source.convertToFormat(QImage::Format_ARGB32);
    int left = image.width();
    int right = -1;
    int top = image.height();
    int bottom = -1;

    for (int y = 0; y < image.height(); ++y) {
        const auto *line = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            if (qAlpha(line[x]) > 8) {
                left = std::min(left, x);
                right = std::max(right, x);
                top = std::min(top, y);
                bottom = std::max(bottom, y);
            }
        }
    }

    if (right < left || bottom < top) {
        return image; // nothing but empty space; leave it alone
    }

    // A couple of pixels of air, so the stroke does not touch the edge.
    const int pad = 2;
    const QRect box
        = QRect(QPoint(left, top), QPoint(right, bottom)).adjusted(-pad, -pad, pad, pad).intersected(image.rect());
    return image.copy(box);
}

} // namespace ps
