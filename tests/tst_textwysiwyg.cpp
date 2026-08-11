/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "core/Document.h"
#include "core/TextEdit.h"
#include "render/PopplerBackend.h"
#include "ui/PageView.h"
#include "ui/TextOverlay.h"

#include <KLocalizedString>

#include <QFontDatabase>
#include <QFontMetricsF>
#include <QLineEdit>
#include <QPainter>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <qpdf/QPDF.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFWriter.hh>

using namespace ps;

/**
 * Typing on the page, and whether what is shown is what the page says.
 *
 * The complaint these answer is the one every in-place PDF editor earns: click
 * into a line and the words become a text field in the desktop's own typeface,
 * at the desktop's own size, in black: a thing that looks nothing like the page
 * it is standing in for. Everything here is about the three answers that have to
 * agree and used not to: what the editor shows, what the preview paints, and
 * which character a click lands in front of.
 *
 * The fixtures use the standard fourteen rather than an embedded face on
 * purpose. A test that depended on a particular font being installed would be a
 * test that fails on somebody else's machine for a reason that has nothing to do
 * with this code; Courier's metrics, on the other hand, are in the PDF
 * specification (every glyph six tenths of an em), so what a click at a given
 * place ought to answer can be worked out rather than measured.
 */
class TestTextWysiwyg : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();

    void readsTheFaceThePageNamesRatherThanJustItsSize();
    void readsTheFillColourOutOfTheContentStream();
    void readsTheSizeOutOfTheTextMatrix();
    void theCaretLandsOnTheCharacterThatWasClicked();
    void theEditorIsSetInThePagesOwnType();
    void showsTheFileOnceTheTypingSettles();
    void writesTheTypeSomebodyAskedFor();
    void refusesAFaceThatCannotTravel();

private:
    /** A one-page document drawing @p text with @p setup before it. */
    QString writePdf(const QString &name, const QByteArray &font, const QByteArray &setup, const QByteArray &text);

    QTemporaryDir m_dir;
};

void TestTextWysiwyg::initTestCase()
{
    KLocalizedString::setApplicationDomain("pdf-smithy");
    QVERIFY(m_dir.isValid());
}

QString TestTextWysiwyg::writePdf(const QString &name, const QByteArray &font, const QByteArray &setup,
                                  const QByteArray &text)
{
    const QString path = m_dir.filePath(name);

    QPDF pdf;
    pdf.emptyPDF();

    QPDFObjectHandle face = pdf.makeIndirectObject(
        QPDFObjectHandle::parse(std::string("<< /Type /Font /Subtype /Type1 /BaseFont ") + font.constData() + " >>"));
    QPDFObjectHandle fonts = QPDFObjectHandle::newDictionary();
    fonts.replaceKey("/F1", face);
    QPDFObjectHandle resources = QPDFObjectHandle::newDictionary();
    resources.replaceKey("/Font", fonts);

    QPDFObjectHandle box = QPDFObjectHandle::newArray();
    for (const int edge : { 0, 0, 612, 400 }) {
        box.appendItem(QPDFObjectHandle::newInteger(edge));
    }

    std::string content = "BT\n";
    content += setup.constData();
    content += "(";
    content.append(text.constData(), size_t(text.size()));
    content += ") Tj\nET\n";

    QPDFObjectHandle page = QPDFObjectHandle::newDictionary();
    page.replaceKey("/Type", QPDFObjectHandle::newName("/Page"));
    page.replaceKey("/MediaBox", box);
    page.replaceKey("/Resources", resources);
    page.replaceKey("/Contents", QPDFObjectHandle::newStream(&pdf, content));
    QPDFPageDocumentHelper(pdf).addPage(pdf.makeIndirectObject(page), false);

    QPDFWriter writer(pdf, QFile::encodeName(path).constData());
    writer.write();
    return path;
}

void TestTextWysiwyg::readsTheFaceThePageNamesRatherThanJustItsSize()
{
    const QString document
        = writePdf(QStringLiteral("bold.pdf"), "/Times-Bold", "/F1 24 Tf 1 0 0 1 72 200 Tm ", "Hamburgefonstiv");

    QString error;
    const QVector<TextEdit::Run> runs = TextEdit::runsOn(document, 0, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(runs.size(), 1);

    const TextEdit::Run &run = runs.constFirst();
    QCOMPARE(run.fontFamily, QStringLiteral("Times-Bold"));
    QCOMPARE(run.fontWeight, 700);
    QCOMPARE(run.italic, false);
    QCOMPARE(run.serif, true);
    QCOMPARE(run.fixedPitch, false);
    QCOMPARE(run.embedded, false);
    QCOMPARE(run.renderMode, 0);

    // A subset prefix is how a producer marks a cut-down font and is no part of
    // the family, so it does not travel into what anybody is shown.
    const QString subsetted
        = writePdf(QStringLiteral("subset.pdf"), "/ABCDEF+Times-Bold", "/F1 24 Tf 1 0 0 1 72 200 Tm ", "Hamburge");
    QCOMPARE(TextEdit::runsOn(subsetted, 0, &error).constFirst().fontFamily, QStringLiteral("Times-Bold"));
}

void TestTextWysiwyg::readsTheFillColourOutOfTheContentStream()
{
    // The colour used not to be read at all, and every replacement was drawn in
    // black: a corrected word in a red heading came out black, on screen and in
    // the file.
    const QString document
        = writePdf(QStringLiteral("red.pdf"), "/Helvetica", "0.8 0.1 0.1 rg /F1 18 Tf 1 0 0 1 72 200 Tm ", "Achtung");

    QString error;
    const QVector<TextEdit::Run> runs = TextEdit::runsOn(document, 0, &error);
    QCOMPARE(runs.size(), 1);
    QCOMPARE(runs.constFirst().colour.red(), 204);
    QCOMPARE(runs.constFirst().colour.green(), 26);
    QCOMPARE(runs.constFirst().colour.blue(), 26);

    // Grey, four-colour and a separation's ink all say something about how dark
    // the letters are, and all of them used to say nothing.
    const QString grey
        = writePdf(QStringLiteral("grey.pdf"), "/Helvetica", "0.5 g /F1 18 Tf 1 0 0 1 72 200 Tm ", "Grau");
    QCOMPARE(TextEdit::runsOn(grey, 0, &error).constFirst().colour.red(), 128);

    const QString cyan
        = writePdf(QStringLiteral("cmyk.pdf"), "/Helvetica", "1 0 0 0 k /F1 18 Tf 1 0 0 1 72 200 Tm ", "Cyan");
    const QColor ink = TextEdit::runsOn(cyan, 0, &error).constFirst().colour;
    QVERIFY2(ink.blue() > 200 && ink.red() < 60, qPrintable(ink.name()));
}

void TestTextWysiwyg::readsTheSizeOutOfTheTextMatrix()
{
    // Several layout programs set `/F1 1 Tf` and do the sizing in the matrix, so
    // the `Tf` operand on its own says nothing about how tall the letters are.
    // Anything that drew from it alone drew them a seventeenth of their size.
    const QString document
        = writePdf(QStringLiteral("matrix.pdf"), "/Helvetica", "/F1 1 Tf 17 0 0 17 72 200 Tm ", "Siebzehn");

    QString error;
    const TextEdit::Run run = TextEdit::runsOn(document, 0, &error).constFirst();
    QCOMPARE(run.fontSize, 1.0);
    QVERIFY2(qAbs(run.scaledSize - 17.0) < 0.01, qPrintable(QString::number(run.scaledSize)));

    // And the box is a fifth taller than the type, which is what puts the
    // baseline where the page put it.
    QVERIFY2(qAbs(run.rect.normalized().height() - 17.0 * 1.2) < 0.01,
             qPrintable(QString::number(run.rect.normalized().height())));
}

void TestTextWysiwyg::theCaretLandsOnTheCharacterThatWasClicked()
{
    // Courier: every glyph six tenths of an em, which the specification settles
    // rather than this machine's fonts. At 24 pt that is 14.4 pt a character, so
    // the boundary before the sixth character is exactly 72 pt along the line,
    // and a caret asked for at that point has one right answer.
    //
    // The letters are chosen so that a proportional font gets it loudly wrong: in
    // any interface typeface five W's are far wider than five sixths of an em
    // each, so a click at 72 pt used to land three characters early.
    const QString document
        = writePdf(QStringLiteral("courier.pdf"), "/Courier", "/F1 24 Tf 1 0 0 1 72 200 Tm ", "WWWWWiiiii");

    Document held;
    QString error;
    QVERIFY2(held.open(document, &error), qPrintable(error));

    PageView view;
    view.setDocument(&held);
    view.resize(900, 700);
    view.setZoom(1.0);
    TextOverlay overlay(&view, &held);

    QString reason;
    const QVector<TextEdit::Run> runs = TextEdit::runsOn(document, 0, &reason);
    QCOMPARE(runs.size(), 1);
    const double left = runs.constFirst().rect.normalized().left();

    QCOMPARE(overlay.caretAt(0, 0, left), 0);
    QCOMPARE(overlay.caretAt(0, 0, left + 5 * 14.4), 5);
    QCOMPARE(overlay.caretAt(0, 0, left + 10 * 14.4), 10);

    // And the same answer in the middle of the line, where an accumulated
    // metric error would have shown up.
    QCOMPARE(overlay.caretAt(0, 0, left + 3 * 14.4), 3);
    QCOMPARE(overlay.caretAt(0, 0, left + 8 * 14.4), 8);

    // The font the caret was measured against is the page's, not the desktop's:
    // fixed pitch, and every character the same width.
    const QFontMetricsF metrics(overlay.fontOf(0, 0));
    QVERIFY2(qAbs(metrics.horizontalAdvance(QStringLiteral("W")) - metrics.horizontalAdvance(QStringLiteral("i")))
                 < 0.51,
             "the run was measured in a proportional font");
}

void TestTextWysiwyg::theEditorIsSetInThePagesOwnType()
{
    const QString document = writePdf(QStringLiteral("editor.pdf"), "/Times-Bold",
                                      "0 0 0.7 rg /F1 30 Tf 1 0 0 1 72 200 Tm ", "Ueberschrift");

    Document held;
    QString error;
    QVERIFY2(held.open(document, &error), qPrintable(error));

    PageView view;
    view.setDocument(&held);
    view.resize(900, 700);
    view.setMode(PageView::Mode::Edit);
    view.setZoom(1.0);
    TextOverlay overlay(&view, &held);
    view.addOverlay(&overlay);
    // Shown, because a widget inside a hidden window is never visible however
    // the overlay places it.
    view.show();

    const QVector<TextEdit::Run> runs = TextEdit::runsOn(document, 0, &error);
    QCOMPARE(runs.size(), 1);
    QVERIFY(overlay.press(0, runs.constFirst().rect.normalized().center(), Qt::LeftButton));

    auto *editor = view.viewport()->findChild<QLineEdit *>();
    QVERIFY2(editor, "clicking into a line did not put a caret in it");
    QVERIFY(editor->isVisible());
    QCOMPARE(editor->text(), QStringLiteral("Ueberschrift"));

    // Thirty points at a zoom of one is thirty pixels of type, whatever the
    // desktop sets its menus in. The old editor asked the widget for its font
    // and only changed the size, so a heading came up in the interface face.
    const QFontMetricsF metrics(editor->font());
    QVERIFY2(qAbs(metrics.height() - QFontMetricsF(view.font()).height()) > 4.0,
             "the editor is still wearing the interface font");
    QVERIFY2(qAbs(editor->font().pointSizeF() * view.logicalDpiY() / 72.0 - 30.0) < 1.0,
             qPrintable(QStringLiteral("the type came out %1 pixels tall")
                            .arg(editor->font().pointSizeF() * view.logicalDpiY() / 72.0)));

    // And the ink is the page's ink rather than the theme's.
    QCOMPARE(editor->palette().color(QPalette::Text).blue(), 179);
    QCOMPARE(editor->palette().color(QPalette::Text).red(), 0);

    // The letters in the widget stand on the line the page drew them on, which
    // is what stops the words jumping when the caret arrives.
    const double baseline = runs.constFirst().rect.normalized().top() + 0.25 * 30.0;
    const double wanted = view.fromPoints(0, QPointF(0.0, baseline)).y();
    const double got = editor->geometry().top() + QFontMetrics(editor->font()).ascent();
    QVERIFY2(qAbs(wanted - got) <= 3.0,
             qPrintable(QStringLiteral("the caret's baseline is %1, the page's is %2").arg(got).arg(wanted)));
}

void TestTextWysiwyg::showsTheFileOnceTheTypingSettles()
{
    const QString document
        = writePdf(QStringLiteral("live.pdf"), "/Helvetica", "/F1 18 Tf 1 0 0 1 72 200 Tm ", "Erste Fassung");

    // The rasteriser before the document is opened: a document registers its
    // sources with whatever backend it has at the time, and one opened without
    // never learns how big its pages are.
    PopplerBackend backend;
    Document held;
    held.setRenderBackend(&backend);

    QString error;
    QVERIFY2(held.open(document, &error), qPrintable(error));

    PageView view;
    view.setDocument(&held);
    view.setRenderBackend(&backend);
    view.resize(900, 700);
    view.setMode(PageView::Mode::Edit);
    view.setZoom(1.0);
    TextOverlay overlay(&view, &held);
    view.addOverlay(&overlay);
    view.show();

    const QVector<TextEdit::Run> runs = TextEdit::runsOn(document, 0, &error);
    QVERIFY(overlay.press(0, runs.constFirst().rect.normalized().center(), Qt::LeftButton));
    auto *editor = view.viewport()->findChild<QLineEdit *>();
    QVERIFY(editor);

    editor->selectAll();
    QTest::keyClicks(editor, QStringLiteral("Zweite Fassung"));
    QVERIFY(overlay.hasEdits());
    QVERIFY2(!overlay.showsTheFile(0), "the page cannot already be rendered before the typing has stopped");

    // A third of a second of quiet, then a page render of a one-page copy: the
    // whole point is that what is on the screen stops being a drawing of the
    // file and becomes the file.
    QTRY_VERIFY_WITH_TIMEOUT(overlay.showsTheFile(0), 15000);

    // And it stays that way after the caret leaves, which is the complaint this
    // answers: the differently-set patch used to sit there until a save.
    overlay.finishEditing();
    QVERIFY(overlay.showsTheFile(0));
    QVERIFY(overlay.hasEdits());

    // Drawn, with the caret gone, over the very page the view lays out: the
    // corrected line has to be on it.
    QImage canvas(view.viewport()->size(), QImage::Format_RGB32);
    canvas.fill(Qt::white);
    {
        QPainter painter(&canvas);
        overlay.paint(painter, 0, view.pageRect(0));
    }
    const QRect box = view.fromPoints(0, runs.constFirst().rect.normalized()).toAlignedRect();
    int ink = 0;
    for (int y = box.top(); y <= box.bottom(); ++y) {
        for (int x = box.left(); x <= box.right(); ++x) {
            if (canvas.rect().contains(x, y) && qGray(canvas.pixel(x, y)) < 128) {
                ++ink;
            }
        }
    }
    QVERIFY2(ink > 20, qPrintable(QStringLiteral("the corrected line was drawn with %1 dark pixels").arg(ink)));

    // Taking the correction back takes the render with it, or the page would go
    // on showing an edit that no longer exists.
    overlay.discardEdits();
    QVERIFY(!overlay.hasEdits());
    QVERIFY(!overlay.showsTheFile(0));
}

void TestTextWysiwyg::writesTheTypeSomebodyAskedFor()
{
    const QString document
        = writePdf(QStringLiteral("restyle.pdf"), "/Helvetica", "/F1 12 Tf 1 0 0 1 72 200 Tm ", "Kleine Zeile");

    Document held;
    QString error;
    QVERIFY2(held.open(document, &error), qPrintable(error));

    PageView view;
    view.setDocument(&held);
    view.resize(900, 700);
    TextOverlay overlay(&view, &held);

    TextEdit::Format wanted;
    wanted.family = QStringLiteral("Liberation Sans");
    wanted.size = 24.0;
    wanted.colour = QColor(0, 128, 0);
    overlay.setFormat(0, 0, wanted);

    QVERIFY2(overlay.hasEdits(), "type set differently is an unsaved change like any other");
    const QVector<TextEdit::Replacement> batch = overlay.replacements(TextOverlay::Fitting::Grow);
    QCOMPARE(batch.size(), 1);
    QCOMPARE(batch.constFirst().text, QStringLiteral("Kleine Zeile"));
    QCOMPARE(batch.constFirst().format.family, QStringLiteral("Liberation Sans"));

    const QString out = m_dir.filePath(QStringLiteral("restyled.pdf"));
    TextEdit::Report report;
    QVERIFY2(
        TextEdit::apply(document, out, batch, TextOverlay::optionsFor(TextOverlay::Fitting::Grow), &report, &error),
        qPrintable(error));
    QCOMPARE(report.replaced, 1);
    QCOMPARE(report.restyled, 1);
    QVERIFY2(report.refusals.isEmpty(), qPrintable(report.refusals.join(QStringLiteral("; "))));

    // Read back out of the file rather than trusting what was written: the size,
    // the colour and the face all have to have arrived.
    const QVector<TextEdit::Run> after = TextEdit::runsOn(out, 0, &error);
    QCOMPARE(after.size(), 1);
    QCOMPARE(after.constFirst().text, QStringLiteral("Kleine Zeile"));
    QVERIFY2(qAbs(after.constFirst().scaledSize - 24.0) < 0.01,
             qPrintable(QString::number(after.constFirst().scaledSize)));
    QCOMPARE(after.constFirst().colour.green(), 128);
    QVERIFY2(after.constFirst().fontFamily.contains(QStringLiteral("Liberation"), Qt::CaseInsensitive),
             qPrintable(after.constFirst().fontFamily));
    QVERIFY2(after.constFirst().embedded, "the new face has to travel with the document");

    // The page's own state is put back afterwards, or one restyled line would
    // restyle everything drawn after it.
    const QString twoLines
        = writePdf(QStringLiteral("two.pdf"), "/Helvetica",
                   "/F1 12 Tf 1 0 0 1 72 200 Tm (Erste Zeile) Tj 1 0 0 1 72 160 Tm ", "Zweite Zeile");
    TextEdit::Replacement only;
    only.page = 0;
    only.index = 0;
    only.text = QStringLiteral("Erste Zeile");
    only.format = wanted;
    const QString mixed = m_dir.filePath(QStringLiteral("mixed.pdf"));
    QVERIFY2(TextEdit::apply(twoLines, mixed, { only }, {}, nullptr, &error), qPrintable(error));

    const QVector<TextEdit::Run> both = TextEdit::runsOn(mixed, 0, &error);
    QCOMPARE(both.size(), 2);
    QVERIFY2(qAbs(both.at(1).scaledSize - 12.0) < 0.01, qPrintable(QString::number(both.at(1).scaledSize)));
    QCOMPARE(both.at(1).colour, QColor(Qt::black));
    QCOMPARE(both.at(1).fontFamily, QStringLiteral("Helvetica"));
}

void TestTextWysiwyg::refusesAFaceThatCannotTravel()
{
    const QString document
        = writePdf(QStringLiteral("refuse.pdf"), "/Helvetica", "/F1 12 Tf 1 0 0 1 72 200 Tm ", "Eine Zeile");

    Document held;
    QString error;
    QVERIFY2(held.open(document, &error), qPrintable(error));

    PageView view;
    view.setDocument(&held);
    view.resize(900, 700);
    TextOverlay overlay(&view, &held);
    QSignalSpy refused(&overlay, &TextOverlay::refused);

    // A face nobody has. Saying so at the moment it is chosen is the whole
    // point: the alternative is a page of empty boxes found after saving.
    TextEdit::Format impossible;
    impossible.family = QStringLiteral("No Such Typeface At All");
    overlay.setFormat(0, 0, impossible);

    QCOMPARE(refused.size(), 1);
    QVERIFY2(refused.constFirst().constFirst().toString().contains(impossible.family),
             qPrintable(refused.constFirst().constFirst().toString()));
    QVERIFY2(!overlay.hasEdits(), "a face that cannot travel must not be recorded as a change");
}

QTEST_MAIN(TestTextWysiwyg)

#include "tst_textwysiwyg.moc"
