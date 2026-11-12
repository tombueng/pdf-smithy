/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#include "TestPdf.h"

#include "core/TextEdit.h"
#include "core/TextFind.h"

#include <QTemporaryDir>
#include <QTest>

using namespace ps;
using namespace Qt::Literals::StringLiterals;

namespace {

QString sample(QTemporaryDir &dir, int pages = 3)
{
    const QString path = dir.filePath(u"sample.pdf"_s);
    return ps::test::writeSamplePdf(path, pages) ? path : QString();
}

/** A word the editor can read on some page, so the search is sure to find it. */
bool aRealToken(const QString &pdf, int pages, int *page, QString *token)
{
    for (int p = 0; p < pages; ++p) {
        QString error;
        const QVector<TextEdit::Run> runs = TextEdit::runsOn(pdf, p, &error);
        for (const TextEdit::Run &run : runs) {
            const QStringList pieces = run.text.split(u' ', Qt::SkipEmptyParts);
            for (const QString &piece : pieces) {
                if (!piece.isEmpty()) {
                    *page = p;
                    *token = piece;
                    return true;
                }
            }
        }
    }
    return false;
}

} // namespace

class TestTextFind : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void findsATermOnItsPage();
    void aMissingTermIsEmpty();
    void caseIsFoldedByDefault();
    void anEmptyTermMatchesNothing();
};

void TestTextFind::findsATermOnItsPage()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString pdf = sample(dir);
    QVERIFY(!pdf.isEmpty());

    int page = -1;
    QString token;
    QVERIFY2(aRealToken(pdf, 3, &page, &token), "the sample drew no text the editor could read");

    QString error;
    const QVector<TextFind::Hit> hits = TextFind::search(pdf, token, Qt::CaseInsensitive, &error);
    QVERIFY(error.isEmpty());

    bool onThatPage = false;
    for (const TextFind::Hit &hit : hits) {
        if (hit.page == page) {
            onThatPage = true;
            QVERIFY(hit.count >= 1);
            QVERIFY(!hit.snippet.isEmpty());
        }
    }
    QVERIFY(onThatPage);
}

void TestTextFind::aMissingTermIsEmpty()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString pdf = sample(dir);
    QVERIFY(!pdf.isEmpty());

    QString error;
    const QVector<TextFind::Hit> hits = TextFind::search(pdf, u"zzqxnotpresent"_s, Qt::CaseInsensitive, &error);
    QVERIFY(error.isEmpty());
    QVERIFY(hits.isEmpty());
}

void TestTextFind::caseIsFoldedByDefault()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString pdf = sample(dir);
    QVERIFY(!pdf.isEmpty());

    int page = -1;
    QString token;
    QVERIFY(aRealToken(pdf, 3, &page, &token));

    QString error;
    const QVector<TextFind::Hit> hits = TextFind::search(pdf, token.toUpper(), Qt::CaseInsensitive, &error);
    bool onThatPage = false;
    for (const TextFind::Hit &hit : hits) {
        if (hit.page == page) {
            onThatPage = true;
        }
    }
    QVERIFY(onThatPage);
}

void TestTextFind::anEmptyTermMatchesNothing()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString pdf = sample(dir);
    QVERIFY(!pdf.isEmpty());

    QString error;
    const QVector<TextFind::Hit> hits = TextFind::search(pdf, QString(), Qt::CaseInsensitive, &error);
    QVERIFY(hits.isEmpty());
}

QTEST_GUILESS_MAIN(TestTextFind)

#include "tst_textfind.moc"
