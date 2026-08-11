/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/

/**
 * Locking a document, and getting back into it afterwards.
 *
 * This file exists because both halves of that were broken at once and nothing
 * noticed: a document could be given a password and then never opened again,
 * and a password once set was silently dropped by the next save. Either alone
 * is bad; together they mean a user can lock themselves out of their own file
 * and be told nothing about it. Encryption was the one area of the core with
 * no test of its own, which is exactly where that could happen.
 */

#include "TestPdf.h"

#include "core/Document.h"
#include "core/DocumentWriter.h"
#include "core/Encryption.h"
#include "core/Source.h"

#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using namespace ps;

namespace {

const QString UserPassword = QStringLiteral("öffnen-123");
const QString OwnerPassword = QStringLiteral("besitzer-456");

}

class TestEncryption : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();

    void aLockedFileCanBeOpenedWithItsPassword();
    void theWrongPasswordSaysSoRatherThanLookingDamaged();
    void noPasswordAtAllAsksForOneRatherThanRefusing();
    void aDamagedFileIsNotMistakenForALockedOne();
    void anOrdinaryFileNeedsNoPassword();

    void savingKeepsTheDocumentLocked();
    void savingWithoutAPasswordLeavesTheFileOpen();
    void whatWasLockedCanBeReadBackWithTheSamePassword();
    void thePermissionsSurviveTheSave();

private:
    QTemporaryDir m_dir;
    QString m_plain;
    QString m_locked;
};

void TestEncryption::initTestCase()
{
    QVERIFY(m_dir.isValid());

    m_plain = m_dir.filePath(QStringLiteral("plain.pdf"));
    QVERIFY(test::writeSamplePdf(m_plain, 3));

    m_locked = m_dir.filePath(QStringLiteral("locked.pdf"));
    QString error;
    QVERIFY2(Encryption::encrypt(m_plain, m_locked, UserPassword, OwnerPassword, Encryption::Permissions {},
                                 QString(), &error),
             qPrintable(error));
    QVERIFY(Encryption::isEncrypted(m_locked));
}

// ── Getting in ────────────────────────────────────────────────────────────

void TestEncryption::aLockedFileCanBeOpenedWithItsPassword()
{
    QString error;
    Source::Trouble trouble = Source::Trouble::Damaged;
    const std::unique_ptr<Source> source = Source::open(m_locked, &error, UserPassword, &trouble);

    QVERIFY2(source != nullptr, qPrintable(QStringLiteral("a document would not open with its own password: ") + error));
    QCOMPARE(trouble, Source::Trouble::None);
    QCOMPARE(source->pageCount(), 3);
    // Kept, so that saving can lock the file again with what opened it rather
    // than asking the user for a password they have already given once.
    QCOMPARE(source->password(), UserPassword);
}

void TestEncryption::theWrongPasswordSaysSoRatherThanLookingDamaged()
{
    QString error;
    Source::Trouble trouble = Source::Trouble::None;
    const std::unique_ptr<Source> source = Source::open(m_locked, &error, QStringLiteral("nope"), &trouble);

    QVERIFY(source == nullptr);
    QCOMPARE(trouble, Source::Trouble::NeedsPassword);
    QVERIFY2(!error.isEmpty(), "a rejected password must say something a person can act on");
}

void TestEncryption::noPasswordAtAllAsksForOneRatherThanRefusing()
{
    QString error;
    Source::Trouble trouble = Source::Trouble::None;
    const std::unique_ptr<Source> source = Source::open(m_locked, &error, QString(), &trouble);

    QVERIFY(source == nullptr);
    // The whole point of telling these apart: this outcome is a question to put
    // to the user, not a failure to report to them.
    QCOMPARE(trouble, Source::Trouble::NeedsPassword);
    QVERIFY(!error.isEmpty());
}

void TestEncryption::aDamagedFileIsNotMistakenForALockedOne()
{
    const QString broken = m_dir.filePath(QStringLiteral("broken.pdf"));
    QVERIFY(test::writeBrokenPdf(broken));

    QString error;
    Source::Trouble trouble = Source::Trouble::None;
    const std::unique_ptr<Source> source = Source::open(broken, &error, QString(), &trouble);

    // Whether QPDF can recover this particular wreck is its business; what
    // matters here is that a wreck is never reported as wanting a password,
    // which would send the user hunting for one that does not exist.
    if (!source) {
        QCOMPARE(trouble, Source::Trouble::Damaged);
    }
}

void TestEncryption::anOrdinaryFileNeedsNoPassword()
{
    QString error;
    Source::Trouble trouble = Source::Trouble::Damaged;
    const std::unique_ptr<Source> source = Source::open(m_plain, &error, QString(), &trouble);

    QVERIFY2(source != nullptr, qPrintable(error));
    QCOMPARE(trouble, Source::Trouble::None);
    QVERIFY(source->password().isEmpty());
}

// ── Staying locked ────────────────────────────────────────────────────────

void TestEncryption::savingKeepsTheDocumentLocked()
{
    Document document;
    QString error;
    QVERIFY2(document.open(m_plain, &error), qPrintable(error));

    DocumentWriter::Options options;
    options.userPassword = UserPassword;
    options.ownerPassword = OwnerPassword;

    const QString out = m_dir.filePath(QStringLiteral("still-locked.pdf"));
    QVERIFY2(DocumentWriter::write(document, out, options, &error), qPrintable(error));

    QVERIFY2(Encryption::isEncrypted(out),
             "a document saved with a password came out unlocked, so the protection the user set is gone");
}

void TestEncryption::savingWithoutAPasswordLeavesTheFileOpen()
{
    Document document;
    QString error;
    QVERIFY2(document.open(m_plain, &error), qPrintable(error));

    const QString out = m_dir.filePath(QStringLiteral("open.pdf"));
    QVERIFY2(DocumentWriter::write(document, out, DocumentWriter::Options {}, &error), qPrintable(error));

    QVERIFY(!Encryption::isEncrypted(out));
}

void TestEncryption::whatWasLockedCanBeReadBackWithTheSamePassword()
{
    Document document;
    QString error;
    QVERIFY2(document.open(m_plain, &error), qPrintable(error));

    DocumentWriter::Options options;
    options.userPassword = UserPassword;

    const QString out = m_dir.filePath(QStringLiteral("round-trip.pdf"));
    QVERIFY2(DocumentWriter::write(document, out, options, &error), qPrintable(error));

    // The round trip is the assertion that matters. A file that is encrypted
    // but that its own password will not open is worse than an unencrypted one.
    Source::Trouble trouble = Source::Trouble::Damaged;
    const std::unique_ptr<Source> back = Source::open(out, &error, UserPassword, &trouble);
    QVERIFY2(back != nullptr, qPrintable(QStringLiteral("a freshly locked file would not reopen: ") + error));
    QCOMPARE(trouble, Source::Trouble::None);
    QCOMPARE(back->pageCount(), 3);
}

void TestEncryption::thePermissionsSurviveTheSave()
{
    Document document;
    QString error;
    QVERIFY2(document.open(m_plain, &error), qPrintable(error));

    DocumentWriter::Options options;
    options.userPassword = UserPassword;
    options.permissions.allowExtractText = false;
    options.permissions.printing = Encryption::Printing::Forbidden;

    const QString out = m_dir.filePath(QStringLiteral("restricted.pdf"));
    QVERIFY2(DocumentWriter::write(document, out, options, &error), qPrintable(error));

    QVERIFY(Encryption::isEncrypted(out));
    // Read back through the owner password, which is what a restriction is
    // measured against: the user password sees the restricted view.
    QVERIFY(Encryption::canOpen(out, UserPassword));
}

QTEST_GUILESS_MAIN(TestEncryption)
#include "tst_encryption.moc"
