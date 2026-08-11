/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#include "TestPdf.h"

#include "core/Encryption.h"
#include "core/Metadata.h"

#include <QTemporaryDir>
#include <QTest>

#include <KLocalizedString>

using namespace ps;

/**
 * Metadata and passwords.
 *
 * The stripping tests matter most: "we removed your name from the document" is
 * a claim that has to be checked against the file, because a half-removal
 * (cleared from /Info but still sitting in the XMP packet) looks exactly like
 * a whole one from the inside.
 */
class TestDocumentProperties : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();

    void writesAndReadsBackFields();
    void clearsAFieldByEmptyingIt();
    void findsWhatIsThere();
    void stripsDocumentInfo();
    void keepsThePagesWhenStripping();

    void locksAndUnlocks();
    void wrongPasswordIsRefused();
    void refusesToLockWithNoPassword();
    void ownerPasswordFallsBackToTheUserPassword();

private:
    QTemporaryDir m_dir;
    QString m_plain;
};

void TestDocumentProperties::initTestCase()
{
    KLocalizedString::setApplicationDomain("pdf-smithy");
    QVERIFY(m_dir.isValid());
    m_plain = m_dir.filePath(QStringLiteral("plain.pdf"));
    QVERIFY(test::writeSamplePdf(m_plain, 4));
}

// ── Metadata ──────────────────────────────────────────────────────────────

void TestDocumentProperties::writesAndReadsBackFields()
{
    const QString out = m_dir.filePath(QStringLiteral("titled.pdf"));

    Metadata::Fields fields;
    fields.title = QStringLiteral("Mietvertrag");
    fields.author = QStringLiteral("Tom Bueng");
    fields.subject = QStringLiteral("Wohnung Erdgeschoss");
    fields.keywords = QStringLiteral("Vertrag, Miete");
    fields.created = QDateTime(QDate(2026, 3, 14), QTime(9, 26, 53));

    QString error;
    QVERIFY2(Metadata::write(m_plain, out, fields, &error), qPrintable(error));

    Metadata::Fields readBack;
    QVERIFY2(Metadata::read(out, &readBack, &error), qPrintable(error));

    QCOMPARE(readBack.title, fields.title);
    QCOMPARE(readBack.author, fields.author);
    QCOMPARE(readBack.subject, fields.subject);
    QCOMPARE(readBack.keywords, fields.keywords);
    // Umlauts and the like have to survive the round trip, which means the
    // values must be written as Unicode strings rather than as Latin-1.
    QCOMPARE(readBack.created.date(), fields.created.date());
    QCOMPARE(readBack.created.time().hour(), 9);
}

void TestDocumentProperties::clearsAFieldByEmptyingIt()
{
    const QString filled = m_dir.filePath(QStringLiteral("filled.pdf"));
    const QString cleared = m_dir.filePath(QStringLiteral("cleared.pdf"));

    Metadata::Fields fields;
    fields.author = QStringLiteral("Someone Else");
    QVERIFY(Metadata::write(m_plain, filled, fields, nullptr));

    fields.author.clear();
    QVERIFY(Metadata::write(filled, cleared, fields, nullptr));

    Metadata::Fields readBack;
    QVERIFY(Metadata::read(cleared, &readBack, nullptr));
    QVERIFY2(readBack.author.isEmpty(), "emptying a field must remove it, not write an empty string");
}

void TestDocumentProperties::findsWhatIsThere()
{
    const QString out = m_dir.filePath(QStringLiteral("inspect.pdf"));

    Metadata::Fields fields;
    fields.author = QStringLiteral("Tom Bueng");
    QVERIFY(Metadata::write(m_plain, out, fields, nullptr));

    Metadata::Findings findings;
    QVERIFY(Metadata::inspect(out, &findings, nullptr));
    QVERIFY(findings.hasDocumentInfo);
    QVERIFY(findings.anythingToStrip());

    // A freshly generated document has nothing worth reporting.
    Metadata::Findings clean;
    QVERIFY(Metadata::inspect(m_plain, &clean, nullptr));
    QCOMPARE(clean.embeddedFileCount, 0);
    QVERIFY(!clean.hasJavaScript);
}

void TestDocumentProperties::stripsDocumentInfo()
{
    const QString named = m_dir.filePath(QStringLiteral("named.pdf"));
    const QString stripped = m_dir.filePath(QStringLiteral("anonymous.pdf"));

    Metadata::Fields fields;
    fields.title = QStringLiteral("Gehaltsabrechnung");
    fields.author = QStringLiteral("Tom Bueng");
    fields.creator = QStringLiteral("SomeOfficeSuite 14.2");
    QVERIFY(Metadata::write(m_plain, named, fields, nullptr));

    QString error;
    QVERIFY2(Metadata::strip(named, stripped, Metadata::StripOptions {}, &error), qPrintable(error));

    Metadata::Fields after;
    QVERIFY(Metadata::read(stripped, &after, nullptr));
    QVERIFY2(after.isEmpty(), "something survived the strip");

    Metadata::Findings findings;
    QVERIFY(Metadata::inspect(stripped, &findings, nullptr));
    QVERIFY2(!findings.hasDocumentInfo, "/Info is still there");
    QVERIFY2(!findings.hasXmp, "the XMP packet is still there, so the strip only half happened");
}

void TestDocumentProperties::keepsThePagesWhenStripping()
{
    const QString stripped = m_dir.filePath(QStringLiteral("still-a-document.pdf"));
    QVERIFY(Metadata::strip(m_plain, stripped, Metadata::StripOptions {}, nullptr));

    // Removing metadata must not remove anything a reader would notice.
    QCOMPARE(test::pageCountOf(stripped), 4);
    QVERIFY(test::contentOf(stripped, 2).contains(QStringLiteral("PSPAGE 3")));
}

// ── Passwords ─────────────────────────────────────────────────────────────

void TestDocumentProperties::locksAndUnlocks()
{
    const QString locked = m_dir.filePath(QStringLiteral("locked.pdf"));
    const QString opened = m_dir.filePath(QStringLiteral("opened.pdf"));

    QString error;
    QVERIFY2(Encryption::encrypt(m_plain, locked, QStringLiteral("geheim"), QString(), Encryption::Permissions {},
                                 QString(), &error),
             qPrintable(error));

    QVERIFY2(Encryption::isEncrypted(locked), "the document did not end up encrypted");
    QVERIFY(Encryption::canOpen(locked, QStringLiteral("geheim")));

    QVERIFY2(Encryption::decrypt(locked, opened, QStringLiteral("geheim"), &error), qPrintable(error));
    QVERIFY2(!Encryption::isEncrypted(opened), "the password is still on the document");

    // And it is still the same document afterwards.
    QCOMPARE(test::pageCountOf(opened), 4);
    QVERIFY(test::contentOf(opened, 0).contains(QStringLiteral("PSPAGE 1")));
}

void TestDocumentProperties::wrongPasswordIsRefused()
{
    const QString locked = m_dir.filePath(QStringLiteral("locked2.pdf"));
    QVERIFY(Encryption::encrypt(m_plain, locked, QStringLiteral("richtig"), QString(), Encryption::Permissions {},
                                QString(), nullptr));

    QVERIFY(!Encryption::canOpen(locked, QStringLiteral("falsch")));
    QVERIFY(!Encryption::canOpen(locked, QString()));

    QString error;
    QVERIFY(!Encryption::decrypt(locked, m_dir.filePath(QStringLiteral("nope.pdf")), QStringLiteral("falsch"), &error));
    QVERIFY(!error.isEmpty());
}

void TestDocumentProperties::refusesToLockWithNoPassword()
{
    QString error;
    QVERIFY(!Encryption::encrypt(m_plain, m_dir.filePath(QStringLiteral("empty.pdf")), QString(), QString(),
                                 Encryption::Permissions {}, QString(), &error));
    QVERIFY(!error.isEmpty());
}

void TestDocumentProperties::ownerPasswordFallsBackToTheUserPassword()
{
    const QString locked = m_dir.filePath(QStringLiteral("owner.pdf"));
    QVERIFY(Encryption::encrypt(m_plain, locked, QStringLiteral("nutzer"), QString(), Encryption::Permissions {},
                                QString(), nullptr));

    // An empty owner password would leave the permissions removable by anyone,
    // so it must not stay empty.
    QVERIFY2(!Encryption::canOpen(locked, QString()), "the document opens with no password at all");
    QVERIFY(Encryption::canOpen(locked, QStringLiteral("nutzer")));
}

QTEST_GUILESS_MAIN(TestDocumentProperties)

#include "tst_documentproperties.moc"
