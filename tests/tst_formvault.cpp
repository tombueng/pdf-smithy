/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#include "core/FormVault.h"

#include <QTemporaryDir>
#include <QTest>

using namespace ps;
using namespace Qt::Literals::StringLiterals;

namespace {

FormField field(const QString &name, const QString &label = QString(), FormField::Kind kind = FormField::Kind::Text,
                const QString &value = QString(), const QStringList &options = QStringList(), bool readOnly = false)
{
    FormField f;
    f.name = name;
    f.label = label;
    f.kind = kind;
    f.value = value;
    f.options = options;
    f.readOnly = readOnly;
    return f;
}

} // namespace

class TestFormVault : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void keyForRecognisesManyLanguages();
    void keyForPrefersSpecific();
    void unknownFieldIsNotPlaced();
    void saveAndLoadRoundTrip();
    void fillMatchesByMeaning();
    void fillRespectsChoiceOptions();
    void fillSkipsReadOnlyAndButtons();
    void learnStoresFilledValues();
    void knownKeysAreListed();
};

void TestFormVault::keyForRecognisesManyLanguages()
{
    QCOMPARE(FormVault::keyFor(field(u"email"_s)), u"email"_s);
    QCOMPARE(FormVault::keyFor(field(u"f1"_s, u"E-Mail-Adresse"_s)), u"email"_s);
    QCOMPARE(FormVault::keyFor(field(u"vorname"_s)), u"given-name"_s);
    QCOMPARE(FormVault::keyFor(field(u"Nachname"_s)), u"family-name"_s);
    QCOMPARE(FormVault::keyFor(field(u"PLZ"_s)), u"postal-code"_s);
    QCOMPARE(FormVault::keyFor(field(u"f2"_s, u"Téléphone"_s)), u"phone"_s);
    QCOMPARE(FormVault::keyFor(field(u"Straße"_s)), u"street"_s);
    QCOMPARE(FormVault::keyFor(field(u"country"_s)), u"country"_s);
    QCOMPARE(FormVault::keyFor(field(u"IBAN"_s)), u"iban"_s);
    QCOMPARE(FormVault::keyFor(field(u"ciudad"_s)), u"city"_s);
}

void TestFormVault::keyForPrefersSpecific()
{
    // "nachname" contains "name", but the specific meaning must win.
    QCOMPARE(FormVault::keyFor(field(u"nachname"_s)), u"family-name"_s);
    QCOMPARE(FormVault::keyFor(field(u"firstName"_s)), u"given-name"_s);
    // A bare "name" falls through to the full name.
    QCOMPARE(FormVault::keyFor(field(u"name"_s)), u"full-name"_s);
}

void TestFormVault::unknownFieldIsNotPlaced()
{
    QVERIFY(FormVault::keyFor(field(u"widget_42"_s)).isEmpty());
    QVERIFY(FormVault::keyFor(field(QString())).isEmpty());
}

void TestFormVault::saveAndLoadRoundTrip()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(u"vault.json"_s);

    FormVault vault;
    vault.set(u"email"_s, u"you@example.org"_s);
    vault.set(u"given-name"_s, u"Alex"_s);
    QString error;
    QVERIFY2(vault.save(path, &error), qPrintable(error));

    const FormVault back = FormVault::load(path);
    QCOMPARE(back.value(u"email"_s), u"you@example.org"_s);
    QCOMPARE(back.value(u"given-name"_s), u"Alex"_s);
    QCOMPARE(back.keys().size(), 2);
}

void TestFormVault::fillMatchesByMeaning()
{
    FormVault vault;
    vault.set(u"email"_s, u"you@example.org"_s);
    vault.set(u"given-name"_s, u"Alex"_s);

    const QVector<FormField> form {
        field(u"email_addr"_s, u"Your email"_s),
        field(u"first"_s, u"Vorname"_s),
        field(u"unmapped"_s),
    };
    const QHash<QString, QString> filled = vault.fillFor(form);
    QCOMPARE(filled.size(), 2);
    QCOMPARE(filled.value(u"email_addr"_s), u"you@example.org"_s);
    QCOMPARE(filled.value(u"first"_s), u"Alex"_s);
    QVERIFY(!filled.contains(u"unmapped"_s));
}

void TestFormVault::fillRespectsChoiceOptions()
{
    FormVault offers;
    offers.set(u"country"_s, u"DE"_s);
    const QVector<FormField> ok { field(u"land"_s, QString(), FormField::Kind::Choice, QString(),
                                        { u"DE"_s, u"FR"_s }) };
    QCOMPARE(offers.fillFor(ok).value(u"land"_s), u"DE"_s);

    FormVault missing;
    missing.set(u"country"_s, u"US"_s);
    const QVector<FormField> notOffered { field(u"land"_s, QString(), FormField::Kind::Choice, QString(),
                                                { u"DE"_s, u"FR"_s }) };
    QVERIFY(missing.fillFor(notOffered).isEmpty());
}

void TestFormVault::fillSkipsReadOnlyAndButtons()
{
    FormVault vault;
    vault.set(u"email"_s, u"you@example.org"_s);

    const QVector<FormField> form {
        field(u"email"_s, QString(), FormField::Kind::Text, QString(), QStringList(), /*readOnly=*/true),
        field(u"email_button"_s, u"Email"_s, FormField::Kind::Button),
    };
    QVERIFY(vault.fillFor(form).isEmpty());
}

void TestFormVault::learnStoresFilledValues()
{
    FormVault vault;
    const QVector<FormField> form {
        field(u"email"_s, QString(), FormField::Kind::Text, u"a@b.c"_s),
        field(u"vorname"_s, QString(), FormField::Kind::Text, u"Tom"_s),
        field(u"empty"_s, u"Nachname"_s, FormField::Kind::Text, QString()),
        field(u"submit"_s, QString(), FormField::Kind::Button, u"ignored"_s),
    };
    const int learned = vault.learnFrom(form);
    QCOMPARE(learned, 2);
    QCOMPARE(vault.value(u"email"_s), u"a@b.c"_s);
    QCOMPARE(vault.value(u"given-name"_s), u"Tom"_s);
    QVERIFY(vault.value(u"family-name"_s).isEmpty());
}

void TestFormVault::knownKeysAreListed()
{
    const QStringList keys = FormVault::knownKeys();
    QVERIFY(!keys.isEmpty());
    QVERIFY(keys.contains(u"email"_s));
    QVERIFY(keys.contains(u"given-name"_s));
}

QTEST_GUILESS_MAIN(TestFormVault)

#include "tst_formvault.moc"
