/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#pragma once

#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

namespace ps {

/**
 * Checking a document against a list of rules, and mending what can be mended.
 *
 * A printer who rejects a job says "the fonts are not embedded"; an archive
 * says "there is no output intent"; a colleague with a screen reader says
 * nothing at all, because the document never told them it was in German. All
 * three complaints are the same shape (a rule, a place in the file, and a
 * severity) so they are all one thing here.
 *
 * ## Profiles
 *
 * A profile is nothing but a list of rule ids with a severity each, plus the
 * numbers some of those rules need. A rule that is *absent* from the map is not
 * checked at all, which is what makes a profile a choice rather than a filter:
 * a web profile genuinely does not care about the trim box, and reporting it as
 * a note the user has to dismiss two hundred times is how preflight tools earn
 * their reputation for being ignored.
 *
 * Severity is per profile, not per rule, for the same reason. CMYK in a
 * document destined for a screen is a warning; in one destined for a press it
 * is how things are supposed to be.
 *
 * ## Honesty
 *
 * Several of the rules in the shipped profiles are not implemented here, and
 * the report says which, in Report::notChecked. That list is the most important
 * thing this class produces. A preflight report that quietly omits a check the
 * profile names is worse than no report: the user reads "no problems found" and
 * sends the file. So the profile keeps naming the rule, the run keeps admitting
 * it cannot judge it, and nobody is misled about what was looked at.
 *
 * The counting follows from that: Report::passed means no *error* was found
 * among the rules that were actually checked. It is not a certificate.
 *
 * ## Fixes
 *
 * Every fix is a real edit to a real file, and each says what it will change
 * before it is applied, in Finding::fixDescription, because "fix all" on a
 * document somebody has spent a week on is not a button anyone should press
 * blind. Fixes that rewrite the whole document through Ghostscript (embedding
 * fonts, downsampling pictures) run before the ones that edit objects with
 * QPDF, and linearising runs last, because otherwise each stage would undo the
 * one before it.
 */
class Preflight
{
public:
    enum class Severity {
        Note, //!< worth knowing; counts towards nothing
        Warning, //!< would survive, but somebody may complain
        Error, //!< the document will be rejected
    };

    struct Finding {
        QString ruleId; //!< stable, untranslated, e.g. "font.not-embedded"
        Severity severity = Severity::Warning;
        QString message; //!< translated, for a person
        int page = -1; //!< -1 when it is about the whole document
        QString object; //!< which resource, when that is known
        /** Empty when nothing here can fix it. */
        QString fixId;
        QString fixDescription; //!< what applying it would change
    };

    struct Profile {
        QString id;
        QString name;
        /** Rule id -> severity. A rule absent from the map is not checked. */
        QHash<QString, Severity> rules;
        /** Numeric thresholds by rule id, e.g. "image.low-resolution" -> 300. */
        QHash<QString, double> thresholds;
    };

    /** The profiles that ship with the program. */
    static QVector<Profile> builtinProfiles();
    static Profile profileById(const QString &id);
    static bool saveProfile(const Profile &profile, const QString &path, QString *error);
    static Profile loadProfile(const QString &path, QString *error);

    struct Report {
        QVector<Finding> findings;
        int errors = 0;
        int warnings = 0;
        bool passed = false;
        /** Rules that were asked for but are not implemented here. Said out loud. */
        QStringList notChecked;
    };
    static Report run(const QString &pdf, const Profile &profile, QString *error);

    /** Applies the named fixes, in a sensible order, reporting what each did. */
    static bool applyFixes(const QString &in, const QString &out, const QStringList &fixIds, QStringList *applied,
                           QString *error);

    /** A report as a PDF, for handing to a printer or an archive. */
    static bool writeReport(const QString &pdf, const Report &report, const QString &outPdf, QString *error);

    /**
     * Every rule id this class recognises, checked or not.
     *
     * A profile, including one a user has written by hand, may only name
     * these; anything else is a typo, and a typo that silently disables a check
     * is the failure mode worth guarding against.
     */
    static QStringList knownRules();

    /** The subset of knownRules() that run() actually inspects. */
    static QStringList implementedRules();

    /** What a rule looks for, as a sentence, for a profile editor to show. */
    static QString describeRule(const QString &ruleId);

    /** Every fix id applyFixes() understands. */
    static QStringList knownFixes();

    /** What applying a fix would change, in the future tense. */
    static QString describeFix(const QString &fixId);

    static QStringList limitations();
};

} // namespace ps
