/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#include "PageRange.h"

#include <KLocalizedString>

#include <algorithm>
#include <numeric>

namespace ps::PageRange {

namespace {

/** Resolves one endpoint: a number, or the word "last"/"letzte". */
bool parseEndpoint(const QString &token, int pageCount, int *out)
{
    const QString trimmed = token.trimmed();
    if (trimmed.isEmpty()) {
        return false;
    }

    if (trimmed.compare(QStringLiteral("last"), Qt::CaseInsensitive) == 0
        || trimmed.compare(QStringLiteral("letzte"), Qt::CaseInsensitive) == 0 || trimmed == QLatin1Char('$')) {
        *out = pageCount;
        return true;
    }

    bool ok = false;
    const int value = trimmed.toInt(&ok);
    if (!ok || value < 0) {
        // Negative endpoints are refused deliberately. "-3" already means
        // "everything up to page 3" in every print dialog ever built, and no
        // second meaning is worth breaking that expectation for. Counting from
        // the end is spelled "last".
        return false;
    }

    *out = value;
    return true;
}

} // namespace

QVector<int> parse(const QString &text, int pageCount, QString *error)
{
    QVector<int> result;

    if (pageCount <= 0) {
        if (error) {
            *error = i18n("The document has no pages.");
        }
        return result;
    }

    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) {
        result.resize(pageCount);
        std::iota(result.begin(), result.end(), 0);
        return result;
    }

    const QStringList parts = trimmed.split(QLatin1Char(','), Qt::SkipEmptyParts);
    for (const QString &rawPart : parts) {
        const QString part = rawPart.trimmed();
        if (part.isEmpty()) {
            continue;
        }

        if (part.compare(QStringLiteral("odd"), Qt::CaseInsensitive) == 0
            || part.compare(QStringLiteral("ungerade"), Qt::CaseInsensitive) == 0) {
            for (int i = 0; i < pageCount; i += 2) {
                result.append(i);
            }
            continue;
        }
        if (part.compare(QStringLiteral("even"), Qt::CaseInsensitive) == 0
            || part.compare(QStringLiteral("gerade"), Qt::CaseInsensitive) == 0) {
            for (int i = 1; i < pageCount; i += 2) {
                result.append(i);
            }
            continue;
        }

        // Searched from the very start: a leading dash is an open range, never
        // a negative number.
        const int dash = part.indexOf(QLatin1Char('-'));
        if (dash < 0) {
            int page = 0;
            if (!parseEndpoint(part, pageCount, &page)) {
                if (error) {
                    *error = i18n("“%1” is not a page number.", part);
                }
                return {};
            }
            if (page < 1 || page > pageCount) {
                if (error) {
                    *error = i18n("Page %1 is outside the document, which has %2 pages.", page, pageCount);
                }
                return {};
            }
            result.append(page - 1);
            continue;
        }

        // An open end means "to the end" or "from the beginning".
        const QString leftText = part.left(dash).trimmed();
        const QString rightText = part.mid(dash + 1).trimmed();

        int first = 1;
        int last = pageCount;
        if (!leftText.isEmpty() && !parseEndpoint(leftText, pageCount, &first)) {
            if (error) {
                *error = i18n("“%1” is not a page number.", leftText);
            }
            return {};
        }
        if (!rightText.isEmpty() && !parseEndpoint(rightText, pageCount, &last)) {
            if (error) {
                *error = i18n("“%1” is not a page number.", rightText);
            }
            return {};
        }

        if (first < 1 || first > pageCount || last < 1 || last > pageCount) {
            if (error) {
                *error = i18n("The range %1 lies outside the document, which has %2 pages.", part, pageCount);
            }
            return {};
        }

        // A descending range reverses that stretch: "9-1" really does mean
        // "these pages, backwards".
        if (first <= last) {
            for (int page = first; page <= last; ++page) {
                result.append(page - 1);
            }
        } else {
            for (int page = first; page >= last; --page) {
                result.append(page - 1);
            }
        }
    }

    if (result.isEmpty() && error) {
        *error = i18n("That selects no pages.");
    }
    return result;
}

QString format(const QVector<int> &indexes)
{
    if (indexes.isEmpty()) {
        return {};
    }

    QVector<int> sorted = indexes;
    std::sort(sorted.begin(), sorted.end());
    sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());

    QStringList parts;
    int runStart = sorted.first();
    int previous = runStart;

    const auto flush = [&parts](int start, int end) {
        if (start == end) {
            parts.append(QString::number(start + 1));
        } else if (end == start + 1) {
            parts.append(QString::number(start + 1));
            parts.append(QString::number(end + 1));
        } else {
            parts.append(QStringLiteral("%1-%2").arg(start + 1).arg(end + 1));
        }
    };

    for (int i = 1; i < sorted.size(); ++i) {
        if (sorted.at(i) != previous + 1) {
            flush(runStart, previous);
            runStart = sorted.at(i);
        }
        previous = sorted.at(i);
    }
    flush(runStart, previous);

    return parts.join(QStringLiteral(", "));
}

bool isValid(const QString &text, int pageCount)
{
    QString error;
    const QVector<int> pages = parse(text, pageCount, &error);
    return !pages.isEmpty() && error.isEmpty();
}

} // namespace ps::PageRange
