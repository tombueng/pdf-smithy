/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later

    Preflight from a script.

    The window can show four hundred findings in a list and let somebody scroll.
    A terminal cannot, so the plain report here groups by severity and then by
    rule, prints each rule once with the places it was found in, and leaves the
    complete list to --json. Everything the header offers has a verb: the
    profiles, the rules behind them, the repairs, and the caveats.
*/
#include "Commands.h"

#include "core/Preflight.h"

#include <KLocalizedString>

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QTextStream>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace ps::cli {

namespace {

using Finding = Preflight::Finding;
using Profile = Preflight::Profile;
using Report = Preflight::Report;
using Severity = Preflight::Severity;

/** What "preflight FILE" checks against when nobody says. */
constexpr QLatin1String DefaultProfile("print-ready");

/**
 * How many places one rule names before the plain report starts counting.
 *
 * The engine already caps a rule at twenty findings, so this only decides how
 * much of that a person is shown at once. A report nobody reads to the end is a
 * report that does not exist; --json still carries every one of them.
 */
constexpr int MaxPlacesShown = 5;

// ── Small shared pieces ───────────────────────────────────────────────────

/** Untranslated, because this is what a script matches on. */
QString severityToken(Severity severity)
{
    switch (severity) {
    case Severity::Note:
        return u"note"_s;
    case Severity::Warning:
        return u"warning"_s;
    case Severity::Error:
        return u"error"_s;
    }
    return u"warning"_s;
}

QString placeOf(const Finding &finding)
{
    const QString where = finding.page >= 0
        ? i18nc("@info where a preflight finding is", "page %1", finding.page + 1)
        : i18nc("@info a preflight finding about the document as a whole", "whole document");
    if (finding.object.isEmpty()) {
        return where;
    }
    return i18nc("@info a place and the resource found there", "%1 · %2", where, finding.object);
}

QString listSeparator()
{
    return i18nc("@item separator in a list", ", ");
}

int countBySeverity(const Report &report, Severity severity)
{
    return int(std::count_if(report.findings.cbegin(), report.findings.cend(),
                             [severity](const Finding &finding) { return finding.severity == severity; }));
}

/** One "id: what it looks for" line per rule. */
QStringList describeRules(const QStringList &ruleIds)
{
    QStringList lines;
    for (const QString &rule : ruleIds) {
        lines.append(i18nc("@info a rule id and what it looks for", "%1: %2", rule, Preflight::describeRule(rule)));
    }
    return lines;
}

/**
 * The profile named on the command line: a built-in id, or a file.
 *
 * A print shop's rules are theirs and not ours, so "save-profile" writes one
 * out, they edit it, and it comes back in here by path.
 */
bool resolveProfile(const QString &given, Profile *profile, QString *error)
{
    const QString id = given.isEmpty() ? QString(DefaultProfile) : given;

    const Profile builtin = Preflight::profileById(id);
    if (!builtin.id.isEmpty()) {
        *profile = builtin;
        return true;
    }

    if (QFileInfo::exists(id)) {
        QString why;
        const Profile loaded = Preflight::loadProfile(id, &why);
        if (!why.isEmpty()) {
            *error = why;
            return false;
        }
        if (loaded.rules.isEmpty()) {
            *error = i18n("“%1” names no rules, so checking against it would look at nothing.", id);
            return false;
        }
        *profile = loaded;
        return true;
    }

    QStringList ids;
    const QVector<Profile> profiles = Preflight::builtinProfiles();
    for (const Profile &known : profiles) {
        ids.append(known.id);
    }
    *error = i18n("There is no profile called “%1”. Use one of these, or the path to a saved profile: %2", id,
                  ids.join(listSeparator()));
    return false;
}

// ── The plain report ──────────────────────────────────────────────────────

/** One rule and everywhere it was found, so the rule is named once. */
struct RuleGroup {
    QString ruleId;
    QVector<Finding> findings;
};

QVector<RuleGroup> groupByRule(const Report &report, Severity severity)
{
    QVector<RuleGroup> groups;
    for (const Finding &finding : report.findings) {
        if (finding.severity != severity) {
            continue;
        }
        const auto existing = std::find_if(groups.begin(), groups.end(), [&finding](const RuleGroup &group) {
            return group.ruleId == finding.ruleId;
        });
        if (existing == groups.end()) {
            groups.append(RuleGroup { finding.ruleId, { finding } });
        } else {
            existing->findings.append(finding);
        }
    }
    return groups;
}

QString severityHeading(Severity severity, int count)
{
    switch (severity) {
    case Severity::Error:
        return i18ncp("@title:group preflight findings that will get the file rejected", "Error (%1)", "Errors (%1)",
                      count);
    case Severity::Warning:
        return i18ncp("@title:group preflight findings somebody may complain about", "Warning (%1)", "Warnings (%1)",
                      count);
    case Severity::Note:
        return i18ncp("@title:group preflight findings that are only worth knowing", "Note (%1)", "Notes (%1)", count);
    }
    return {};
}

void printGroup(const RuleGroup &group)
{
    const QVector<Finding> &findings = group.findings;
    const Finding &first = findings.constFirst();

    if (findings.size() == 1) {
        out() << u"  "_s
              << i18nc("@info a rule id and the one place it was found", "%1 · %2", group.ruleId, placeOf(first))
              << Qt::endl;
        out() << u"    "_s << first.message << Qt::endl;
    } else {
        out() << u"  "_s
              << i18ncp("@info a rule id and how many places it was found in", "%2 · %1 place", "%2 · %1 places",
                        findings.size(), group.ruleId)
              << Qt::endl;

        const bool oneMessage = std::all_of(findings.cbegin(), findings.cend(), [&first](const Finding &finding) {
            return finding.message == first.message;
        });
        const int hidden = std::max(0, int(findings.size()) - MaxPlacesShown);

        if (oneMessage) {
            // The same sentence twenty times over teaches nobody anything; the
            // places are what differ, so they are what gets listed.
            out() << u"    "_s << first.message << Qt::endl;
            QStringList places;
            for (int i = 0; i < findings.size() && i < MaxPlacesShown; ++i) {
                places.append(placeOf(findings.at(i)));
            }
            const QString shown = places.join(listSeparator());
            out() << u"    "_s
                  << (hidden == 0 ? i18nc("@info everywhere a repeated finding applies", "At: %1.", shown)
                                  : i18ncp("@info where a repeated finding applies, with the rest left out",
                                           "At: %2, and one place more.", "At: %2, and %1 places more.", hidden, shown))
                  << Qt::endl;
        } else {
            for (int i = 0; i < findings.size() && i < MaxPlacesShown; ++i) {
                const Finding &finding = findings.at(i);
                out() << u"    "_s
                      << i18nc("@info a place and what was found there", "%1: %2", placeOf(finding), finding.message)
                      << Qt::endl;
            }
            if (hidden > 0) {
                out() << u"    "_s
                      << i18ncp("@info findings left out of the plain report",
                                "One place more is not listed; --json lists them all.",
                                "%1 places more are not listed; --json lists them all.", hidden)
                      << Qt::endl;
            }
        }
    }

    const auto mendable = std::find_if(findings.cbegin(), findings.cend(),
                                       [](const Finding &finding) { return !finding.fixId.isEmpty(); });
    if (mendable != findings.cend()) {
        out() << u"    "_s
              << i18nc("@info a repair exists for this finding", "Can be mended by --fix (%1).", mendable->fixId)
              << Qt::endl;
    }
}

void printReport(const Report &report, const QString &path, const Profile &profile)
{
    out() << i18nc("@info the document that was checked and the profile it was checked against",
                   "%1 checked against “%2” (%3).", QFileInfo(path).fileName(), profile.name, profile.id)
          << Qt::endl;

    for (const Severity severity : { Severity::Error, Severity::Warning, Severity::Note }) {
        const QVector<RuleGroup> groups = groupByRule(report, severity);
        if (groups.isEmpty()) {
            continue;
        }
        out() << Qt::endl << severityHeading(severity, countBySeverity(report, severity)) << Qt::endl;
        for (const RuleGroup &group : groups) {
            printGroup(group);
        }
    }

    if (!report.notChecked.isEmpty()) {
        // The most important part of the report: a check the profile asked for
        // and did not get. Silence here would read as a pass.
        out() << Qt::endl
              << i18nc("@title:group rules the profile names that this program cannot judge",
                       "Asked for but not checked (%1)", report.notChecked.size())
              << Qt::endl;
        const QStringList lines = describeRules(report.notChecked);
        for (const QString &line : lines) {
            out() << u"  "_s << line << Qt::endl;
        }
        out() << u"  "_s << i18n("Nothing above says anything about these either way.") << Qt::endl;
    }

    out() << Qt::endl;
    out() << (report.passed ? i18n("No errors were found among the rules that were checked.")
                            : i18np("One error was found.", "%1 errors were found.", report.errors))
          << Qt::endl;
    out() << i18n("This is a check, not a certification. “preflight limitations” says what it cannot see.") << Qt::endl;
}

// ── The report as JSON ────────────────────────────────────────────────────

QJsonObject reportJson(const Report &report)
{
    QJsonArray findings;
    for (const Finding &finding : report.findings) {
        QJsonObject item { { u"rule"_s, finding.ruleId },
                           { u"severity"_s, severityToken(finding.severity) },
                           { u"message"_s, finding.message },
                           { u"page"_s, finding.page < 0 ? QJsonValue() : QJsonValue(finding.page + 1) } };
        if (!finding.object.isEmpty()) {
            item.insert(u"object"_s, finding.object);
        }
        if (!finding.fixId.isEmpty()) {
            item.insert(u"fix"_s, finding.fixId);
            item.insert(u"fixDescription"_s, finding.fixDescription);
        }
        findings.append(item);
    }

    return QJsonObject {
        { u"passed"_s, report.passed },     { u"errors"_s, report.errors },
        { u"warnings"_s, report.warnings }, { u"notes"_s, countBySeverity(report, Severity::Note) },
        { u"findings"_s, findings },        { u"notChecked"_s, QJsonArray::fromStringList(report.notChecked) }
    };
}

void printJson(const QJsonObject &root)
{
    out() << QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Indented));
}

// ── preflight FILE ────────────────────────────────────────────────────────

/**
 * The exit code is the verdict.
 *
 * Success only when the check found nothing at all, DifferencesFound otherwise,
 * the same code compare uses, and for the same reason: it lets a script gate
 * on it, as in "preflight job.pdf && lp job.pdf". After --fix the verdict is
 * about the file that was written, because that is the one being handed over.
 */
int verdictFor(const Report &report)
{
    return report.findings.isEmpty() ? Success : DifferencesFound;
}

int commandRun(const QStringList &arguments, const QCommandLineParser &parser)
{
    QString path;
    int code = Success;
    if (!oneInputFile(arguments, u"preflight"_s, &path, &code)) {
        return code;
    }

    Profile profile;
    QString error;
    if (!resolveProfile(parser.value(u"profile"_s), &profile, &error)) {
        return fail(error, UsageError);
    }

    const Report report = Preflight::run(path, profile, &error);
    if (!error.isEmpty()) {
        return fail(error);
    }

    const QStringList knownFixes = Preflight::knownFixes();
    const QStringList named = parser.values(u"apply-fix"_s);
    QStringList wanted;
    for (const QString &fix : named) {
        if (!knownFixes.contains(fix)) {
            return fail(i18n("There is no repair called “%1”. “preflight fixes” lists them.", fix), UsageError);
        }
        if (!wanted.contains(fix)) {
            wanted.append(fix);
        }
    }
    if (parser.isSet(u"fix"_s)) {
        for (const Finding &finding : report.findings) {
            if (!finding.fixId.isEmpty() && !wanted.contains(finding.fixId)) {
                wanted.append(finding.fixId);
            }
        }
    }
    const bool asJson = parser.isSet(u"json"_s);
    const bool mending = !wanted.isEmpty();

    QString output;
    QStringList applied;
    Report after;
    bool checkedAgain = false;
    if (mending) {
        output = outputOr(parser.value(u"output"_s), path, u"-fixed.pdf"_s);
        if (!Preflight::applyFixes(path, output, wanted, &applied, &error)) {
            return fail(error, OutputError);
        }
        QString againError;
        after = Preflight::run(output, profile, &againError);
        // A repair that leaves an unreadable file is worth knowing about, but
        // the file is written and the report above still stands.
        if (againError.isEmpty()) {
            checkedAgain = true;
        } else {
            err() << i18n("Note: the repaired file could not be checked again: %1", againError) << Qt::endl;
        }
    }

    const Report &verdict = checkedAgain ? after : report;

    const QString reportPdf = parser.value(u"report"_s);
    if (!reportPdf.isEmpty()) {
        if (!Preflight::writeReport(checkedAgain ? output : path, verdict, reportPdf, &error)) {
            return fail(error, OutputError);
        }
    }

    if (asJson) {
        QJsonObject root { { u"file"_s, QFileInfo(path).absoluteFilePath() },
                           { u"profile"_s, QJsonObject { { u"id"_s, profile.id }, { u"name"_s, profile.name } } } };
        const QJsonObject before = reportJson(report);
        for (auto it = before.constBegin(); it != before.constEnd(); ++it) {
            root.insert(it.key(), it.value());
        }
        if (mending) {
            QJsonObject fixes { { u"requested"_s, QJsonArray::fromStringList(wanted) },
                                { u"output"_s, QFileInfo(output).absoluteFilePath() },
                                { u"applied"_s, QJsonArray::fromStringList(applied) } };
            if (checkedAgain) {
                fixes.insert(u"after"_s, reportJson(after));
            }
            root.insert(u"fix"_s, fixes);
        }
        if (!reportPdf.isEmpty()) {
            root.insert(u"reportPdf"_s, QFileInfo(reportPdf).absoluteFilePath());
        }
        printJson(root);
        return verdictFor(verdict);
    }

    printReport(report, path, profile);

    if (parser.isSet(u"fix"_s) && !mending) {
        out() << Qt::endl << i18n("Nothing found here can be mended automatically.") << Qt::endl;
    }
    if (mending) {
        out() << Qt::endl << i18nc("@title heading before a list of repairs", "Repairs:") << Qt::endl;
        for (const QString &line : std::as_const(applied)) {
            out() << u"  · "_s << line << Qt::endl;
        }
        out() << i18n("Wrote %1.", output) << Qt::endl;

        if (checkedAgain) {
            // What was *not* fixed, said the only way that cannot be wrong: by
            // checking the result the same way the original was checked.
            out() << Qt::endl << i18n("Checked again after the repairs:") << Qt::endl;
            printReport(after, output, profile);
        }
    }
    if (!reportPdf.isEmpty()) {
        out() << i18n("Wrote the report to %1.", reportPdf) << Qt::endl;
    }

    return verdictFor(verdict);
}

// ── preflight profiles ────────────────────────────────────────────────────

int commandProfiles(bool asJson)
{
    const QVector<Profile> profiles = Preflight::builtinProfiles();
    const QStringList implemented = Preflight::implementedRules();

    if (asJson) {
        QJsonArray list;
        for (const Profile &profile : profiles) {
            QStringList ruleIds = profile.rules.keys();
            ruleIds.sort();
            QJsonObject rules;
            for (const QString &rule : std::as_const(ruleIds)) {
                rules.insert(rule, severityToken(profile.rules.value(rule)));
            }
            QStringList thresholdIds = profile.thresholds.keys();
            thresholdIds.sort();
            QJsonObject thresholds;
            for (const QString &rule : std::as_const(thresholdIds)) {
                thresholds.insert(rule, profile.thresholds.value(rule));
            }
            list.append(QJsonObject { { u"id"_s, profile.id },
                                      { u"name"_s, profile.name },
                                      { u"rules"_s, rules },
                                      { u"thresholds"_s, thresholds } });
        }
        printJson(QJsonObject { { u"profiles"_s, list } });
        return Success;
    }

    const QString indent(17, u' '); // Under the name, past the id column.
    for (const Profile &profile : profiles) {
        int errors = 0;
        int warnings = 0;
        int notes = 0;
        QStringList unchecked;
        for (auto it = profile.rules.constBegin(); it != profile.rules.constEnd(); ++it) {
            switch (it.value()) {
            case Severity::Error:
                ++errors;
                break;
            case Severity::Warning:
                ++warnings;
                break;
            case Severity::Note:
                ++notes;
                break;
            }
            if (!implemented.contains(it.key())) {
                unchecked.append(it.key());
            }
        }
        unchecked.sort();

        out() << u"  "_s << profile.id.leftJustified(15) << profile.name << Qt::endl;
        out() << indent
              << i18nc("@info how many rules a profile has, and at which severity",
                       "%1 rules: %2 as errors, %3 as warnings, %4 as notes.", profile.rules.size(), errors, warnings,
                       notes)
              << Qt::endl;
        if (!unchecked.isEmpty()) {
            out() << indent
                  << i18ncp("@info rules a profile names that this program cannot judge",
                            "One of them cannot be judged here: %2.", "%1 of them cannot be judged here: %2.",
                            unchecked.size(), unchecked.join(listSeparator()))
                  << Qt::endl;
        }
    }

    out() << Qt::endl
          << i18n("Check against one with “preflight FILE --profile ID”, or against a profile of your own by giving "
                  "its file name instead.")
          << Qt::endl;
    return Success;
}

// ── preflight rules ───────────────────────────────────────────────────────

int commandRules(const QCommandLineParser &parser)
{
    const bool asJson = parser.isSet(u"json"_s);
    const QString wantedProfile = parser.value(u"profile"_s);
    const QStringList implemented = Preflight::implementedRules();

    Profile profile;
    const bool forProfile = !wantedProfile.isEmpty();
    if (forProfile) {
        QString error;
        if (!resolveProfile(wantedProfile, &profile, &error)) {
            return fail(error, UsageError);
        }
    }

    QStringList ruleIds = forProfile ? profile.rules.keys() : Preflight::knownRules();
    if (forProfile) {
        ruleIds.sort();
    }

    if (asJson) {
        QJsonArray list;
        for (const QString &rule : std::as_const(ruleIds)) {
            QJsonObject item { { u"id"_s, rule },
                               { u"describes"_s, Preflight::describeRule(rule) },
                               { u"checked"_s, implemented.contains(rule) } };
            if (forProfile) {
                item.insert(u"severity"_s, severityToken(profile.rules.value(rule)));
                if (profile.thresholds.contains(rule)) {
                    item.insert(u"threshold"_s, profile.thresholds.value(rule));
                }
            }
            list.append(item);
        }
        QJsonObject root { { u"rules"_s, list } };
        if (forProfile) {
            root.insert(u"profile"_s, QJsonObject { { u"id"_s, profile.id }, { u"name"_s, profile.name } });
        }
        printJson(root);
        return Success;
    }

    out() << (forProfile ? i18n("The rules of “%1”:", profile.name)
                         : i18n("Every rule this program knows. A profile names the ones it cares about."))
          << Qt::endl;

    for (const QString &rule : std::as_const(ruleIds)) {
        out() << u"  "_s;
        if (forProfile) {
            // The severity is shown as the word a saved profile spells, since
            // this listing is what somebody writing one by hand copies from.
            out() << i18nc("@info a rule id and the severity a profile gives it", "%1: %2", rule,
                           severityToken(profile.rules.value(rule)));
        } else {
            out() << rule;
        }
        out() << Qt::endl;
        out() << u"      "_s << Preflight::describeRule(rule) << Qt::endl;
        if (forProfile && profile.thresholds.contains(rule)) {
            out() << u"      "_s
                  << i18nc("@info the number a rule compares against", "Threshold: %1.", profile.thresholds.value(rule))
                  << Qt::endl;
        }
        if (!implemented.contains(rule)) {
            out() << u"      "_s << i18n("Not checked: this program cannot judge it, and says so in the report.")
                  << Qt::endl;
        }
    }
    return Success;
}

// ── preflight fixes ───────────────────────────────────────────────────────

int commandFixes(bool asJson)
{
    const QStringList fixes = Preflight::knownFixes();

    if (asJson) {
        QJsonArray list;
        for (const QString &fix : fixes) {
            list.append(QJsonObject { { u"id"_s, fix }, { u"describes"_s, Preflight::describeFix(fix) } });
        }
        printJson(QJsonObject { { u"fixes"_s, list } });
        return Success;
    }

    out() << i18n("Every repair, and what applying it would change:") << Qt::endl;
    for (const QString &fix : fixes) {
        out() << u"  "_s << fix << Qt::endl;
        out() << u"      "_s << Preflight::describeFix(fix) << Qt::endl;
    }
    out() << Qt::endl
          << i18n("“preflight FILE --fix -o OUT” applies the ones the findings ask for; “--apply-fix ID” applies one "
                  "whether anything asked for it or not.")
          << Qt::endl;
    return Success;
}

// ── preflight limitations ─────────────────────────────────────────────────

int commandLimitations(bool asJson)
{
    const QStringList limitations = Preflight::limitations();

    if (asJson) {
        printJson(QJsonObject { { u"limitations"_s, QJsonArray::fromStringList(limitations) } });
        return Success;
    }

    out() << i18n("What this check cannot promise:") << Qt::endl;
    for (const QString &limitation : limitations) {
        out() << u"  · "_s << limitation << Qt::endl;
    }
    return Success;
}

// ── preflight save-profile ────────────────────────────────────────────────

int commandSaveProfile(const QStringList &arguments, const QCommandLineParser &parser)
{
    if (arguments.size() != 2) {
        return fail(i18n("save-profile needs the id of one profile. “preflight profiles” lists them."), UsageError);
    }
    const QString id = arguments.at(1);
    const Profile profile = Preflight::profileById(id);
    if (profile.id.isEmpty()) {
        return fail(i18n("There is no profile called “%1”. “preflight profiles” lists them.", id), UsageError);
    }

    const QString output = outputOr(parser.value(u"output"_s), QDir::currentPath() + u'/' + id, u".json"_s);
    QString error;
    if (!Preflight::saveProfile(profile, output, &error)) {
        return fail(error, OutputError);
    }

    out() << i18n("Wrote “%1” to %2.", profile.name, output) << Qt::endl;
    out() << i18n("Edit it and check against it with “preflight FILE --profile %1”.", output) << Qt::endl;
    return Success;
}

// ── Registration ──────────────────────────────────────────────────────────

void addOptions(QCommandLineParser &parser)
{
    const QCommandLineOption profileOption(
        u"profile"_s, i18n("Which profile to check against: an id from “preflight profiles”, or a saved profile file."),
        u"name"_s);
    const QCommandLineOption fixOption(u"fix"_s,
                                       i18n("Apply every repair the findings offer and write the result to -o."));
    const QCommandLineOption applyFixOption(
        u"apply-fix"_s, i18n("Apply one named repair, whether or not a finding asked for it. Repeat for more."),
        u"id"_s);
    const QCommandLineOption reportOption(u"report"_s, i18n("Also write the report itself as a PDF."), u"file"_s);

    parser.addOptions({ profileOption, fixOption, applyFixOption, reportOption });
}

std::optional<int> run(const QString &verb, const QStringList &arguments, const QCommandLineParser &parser)
{
    if (verb != QLatin1String("preflight")) {
        return std::nullopt;
    }

    const bool asJson = parser.isSet(u"json"_s);
    // A word that names a sub-command wins over a file of the same name; a file
    // really called "rules" can still be reached as ./rules.
    const QString sub = arguments.value(0);
    if (sub == QLatin1String("profiles")) {
        return commandProfiles(asJson);
    }
    if (sub == QLatin1String("rules")) {
        return commandRules(parser);
    }
    if (sub == QLatin1String("fixes")) {
        return commandFixes(asJson);
    }
    if (sub == QLatin1String("limitations") || sub == QLatin1String("limits")) {
        return commandLimitations(asJson);
    }
    if (sub == QLatin1String("save-profile")) {
        return commandSaveProfile(arguments, parser);
    }
    if (arguments.isEmpty()) {
        return fail(i18n("preflight needs a file, or one of: profiles, rules, fixes, limitations, save-profile."),
                    UsageError);
    }
    return commandRun(arguments, parser);
}

QStringList help()
{
    return {
        i18nc("@info:shell one line of the command list, keep the columns lined up",
              "  preflight FILE     [-o OUT]         Check against a profile, and mend faults"),
        i18nc("@info:shell one line of the command list, keep the columns lined up",
              "  preflight profiles                  List the profiles that ship here"),
        i18nc("@info:shell one line of the command list, keep the columns lined up",
              "  preflight rules                     Every rule, and whether it is checked"),
        i18nc("@info:shell one line of the command list, keep the columns lined up",
              "  preflight fixes                     Every repair and what it changes"),
        i18nc("@info:shell one line of the command list, keep the columns lined up",
              "  preflight save-profile ID -o FILE   Write a profile out as JSON to edit"),
        i18nc("@info:shell one line of the command list, keep the columns lined up",
              "  preflight limitations               What this check cannot see"),
    };
}

} // namespace

Group preflightGroup()
{
    return { addOptions, run, help };
}

} // namespace ps::cli
