/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#include "core/PdfGeometry.h"

#include <QTest>

#include <qpdf/QPDF.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>

#include <clocale>
#include <cstdlib>

using namespace ps;

/**
 * The two locale rules, in one place, checked from both sides.
 *
 * PDF numbers are written and read in the C locale. Everything that goes
 * through the C library instead (snprintf on the way out, strtod on the way in)
 * silently follows LC_NUMERIC, and on most of Europe's desktops that means a
 * comma. A written number no reader accepts is at least obvious; a number read
 * back as zero is not. An A4 page becomes 595 × 841, a yellow highlight comes
 * out red, and a form's matrix becomes the identity.
 *
 * The suite runs every test twice, once under LC_ALL=C and once under a German
 * locale, and this is the file that exists purely to be run the second time.
 */
class TestPdfGeometry : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void theLocaleUnderTestIsActuallyInEffect();
    void writesNumbersWithAFullStop();
    void readsFractionsBackExactly();
    void readsIntegersAndRefusesTheRest();
    void measuresAnA4PageExactly();
    void mapsDisplayToPageForEveryRotation();
    void writesTheCharactersLatin1Loses();
    void escapesWhatAPdfStringCannotHoldRaw();
};

void TestPdfGeometry::theLocaleUnderTestIsActuallyInEffect()
{
    // The suite runs everything twice, the second time under a comma locale. But
    // glibc falls back to C *silently* when the named locale is not generated on
    // the machine, so the German run would quietly become a second copy of the C
    // run: green, and testing nothing. Worse than a failure, because it looks
    // like coverage.
    const QByteArray requested = qgetenv("LC_ALL");
    if (!requested.startsWith("de_DE")) {
        QSKIP("this case only has something to say under the German locale");
    }

    const char *separator = std::localeconv()->decimal_point;
    QVERIFY2(separator && *separator == ',',
             "LC_ALL asks for de_DE.UTF-8 but the decimal separator is not a comma: the locale is not generated on "
             "this machine, so every German test run is silently a duplicate of the C run. Run "
             "'sudo locale-gen de_DE.UTF-8 && sudo update-locale'.");
}

void TestPdfGeometry::writesNumbersWithAFullStop()
{
    QVERIFY2(PdfGeometry::number(12.34).find(',') == std::string::npos, PdfGeometry::number(12.34).c_str());
    QCOMPARE(PdfGeometry::number(12.34), std::string("12.3400"));
    QCOMPARE(PdfGeometry::number(-0.5), std::string("-0.5000"));
    QCOMPARE(PdfGeometry::number(0.0), std::string("0.0000"));
}

void TestPdfGeometry::readsFractionsBackExactly()
{
    QPDFObjectHandle array = QPDFObjectHandle::parse("[1.0000 0.8314 0.0000 595.2760]");

    // The failing case: everything after the full stop is thrown away when the
    // C library is asked to parse it under a comma locale.
    QCOMPARE(PdfGeometry::numberAt(array, 1, -1.0), 0.8314);
    QCOMPARE(PdfGeometry::numberAt(array, 3, -1.0), 595.276);
    QCOMPARE(PdfGeometry::numberAt(array, 0, -1.0), 1.0);
    QCOMPARE(PdfGeometry::numberAt(array, 2, -1.0), 0.0);

    // Out of range and non-arrays fall back rather than crashing.
    QCOMPARE(PdfGeometry::numberAt(array, 9, -7.0), -7.0);
    QCOMPARE(PdfGeometry::numberAt(QPDFObjectHandle::newNull(), 0, -7.0), -7.0);

    // boxValue insists on exactly four entries, which is what makes it wrong
    // for colours and matrices and right for rectangles.
    QCOMPARE(PdfGeometry::boxValue(array, 3, -1.0), 595.276);
    QCOMPARE(PdfGeometry::boxValue(QPDFObjectHandle::parse("[1 2 3]"), 1, -1.0), -1.0);
}

void TestPdfGeometry::readsIntegersAndRefusesTheRest()
{
    QCOMPARE(PdfGeometry::numericValue(QPDFObjectHandle::newInteger(42), -1.0), 42.0);
    QCOMPARE(PdfGeometry::numericValue(QPDFObjectHandle::parse("-17"), -1.0), -17.0);
    QCOMPARE(PdfGeometry::numericValue(QPDFObjectHandle::newName("/Fm0"), -1.0), -1.0);
    QCOMPARE(PdfGeometry::numericValue(QPDFObjectHandle::newNull(), -1.0), -1.0);
}

void TestPdfGeometry::measuresAnA4PageExactly()
{
    // A4 is the size that exposes this: its dimensions are not whole numbers,
    // and it is what most of the world prints on.
    QPDF pdf;
    pdf.emptyPDF();
    QPDFObjectHandle page = QPDFObjectHandle::parse("<< /Type /Page /MediaBox [0 0 595.276 841.89] >>");
    page.replaceKey("/Resources", QPDFObjectHandle::parse("<< >>"));
    page.replaceKey("/Contents", QPDFObjectHandle::newStream(&pdf, "\n"));

    QPDFPageDocumentHelper documents(pdf);
    documents.addPage(QPDFPageObjectHelper(pdf.makeIndirectObject(page)), false);

    QPDFPageObjectHelper handle = documents.getAllPages().at(0);
    const QRectF media = PdfGeometry::mediaBoxOf(handle);
    QCOMPARE(media.width(), 595.276);
    QCOMPARE(media.height(), 841.89);
}

void TestPdfGeometry::mapsDisplayToPageForEveryRotation()
{
    const double width = 595.276;
    const double height = 841.89;

    // Un-turned, the two spaces are the same.
    QCOMPARE(PdfGeometry::displayToPageTransform(0, width, height).map(QPointF(10, 20)), QPointF(10, 20));

    // Turned a quarter clockwise, the bottom-left of the display is the
    // top-left of the page.
    const QPointF turned = PdfGeometry::displayToPageTransform(90, width, height).map(QPointF(0, 0));
    QCOMPARE(turned, QPointF(width, 0));

    // And every rotation, applied then undone, is the identity: the property
    // that decides whether a comment lands where it was drawn.
    for (int rotate : { 0, 90, 180, 270 }) {
        bool invertible = false;
        const QTransform there = PdfGeometry::displayToPageTransform(rotate, width, height);
        const QTransform back = there.inverted(&invertible);
        QVERIFY(invertible);
        const QPointF point(123.456, 654.321);
        const QPointF returned = back.map(there.map(point));
        QVERIFY2(qAbs(returned.x() - point.x()) < 1e-9 && qAbs(returned.y() - point.y()) < 1e-9,
                 qPrintable(
                     QStringLiteral("rotation %1 came back at %2,%3").arg(rotate).arg(returned.x()).arg(returned.y())));
    }
}

void TestPdfGeometry::writesTheCharactersLatin1Loses()
{
    // The whole reason this encoder exists. Every one of these sits in
    // 0x80 to 0x9F, which WinAnsi fills and Latin-1 leaves empty, so QString's own
    // toLatin1() answers "?" for all of them. It did, in every watermark and
    // every signature stamp this application produced.
    QCOMPARE(PdfGeometry::winAnsi(QStringLiteral("–")), std::string("\x96"));
    QCOMPARE(PdfGeometry::winAnsi(QStringLiteral("—")), std::string("\x97"));
    QCOMPARE(PdfGeometry::winAnsi(QStringLiteral("€")), std::string("\x80"));
    QCOMPARE(PdfGeometry::winAnsi(QStringLiteral("„")), std::string("\x84"));
    QCOMPARE(PdfGeometry::winAnsi(QStringLiteral("“")), std::string("\x93"));
    QCOMPARE(PdfGeometry::winAnsi(QStringLiteral("”")), std::string("\x94"));
    QCOMPARE(PdfGeometry::winAnsi(QStringLiteral("‘")), std::string("\x91"));
    QCOMPARE(PdfGeometry::winAnsi(QStringLiteral("’")), std::string("\x92"));

    // A whole line, as a user would actually type it.
    const std::string stamped = PdfGeometry::winAnsi(QStringLiteral("Entwurf – nicht freigegeben"));
    QVERIFY2(stamped.find('?') == std::string::npos, stamped.c_str());
    QCOMPARE(stamped, std::string("Entwurf \x96 nicht freigegeben"));

    // German text has to survive unharmed too; these are the range where
    // Latin-1 and WinAnsi agree, so they were never the problem.
    // Split across literals on purpose: "\xDFe" is one hex escape as far as the
    // compiler is concerned, and 0xDFE truncated to a byte is 0xFE, so writing
    // it in one piece tests the wrong thing and fails for the wrong reason.
    QCOMPARE(PdfGeometry::winAnsi(QStringLiteral("Grüße, Straße")),
             std::string("Gr\xFC"
                         "\xDF"
                         "e, Stra\xDF"
                         "e"));
}

void TestPdfGeometry::escapesWhatAPdfStringCannotHoldRaw()
{
    QCOMPARE(PdfGeometry::winAnsi(QStringLiteral("(a)")), std::string("\\(a\\)"));
    QCOMPARE(PdfGeometry::winAnsi(QStringLiteral("a\\b")), std::string("a\\\\b"));
    QCOMPARE(PdfGeometry::winAnsiLiteral(QStringLiteral("hi")), std::string("(hi)"));

    // Anything the encoding genuinely cannot carry becomes a question mark
    // rather than disappearing: a visibly wrong character can be reported, a
    // silently shortened line cannot.
    QCOMPARE(PdfGeometry::winAnsi(QStringLiteral("平")), std::string("?"));

    // U+0080 to U+009F are the C1 controls, and WinAnsi keeps printable
    // characters at those byte values. Writing them straight through would turn
    // an invisible control into a euro sign on the page.
    QCOMPARE(PdfGeometry::winAnsi(QString(QChar(0x0085))), std::string("?"));
}

QTEST_MAIN(TestPdfGeometry)

#include "tst_pdfgeometry.moc"
