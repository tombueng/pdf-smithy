/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#include "BlankPdf.h"

#include "PdfGeometry.h"

#include <QFile>
#include <QFileInfo>
#include <QTemporaryFile>

#include <KLocalizedString>

#include <qpdf/QPDF.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>
#include <qpdf/QPDFWriter.hh>

namespace ps {

bool BlankPdf::write(const QString &path, int pageCount, const QSizeF &sizePoints, QString *error)
{
    if (pageCount <= 0) {
        if (error) {
            *error = i18n("A document needs at least one page.");
        }
        return false;
    }
    if (sizePoints.width() <= 0.0 || sizePoints.height() <= 0.0) {
        if (error) {
            *error = i18n("A page cannot be %1 by %2 points.", sizePoints.width(), sizePoints.height());
        }
        return false;
    }

    // Written beside the target and renamed, so a full disk or a crash leaves
    // whatever was already there untouched rather than half a file.
    QTemporaryFile temp(QFileInfo(path).absolutePath() + QLatin1String("/.pdf-smithy-blank-XXXXXX.pdf"));
    temp.setAutoRemove(false);
    if (!temp.open()) {
        if (error) {
            *error = i18n("Cannot write to “%1”.", QFileInfo(path).absolutePath());
        }
        return false;
    }
    const QString tempPath = temp.fileName();
    temp.close();

    try {
        QPDF pdf;
        pdf.emptyPDF();
        QPDFPageDocumentHelper pages(pdf);

        // Written through PdfGeometry::number rather than by QPDF's own real
        // numbers: those go through snprintf, which on a German desktop writes
        // 595,276 and produces a file no reader can measure.
        const QString box = QStringLiteral("[0 0 %1 %2]")
                                .arg(PdfGeometry::number(sizePoints.width()), PdfGeometry::number(sizePoints.height()));

        for (int i = 0; i < pageCount; ++i) {
            QPDFObjectHandle page = QPDFObjectHandle::newDictionary();
            page.replaceKey("/Type", QPDFObjectHandle::newName("/Page"));
            page.replaceKey("/MediaBox", QPDFObjectHandle::parse(box.toStdString()));
            page.replaceKey("/Resources", QPDFObjectHandle::newDictionary());
            page.replaceKey("/Contents", QPDFObjectHandle::newStream(&pdf, ""));
            pages.addPage(QPDFPageObjectHelper(pdf.makeIndirectObject(page)), false);
        }

        QPDFWriter writer(pdf);
        writer.setOutputFilename(QFile::encodeName(tempPath).constData());
        writer.setDeterministicID(true);
        writer.write();
    } catch (const std::exception &e) {
        QFile::remove(tempPath);
        if (error) {
            *error = QString::fromUtf8(e.what());
        }
        return false;
    }

    QFile::remove(path);
    if (!QFile::rename(tempPath, path)) {
        QFile::remove(tempPath);
        if (error) {
            *error = i18n("“%1” could not be written.", path);
        }
        return false;
    }
    return true;
}

} // namespace ps
