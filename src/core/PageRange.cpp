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

/**
 * The words this language offers for one keyword.
 *
 * A translation may list several spellings separated by a vertical bar, so that
 * "impair|impaires" both work; the first one is what the captions show. The
 * English word is not part of the message: it is accepted in every language,
 * because the handbook and any script written against it say "odd".
 */
QStringList spellings(const QString &translated)
{
    QStringList words;
    const QStringList parts = translated.split(QLatin1Char('|'), Qt::SkipEmptyParts);
    for (const QString &part : parts) {
        const QString trimmed = part.trimmed();
        if (!trimmed.isEmpty()) {
            words.append(trimmed);
        }
    }
    return words;
}

QString oddKeyword()
{
    return i18nc("@item the word this language uses for the odd-numbered pages in a page-range box, for example "
                 "“ungerade”. Several spellings may be given separated by a vertical bar, and the first one is the "
                 "one the captions show. The English “odd” keeps working in every language.",
                 "odd");
}

QString evenKeyword()
{
    return i18nc("@item the word this language uses for the even-numbered pages in a page-range box, for example "
                 "“gerade”. Several spellings may be given separated by a vertical bar, and the first one is the "
                 "one the captions show. The English “even” keeps working in every language.",
                 "even");
}

QString lastKeyword()
{
    return i18nc("@item the word this language uses for the final page in a page-range box, as in “12-last”. "
                 "Several spellings may be given separated by a vertical bar, and the first one is the one the "
                 "captions show. The English “last” keeps working in every language.",
                 "last");
}

/**
 * True when @p token is one of the words that stand for a keyword.
 *
 * Three sources answer, in this order of standing: the English word, which the
 * handbook and every script use and which no language may take away; German,
 * which was hard-wired here before there were any catalogues and is still in
 * people's scripts; and this language's own spellings.
 */
bool matchesKeyword(const QString &token, const QString &english, const QString &german, const QString &translated)
{
    if (token.compare(english, Qt::CaseInsensitive) == 0 || token.compare(german, Qt::CaseInsensitive) == 0) {
        return true;
    }
    const QStringList words = spellings(translated);
    for (const QString &word : words) {
        if (token.compare(word, Qt::CaseInsensitive) == 0) {
            return true;
        }
    }
    return false;
}

/** Resolves one endpoint: a number, or the word for the last page. */
bool parseEndpoint(const QString &token, int pageCount, int *out)
{
    const QString trimmed = token.trimmed();
    if (trimmed.isEmpty()) {
        return false;
    }

    if (trimmed == QLatin1Char('$')
        || matchesKeyword(trimmed, QStringLiteral("last"), QStringLiteral("letzte"), lastKeyword())) {
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

        if (matchesKeyword(part, QStringLiteral("odd"), QStringLiteral("ungerade"), oddKeyword())) {
            for (int i = 0; i < pageCount; i += 2) {
                result.append(i);
            }
            continue;
        }
        if (matchesKeyword(part, QStringLiteral("even"), QStringLiteral("gerade"), evenKeyword())) {
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

QString oddWord()
{
    const QStringList words = spellings(oddKeyword());
    return words.isEmpty() ? QStringLiteral("odd") : words.constFirst();
}

QString evenWord()
{
    const QStringList words = spellings(evenKeyword());
    return words.isEmpty() ? QStringLiteral("even") : words.constFirst();
}

QString lastWord()
{
    const QStringList words = spellings(lastKeyword());
    return words.isEmpty() ? QStringLiteral("last") : words.constFirst();
}

bool isValid(const QString &text, int pageCount)
{
    QString error;
    const QVector<int> pages = parse(text, pageCount, &error);
    return !pages.isEmpty() && error.isEmpty();
}

} // namespace ps::PageRange
