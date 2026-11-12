/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later

    Finding a word in a document, from the command line.
*/
#include "Commands.h"

#include "core/TextFind.h"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>

#include <KLocalizedString>

using namespace Qt::Literals::StringLiterals;

namespace ps::cli {

namespace {

void searchOptions(QCommandLineParser &parser)
{
    const QCommandLineOption caseOption(u"case-sensitive"_s, i18n("Match upper and lower case exactly."));
    parser.addOptions({ caseOption });
}

std::optional<int> searchRun(const QString &verb, const QStringList &arguments, const QCommandLineParser &parser)
{
    if (verb != QLatin1String("search")) {
        return std::nullopt;
    }

    if (arguments.size() < 2) {
        return fail(i18n("Give a file and what to look for: search FILE \"term\"."), UsageError);
    }
    const QString input = arguments.constFirst();
    const QString term = QStringList(arguments.mid(1)).join(u' ');

    const QFileInfo info(input);
    if (!info.exists() || !info.isReadable()) {
        return fail(i18n("Cannot read %1.", input));
    }

    const Qt::CaseSensitivity cs = parser.isSet(u"case-sensitive"_s) ? Qt::CaseSensitive : Qt::CaseInsensitive;
    QString error;
    const QVector<TextFind::Hit> hits = TextFind::search(input, term, cs, &error);
    if (!error.isEmpty()) {
        return fail(error);
    }

    int total = 0;
    for (const TextFind::Hit &hit : hits) {
        total += hit.count;
    }

    if (parser.isSet(u"json"_s)) {
        QJsonArray pages;
        for (const TextFind::Hit &hit : hits) {
            pages.append(
                QJsonObject { { u"page"_s, hit.page + 1 }, { u"count"_s, hit.count }, { u"snippet"_s, hit.snippet } });
        }
        const QJsonObject root { { u"term"_s, term }, { u"matches"_s, total }, { u"pages"_s, pages } };
        out() << QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Indented));
        return Success;
    }

    if (hits.isEmpty()) {
        out() << i18n("“%1” is on no page. A scanned page needs text recognition first.", term) << Qt::endl;
        return Success;
    }

    for (const TextFind::Hit &hit : hits) {
        out() << i18nc("@info a search result line's page", "Page %1", hit.page + 1) << u", "_s
              << i18ncp("@info occurrences on the page", "%1 match", "%1 matches", hit.count) << u":  "_s << hit.snippet
              << Qt::endl;
    }
    out() << i18ncp("@info total occurrences across the document", "%1 match in all.", "%1 matches in all.", total)
          << Qt::endl;
    return Success;
}

QStringList searchHelp()
{
    const auto line
        = [](const QString &syntax, const QString &what) { return u"  "_s + syntax.leftJustified(46) + what; };
    return {
        line(u"search  FILE \"term\""_s, i18n("Find a word across the pages, with where and how often")),
    };
}

} // namespace

Group searchGroup()
{
    return { searchOptions, searchRun, searchHelp };
}

} // namespace ps::cli
