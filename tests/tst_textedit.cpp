/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#include "TestPdf.h"

#include <qpdf/QPDF.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFWriter.hh>

#include "core/TextEdit.h"
#include "render/PopplerBackend.h"

#include <KLocalizedString>

#include <QColor>
#include <QTemporaryDir>
#include <QTest>

using namespace ps;

/**
 * Changing the words on a page.
 *
 * The tests that matter are the ones that check the *rest* of the page: a typo
 * fix that also nudges the line below it, or that quietly draws a different
 * letter than the one asked for, is worse than no editing at all. So every case
 * reads the result back through a renderer rather than trusting what was written.
 */
class TestTextEdit : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();

    void findsTheTextAndWhereItIs();
    void replacesAWordAndLeavesTheRestAlone();
    void shortensTheLineRatherThanStretchingTheLetters();
    void squeezesTextThatWouldBeTooWide();
    void refusesCharactersTheFontDoesNotHave();
    void carriesOnAfterARefusal();
    void leavesTheOtherPagesUntouched();
    void refusesAnEmptyRequest();
    void readsTheCharactersWinAnsiKeepsWhereLatin1IsEmpty();
    void readsMacRomanAsMacRomanAndNotAsLatin1();
    void reportsItsOwnLimits();

private:
    /** A one-page document showing @p bytes in a font declaring @p encoding. */
    QString writeEncodedPdf(const QString &name, const QByteArray &bytes, const char *encoding);

    QTemporaryDir m_dir;
    QString m_source;
};

void TestTextEdit::initTestCase()
{
    KLocalizedString::setApplicationDomain("pdf-smithy");
    QVERIFY(m_dir.isValid());
    m_source = m_dir.filePath(QStringLiteral("text.pdf"));
    QVERIFY(test::writeTextHeavyPdf(m_source, 3));
}

void TestTextEdit::findsTheTextAndWhereItIs()
{
    QString error;
    const QVector<TextEdit::Run> runs = TextEdit::runsOn(m_source, 0, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));

    // The fixture draws forty lines per page, each its own show operator.
    QCOMPARE(runs.size(), 40);

    const TextEdit::Run &fifth = runs.at(4);
    QCOMPARE(fifth.index, 4);
    QCOMPARE(fifth.page, 0);
    QVERIFY2(fifth.text.startsWith(QStringLiteral("Zeile 5 auf Seite 1")), qPrintable(fifth.text));
    QCOMPARE(fifth.fontResource, QStringLiteral("/F1"));
    QCOMPARE(fifth.fontSize, 13.0);
    QVERIFY2(fifth.editable, qPrintable(fifth.limitation));

    // What the run is set in, and not only how big. Anything drawing this line
    // somewhere else has to start from the face the page names, or it draws the
    // page's words in the desktop's typeface.
    QCOMPARE(fifth.fontFamily, QStringLiteral("Helvetica"));
    QCOMPARE(fifth.fontWeight, 400);
    QCOMPARE(fifth.italic, false);
    QCOMPARE(fifth.embedded, false);
    QCOMPARE(fifth.scaledSize, 13.0);
    QCOMPARE(fifth.colour, QColor(Qt::black));
    QCOMPARE(fifth.horizontalScale, 1.0);
    QCOMPARE(fifth.renderMode, 0);

    // Baseline 720 - 4 * 16 = 656, and the box straddles it.
    QVERIFY2(qAbs(fifth.rect.x() - 72.0) < 1.0, qPrintable(QString::number(fifth.rect.x())));
    QVERIFY2(fifth.rect.y() < 656.0 && fifth.rect.bottom() > 656.0,
             qPrintable(QStringLiteral("box is %1..%2").arg(fifth.rect.y()).arg(fifth.rect.bottom())));
}

void TestTextEdit::replacesAWordAndLeavesTheRestAlone()
{
    const QString out = m_dir.filePath(QStringLiteral("fixed.pdf"));

    TextEdit::Replacement change;
    change.page = 0;
    change.index = 4;
    change.text = QStringLiteral("Zeile 5 wurde geaendert und ist nun anders");

    TextEdit::Report report;
    QString error;
    QVERIFY2(TextEdit::apply(m_source, out, { change }, {}, &report, &error), qPrintable(error));
    QCOMPARE(report.replaced, 1);
    QVERIFY2(report.refusals.isEmpty(), qPrintable(report.refusals.join(QStringLiteral("; "))));

    PopplerBackend backend;
    QVERIFY(backend.addDocument(1, out, nullptr));
    const QString text = backend.extractText(1, 0);

    QVERIFY2(text.contains(QStringLiteral("wurde geaendert")), qPrintable(text.left(300)));
    QVERIFY2(!text.contains(QStringLiteral("Zeile 5 auf Seite 1")), "the old text is still there");

    // Its neighbours are untouched, which is the whole point.
    QVERIFY(text.contains(QStringLiteral("Zeile 4 auf Seite 1")));
    QVERIFY(text.contains(QStringLiteral("Zeile 6 auf Seite 1")));
    QVERIFY(text.contains(QStringLiteral("Zeile 40 auf Seite 1")));
}

void TestTextEdit::shortensTheLineRatherThanStretchingTheLetters()
{
    const QString out = m_dir.filePath(QStringLiteral("shorter.pdf"));

    // Much shorter than what it replaces. Stretching it back out to the old
    // width would give letters four times their proper width, which fools
    // nobody; a shorter line is simply shorter.
    TextEdit::Replacement change;
    change.page = 0;
    change.index = 0;
    change.text = QStringLiteral("Kurz");

    TextEdit::Report report;
    QString error;
    QVERIFY2(TextEdit::apply(m_source, out, { change }, {}, &report, &error), qPrintable(error));
    QCOMPARE(report.replaced, 1);
    QCOMPARE(report.widthAdjustment, 0.0);
    QVERIFY(report.overflowed.isEmpty());

    const QVector<TextEdit::Run> before = TextEdit::runsOn(m_source, 0, &error);
    const QVector<TextEdit::Run> after = TextEdit::runsOn(out, 0, &error);
    QCOMPARE(after.size(), 40);
    QCOMPARE(after.constFirst().text, QStringLiteral("Kurz"));
    QVERIFY2(after.constFirst().rect.width() < before.constFirst().rect.width() / 2.0,
             qPrintable(QStringLiteral("width went from %1 to %2")
                            .arg(before.constFirst().rect.width())
                            .arg(after.constFirst().rect.width())));

    // And the letters are the same size as everywhere else on the page: no Tz
    // was emitted at all.
    QVERIFY2(!test::contentOf(out, 0).contains(QStringLiteral(" Tz")),
             "a shorter replacement should not scale the type");

    // Nothing on another line moved a point.
    QVERIFY2(
        qAbs(after.at(1).rect.y() - before.at(1).rect.y()) < 0.01,
        qPrintable(
            QStringLiteral("the next line moved from %1 to %2").arg(before.at(1).rect.y()).arg(after.at(1).rect.y())));

    PopplerBackend backend;
    QVERIFY(backend.addDocument(1, out, nullptr));
    QVERIFY2(backend.extractText(1, 0).contains(QStringLiteral("Kurz")),
             qPrintable(backend.extractText(1, 0).left(120)));
}

void TestTextEdit::squeezesTextThatWouldBeTooWide()
{
    const QString out = m_dir.filePath(QStringLiteral("wider.pdf"));

    // Half again as long as the line it replaces. Left alone it would run into
    // the margin, so it is squeezed, and the line keeps the width it had.
    TextEdit::Replacement change;
    change.page = 0;
    change.index = 0;
    change.text = QStringLiteral("Zeile 1 auf Seite 1 mit deutlich mehr Text als vorher darin stand");

    TextEdit::Report report;
    QString error;
    QVERIFY2(TextEdit::apply(m_source, out, { change }, {}, &report, &error), qPrintable(error));
    QCOMPARE(report.replaced, 1);
    QVERIFY2(report.widthAdjustment > 5.0,
             qPrintable(QStringLiteral("expected a squeeze, got %1%").arg(report.widthAdjustment)));

    const QVector<TextEdit::Run> before = TextEdit::runsOn(m_source, 0, &error);
    const QVector<TextEdit::Run> after = TextEdit::runsOn(out, 0, &error);
    QVERIFY2(qAbs(after.constFirst().rect.width() - before.constFirst().rect.width()) < 1.5,
             qPrintable(QStringLiteral("width went from %1 to %2")
                            .arg(before.constFirst().rect.width())
                            .arg(after.constFirst().rect.width())));

    // The squeeze is put back afterwards, or every later line on the page would
    // inherit it.
    QVERIFY(qAbs(after.at(1).rect.width() - before.at(1).rect.width()) < 0.5);

    // And there is a floor: a replacement ten times too long is reported as
    // overflowing rather than squeezed into a smear.
    TextEdit::Replacement absurd;
    absurd.page = 0;
    absurd.index = 1;
    absurd.text = QString(400, QLatin1Char('W'));
    TextEdit::Report second;
    QVERIFY2(TextEdit::apply(m_source, m_dir.filePath(QStringLiteral("absurd.pdf")), { absurd }, {}, &second, &error),
             qPrintable(error));
    QCOMPARE(second.replaced, 1);
    QVERIFY2(!second.overflowed.isEmpty(), "a replacement far too long was squeezed silently");
    QVERIFY2(second.widthAdjustment <= 40.5,
             qPrintable(QStringLiteral("squeezed to %1%, past the floor").arg(second.widthAdjustment)));
}

void TestTextEdit::refusesCharactersTheFontDoesNotHave()
{
    const QString out = m_dir.filePath(QStringLiteral("refused.pdf"));

    // The fixture's font is base-14 Helvetica with no /Widths and no /ToUnicode,
    // so printable ASCII is all it can be trusted with. A Cyrillic word has no
    // code in it, and drawing something else instead would be a silent lie.
    TextEdit::Replacement change;
    change.page = 0;
    change.index = 2;
    change.text = QStringLiteral("Привет");

    TextEdit::Report report;
    QString error;
    QVERIFY2(TextEdit::apply(m_source, out, { change }, {}, &report, &error), qPrintable(error));
    QCOMPARE(report.replaced, 0);
    QCOMPARE(report.refusals.size(), 1);
    QVERIFY2(!report.refusals.constFirst().isEmpty(), "the refusal came with no explanation");

    // And the original text is still exactly there.
    PopplerBackend backend;
    QVERIFY(backend.addDocument(1, out, nullptr));
    QVERIFY(backend.extractText(1, 0).contains(QStringLiteral("Zeile 3 auf Seite 1")));
}

void TestTextEdit::carriesOnAfterARefusal()
{
    const QString out = m_dir.filePath(QStringLiteral("partial.pdf"));

    // Fixing four typos must not fall over because the fifth needs a character
    // the font has never seen.
    QVector<TextEdit::Replacement> changes;
    for (int i = 0; i < 3; ++i) {
        TextEdit::Replacement good;
        good.page = 0;
        good.index = i;
        good.text = QStringLiteral("Ersetzt %1").arg(i);
        changes.append(good);
    }
    TextEdit::Replacement bad;
    bad.page = 0;
    bad.index = 3;
    bad.text = QStringLiteral("日本語");
    changes.append(bad);

    TextEdit::Report report;
    QString error;
    QVERIFY2(TextEdit::apply(m_source, out, changes, {}, &report, &error), qPrintable(error));
    QCOMPARE(report.replaced, 3);
    QCOMPARE(report.refusals.size(), 1);

    PopplerBackend backend;
    QVERIFY(backend.addDocument(1, out, nullptr));
    const QString text = backend.extractText(1, 0);
    QVERIFY2(text.contains(QStringLiteral("Ersetzt 0")), qPrintable(text.left(200)));
    QVERIFY2(text.contains(QStringLiteral("Ersetzt 2")), qPrintable(text.left(200)));
    QVERIFY(text.contains(QStringLiteral("Zeile 4 auf Seite 1")));
}

void TestTextEdit::leavesTheOtherPagesUntouched()
{
    const QString out = m_dir.filePath(QStringLiteral("onepage.pdf"));

    TextEdit::Replacement change;
    change.page = 1;
    change.index = 0;
    change.text = QStringLiteral("Nur diese Zeile");

    TextEdit::Report report;
    QString error;
    QVERIFY2(TextEdit::apply(m_source, out, { change }, {}, &report, &error), qPrintable(error));
    QCOMPARE(report.replaced, 1);

    PopplerBackend backend;
    QVERIFY(backend.addDocument(1, out, nullptr));
    QVERIFY(backend.extractText(1, 1).contains(QStringLiteral("Nur diese Zeile")));
    QVERIFY(backend.extractText(1, 0).contains(QStringLiteral("Zeile 1 auf Seite 1")));
    QVERIFY(backend.extractText(1, 2).contains(QStringLiteral("Zeile 1 auf Seite 3")));

    // Untouched pages keep their content stream byte for byte.
    QCOMPARE(test::contentOf(out, 2), test::contentOf(m_source, 2));
}

void TestTextEdit::refusesAnEmptyRequest()
{
    QString error;
    QVERIFY(!TextEdit::apply(m_source, m_dir.filePath(QStringLiteral("nothing.pdf")), {}, {}, nullptr, &error));
    QVERIFY(!error.isEmpty());

    TextEdit::Replacement offPage;
    offPage.page = 99;
    offPage.index = 0;
    offPage.text = QStringLiteral("x");
    QVERIFY(
        !TextEdit::apply(m_source, m_dir.filePath(QStringLiteral("nothing2.pdf")), { offPage }, {}, nullptr, &error));
    QVERIFY(!error.isEmpty());

    QVERIFY(TextEdit::runsOn(m_source, 99, &error).isEmpty());
    QVERIFY(!error.isEmpty());
}

QString TestTextEdit::writeEncodedPdf(const QString &name, const QByteArray &bytes, const char *encoding)
{
    const QString path = m_dir.filePath(name);

    QPDF pdf;
    pdf.emptyPDF();

    QPDFObjectHandle font = pdf.makeIndirectObject(QPDFObjectHandle::parse(
        std::string("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding ") + encoding + " >>"));

    QPDFObjectHandle fonts = QPDFObjectHandle::newDictionary();
    fonts.replaceKey("/F1", font);
    QPDFObjectHandle resources = QPDFObjectHandle::newDictionary();
    resources.replaceKey("/Font", fonts);

    QPDFObjectHandle box = QPDFObjectHandle::newArray();
    for (const int edge : { 0, 0, 400, 200 }) {
        box.appendItem(QPDFObjectHandle::newInteger(edge));
    }

    // The bytes go in raw: the point of the case is what a code *means* to the
    // reader, so writing them through anything that already knows the encoding
    // would test that thing instead.
    std::string content = "BT /F1 14 Tf 20 100 Td (";
    content.append(bytes.constData(), size_t(bytes.size()));
    content += ") Tj ET\n";

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

void TestTextEdit::readsTheCharactersWinAnsiKeepsWhereLatin1IsEmpty()
{
    // 0x80 to 0x9F is the range Latin-1 leaves empty and WinAnsi fills, and what it
    // fills it with is exactly what a word processor emits without being asked:
    // the euro sign, both dashes, the ellipsis and all four curly quotes. Read as
    // Latin-1 they are C1 control codes, so every one of them came back as a
    // replacement character, not only in extracted text but in search and in
    // redaction, where a phrase that fails to match is not a cosmetic problem.
    // Split at every escape: "\x93E" is one hex sequence to the compiler, and
    // 0x93E truncated to a byte is '>', so writing it in one piece would test
    // something else entirely and fail for a reason that has nothing to do with
    // encodings.
    const QByteArray bytes("Angebot \x96 \x93"
                           "Entwurf\x94 \x85 \x80"
                           "100");
    const QString document = writeEncodedPdf(QStringLiteral("winansi.pdf"), bytes, "/WinAnsiEncoding");

    QString error;
    const QVector<TextEdit::Run> runs = TextEdit::runsOn(document, 0, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(runs.size(), 1);

    QCOMPARE(runs.first().text, QStringLiteral("Angebot – “Entwurf” … €100"));
}

void TestTextEdit::readsMacRomanAsMacRomanAndNotAsLatin1()
{
    // MacRoman disagrees with Latin-1 across the whole upper half rather than in
    // one gap, so the failure is louder: 0x9F is ü on a Mac and Ÿ in Latin-1, and
    // 0xA7 is ß rather than the section sign.
    const QByteArray bytes("Gr\x9F"
                           "\xA7"
                           "e aus M\x9F"
                           "nchen");
    const QString document = writeEncodedPdf(QStringLiteral("macroman.pdf"), bytes, "/MacRomanEncoding");

    QString error;
    const QVector<TextEdit::Run> runs = TextEdit::runsOn(document, 0, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(runs.size(), 1);

    QCOMPARE(runs.first().text, QStringLiteral("Grüße aus München"));
}

void TestTextEdit::reportsItsOwnLimits()
{
    // Text editing is the feature most likely to be misunderstood, so what it
    // will not do is part of the API rather than a footnote.
    const QStringList limits = TextEdit::limitations();
    QVERIFY(limits.size() >= 3);
    for (const QString &limit : limits) {
        QVERIFY(!limit.trimmed().isEmpty());
    }
}

QTEST_MAIN(TestTextEdit)

#include "tst_textedit.moc"
