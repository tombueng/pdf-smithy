/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#include "core/PdfImage.h"
#include "core/Redaction.h"
#include "render/PopplerBackend.h"

#include <QImage>
#include <QPainter>
#include <QTemporaryDir>
#include <QTest>

#include <qpdf/Pl_Buffer.hh>
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFWriter.hh>

using namespace ps;

/**
 * The one test that decides whether this feature is worth having.
 *
 * A black rectangle is easy; every one of these cases asks the harder question
 * instead: is the text still in the file? If any of them ever passes because
 * the box was drawn rather than because the content went, the feature is a lie
 * and should be removed.
 */
class TestRedaction : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();

    void removesTextFromTheFile();
    void keepsTheRestOfTheLine();
    void survivesAnUnembeddedStandardFont();
    void leavesOtherPagesAlone();
    void followsPageRotation();
    void editsThePixelsOfAnOverlappingImage();
    void editsAJpegWithoutDroppingIt();
    void flattensPagesItCannotEdit();
    void seesThroughAFormMatrix();
    void dropsAnnotationsUnderTheBox();
    void refusesWithNothingMarked();
    void reportsItsOwnLimits();

private:
    /**
     * A page of plain text at known places.
     *
     * @p withWidths controls whether the font dictionary carries /Widths.
     * Leaving them out is legal for the standard fourteen and is exactly the
     * case where positions have to come from built-in metrics.
     */
    QString writeTextPdf(const QString &name, bool withWidths = true, int rotate = 0, int pages = 1);

    QString extractText(const QString &path, int page);

    QTemporaryDir m_dir;
};

namespace {

// Helvetica, in thousandths of the font size. Only what the fixture needs.
int helveticaWidth(char letter)
{
    switch (letter) {
    case ' ':
        return 278;
    case 'I':
        return 278;
    case 'L':
        return 556;
    case 'E':
    case 'S':
    case 'A':
    case 'B':
    case 'K':
    case 'P':
    case 'X':
    case 'H':
    case 'N':
    case 'R':
    case 'U':
    case 'V':
        return 667;
    case 'C':
    case 'D':
    case 'G':
    case 'O':
    case 'Q':
        return 722;
    case 'M':
    case 'W':
        return 833;
    case 'F':
    case 'T':
    case 'Y':
    case 'Z':
    case 'J':
        return 611;
    default:
        return 667;
    }
}

double textWidth(const QString &text, double fontSize)
{
    double total = 0;
    for (const QChar &character : text) {
        total += helveticaWidth(character.toLatin1()) / 1000.0 * fontSize;
    }
    return total;
}

} // namespace

void TestRedaction::initTestCase()
{
    QVERIFY(m_dir.isValid());
}

QString TestRedaction::writeTextPdf(const QString &name, bool withWidths, int rotate, int pages)
{
    const QString path = m_dir.filePath(name);

    QPDF pdf;
    pdf.emptyPDF();

    QPDFObjectHandle font = pdf.makeIndirectObject(
        QPDFObjectHandle::parse("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>"));
    if (withWidths) {
        // 224 entries starting at code 32, the standard Helvetica advances for
        // the letters this fixture uses and a plausible value for the rest.
        std::string widths = "[";
        for (int code = 32; code <= 255; ++code) {
            widths += " " + std::to_string(helveticaWidth(char(code)));
        }
        widths += " ]";
        font.replaceKey("/FirstChar", QPDFObjectHandle::newInteger(32));
        font.replaceKey("/LastChar", QPDFObjectHandle::newInteger(255));
        font.replaceKey("/Widths", QPDFObjectHandle::parse(widths));
    }

    QPDFObjectHandle resources = QPDFObjectHandle::parse("<< /Font << >> >>");
    resources.getKey("/Font").replaceKey("/F1", font);

    QPDFPageDocumentHelper documents(pdf);
    for (int page = 0; page < pages; ++page) {
        const std::string marker = std::to_string(page + 1);
        const std::string content = "BT /F1 12 Tf 72 700 Td (PUBLIC ALPHA BRAVO) Tj ET\n"
                                    "BT /F1 12 Tf 72 680 Td (SECRET CHARLIE DELTA) Tj ET\n"
                                    "BT /F1 12 Tf 72 660 Td (PUBLIC ECHO PAGE "
            + marker + ") Tj ET\n";

        QPDFObjectHandle object = QPDFObjectHandle::parse("<< /Type /Page /MediaBox [0 0 612 792] >>");
        object.replaceKey("/Resources", resources);
        object.replaceKey("/Contents", QPDFObjectHandle::newStream(&pdf, content));
        if (rotate != 0) {
            object.replaceKey("/Rotate", QPDFObjectHandle::newInteger(rotate));
        }
        documents.addPage(QPDFPageObjectHelper(pdf.makeIndirectObject(object)), false);
    }

    QPDFWriter writer(pdf, path.toUtf8().constData());
    writer.write();
    return path;
}

QString TestRedaction::extractText(const QString &path, int page)
{
    PopplerBackend backend;
    if (!backend.addDocument(1, path, nullptr)) {
        return {};
    }
    return backend.extractText(1, page);
}

void TestRedaction::removesTextFromTheFile()
{
    const QString source = writeTextPdf(QStringLiteral("plain.pdf"));
    const QString out = m_dir.filePath(QStringLiteral("plain-redacted.pdf"));

    QVERIFY(extractText(source, 0).contains(QStringLiteral("SECRET")));

    // A box around the first word of the middle line, and nothing else.
    Redaction::Area area;
    area.page = 0;
    area.rect = QRectF(70, 674, textWidth(QStringLiteral("SECRET"), 12) + 2, 20);

    Redaction::Report report;
    QString error;
    QVERIFY2(Redaction::apply(source, out, { area }, {}, &report, &error), qPrintable(error));
    QCOMPARE(report.areasApplied, 1);
    QVERIFY(report.glyphsRemoved >= 6);

    // The whole point: not merely invisible, but absent.
    const QString extracted = extractText(out, 0);
    QVERIFY2(!extracted.contains(QStringLiteral("SECRET")),
             qPrintable(QStringLiteral("the redacted word is still in the file: %1").arg(extracted)));
}

void TestRedaction::keepsTheRestOfTheLine()
{
    const QString source = writeTextPdf(QStringLiteral("line.pdf"));
    const QString out = m_dir.filePath(QStringLiteral("line-redacted.pdf"));

    Redaction::Area area;
    area.page = 0;
    area.rect = QRectF(70, 674, textWidth(QStringLiteral("SECRET"), 12) + 2, 20);

    QString error;
    QVERIFY2(Redaction::apply(source, out, { area }, {}, nullptr, &error), qPrintable(error));

    // Dropping the operator would have been far easier and would have taken
    // these with it.
    const QString extracted = extractText(out, 0);
    QVERIFY2(extracted.contains(QStringLiteral("CHARLIE")), qPrintable(extracted));
    QVERIFY2(extracted.contains(QStringLiteral("DELTA")), qPrintable(extracted));
    QVERIFY2(extracted.contains(QStringLiteral("ALPHA")), qPrintable(extracted));
    QVERIFY2(extracted.contains(QStringLiteral("ECHO")), qPrintable(extracted));
}

void TestRedaction::survivesAnUnembeddedStandardFont()
{
    // No /Widths at all, which the standard fourteen are allowed to omit. If
    // the built-in metrics were missing, every glyph after the first would be
    // measured in the wrong place and the wrong words would go.
    const QString source = writeTextPdf(QStringLiteral("nowidths.pdf"), false);
    const QString out = m_dir.filePath(QStringLiteral("nowidths-redacted.pdf"));

    Redaction::Area area;
    area.page = 0;
    area.rect = QRectF(70, 674, textWidth(QStringLiteral("SECRET"), 12) + 2, 20);

    QString error;
    QVERIFY2(Redaction::apply(source, out, { area }, {}, nullptr, &error), qPrintable(error));

    const QString extracted = extractText(out, 0);
    QVERIFY2(!extracted.contains(QStringLiteral("SECRET")), qPrintable(extracted));
    QVERIFY2(extracted.contains(QStringLiteral("CHARLIE")), qPrintable(extracted));
}

void TestRedaction::leavesOtherPagesAlone()
{
    const QString source = writeTextPdf(QStringLiteral("multi.pdf"), true, 0, 3);
    const QString out = m_dir.filePath(QStringLiteral("multi-redacted.pdf"));

    Redaction::Area area;
    area.page = 1;
    area.rect = QRectF(70, 674, textWidth(QStringLiteral("SECRET"), 12) + 2, 20);

    QString error;
    QVERIFY2(Redaction::apply(source, out, { area }, {}, nullptr, &error), qPrintable(error));

    QVERIFY(!extractText(out, 1).contains(QStringLiteral("SECRET")));
    QVERIFY(extractText(out, 0).contains(QStringLiteral("SECRET")));
    QVERIFY(extractText(out, 2).contains(QStringLiteral("SECRET")));
}

void TestRedaction::followsPageRotation()
{
    // Areas are measured on the page as shown, so on a turned page they have to
    // be mapped back before they mean anything to the content stream.
    const QString source = writeTextPdf(QStringLiteral("rotated.pdf"), true, 90);
    const QString out = m_dir.filePath(QStringLiteral("rotated-redacted.pdf"));

    // Page space (72, 680) with /Rotate 90 shows up at display (680, 612 - 72).
    const double width = textWidth(QStringLiteral("SECRET"), 12) + 2;
    Redaction::Area area;
    area.page = 0;
    area.rect = QRectF(674, 612 - 72 - width, 20, width);

    QString error;
    QVERIFY2(Redaction::apply(source, out, { area }, {}, nullptr, &error), qPrintable(error));

    const QString extracted = extractText(out, 0);
    QVERIFY2(!extracted.contains(QStringLiteral("SECRET")), qPrintable(extracted));
    QVERIFY2(extracted.contains(QStringLiteral("CHARLIE")), qPrintable(extracted));
}

void TestRedaction::editsThePixelsOfAnOverlappingImage()
{
    const QString path = m_dir.filePath(QStringLiteral("picture.pdf"));

    // Half red, half green. The red half gets redacted, so afterwards no red
    // may remain anywhere in the file's pixel data.
    QImage picture(200, 100, QImage::Format_RGB32);
    picture.fill(Qt::green);
    QPainter painter(&picture);
    painter.fillRect(0, 0, 100, 100, Qt::red);
    painter.end();

    {
        QPDF pdf;
        pdf.emptyPDF();
        QPDFObjectHandle image = PdfImage::embed(pdf, picture);
        QVERIFY(image.isStream());

        QPDFObjectHandle resources = QPDFObjectHandle::parse("<< /XObject << >> >>");
        resources.getKey("/XObject").replaceKey("/Im0", image);

        QPDFObjectHandle object = QPDFObjectHandle::parse("<< /Type /Page /MediaBox [0 0 400 400] >>");
        object.replaceKey("/Resources", resources);
        object.replaceKey("/Contents", QPDFObjectHandle::newStream(&pdf, "q 200 0 0 100 100 200 cm /Im0 Do Q\n"));

        QPDFPageDocumentHelper documents(pdf);
        documents.addPage(QPDFPageObjectHelper(pdf.makeIndirectObject(object)), false);
        QPDFWriter writer(pdf, path.toUtf8().constData());
        writer.write();
    }

    const QString out = m_dir.filePath(QStringLiteral("picture-redacted.pdf"));
    Redaction::Area area;
    area.page = 0;
    area.rect = QRectF(100, 200, 100, 100); // exactly the red half

    Redaction::Report report;
    QString error;
    QVERIFY2(Redaction::apply(path, out, { area }, {}, &report, &error), qPrintable(error));
    QCOMPARE(report.imagesEdited, 1);
    QCOMPARE(report.imagesRemoved, 0);
    QVERIFY(report.pagesRasterised.isEmpty());

    // Look at the stored pixels rather than at the rendered page, because a
    // black box on top would make a rendering look right either way.
    QPDF check;
    check.processFile(out.toUtf8().constData());
    bool sawRed = false;
    bool sawGreen = false;
    for (QPDFObjectHandle object : check.getAllObjects()) {
        if (!object.isStream() || object.getDict().getKey("/Subtype").isName() == false
            || object.getDict().getKey("/Subtype").getName() != "/Image") {
            continue;
        }
        Pl_Buffer buffer("pixels");
        bool attempted = false;
        if (!object.pipeStreamData(&buffer, &attempted, 0, qpdf_dl_all, true, false)) {
            continue;
        }
        const auto data = buffer.getBufferSharedPointer();
        const uchar *bytes = data->getBuffer();
        for (size_t i = 0; i + 2 < data->getSize(); i += 3) {
            sawRed = sawRed || (bytes[i] > 200 && bytes[i + 1] < 60 && bytes[i + 2] < 60);
            sawGreen = sawGreen || (bytes[i] < 60 && bytes[i + 1] > 130 && bytes[i + 2] < 60);
        }
    }
    QVERIFY2(!sawRed, "the redacted half of the picture is still in the file");
    QVERIFY2(sawGreen, "the half that was not marked was destroyed as well");
}

void TestRedaction::editsAJpegWithoutDroppingIt()
{
    // Scans are JPEGs, so this is the case that decides whether redacting a
    // scanned page keeps the page. It once did not: the shorter overload of
    // pipeStreamData reports whether filtering was attempted rather than
    // whether it worked, and a JPEG deliberately left alone reads as failure.
    const QString path = m_dir.filePath(QStringLiteral("photo.pdf"));

    QImage picture(240, 120, QImage::Format_RGB32);
    picture.fill(Qt::white);
    QPainter painter(&picture);
    painter.fillRect(0, 0, 120, 120, QColor(20, 40, 200));
    painter.end();

    {
        QPDF pdf;
        pdf.emptyPDF();
        QPDFObjectHandle image = PdfImage::embedAsJpeg(pdf, picture, 90);
        QVERIFY(image.isStream());
        QCOMPARE(image.getDict().getKey("/Filter").getName(), std::string("/DCTDecode"));

        QPDFObjectHandle resources = QPDFObjectHandle::parse("<< /XObject << >> >>");
        resources.getKey("/XObject").replaceKey("/Im0", image);

        QPDFObjectHandle object = QPDFObjectHandle::parse("<< /Type /Page /MediaBox [0 0 400 400] >>");
        object.replaceKey("/Resources", resources);
        object.replaceKey("/Contents", QPDFObjectHandle::newStream(&pdf, "q 240 0 0 120 80 200 cm /Im0 Do Q\n"));

        QPDFPageDocumentHelper documents(pdf);
        documents.addPage(QPDFPageObjectHelper(pdf.makeIndirectObject(object)), false);
        QPDFWriter writer(pdf, path.toUtf8().constData());
        writer.write();
    }

    const QString out = m_dir.filePath(QStringLiteral("photo-redacted.pdf"));
    Redaction::Area area;
    area.page = 0;
    area.rect = QRectF(80, 200, 120, 120); // the blue half

    Redaction::Report report;
    QString error;
    QVERIFY2(Redaction::apply(path, out, { area }, {}, &report, &error), qPrintable(error));
    QCOMPARE(report.imagesEdited, 1);
    QCOMPARE(report.imagesRemoved, 0);
    QVERIFY(report.warnings.isEmpty());

    // The picture is still on the page, and the half that was not marked still
    // shows. Removing it would have been an easy way to pass everything above.
    PopplerBackend backend;
    QVERIFY(backend.addDocument(1, out, nullptr));
    const QImage rendered = backend.renderPage(1, 0, 400);
    QVERIFY(!rendered.isNull());
    QCOMPARE(rendered.pixelColor(280, 400 - 260).rgb() & 0xf0f0f0, QColor(Qt::white).rgb() & 0xf0f0f0);
    QVERIFY2(qGray(rendered.pixelColor(140, 400 - 260).rgb()) < 60, "the marked half is not blacked out");
}

void TestRedaction::seesThroughAFormMatrix()
{
    // A drawing whose /BBox sits at the origin and whose /Matrix moves it into
    // the marked area. Ignoring the matrix, which reading it with a helper
    // that insists on four entries quietly does, leaves it in place, and an
    // area judged clear is an area not redacted.
    const QString path = m_dir.filePath(QStringLiteral("form.pdf"));

    {
        QPDF pdf;
        pdf.emptyPDF();

        QPDFObjectHandle form = QPDFObjectHandle::newStream(&pdf, "1 0 0 rg 0 0 40 20 re f\n");
        QPDFObjectHandle dict = QPDFObjectHandle::parse("<< /Type /XObject /Subtype /Form /BBox [0 0 40 20]"
                                                        " /Matrix [1 0 0 1 300 500] /Resources << >> >>");
        form.replaceDict(dict);

        QPDFObjectHandle resources = QPDFObjectHandle::parse("<< /XObject << >> >>");
        resources.getKey("/XObject").replaceKey("/Fm0", pdf.makeIndirectObject(form));

        QPDFObjectHandle object = QPDFObjectHandle::parse("<< /Type /Page /MediaBox [0 0 612 792] >>");
        object.replaceKey("/Resources", resources);
        object.replaceKey("/Contents", QPDFObjectHandle::newStream(&pdf, "q /Fm0 Do Q\n"));

        QPDFPageDocumentHelper documents(pdf);
        documents.addPage(QPDFPageObjectHelper(pdf.makeIndirectObject(object)), false);
        QPDFWriter writer(pdf, path.toUtf8().constData());
        writer.write();
    }

    // Where the matrix actually puts it, not where its /BBox claims.
    Redaction::Area area;
    area.page = 0;
    area.rect = QRectF(295, 495, 60, 40);

    Redaction::Report report;
    QString error;
    QVERIFY2(Redaction::apply(path, m_dir.filePath(QStringLiteral("form-redacted.pdf")), { area }, {}, &report, &error),
             qPrintable(error));
    QCOMPARE(report.formsRemoved, 1);

    // And a drawing nowhere near the area is left alone.
    Redaction::Area elsewhere;
    elsewhere.page = 0;
    elsewhere.rect = QRectF(50, 50, 60, 40);

    Redaction::Report second;
    QVERIFY2(
        Redaction::apply(path, m_dir.filePath(QStringLiteral("form-kept.pdf")), { elsewhere }, {}, &second, &error),
        qPrintable(error));
    QCOMPARE(second.formsRemoved, 0);
}

void TestRedaction::flattensPagesItCannotEdit()
{
    const QString path = m_dir.filePath(QStringLiteral("fax.pdf"));

    // A picture claiming a filter nothing here can decode. Without a rasteriser
    // it must be removed rather than left; with one, the page is flattened.
    {
        QPDF pdf;
        pdf.emptyPDF();
        QPDFObjectHandle image = QPDFObjectHandle::newStream(&pdf, std::string(64, '\0'));
        image.replaceDict(QPDFObjectHandle::parse("<< /Type /XObject /Subtype /Image /Width 16 /Height 16"
                                                  " /ColorSpace /DeviceGray /BitsPerComponent 1"
                                                  " /Filter /JBIG2Decode >>"));

        QPDFObjectHandle resources = QPDFObjectHandle::parse("<< /XObject << >> >>");
        resources.getKey("/XObject").replaceKey("/Im0", image);

        QPDFObjectHandle object = QPDFObjectHandle::parse("<< /Type /Page /MediaBox [0 0 400 400] >>");
        object.replaceKey("/Resources", resources);
        object.replaceKey("/Contents", QPDFObjectHandle::newStream(&pdf, "q 400 0 0 400 0 0 cm /Im0 Do Q\n"));

        QPDFPageDocumentHelper documents(pdf);
        documents.addPage(QPDFPageObjectHelper(pdf.makeIndirectObject(object)), false);
        QPDFWriter writer(pdf, path.toUtf8().constData());
        writer.write();
    }

    Redaction::Area area;
    area.page = 0;
    area.rect = QRectF(50, 50, 100, 100);

    // No rasteriser: it says so instead of pretending.
    {
        Redaction::Report report;
        QString error;
        QVERIFY2(
            Redaction::apply(path, m_dir.filePath(QStringLiteral("fax-dropped.pdf")), { area }, {}, &report, &error),
            qPrintable(error));
        QCOMPARE(report.imagesRemoved, 1);
        QVERIFY(!report.warnings.isEmpty());
    }

    // With one, the page becomes a picture of itself and keeps its appearance.
    Redaction::Options options;
    options.pageRasteriser = [](int, double) {
        QImage rendered(400, 400, QImage::Format_RGB32);
        rendered.fill(Qt::white);
        return rendered;
    };

    Redaction::Report report;
    QString error;
    const QString out = m_dir.filePath(QStringLiteral("fax-flat.pdf"));
    QVERIFY2(Redaction::apply(path, out, { area }, options, &report, &error), qPrintable(error));
    QCOMPARE(report.pagesRasterised, QVector<int> { 0 });

    QPDF check;
    check.processFile(out.toUtf8().constData());
    QPDFPageDocumentHelper documents(check);
    QCOMPARE(int(documents.getAllPages().size()), 1);
}

void TestRedaction::dropsAnnotationsUnderTheBox()
{
    const QString path = m_dir.filePath(QStringLiteral("annots.pdf"));

    {
        QPDF pdf;
        pdf.emptyPDF();
        QPDFObjectHandle object = QPDFObjectHandle::parse("<< /Type /Page /MediaBox [0 0 612 792] >>");
        object.replaceKey("/Resources", QPDFObjectHandle::parse("<< >>"));
        object.replaceKey("/Contents", QPDFObjectHandle::newStream(&pdf, "\n"));
        object.replaceKey("/Annots",
                          QPDFObjectHandle::parse("[ << /Type /Annot /Subtype /Text /Rect [80 680 120 700]"
                                                  "     /Contents (a note nobody should read) >>"
                                                  "  << /Type /Annot /Subtype /Text /Rect [80 100 120 120]"
                                                  "     /Contents (harmless) >> ]"));
        QPDFPageDocumentHelper documents(pdf);
        documents.addPage(QPDFPageObjectHelper(pdf.makeIndirectObject(object)), false);
        QPDFWriter writer(pdf, path.toUtf8().constData());
        writer.write();
    }

    const QString out = m_dir.filePath(QStringLiteral("annots-redacted.pdf"));
    Redaction::Area area;
    area.page = 0;
    area.rect = QRectF(70, 674, 80, 40);

    Redaction::Report report;
    QString error;
    QVERIFY2(Redaction::apply(path, out, { area }, {}, &report, &error), qPrintable(error));
    QCOMPARE(report.annotationsRemoved, 1);

    QPDF check;
    check.processFile(out.toUtf8().constData());
    QPDFPageDocumentHelper documents(check);
    QPDFObjectHandle annotations = documents.getAllPages().at(0).getObjectHandle().getKey("/Annots");
    QCOMPARE(annotations.getArrayNItems(), 1);
    QVERIFY(annotations.getArrayItem(0).getKey("/Contents").getUTF8Value() == "harmless");
}

void TestRedaction::refusesWithNothingMarked()
{
    QString error;
    QVERIFY(!Redaction::apply(writeTextPdf(QStringLiteral("nothing.pdf")),
                              m_dir.filePath(QStringLiteral("nothing-out.pdf")), {}, {}, nullptr, &error));
    QVERIFY(!error.isEmpty());
}

void TestRedaction::reportsItsOwnLimits()
{
    // A tool that quietly does less than the user believes is the failure mode
    // this whole feature exists to avoid, so the caveats are part of the API.
    const QStringList limits = Redaction::limitations();
    QVERIFY(limits.size() >= 4);
    for (const QString &limit : limits) {
        QVERIFY(!limit.trimmed().isEmpty());
    }
}

QTEST_MAIN(TestRedaction)

#include "tst_redaction.moc"
