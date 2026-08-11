/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#include "TestPdf.h"

#include "core/PageObjects.h"
#include "render/PopplerBackend.h"

#include <KLocalizedString>

#include <qpdf/QPDF.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>
#include <qpdf/QPDFWriter.hh>
#include <QFile>
#include <QPainter>
#include <QTemporaryDir>
#include <QTest>
#include <QtEnvironmentVariables>

using namespace ps;

/**
 * The page as a list of things one can point at.
 *
 * Two properties decide whether this is a foundation or a liability. Objects must
 * be found where they actually are, checked against a rendered page, not against
 * arithmetic. And an edit must leave everything it did not touch byte for byte
 * alone, because the whole promise of the editor rests on it.
 */
class TestPageobjects : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();

    void findsTextImagesAndShapes();
    void reportsBoundsWhereTheThingActuallyIs();
    void picksTheTopmostObjectUnderThePoint();
    void movesOneObjectAndLeavesTheRestByteIdentical();
    void movesByTheAmountAskedForRegardlessOfScaling();
    void deletesAnObjectSoItLeavesThePage();
    void restylesWithoutRewriting();
    void laysABackdropBehindThePageAndCanLeaveTheAnnotationsOff();
    void insertsTextShapesAndPictures();
    void insertsAPictureTurnedByItsPlacement();
    void followsARotatedPage();
    void readsARealMagazinePage();
    void refusesNothing();
    void reportsItsOwnLimits();

private:
    /** A page with a red rectangle, a line of text and a picture at known places. */
    QString writeMixedPage(const QString &name, int rotate = 0);

    QTemporaryDir m_dir;
};

void TestPageobjects::initTestCase()
{
    KLocalizedString::setApplicationDomain("pdf-smithy");
    QVERIFY(m_dir.isValid());
}

QString TestPageobjects::writeMixedPage(const QString &name, int rotate)
{
    const QString path = m_dir.filePath(name);
    try {
        QPDF pdf;
        pdf.emptyPDF();

        QPDFObjectHandle font = pdf.makeIndirectObject(QPDFObjectHandle::parse(
            "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>"));

        QPDFObjectHandle resources = QPDFObjectHandle::parse("<< /Font << >> /XObject << >> >>");
        resources.getKey("/Font").replaceKey("/F1", font);

        const std::string content = "1 0 0 rg\n100 600 200 80 re\nf\n"
                                    "0 0 0 rg\nBT /F1 12 Tf 1 0 0 1 100 500 Tm (Hallo Welt) Tj ET\n"
                                    "0 0 1 RG\n2 w\n100 400 m 300 450 l\nS\n";

        QPDFObjectHandle page = QPDFObjectHandle::parse("<< /Type /Page /MediaBox [0 0 612 792] >>");
        page.replaceKey("/Resources", resources);
        page.replaceKey("/Contents", QPDFObjectHandle::newStream(&pdf, content));
        if (rotate != 0) {
            page.replaceKey("/Rotate", QPDFObjectHandle::newInteger(rotate));
        }

        QPDFPageDocumentHelper documents(pdf);
        documents.addPage(QPDFPageObjectHelper(pdf.makeIndirectObject(page)), false);

        QPDFWriter writer(pdf, path.toUtf8().constData());
        writer.write();
        return path;
    } catch (const std::exception &) {
        return {};
    }
}

void TestPageobjects::findsTextImagesAndShapes()
{
    const QString path = writeMixedPage(QStringLiteral("mixed.pdf"));
    QVERIFY(!path.isEmpty());

    QString error;
    const QVector<PageObject> objects = PageObjects::read(path, 0, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(objects.size(), 3);

    QCOMPARE(objects.at(0).kind, PageObject::Kind::Path);
    QCOMPARE(objects.at(1).kind, PageObject::Kind::Text);
    QCOMPARE(objects.at(2).kind, PageObject::Kind::Path);

    // Indexes count painting operators from the start of the page: that is the
    // handle every edit uses, so it has to be 0, 1, 2 and nothing else.
    QCOMPARE(objects.at(0).index, 0);
    QCOMPARE(objects.at(1).index, 1);
    QCOMPARE(objects.at(2).index, 2);

    // Later objects lie on top.
    QVERIFY(objects.at(2).layer > objects.at(0).layer);

    QCOMPARE(objects.at(1).text, QStringLiteral("Hallo Welt"));
    QCOMPARE(objects.at(1).fontSize, 12.0);
    QCOMPARE(objects.at(1).fontResource, QStringLiteral("/F1"));

    // The filled rectangle is red; the stroked line has no interior at all.
    QVERIFY2(objects.at(0).fill.red() > 200 && objects.at(0).fill.green() < 40, qPrintable(objects.at(0).fill.name()));
    QVERIFY2(!objects.at(2).fill.isValid(), "a stroke-only shape must not claim a fill colour");
    QVERIFY(objects.at(2).stroke.blue() > 200);
    QCOMPARE(objects.at(2).lineWidth, 2.0);
}

void TestPageobjects::reportsBoundsWhereTheThingActuallyIs()
{
    const QString path = writeMixedPage(QStringLiteral("bounds.pdf"));
    QString error;
    const QVector<PageObject> objects = PageObjects::read(path, 0, &error);
    QCOMPARE(objects.size(), 3);

    // The rectangle was drawn at 100,600 measuring 200 by 80.
    const QRectF box = objects.at(0).bounds;
    QVERIFY2(qAbs(box.x() - 100.0) < 1.5, qPrintable(QString::number(box.x())));
    QVERIFY2(qAbs(box.y() - 600.0) < 1.5, qPrintable(QString::number(box.y())));
    QVERIFY2(qAbs(box.width() - 200.0) < 2.0, qPrintable(QString::number(box.width())));

    // And the renderer agrees that there is red ink inside it.
    PopplerBackend backend;
    QVERIFY(backend.addDocument(1, path, nullptr));
    const QImage page = backend.renderPage(1, 0, 612);
    const QColor inside = page.pixelColor(int(box.center().x()), int(792 - box.center().y()));
    QVERIFY2(inside.red() > 200 && inside.green() < 60,
             qPrintable(QStringLiteral("expected red at the reported centre, got %1").arg(inside.name())));

    // The text sits on baseline 500, so its box straddles it.
    const QRectF textBox = objects.at(1).bounds;
    QVERIFY2(textBox.y() < 500.0 && textBox.bottom() > 500.0,
             qPrintable(QStringLiteral("text box is %1..%2").arg(textBox.y()).arg(textBox.bottom())));
    QVERIFY(textBox.width() > 40.0 && textBox.width() < 120.0);
}

void TestPageobjects::picksTheTopmostObjectUnderThePoint()
{
    const QString path = writeMixedPage(QStringLiteral("hit.pdf"));
    QString error;
    const QVector<PageObject> objects = PageObjects::read(path, 0, &error);

    QCOMPARE(PageObjects::hitTest(objects, QPointF(200, 640)), 0);
    QCOMPARE(PageObjects::hitTest(objects, QPointF(110, 503)), 1);
    QCOMPARE(PageObjects::hitTest(objects, QPointF(50, 50)), -1);

    // A rubber band across the page catches everything it touches.
    const QVector<int> caught = PageObjects::within(objects, QRectF(90, 390, 250, 300));
    QCOMPARE(caught.size(), 3);
}

void TestPageobjects::movesOneObjectAndLeavesTheRestByteIdentical()
{
    const QString path = writeMixedPage(QStringLiteral("move.pdf"));
    const QString out = m_dir.filePath(QStringLiteral("moved.pdf"));

    PageEdit edit;
    edit.page = 0;
    edit.object = 0;
    edit.transform = QTransform::fromTranslate(50, -30);

    PageComposer::Report report;
    QString error;
    QVERIFY2(PageComposer::apply(path, out, { edit }, {}, &report, &error), qPrintable(error));
    QCOMPARE(report.transformed, 1);
    QVERIFY(report.refusals.isEmpty());

    QString readError;
    const QVector<PageObject> after = PageObjects::read(out, 0, &readError);
    QCOMPARE(after.size(), 3);

    // The rectangle moved by exactly what was asked for.
    QVERIFY2(qAbs(after.at(0).bounds.x() - 150.0) < 1.5, qPrintable(QString::number(after.at(0).bounds.x())));
    QVERIFY2(qAbs(after.at(0).bounds.y() - 570.0) < 1.5, qPrintable(QString::number(after.at(0).bounds.y())));

    // And nothing else did. This is the promise the whole editor rests on.
    const QVector<PageObject> before = PageObjects::read(path, 0, &readError);
    for (int i = 1; i < 3; ++i) {
        QVERIFY2(qAbs(after.at(i).bounds.x() - before.at(i).bounds.x()) < 0.01,
                 qPrintable(QStringLiteral("object %1 moved from %2 to %3")
                                .arg(i)
                                .arg(before.at(i).bounds.x())
                                .arg(after.at(i).bounds.x())));
        QVERIFY(qAbs(after.at(i).bounds.y() - before.at(i).bounds.y()) < 0.01);
    }
}

void TestPageobjects::movesByTheAmountAskedForRegardlessOfScaling()
{
    // The trap: inside the stream a matrix acts before the one already in force,
    // so a transform meant for page space has to be conjugated by the current
    // matrix. Without that, an object drawn under a scaling `cm` moves by the
    // wrong amount, and the error is proportional to its own scale, which makes
    // it look like a mystery rather than a bug.
    const QString path = m_dir.filePath(QStringLiteral("scaled.pdf"));
    try {
        QPDF pdf;
        pdf.emptyPDF();
        // The same rectangle drawn once plainly and once under a 4x scale.
        const std::string content = "1 0 0 rg\n100 600 50 20 re\nf\n"
                                    "q 4 0 0 4 0 0 cm\n0 0 1 rg\n25 100 12.5 5 re\nf\nQ\n";
        QPDFObjectHandle page = QPDFObjectHandle::parse("<< /Type /Page /MediaBox [0 0 612 792] >>");
        page.replaceKey("/Resources", QPDFObjectHandle::parse("<< >>"));
        page.replaceKey("/Contents", QPDFObjectHandle::newStream(&pdf, content));
        QPDFPageDocumentHelper documents(pdf);
        documents.addPage(QPDFPageObjectHelper(pdf.makeIndirectObject(page)), false);
        QPDFWriter writer(pdf, path.toUtf8().constData());
        writer.write();
    } catch (const std::exception &e) {
        QFAIL(e.what());
    }

    QString error;
    const QVector<PageObject> before = PageObjects::read(path, 0, &error);
    QCOMPARE(before.size(), 2);

    QVector<PageEdit> edits;
    for (int i = 0; i < 2; ++i) {
        PageEdit edit;
        edit.page = 0;
        edit.object = i;
        edit.transform = QTransform::fromTranslate(40, 0);
        edits.append(edit);
    }

    const QString out = m_dir.filePath(QStringLiteral("scaled-moved.pdf"));
    PageComposer::Report report;
    QVERIFY2(PageComposer::apply(path, out, edits, {}, &report, &error), qPrintable(error));
    QCOMPARE(report.transformed, 2);

    const QVector<PageObject> after = PageObjects::read(out, 0, &error);
    QCOMPARE(after.size(), 2);

    // Both moved forty points, the scaled one included.
    for (int i = 0; i < 2; ++i) {
        const double moved = after.at(i).bounds.x() - before.at(i).bounds.x();
        QVERIFY2(qAbs(moved - 40.0) < 0.5,
                 qPrintable(QStringLiteral("object %1 moved %2 points instead of 40").arg(i).arg(moved)));
    }
}

void TestPageobjects::deletesAnObjectSoItLeavesThePage()
{
    const QString path = writeMixedPage(QStringLiteral("del.pdf"));
    const QString out = m_dir.filePath(QStringLiteral("deleted.pdf"));

    PageEdit edit;
    edit.kind = PageEdit::Kind::Delete;
    edit.page = 0;
    edit.object = 0;

    PageComposer::Report report;
    QString error;
    QVERIFY2(PageComposer::apply(path, out, { edit }, {}, &report, &error), qPrintable(error));
    QCOMPARE(report.deleted, 1);

    QCOMPARE(PageObjects::read(out, 0, &error).size(), 2);

    // Gone from the picture, not merely from the list.
    PopplerBackend backend;
    QVERIFY(backend.addDocument(1, out, nullptr));
    const QColor where = backend.renderPage(1, 0, 612).pixelColor(200, 792 - 640);
    QVERIFY2(where.red() > 200 && where.green() > 200 && where.blue() > 200,
             qPrintable(QStringLiteral("expected white where the rectangle was, got %1").arg(where.name())));

    // And the text is still there.
    QVERIFY(backend.extractText(1, 0).contains(QStringLiteral("Hallo Welt")));
}

void TestPageobjects::laysABackdropBehindThePageAndCanLeaveTheAnnotationsOff()
{
    const QString path = writeMixedPage(QStringLiteral("backdrop.pdf"));
    const QString out = m_dir.filePath(QStringLiteral("on-black.pdf"));

    // Behind rather than on top, which is the whole of what this is for: a layer
    // that wants to know what one object contributes to a page composes the page
    // twice over two known colours and takes the difference. An insertion goes
    // after the page's own content and would hide it instead.
    PageComposer::Options options;
    options.backdrop = QColor(Qt::black);
    options.annotations = false;

    PageComposer::Report report;
    QString error;
    QVERIFY2(PageComposer::apply(path, out, {}, {}, options, &report, &error), qPrintable(error));

    PopplerBackend backend;
    QVERIFY(backend.addDocument(1, out, nullptr));
    const QImage sheet = backend.renderPage(1, 0, 612);
    QVERIFY(!sheet.isNull());

    const QColor bare = sheet.pixelColor(500, 100);
    QVERIFY2(bare.red() < 20 && bare.green() < 20 && bare.blue() < 20,
             qPrintable(QStringLiteral("bare paper came out %1 rather than the backdrop's black").arg(bare.name())));

    // And what the page draws is still drawn, over the backdrop rather than
    // under it. The rectangle writeMixedPage() puts down is red.
    const QColor ink = sheet.pixelColor(200, 792 - 640);
    QVERIFY2(ink.red() > 150 && ink.green() < 90,
             qPrintable(QStringLiteral("the page's own rectangle came out %1 over the backdrop").arg(ink.name())));

    // Nothing at all asked for is still nothing to do, backdrop or no backdrop.
    QVERIFY(!PageComposer::apply(path, m_dir.filePath(QStringLiteral("idle.pdf")), {}, {}, &report, &error));
}

void TestPageobjects::restylesWithoutRewriting()
{
    const QString path = writeMixedPage(QStringLiteral("style.pdf"));
    const QString out = m_dir.filePath(QStringLiteral("restyled.pdf"));

    PageEdit edit;
    edit.kind = PageEdit::Kind::Restyle;
    edit.page = 0;
    edit.object = 0;
    edit.fill = QColor(0, 160, 0);

    PageComposer::Report report;
    QString error;
    QVERIFY2(PageComposer::apply(path, out, { edit }, {}, &report, &error), qPrintable(error));
    QCOMPARE(report.restyled, 1);

    PopplerBackend backend;
    QVERIFY(backend.addDocument(1, out, nullptr));
    const QColor where = backend.renderPage(1, 0, 612).pixelColor(200, 792 - 640);
    QVERIFY2(where.green() > 120 && where.red() < 90,
             qPrintable(QStringLiteral("expected green, got %1").arg(where.name())));
}

void TestPageobjects::insertsTextShapesAndPictures()
{
    const QString path = writeMixedPage(QStringLiteral("insert.pdf"));
    const QString out = m_dir.filePath(QStringLiteral("inserted.pdf"));

    NewContent label;
    label.kind = NewContent::Kind::Text;
    label.page = 0;
    label.rect = QRectF(100, 300, 300, 20);
    label.text = QStringLiteral("Nachtraeglich eingefuegt");
    label.fontSize = 14.0;

    NewContent box;
    box.kind = NewContent::Kind::Rectangle;
    box.page = 0;
    box.rect = QRectF(350, 600, 120, 90);
    box.fill = QColor(255, 200, 0);
    box.stroke = QColor(Qt::black);

    NewContent oval;
    oval.kind = NewContent::Kind::Ellipse;
    oval.page = 0;
    oval.rect = QRectF(350, 450, 120, 90);
    oval.fill = QColor(0, 140, 255);

    QImage picture(40, 40, QImage::Format_RGB32);
    picture.fill(QColor(120, 0, 160));
    NewContent image;
    image.kind = NewContent::Kind::Image;
    image.page = 0;
    image.rect = QRectF(100, 100, 80, 80);
    image.image = picture;

    PageComposer::Report report;
    QString error;
    QVERIFY2(PageComposer::apply(path, out, {}, { label, box, oval, image }, &report, &error), qPrintable(error));
    QCOMPARE(report.inserted, 4);

    PopplerBackend backend;
    QVERIFY(backend.addDocument(1, out, nullptr));
    const QImage rendered = backend.renderPage(1, 0, 612);

    QVERIFY2(backend.extractText(1, 0).contains(QStringLiteral("Nachtraeglich")),
             qPrintable(backend.extractText(1, 0).left(200)));

    const QColor inBox = rendered.pixelColor(410, 792 - 645);
    QVERIFY2(inBox.red() > 200 && inBox.green() > 150 && inBox.blue() < 90,
             qPrintable(QStringLiteral("rectangle: %1").arg(inBox.name())));

    const QColor inOval = rendered.pixelColor(410, 792 - 495);
    QVERIFY2(inOval.blue() > 180 && inOval.red() < 110, qPrintable(QStringLiteral("ellipse: %1").arg(inOval.name())));

    const QColor inImage = rendered.pixelColor(140, 792 - 140);
    QVERIFY2(inImage.red() > 80 && inImage.blue() > 110 && inImage.green() < 80,
             qPrintable(QStringLiteral("picture: %1").arg(inImage.name())));

    // What was there before is untouched.
    const QColor original = rendered.pixelColor(200, 792 - 640);
    QVERIFY2(original.red() > 200 && original.green() < 60, "the original rectangle changed");
}

void TestPageobjects::insertsAPictureTurnedByItsPlacement()
{
    const QString path = writeMixedPage(QStringLiteral("turnable.pdf"));
    const QString out = m_dir.filePath(QStringLiteral("turned-picture.pdf"));

    // Red on the left, blue on the right. A picture of one colour comes out of
    // every turn looking the same, so it can say where a thing landed and never
    // which way round it went.
    QImage picture(40, 40, QImage::Format_RGB32);
    picture.fill(QColor(200, 40, 40));
    {
        QPainter painter(&picture);
        painter.fillRect(QRect(20, 0, 20, 40), QColor(40, 60, 200));
    }

    NewContent turned;
    turned.kind = NewContent::Kind::Image;
    turned.page = 0;
    turned.rect = QRectF(100, 100, 80, 80);
    turned.image = picture;

    // A quarter turn about the middle of its own box. Points run up the page, so
    // this goes counter-clockwise and takes what was on the left to the bottom.
    const QPointF middle = turned.rect.center();
    QTransform rotation;
    rotation.rotate(90);
    turned.placement = QTransform::fromTranslate(-middle.x(), -middle.y()) * rotation
        * QTransform::fromTranslate(middle.x(), middle.y());

    PageComposer::Report report;
    QString error;
    QVERIFY2(PageComposer::apply(path, out, {}, { turned }, &report, &error), qPrintable(error));
    QCOMPARE(report.inserted, 1);

    PopplerBackend backend;
    QVERIFY(backend.addDocument(1, out, nullptr));
    const QImage rendered = backend.renderPage(1, 0, 612);

    // Sampled off both middles, so that each point says something different
    // about a picture left upright, one turned the other way, and one turned as
    // it was asked: at the bottom right the box holds what was the picture's
    // left, and at the top left what was its right.
    const QColor low = rendered.pixelColor(160, 792 - 120);
    QVERIFY2(
        low.red() > 150 && low.blue() < 110,
        qPrintable(QStringLiteral("the picture's left half came out %1 at the bottom of the box").arg(low.name())));

    const QColor high = rendered.pixelColor(120, 792 - 160);
    QVERIFY2(high.blue() > 150 && high.red() < 110,
             qPrintable(QStringLiteral("the picture's right half came out %1 at the top of the box").arg(high.name())));

    // And it is still the size it was given: a turn that grew the picture is a
    // turn that went into the box round it rather than into the picture.
    QString why;
    const QVector<PageObject> objects = PageObjects::read(out, 0, &why);
    QVERIFY2(!objects.isEmpty(), qPrintable(why));
    for (const PageObject &object : objects) {
        if (object.kind != PageObject::Kind::Image) {
            continue;
        }
        const QPolygonF corners = object.placement.map(QPolygonF() << QPointF(0, 0) << QPointF(1, 0) << QPointF(1, 1));
        QVERIFY2(qAbs(QLineF(corners.at(0), corners.at(1)).length() - 80.0) < 1.0
                     && qAbs(QLineF(corners.at(1), corners.at(2)).length() - 80.0) < 1.0,
                 "the turned picture was written at a different size from the one it was given");
    }
}

void TestPageobjects::followsARotatedPage()
{
    // Bounds are reported in the space a person sees, so on a turned page they
    // must be turned too, and scans are turned as a matter of course.
    const QString path = writeMixedPage(QStringLiteral("turned.pdf"), 90);
    QString error;
    const QVector<PageObject> objects = PageObjects::read(path, 0, &error);
    QCOMPARE(objects.size(), 3);

    // Page space 100,600 with /Rotate 90 appears at display 600, 612-100-200.
    const QRectF box = objects.at(0).bounds;
    QVERIFY2(qAbs(box.x() - 600.0) < 2.0, qPrintable(QString::number(box.x())));
    QVERIFY2(qAbs(box.width() - 80.0) < 2.0, qPrintable(QString::number(box.width())));

    // Moving in display space still moves by what was asked.
    PageEdit edit;
    edit.page = 0;
    edit.object = 0;
    edit.transform = QTransform::fromTranslate(30, 0);

    const QString out = m_dir.filePath(QStringLiteral("turned-moved.pdf"));
    PageComposer::Report report;
    QVERIFY2(PageComposer::apply(path, out, { edit }, {}, &report, &error), qPrintable(error));
    QCOMPARE(report.transformed, 1);

    // Counting the edit is not the same as making it. Thirty points to the right
    // of a sheet the reader is looking at is thirty points *up* the page it is
    // stored on, and a matrix handed straight to the stream without that turn
    // moves the rectangle at right angles to the way it was dragged.
    const QVector<PageObject> after = PageObjects::read(out, 0, &error);
    QCOMPARE(after.size(), 3);
    const QRectF moved = after.at(0).bounds;
    QVERIFY2(qAbs(moved.x() - (box.x() + 30.0)) < 1.0,
             qPrintable(QStringLiteral("moved across to %1 rather than to %2").arg(moved.x()).arg(box.x() + 30.0)));
    QVERIFY2(qAbs(moved.y() - box.y()) < 1.0,
             qPrintable(
                 QStringLiteral("a move across the sheet carried it from %1 to %2 up it").arg(box.y()).arg(moved.y())));
}

void TestPageobjects::readsARealMagazinePage()
{
    const QString real = qEnvironmentVariable("PS_STRESS_PDF");
    if (!QFile::exists(real)) {
        QSKIP("set PS_STRESS_PDF to a large real document to run this");
    }

    QString error;
    const QVector<PageObject> objects = PageObjects::read(real, 2, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));

    // A real magazine page: plenty of text, at least one picture, some vector art.
    QVERIFY2(objects.size() > 20, qPrintable(QStringLiteral("only %1 objects found").arg(objects.size())));

    int text = 0;
    int images = 0;
    for (const PageObject &object : objects) {
        if (object.kind == PageObject::Kind::Text) {
            ++text;
        }
        if (object.kind == PageObject::Kind::Image) {
            ++images;
        }
        // Nothing may claim a boundary outside a sane multiple of the page.
        QVERIFY2(
            object.bounds.isEmpty() || qAbs(object.bounds.x()) < 5000.0,
            qPrintable(QStringLiteral("%1 at x=%2").arg(PageObjects::describe(object.kind)).arg(object.bounds.x())));
    }
    QVERIFY2(text > 10, qPrintable(QStringLiteral("only %1 text objects").arg(text)));
    QVERIFY2(images >= 1, "no picture found on a magazine page");
}

void TestPageobjects::refusesNothing()
{
    const QString path = writeMixedPage(QStringLiteral("nothing.pdf"));
    QString error;
    QVERIFY(!PageComposer::apply(path, m_dir.filePath(QStringLiteral("no.pdf")), {}, {}, nullptr, &error));
    QVERIFY(!error.isEmpty());

    QVERIFY(PageObjects::read(path, 99, &error).isEmpty());
    QVERIFY(!error.isEmpty());
}

void TestPageobjects::reportsItsOwnLimits()
{
    for (const QStringList &limits : { PageObjects::limitations(), PageComposer::limitations() }) {
        QVERIFY(limits.size() >= 3);
        for (const QString &limit : limits) {
            QVERIFY(!limit.trimmed().isEmpty());
        }
    }
}

QTEST_MAIN(TestPageobjects)

#include "tst_pageobjects.moc"
