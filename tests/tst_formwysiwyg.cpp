/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "TestPdf.h"
#include "core/Annotation.h"
#include "core/Document.h"
#include "core/FormBuilder.h"
#include "core/Forms.h"
#include "render/PopplerBackend.h"
#include "ui/FormDesignOverlay.h"
#include "ui/PageView.h"

#include <KLocalizedString>

#include <QImage>
#include <QTemporaryDir>
#include <QTest>

using namespace ps;

/**
 * Laying a form out on the page, and whether the page is telling the truth.
 *
 * The complaint these answer: drag a field and there are two of it. The one
 * under the pointer, drawn by the layer that is moving it, and the original,
 * painted into the page bitmap by the rasteriser, which knows nothing about the
 * gesture and goes on drawing the field where the file still has it. No amount
 * of drawing on top removes it, because it is not on top; it is the page.
 *
 * The fixtures are built with FormBuilder rather than by hand, for two reasons.
 * A field with no appearance stream renders as nothing at all, so a hand-written
 * one gives a pixel test nothing to look at. And FormBuilder is the engine the
 * work is eventually handed to, so a field it made is a field whose appearance
 * the screen is claiming it can reproduce, which is exactly the claim worth
 * checking.
 */
class TestFormWysiwyg : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();

    void theRenderLeavesTheFieldsOutAndKeepsTheComments();
    void aMovedFieldLeavesNothingStandingBehindIt();
    void whatIsShownIsWhatWillBeSaved();
    void everyButtonOfAGroupIsStillDrawn();
    void readingModeIsUnchanged();

private:
    /** A one-page document carrying @p fields, built the way the engine builds them. */
    QString formPdf(const QString &name, const QVector<FormBuilder::Field> &fields);

    /** One plain text field, yellow enough to be counted on a white page. */
    static FormBuilder::Field yellowField(const QString &name, const QRectF &rect);

    /** How many pixels of @p image inside @p box are the field's own yellow. */
    static int yellowInside(const QImage &image, const QRect &box);

    /** What the view actually puts on the screen: the page, and every layer over it. */
    static QImage onScreen(PageView &view);

    QTemporaryDir m_dir;
};

void TestFormWysiwyg::initTestCase()
{
    KLocalizedString::setApplicationDomain("pdf-smithy");
    QVERIFY(m_dir.isValid());
}

FormBuilder::Field TestFormWysiwyg::yellowField(const QString &name, const QRectF &rect)
{
    FormBuilder::Field field;
    field.kind = FormBuilder::Field::Kind::Text;
    field.name = name;
    field.page = 0;
    field.rect = rect;
    // A colour no page furniture uses, so "is the field here" is a question
    // about pixels rather than about ink in general.
    field.backgroundColour = QColor(255, 235, 0);
    field.borderColour = QColor(0, 0, 0);
    field.borderWidth = 1.0;
    field.fontSize = 10.0;
    return field;
}

QString TestFormWysiwyg::formPdf(const QString &name, const QVector<FormBuilder::Field> &fields)
{
    const QString blank = m_dir.filePath(QLatin1String("blank-") + name);
    if (!test::writeSamplePdf(blank, 1)) {
        return {};
    }
    const QString path = m_dir.filePath(name);
    QString error;
    if (!FormBuilder::addFields(blank, path, fields, nullptr, &error)) {
        qWarning("%s", qPrintable(error));
        return {};
    }
    return path;
}

int TestFormWysiwyg::yellowInside(const QImage &image, const QRect &box)
{
    int found = 0;
    for (int y = box.top(); y <= box.bottom(); ++y) {
        for (int x = box.left(); x <= box.right(); ++x) {
            if (!image.rect().contains(x, y)) {
                continue;
            }
            const QColor pixel = image.pixelColor(x, y);
            if (pixel.red() > 200 && pixel.green() > 180 && pixel.blue() < 120) {
                ++found;
            }
        }
    }
    return found;
}

QImage TestFormWysiwyg::onScreen(PageView &view)
{
    // Grabbed rather than composed by hand: what is on the screen is the page
    // the view drew plus every layer it asked, in the order it asks them, and
    // any test that assembled those itself would be checking its own assembly.
    return view.viewport()->grab().toImage();
}

void TestFormWysiwyg::theRenderLeavesTheFieldsOutAndKeepsTheComments()
{
    const QString form
        = formPdf(QStringLiteral("comment.pdf"), { yellowField(QStringLiteral("Name"), QRectF(72, 600, 200, 24)) });
    QVERIFY(!form.isEmpty());

    // A comment on the same page as the field. Poppler's own HideAnnotations
    // hint would take both away, which is why it is not what the backend uses.
    Annotation note;
    note.type = Annotation::Type::Highlight;
    note.page = 0;
    note.rect = QRectF(72, 500, 200, 20);
    note.quads = { note.rect };
    note.colour = QColor(0, 120, 255);
    const QString marked = m_dir.filePath(QStringLiteral("marked.pdf"));
    QString error;
    QVERIFY2(Annotations::add(form, marked, { note }, &error), qPrintable(error));

    PopplerBackend backend;
    QVERIFY(backend.addDocument(1, marked, &error));

    RenderBackend::Request whole;
    whole.widthPx = 612;
    const QImage complete = backend.render(1, 0, whole);
    QVERIFY(!complete.isNull());

    RenderBackend::Request withoutFields = whole;
    withoutFields.omit = RenderBackend::Request::Omit::FormFields;
    const QImage stripped = backend.render(1, 0, withoutFields);
    QCOMPARE(stripped.size(), complete.size());

    // The page is 792 points tall and the render is one pixel to the point, and
    // the origin runs the other way.
    const QRect fieldBox(72, 792 - 624, 200, 24);
    const QRect commentBox(72, 792 - 520, 200, 20);

    QVERIFY2(yellowInside(complete, fieldBox) > 3000,
             qPrintable(QStringLiteral("the field is not on the plain render: %1 yellow pixels")
                            .arg(yellowInside(complete, fieldBox))));
    QCOMPARE(yellowInside(stripped, fieldBox), 0);

    // And the comment is untouched, pixel for pixel.
    int commentDiffers = 0;
    for (int y = commentBox.top(); y <= commentBox.bottom(); ++y) {
        for (int x = commentBox.left(); x <= commentBox.right(); ++x) {
            if (complete.pixel(x, y) != stripped.pixel(x, y)) {
                ++commentDiffers;
            }
        }
    }
    QCOMPARE(commentDiffers, 0);

    // The flags are put back: the handle goes into a pool and is used again, and
    // a field left hidden would be missing from every later render made with it.
    const QImage again = backend.render(1, 0, whole);
    QCOMPARE(yellowInside(again, fieldBox), yellowInside(complete, fieldBox));
}

void TestFormWysiwyg::aMovedFieldLeavesNothingStandingBehindIt()
{
    const QRectF where(72, 600, 200, 24);
    const QString form = formPdf(QStringLiteral("moved.pdf"), { yellowField(QStringLiteral("Name"), where) });
    QVERIFY(!form.isEmpty());

    PopplerBackend backend;
    Document held;
    held.setRenderBackend(&backend);
    QString error;
    QVERIFY2(held.open(form, &error), qPrintable(error));

    PageView view;
    view.setDocument(&held);
    view.setRenderBackend(&backend);
    view.resize(700, 900);
    view.setZoom(1.0);
    view.setMode(PageView::Mode::Edit);

    FormDesignOverlay design(&view);
    design.setDocument(&held);
    design.setSource(form);
    design.setFields(Forms::read(form, &error));
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(design.fields().size(), 1);
    view.show();
    QTRY_VERIFY_WITH_TIMEOUT(view.isSharp(0), 15000);

    // Picked up in the middle and carried a hundred points down the page.
    const QPointF from = where.center();
    const QPointF to = from - QPointF(0.0, 100.0);
    QVERIFY(design.press(0, from, Qt::LeftButton));
    QVERIFY(design.move(0, to, Qt::LeftButton));
    QVERIFY(design.release(0, to));
    QVERIFY(design.hasPendingWork());
    QTRY_VERIFY_WITH_TIMEOUT(view.isSharp(0), 15000);

    const QImage screen = onScreen(view);
    const QRect wasThere = view.fromPoints(0, where).toAlignedRect();
    const QRect isNow = view.fromPoints(0, where.translated(0.0, -100.0)).toAlignedRect();

    // This is the complaint, in one number. Before the page was asked for
    // without its fields, the rasteriser went on painting this one into the
    // bitmap wherever the file had it, and the drag drew a second copy beside
    // it: 4356 yellow pixels here where the field is no longer meant to be.
    QVERIFY2(yellowInside(screen, wasThere) == 0,
             qPrintable(QStringLiteral("the original is still standing: %1 yellow pixels where the field was")
                            .arg(yellowInside(screen, wasThere))));

    // And exactly one of it, where the hand put it.
    QVERIFY2(
        yellowInside(screen, isNow) > 2500,
        qPrintable(
            QStringLiteral("the moved field is not being drawn: %1 yellow pixels").arg(yellowInside(screen, isNow))));
}

void TestFormWysiwyg::whatIsShownIsWhatWillBeSaved()
{
    const QRectF where(72, 600, 200, 24);
    FormBuilder::Field styled = yellowField(QStringLiteral("Name"), where);
    styled.defaultValue = QStringLiteral("Muster");
    const QString form = formPdf(QStringLiteral("settled.pdf"), { styled });
    QVERIFY(!form.isEmpty());

    PopplerBackend backend;
    Document held;
    held.setRenderBackend(&backend);
    QString error;
    QVERIFY2(held.open(form, &error), qPrintable(error));

    PageView view;
    view.setDocument(&held);
    view.setRenderBackend(&backend);
    view.resize(700, 900);
    view.setZoom(1.0);
    view.setMode(PageView::Mode::Edit);

    FormDesignOverlay design(&view);
    design.setDocument(&held);
    design.setSource(form);
    design.setFields(Forms::read(form, &error));
    view.show();
    QTRY_VERIFY_WITH_TIMEOUT(view.isSharp(0), 15000);

    const QPointF from = where.center();
    const QPointF to = from + QPointF(60.0, -140.0);
    QVERIFY(design.press(0, from, Qt::LeftButton));
    QVERIFY(design.move(0, to, Qt::LeftButton));
    QVERIFY(design.release(0, to));

    // The oracle: the very file the window would write, put through the same
    // rasteriser at the same size. Built from the work the overlay itself says
    // is waiting, so that what is checked is the overlay's own claim.
    const FormDesignOverlay::Work work = design.workOn(0);
    QCOMPARE(work.changed.size(), 1);
    QCOMPARE(work.changed.constFirst().page, 0);

    const QString saved = m_dir.filePath(QStringLiteral("as-saved.pdf"));
    QVERIFY2(FormBuilder::updateFields(form, saved, work.changed, nullptr, &error), qPrintable(error));

    PopplerBackend oracle;
    QVERIFY(oracle.addDocument(1, saved, &error));
    const QRect page = view.pageRect(0);
    const QImage truth = oracle.renderPage(1, 0, page.width());
    QVERIFY(!truth.isNull());

    // The field's own box and a margin round it, which is the part of the page
    // this layer answers for. Nothing is chosen while the picture is taken: what
    // a chosen field wears (the wash, the ring and the eight grips) is this
    // program's scaffolding and is meant to be there, and what is being compared
    // is the field rather than the furniture round it.
    const QRect box = view.fromPoints(0, where.translated(60.0, -140.0)).toAlignedRect().adjusted(-4, -4, 4, 4);
    const auto disagreement = [&](const QImage &screen) {
        int differing = 0;
        int counted = 0;
        for (int y = box.top(); y <= box.bottom(); ++y) {
            for (int x = box.left(); x <= box.right(); ++x) {
                const QPoint onPage = QPoint(x, y) - page.topLeft();
                if (!screen.rect().contains(x, y) || !truth.rect().contains(onPage)) {
                    continue;
                }
                ++counted;
                const QColor shown = screen.pixelColor(x, y);
                const QColor written = truth.pixelColor(onPage);
                // A tolerance rather than an identity: the two pictures reach the
                // same pixels through different antialiasing.
                if (std::abs(shown.red() - written.red()) > 40 || std::abs(shown.green() - written.green()) > 40
                    || std::abs(shown.blue() - written.blue()) > 40) {
                    ++differing;
                }
            }
        }
        QTest::qVerify(counted > 3000, "counted > 3000", "", __FILE__, __LINE__);
        return differing * 100.0 / std::max(1, counted);
    };

    // While the hand is still on it, what is drawn is this program's
    // reconstruction of the field from /MK, /BS and /DA, the same four things
    // FormBuilder draws its appearance from, so it is close rather than exact.
    // Measured here so that a change which makes it worse shows up as a number.
    design.clearChoice();
    const double drawn = disagreement(onScreen(view));
    QVERIFY2(drawn < 8.0, qPrintable(QStringLiteral("the drawing of the field is %1% away from the file").arg(drawn)));

    // A third of a second of quiet, then a render of the page as it would be if
    // it were saved this instant, and from then on the screen *is* the file.
    QTRY_VERIFY_WITH_TIMEOUT(design.showsTheFile(0), 20000);
    QTRY_VERIFY_WITH_TIMEOUT(view.isSharp(0), 15000);
    const double settled = disagreement(onScreen(view));
    QVERIFY2(settled < 2.0, qPrintable(QStringLiteral("the settled page is %1% away from the file").arg(settled)));
    QVERIFY2(settled <= drawn,
             qPrintable(QStringLiteral("settling made it worse: %1% against %2%").arg(settled).arg(drawn)));
    qInfo("agreement with the file: %.2f%% differing while drawn, %.2f%% once settled", drawn, settled);
}

void TestFormWysiwyg::everyButtonOfAGroupIsStillDrawn()
{
    // One field, three buttons. Hiding the widgets takes all three off the page,
    // and a layer that draws a field by its first box would put one back.
    QVector<FormBuilder::Field> group;
    for (int i = 0; i < 3; ++i) {
        FormBuilder::Field button = yellowField(QStringLiteral("Anrede"), QRectF(72 + 40 * i, 600, 20, 20));
        button.kind = FormBuilder::Field::Kind::Radio;
        button.radioGroup = QStringLiteral("Anrede");
        button.onState = QLatin1Char('/') + QStringLiteral("Wahl%1").arg(i);
        group.append(button);
    }
    const QString form = formPdf(QStringLiteral("group.pdf"), group);
    QVERIFY(!form.isEmpty());

    PopplerBackend backend;
    Document held;
    held.setRenderBackend(&backend);
    QString error;
    QVERIFY2(held.open(form, &error), qPrintable(error));

    PageView view;
    view.setDocument(&held);
    view.setRenderBackend(&backend);
    view.resize(700, 900);
    view.setZoom(1.0);
    view.setMode(PageView::Mode::Edit);

    FormDesignOverlay design(&view);
    design.setDocument(&held);
    design.setSource(form);
    design.setFields(Forms::read(form, &error));
    QCOMPARE(design.fields().size(), 1);
    QCOMPARE(design.fields().constFirst().widgets.size(), 3);
    view.show();
    QTRY_VERIFY_WITH_TIMEOUT(view.isSharp(0), 15000);

    const QImage screen = onScreen(view);
    for (int i = 0; i < 3; ++i) {
        const QRect box = view.fromPoints(0, QRectF(72 + 40 * i, 600, 20, 20)).toAlignedRect();
        QVERIFY2(yellowInside(screen, box) > 100,
                 qPrintable(QStringLiteral("button %1 of the group was not drawn: %2 yellow pixels")
                                .arg(i)
                                .arg(yellowInside(screen, box))));
    }
}

void TestFormWysiwyg::readingModeIsUnchanged()
{
    const QString form
        = formPdf(QStringLiteral("reading.pdf"), { yellowField(QStringLiteral("Name"), QRectF(72, 600, 200, 24)) });
    QVERIFY(!form.isEmpty());

    PopplerBackend backend;
    Document held;
    held.setRenderBackend(&backend);
    QString error;
    QVERIFY2(held.open(form, &error), qPrintable(error));

    PageView view;
    view.setDocument(&held);
    view.setRenderBackend(&backend);
    view.resize(700, 900);
    view.setZoom(1.0);

    FormDesignOverlay design(&view);
    design.setDocument(&held);
    design.setSource(form);
    design.setFields(Forms::read(form, &error));

    // Somebody filling a form in must see the document exactly as any other
    // reader would, so nothing is asked to be left off the page.
    QCOMPARE(view.mode(), PageView::Mode::View);
    QCOMPARE(view.omittedFromRenders(), RenderBackend::Request::Omit::Nothing);

    view.setMode(PageView::Mode::Edit);
    QCOMPARE(view.omittedFromRenders(), RenderBackend::Request::Omit::FormFields);

    view.setMode(PageView::Mode::View);
    QCOMPARE(view.omittedFromRenders(), RenderBackend::Request::Omit::Nothing);

    // And a layer taken off the view stops asking, even though the mode says
    // otherwise: some modes edit the text of a form without laying it out.
    view.setMode(PageView::Mode::Edit);
    QCOMPARE(view.omittedFromRenders(), RenderBackend::Request::Omit::FormFields);
    view.removeOverlay(&design);
    QTRY_COMPARE(view.omittedFromRenders(), RenderBackend::Request::Omit::Nothing);
}

QTEST_MAIN(TestFormWysiwyg)

#include "tst_formwysiwyg.moc"
