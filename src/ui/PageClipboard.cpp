/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "PageClipboard.h"

#include <KLocalizedString>

#include <QDataStream>
#include <QIODevice>
#include <QImage>
#include <QMimeData>
#include <QVariant>

#include <algorithm>
#include <cmath>

using namespace Qt::Literals::StringLiterals;

namespace ps {

namespace {

/** "PSOB", so a payload from something else is refused rather than misread. */
constexpr quint32 Magic = 0x50534f42;

/** Raised when a field is added; an older reader stops at the mismatch. */
constexpr quint16 Format = 2;

/**
 * The stream version, pinned.
 *
 * Both ends of a clipboard round trip are separate processes, and Qt's default
 * version follows whichever Qt each was built against. Writing that down means a
 * parcel copied from one build can be pasted into another.
 */
constexpr QDataStream::Version Wire = QDataStream::Qt_6_0;

/** How far from the paper's edge content from another program is laid down. */
constexpr double ForeignMargin = 36.0;

/** Screen pixels are 96 to the inch and a point is 72, which is the whole sum. */
constexpr double PixelsToPoints = 72.0 / 96.0;

/** What text arriving from elsewhere is set in, until somebody says otherwise. */
constexpr double ForeignFontSize = 11.0;

/** The composer steps a line down by this much of the type size. */
constexpr double LineStep = 1.35;

QString describeItem(const NewContent &item)
{
    switch (item.kind) {
    case NewContent::Kind::Text:
        return item.text;
    case NewContent::Kind::Image:
        return i18nc("@info stands for a copied picture where only text can go", "[picture, %1 × %2 pixels]",
                     item.image.width(), item.image.height());
    case NewContent::Kind::Rectangle:
        return i18nc("@info stands for a copied shape where only text can go", "[rectangle]");
    case NewContent::Kind::Ellipse:
        return i18nc("@info stands for a copied shape where only text can go", "[ellipse]");
    case NewContent::Kind::Line:
        return i18nc("@info stands for a copied shape where only text can go", "[line]");
    }
    return {};
}

} // namespace

QString PageClipboard::objectsMimeType()
{
    return u"application/x-pdf-smithy-objects"_s;
}

QString PageClipboard::asPlainText(const Parcel &parcel)
{
    QStringList lines;
    for (const NewContent &item : parcel.items) {
        const QString one = describeItem(item);
        if (!one.isEmpty()) {
            lines.append(one);
        }
    }
    return lines.join(u'\n');
}

QMimeData *PageClipboard::pack(const Parcel &parcel)
{
    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);
    stream.setVersion(Wire);
    stream << Magic << Format << qint32(parcel.items.size());
    for (const NewContent &item : parcel.items) {
        // Field by field rather than as a struct: what goes on the clipboard is
        // read by a build that may lay NewContent out differently.
        stream << qint32(item.kind) << item.rect << item.placement << item.text << item.fontFamily << item.fontSize
               << item.image << qint32(item.jpegQuality) << item.fill << item.stroke << item.lineWidth << item.opacity;
    }
    stream << parcel.missing;

    auto *data = new QMimeData;
    data->setData(objectsMimeType(), payload);

    const QString text = asPlainText(parcel);
    if (!text.isEmpty()) {
        data->setText(text);
    }

    // One picture and nothing else also goes on as a picture, so that a logo
    // lifted off a page can be pasted straight into anything else on the desktop.
    if (parcel.items.size() == 1 && parcel.items.constFirst().kind == NewContent::Kind::Image
        && !parcel.items.constFirst().image.isNull()) {
        data->setImageData(parcel.items.constFirst().image);
    }
    return data;
}

bool PageClipboard::holdsObjects(const QMimeData *data)
{
    return data && data->hasFormat(objectsMimeType());
}

PageClipboard::Parcel PageClipboard::unpack(const QMimeData *data)
{
    Parcel parcel;
    if (!holdsObjects(data)) {
        return parcel;
    }

    QByteArray payload = data->data(objectsMimeType());
    QDataStream stream(&payload, QIODevice::ReadOnly);
    stream.setVersion(Wire);

    quint32 magic = 0;
    quint16 format = 0;
    qint32 count = 0;
    stream >> magic >> format >> count;
    if (magic != Magic || format != Format || count < 0) {
        return parcel;
    }

    parcel.items.reserve(count);
    for (qint32 i = 0; i < count; ++i) {
        NewContent item;
        qint32 kind = 0;
        qint32 quality = -1;
        stream >> kind >> item.rect >> item.placement >> item.text >> item.fontFamily >> item.fontSize >> item.image
            >> quality >> item.fill >> item.stroke >> item.lineWidth >> item.opacity;
        if (stream.status() != QDataStream::Ok) {
            // A truncated parcel is not half a parcel: whatever was read of it
            // would paste content nobody copied.
            return {};
        }
        item.kind = static_cast<NewContent::Kind>(kind);
        item.jpegQuality = quality;
        parcel.items.append(item);
    }
    stream >> parcel.missing;
    if (stream.status() != QDataStream::Ok) {
        return {};
    }
    return parcel;
}

PageClipboard::Parcel PageClipboard::fromForeign(const QMimeData *data, const QSizeF &pageSizePoints)
{
    Parcel parcel;
    if (!data) {
        return parcel;
    }

    const QSizeF paper = pageSizePoints.isEmpty() ? QSizeF(595.0, 842.0) : pageSizePoints;
    const double room = std::max(1.0, paper.width() - 2 * ForeignMargin);
    const double headroom = std::max(1.0, paper.height() - 2 * ForeignMargin);

    if (data->hasImage()) {
        const QImage image = qvariant_cast<QImage>(data->imageData());
        if (!image.isNull()) {
            NewContent item;
            item.kind = NewContent::Kind::Image;
            item.image = image;

            QSizeF size(image.width() * PixelsToPoints, image.height() * PixelsToPoints);
            if (size.width() > room || size.height() > headroom) {
                size.scale(room, headroom, Qt::KeepAspectRatio);
            }
            // Down from the top left corner, which is where a reader looks for
            // something that has just arrived.
            item.rect
                = QRectF(ForeignMargin, paper.height() - ForeignMargin - size.height(), size.width(), size.height());
            parcel.items.append(item);
            return parcel;
        }
    }

    if (data->hasText() && !data->text().trimmed().isEmpty()) {
        NewContent item;
        item.kind = NewContent::Kind::Text;
        item.text = data->text();
        item.fontSize = ForeignFontSize;
        item.fill = QColor(Qt::black);

        // Tall enough for every line the composer will set: it puts the first
        // baseline one type size below the top of the box and steps down from
        // there, so a box measured for one line would push the rest off it.
        const int lines = std::max(1, int(item.text.split(u'\n').size()));
        const double height = std::min(headroom, ForeignFontSize * (1.0 + LineStep * (lines - 1) + 0.3));
        item.rect = QRectF(ForeignMargin, paper.height() - ForeignMargin - height, room, height);
        parcel.items.append(item);
    }
    return parcel;
}

} // namespace ps
