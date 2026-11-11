/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later

    The things you keep typing into forms, kept once — on the command line.

    "form fill" sets named fields from values given on the spot. This keeps those
    values between forms, locally, and works out which stored fact each field is
    asking for, so a form can be filled from what you have answered before. The
    vault is a plain JSON file on this machine and goes nowhere else.
*/
#include "Commands.h"

#include "core/FormVault.h"
#include "core/Forms.h"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QFileInfo>
#include <QTextStream>

#include <KLocalizedString>

using namespace Qt::Literals::StringLiterals;

namespace ps::cli {

namespace {

QString vaultPath(const QCommandLineParser &parser)
{
    return parser.isSet(u"vault"_s) ? parser.value(u"vault"_s) : FormVault::defaultPath();
}

int commandSet(const QStringList &rest, const QCommandLineParser &parser)
{
    if (rest.size() < 2) {
        return fail(i18n("Say the field and the value: autofill set KEY VALUE. “autofill keys” lists the fields."),
                    UsageError);
    }
    const QString key = rest.at(0);
    const QString value = QStringList(rest.mid(1)).join(u' ');

    const QString path = vaultPath(parser);
    FormVault vault = FormVault::load(path);
    vault.set(key, value);
    QString error;
    if (!vault.save(path, &error)) {
        return fail(i18n("Could not write the vault: %1", error), OutputError);
    }

    out() << i18n("Stored %1.", key) << Qt::endl;
    if (!FormVault::knownKeys().contains(key)) {
        out() << i18n("Note: “%1” is not a field autofill recognises, so it is kept but will not be matched to a form "
                      "on its own. “autofill keys” lists the ones that are.",
                      key)
              << Qt::endl;
    }
    return Success;
}

int commandGet(const QStringList &rest, const QCommandLineParser &parser)
{
    if (rest.isEmpty()) {
        return fail(i18n("Say which field: autofill get KEY."), UsageError);
    }
    const FormVault vault = FormVault::load(vaultPath(parser));
    const QString value = vault.value(rest.constFirst());
    if (value.isEmpty()) {
        return fail(i18n("Nothing is stored for “%1”.", rest.constFirst()));
    }
    out() << value << Qt::endl;
    return Success;
}

int commandList(const QCommandLineParser &parser)
{
    const FormVault vault = FormVault::load(vaultPath(parser));
    if (vault.isEmpty()) {
        out() << i18n("The vault is empty. Add a value with: autofill set KEY VALUE.") << Qt::endl;
        return Success;
    }
    const QStringList keys = vault.keys();
    int width = 0;
    for (const QString &key : keys) {
        width = qMax(width, key.size());
    }
    for (const QString &key : keys) {
        out() << key.leftJustified(width + 2) << vault.value(key) << Qt::endl;
    }
    return Success;
}

int commandRemove(const QStringList &rest, const QCommandLineParser &parser)
{
    if (rest.isEmpty()) {
        return fail(i18n("Say which field: autofill remove KEY."), UsageError);
    }
    const QString path = vaultPath(parser);
    FormVault vault = FormVault::load(path);
    if (!vault.remove(rest.constFirst())) {
        return fail(i18n("Nothing is stored for “%1”.", rest.constFirst()));
    }
    QString error;
    if (!vault.save(path, &error)) {
        return fail(i18n("Could not write the vault: %1", error), OutputError);
    }
    out() << i18n("Removed %1.", rest.constFirst()) << Qt::endl;
    return Success;
}

int commandKeys()
{
    out() << i18n("The fields autofill recognises on a form:") << Qt::endl;
    for (const QString &key : FormVault::knownKeys()) {
        out() << u"  "_s << key << Qt::endl;
    }
    return Success;
}

int commandFill(const QStringList &rest, const QCommandLineParser &parser)
{
    QString input;
    int code = 0;
    if (!oneInputFile(rest, u"autofill fill"_s, &input, &code)) {
        return code;
    }

    const FormVault vault = FormVault::load(vaultPath(parser));
    if (vault.isEmpty()) {
        return fail(i18n("The vault is empty, so there is nothing to fill from. Add values with: autofill set KEY "
                         "VALUE."));
    }

    QString error;
    const QVector<FormField> fields = Forms::read(input, &error);
    if (!error.isEmpty() && fields.isEmpty()) {
        return fail(error);
    }

    const QHash<QString, QString> values = vault.fillFor(fields);
    if (values.isEmpty()) {
        out() << i18n("Nothing in the vault matched a field on this form.") << Qt::endl;
        return Success;
    }

    const QString output = outputOr(parser.value(u"output"_s), input, u"-filled.pdf"_s);
    int filled = 0;
    QStringList unknown;
    if (!Forms::fill(input, output, values, &filled, &unknown, &error)) {
        return fail(error.isEmpty() ? i18n("Could not fill the form.") : error, OutputError);
    }

    out() << i18ncp("@info", "Filled %1 field, saved as %2.", "Filled %1 fields, saved as %2.", filled,
                    QFileInfo(output).fileName())
          << Qt::endl;
    QStringList names = values.keys();
    names.sort();
    for (const QString &name : names) {
        out() << u"  "_s << name << Qt::endl;
    }
    return Success;
}

int commandLearn(const QStringList &rest, const QCommandLineParser &parser)
{
    QString input;
    int code = 0;
    if (!oneInputFile(rest, u"autofill learn"_s, &input, &code)) {
        return code;
    }

    QString error;
    const QVector<FormField> fields = Forms::read(input, &error);
    if (!error.isEmpty() && fields.isEmpty()) {
        return fail(error);
    }

    const QString path = vaultPath(parser);
    FormVault vault = FormVault::load(path);
    const int learned = vault.learnFrom(fields);
    if (learned == 0) {
        out() << i18n("Nothing on this form could be placed and stored.") << Qt::endl;
        return Success;
    }
    if (!vault.save(path, &error)) {
        return fail(i18n("Could not write the vault: %1", error), OutputError);
    }
    out() << i18ncp("@info", "Learned %1 value from this form.", "Learned %1 values from this form.", learned)
          << Qt::endl;
    return Success;
}

int commandLimits()
{
    const QStringList lines = {
        i18n("Only text and drop-down fields are filled; tick boxes and radio buttons are left alone."),
        i18n("A field is only filled when its name or label names a fact the vault recognises; “autofill keys” lists "
             "them."),
        i18n("A drop-down is only set to a value it already offers."),
        i18n("The vault is a plain file on this machine. It is never sent anywhere."),
    };
    for (const QString &line : lines) {
        out() << u"• "_s << line << Qt::endl;
    }
    return Success;
}

void autofillOptions(QCommandLineParser &parser)
{
    const QCommandLineOption vaultOption(
        u"vault"_s, i18n("The vault file to use, instead of the one in your data folder."), u"file"_s);
    parser.addOptions({ vaultOption });
}

std::optional<int> autofillRun(const QString &verb, const QStringList &arguments, const QCommandLineParser &parser)
{
    if (verb != QLatin1String("autofill")) {
        return std::nullopt;
    }

    QStringList rest = arguments;
    const QString action = rest.isEmpty() ? QString() : rest.takeFirst().toLower();

    if (action.isEmpty()) {
        return fail(i18n("Say what to do: autofill set, get, list, remove, keys, fill, learn or limits."), UsageError);
    }
    if (action == QLatin1String("limits") || action == QLatin1String("limitations")) {
        return commandLimits();
    }
    if (action == QLatin1String("set")) {
        return commandSet(rest, parser);
    }
    if (action == QLatin1String("get")) {
        return commandGet(rest, parser);
    }
    if (action == QLatin1String("list")) {
        return commandList(parser);
    }
    if (action == QLatin1String("remove") || action == QLatin1String("delete")) {
        return commandRemove(rest, parser);
    }
    if (action == QLatin1String("keys")) {
        return commandKeys();
    }
    if (action == QLatin1String("fill")) {
        return commandFill(rest, parser);
    }
    if (action == QLatin1String("learn")) {
        return commandLearn(rest, parser);
    }

    return fail(
        i18n("“autofill %1” is not a command. Try set, get, list, remove, keys, fill, learn or limits.", action),
        UsageError);
}

QStringList autofillHelp()
{
    const auto line
        = [](const QString &syntax, const QString &what) { return u"  "_s + syntax.leftJustified(46) + what; };
    return {
        line(u"autofill set     KEY VALUE"_s, i18n("Remember a value, e.g. autofill set email you@example.org")),
        line(u"autofill list"_s, i18n("Show what the vault holds")),
        line(u"autofill remove  KEY"_s, i18n("Forget one value")),
        line(u"autofill keys"_s, i18n("The fields a form can be filled from")),
        line(u"autofill fill    FILE"_s, i18n("Fill a form from the vault, saving a copy")),
        line(u"autofill learn   FILE"_s, i18n("Take a filled form's answers into the vault")),
        line(u"autofill limits"_s, i18n("What autofill does not touch")),
    };
}

} // namespace

Group autofillGroup()
{
    return { autofillOptions, autofillRun, autofillHelp };
}

} // namespace ps::cli
