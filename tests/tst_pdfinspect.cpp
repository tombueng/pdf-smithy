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

int firstStreamObject(const QString &pdf)
{
    QVector<PdfInspect::ObjectSummary> rows;
    QString error;
    if (!PdfInspect::objects(pdf, &rows, &error)) {
        return -1;
    }
    for (const PdfInspect::ObjectSummary &row : rows) {
        if (row.typeName == u"stream"_s) {
            return row.obj;
        }
    }
    return -1;
}

const PdfInspect::Node *childByLabel(const PdfInspect::Node &node, const QString &label)
{
    for (const PdfInspect::Node &child : node.children) {
        if (child.label == label) {
            return &child;
        }
    }
    return nullptr;
}

int catalogueObject(const QString &pdf)
{
    QVector<PdfInspect::ObjectSummary> rows;
    QString error;
    if (!PdfInspect::objects(pdf, &rows, &error)) {
        return -1;
    }
    for (const PdfInspect::ObjectSummary &row : rows) {
        if (row.pdfType.contains(u"/Catalog"_s)) {
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
    void findByTypeMatchesPages();
    void findByKeyFiltersDicts();
    void findByTextIsCaseInsensitive();
    void emptyQueryMatchesEverything();
    void setReplacesAScalarAndChangesType();
    void setRefusesAMissingKey();
    void setRefusesAContainerValue();
    void setRejectsABadNumber();
    void streamReadsContent();
    void streamRefusesANonStream();
    void diffOfIdenticalFilesIsEmpty();
    void diffSeesAChangedValue();
    void diffSeesALongStringChangePastTheSummaryCutoff();
    void diffSeesAStructuralChange();
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

void TestPdfInspect::findByTypeMatchesPages()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString pdf = sample(dir, 4);
    QVERIFY(!pdf.isEmpty());

    PdfInspect::Query query;
    query.type = u"/Page"_s;
    QVector<PdfInspect::ObjectSummary> rows;
    QString error;
    QVERIFY2(PdfInspect::find(pdf, query, &rows, &error), qPrintable(error));
    QCOMPARE(rows.size(), 4);
    for (const PdfInspect::ObjectSummary &row : rows) {
        QCOMPARE(row.pdfType, u"/Page"_s);
    }

    // The leading slash is optional.
    PdfInspect::Query bare;
    bare.type = u"Page"_s;
    QVector<PdfInspect::ObjectSummary> same;
    QVERIFY(PdfInspect::find(pdf, bare, &same, &error));
    QCOMPARE(same.size(), rows.size());
}

void TestPdfInspect::findByKeyFiltersDicts()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString pdf = sample(dir);
    QVERIFY(!pdf.isEmpty());

    PdfInspect::Query query;
    query.key = u"/Kids"_s; // the page tree node carries /Kids
    QVector<PdfInspect::ObjectSummary> rows;
    QString error;
    QVERIFY2(PdfInspect::find(pdf, query, &rows, &error), qPrintable(error));
    QVERIFY(!rows.isEmpty());

    bool pagesNode = false;
    for (const PdfInspect::ObjectSummary &row : rows) {
        if (row.pdfType == u"/Pages"_s) {
            pagesNode = true;
        }
    }
    QVERIFY(pagesNode);
}

void TestPdfInspect::findByTextIsCaseInsensitive()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString pdf = sample(dir);
    QVERIFY(!pdf.isEmpty());

    PdfInspect::Query query;
    query.text = u"catalog"_s; // lower case, the file says /Catalog
    QVector<PdfInspect::ObjectSummary> rows;
    QString error;
    QVERIFY2(PdfInspect::find(pdf, query, &rows, &error), qPrintable(error));

    bool catalogue = false;
    for (const PdfInspect::ObjectSummary &row : rows) {
        if (row.pdfType.contains(u"/Catalog"_s)) {
            catalogue = true;
        }
    }
    QVERIFY(catalogue);
}

void TestPdfInspect::emptyQueryMatchesEverything()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString pdf = sample(dir);
    QVERIFY(!pdf.isEmpty());

    QString error;
    QVector<PdfInspect::ObjectSummary> all;
    QVERIFY(PdfInspect::objects(pdf, &all, &error));

    QVector<PdfInspect::ObjectSummary> matched;
    QVERIFY(PdfInspect::find(pdf, PdfInspect::Query(), &matched, &error));
    QCOMPARE(matched.size(), all.size());
}

void TestPdfInspect::setReplacesAScalarAndChangesType()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString pdf = sample(dir);
    QVERIFY(!pdf.isEmpty());

    // A page, not the catalogue: QPDF guards the root's /Type, re-stamping
    // /Catalog on write, which is the one place this edit cannot take.
    const int page = firstLeafPageObject(pdf);
    QVERIFY(page >= 0);

    // Keep the type: set /Type back to /Page. The old value reads as /Page, and
    // the file still opens.
    const QString kept = dir.filePath(u"kept.pdf"_s);
    QString oldValue;
    QString error;
    QVERIFY2(
        PdfInspect::setScalar(pdf, kept, page, -1, u"/Type"_s, u"/Page"_s, PdfInspect::Scalar::Keep, &oldValue, &error),
        qPrintable(error));
    QCOMPARE(oldValue, u"/Page"_s);
    PdfInspect::Overview overview;
    QVERIFY2(PdfInspect::overview(kept, &overview, &error), qPrintable(error));

    // Change the type: store /Type as a string instead of a name.
    const QString changed = dir.filePath(u"changed.pdf"_s);
    QVERIFY2(PdfInspect::setScalar(pdf, changed, page, -1, u"/Type"_s, u"hello"_s, PdfInspect::Scalar::String,
                                   &oldValue, &error),
             qPrintable(error));

    // Follow references from the catalogue rather than trust a number: the edited
    // page can no longer be found by being a /Page, and a save may renumber. Some
    // page under /Pages /Kids now carries a string /Type.
    PdfInspect::Node root;
    QVERIFY2(PdfInspect::root(changed, 6, &root, &error), qPrintable(error));
    const PdfInspect::Node *pages = childByLabel(root, u"/Pages"_s);
    QVERIFY(pages != nullptr);
    const PdfInspect::Node *kids = childByLabel(*pages, u"/Kids"_s);
    QVERIFY(kids != nullptr);
    QVERIFY(!kids->children.isEmpty());

    bool checked = false;
    for (const PdfInspect::Node &kid : kids->children) {
        const PdfInspect::Node *type = childByLabel(kid, u"/Type"_s);
        if (type != nullptr && type->type == PdfInspect::Type::String && type->value.contains(u"hello"_s)) {
            checked = true;
        }
    }
    QVERIFY(checked);
}

void TestPdfInspect::setRefusesAMissingKey()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString pdf = sample(dir);
    const int cat = catalogueObject(pdf);
    QVERIFY(cat >= 0);

    QString oldValue;
    QString error;
    QVERIFY(!PdfInspect::setScalar(pdf, dir.filePath(u"out.pdf"_s), cat, -1, u"/NoSuchKey"_s, u"x"_s,
                                   PdfInspect::Scalar::String, &oldValue, &error));
    QVERIFY(!error.isEmpty());
}

void TestPdfInspect::setRefusesAContainerValue()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString pdf = sample(dir);
    const int cat = catalogueObject(pdf);
    QVERIFY(cat >= 0);

    // The catalogue's /Pages is a dictionary, not a single value.
    QString oldValue;
    QString error;
    QVERIFY(!PdfInspect::setScalar(pdf, dir.filePath(u"out.pdf"_s), cat, -1, u"/Pages"_s, u"x"_s,
                                   PdfInspect::Scalar::String, &oldValue, &error));
    QVERIFY(!error.isEmpty());
}

void TestPdfInspect::setRejectsABadNumber()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString pdf = sample(dir);
    const int cat = catalogueObject(pdf);
    QVERIFY(cat >= 0);

    QString oldValue;
    QString error;
    QVERIFY(!PdfInspect::setScalar(pdf, dir.filePath(u"out.pdf"_s), cat, -1, u"/Type"_s, u"notanumber"_s,
                                   PdfInspect::Scalar::Integer, &oldValue, &error));
    QVERIFY(!error.isEmpty());
}

void TestPdfInspect::streamReadsContent()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString pdf = sample(dir);
    QVERIFY(!pdf.isEmpty());

    const int stream = firstStreamObject(pdf);
    QVERIFY(stream >= 0);

    QByteArray decoded;
    QString filters;
    QString error;
    QVERIFY2(PdfInspect::streamData(pdf, stream, -1, false, &decoded, &filters, &error), qPrintable(error));
    QVERIFY(!decoded.isEmpty());

    QByteArray raw;
    QVERIFY2(PdfInspect::streamData(pdf, stream, -1, true, &raw, &filters, &error), qPrintable(error));
    QVERIFY(!raw.isEmpty());
}

void TestPdfInspect::streamRefusesANonStream()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString pdf = sample(dir);
    const int cat = catalogueObject(pdf);
    QVERIFY(cat >= 0);

    QByteArray bytes;
    QString filters;
    QString error;
    QVERIFY(!PdfInspect::streamData(pdf, cat, -1, false, &bytes, &filters, &error));
    QVERIFY(!error.isEmpty());
}

void TestPdfInspect::diffOfIdenticalFilesIsEmpty()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString pdf = sample(dir);
    QVERIFY(!pdf.isEmpty());

    QVector<PdfInspect::Difference> diffs;
    QString error;
    QVERIFY2(PdfInspect::diff(pdf, pdf, &diffs, &error), qPrintable(error));
    QVERIFY(diffs.isEmpty());
}

void TestPdfInspect::diffSeesAChangedValue()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString pdf = sample(dir);
    QVERIFY(!pdf.isEmpty());

    const int page = firstLeafPageObject(pdf);
    QVERIFY(page >= 0);

    const QString edited = dir.filePath(u"edited.pdf"_s);
    QString oldValue;
    QString error;
    QVERIFY2(PdfInspect::setScalar(pdf, edited, page, -1, u"/Type"_s, u"hello"_s, PdfInspect::Scalar::String, &oldValue,
                                   &error),
             qPrintable(error));

    QVector<PdfInspect::Difference> diffs;
    QVERIFY2(PdfInspect::diff(pdf, edited, &diffs, &error), qPrintable(error));
    QVERIFY(!diffs.isEmpty());

    bool found = false;
    for (const PdfInspect::Difference &entry : diffs) {
        if (entry.kind == PdfInspect::Difference::Kind::Changed && entry.path.contains(u"/Type"_s)
            && entry.before.contains(u"/Page"_s) && entry.after.contains(u"hello"_s)) {
            found = true;
        }
    }
    QVERIFY(found);
}

void TestPdfInspect::diffSeesALongStringChangePastTheSummaryCutoff()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString pdf = sample(dir);
    QVERIFY(!pdf.isEmpty());

    const int page = firstLeafPageObject(pdf);
    QVERIFY(page >= 0);

    // Two values that are identical for the first eighty characters and differ
    // only at the very end — past where the readable summary elides them. The
    // diff must still see them as different.
    const QString shared(80, u'A');
    const QString longOne = shared + u"ONE"_s;
    const QString longTwo = shared + u"TWO"_s;

    const QString fileOne = dir.filePath(u"one.pdf"_s);
    const QString fileTwo = dir.filePath(u"two.pdf"_s);
    QString ignore;
    QString error;
    QVERIFY2(
        PdfInspect::setScalar(pdf, fileOne, page, -1, u"/Type"_s, longOne, PdfInspect::Scalar::String, &ignore, &error),
        qPrintable(error));
    QVERIFY2(
        PdfInspect::setScalar(pdf, fileTwo, page, -1, u"/Type"_s, longTwo, PdfInspect::Scalar::String, &ignore, &error),
        qPrintable(error));

    QVector<PdfInspect::Difference> diffs;
    QVERIFY2(PdfInspect::diff(fileOne, fileTwo, &diffs, &error), qPrintable(error));

    bool found = false;
    for (const PdfInspect::Difference &entry : diffs) {
        if (entry.kind == PdfInspect::Difference::Kind::Changed && entry.path.contains(u"/Type"_s)) {
            found = true;
        }
    }
    QVERIFY2(found, "a change past the summary's cutoff was not reported");
}

void TestPdfInspect::diffSeesAStructuralChange()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString three = dir.filePath(u"three.pdf"_s);
    const QString five = dir.filePath(u"five.pdf"_s);
    QVERIFY(ps::test::writeSamplePdf(three, 3));
    QVERIFY(ps::test::writeSamplePdf(five, 5));

    QVector<PdfInspect::Difference> diffs;
    QString error;
    QVERIFY2(PdfInspect::diff(three, five, &diffs, &error), qPrintable(error));
    QVERIFY(!diffs.isEmpty());

    bool countSeen = false;
    for (const PdfInspect::Difference &entry : diffs) {
        if (entry.path.contains(u"Count"_s)) {
            countSeen = true;
        }
    }
    QVERIFY(countSeen);
}

void TestPdfInspect::limitationsAreStated()
{
    QVERIFY(!PdfInspect::limitations().isEmpty());
}

QTEST_GUILESS_MAIN(TestPdfInspect)

#include "tst_pdfinspect.moc"
