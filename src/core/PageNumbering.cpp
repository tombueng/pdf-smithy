/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#include "PageNumbering.h"

#include <QDate>
#include <QHash>
#include <QLocale>

#include <KLocalizedString>

namespace ps {

QString PageNumbering::render(const Options &options, int indexInRun, int totalInRun)
{
    const int number = options.startNumber + indexInRun;

    QString bates = QString::number(number);
    if (options.batesDigits > 0) {
        bates = bates.rightJustified(options.batesDigits, QLatin1Char('0'));
    }
    bates.prepend(options.batesPrefix);

    QString text = options.text;
    text.replace(QLatin1String("{page}"), QString::number(number));
    text.replace(QLatin1String("{pages}"), QString::number(totalInRun));
    text.replace(QLatin1String("{bates}"), bates);
    text.replace(QLatin1String("{file}"), options.fileName);
    text.replace(QLatin1String("{date}"), QLocale().toString(QDate::currentDate(), QLocale::ShortFormat));
    return text;
}

bool PageNumbering::apply(const QString &inputPdf, const QString &outputPdf, const QVector<int> &pages,
                          const Options &options, QString *error)
{
    if (pages.isEmpty()) {
        if (error) {
            *error = i18n("There are no pages to number.");
        }
        return false;
    }
    if (options.text.trimmed().isEmpty()) {
        if (error) {
            *error = i18n("The text to stamp is empty.");
        }
        return false;
    }

    QHash<int, Overlay::TextStamp> stamps;
    stamps.reserve(pages.size());

    for (int i = 0; i < pages.size(); ++i) {
        Overlay::TextStamp stamp;
        stamp.text = render(options, i, pages.size());
        stamp.fontSize = options.fontSize;
        stamp.colour = options.colour;
        stamp.anchor = options.anchor;
        stamp.marginPoints = options.marginPoints;
        // Level and solid: a page number that is faint or tilted is a
        // decoration, and this is meant to be read and quoted.
        stamp.rotation = 0.0;
        stamp.opacity = 1.0;
        stamp.outlineOnly = false;

        stamps.insert(pages.at(i), stamp);
    }

    return Overlay::stampTexts(inputPdf, outputPdf, stamps, error);
}

} // namespace ps
