/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#include "TestPdf.h"

#include "core/Document.h"
#include "core/DocumentWriter.h"
#include "core/Metadata.h"

#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>

using namespace ps;

/**
 * The writer is where mistakes become permanent, so these tests check the
 * result on disk rather than the in-memory model.
 */
class TestDocumentWriter : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();

    void writesEveryPage();
    void preservesPageOrder();
    void writesReorderedPages();
    void writesRotation();
    void addsRotationToExistingRotation();
    void writesSelectionOnly();
    void writesMergedSources();
    void writesDuplicatedPages();

    void refusesEmptySelection();
    void overwritesItsOwnSourceSafely();
    void leavesTargetIntactWhenWriteFails();
    void producesDeterministicOutput();
    void carriesMetadataAcross();

private:
    QTemporaryDir m_dir;
    QString m_tenPages;
    QString m_threePages;
    QString m_rotated;
    DocumentWriter::Options m_options;
};

void TestDocumentWriter::initTestCase()
{
    QVERIFY(m_dir.isValid());
    m_tenPages = m_dir.filePath(QStringLiteral("ten.pdf"));
    m_threePages = m_dir.filePath(QStringLiteral("three.pdf"));
    m_rotated = m_dir.filePath(QStringLiteral("rotated.pdf"));

    QVERIFY(test::writeSamplePdf(m_tenPages, 10));
    QVERIFY(test::writeSamplePdf(m_threePages, 3));
    QVERIFY(test::writeRotatedPdf(m_rotated, 4, 90));
}

void TestDocumentWriter::writesEveryPage()
{
    Document document;
    QVERIFY(document.open(m_tenPages, nullptr));

    const QString out = m_dir.filePath(QStringLiteral("all.pdf"));
    QString error;
    QVERIFY2(DocumentWriter::write(document, out, m_options, &error), qPrintable(error));

    QCOMPARE(test::pageCountOf(out), 10);
}

void TestDocumentWriter::preservesPageOrder()
{
    Document document;
    QVERIFY(document.open(m_tenPages, nullptr));

    const QString out = m_dir.filePath(QStringLiteral("order.pdf"));
    QVERIFY(DocumentWriter::write(document, out, m_options, nullptr));

    for (int i = 0; i < 10; ++i) {
        QVERIFY2(test::contentOf(out, i).contains(QStringLiteral("PSPAGE %1").arg(i + 1)),
                 qPrintable(QStringLiteral("page %1 is not the original page %1").arg(i + 1)));
    }
}

void TestDocumentWriter::writesReorderedPages()
{
    Document document;
    QVERIFY(document.open(m_tenPages, nullptr));

    // Reverse the document, then check the file really came out backwards.
    QVector<PageRef> reversed;
    for (int i = document.pageCount() - 1; i >= 0; --i) {
        reversed.append(document.pageAt(i));
    }
    document.setPages(reversed);

    const QString out = m_dir.filePath(QStringLiteral("reversed.pdf"));
    QVERIFY(DocumentWriter::write(document, out, m_options, nullptr));

    QCOMPARE(test::pageCountOf(out), 10);
    QVERIFY(test::contentOf(out, 0).contains(QStringLiteral("PSPAGE 10")));
    QVERIFY(test::contentOf(out, 9).contains(QStringLiteral("PSPAGE 1")));
}

void TestDocumentWriter::writesRotation()
{
    Document document;
    QVERIFY(document.open(m_tenPages, nullptr));
    document.applyRotation({ 0, 1 }, 90);
    document.applyRotation({ 2 }, 180);

    const QString out = m_dir.filePath(QStringLiteral("rotation.pdf"));
    QVERIFY(DocumentWriter::write(document, out, m_options, nullptr));

    QCOMPARE(test::rotationOf(out, 0), 90);
    QCOMPARE(test::rotationOf(out, 1), 90);
    QCOMPARE(test::rotationOf(out, 2), 180);
    QCOMPARE(test::rotationOf(out, 3), 0);
}

void TestDocumentWriter::addsRotationToExistingRotation()
{
    Document document;
    QVERIFY(document.open(m_rotated, nullptr));
    // The source pages already sit at /Rotate 90; the user adds another turn.
    document.applyRotation({ 0 }, 90);

    const QString out = m_dir.filePath(QStringLiteral("double-rotation.pdf"));
    QVERIFY(DocumentWriter::write(document, out, m_options, nullptr));

    QCOMPARE(test::rotationOf(out, 0), 180);
    QCOMPARE(test::rotationOf(out, 1), 90);
}

void TestDocumentWriter::writesSelectionOnly()
{
    Document document;
    QVERIFY(document.open(m_tenPages, nullptr));

    const QString out = m_dir.filePath(QStringLiteral("selection.pdf"));
    QVERIFY(DocumentWriter::writeSelection(document, { 7, 2, 0 }, out, m_options, nullptr));

    // Written in the order given, not in page order, which is what "extract
    // these three pages, in this order" has to mean.
    QCOMPARE(test::pageCountOf(out), 3);
    QVERIFY(test::contentOf(out, 0).contains(QStringLiteral("PSPAGE 8")));
    QVERIFY(test::contentOf(out, 1).contains(QStringLiteral("PSPAGE 3")));
    QVERIFY(test::contentOf(out, 2).contains(QStringLiteral("PSPAGE 1")));
}

void TestDocumentWriter::writesMergedSources()
{
    Document document;
    QVERIFY(document.open(m_threePages, nullptr));
    QVERIFY(document.importFile(m_tenPages, -1, nullptr));

    const QString out = m_dir.filePath(QStringLiteral("merged.pdf"));
    QString error;
    QVERIFY2(DocumentWriter::write(document, out, m_options, &error), qPrintable(error));

    QCOMPARE(test::pageCountOf(out), 13);
    QVERIFY(test::contentOf(out, 2).contains(QStringLiteral("PSPAGE 3")));
    QVERIFY(test::contentOf(out, 3).contains(QStringLiteral("PSPAGE 1")));
    QVERIFY(test::contentOf(out, 12).contains(QStringLiteral("PSPAGE 10")));
}

void TestDocumentWriter::writesDuplicatedPages()
{
    Document document;
    QVERIFY(document.open(m_threePages, nullptr));

    // The same source page three times, each turned differently. If the writer
    // let them share one page object, the rotations would fight each other.
    document.setPages({ PageRef { 0, 1, 0 }, PageRef { 0, 1, 90 }, PageRef { 0, 1, 180 } });

    const QString out = m_dir.filePath(QStringLiteral("duplicated.pdf"));
    QString error;
    QVERIFY2(DocumentWriter::write(document, out, m_options, &error), qPrintable(error));

    QCOMPARE(test::pageCountOf(out), 3);
    QCOMPARE(test::rotationOf(out, 0), 0);
    QCOMPARE(test::rotationOf(out, 1), 90);
    QCOMPARE(test::rotationOf(out, 2), 180);
    for (int i = 0; i < 3; ++i) {
        QVERIFY(test::contentOf(out, i).contains(QStringLiteral("PSPAGE 2")));
    }
}

// ── Failure modes ─────────────────────────────────────────────────────────

void TestDocumentWriter::refusesEmptySelection()
{
    Document document;
    QVERIFY(document.open(m_tenPages, nullptr));

    QString error;
    QVERIFY(
        !DocumentWriter::writeSelection(document, {}, m_dir.filePath(QStringLiteral("empty.pdf")), m_options, &error));
    QVERIFY(!error.isEmpty());
}

void TestDocumentWriter::overwritesItsOwnSourceSafely()
{
    // Saving over the file you are reading from is the single most dangerous
    // thing this class does, and the most common thing a user asks of it.
    const QString path = m_dir.filePath(QStringLiteral("in-place.pdf"));
    QVERIFY(test::writeSamplePdf(path, 6));

    Document document;
    QVERIFY(document.open(path, nullptr));
    document.removePages({ 0, 1 });

    QString error;
    QVERIFY2(DocumentWriter::write(document, path, m_options, &error), qPrintable(error));

    QCOMPARE(test::pageCountOf(path), 4);
    QVERIFY(test::contentOf(path, 0).contains(QStringLiteral("PSPAGE 3")));

    // No temporary files left lying about next to the document.
    const QStringList leftovers
        = QDir(m_dir.path()).entryList({ QStringLiteral(".pdf-smithy-*") }, QDir::Files | QDir::Hidden);
    QVERIFY2(leftovers.isEmpty(), qPrintable(leftovers.join(QLatin1Char(' '))));
}

void TestDocumentWriter::leavesTargetIntactWhenWriteFails()
{
    const QString target = m_dir.filePath(QStringLiteral("precious.pdf"));
    QVERIFY(test::writeSamplePdf(target, 5));
    const qint64 sizeBefore = QFileInfo(target).size();

    Document document;
    QVERIFY(document.open(m_tenPages, nullptr));

    QString error;
    // Nothing selected: the writer must bail out before touching the target.
    QVERIFY(!DocumentWriter::writeSelection(document, {}, target, m_options, &error));

    QCOMPARE(QFileInfo(target).size(), sizeBefore);
    QCOMPARE(test::pageCountOf(target), 5);
}

void TestDocumentWriter::producesDeterministicOutput()
{
    Document document;
    QVERIFY(document.open(m_tenPages, nullptr));

    const QString first = m_dir.filePath(QStringLiteral("det-1.pdf"));
    const QString second = m_dir.filePath(QStringLiteral("det-2.pdf"));
    QVERIFY(DocumentWriter::write(document, first, m_options, nullptr));
    QVERIFY(DocumentWriter::write(document, second, m_options, nullptr));

    QFile a(first);
    QFile b(second);
    QVERIFY(a.open(QIODevice::ReadOnly));
    QVERIFY(b.open(QIODevice::ReadOnly));
    // Byte-identical output makes regressions visible in a diff and lets the
    // build be reproducible.
    QCOMPARE(a.readAll(), b.readAll());
}

void TestDocumentWriter::carriesMetadataAcross()
{
    // Opening a document and saving it must not quietly empty out its title
    // and author. Output is assembled from scratch, so this only works if the
    // metadata is deliberately carried over.
    const QString titled = m_dir.filePath(QStringLiteral("titled-source.pdf"));
    QVERIFY(test::writeSamplePdf(titled, 3));

    Metadata::Fields fields;
    fields.title = QStringLiteral("Mietvertrag");
    fields.author = QStringLiteral("Tom Bueng");
    QVERIFY(Metadata::write(titled, titled, fields, nullptr));

    Document document;
    QVERIFY(document.open(titled, nullptr));
    QCOMPARE(document.metadata().title, QStringLiteral("Mietvertrag"));

    const QString out = m_dir.filePath(QStringLiteral("kept-metadata.pdf"));
    QString error;
    QVERIFY2(DocumentWriter::write(document, out, m_options, &error), qPrintable(error));

    Metadata::Fields after;
    QVERIFY(Metadata::read(out, &after, nullptr));
    QCOMPARE(after.title, QStringLiteral("Mietvertrag"));
    QCOMPARE(after.author, QStringLiteral("Tom Bueng"));

    // And editing it takes effect on the next save.
    Metadata::Fields edited = document.metadata();
    edited.author = QStringLiteral("Jemand Anderes");
    document.setMetadata(edited);

    const QString second = m_dir.filePath(QStringLiteral("edited-metadata.pdf"));
    QVERIFY(DocumentWriter::write(document, second, m_options, nullptr));
    QVERIFY(Metadata::read(second, &after, nullptr));
    QCOMPARE(after.author, QStringLiteral("Jemand Anderes"));
}

QTEST_GUILESS_MAIN(TestDocumentWriter)

#include "tst_documentwriter.moc"
