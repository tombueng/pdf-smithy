/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#include "TestPdf.h"

#include "core/Document.h"
#include "core/commands/PageCommands.h"

#include <QTemporaryDir>
#include <QTest>
#include <QUndoStack>

using namespace ps;

/**
 * Undo has one job: put the document back exactly as it was. Every test here
 * captures the page order before the edit and demands it back afterwards.
 */
class TestPageCommands : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void init();

    void removeIsUndoable();
    void removeScatteredIsUndoable();
    void insertIsUndoable();
    void moveIsUndoable();
    void moveBackwardIsUndoable();
    void moveScatteredIsUndoable();
    void rotateIsUndoable();
    void rotationsMergeIntoOneStep();
    void fourQuarterTurnsCancelOut();
    void duplicateIsUndoable();

    void redoRestoresTheEdit();
    void editingMarksDocumentModified();
    void longSequenceUndoesCompletely();

private:
    /** The page order as a comparable fingerprint. */
    static QVector<QPair<int, int>> fingerprint(const Document &document);

    QTemporaryDir m_dir;
    QString m_file;
    Document m_document;
};

QVector<QPair<int, int>> TestPageCommands::fingerprint(const Document &document)
{
    QVector<QPair<int, int>> out;
    out.reserve(document.pageCount());
    for (int i = 0; i < document.pageCount(); ++i) {
        const PageRef ref = document.pageAt(i);
        out.append({ ref.sourcePage, ref.rotation });
    }
    return out;
}

void TestPageCommands::initTestCase()
{
    QVERIFY(m_dir.isValid());
    m_file = m_dir.filePath(QStringLiteral("ten.pdf"));
    QVERIFY(test::writeSamplePdf(m_file, 10));
}

void TestPageCommands::init()
{
    QVERIFY(m_document.open(m_file, nullptr));
}

// ── Individual commands ───────────────────────────────────────────────────

void TestPageCommands::removeIsUndoable()
{
    const auto before = fingerprint(m_document);

    m_document.undoStack()->push(new RemovePagesCommand(&m_document, { 2, 3, 4 }));
    QCOMPARE(m_document.pageCount(), 7);

    m_document.undoStack()->undo();
    QCOMPARE(m_document.pageCount(), 10);
    QCOMPARE(fingerprint(m_document), before);
}

void TestPageCommands::removeScatteredIsUndoable()
{
    const auto before = fingerprint(m_document);

    m_document.undoStack()->push(new RemovePagesCommand(&m_document, { 0, 4, 5, 9 }));
    QCOMPARE(m_document.pageCount(), 6);

    m_document.undoStack()->undo();
    QCOMPARE(fingerprint(m_document), before);
}

void TestPageCommands::insertIsUndoable()
{
    const auto before = fingerprint(m_document);

    const QVector<PageRef> extra { PageRef { 0, 1, 90 }, PageRef { 0, 2, 180 } };
    m_document.undoStack()->push(new InsertPagesCommand(&m_document, 4, extra, QStringLiteral("test insert")));
    QCOMPARE(m_document.pageCount(), 12);
    QCOMPARE(m_document.pageAt(4).rotation, 90);

    m_document.undoStack()->undo();
    QCOMPARE(fingerprint(m_document), before);
}

void TestPageCommands::moveIsUndoable()
{
    const auto before = fingerprint(m_document);

    m_document.undoStack()->push(new MovePagesCommand(&m_document, { 0, 1 }, 6));
    QCOMPARE(m_document.pageAt(4).sourcePage, 0);

    m_document.undoStack()->undo();
    QCOMPARE(fingerprint(m_document), before);
}

void TestPageCommands::moveBackwardIsUndoable()
{
    const auto before = fingerprint(m_document);

    m_document.undoStack()->push(new MovePagesCommand(&m_document, { 8, 9 }, 1));
    QCOMPARE(m_document.pageAt(1).sourcePage, 8);

    m_document.undoStack()->undo();
    QCOMPARE(fingerprint(m_document), before);
}

void TestPageCommands::moveScatteredIsUndoable()
{
    const auto before = fingerprint(m_document);

    m_document.undoStack()->push(new MovePagesCommand(&m_document, { 1, 4, 7 }, 3));
    QCOMPARE(m_document.pageCount(), 10);

    m_document.undoStack()->undo();
    QCOMPARE(fingerprint(m_document), before);
}

void TestPageCommands::rotateIsUndoable()
{
    const auto before = fingerprint(m_document);

    m_document.undoStack()->push(new RotatePagesCommand(&m_document, { 1, 3 }, 90));
    QCOMPARE(m_document.pageAt(1).rotation, 90);
    QCOMPARE(m_document.pageAt(3).rotation, 90);
    QCOMPARE(m_document.pageAt(2).rotation, 0);

    m_document.undoStack()->undo();
    QCOMPARE(fingerprint(m_document), before);
}

void TestPageCommands::rotationsMergeIntoOneStep()
{
    m_document.undoStack()->push(new RotatePagesCommand(&m_document, { 2 }, 90));
    m_document.undoStack()->push(new RotatePagesCommand(&m_document, { 2 }, 90));

    QCOMPARE(m_document.pageAt(2).rotation, 180);
    // Hammering the rotate button is one thought, so it is one undo step.
    QCOMPARE(m_document.undoStack()->count(), 1);

    m_document.undoStack()->undo();
    QCOMPARE(m_document.pageAt(2).rotation, 0);
}

void TestPageCommands::fourQuarterTurnsCancelOut()
{
    for (int i = 0; i < 4; ++i) {
        m_document.undoStack()->push(new RotatePagesCommand(&m_document, { 5 }, 90));
    }

    QCOMPARE(m_document.pageAt(5).rotation, 0);
    // A command that changes nothing must not linger on the stack.
    QCOMPARE(m_document.undoStack()->count(), 0);
}

void TestPageCommands::duplicateIsUndoable()
{
    const auto before = fingerprint(m_document);

    m_document.undoStack()->push(new DuplicatePagesCommand(&m_document, { 1, 2 }));
    QCOMPARE(m_document.pageCount(), 12);
    // Copies land right after the selection.
    QCOMPARE(m_document.pageAt(3).sourcePage, 1);
    QCOMPARE(m_document.pageAt(4).sourcePage, 2);

    m_document.undoStack()->undo();
    QCOMPARE(fingerprint(m_document), before);
}

// ── Stack behaviour ───────────────────────────────────────────────────────

void TestPageCommands::redoRestoresTheEdit()
{
    m_document.undoStack()->push(new RemovePagesCommand(&m_document, { 3, 4 }));
    const auto afterEdit = fingerprint(m_document);

    m_document.undoStack()->undo();
    QCOMPARE(m_document.pageCount(), 10);

    m_document.undoStack()->redo();
    QCOMPARE(fingerprint(m_document), afterEdit);
}

void TestPageCommands::editingMarksDocumentModified()
{
    QVERIFY(!m_document.isModified());

    m_document.undoStack()->push(new RotatePagesCommand(&m_document, { 0 }, 90));
    QVERIFY(m_document.isModified());

    // Undoing back past the save point clears the flag again.
    m_document.undoStack()->undo();
    QVERIFY(!m_document.isModified());
}

void TestPageCommands::longSequenceUndoesCompletely()
{
    const auto before = fingerprint(m_document);

    m_document.undoStack()->push(new RotatePagesCommand(&m_document, { 0, 1 }, 90));
    m_document.undoStack()->push(new RemovePagesCommand(&m_document, { 4, 6 }));
    m_document.undoStack()->push(new MovePagesCommand(&m_document, { 0, 1 }, 5));
    m_document.undoStack()->push(new DuplicatePagesCommand(&m_document, { 2 }));
    m_document.undoStack()->push(new RemovePagesCommand(&m_document, { 0 }));

    QVERIFY(fingerprint(m_document) != before);

    while (m_document.undoStack()->canUndo()) {
        m_document.undoStack()->undo();
    }

    QCOMPARE(fingerprint(m_document), before);
    QVERIFY(!m_document.isModified());
}

QTEST_GUILESS_MAIN(TestPageCommands)

#include "tst_pagecommands.moc"
