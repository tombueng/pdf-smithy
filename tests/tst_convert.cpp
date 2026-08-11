/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#include "TestPdf.h"

#include "core/Convert.h"
#include "core/PdfFile.h"

#include <KLocalizedString>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QtEnvironmentVariables>

#include <qpdf/QPDF.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>
#include <qpdf/QPDFWriter.hh>

#include <string>
#include <vector>

using namespace ps;

/**
 * What the export side has to get right.
 *
 * The load-bearing case is the two-column magazine page: everything else here
 * could pass on a fixture built to suit it, and a real page laid out by somebody
 * who never thought about extraction is the only honest test of reading order.
 */
class TestConvert : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;

    QString path(const QString &name) const { return m_dir.filePath(name); }

    /** The whole of a text file this produced, as one string. */
    static QString contentsOf(const QString &file)
    {
        QFile handle(file);
        if (!handle.open(QIODevice::ReadOnly)) {
            return {};
        }
        return QString::fromUtf8(handle.readAll());
    }

    /**
     * A page with three columns of short values on it, aligned and ruled.
     *
     * Built here rather than in the shared fixtures because nothing else needs
     * it, and because a table is exactly the geometry that has to be got right:
     * separate text-showing operators at repeating x positions, which is what
     * every layout program produces and what the detection has to see through.
     */
    static bool writeTablePdf(const QString &file)
    {
        try {
            QPDF pdf;
            pdf.emptyPDF();
            QPDFPageDocumentHelper pages(pdf);

            const int columns[3] = { 72, 240, 400 };
            const char *cells[4][3] = {
                { "Artikel", "Menge", "Preis" },
                { "Schraube", "120", "4.50" },
                { "Mutter", "240", "2.10" },
                { "Scheibe", "480", "1.75" },
            };
            const int baselines[4] = { 700, 676, 656, 636 };

            std::string body;
            // Thin filled rectangles, which is how nearly every producer rules a
            // table and what the rule detection has to recognise.
            for (const int y : { 712, 690, 630 }) {
                body += "72 " + QByteArray::number(y).toStdString() + " 400 0.8 re f\n";
            }
            body += "BT /F1 11 Tf\n";
            for (int row = 0; row < 4; ++row) {
                for (int column = 0; column < 3; ++column) {
                    body += "1 0 0 1 " + QByteArray::number(columns[column]).toStdString() + " "
                        + QByteArray::number(baselines[row]).toStdString() + " Tm (" + cells[row][column] + ") Tj\n";
                }
            }
            body += "ET\n";

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

            QPDFWriter writer(pdf);
            writer.setOutputFilename(QFile::encodeName(file).constData());
            writer.setDeterministicID(true);
            writer.write();
        } catch (const std::exception &) {
            return false;
        }
        return true;
    }

    /** One line of body text, so that the fixture and what is expected of it cannot drift apart. */
    static QString bodyLine(int number)
    {
        return QStringLiteral("Zeile %1 des Textkoerpers mit genug Buchstaben fuer die Messung").arg(number);
    }

    /**
     * Two pages set in four sizes, which is all the heading inference has to go on.
     *
     * Built here rather than in the shared fixtures because the sizes are the
     * whole of what is being tested: a body at eleven points and three ranks
     * above it at twenty-four, sixteen and fourteen. The vertical gaps are wide
     * enough that a paragraph break is certain either way, so what the test
     * measures is the ranking and not the paragraph building underneath it.
     */
    static bool writeHeadingsPdf(const QString &file)
    {
        struct Set {
            int size;
            int baseline;
            QString text;
        };
        const auto body = [](int baseline, int number) { return Set { 11, baseline, bodyLine(number) }; };

        const std::vector<std::vector<Set>> pages {
            { { 24, 720, QStringLiteral("Einleitung") },
              { 16, 676, QStringLiteral("Was hier steht") },
              body(650, 1),
              body(636, 2),
              body(622, 3),
              body(608, 4),
              { 16, 560, QStringLiteral("Aufbau") },
              body(534, 5),
              body(520, 6),
              body(506, 7) },
            { { 24, 720, QStringLiteral("Hauptteil") },
              { 16, 676, QStringLiteral("Vorgehen") },
              body(650, 8),
              body(636, 9),
              body(622, 10),
              body(608, 11),
              { 14, 560, QStringLiteral("Einzelheiten") },
              body(534, 12),
              body(520, 13),
              body(506, 14) },
        };

        try {
            QPDF pdf;
            pdf.emptyPDF();
            QPDFPageDocumentHelper helper(pdf);

            for (const std::vector<Set> &page : pages) {
                std::string content = "BT\n";
                for (const Set &set : page) {
                    content += "/F1 " + std::to_string(set.size) + " Tf\n";
                    content
                        += "1 0 0 1 72 " + std::to_string(set.baseline) + " Tm (" + set.text.toStdString() + ") Tj\n";
                }
                content += "ET\n";

                QPDFObjectHandle font = pdf.makeIndirectObject(
                    QPDFObjectHandle::parse("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>"));
                QPDFObjectHandle fonts = QPDFObjectHandle::newDictionary();
                fonts.replaceKey("/F1", font);
                QPDFObjectHandle resources = QPDFObjectHandle::newDictionary();
                resources.replaceKey("/Font", fonts);

                QPDFObjectHandle object = QPDFObjectHandle::newDictionary();
                object.replaceKey("/Type", QPDFObjectHandle::newName("/Page"));
                object.replaceKey("/MediaBox", QPDFObjectHandle::parse("[0 0 612 792]"));
                object.replaceKey("/Contents", QPDFObjectHandle::newStream(&pdf, content));
                object.replaceKey("/Resources", resources);
                helper.addPage(QPDFPageObjectHelper(pdf.makeIndirectObject(object)), false);
            }

            QPDFWriter writer(pdf);
            writer.setOutputFilename(QFile::encodeName(file).constData());
            writer.setDeterministicID(true);
            writer.write();
        } catch (const std::exception &) {
            return false;
        }
        return true;
    }

    /** Every `#`-prefixed line of a Markdown file, as (level, text). */
    static QVector<QPair<int, QString>> headingLinesOf(const QString &markdown)
    {
        QVector<QPair<int, QString>> found;
        static const QRegularExpression heading(QStringLiteral("^(#{1,6}) (.+)$"));
        const QStringList lines = markdown.split(QLatin1Char('\n'));
        for (const QString &line : lines) {
            const QRegularExpressionMatch match = heading.match(line);
            if (match.hasMatch()) {
                found.append({ int(match.captured(1).size()), match.captured(2) });
            }
        }
        return found;
    }

    /** The same document again, with its objects packed into object streams. */
    static bool packIntoObjectStreams(const QString &in, const QString &out)
    {
        try {
            QPDF pdf;
            PdfFile::open(pdf, in);
            QPDFWriter writer(pdf, QFile::encodeName(out).constData());
            writer.setObjectStreamMode(qpdf_o_generate);
            writer.setStreamDataMode(qpdf_s_compress);
            writer.write();
        } catch (const std::exception &) {
            return false;
        }
        return true;
    }

    /** The XMP packet of a file, or empty when it has none. */
    static QString metadataOf(const QString &file)
    {
        try {
            QPDF pdf;
            PdfFile::open(pdf, file);
            QPDFObjectHandle metadata = pdf.getRoot().getKey("/Metadata");
            if (!metadata.isStream()) {
                return {};
            }
            const auto buffer = metadata.getStreamData();
            return QString::fromUtf8(reinterpret_cast<const char *>(buffer->getBuffer()), qsizetype(buffer->getSize()));
        } catch (const std::exception &) {
            return {};
        }
    }

    static QString magazine() { return qEnvironmentVariable("PS_STRESS_PDF"); }

private Q_SLOTS:
    void initTestCase()
    {
        KLocalizedString::setApplicationDomain("pdf-smithy");
        QVERIFY(m_dir.isValid());
    }

    void plainTextGivesEveryLineOfThePageOnceAndInOrder()
    {
        const QString source = path(QStringLiteral("lines.pdf"));
        QVERIFY(test::writeTextHeavyPdf(source, 1));

        const QString target = path(QStringLiteral("lines.txt"));
        QString error;
        Convert::TextOptions options;
        QVERIFY2(Convert::toText(source, target, options, &error), qPrintable(error));

        const QStringList lines = contentsOf(target).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        QCOMPARE(lines.size(), 40);
        for (int i = 0; i < 40; ++i) {
            // Equality, not containment: a run broken across two output lines
            // would still contain its beginning, and that is the failure this
            // has to catch.
            QCOMPARE(lines.at(i), QStringLiteral("Zeile %1 auf Seite 1 mit genug Text fuer die Messung").arg(i + 1));
        }
    }

    void rawTextGivesOneLinePerRun()
    {
        const QString source = path(QStringLiteral("lines.pdf"));
        QVERIFY(test::writeTextHeavyPdf(source, 1));

        const QString target = path(QStringLiteral("raw.txt"));
        QString error;
        Convert::TextOptions options;
        options.preserveLayout = false;
        QVERIFY2(Convert::toText(source, target, options, &error), qPrintable(error));
        QCOMPARE(contentsOf(target).split(QLatin1Char('\n'), Qt::SkipEmptyParts).size(), 40);
    }

    /**
     * The test that matters.
     *
     * Page three of a real magazine: a kicker, a headline that spans the whole
     * measure, two columns of German prose whose lines share baselines across
     * the gutter, a by-line at the foot of the right column and a running foot
     * under both. Getting this wrong reads the two columns alternately, one line
     * of each, which is what nearly every naive extractor does.
     */
    void twoColumnsAreReadOneAfterTheOther()
    {
        if (!QFile::exists(magazine())) {
            QSKIP("set PS_STRESS_PDF to a large real document to run this");
        }

        const QString target = path(QStringLiteral("magazine.txt"));
        QString error;
        Convert::TextOptions options;
        options.pages = { 2 };
        QVERIFY2(Convert::toText(magazine(), target, options, &error), qPrintable(error));

        const QString text = contentsOf(target);

        // A sentence out of each column, so that neither is simply missing.
        const QString leftFirst = QStringLiteral("Ihre Chefs brauchten sie mehr als sie ihre");
        const QString leftLast = QStringLiteral("und anderer Büroangestellter");
        const QString rightFirst = QStringLiteral("einer solchen Erzählung nämlich dazu antreiben");
        const QString rightLast = QStringLiteral("einer Gewerkschaft beizutreten");
        QVERIFY(text.contains(leftFirst));
        QVERIFY(text.contains(leftLast));
        QVERIFY(text.contains(rightFirst));
        QVERIFY(text.contains(rightLast));

        // The whole of the left column before any of the right one: its last
        // line has to precede the right column's first.
        QVERIFY(text.indexOf(leftFirst) < text.indexOf(leftLast));
        QVERIFY(text.indexOf(leftLast) < text.indexOf(rightFirst));
        QVERIFY(text.indexOf(rightFirst) < text.indexOf(rightLast));

        // And no line holding one column's words next to the other's. These
        // three pairs sit on shared baselines on the page, which is exactly what
        // a line-by-line reading would glue together.
        const QVector<QPair<QString, QString>> sharedBaselines {
            { QStringLiteral("Büroangestellter"), QStringLiteral("Kathrin Stoll") },
            { QStringLiteral("Arbeitnehmerinnen"), QStringLiteral("kürzerer Zeit") },
            { QStringLiteral("Cory Doctorow"), QStringLiteral("Arbeitnehmervereinigungen") },
        };
        const QStringList lines = text.split(QLatin1Char('\n'));
        for (const QString &line : lines) {
            for (const auto &pair : sharedBaselines) {
                QVERIFY2(!(line.contains(pair.first) && line.contains(pair.second)), qPrintable(line));
            }
        }
    }

    void markdownFindsAHeadingAndLeavesNoStrayBullets()
    {
        if (!QFile::exists(magazine())) {
            QSKIP("set PS_STRESS_PDF to a large real document to run this");
        }

        const QString target = path(QStringLiteral("magazine.md"));
        QString error;
        Convert::TextOptions options;
        options.pages = { 2 };
        QVERIFY2(Convert::toMarkdown(magazine(), target, options, &error), qPrintable(error));

        const QStringList lines = contentsOf(target).split(QLatin1Char('\n'));
        int headings = 0;
        static const QRegularExpression heading(QStringLiteral("^#{1,6} \\S"));
        for (const QString &line : lines) {
            if (heading.match(line).hasMatch()) {
                ++headings;
            }
            // A bullet the document drew has to become a Markdown list marker or
            // be gone; left where it was it is a character nobody meant to read.
            for (const QChar character : QStringLiteral("•·–▪∙⁃")) {
                QVERIFY2(!line.startsWith(character), qPrintable(line));
            }
        }
        QVERIFY(headings >= 1);
    }

    void headingsAreRankedByTheSizeTheyAreSetIn()
    {
        const QString source = path(QStringLiteral("headings.pdf"));
        QVERIFY(writeHeadingsPdf(source));

        QString error;
        const QVector<Convert::Heading> headings = Convert::findHeadings(source, {}, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));

        // Three sizes above the body, so three levels, and in the order the
        // document reads rather than the order the sizes rank.
        const QVector<QPair<int, QString>> expected {
            { 1, QStringLiteral("Einleitung") }, { 2, QStringLiteral("Was hier steht") },
            { 2, QStringLiteral("Aufbau") },     { 1, QStringLiteral("Hauptteil") },
            { 2, QStringLiteral("Vorgehen") },   { 3, QStringLiteral("Einzelheiten") },
        };
        QCOMPARE(headings.size(), expected.size());
        for (qsizetype i = 0; i < expected.size(); ++i) {
            QCOMPARE(headings.at(i).text, expected.at(i).second);
            QCOMPARE(headings.at(i).level, expected.at(i).first);
        }
        QCOMPARE(headings.at(0).page, 0);
        QCOMPARE(headings.at(2).page, 0);
        QCOMPARE(headings.at(3).page, 1);
        QCOMPARE(headings.at(5).page, 1);
    }

    void aDocumentSetInOneSizeThroughoutHasNoHeadings()
    {
        const QString source = path(QStringLiteral("flat.pdf"));
        QVERIFY(test::writeTextHeavyPdf(source, 2));

        QString error;
        const QVector<Convert::Heading> headings = Convert::findHeadings(source, {}, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));

        // Eighty lines all set at thirteen points give the ranking nothing to
        // rank. Nothing is the right answer: a level invented from where a line
        // sits on the page would be a table of contents nobody could trust.
        QVERIFY2(headings.isEmpty(), qPrintable(headings.isEmpty() ? QString() : headings.constFirst().text));
    }

    /**
     * The test that the heading inference really is shared and not copied.
     *
     * toMarkdown() ranks its own headings and findHeadings() reports them, and
     * if the two ever stopped being the same code this is where it would show:
     * one of them would find a heading the other did not, or put it at another
     * level.
     */
    void markdownHeadingsAreExactlyTheOnesTheInferenceReports()
    {
        const QString source = path(QStringLiteral("headings.pdf"));
        QVERIFY(writeHeadingsPdf(source));

        const QString target = path(QStringLiteral("headings.md"));
        QString error;
        Convert::TextOptions options;
        QVERIFY2(Convert::toMarkdown(source, target, options, &error), qPrintable(error));

        const QVector<QPair<int, QString>> written = headingLinesOf(contentsOf(target));
        const QVector<Convert::Heading> found = Convert::findHeadings(source, {}, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));

        QCOMPARE(written.size(), found.size());
        QCOMPARE(found.size(), 6);
        for (qsizetype i = 0; i < found.size(); ++i) {
            QCOMPARE(written.at(i).first, found.at(i).level);
            QCOMPARE(written.at(i).second, found.at(i).text);
        }
    }

    /**
     * And that sharing it changed nothing about what the export writes.
     *
     * Not only the headings: the paragraphs between them, joined into flowing
     * text with the lines put back together, are the other half of what
     * toMarkdown() promises and the half a careless lift would disturb.
     */
    void markdownStillWritesTheWholeDocumentAsItDid()
    {
        const QString source = path(QStringLiteral("headings.pdf"));
        QVERIFY(writeHeadingsPdf(source));

        const QString target = path(QStringLiteral("whole.md"));
        QString error;
        Convert::TextOptions options;
        QVERIFY2(Convert::toMarkdown(source, target, options, &error), qPrintable(error));

        const QString markdown = contentsOf(target);
        QVERIFY2(markdown.startsWith(QStringLiteral("# Einleitung\n\n## Was hier steht\n\n")), qPrintable(markdown));
        QVERIFY(markdown.contains(QStringLiteral("\n## Aufbau\n")));
        QVERIFY(markdown.contains(QStringLiteral("\n# Hauptteil\n")));
        QVERIFY(markdown.contains(QStringLiteral("\n### Einzelheiten\n")));

        // The four lines under the first sub-heading come out as one flowing
        // paragraph, which is what Markdown is for and what toText() does not do.
        QStringList first;
        for (int number = 1; number <= 4; ++number) {
            first.append(bodyLine(number));
        }
        QVERIFY2(markdown.contains(first.join(QLatin1Char(' '))), qPrintable(markdown));

        // Six headings and six paragraphs of body, and nothing that looks like a
        // heading in the middle of the prose.
        QCOMPARE(headingLinesOf(markdown).size(), 6);
        QCOMPARE(markdown.count(bodyLine(1)), 1);
        QCOMPARE(markdown.count(QStringLiteral("Zeile ")), 14);
    }

    void htmlFlowsRatherThanReproducingThePage()
    {
        if (!QFile::exists(magazine())) {
            QSKIP("set PS_STRESS_PDF to a large real document to run this");
        }

        const QString target = path(QStringLiteral("magazine.html"));
        QString error;
        Convert::TextOptions options;
        options.pages = { 2 };
        QVERIFY2(Convert::toHtml(magazine(), target, options, &error), qPrintable(error));

        const QString html = contentsOf(target);
        QVERIFY(html.contains(QStringLiteral("<p>")));
        // The whole point: absolute positioning would reproduce the page and
        // produce a file nobody can read on a telephone.
        QVERIFY(!html.contains(QStringLiteral("position:absolute")));
        QVERIFY(!html.contains(QStringLiteral("position: absolute")));
        QVERIFY(html.contains(QStringLiteral("einer Gewerkschaft beizutreten")));
    }

    void tablesAreFoundByTheColumnsTheirTextMakes()
    {
        const QString source = path(QStringLiteral("table.pdf"));
        QVERIFY(writeTablePdf(source));

        QString error;
        const QVector<Convert::Table> tables = Convert::findTables(source, { 0 }, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(tables.size(), 1);
        QCOMPARE(tables.first().rows.size(), 4);
        for (const QStringList &row : tables.first().rows) {
            QCOMPARE(row.size(), 3);
        }
        QCOMPARE(tables.first().rows.at(0).at(0), QStringLiteral("Artikel"));
        QCOMPARE(tables.first().rows.at(0).at(2), QStringLiteral("Preis"));
        QCOMPARE(tables.first().rows.at(2).at(1), QStringLiteral("240"));
        QVERIFY(tables.first().confidence > 0.0);
        QVERIFY(tables.first().confidence <= 1.0);
        QCOMPARE(tables.first().page, 0);
    }

    void tablesBecomeCommaSeparatedValues()
    {
        const QString source = path(QStringLiteral("table.pdf"));
        QVERIFY(writeTablePdf(source));

        const QString target = path(QStringLiteral("table.csv"));
        QString error;
        QVERIFY2(Convert::tablesToCsv(source, target, { 0 }, &error), qPrintable(error));

        const QStringList lines = contentsOf(target).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        QCOMPARE(lines.size(), 4);
        QCOMPARE(lines.first().count(QLatin1Char(',')), 2);
        QCOMPARE(lines.first(), QStringLiteral("Artikel,Menge,Preis"));
    }

    void prosePassingForATableIsRejected()
    {
        if (!QFile::exists(magazine())) {
            QSKIP("set PS_STRESS_PDF to a large real document to run this");
        }

        // Two columns of justified prose line up as well as any table does. The
        // only thing that says they are not one is that their lines fill the
        // measure, and this is the test of that judgement.
        QString error;
        const QVector<Convert::Table> tables = Convert::findTables(magazine(), { 2 }, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(tables.size(), 0);
    }

    void linearisedFilesSaySoWhenReadBack()
    {
        const QString source = path(QStringLiteral("plain.pdf"));
        QVERIFY(test::writeSamplePdf(source, 6));

        const QString target = path(QStringLiteral("fast.pdf"));
        QString error;
        QVERIFY2(Convert::linearise(source, target, &error), qPrintable(error));

        QPDF pdf;
        PdfFile::open(pdf, target);
        QVERIFY(pdf.isLinearized());
        QCOMPARE(test::pageCountOf(target), 6);
    }

    void versionCannotBeLoweredBelowWhatTheDocumentNeeds()
    {
        const QString plain = path(QStringLiteral("plain.pdf"));
        QVERIFY(test::writeSamplePdf(plain, 2));
        const QString packed = path(QStringLiteral("packed.pdf"));
        QVERIFY(packIntoObjectStreams(plain, packed));

        const QString target = path(QStringLiteral("lowered.pdf"));
        QStringList warnings;
        QString error;
        QVERIFY(!Convert::setVersion(packed, target, QStringLiteral("1.3"), &warnings, &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(!warnings.isEmpty());
        // The version the object streams need, which is a number and not a
        // translated sentence.
        QVERIFY2(warnings.first().contains(QStringLiteral("1.5")), qPrintable(warnings.join(QLatin1Char('\n'))));
        QVERIFY(!QFileInfo::exists(target));
    }

    void versionCanBeSetWhereTheDocumentAllowsIt()
    {
        const QString plain = path(QStringLiteral("plain.pdf"));
        QVERIFY(test::writeSamplePdf(plain, 2));

        const QString target = path(QStringLiteral("claimed.pdf"));
        QStringList warnings;
        QString error;
        QVERIFY2(Convert::setVersion(plain, target, QStringLiteral("1.4"), &warnings, &error), qPrintable(error));

        QPDF pdf;
        PdfFile::open(pdf, target);
        QCOMPARE(QString::fromStdString(pdf.getPDFVersion()), QStringLiteral("1.4"));
        QCOMPARE(test::pageCountOf(target), 2);
    }

    void nonsenseVersionsAreRefused()
    {
        const QString plain = path(QStringLiteral("plain.pdf"));
        QVERIFY(test::writeSamplePdf(plain, 1));

        QStringList warnings;
        QString error;
        QVERIFY(!Convert::setVersion(plain, path(QStringLiteral("nope.pdf")), QStringLiteral("Acrobat 9"), &warnings,
                                     &error));
        QVERIFY(!error.isEmpty());
    }

    void pdfXSaysWhatItIsInItsMetadata()
    {
        if (!test::haveGhostscript()) {
            QSKIP("Ghostscript, which does the conversion, is not installed");
        }

        const QString source = path(QStringLiteral("print-in.pdf"));
        QVERIFY(test::writeTextHeavyPdf(source, 1));

        // Named explicitly where the machine has one, so that the assertion
        // about the output intent does not depend on what happens to be
        // installed. Nothing else in the conversion changes with it.
        QString profile;
        for (const QString &candidate : { QStringLiteral("/usr/share/color/icc/ISOcoated_v2_bas.ICC"),
                                          QStringLiteral("/usr/share/color/icc/ISOcoated_v2_300_bas.ICC") }) {
            if (QFileInfo::exists(candidate)) {
                profile = candidate;
                break;
            }
        }

        const QString target = path(QStringLiteral("print-out.pdf"));
        QStringList changes;
        QStringList warnings;
        QString error;
        QVERIFY2(Convert::toPdfX(source, target, Convert::XLevel::X3, profile, &changes, &warnings, &error),
                 qPrintable(error));
        QVERIFY(QFileInfo(target).size() > 0);
        QVERIFY(!changes.isEmpty());

        // The standard's own names, which are the same in every language and so
        // are safe to assert on.
        const QString xmp = metadataOf(target);
        QVERIFY2(xmp.contains(QStringLiteral("GTS_PDFXVersion")), qPrintable(xmp.left(400)));
        QVERIFY(xmp.contains(QStringLiteral("PDF/X-3:2003")));

        QPDF pdf;
        PdfFile::open(pdf, target);
        QCOMPARE(int(QPDFPageDocumentHelper(pdf).getAllPages().size()), 1);
        if (!profile.isEmpty()) {
            QPDFObjectHandle intents = pdf.getRoot().getKey("/OutputIntents");
            QVERIFY(intents.isArray());
            QVERIFY(intents.getArrayNItems() >= 1);
            QPDFObjectHandle intent = intents.getArrayItem(0);
            QVERIFY(intent.isDictionary());
            QVERIFY(intent.hasKey("/DestOutputProfile"));
        }
    }

    void svgIsWrittenOnePerPage()
    {
        if (QStandardPaths::findExecutable(QStringLiteral("pdftocairo")).isEmpty()) {
            QSKIP("pdftocairo, which draws the SVG, is not installed");
        }

        const QString source = path(QStringLiteral("svg-in.pdf"));
        QVERIFY(test::writeTextHeavyPdf(source, 3));

        QString error;
        const QString tmpl = path(QStringLiteral("page-%1.svg"));
        QVERIFY2(Convert::toSvg(source, tmpl, { 0, 2 }, &error), qPrintable(error));

        QVERIFY(QFileInfo::exists(path(QStringLiteral("page-001.svg"))));
        QVERIFY(QFileInfo::exists(path(QStringLiteral("page-003.svg"))));
        QVERIFY(!QFileInfo::exists(path(QStringLiteral("page-002.svg"))));
        QVERIFY(contentsOf(path(QStringLiteral("page-001.svg"))).contains(QStringLiteral("<svg")));
    }

    void limitationsAreStated()
    {
        const QStringList limitations = Convert::limitations();
        QVERIFY(limitations.size() >= 8);
        for (const QString &limitation : limitations) {
            QVERIFY(!limitation.isEmpty());
        }
    }

    void missingPagesAreReportedRatherThanIgnored()
    {
        const QString plain = path(QStringLiteral("plain.pdf"));
        QVERIFY(test::writeSamplePdf(plain, 2));

        QString error;
        Convert::TextOptions options;
        options.pages = { 9 };
        QVERIFY(!Convert::toText(plain, path(QStringLiteral("nope.txt")), options, &error));
        QVERIFY(!error.isEmpty());
    }
};

QTEST_MAIN(TestConvert)

#include "tst_convert.moc"
