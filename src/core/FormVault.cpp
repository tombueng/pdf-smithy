/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#include "FormVault.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

#include <array>

using namespace Qt::Literals::StringLiterals;

namespace ps {

namespace {

/** Accents dropped, case folded, everything but ASCII letters and digits gone. */
QString normalize(const QString &name, const QString &label)
{
    QString raw = name + u' ' + label;
    QString decomposed = raw.normalized(QString::NormalizationForm_KD);
    decomposed.replace(QChar(0x00DF), u"ss"_s); // ß does not decompose
    decomposed.replace(QChar(0x00C6), u"ae"_s); // Æ
    decomposed.replace(QChar(0x00E6), u"ae"_s); // æ

    QString token;
    token.reserve(decomposed.size());
    for (const QChar c : decomposed) {
        if (c.category() == QChar::Mark_NonSpacing || c.category() == QChar::Mark_SpacingCombining) {
            continue;
        }
        if (c.unicode() < 128 && c.isLetterOrNumber()) {
            token.append(c.toLower());
        }
    }
    return token;
}

/**
 * The meanings and the words that stand for them, most specific first so that
 * "nachname" is a family name before the bare "name" in it makes it a full one.
 */
struct Rule {
    const char *key;
    std::array<const char *, 8> words;
};

const std::array<Rule, 16> &rules()
{
    static const std::array<Rule, 16> table { {
        { "email", { "email", "mail", "correo", "courriel", nullptr, nullptr, nullptr, nullptr } },
        { "given-name",
          { "firstname", "givenname", "forename", "vorname", "prenom", "christianname", nullptr, nullptr } },
        { "family-name", { "lastname", "surname", "familyname", "nachname", "apellido", "familyn", nullptr, nullptr } },
        { "phone", { "phone", "telephone", "telefon", "mobile", "handy", "movil", "telefono", nullptr } },
        { "organization",
          { "company", "organization", "organisation", "firma", "unternehmen", "empresa", "societe", "entreprise" } },
        { "postal-code",
          { "postalcode", "zipcode", "postcode", "plz", "postleitzahl", "codepostal", "codigopostal", nullptr } },
        { "city", { "city", "town", "stadt", "ville", "ciudad", "localidad", nullptr, nullptr } },
        { "state", { "province", "bundesland", "region", "estado", nullptr, nullptr, nullptr, nullptr } },
        { "country", { "country", "land", "pays", "pais", nullptr, nullptr, nullptr, nullptr } },
        { "street", { "street", "strasse", "rue", "calle", "anschrift", "adresse", "address", nullptr } },
        { "house-number", { "housenumber", "hausnummer", "houseno", nullptr, nullptr, nullptr, nullptr, nullptr } },
        { "iban", { "iban", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr } },
        { "bic", { "bic", "swift", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr } },
        { "website", { "website", "homepage", "webseite", "sitioweb", nullptr, nullptr, nullptr, nullptr } },
        { "date", { "date", "datum", "fecha", nullptr, nullptr, nullptr, nullptr, nullptr } },
        { "full-name",
          { "fullname", "nombrecompleto", "nomcomplet", "vollstandigername", "name", nullptr, nullptr, nullptr } },
    } };
    return table;
}

bool fillable(const FormField &field)
{
    return !field.readOnly && (field.kind == FormField::Kind::Text || field.kind == FormField::Kind::Choice);
}

} // namespace

QString FormVault::keyFor(const FormField &field)
{
    const QString token = normalize(field.name, field.label);
    if (token.isEmpty()) {
        return {};
    }
    for (const Rule &rule : rules()) {
        for (const char *word : rule.words) {
            if (word == nullptr) {
                break;
            }
            if (token.contains(QLatin1String(word))) {
                return QString::fromLatin1(rule.key);
            }
        }
    }
    return {};
}

QStringList FormVault::knownKeys()
{
    QStringList out;
    for (const Rule &rule : rules()) {
        out << QString::fromLatin1(rule.key);
    }
    return out;
}

FormVault FormVault::load(const QString &path)
{
    FormVault vault;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return vault;
    }
    const QJsonObject object = QJsonDocument::fromJson(file.readAll()).object();
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        if (it.value().isString()) {
            vault.m_entries.insert(it.key(), it.value().toString());
        }
    }
    return vault;
}

bool FormVault::save(const QString &path, QString *error) const
{
    const QFileInfo info(path);
    if (!QDir().mkpath(info.absolutePath())) {
        if (error) {
            *error = QStringLiteral("Could not create %1").arg(info.absolutePath());
        }
        return false;
    }

    QJsonObject object;
    for (auto it = m_entries.constBegin(); it != m_entries.constEnd(); ++it) {
        object.insert(it.key(), it.value());
    }

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error) {
            *error = file.errorString();
        }
        return false;
    }
    file.write(QJsonDocument(object).toJson(QJsonDocument::Indented));
    return true;
}

QString FormVault::defaultPath()
{
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return base + u"/autofill.json"_s;
}

void FormVault::set(const QString &key, const QString &value)
{
    m_entries.insert(key, value);
}

QString FormVault::value(const QString &key) const
{
    return m_entries.value(key);
}

bool FormVault::remove(const QString &key)
{
    return m_entries.remove(key) > 0;
}

QStringList FormVault::keys() const
{
    return m_entries.keys();
}

QHash<QString, QString> FormVault::fillFor(const QVector<FormField> &form) const
{
    QHash<QString, QString> out;
    for (const FormField &field : form) {
        if (!fillable(field)) {
            continue;
        }
        const QString key = keyFor(field);
        if (key.isEmpty() || !m_entries.contains(key)) {
            continue;
        }
        const QString value = m_entries.value(key);
        if (field.kind == FormField::Kind::Choice && !field.options.contains(value)) {
            continue; // never set a drop-down to something it does not offer
        }
        out.insert(field.name, value);
    }
    return out;
}

int FormVault::learnFrom(const QVector<FormField> &form)
{
    int learned = 0;
    for (const FormField &field : form) {
        if (!fillable(field) || field.value.isEmpty()) {
            continue;
        }
        const QString key = keyFor(field);
        if (key.isEmpty()) {
            continue;
        }
        m_entries.insert(key, field.value);
        ++learned;
    }
    return learned;
}

} // namespace ps
