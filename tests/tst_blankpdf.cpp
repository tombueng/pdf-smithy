/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/

/**
 * Empty paper.
 *
 * Small, but the measurements matter more here than anywhere else in the
 * program: a blank page is the one page whose size nothing else can imply, so
 * if it is written wrong there is no content to notice it by. The German-locale
 * run of this file is the point of it: a page 595,276 points wide is a page no
 * reader can measure, and an empty page is exactly where that would go unseen.
 */

#include "TestPdf.h"

#include "core/BlankPdf.h"
#include "core/Document.h"
#include "core/PdfFile.h"
#include "core/PdfGeometry.h"

#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#include <qpdf/QPDF.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>

using namespace ps;

namespace {

/**
 * The page's box, straight out of the file.
 *
 * Not Document::pageSizePoints: that answers through the render backend, and
 * ps_core is MIT and never links Poppler, so a core test has none. Reading the
 * file is also the more direct question: what was written, rather than what
 * something else made of it.
 */
QSizeF mediaBoxOf(const QString &path, int page)
{
    QPDF pdf;
    PdfFile::open(pdf, path);
    const std::vector<QPDFPageObjectHelper> pages = QPDFPageDocumentHelper(pdf).getAllPages();
    if (page < 0 || page >= static_cast<int>(pages.size())) {
        return {};
    }
    QPDFObjectHandle box = pages[static_cast<size_t>(page)].getObjectHandle().getKey("/MediaBox");
    if (!box.isArray() || box.getArrayNItems() != 4) {
        return {};
    }
    const double left = PdfGeometry::numericValue(box.getArrayItem(0), 0.0);
    const double bottom = PdfGeometry::numericValue(box.getArrayItem(1), 0.0);
    const double right = PdfGeometry::numericValue(box.getArrayItem(2), 0.0);
    const double top = PdfGeometry::numericValue(box.getArrayItem(3), 0.0);
    return QSizeF(right - left, top - bottom);
}

}

class TestBlankPdf : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();

    void writesAsManyPagesAsAsked();
    void writesThePageSizeItWasGiven();
    void writesASizeWithAFractionInEveryLocale();
    void takesLandscapeAsAWideSheet();
    void refusesAPageOfNoSize();
    void refusesADocumentWithNoPages();
    void leavesWhatWasThereWhenItCannotWrite();

private:
    QTemporaryDir m_dir;
};

void TestBlankPdf::initTestCase()
{
    QVERIFY(m_dir.isValid());
}

void TestBlankPdf::writesAsManyPagesAsAsked()
{
    const QString path = m_dir.filePath(QStringLiteral("three.pdf"));
    QString error;
    QVERIFY2(BlankPdf::write(path, 3, BlankPdf::defaultSize(), &error), qPrintable(error));

    Document document;
    QVERIFY2(document.open(path, &error), qPrintable(error));
    QCOMPARE(document.pageCount(), 3);
}

void TestBlankPdf::writesThePageSizeItWasGiven()
{
    const QString path = m_dir.filePath(QStringLiteral("a4.pdf"));
    QString error;
    QVERIFY2(BlankPdf::write(path, 1, BlankPdf::defaultSize(), &error), qPrintable(error));

    const QSizeF size = mediaBoxOf(path, 0);
    QVERIFY2(qAbs(size.width() - 595.276) < 0.01, qPrintable(QString::number(size.width())));
    QVERIFY2(qAbs(size.height() - 841.89) < 0.01, qPrintable(QString::number(size.height())));
}

void TestBlankPdf::writesASizeWithAFractionInEveryLocale()
{
    // The fraction is the whole assertion. Written through anything that goes
    // by the C library's idea of a decimal point, this page comes out 595
    // points wide on half of Europe's desktops, or unreadable.
    const QString path = m_dir.filePath(QStringLiteral("fraction.pdf"));
    QString error;
    QVERIFY2(BlankPdf::write(path, 1, QSizeF(200.5, 400.25), &error), qPrintable(error));

    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QByteArray bytes = file.readAll();
    QVERIFY2(!bytes.contains("200,5"), "the page size was written with a comma, which no PDF reader can parse");
    QVERIFY2(bytes.contains("200.5"), "the page size is not in the file at all");

    QVERIFY2(qAbs(mediaBoxOf(path, 0).width() - 200.5) < 0.01,
             qPrintable(QString::number(mediaBoxOf(path, 0).width())));
}

void TestBlankPdf::takesLandscapeAsAWideSheet()
{
    // As the reader sees it, not as a portrait page that has been turned: a
    // landscape blank must merge with landscape scans without one of them
    // arriving on its side.
    const QString path = m_dir.filePath(QStringLiteral("landscape.pdf"));
    QString error;
    QVERIFY2(BlankPdf::write(path, 1, QSizeF(841.89, 595.276), &error), qPrintable(error));

    const QSizeF size = mediaBoxOf(path, 0);
    QVERIFY2(size.width() > size.height(), "a landscape page came out upright");
}

void TestBlankPdf::refusesAPageOfNoSize()
{
    const QString path = m_dir.filePath(QStringLiteral("nothing.pdf"));
    QString error;
    QVERIFY(!BlankPdf::write(path, 1, QSizeF(0, 100), &error));
    QVERIFY2(!error.isEmpty(), "a refusal has to say what was wrong with it");
    QVERIFY2(!QFile::exists(path), "a refused write left a file behind");
}

void TestBlankPdf::refusesADocumentWithNoPages()
{
    const QString path = m_dir.filePath(QStringLiteral("none.pdf"));
    QString error;
    QVERIFY(!BlankPdf::write(path, 0, BlankPdf::defaultSize(), &error));
    QVERIFY(!error.isEmpty());
}

void TestBlankPdf::leavesWhatWasThereWhenItCannotWrite()
{
    QString error;
    // A directory that does not exist stands in for any unwritable place; what
    // is being checked is that the failure is reported rather than surviving as
    // a stray temporary file beside the target.
    QVERIFY(!BlankPdf::write(m_dir.filePath(QStringLiteral("nowhere/blank.pdf")), 1, BlankPdf::defaultSize(), &error));
    QVERIFY(!error.isEmpty());
}

QTEST_GUILESS_MAIN(TestBlankPdf)
#include "tst_blankpdf.moc"
