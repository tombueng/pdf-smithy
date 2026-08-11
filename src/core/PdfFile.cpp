/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#include "PdfFile.h"

#include <KLocalizedString>

#include <QFile>
#include <QTimeZone>

#include <qpdf/QPDF.hh>

#include <cmath>

namespace ps::PdfFile {

void open(QPDF &pdf, const QString &path)
{
    // Set before processing, or the warnings raised while reading are already
    // on their way to stderr by the time we ask for quiet.
    pdf.setSuppressWarnings(true);
    pdf.processFile(QFile::encodeName(path).constData());
}

void open(QPDF &pdf, const QString &path, const QString &password)
{
    pdf.setSuppressWarnings(true);
    pdf.processFile(QFile::encodeName(path).constData(), password.toUtf8().constData());
}

QString formatDate(const QDateTime &stamp)
{
    if (!stamp.isValid()) {
        return {};
    }
    const int offsetMinutes = stamp.offsetFromUtc() / 60;
    return QStringLiteral("D:%1%2%3'%4'")
        .arg(stamp.toString(QStringLiteral("yyyyMMddHHmmss")),
             offsetMinutes < 0 ? QStringLiteral("-") : QStringLiteral("+"),
             QString::number(std::abs(offsetMinutes) / 60).rightJustified(2, QLatin1Char('0')),
             QString::number(std::abs(offsetMinutes) % 60).rightJustified(2, QLatin1Char('0')));
}

QDateTime parseDate(const QString &raw)
{
    QString text = raw.trimmed();
    if (text.startsWith(QLatin1String("D:"))) {
        text = text.mid(2);
    }
    if (text.size() < 14) {
        return {};
    }
    QDateTime stamp = QDateTime::fromString(text.left(14), QStringLiteral("yyyyMMddHHmmss"));
    // The zone suffix is optional in the wild; local time is the conventional
    // reading when it is missing.
    stamp.setTimeZone(QTimeZone::LocalTime);
    return stamp;
}

QString warningSummary(QPDF &pdf)
{
    // getWarnings() empties the list, so this reports each batch once.
    const size_t count = pdf.getWarnings().size();
    if (count == 0) {
        return {};
    }
    return i18np("The file does not quite follow the PDF specification in one place. It was read anyway, and "
                 "the result should be sound.",
                 "The file does not quite follow the PDF specification in %1 places. It was read anyway, and "
                 "the result should be sound.",
                 int(count));
}

} // namespace ps::PdfFile
