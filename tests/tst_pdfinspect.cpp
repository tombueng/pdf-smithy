/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#include "TestPdf.h"

#include "core/PdfInspect.h"

#include <QTemporaryDir>
#include <QTest>
#include <QVector>

using namespace ps;
using namespace Qt::Literals::StringLiterals;

namespace {

QString sample(QTemporaryDir &dir, int pages = 3)
{
    const QString path = dir.filePath(u"sample.pdf"_s);
    return ps::test::writeSamplePdf(path, pages) ? path : QString();
}

int firstLeafPageObject(const QString &pdf)
{
    QVector<PdfInspect::ObjectSummary> rows;
    QString error;
    if (!PdfInspect::objects(pdf, &rows, &error)) {
        return -1;
    }
    for (const PdfInspect::ObjectSummary &row : rows) {
        if (row.pdfType.contains(u"/Page"_s) && !row.pdfType.contains(u"/Pages"_s)) {
            return row.obj;
        }
    }
    return -1;
}

} // namespace

class TestPdfInspect : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void overviewReportsTheBasics();
    void objectsAreListedInOrder();
    void trailerLeadsToRoot();
    void rootIsTheCatalogue();
    void objectOpensByNumber();
    void depthLimitTruncates();
    void missingObjectIsReported();
    void brokenFileFailsCleanly();
    void limitationsAreStated();
};

void TestPdfInspect::overviewReportsTheBasics()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString pdf = sample(dir, 4);
    QVERIFY(!pdf.isEmpty());

    PdfInspect::Overview overview;
    QString error;
    QVERIFY2(PdfInspect::overview(pdf, &overview, &error), qPrintable(error));
    QVERIFY(!overview.version.isEmpty());
    QVERIFY(overview.objectCount > 0);
    QCOMPARE(overview.pageCount, 4);
    QVERIFY(!overview.encrypted);
}

void TestPdfInspect::objectsAreListedInOrder()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString pdf = sample(dir, 3);
    QVERIFY(!pdf.isEmpty());

    QVector<PdfInspect::ObjectSummary> rows;
    QString error;
    QVERIFY2(PdfInspect::objects(pdf, &rows, &error), qPrintable(error));
    QVERIFY(!rows.isEmpty());

    for (int i = 1; i < rows.size(); ++i) {
        QVERIFY(rows[i].obj >= rows[i - 1].obj);
    }

    bool hasCatalogue = false;
    bool hasPage = false;
    for (const PdfInspect::ObjectSummary &row : rows) {
        if (row.pdfType.contains(u"/Catalog"_s)) {
            hasCatalogue = true;
        }
        if (row.pdfType.contains(u"/Page"_s)) {
            hasPage = true;
        }
    }
    QVERIFY(hasCatalogue);
    QVERIFY(hasPage);
}

void TestPdfInspect::trailerLeadsToRoot()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString pdf = sample(dir);
    QVERIFY(!pdf.isEmpty());

    PdfInspect::Node node;
    QString error;
    QVERIFY2(PdfInspect::trailer(pdf, 3, &node, &error), qPrintable(error));
    QVERIFY(node.type == PdfInspect::Type::Dictionary);

    bool rootChild = false;
    for (const PdfInspect::Node &child : node.children) {
        if (child.label == u"/Root"_s) {
            QVERIFY(child.indirect);
            rootChild = true;
        }
    }
    QVERIFY(rootChild);
}

void TestPdfInspect::rootIsTheCatalogue()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString pdf = sample(dir);
    QVERIFY(!pdf.isEmpty());

    PdfInspect::Node node;
    QString error;
    QVERIFY2(PdfInspect::root(pdf, 2, &node, &error), qPrintable(error));
    QVERIFY(node.indirect);
    QVERIFY(node.type == PdfInspect::Type::Dictionary);
    QVERIFY(node.summary.contains(u"/Catalog"_s));

    bool pagesChild = false;
    for (const PdfInspect::Node &child : node.children) {
        if (child.label == u"/Pages"_s) {
            pagesChild = true;
        }
    }
    QVERIFY(pagesChild);
}

void TestPdfInspect::objectOpensByNumber()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString pdf = sample(dir);
    QVERIFY(!pdf.isEmpty());

    const int page = firstLeafPageObject(pdf);
    QVERIFY(page >= 0);

    PdfInspect::Node node;
    QString error;
    QVERIFY2(PdfInspect::object(pdf, page, -1, 4, &node, &error), qPrintable(error));
    QVERIFY(node.indirect);
    QCOMPARE(node.obj, page);
    QVERIFY(node.type == PdfInspect::Type::Dictionary);

    bool typeKey = false;
    for (const PdfInspect::Node &child : node.children) {
        if (child.label == u"/Type"_s) {
            typeKey = true;
        }
    }
    QVERIFY(typeKey);
}

void TestPdfInspect::depthLimitTruncates()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString pdf = sample(dir);
    QVERIFY(!pdf.isEmpty());

    PdfInspect::Node node;
    QString error;
    QVERIFY2(PdfInspect::root(pdf, 0, &node, &error), qPrintable(error));
    QVERIFY(node.truncated);
    QVERIFY(node.children.isEmpty());
}

void TestPdfInspect::missingObjectIsReported()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString pdf = sample(dir);
    QVERIFY(!pdf.isEmpty());

    PdfInspect::Node node;
    QString error;
    QVERIFY(!PdfInspect::object(pdf, 999999, -1, 4, &node, &error));
    QVERIFY(!error.isEmpty());
}

void TestPdfInspect::brokenFileFailsCleanly()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString broken = dir.filePath(u"broken.pdf"_s);
    QVERIFY(ps::test::writeBrokenPdf(broken));

    PdfInspect::Overview overview;
    QString error;
    // Either it recovers or it refuses — it must not crash, and a refusal must
    // say something.
    if (!PdfInspect::overview(broken, &overview, &error)) {
        QVERIFY(!error.isEmpty());
    }
}

void TestPdfInspect::limitationsAreStated()
{
    QVERIFY(!PdfInspect::limitations().isEmpty());
}

QTEST_GUILESS_MAIN(TestPdfInspect)

#include "tst_pdfinspect.moc"
