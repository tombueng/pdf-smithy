/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#include "TestPdf.h"

#include "core/Annotation.h"
#include "render/PopplerBackend.h"

#include <QFile>
#include <QTemporaryDir>
#include <QTest>

using namespace ps;

/**
 * Annotations, checked by looking at the page rather than at the object tree.
 *
 * A comment that is present in the file but invisible in a reader is not a
 * comment, and the way to find that out is to render the page and look. Every
 * case here that can be checked in pixels is.
 */
class TestAnnotation : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();

    void highlightIsVisibleAndKeepsTextReadable();
    void inkIsDrawnWhereItWasPut();
    void readsBackWhatItWrote();
    void landsInTheRightPlaceOnATurnedPage();
    void leavesThePageContentAlone();
    void removesCommentsButNotFormFields();
    void flattenDrawsThemInAndTakesThemOff();
    void refusesAnEmptyList();
    void travelsThroughXfdfAndBack();
    void xfdfSkipsCommentsForPagesThatAreNotThere();
    void refusesRubbishXfdf();

private:
    /** A white page with a black word at a known place. */
    QString writePage(const QString &name, int rotate = 0);

    QTemporaryDir m_dir;
};

void TestAnnotation::initTestCase()
{
    QVERIFY(m_dir.isValid());
}

QString TestAnnotation::writePage(const QString &name, int rotate)
{
    // QVERIFY cannot be used here: it returns void, and this returns a path.
    const QString path = m_dir.filePath(name);
    return test::writeRotatedPdf(path, 2, rotate) ? path : QString();
}

void TestAnnotation::highlightIsVisibleAndKeepsTextReadable()
{
    const QString source = writePage(QStringLiteral("highlight-source.pdf"));
    const QString out = m_dir.filePath(QStringLiteral("highlight.pdf"));

    Annotation annotation;
    annotation.type = Annotation::Type::Highlight;
    annotation.page = 0;
    annotation.quads = { QRectF(60, 690, 200, 22) };
    annotation.colour = QColor(255, 235, 0);
    annotation.author = QStringLiteral("Tom");
    annotation.contents = QStringLiteral("Wichtig");

    QString error;
    QVERIFY2(Annotations::add(source, out, { annotation }, &error), qPrintable(error));

    PopplerBackend backend;
    QVERIFY(backend.addDocument(1, out, nullptr));
    const QImage page = backend.renderPage(1, 0, 612);
    QVERIFY(!page.isNull());

    // Yellow where it was put…
    const QColor inside = page.pixelColor(150, 792 - 700);
    QVERIFY2(inside.red() > 200 && inside.green() > 180 && inside.blue() < 120,
             qPrintable(QStringLiteral("expected yellow, got %1").arg(inside.name())));

    // …and white just outside it, so it did not spread.
    const QColor outside = page.pixelColor(150, 792 - 740);
    QVERIFY2(outside.blue() > 200, qPrintable(QStringLiteral("expected white, got %1").arg(outside.name())));
}

void TestAnnotation::inkIsDrawnWhereItWasPut()
{
    const QString source = writePage(QStringLiteral("ink-source.pdf"));
    const QString out = m_dir.filePath(QStringLiteral("ink.pdf"));

    Annotation annotation;
    annotation.type = Annotation::Type::Ink;
    annotation.page = 0;
    annotation.strokes = { { QPointF(100, 400), QPointF(300, 400) } };
    annotation.colour = QColor(220, 0, 0);
    annotation.lineWidth = 6.0;

    QString error;
    QVERIFY2(Annotations::add(source, out, { annotation }, &error), qPrintable(error));

    PopplerBackend backend;
    QVERIFY(backend.addDocument(1, out, nullptr));
    const QImage page = backend.renderPage(1, 0, 612);

    const QColor onTheLine = page.pixelColor(200, 792 - 400);
    QVERIFY2(onTheLine.red() > 150 && onTheLine.green() < 100,
             qPrintable(QStringLiteral("expected red, got %1").arg(onTheLine.name())));
    QVERIFY(page.pixelColor(200, 792 - 450).red() > 200 && page.pixelColor(200, 792 - 450).green() > 200);
}

void TestAnnotation::readsBackWhatItWrote()
{
    const QString source = writePage(QStringLiteral("roundtrip-source.pdf"));
    const QString out = m_dir.filePath(QStringLiteral("roundtrip.pdf"));

    Annotation note;
    note.type = Annotation::Type::Note;
    note.page = 1;
    note.rect = QRectF(300, 500, 20, 20);
    note.author = QStringLiteral("Tom Bueng");
    note.contents = QStringLiteral("Bitte prüfen, Umlaute: äöüß");
    note.colour = QColor(0, 120, 255);

    Annotation marker;
    marker.type = Annotation::Type::Highlight;
    marker.page = 0;
    marker.quads = { QRectF(60, 690, 120, 20), QRectF(60, 660, 200, 20) };

    Annotation scribble;
    scribble.type = Annotation::Type::Ink;
    scribble.page = 1;
    scribble.strokes = { { QPointF(100, 100), QPointF(150, 140), QPointF(200, 100) } };

    Annotation box;
    box.type = Annotation::Type::Square;
    box.page = 0;
    box.rect = QRectF(80, 200, 240, 90);
    box.colour = QColor(0, 160, 0);
    box.interior = QColor(230, 255, 230);

    QString error;
    QVERIFY2(Annotations::add(source, out, { note, box, marker, scribble }, &error), qPrintable(error));

    const QVector<Annotation> back = Annotations::read(out, {}, &error);
    QCOMPARE(back.size(), 4);

    const Annotation *readNote = nullptr;
    const Annotation *readBox = nullptr;
    for (const Annotation &annotation : back) {
        if (annotation.type == Annotation::Type::Note) {
            readNote = &annotation;
        }
        if (annotation.type == Annotation::Type::Square) {
            readBox = &annotation;
        }
    }
    QVERIFY(readNote && readBox);

    QCOMPARE(readNote->page, 1);
    QCOMPARE(readNote->author, QStringLiteral("Tom Bueng"));
    // The comment goes in as UTF-16, so it has to come back with its umlauts.
    QCOMPARE(readNote->contents, QStringLiteral("Bitte prüfen, Umlaute: äöüß"));
    QVERIFY(!readNote->identifier.isEmpty());
    QVERIFY(readNote->created.isValid());

    QCOMPARE(readBox->page, 0);
    QVERIFY(qAbs(readBox->rect.x() - 80.0) < 1.0);
    QVERIFY(qAbs(readBox->rect.y() - 200.0) < 1.0);
    QVERIFY(qAbs(readBox->rect.width() - 240.0) < 1.0);
    QVERIFY(readBox->interior.isValid());
    QCOMPARE(readBox->interior.name(), QStringLiteral("#e6ffe6"));
    QVERIFY2(readBox->interior.green() > 200,
             qPrintable(QStringLiteral("interior came back as %1").arg(readBox->interior.name())));

    // Quad points and ink paths are arrays of their own length, and reading
    // them with a helper that insists on exactly four entries gives silent
    // zeroes. That happened; this is what would have caught it.
    const Annotation *readMarker = nullptr;
    const Annotation *readScribble = nullptr;
    for (const Annotation &annotation : back) {
        if (annotation.type == Annotation::Type::Highlight) {
            readMarker = &annotation;
        }
        if (annotation.type == Annotation::Type::Ink) {
            readScribble = &annotation;
        }
    }
    QVERIFY(readMarker && readScribble);

    QCOMPARE(readMarker->quads.size(), 2);
    QVERIFY2(qAbs(readMarker->quads.constFirst().x() - 60.0) < 1.0,
             qPrintable(QStringLiteral("first quad starts at %1").arg(readMarker->quads.constFirst().x())));
    QVERIFY(qAbs(readMarker->quads.constFirst().width() - 120.0) < 1.0);
    QVERIFY(qAbs(readMarker->quads.at(1).width() - 200.0) < 1.0);

    QCOMPARE(readScribble->strokes.size(), 1);
    QCOMPARE(readScribble->strokes.constFirst().size(), 3);
    QVERIFY2(qAbs(readScribble->strokes.constFirst().at(1).y() - 140.0) < 1.0,
             qPrintable(
                 QStringLiteral("stroke midpoint came back at %1").arg(readScribble->strokes.constFirst().at(1).y())));
}

void TestAnnotation::landsInTheRightPlaceOnATurnedPage()
{
    // The appearance is drawn in display coordinates and carried into page
    // space by /Matrix. If that were wrong, the mark would be somewhere else
    // entirely on a rotated page, and rotated pages are the normal state of a
    // scan.
    const QString source = writePage(QStringLiteral("turned-source.pdf"), 90);
    const QString out = m_dir.filePath(QStringLiteral("turned.pdf"));

    Annotation annotation;
    annotation.type = Annotation::Type::Square;
    annotation.page = 0;
    annotation.rect = QRectF(100, 100, 120, 60);
    annotation.colour = QColor(255, 0, 0);
    annotation.interior = QColor(255, 0, 0);
    annotation.lineWidth = 1.0;

    QString error;
    QVERIFY2(Annotations::add(source, out, { annotation }, &error), qPrintable(error));

    PopplerBackend backend;
    QVERIFY(backend.addDocument(1, out, nullptr));
    const QSizeF shown = backend.pageSizePoints(1, 0);
    const QImage page = backend.renderPage(1, 0, qRound(shown.width()));
    QVERIFY(!page.isNull());

    const double scale = page.width() / shown.width();
    const QPoint centre(qRound(160 * scale), qRound((shown.height() - 130) * scale));
    const QColor middle = page.pixelColor(centre);
    QVERIFY2(
        middle.red() > 200 && middle.green() < 80,
        qPrintable(
            QStringLiteral("expected red at %1,%2 but got %3").arg(centre.x()).arg(centre.y()).arg(middle.name())));

    // And reading it back gives the coordinates that went in.
    const QVector<Annotation> back = Annotations::read(out, {}, &error);
    QCOMPARE(back.size(), 1);
    QVERIFY2(qAbs(back.constFirst().rect.x() - 100.0) < 1.5,
             qPrintable(QStringLiteral("x came back as %1").arg(back.constFirst().rect.x())));
    QVERIFY2(qAbs(back.constFirst().rect.y() - 100.0) < 1.5,
             qPrintable(QStringLiteral("y came back as %1").arg(back.constFirst().rect.y())));
}

void TestAnnotation::leavesThePageContentAlone()
{
    const QString source = writePage(QStringLiteral("untouched-source.pdf"));
    const QString out = m_dir.filePath(QStringLiteral("untouched.pdf"));

    Annotation annotation;
    annotation.type = Annotation::Type::Highlight;
    annotation.page = 0;
    annotation.quads = { QRectF(60, 690, 200, 22) };

    QString error;
    QVERIFY2(Annotations::add(source, out, { annotation }, &error), qPrintable(error));

    // The point of annotations: the page itself is not rewritten.
    QCOMPARE(test::contentOf(out, 0), test::contentOf(source, 0));
}

void TestAnnotation::removesCommentsButNotFormFields()
{
    const QString source = writePage(QStringLiteral("mixed-source.pdf"));
    const QString withComment = m_dir.filePath(QStringLiteral("mixed.pdf"));

    Annotation annotation;
    annotation.type = Annotation::Type::Highlight;
    annotation.page = 0;
    annotation.quads = { QRectF(60, 690, 200, 22) };

    QString error;
    QVERIFY2(Annotations::add(source, withComment, { annotation }, &error), qPrintable(error));

    // A form field sitting on the same page. Clearing comments must not take it.
    const QString withField = m_dir.filePath(QStringLiteral("mixed-field.pdf"));
    QVERIFY(test::addWidgetAnnotation(withComment, withField, 0));

    const QString cleared = m_dir.filePath(QStringLiteral("cleared.pdf"));
    int removed = 0;
    QVERIFY2(Annotations::remove(withField, cleared, {}, {}, &removed, &error), qPrintable(error));
    QCOMPARE(removed, 1);

    QCOMPARE(Annotations::read(cleared, {}, &error).size(), 0);
    QVERIFY2(test::hasWidgetAnnotation(cleared, 0), "the form field was removed along with the comment");
}

void TestAnnotation::flattenDrawsThemInAndTakesThemOff()
{
    const QString source = writePage(QStringLiteral("flatten-source.pdf"));
    const QString marked = m_dir.filePath(QStringLiteral("flatten-marked.pdf"));

    Annotation annotation;
    annotation.type = Annotation::Type::Square;
    annotation.page = 0;
    annotation.rect = QRectF(100, 300, 200, 80);
    annotation.colour = QColor(255, 0, 0);
    annotation.interior = QColor(255, 0, 0);

    QString error;
    QVERIFY2(Annotations::add(source, marked, { annotation }, &error), qPrintable(error));

    const QString flat = m_dir.filePath(QStringLiteral("flat.pdf"));
    int flattened = 0;
    QVERIFY2(Annotations::flatten(marked, flat, {}, &flattened, &error), qPrintable(error));
    QCOMPARE(flattened, 1);

    // No annotation left…
    QCOMPARE(Annotations::read(flat, {}, &error).size(), 0);

    // …and it is still on the page, now as part of it.
    PopplerBackend backend;
    QVERIFY(backend.addDocument(1, flat, nullptr));
    const QImage page = backend.renderPage(1, 0, 612);
    const QColor middle = page.pixelColor(200, 792 - 340);
    QVERIFY2(middle.red() > 200 && middle.green() < 80,
             qPrintable(QStringLiteral("the flattened shape is gone: %1").arg(middle.name())));
}

void TestAnnotation::refusesAnEmptyList()
{
    QString error;
    QVERIFY(!Annotations::add(writePage(QStringLiteral("empty-source.pdf")),
                              m_dir.filePath(QStringLiteral("empty.pdf")), {}, &error));
    QVERIFY(!error.isEmpty());
}

void TestAnnotation::travelsThroughXfdfAndBack()
{
    const QString source = writePage(QStringLiteral("xfdf-source.pdf"));
    const QString marked = m_dir.filePath(QStringLiteral("xfdf-marked.pdf"));

    Annotation highlight;
    highlight.type = Annotation::Type::Highlight;
    highlight.page = 0;
    highlight.quads = { QRectF(60, 690, 200, 22), QRectF(60, 650, 140, 22) };
    highlight.colour = QColor(255, 200, 0);
    highlight.author = QStringLiteral("Kollegin");
    highlight.contents = QStringLiteral("Sieh dir das an: äöü");

    Annotation scribble;
    scribble.type = Annotation::Type::Ink;
    scribble.page = 1;
    scribble.strokes = { { QPointF(100, 300), QPointF(160, 340), QPointF(220, 300) } };
    scribble.colour = QColor(200, 0, 0);
    scribble.lineWidth = 4.0;

    QString error;
    QVERIFY2(Annotations::add(source, marked, { highlight, scribble }, &error), qPrintable(error));

    // Out to a file that holds the remarks and not the document.
    const QString xfdf = m_dir.filePath(QStringLiteral("comments.xfdf"));
    QVERIFY2(Annotations::exportXfdf(marked, xfdf, {}, QStringLiteral("xfdf-source.pdf"), &error), qPrintable(error));

    QFile written(xfdf);
    QVERIFY(written.open(QIODevice::ReadOnly));
    const QString text = QString::fromUtf8(written.readAll());
    QVERIFY2(text.contains(QStringLiteral("http://ns.adobe.com/xfdf/")), qPrintable(text.left(200)));
    QVERIFY(text.contains(QStringLiteral("<highlight")));
    QVERIFY(text.contains(QStringLiteral("<gesture>")));
    // The document itself must not be in there; that is the whole point.
    QVERIFY(!text.contains(QStringLiteral("%PDF")));

    // And back onto a clean copy of the same document.
    const QString reunited = m_dir.filePath(QStringLiteral("xfdf-back.pdf"));
    int added = 0;
    QStringList warnings;
    QVERIFY2(Annotations::importXfdf(source, xfdf, reunited, &added, &warnings, &error), qPrintable(error));
    QCOMPARE(added, 2);
    QVERIFY(warnings.isEmpty());

    const QVector<Annotation> back = Annotations::read(reunited, {}, &error);
    QCOMPARE(back.size(), 2);

    const Annotation *readHighlight = nullptr;
    const Annotation *readInk = nullptr;
    for (const Annotation &annotation : back) {
        if (annotation.type == Annotation::Type::Highlight) {
            readHighlight = &annotation;
        }
        if (annotation.type == Annotation::Type::Ink) {
            readInk = &annotation;
        }
    }
    QVERIFY(readHighlight && readInk);

    QCOMPARE(readHighlight->page, 0);
    QCOMPARE(readHighlight->author, QStringLiteral("Kollegin"));
    QCOMPARE(readHighlight->contents, QStringLiteral("Sieh dir das an: äöü"));
    QCOMPARE(readHighlight->quads.size(), 2);
    QVERIFY2(qAbs(readHighlight->quads.constFirst().x() - 60.0) < 1.0,
             qPrintable(QStringLiteral("quad came back at %1").arg(readHighlight->quads.constFirst().x())));
    QVERIFY(qAbs(readHighlight->quads.at(1).width() - 140.0) < 1.0);
    // Naming the exact colour rather than "reddish": checking two channels of
    // three let a wrong green through once already.
    QCOMPARE(readHighlight->colour.name(), QStringLiteral("#ffc800"));

    QCOMPARE(readInk->page, 1);
    QCOMPARE(readInk->strokes.size(), 1);
    QCOMPARE(readInk->strokes.constFirst().size(), 3);
    QVERIFY2(qAbs(readInk->strokes.constFirst().at(1).y() - 340.0) < 1.0,
             qPrintable(QStringLiteral("stroke came back at %1").arg(readInk->strokes.constFirst().at(1).y())));
}

void TestAnnotation::xfdfSkipsCommentsForPagesThatAreNotThere()
{
    // A comment on page 9 of a two-page document has no right answer. Putting
    // it on the last page would be a guess dressed as a result.
    const QString xfdf = m_dir.filePath(QStringLiteral("stray.xfdf"));
    QFile file(xfdf);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(R"(<?xml version="1.0" encoding="UTF-8"?>
<xfdf xmlns="http://ns.adobe.com/xfdf/" xml:space="preserve">
  <annots>
    <square page="0" rect="100,100,200,150" color="#FF0000" width="2"/>
    <square page="8" rect="100,100,200,150" color="#FF0000" width="2"/>
  </annots>
</xfdf>
)");
    file.close();

    const QString source = writePage(QStringLiteral("stray-source.pdf"));
    const QString out = m_dir.filePath(QStringLiteral("stray-out.pdf"));

    int added = 0;
    QStringList warnings;
    QString error;
    QVERIFY2(Annotations::importXfdf(source, xfdf, out, &added, &warnings, &error), qPrintable(error));
    QCOMPARE(added, 1);
    QCOMPARE(warnings.size(), 1);
    QVERIFY2(!warnings.constFirst().isEmpty(), "the skipped comment was not mentioned");

    QCOMPARE(Annotations::read(out, {}, &error).size(), 1);
}

void TestAnnotation::refusesRubbishXfdf()
{
    const QString broken = m_dir.filePath(QStringLiteral("broken.xfdf"));
    QFile file(broken);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("<xfdf><annots><square page=\"0\"");
    file.close();

    QString error;
    QVERIFY(!Annotations::importXfdf(writePage(QStringLiteral("rubbish-source.pdf")), broken,
                                     m_dir.filePath(QStringLiteral("rubbish-out.pdf")), nullptr, nullptr, &error));
    QVERIFY(!error.isEmpty());

    // And a file that is not there at all.
    QVERIFY(!Annotations::importXfdf(writePage(QStringLiteral("missing-source.pdf")),
                                     m_dir.filePath(QStringLiteral("nope.xfdf")),
                                     m_dir.filePath(QStringLiteral("missing-out.pdf")), nullptr, nullptr, &error));
    QVERIFY(!error.isEmpty());
}

QTEST_MAIN(TestAnnotation)

#include "tst_annotation.moc"
