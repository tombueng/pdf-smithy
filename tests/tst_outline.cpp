/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#include "TestPdf.h"

#include "core/Convert.h"
#include "core/Document.h"
#include "core/DocumentWriter.h"
#include "core/Outline.h"
#include "core/commands/PageCommands.h"
#include "render/PopplerBackend.h"

#include <QColor>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>
#include <QUndoStack>

#include <qpdf/QPDF.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>
#include <qpdf/QPDFWriter.hh>

#include <string>
#include <vector>

using namespace ps;

/**
 * The table of contents, which every save used to throw away.
 *
 * A manual whose navigation disappears the first time somebody rotates a page
 * is not a document editor, so the cases that matter here are not "can it write
 * an outline" but "does the outline still point at the right pages after the
 * pages have been moved about".
 */
class TestOutline : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();

    void readsWhatIsThere();
    void survivesASave();
    void followsPagesThatMove();
    void dropsEntriesWhosePageIsDeleted();
    void keepsNestingAndClosedState();
    void keepsBoldItalicAndColour();
    void survivesRearrangingTheTree();
    void mergingAppendsBothTablesOfContents();
    void isUndoable();
    void refusesNothingGracefully();
    void linksFollowTheirPage();
    void dropsLinksWhoseTargetIsDeleted();

    void buildsATreeFromTheHeadingsADocumentAppearsToHave();
    void aDocumentWithNoHeadingsGetsNoTreeRatherThanAnEmptyOne();
    void nestsBySkippedLevelsWithoutInventingEntries();
    void leavesOutHeadingsWithNothingToSayOrNowhereToGo();

private:
    /** A document with three top-level bookmarks on pages 1, 3 and 5. */
    QString writeOutlined(const QString &name);

    /**
     * Two pages whose only structure is how big their text is set.
     *
     * Eleven points of body with three ranks above it, which is exactly what
     * the heading inference has to work from and the only kind of document a
     * table of contents can be built out of at all.
     */
    static bool writeHeadingsPdf(const QString &path);

    QTemporaryDir m_dir;
};

void TestOutline::initTestCase()
{
    QVERIFY(m_dir.isValid());
}

QString TestOutline::writeOutlined(const QString &name)
{
    const QString path = m_dir.filePath(name);
    if (!test::writeSamplePdf(path, 6)) {
        return {};
    }

    QVector<OutlineItem> items;
    for (const auto &pair :
         { std::pair<const char *, int> { "Einleitung", 0 }, std::pair<const char *, int> { "Hauptteil", 2 },
           std::pair<const char *, int> { "Schluss", 4 } }) {
        OutlineItem item;
        item.title = QString::fromUtf8(pair.first);
        item.page = pair.second;
        items.append(item);
    }

    const QString out = m_dir.filePath(QStringLiteral("outlined-") + name);
    QString error;
    return Outline::write(path, out, items, &error) ? out : QString();
}

bool TestOutline::writeHeadingsPdf(const QString &path)
{
    struct Set {
        int size;
        int baseline;
        QString text;
    };
    const auto body = [](int baseline, int number) {
        return Set { 11, baseline,
                     QStringLiteral("Zeile %1 des Textkoerpers mit genug Buchstaben fuer die Messung").arg(number) };
    };

    const std::vector<std::vector<Set>> pages {
        { { 24, 720, QStringLiteral("Einleitung") },
          { 16, 676, QStringLiteral("Was hier steht") },
          body(650, 1),
          body(636, 2),
          body(622, 3),
          body(608, 4),
          { 16, 560, QStringLiteral("Aufbau") },
          body(534, 5),
          body(520, 6),
          body(506, 7) },
        { { 24, 720, QStringLiteral("Hauptteil") },
          { 16, 676, QStringLiteral("Vorgehen") },
          body(650, 8),
          body(636, 9),
          body(622, 10),
          body(608, 11),
          { 14, 560, QStringLiteral("Einzelheiten") },
          body(534, 12),
          body(520, 13),
          body(506, 14) },
    };

    try {
        QPDF pdf;
        pdf.emptyPDF();
        QPDFPageDocumentHelper helper(pdf);

        for (const std::vector<Set> &page : pages) {
            std::string content = "BT\n";
            for (const Set &set : page) {
                content += "/F1 " + std::to_string(set.size) + " Tf\n";
                content += "1 0 0 1 72 " + std::to_string(set.baseline) + " Tm (" + set.text.toStdString() + ") Tj\n";
            }
            content += "ET\n";

            QPDFObjectHandle font = pdf.makeIndirectObject(
                QPDFObjectHandle::parse("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>"));
            QPDFObjectHandle fonts = QPDFObjectHandle::newDictionary();
            fonts.replaceKey("/F1", font);
            QPDFObjectHandle resources = QPDFObjectHandle::newDictionary();
            resources.replaceKey("/Font", fonts);

            QPDFObjectHandle object = QPDFObjectHandle::newDictionary();
            object.replaceKey("/Type", QPDFObjectHandle::newName("/Page"));
            object.replaceKey("/MediaBox", QPDFObjectHandle::parse("[0 0 612 792]"));
            object.replaceKey("/Contents", QPDFObjectHandle::newStream(&pdf, content));
            object.replaceKey("/Resources", resources);
            helper.addPage(QPDFPageObjectHelper(pdf.makeIndirectObject(object)), false);
        }

        QPDFWriter writer(pdf);
        writer.setOutputFilename(QFile::encodeName(path).constData());
        writer.setDeterministicID(true);
        writer.write();
    } catch (const std::exception &) {
        return false;
    }
    return true;
}

void TestOutline::readsWhatIsThere()
{
    const QString path = writeOutlined(QStringLiteral("read.pdf"));
    QVERIFY(!path.isEmpty());

    QString error;
    const QVector<OutlineItem> items = Outline::read(path, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(items.size(), 3);
    QCOMPARE(items.constFirst().title, QStringLiteral("Einleitung"));
    QCOMPARE(items.constFirst().page, 0);
    QCOMPARE(items.at(1).page, 2);
    QCOMPARE(items.at(2).title, QStringLiteral("Schluss"));
    QCOMPARE(Outline::count(items), 3);
}

void TestOutline::survivesASave()
{
    const QString path = writeOutlined(QStringLiteral("save.pdf"));
    QVERIFY(!path.isEmpty());

    PopplerBackend backend;
    Document document;
    document.setRenderBackend(&backend);
    QVERIFY(document.open(path, nullptr));
    QVERIFY2(document.hasOutline(), "the table of contents was not read on open");
    QCOMPARE(document.outline().size(), 3);

    const QString out = m_dir.filePath(QStringLiteral("saved.pdf"));
    QString error;
    QVERIFY2(DocumentWriter::write(document, out, {}, &error), qPrintable(error));

    // The case that used to fail: saving is not an edit, and must not cost the
    // document its navigation.
    const QVector<OutlineItem> after = Outline::read(out, &error);
    QCOMPARE(after.size(), 3);
    QCOMPARE(after.constFirst().page, 0);
    QCOMPARE(after.at(2).page, 4);
}

void TestOutline::followsPagesThatMove()
{
    const QString path = writeOutlined(QStringLiteral("move.pdf"));
    QVERIFY(!path.isEmpty());

    PopplerBackend backend;
    Document document;
    document.setRenderBackend(&backend);
    QVERIFY(document.open(path, nullptr));

    // Reverse the document. Bookmarks are anchored to their page, not to its
    // number, so "Schluss" must now be near the front.
    QVector<int> reversed;
    for (int i = document.pageCount() - 1; i >= 0; --i) {
        reversed.append(i);
    }

    const QString out = m_dir.filePath(QStringLiteral("moved.pdf"));
    QString error;
    QVERIFY2(DocumentWriter::writeSelection(document, reversed, out, {}, &error), qPrintable(error));

    const QVector<OutlineItem> after = Outline::read(out, &error);
    QCOMPARE(after.size(), 3);

    // Page 4 of six, reversed, becomes page 1.
    const auto pageOf = [&after](const QString &title) {
        for (const OutlineItem &item : after) {
            if (item.title == title) {
                return item.page;
            }
        }
        return -99;
    };
    QCOMPARE(pageOf(QStringLiteral("Schluss")), 1);
    QCOMPARE(pageOf(QStringLiteral("Hauptteil")), 3);
    QCOMPARE(pageOf(QStringLiteral("Einleitung")), 5);
}

void TestOutline::dropsEntriesWhosePageIsDeleted()
{
    const QString path = writeOutlined(QStringLiteral("delete.pdf"));
    QVERIFY(!path.isEmpty());

    PopplerBackend backend;
    Document document;
    document.setRenderBackend(&backend);
    QVERIFY(document.open(path, nullptr));

    // Keep only the first two pages: "Hauptteil" and "Schluss" have nowhere to
    // point, and a bookmark that goes nowhere is worse than one that is gone.
    const QString out = m_dir.filePath(QStringLiteral("deleted.pdf"));
    QString error;
    QVERIFY2(DocumentWriter::writeSelection(document, { 0, 1 }, out, {}, &error), qPrintable(error));

    const QVector<OutlineItem> after = Outline::read(out, &error);
    QCOMPARE(after.size(), 1);
    QCOMPARE(after.constFirst().title, QStringLiteral("Einleitung"));
}

void TestOutline::keepsNestingAndClosedState()
{
    const QString path = m_dir.filePath(QStringLiteral("nest.pdf"));
    QVERIFY(test::writeSamplePdf(path, 6));

    OutlineItem child;
    child.title = QStringLiteral("Unterkapitel");
    child.page = 3;

    OutlineItem parent;
    parent.title = QStringLiteral("Hauptteil");
    parent.page = 2;
    parent.open = false; // closed when the document opens
    parent.children = { child };

    OutlineItem first;
    first.title = QStringLiteral("Einleitung");
    first.page = 0;

    const QString out = m_dir.filePath(QStringLiteral("nested.pdf"));
    QString error;
    QVERIFY2(Outline::write(path, out, { first, parent }, &error), qPrintable(error));

    const QVector<OutlineItem> after = Outline::read(out, &error);
    QCOMPARE(after.size(), 2);
    QCOMPARE(after.at(1).children.size(), 1);
    QCOMPARE(after.at(1).children.constFirst().title, QStringLiteral("Unterkapitel"));
    QCOMPARE(after.at(1).children.constFirst().page, 3);
    QVERIFY2(!after.at(1).open, "a closed entry came back open");
    QCOMPARE(Outline::count(after), 3);

    // Flattening walks it in the order a sidebar shows.
    const auto flat = Outline::flatten(after);
    QCOMPARE(flat.size(), 3);
    QCOMPARE(flat.at(0).second, 0);
    QCOMPARE(flat.at(1).second, 0);
    QCOMPARE(flat.at(2).second, 1);
    QCOMPARE(flat.at(2).first->title, QStringLiteral("Unterkapitel"));
}

void TestOutline::keepsBoldItalicAndColour()
{
    const QString path = m_dir.filePath(QStringLiteral("styled.pdf"));
    QVERIFY(test::writeSamplePdf(path, 3));

    OutlineItem part;
    part.title = QStringLiteral("Teil I");
    part.page = 0;
    part.bold = true;
    part.colour = QColor::fromRgbF(0.2f, 0.4f, 0.6f);

    OutlineItem note;
    note.title = QStringLiteral("Anhang");
    note.page = 2;
    note.italic = true;

    const QString out = m_dir.filePath(QStringLiteral("styled-out.pdf"));
    QString error;
    QVERIFY2(Outline::write(path, out, { part, note }, &error), qPrintable(error));

    const QVector<OutlineItem> after = Outline::read(out, &error);
    QCOMPARE(after.size(), 2);
    QVERIFY2(after.at(0).bold, "a bold entry came back plain");
    QVERIFY(!after.at(0).italic);
    QVERIFY2(after.at(1).italic, "an italic entry came back plain");
    QVERIFY(!after.at(1).bold);

    // The colour is where the locale bites: written through snprintf or read
    // through strtod, [0.2 0.4 0.6] becomes black on a German desktop.
    QVERIFY2(after.at(0).colour.isValid(), "a coloured entry came back without its colour");
    QVERIFY(qAbs(after.at(0).colour.redF() - 0.2f) < 0.01f);
    QVERIFY(qAbs(after.at(0).colour.greenF() - 0.4f) < 0.01f);
    QVERIFY(qAbs(after.at(0).colour.blueF() - 0.6f) < 0.01f);
    QVERIFY2(!after.at(1).colour.isValid(), "an entry with no colour of its own came back with one");
}

void TestOutline::survivesRearrangingTheTree()
{
    const QString path = writeOutlined(QStringLiteral("rearrange.pdf"));
    QVERIFY(!path.isEmpty());

    PopplerBackend backend;
    Document document;
    document.setRenderBackend(&backend);
    QVERIFY(document.open(path, nullptr));

    // What the sidebar does when an entry is dragged onto the one above it: the
    // whole tree is handed back rearranged, in one step.
    QVector<OutlineItem> edited = document.outline();
    QCOMPARE(edited.size(), 3);
    const OutlineItem moved = edited.at(1);
    edited.removeAt(1);
    edited[0].children.append(moved);
    edited[0].open = false;
    document.undoStack()->push(new SetOutlineCommand(&document, edited, QStringLiteral("nest")));

    QCOMPARE(document.outline().size(), 2);
    QCOMPARE(document.outline().constFirst().children.size(), 1);

    const QString out = m_dir.filePath(QStringLiteral("rearranged.pdf"));
    QString error;
    QVERIFY2(DocumentWriter::write(document, out, {}, &error), qPrintable(error));

    const QVector<OutlineItem> after = Outline::read(out, &error);
    QCOMPARE(after.size(), 2);
    QCOMPARE(after.constFirst().children.size(), 1);
    QCOMPARE(after.constFirst().children.constFirst().title, QStringLiteral("Hauptteil"));
    QCOMPARE(after.constFirst().children.constFirst().page, 2);
    QVERIFY2(!after.constFirst().open, "the entry was nested under a closed one and came back open");
    QCOMPARE(Outline::count(after), 3);

    // And the whole rearrangement is one Ctrl+Z, like every other edit.
    document.undoStack()->undo();
    QCOMPARE(document.outline().size(), 3);
    QVERIFY(document.outline().constFirst().children.isEmpty());
}

void TestOutline::mergingAppendsBothTablesOfContents()
{
    const QString first = writeOutlined(QStringLiteral("merge-a.pdf"));
    const QString second = writeOutlined(QStringLiteral("merge-b.pdf"));
    QVERIFY(!first.isEmpty() && !second.isEmpty());

    PopplerBackend backend;
    Document document;
    document.setRenderBackend(&backend);
    QVERIFY(document.importFile(first, -1, nullptr));
    QVERIFY(document.importFile(second, -1, nullptr));
    QCOMPARE(document.pageCount(), 12);

    // Six entries, and the second file's point into its own pages rather than
    // back at the first file's.
    const QVector<OutlineItem> items = document.outline();
    QCOMPARE(items.size(), 6);
    QCOMPARE(items.at(0).page, 0);
    QCOMPARE(items.at(3).page, 6);
    QCOMPARE(items.at(5).page, 10);

    const QString out = m_dir.filePath(QStringLiteral("merged.pdf"));
    QString error;
    QVERIFY2(DocumentWriter::write(document, out, {}, &error), qPrintable(error));
    QCOMPARE(Outline::read(out, &error).size(), 6);
}

void TestOutline::isUndoable()
{
    const QString path = writeOutlined(QStringLiteral("undo.pdf"));
    QVERIFY(!path.isEmpty());

    PopplerBackend backend;
    Document document;
    document.setRenderBackend(&backend);
    QVERIFY(document.open(path, nullptr));

    QVector<OutlineItem> edited = document.outline();
    edited.removeAt(1);
    edited[0].title = QStringLiteral("Vorwort");

    // On the same stack as the page operations: one Ctrl+Z, one meaning.
    document.undoStack()->push(new SetOutlineCommand(&document, edited, QStringLiteral("edit")));
    QCOMPARE(document.outline().size(), 2);
    QCOMPARE(document.outline().constFirst().title, QStringLiteral("Vorwort"));

    document.undoStack()->undo();
    QCOMPARE(document.outline().size(), 3);
    QCOMPARE(document.outline().constFirst().title, QStringLiteral("Einleitung"));

    document.undoStack()->redo();
    QCOMPARE(document.outline().size(), 2);
}

void TestOutline::refusesNothingGracefully()
{
    const QString plain = m_dir.filePath(QStringLiteral("plain.pdf"));
    QVERIFY(test::writeSamplePdf(plain, 2));

    QString error;
    QVERIFY(Outline::read(plain, &error).isEmpty());
    QVERIFY(error.isEmpty()); // No outline is not a failure.

    // Writing an empty one removes what was there, rather than leaving a stub.
    const QString outlined = writeOutlined(QStringLiteral("clear.pdf"));
    const QString cleared = m_dir.filePath(QStringLiteral("cleared.pdf"));
    QVERIFY2(Outline::write(outlined, cleared, {}, &error), qPrintable(error));
    QVERIFY(Outline::read(cleared, &error).isEmpty());

    // An entry pointing past the end of the document is dropped, not clamped.
    OutlineItem stray;
    stray.title = QStringLiteral("Nirgendwo");
    stray.page = 99;
    const QString strayOut = m_dir.filePath(QStringLiteral("stray.pdf"));
    QVERIFY2(Outline::write(plain, strayOut, { stray }, &error), qPrintable(error));
    QVERIFY(Outline::read(strayOut, &error).isEmpty());
}

void TestOutline::linksFollowTheirPage()
{
    const QString path = m_dir.filePath(QStringLiteral("link.pdf"));
    QVERIFY(test::writeLinkedPdf(path, 4, 3));

    int target = -1;
    QCOMPARE(test::countLinks(path, &target), 1);
    QCOMPARE(target, 3);

    PopplerBackend backend;
    Document document;
    document.setRenderBackend(&backend);
    QVERIFY(document.open(path, nullptr));

    const QString out = m_dir.filePath(QStringLiteral("link-reversed.pdf"));
    QString error;
    QVERIFY2(DocumentWriter::writeSelection(document, { 3, 2, 1, 0 }, out, {}, &error), qPrintable(error));

    // The link was on page 1 pointing at page 4. Reversed, it sits on page 4
    // and must point at page 1: the page, not the number.
    QCOMPARE(test::countLinks(out, &target), 1);
    QCOMPARE(target, 0);
}

void TestOutline::dropsLinksWhoseTargetIsDeleted()
{
    const QString path = m_dir.filePath(QStringLiteral("link-cut.pdf"));
    QVERIFY(test::writeLinkedPdf(path, 4, 3));

    PopplerBackend backend;
    Document document;
    document.setRenderBackend(&backend);
    QVERIFY(document.open(path, nullptr));

    // Keep the first two pages. The link's destination is gone, so the link is
    // gone: a rectangle that does nothing when clicked is not worth keeping,
    // and the reference is also the last thing holding the deleted page.
    const QString out = m_dir.filePath(QStringLiteral("link-dead.pdf"));
    QString error;
    QVERIFY2(DocumentWriter::writeSelection(document, { 0, 1 }, out, {}, &error), qPrintable(error));

    int target = -1;
    QCOMPARE(test::countLinks(out, &target), 0);
    QCOMPARE(test::pageCountOf(out), 2);
}

// ── Building one where there was none ─────────────────────────────────────

void TestOutline::buildsATreeFromTheHeadingsADocumentAppearsToHave()
{
    const QString path = m_dir.filePath(QStringLiteral("headings.pdf"));
    QVERIFY(writeHeadingsPdf(path));

    QString error;
    const QVector<Convert::Heading> headings = Convert::findHeadings(path, {}, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(headings.size(), 6);

    const QVector<OutlineItem> built = Outline::fromHeadings(headings);

    // Two chapters, each with what was set smaller beneath it, and the one
    // third-rank heading beneath the second-rank one it followed.
    QCOMPARE(built.size(), 2);
    QCOMPARE(Outline::count(built), 6);

    QCOMPARE(built.at(0).title, QStringLiteral("Einleitung"));
    QCOMPARE(built.at(0).page, 0);
    QCOMPARE(built.at(0).children.size(), 2);
    QCOMPARE(built.at(0).children.at(0).title, QStringLiteral("Was hier steht"));
    QCOMPARE(built.at(0).children.at(1).title, QStringLiteral("Aufbau"));
    QVERIFY(built.at(0).children.at(1).children.isEmpty());

    QCOMPARE(built.at(1).title, QStringLiteral("Hauptteil"));
    QCOMPARE(built.at(1).page, 1);
    QCOMPARE(built.at(1).children.size(), 1);
    QCOMPARE(built.at(1).children.at(0).title, QStringLiteral("Vorgehen"));
    QCOMPARE(built.at(1).children.at(0).children.size(), 1);
    QCOMPARE(built.at(1).children.at(0).children.at(0).title, QStringLiteral("Einzelheiten"));
    QCOMPARE(built.at(1).children.at(0).children.at(0).page, 1);

    // Depth-first, the way the sidebar shows it and the way a reader's panel
    // will show it once the file has been through the writer.
    const auto flat = Outline::flatten(built);
    QCOMPARE(flat.size(), 6);
    QCOMPARE(flat.at(5).second, 2);

    const QString out = m_dir.filePath(QStringLiteral("built.pdf"));
    QVERIFY2(Outline::write(path, out, built, &error), qPrintable(error));
    const QVector<OutlineItem> after = Outline::read(out, &error);
    QCOMPARE(Outline::count(after), 6);
    QCOMPARE(after.size(), 2);
    QCOMPARE(after.at(1).children.at(0).children.at(0).title, QStringLiteral("Einzelheiten"));
}

void TestOutline::aDocumentWithNoHeadingsGetsNoTreeRatherThanAnEmptyOne()
{
    const QString path = m_dir.filePath(QStringLiteral("flat.pdf"));
    QVERIFY(test::writeTextHeavyPdf(path, 2));

    QString error;
    const QVector<Convert::Heading> headings = Convert::findHeadings(path, {}, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(headings.isEmpty());

    // Nothing at all, not one entry per page and not a root called after the
    // file. A document set in one size says nothing about its own structure,
    // and the panel has to be able to tell the user so.
    QVERIFY(Outline::fromHeadings(headings).isEmpty());
}

void TestOutline::nestsBySkippedLevelsWithoutInventingEntries()
{
    // Hand-made rather than inferred, because what is being tested is the
    // shaping and not the guessing: levels that skip a rank, a level that comes
    // back out again, and a document that starts halfway down.
    const auto heading = [](int level, int page, const char *title) {
        Convert::Heading item;
        item.level = level;
        item.page = page;
        item.text = QString::fromUtf8(title);
        return item;
    };

    const QVector<Convert::Heading> skipping {
        heading(1, 0, "Teil"),
        heading(3, 1, "Ganz tief"),
        heading(2, 2, "Wieder hoeher"),
        heading(1, 3, "Teil zwei"),
    };
    const QVector<OutlineItem> tree = Outline::fromHeadings(skipping);

    // Four headings in, four bookmarks out: the missing second level is not
    // filled with a placeholder that would go nowhere when clicked.
    QCOMPARE(Outline::count(tree), 4);
    QCOMPARE(tree.size(), 2);
    QCOMPARE(tree.at(0).children.size(), 2);
    QCOMPARE(tree.at(0).children.at(0).title, QStringLiteral("Ganz tief"));
    QCOMPARE(tree.at(0).children.at(0).page, 1);
    QCOMPARE(tree.at(0).children.at(1).title, QStringLiteral("Wieder hoeher"));
    QCOMPARE(tree.at(1).title, QStringLiteral("Teil zwei"));
    QVERIFY(tree.at(1).children.isEmpty());

    // A document whose first heading is not its shallowest: the deeper one
    // stands on its own rather than waiting for a parent that never came.
    const QVector<Convert::Heading> upsideDown { heading(3, 0, "Vorbemerkung"), heading(1, 1, "Erstes Kapitel"),
                                                 heading(2, 2, "Abschnitt") };
    const QVector<OutlineItem> second = Outline::fromHeadings(upsideDown);
    QCOMPARE(second.size(), 2);
    QCOMPARE(second.at(0).title, QStringLiteral("Vorbemerkung"));
    QVERIFY(second.at(0).children.isEmpty());
    QCOMPARE(second.at(1).children.size(), 1);
    QCOMPARE(second.at(1).children.at(0).title, QStringLiteral("Abschnitt"));

    // Every entry opens, because a tree that arrives closed looks like no tree.
    for (const auto &pair : Outline::flatten(second)) {
        QVERIFY(pair.first->open);
    }
}

void TestOutline::leavesOutHeadingsWithNothingToSayOrNowhereToGo()
{
    Convert::Heading real;
    real.level = 1;
    real.page = 0;
    real.text = QStringLiteral("  Kapitel   eins  ");

    Convert::Heading blank;
    blank.level = 2;
    blank.page = 1;
    blank.text = QStringLiteral("   ");

    Convert::Heading homeless;
    homeless.level = 2;
    homeless.page = -1;
    homeless.text = QStringLiteral("Nirgendwo");

    const QVector<OutlineItem> tree = Outline::fromHeadings({ real, blank, homeless });
    QCOMPARE(Outline::count(tree), 1);
    QCOMPARE(tree.size(), 1);
    // Collapsed, because a bookmark title is shown on one line and the line
    // breaks of the page it came from mean nothing there.
    QCOMPARE(tree.constFirst().title, QStringLiteral("Kapitel eins"));
    QVERIFY(tree.constFirst().children.isEmpty());
}

QTEST_MAIN(TestOutline)

#include "tst_outline.moc"
