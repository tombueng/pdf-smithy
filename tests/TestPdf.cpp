/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#include "TestPdf.h"

#include <QFile>
#include <QProcess>
#include <QStandardPaths>

#include <cmath>
#include <cstdio>

#include <qpdf/Buffer.hh>
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>
#include <qpdf/QPDFWriter.hh>

#include <map>

namespace ps::test {

namespace {

QPDFObjectHandle makePage(QPDF &pdf, int number, const QSizeF &sizePoints, int rotate)
{
    // A recognisable marker per page: the round-trip tests assert that page 7
    // is still page 7 after being shuffled, deleted around and written out.
    const std::string text = "BT /F1 24 Tf 72 700 Td (PSPAGE " + std::to_string(number) + ") Tj ET\n";

    QPDFObjectHandle font
        = pdf.makeIndirectObject(QPDFObjectHandle::parse("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>"));

    QPDFObjectHandle fonts = QPDFObjectHandle::newDictionary();
    fonts.replaceKey("/F1", font);

    QPDFObjectHandle resources = QPDFObjectHandle::newDictionary();
    resources.replaceKey("/Font", fonts);

    QPDFObjectHandle page = QPDFObjectHandle::newDictionary();
    page.replaceKey("/Type", QPDFObjectHandle::newName("/Page"));
    // QByteArray::number, not std::to_string: the latter follows the C locale
    // and would emit "612,000000" on a German system.
    page.replaceKey("/MediaBox",
                    QPDFObjectHandle::parse("[0 0 " + QByteArray::number(sizePoints.width(), 'f', 2).toStdString() + " "
                                            + QByteArray::number(sizePoints.height(), 'f', 2).toStdString() + "]"));
    page.replaceKey("/Contents", QPDFObjectHandle::newStream(&pdf, text));
    page.replaceKey("/Resources", resources);
    if (rotate != 0) {
        page.replaceKey("/Rotate", QPDFObjectHandle::newInteger(rotate));
    }

    return pdf.makeIndirectObject(page);
}

bool build(const QString &path, int pageCount, const QSizeF &sizePoints, int rotate)
{
    try {
        QPDF pdf;
        pdf.emptyPDF();
        QPDFPageDocumentHelper pages(pdf);

        for (int i = 1; i <= pageCount; ++i) {
            pages.addPage(QPDFPageObjectHelper(makePage(pdf, i, sizePoints, rotate)), false);
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

} // namespace

bool writeSamplePdf(const QString &path, int pageCount, const QSizeF &sizePoints)
{
    return build(path, pageCount, sizePoints, 0);
}

bool writeRotatedPdf(const QString &path, int pageCount, int rotate)
{
    return build(path, pageCount, QSizeF(612, 792), rotate);
}

bool writeTextHeavyPdf(const QString &path, int pageCount, double skewDegrees)
{
    try {
        QPDF pdf;
        pdf.emptyPDF();
        QPDFPageDocumentHelper pages(pdf);

        for (int p = 1; p <= pageCount; ++p) {
            std::string body;
            if (skewDegrees != 0.0) {
                // Tilt the whole page about its centre, so the fixture really
                // is a crooked scan rather than straight text drawn at an angle.
                const double r = skewDegrees * M_PI / 180.0;
                const double c = std::cos(r);
                const double s = std::sin(r);
                const double cx = 306.0;
                const double cy = 396.0;
                const auto num = [](double v) { return QByteArray::number(v, 'f', 5).toStdString(); };
                body += "q " + num(c) + " " + num(s) + " " + num(-s) + " " + num(c) + " " + num(cx - cx * c + cy * s)
                    + " " + num(cy - cx * s - cy * c) + " cm\n";
            }
            body += "BT /F1 13 Tf\n";
            for (int line = 0; line < 40; ++line) {
                char op[220];
                std::snprintf(op, sizeof(op),
                              "1 0 0 1 72 %d Tm (Zeile %d auf Seite %d mit genug Text fuer die Messung) Tj\n",
                              720 - line * 16, line + 1, p);
                body += op;
            }
            body += "ET\n";
            if (skewDegrees != 0.0) {
                body += "Q\n";
            }

            QPDFObjectHandle font = pdf.makeIndirectObject(
                QPDFObjectHandle::parse("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>"));
            QPDFObjectHandle fonts = QPDFObjectHandle::newDictionary();
            fonts.replaceKey("/F1", font);
            QPDFObjectHandle resources = QPDFObjectHandle::newDictionary();
            resources.replaceKey("/Font", fonts);

            QPDFObjectHandle page = QPDFObjectHandle::newDictionary();
            page.replaceKey("/Type", QPDFObjectHandle::newName("/Page"));
            page.replaceKey("/MediaBox", QPDFObjectHandle::parse("[0 0 612 792]"));
            page.replaceKey("/Contents", QPDFObjectHandle::newStream(&pdf, body));
            page.replaceKey("/Resources", resources);

            pages.addPage(QPDFPageObjectHelper(pdf.makeIndirectObject(page)), false);
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

bool writeBookmarkedPdf(const QString &path, int pageCount, const QMap<int, QString> &chapters)
{
    try {
        QPDF pdf;
        pdf.emptyPDF();
        QPDFPageDocumentHelper pages(pdf);

        for (int i = 1; i <= pageCount; ++i) {
            pages.addPage(QPDFPageObjectHelper(makePage(pdf, i, QSizeF(612, 792), 0)), false);
        }

        std::vector<QPDFPageObjectHelper> all = pages.getAllPages();

        // Built by hand rather than through a helper: QPDF can read outlines
        // but has no API for writing them, so the linked list of items is
        // assembled here.
        QPDFObjectHandle outlines = pdf.makeIndirectObject(QPDFObjectHandle::newDictionary());
        outlines.replaceKey("/Type", QPDFObjectHandle::newName("/Outlines"));

        std::vector<QPDFObjectHandle> items;
        for (auto it = chapters.constBegin(); it != chapters.constEnd(); ++it) {
            if (it.key() < 0 || it.key() >= static_cast<int>(all.size())) {
                continue;
            }
            QPDFObjectHandle item = pdf.makeIndirectObject(QPDFObjectHandle::newDictionary());
            item.replaceKey("/Title", QPDFObjectHandle::newUnicodeString(it.value().toStdString()));
            item.replaceKey("/Parent", outlines);

            QPDFObjectHandle dest = QPDFObjectHandle::newArray();
            dest.appendItem(all[static_cast<size_t>(it.key())].getObjectHandle());
            dest.appendItem(QPDFObjectHandle::newName("/Fit"));
            item.replaceKey("/Dest", dest);

            items.push_back(item);
        }

        for (size_t i = 0; i < items.size(); ++i) {
            if (i > 0) {
                items[i].replaceKey("/Prev", items[i - 1]);
            }
            if (i + 1 < items.size()) {
                items[i].replaceKey("/Next", items[i + 1]);
            }
        }

        if (!items.empty()) {
            outlines.replaceKey("/First", items.front());
            outlines.replaceKey("/Last", items.back());
            outlines.replaceKey("/Count", QPDFObjectHandle::newInteger(static_cast<int>(items.size())));
            pdf.getRoot().replaceKey("/Outlines", outlines);
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

bool haveGhostscript()
{
    return !QStandardPaths::findExecutable(QStringLiteral("gs")).isEmpty();
}

bool rasterizePdf(const QString &input, const QString &output, int dpi)
{
    const QString gs = QStandardPaths::findExecutable(QStringLiteral("gs"));
    if (gs.isEmpty()) {
        return false;
    }

    // pdfimage24 throws away every text object, leaving nothing but pixels:
    // exactly what comes out of a scanner.
    QProcess process;
    process.start(gs,
                  { QStringLiteral("-sDEVICE=pdfimage24"), QStringLiteral("-dNOPAUSE"), QStringLiteral("-dBATCH"),
                    QStringLiteral("-dQUIET"), QStringLiteral("-r%1").arg(dpi),
                    QStringLiteral("-sOutputFile=%1").arg(output), input });
    return process.waitForFinished(120000) && process.exitCode() == 0;
}

bool writeBrokenPdf(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    // A plausible header followed by garbage: enough to get past a sniff test
    // and blow up anything that trusts what follows.
    file.write("%PDF-1.7\n1 0 obj\n<< /Type /Catalog /Pages 9 9 R >>\nendobj\ntrailer\n<< /Root 1 0 R >>\n");
    file.write(QByteArray(512, '\x01'));
    return true;
}

int pageCountOf(const QString &path)
{
    try {
        QPDF pdf;
        pdf.processFile(QFile::encodeName(path).constData());
        return static_cast<int>(QPDFPageDocumentHelper(pdf).getAllPages().size());
    } catch (const std::exception &) {
        return -1;
    }
}

int rotationOf(const QString &path, int page)
{
    try {
        QPDF pdf;
        pdf.processFile(QFile::encodeName(path).constData());
        auto pages = QPDFPageDocumentHelper(pdf).getAllPages();
        if (page < 0 || page >= static_cast<int>(pages.size())) {
            return -1;
        }
        // Inheritable, so ask through the helper rather than the raw dictionary.
        QPDFObjectHandle rotate = pages[static_cast<size_t>(page)].getAttribute("/Rotate", false);
        return rotate.isInteger() ? rotate.getIntValueAsInt() : 0;
    } catch (const std::exception &) {
        return -1;
    }
}

QString contentOf(const QString &path, int page)
{
    try {
        QPDF pdf;
        pdf.processFile(QFile::encodeName(path).constData());
        auto pages = QPDFPageDocumentHelper(pdf).getAllPages();
        if (page < 0 || page >= static_cast<int>(pages.size())) {
            return {};
        }
        // A page may carry several content streams; the marker could be in any
        // of them, so glue them together the way a renderer would.
        QString combined;
        for (QPDFObjectHandle &stream : pages[static_cast<size_t>(page)].getPageContents()) {
            const std::shared_ptr<Buffer> buffer = stream.getStreamData();
            combined += QString::fromLatin1(reinterpret_cast<const char *>(buffer->getBuffer()),
                                            static_cast<qsizetype>(buffer->getSize()));
        }
        return combined;
    } catch (const std::exception &) {
        return {};
    }
}

bool addWidgetAnnotation(const QString &input, const QString &output, int page)
{
    try {
        QPDF pdf;
        pdf.processFile(QFile::encodeName(input).constData());
        auto pages = QPDFPageDocumentHelper(pdf).getAllPages();
        if (page < 0 || page >= static_cast<int>(pages.size())) {
            return false;
        }

        QPDFObjectHandle widget = pdf.makeIndirectObject(
            QPDFObjectHandle::parse("<< /Type /Annot /Subtype /Widget /FT /Tx /T (Name) /Rect [72 72 272 96] /F 4 >>"));

        QPDFObjectHandle object = pages[static_cast<size_t>(page)].getObjectHandle();
        QPDFObjectHandle annots = object.getKey("/Annots");
        if (!annots.isArray()) {
            annots = QPDFObjectHandle::newArray();
            object.replaceKey("/Annots", annots);
        }
        annots.appendItem(widget);

        QPDFWriter writer(pdf, QFile::encodeName(output).constData());
        writer.write();
        return true;
    } catch (const std::exception &) {
        return false;
    }
}

bool hasWidgetAnnotation(const QString &path, int page)
{
    try {
        QPDF pdf;
        pdf.processFile(QFile::encodeName(path).constData());
        auto pages = QPDFPageDocumentHelper(pdf).getAllPages();
        if (page < 0 || page >= static_cast<int>(pages.size())) {
            return false;
        }
        QPDFObjectHandle annots = pages[static_cast<size_t>(page)].getObjectHandle().getKey("/Annots");
        if (!annots.isArray()) {
            return false;
        }
        for (int i = 0; i < annots.getArrayNItems(); ++i) {
            QPDFObjectHandle item = annots.getArrayItem(i);
            if (item.isDictionary() && item.getKey("/Subtype").isName()
                && item.getKey("/Subtype").getName() == "/Widget") {
                return true;
            }
        }
        return false;
    } catch (const std::exception &) {
        return false;
    }
}

bool writeLinkedPdf(const QString &path, int pageCount, int target)
{
    try {
        QPDF pdf;
        pdf.emptyPDF();
        QPDFPageDocumentHelper documents(pdf);
        for (int i = 0; i < pageCount; ++i) {
            QPDFObjectHandle page = QPDFObjectHandle::parse("<< /Type /Page /MediaBox [0 0 612 792] >>");
            page.replaceKey("/Resources", QPDFObjectHandle::parse("<< >>"));
            page.replaceKey("/Contents", QPDFObjectHandle::newStream(&pdf, "\n"));
            documents.addPage(QPDFPageObjectHelper(pdf.makeIndirectObject(page)), false);
        }

        auto pages = documents.getAllPages();
        if (target < 0 || target >= static_cast<int>(pages.size())) {
            return false;
        }

        QPDFObjectHandle link = pdf.makeIndirectObject(
            QPDFObjectHandle::parse("<< /Type /Annot /Subtype /Link /Rect [72 700 300 730] /Border [0 0 0] >>"));
        QPDFObjectHandle destination = QPDFObjectHandle::newArray();
        destination.appendItem(pages[static_cast<size_t>(target)].getObjectHandle());
        destination.appendItem(QPDFObjectHandle::newName("/Fit"));
        link.replaceKey("/Dest", destination);

        QPDFObjectHandle annots = QPDFObjectHandle::newArray();
        annots.appendItem(link);
        pages[0].getObjectHandle().replaceKey("/Annots", annots);

        QPDFWriter writer(pdf, QFile::encodeName(path).constData());
        writer.write();
        return true;
    } catch (const std::exception &) {
        return false;
    }
}

int countLinks(const QString &path, int *firstTargetPage)
{
    if (firstTargetPage) {
        *firstTargetPage = -1;
    }
    try {
        QPDF pdf;
        pdf.processFile(QFile::encodeName(path).constData());

        std::map<QPDFObjGen, int> pageIndex;
        auto pages = QPDFPageDocumentHelper(pdf).getAllPages();
        for (size_t i = 0; i < pages.size(); ++i) {
            pageIndex[pages[i].getObjectHandle().getObjGen()] = static_cast<int>(i);
        }

        int found = 0;
        for (QPDFPageObjectHelper &page : pages) {
            QPDFObjectHandle annots = page.getObjectHandle().getKey("/Annots");
            if (!annots.isArray()) {
                continue;
            }
            for (int i = 0; i < annots.getArrayNItems(); ++i) {
                QPDFObjectHandle annot = annots.getArrayItem(i);
                if (!annot.isDictionary() || !annot.getKey("/Subtype").isName()
                    || annot.getKey("/Subtype").getName() != "/Link") {
                    continue;
                }
                ++found;
                QPDFObjectHandle destination = annot.getKey("/Dest");
                if (found == 1 && firstTargetPage && destination.isArray() && destination.getArrayNItems() > 0) {
                    const auto it = pageIndex.find(destination.getArrayItem(0).getObjGen());
                    *firstTargetPage = it != pageIndex.end() ? it->second : -1;
                }
            }
        }
        return found;
    } catch (const std::exception &) {
        return -1;
    }
}

bool writeFormPdf(const QString &path)
{
    try {
        QPDF pdf;
        pdf.emptyPDF();
        QPDFPageDocumentHelper documents(pdf);

        QPDFObjectHandle page = QPDFObjectHandle::parse("<< /Type /Page /MediaBox [0 0 612 792] >>");
        page.replaceKey("/Resources", QPDFObjectHandle::parse("<< >>"));
        page.replaceKey("/Contents", QPDFObjectHandle::newStream(&pdf, "\n"));
        documents.addPage(QPDFPageObjectHelper(pdf.makeIndirectObject(page)), false);
        QPDFPageObjectHelper first = documents.getAllPages().at(0);

        QPDFObjectHandle font = pdf.makeIndirectObject(QPDFObjectHandle::parse(
            "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>"));
        QPDFObjectHandle resources = QPDFObjectHandle::parse("<< /Font << >> >>");
        resources.getKey("/Font").replaceKey("/Helv", font);

        // /Ff bits: 1 read-only, 2 required, 4096 multiline, 131072 combo.
        const char *widgets[] = {
            "<< /Type /Annot /Subtype /Widget /FT /Tx /T (Name) /TU (Ihr Name) /Ff 2"
            "   /Rect [72 700 372 724] /F 4 /DA (/Helv 12 Tf 0 g) >>",
            "<< /Type /Annot /Subtype /Widget /FT /Tx /T (Bemerkung) /Ff 4096"
            "   /Rect [72 600 372 680] /F 4 /DA (/Helv 11 Tf 0 g) >>",
            "<< /Type /Annot /Subtype /Widget /FT /Btn /T (Einverstanden) /V /Off /AS /Off"
            "   /Rect [72 560 92 580] /F 4 /MK << /CA (4) >>"
            "   /AP << /N << /Ja << >> /Off << >> >> >> >>",
            "<< /Type /Annot /Subtype /Widget /FT /Ch /T (Land) /Ff 131072"
            "   /Opt [(Schweiz) (Deutschland) (Oesterreich)] /Rect [72 520 272 544] /F 4"
            "   /DA (/Helv 12 Tf 0 g) >>",
            "<< /Type /Annot /Subtype /Widget /FT /Tx /T (Aktenzeichen) /Ff 1 /V (AZ-2026-001)"
            "   /Rect [72 480 272 504] /F 4 /DA (/Helv 12 Tf 0 g) >>",
        };

        QPDFObjectHandle annots = QPDFObjectHandle::newArray();
        QPDFObjectHandle fields = QPDFObjectHandle::newArray();
        for (const char *definition : widgets) {
            QPDFObjectHandle widget = pdf.makeIndirectObject(QPDFObjectHandle::parse(definition));
            widget.replaceKey("/P", first.getObjectHandle());
            annots.appendItem(widget);
            fields.appendItem(widget);
        }
        first.getObjectHandle().replaceKey("/Annots", annots);

        QPDFObjectHandle acroForm = QPDFObjectHandle::newDictionary();
        acroForm.replaceKey("/Fields", fields);
        acroForm.replaceKey("/DR", resources);
        acroForm.replaceKey("/DA", QPDFObjectHandle::newString("/Helv 12 Tf 0 g"));
        acroForm.replaceKey("/NeedAppearances", QPDFObjectHandle::newBool(true));
        pdf.getRoot().replaceKey("/AcroForm", pdf.makeIndirectObject(acroForm));

        QPDFWriter writer(pdf, QFile::encodeName(path).constData());
        writer.write();
        return true;
    } catch (const std::exception &) {
        return false;
    }
}

} // namespace ps::test
