/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#include "TestPdf.h"

#include "core/PdfGeometry.h"
#include "core/Typeset.h"

#include <KLocalizedString>

#include <QFile>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include <qpdf/QPDF.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>

using namespace ps;

namespace {

/** One `Tj` out of a content stream, with the state that was in force for it. */
struct SetLine {
    QString font;
    double size = 0.0;
    double x = 0.0;
    double y = 0.0;
    double wordSpacing = 0.0;
    QString text;
};

/**
 * Reads the lines back out of a page.
 *
 * A core test cannot use Poppler, and that turns out to be an advantage: taking
 * the coordinates out of the content stream itself proves what was written
 * rather than what some renderer made of it. Only possible because the stream is
 * emitted one operator to a line, which is itself worth defending.
 */
QVector<SetLine> setLinesOf(const QString &path, int page)
{
    const QString content = test::contentOf(path, page);
    QVector<SetLine> lines;
    SetLine state;

    for (const QString &raw : content.split(QLatin1Char('\n'))) {
        const QString line = raw.trimmed();
        if (line.endsWith(QLatin1String(" Tf"))) {
            const QStringList parts = line.split(QLatin1Char(' '));
            state.font = parts.at(0);
            state.size = parts.at(1).toDouble();
        } else if (line.endsWith(QLatin1String(" Tw"))) {
            state.wordSpacing = line.split(QLatin1Char(' ')).at(0).toDouble();
        } else if (line.endsWith(QLatin1String(" Tm"))) {
            const QStringList parts = line.split(QLatin1Char(' '));
            state.x = parts.at(4).toDouble();
            state.y = parts.at(5).toDouble();
        } else if (line.startsWith(QLatin1Char('(')) && line.endsWith(QLatin1String(") Tj"))) {
            SetLine emitted = state;
            const QString body = line.mid(1, line.size() - 5);
            QString text;
            for (int i = 0; i < body.size(); ++i) {
                if (body.at(i) == QLatin1Char('\\') && i + 1 < body.size()) {
                    ++i;
                }
                text += body.at(i);
            }
            emitted.text = text;
            lines.append(emitted);
        }
    }
    return lines;
}

/** The family behind a resource name, which this class assigns by Core14 index. */
QString familyOfResource(const QString &name)
{
    static const QStringList families = {
        QStringLiteral("Helvetica"), QStringLiteral("Helvetica"), QStringLiteral("Helvetica"),
        QStringLiteral("Helvetica"), QStringLiteral("Times"),     QStringLiteral("Times"),
        QStringLiteral("Times"),     QStringLiteral("Times"),     QStringLiteral("Courier"),
        QStringLiteral("Courier"),   QStringLiteral("Courier"),   QStringLiteral("Courier"),
    };
    const int index = QStringView(name).mid(2).toInt();
    return families.value(index, QStringLiteral("Helvetica"));
}

/** Where a set line ends, word spacing included: the number justification is about. */
double rightEdgeOf(const SetLine &line)
{
    const int index = QStringView(line.font).mid(2).toInt();
    const double width = Typeset::textWidth(line.text, familyOfResource(line.font), index % 2 == 1,
                                            (index % 4) >= 2, line.size);
    return line.x + width + line.wordSpacing * double(line.text.count(QLatin1Char(' ')));
}

/** Every /BaseFont a page declares, which is how a bold run proves itself. */
QStringList baseFontsOf(const QString &path, int page)
{
    QStringList names;
    try {
        QPDF pdf;
        pdf.processFile(QFile::encodeName(path).constData());
        auto pages = QPDFPageDocumentHelper(pdf).getAllPages();
        if (page < 0 || page >= int(pages.size())) {
            return names;
        }
        QPDFObjectHandle fonts = pages[size_t(page)].getObjectHandle().getKey("/Resources").getKey("/Font");
        if (!fonts.isDictionary()) {
            return names;
        }
        for (const std::string &key : fonts.getKeys()) {
            QPDFObjectHandle font = fonts.getKey(key);
            if (font.isDictionary() && font.getKey("/BaseFont").isName()) {
                names.append(QString::fromStdString(font.getKey("/BaseFont").getName()));
            }
        }
    } catch (const std::exception &) {
        return names;
    }
    names.sort();
    return names;
}

/** The page's /MediaBox width, read the locale-safe way. */
double mediaWidthOf(const QString &path, int page)
{
    try {
        QPDF pdf;
        pdf.processFile(QFile::encodeName(path).constData());
        auto pages = QPDFPageDocumentHelper(pdf).getAllPages();
        if (page < 0 || page >= int(pages.size())) {
            return -1.0;
        }
        QPDFObjectHandle box = pages[size_t(page)].getObjectHandle().getKey("/MediaBox");
        return PdfGeometry::boxValue(box, 2, -1.0);
    } catch (const std::exception &) {
        return -1.0;
    }
}

QString wordsOf(const QString &path, int pageCount)
{
    QStringList all;
    for (int page = 0; page < pageCount; ++page) {
        for (const SetLine &line : setLinesOf(path, page)) {
            all.append(line.text);
        }
    }
    return all.join(QLatin1Char(' '));
}

QString numberedWords(int count)
{
    QStringList words;
    for (int i = 1; i <= count; ++i) {
        words.append(QStringLiteral("w%1").arg(i));
    }
    return words.join(QLatin1Char(' '));
}

/** Text extracted by an outside tool, which is the only extraction test that counts. */
QString extractedText(const QString &path)
{
    const QString tool = QStandardPaths::findExecutable(QStringLiteral("pdftotext"));
    if (tool.isEmpty()) {
        return {};
    }
    QProcess process;
    process.start(tool, { QStringLiteral("-layout"), path, QStringLiteral("-") });
    if (!process.waitForFinished(30000)) {
        return {};
    }
    return QString::fromUtf8(process.readAllStandardOutput());
}

} // namespace

/**
 * What a typeset page has to get right.
 *
 * The cases here are the ones that separate composition from printing a text
 * file: the measurements agree with the font, the justified lines end where they
 * are supposed to, no paragraph loses a line to the page break, a heading keeps
 * its text, and every word that went in comes back out.
 */
class TestTypeset : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();

    void measuresAgainstTheFontsOwnWidths();
    void aShortNoteBecomesOnePageWithItsWordsInIt();
    void aLongTextSpillsOverAndKeepsEveryWord();
    void justifiedLinesEndTogetherAndTheLastDoesNot();
    void aParagraphNeverLeavesOneLineBehind();
    void aHeadingGoesWithTheParagraphItIntroduces();
    void headingsTakeTheStyleTheyWereGiven();
    void aNestedListStandsFurtherIn();
    void codeIsSetInCourierAndBoldAsksForABoldFont();
    void aCharacterOutsideWinAnsiIsReportedRatherThanGuessedAt();
    void everyCoordinateUsesAFullStop();
    void tablesQuotesAndRulesSurviveTheirMarkdown();
    void wordsTooWideForTheColumnAreBrokenAndReported();
    void textIsExtractableByAnOutsideTool();
    void aTextFileOnDiskIsTypesetByItsSuffix();
    void twoColumnsFillLeftBeforeRight();
    void runningHeadsGetTheirPlaceholdersFilled();
    void nothingUsableIsRefusedWithAReason();
    void limitationsAreStated();

private:
    QTemporaryDir dir;
    QString out(const QString &name) const { return dir.filePath(name); }
};

void TestTypeset::initTestCase()
{
    KLocalizedString::setApplicationDomain("pdf-smithy");
    QVERIFY(dir.isValid());
}

void TestTypeset::measuresAgainstTheFontsOwnWidths()
{
    // W is 944 thousandths and o is 556 in Helvetica, read out of
    // Core14Widths.h rather than remembered, because 833 is the M and getting
    // the two confused is exactly how a typesetter ends up a hair off on every
    // line. Ten points of "Wo" is therefore exactly 15, and nothing about the
    // arithmetic may round it.
    QCOMPARE(Typeset::textWidth(QStringLiteral("Wo"), QStringLiteral("Helvetica"), false, false, 10.0),
             (944.0 + 556.0) / 100.0);
    QCOMPARE(Typeset::textWidth(QStringLiteral("Wo"), QStringLiteral("Helvetica-Bold"), true, false, 10.0),
             (944.0 + 611.0) / 100.0);
    // Courier is Courier at every weight, which is a useful independent check.
    QCOMPARE(Typeset::textWidth(QStringLiteral("iW"), QStringLiteral("Courier"), false, false, 12.0), 14.4);
    QCOMPARE(Typeset::textWidth(QStringLiteral(""), QStringLiteral("Times"), false, false, 11.0), 0.0);

    // An unwritable character measures as nothing, because that is what happens
    // to it on the page; a measurement that disagreed would be worse than none.
    QCOMPARE(Typeset::textWidth(QStringLiteral("W→o"), QStringLiteral("Helvetica"), false, false, 10.0),
             (944.0 + 556.0) / 100.0);
}

void TestTypeset::aShortNoteBecomesOnePageWithItsWordsInIt()
{
    Typeset::Document document;
    Typeset::Report report;
    QString error;

    const QString path = out(QStringLiteral("short.pdf"));
    QVERIFY2(Typeset::fromPlainText(QStringLiteral("Guten Morgen.\n\nDie Sonne scheint."), path, document, &report,
                                    &error),
             qPrintable(error));

    QCOMPARE(test::pageCountOf(path), 1);
    QCOMPARE(report.pages, 1);
    QCOMPARE(report.paragraphs, 2);
    QVERIFY(report.overflows.isEmpty());

    const QString content = test::contentOf(path, 0);
    QVERIFY(content.contains(QLatin1String("(Guten Morgen.) Tj")));
    QVERIFY(content.contains(QLatin1String("(Die Sonne scheint.) Tj")));
    QVERIFY(content.contains(QLatin1String("/Encoding")) == false); // that lives in the font, not the stream
    QCOMPARE(baseFontsOf(path, 0), QStringList{ QStringLiteral("/Helvetica") });
}

void TestTypeset::aLongTextSpillsOverAndKeepsEveryWord()
{
    Typeset::Document document;
    Typeset::Report report;
    QString error;

    const QString source = numberedWords(1200);
    const QString path = out(QStringLiteral("long.pdf"));
    QVERIFY2(Typeset::fromPlainText(source, path, document, &report, &error), qPrintable(error));

    QVERIFY(report.pages > 1);
    QCOMPARE(test::pageCountOf(path), report.pages);

    // Every word, in order, and not one of them twice: the count alone would let
    // a lost word hide behind a duplicated one.
    const QStringList went = source.split(QLatin1Char(' '));
    const QStringList came = wordsOf(path, report.pages).split(QLatin1Char(' '), Qt::SkipEmptyParts);
    QCOMPARE(came.size(), went.size());
    QCOMPARE(came, went);
}

void TestTypeset::justifiedLinesEndTogetherAndTheLastDoesNot()
{
    Typeset::Document document;
    document.body.alignment = Qt::AlignJustify;
    document.body.fontSize = 11.0;
    Typeset::Report report;
    QString error;

    QString source;
    for (int i = 0; i < 40; ++i) {
        source += QStringLiteral("Satzbau und Zeilenfall gehoeren zusammen wenn eine Seite ruhig wirken soll. ");
    }
    source += QStringLiteral("Kurzer Schluss.");

    const QString path = out(QStringLiteral("justified.pdf"));
    QVERIFY2(Typeset::fromPlainText(source, path, document, &report, &error), qPrintable(error));
    QCOMPARE(report.pages, 1);

    const QVector<SetLine> lines = setLinesOf(path, 0);
    QVERIFY(lines.size() > 5);
    const double target = document.pageSize.width() - document.marginRight;

    for (int i = 0; i < lines.size() - 1; ++i) {
        QVERIFY2(qAbs(rightEdgeOf(lines.at(i)) - target) < 1.0,
                 qPrintable(QStringLiteral("line %1 ends at %2 rather than %3: %4")
                                .arg(i)
                                .arg(rightEdgeOf(lines.at(i)))
                                .arg(target)
                                .arg(lines.at(i).text)));
        QVERIFY(lines.at(i).wordSpacing > 0.0);
    }

    // The one line that must not be stretched. A justified last line is the
    // classic mark of an engine that does not know what it is doing.
    const SetLine &last = lines.constLast();
    QCOMPARE(last.wordSpacing, 0.0);
    QVERIFY(rightEdgeOf(last) < target - 1.0);
}

void TestTypeset::aParagraphNeverLeavesOneLineBehind()
{
    Typeset::Document document;
    document.pageSize = QSizeF(400, 220);
    document.marginTop = document.marginBottom = document.marginLeft = document.marginRight = 20.0;
    document.body.fontSize = 10.0;
    document.body.leading = 15.0;

    // The interesting case is the smallest text that does not fit on one page:
    // there the paragraph overflows by exactly one line, which is the widow.
    int words = 20;
    Typeset::Report report;
    QString error;
    const QString path = out(QStringLiteral("widow.pdf"));
    while (words < 400) {
        QVERIFY2(Typeset::fromPlainText(numberedWords(words), path, document, &report, &error), qPrintable(error));
        if (report.pages > 1) {
            break;
        }
        ++words;
    }
    QCOMPARE(report.pages, 2);

    const int onTheSecondPage = setLinesOf(path, 1).size();
    QVERIFY2(onTheSecondPage >= 2,
             qPrintable(QStringLiteral("%1 line taken over on its own from %2 words").arg(onTheSecondPage).arg(words)));

    // And nothing was lost while the lines were being shuffled about.
    QCOMPARE(wordsOf(path, 2).split(QLatin1Char(' '), Qt::SkipEmptyParts).size(), words);
}

void TestTypeset::aHeadingGoesWithTheParagraphItIntroduces()
{
    // Geometry chosen so the heading fits at the foot of the first page but its
    // paragraph cannot follow it there. Every style is given explicitly, because
    // this case is about a few points either way.
    Typeset::Document document;
    document.pageSize = QSizeF(300, 200);
    document.marginTop = document.marginBottom = document.marginLeft = document.marginRight = 20.0;
    document.body.fontSize = 10.0;
    document.body.leading = 15.0;

    Typeset::Style heading;
    heading.fontSize = 14.0;
    heading.leading = 20.0;
    heading.bold = true;
    heading.keepWithNext = true;
    document.headings = { heading };

    // Words wide enough that each takes a line of its own, so the count of lines
    // is the count of words and the arithmetic above holds.
    const QString wide = QString(20, QLatin1Char('M'));
    QStringList filler;
    for (int i = 0; i < 9; ++i) {
        filler.append(wide);
    }
    const QString markdown =
        filler.join(QLatin1Char('\n')) + QStringLiteral("\n\n# Kapitel\n\n") + wide + QLatin1Char('\n') + wide;

    Typeset::Report report;
    QString error;
    const QString path = out(QStringLiteral("orphan.pdf"));
    QVERIFY2(Typeset::fromMarkdown(markdown, path, document, &report, &error), qPrintable(error));
    QCOMPARE(report.pages, 2);

    QVERIFY2(!test::contentOf(path, 0).contains(QLatin1String("(Kapitel)")),
             "the heading stayed at the foot of the page while its text went overleaf");
    QVERIFY(test::contentOf(path, 1).contains(QLatin1String("(Kapitel)")));
    QCOMPARE(setLinesOf(path, 0).size(), 9);

    // The control: told not to keep with the next block, the heading does sit
    // there, which shows the geometry really is on the boundary.
    heading.keepWithNext = false;
    document.headings = { heading };
    const QString loose = out(QStringLiteral("orphan-allowed.pdf"));
    QVERIFY2(Typeset::fromMarkdown(markdown, loose, document, &report, &error), qPrintable(error));
    QVERIFY(test::contentOf(loose, 0).contains(QLatin1String("(Kapitel)")));
}

void TestTypeset::headingsTakeTheStyleTheyWereGiven()
{
    Typeset::Document document;
    Typeset::Style first;
    first.fontSize = 21.0;
    first.leading = 25.0;
    first.bold = true;
    first.family = QStringLiteral("Times");
    Typeset::Style second;
    second.fontSize = 15.0;
    second.italic = true;
    second.family = QStringLiteral("Times");
    document.headings = { first, second };

    Typeset::Report report;
    QString error;
    const QString path = out(QStringLiteral("headings.pdf"));
    QVERIFY2(Typeset::fromMarkdown(QStringLiteral("# Erstens\n\n## Zweitens\n\nEin Satz.\n"), path, document, &report,
                                   &error),
             qPrintable(error));

    const QVector<SetLine> lines = setLinesOf(path, 0);
    QCOMPARE(lines.size(), 3);
    QCOMPARE(lines.at(0).text, QStringLiteral("Erstens"));
    QCOMPARE(lines.at(0).size, 21.0);
    QCOMPARE(familyOfResource(lines.at(0).font), QStringLiteral("Times"));
    QCOMPARE(lines.at(1).text, QStringLiteral("Zweitens"));
    QCOMPARE(lines.at(1).size, 15.0);
    QCOMPARE(lines.at(2).size, document.body.fontSize);

    // Ranked down the page in the order they were written, and the second
    // heading further down than the first.
    QVERIFY(lines.at(0).y > lines.at(1).y);
    QVERIFY(lines.at(1).y > lines.at(2).y);
    QVERIFY(baseFontsOf(path, 0).contains(QStringLiteral("/Times-Bold")));
    QVERIFY(baseFontsOf(path, 0).contains(QStringLiteral("/Times-Italic")));
}

void TestTypeset::aNestedListStandsFurtherIn()
{
    Typeset::Document document;
    Typeset::Report report;
    QString error;

    const QString path = out(QStringLiteral("list.pdf"));
    QVERIFY2(Typeset::fromMarkdown(QStringLiteral("- aussen\n  - innen\n    1. tiefer\n"), path, document, &report,
                                   &error),
             qPrintable(error));

    const QVector<SetLine> lines = setLinesOf(path, 0);
    double outer = -1.0;
    double inner = -1.0;
    double deeper = -1.0;
    for (const SetLine &line : lines) {
        if (line.text == QStringLiteral("aussen")) {
            outer = line.x;
        } else if (line.text == QStringLiteral("innen")) {
            inner = line.x;
        } else if (line.text == QStringLiteral("tiefer")) {
            deeper = line.x;
        }
    }
    QVERIFY(outer > 0.0);
    QVERIFY2(inner > outer, "the nested item did not indent");
    QVERIFY2(deeper > inner, "the third level did not indent");

    // The markers came too, and the numbered one still reads as a number.
    const QString content = test::contentOf(path, 0);
    QVERIFY(content.contains(QLatin1String("(1.) Tj")));
}

void TestTypeset::codeIsSetInCourierAndBoldAsksForABoldFont()
{
    Typeset::Document document;
    Typeset::Report report;
    QString error;

    const QString markdown = QStringLiteral("Ein **fetter** Anfang mit `code` darin.\n"
                                            "\n"
                                            "```cpp\n"
                                            "int main() { return 0; }\n"
                                            "```\n"
                                            "\n"
                                            "Ein *schraeger* Schluss.\n");
    const QString path = out(QStringLiteral("inline.pdf"));
    QVERIFY2(Typeset::fromMarkdown(markdown, path, document, &report, &error), qPrintable(error));

    const QStringList fonts = baseFontsOf(path, 0);
    QVERIFY2(fonts.contains(QStringLiteral("/Helvetica-Bold")), qPrintable(fonts.join(QLatin1Char(' '))));
    QVERIFY2(fonts.contains(QStringLiteral("/Helvetica-Oblique")), qPrintable(fonts.join(QLatin1Char(' '))));
    QVERIFY2(fonts.contains(QStringLiteral("/Courier")), qPrintable(fonts.join(QLatin1Char(' '))));

    // The fence's language tag is not text and must not be set as any.
    const QString content = test::contentOf(path, 0);
    QVERIFY(!content.contains(QLatin1String("(cpp)")));
    QVERIFY(content.contains(QLatin1String("int main")));

    // The bold word is its own run, in the bold font, and the spaces around it
    // survived; "Einfetter" would mean the runs had been glued together.
    bool foundBold = false;
    for (const SetLine &line : setLinesOf(path, 0)) {
        if (line.text == QStringLiteral("fetter ")) {
            foundBold = true;
            QCOMPARE(line.font, QStringLiteral("/F1"));
        }
    }
    QVERIFY(foundBold);
    QVERIFY(extractedText(path).isEmpty() || extractedText(path).contains(QStringLiteral("Ein fetter Anfang")));
}

void TestTypeset::aCharacterOutsideWinAnsiIsReportedRatherThanGuessedAt()
{
    Typeset::Document document;
    Typeset::Report report;
    QString error;

    const QString path = out(QStringLiteral("winansi.pdf"));
    QVERIFY2(Typeset::fromPlainText(QStringLiteral("Pfeil → Ziel und 中 auch."), path, document, &report,
                                    &error),
             qPrintable(error));

    QVERIFY(!report.overflows.isEmpty());
    bool namedTheArrow = false;
    bool namedTheHan = false;
    for (const QString &note : report.overflows) {
        namedTheArrow = namedTheArrow || note.contains(QStringLiteral("→"));
        namedTheHan = namedTheHan || note.contains(QStringLiteral("中"));
    }
    QVERIFY2(namedTheArrow, qPrintable(report.overflows.join(QLatin1Char('|'))));
    QVERIFY(namedTheHan);

    // What matters as much: no stand-in glyph was invented for it.
    const QString content = test::contentOf(path, 0);
    QVERIFY(content.contains(QLatin1String("Pfeil")));
    QVERIFY(content.contains(QLatin1String("Ziel")));
    QVERIFY(!content.contains(QLatin1Char('?')));

    // The characters WinAnsi does have, on the other hand, go through untouched:
    // quotation marks, dashes and the euro sign are the whole point of it.
    report = Typeset::Report();
    const QString rich = out(QStringLiteral("winansi-rich.pdf"));
    QVERIFY2(Typeset::fromPlainText(QString::fromUtf8("„Grüße“ – 14 €, ca. 30°"), rich,
                                    document, &report, &error),
             qPrintable(error));
    QVERIFY2(report.overflows.isEmpty(), qPrintable(report.overflows.join(QLatin1Char('|'))));
}

void TestTypeset::everyCoordinateUsesAFullStop()
{
    Typeset::Document document;
    document.body.alignment = Qt::AlignJustify; // fractional word spacing as well as coordinates
    document.footer = QStringLiteral("{page} / {pages}");
    Typeset::Report report;
    QString error;

    QString source;
    for (int i = 0; i < 30; ++i) {
        source += QStringLiteral("Zahlen im Inhaltsstrom haben immer einen Punkt und niemals ein Komma. ");
    }

    const QString path = out(QStringLiteral("locale.pdf"));
    QVERIFY2(Typeset::fromPlainText(source, path, document, &report, &error), qPrintable(error));

    static const QRegularExpression commaBetweenDigits(QStringLiteral("[0-9],[0-9]"));
    for (int page = 0; page < report.pages; ++page) {
        const QString content = test::contentOf(path, page);
        QVERIFY(!content.isEmpty());
        QVERIFY2(!commaBetweenDigits.match(content).hasMatch(), qPrintable(content.left(400)));
        QVERIFY(content.contains(QLatin1Char('.')));
    }

    // And the page box, which is written the same way and read back the same way.
    QCOMPARE(mediaWidthOf(path, 0), 595.276);
}

void TestTypeset::tablesQuotesAndRulesSurviveTheirMarkdown()
{
    Typeset::Document document;
    Typeset::Report report;
    QString error;

    const QString markdown = QStringLiteral("Vorher.\n"
                                            "\n"
                                            "> Ein Zitat, das laenger ist als ein Wort.\n"
                                            "\n"
                                            "---\n"
                                            "\n"
                                            "| Jahr | Ort |\n"
                                            "|------|----:|\n"
                                            "| 1902 | Berlin |\n"
                                            "| 1903 | Wien |\n"
                                            "\n"
                                            "Nachher mit [Link](https://example.org/x).\n");
    const QString path = out(QStringLiteral("markdown.pdf"));
    QVERIFY2(Typeset::fromMarkdown(markdown, path, document, &report, &error), qPrintable(error));
    QCOMPARE(report.pages, 1);

    const QString content = test::contentOf(path, 0);
    QVERIFY(content.contains(QLatin1String("(Jahr)")));
    QVERIFY(content.contains(QLatin1String("(1902)")));
    QVERIFY(content.contains(QLatin1String("(Wien)")));
    QVERIFY(content.contains(QLatin1String("Zitat")));
    // The rule and the table's grid are drawn, not written.
    QVERIFY(content.contains(QLatin1String(" re f")));

    // A link keeps its text, says where it goes, and is worth clicking on.
    QVERIFY(content.contains(QLatin1String("(Link")));
    QVERIFY(content.contains(QLatin1String("example.org")));
    try {
        QPDF pdf;
        pdf.processFile(QFile::encodeName(path).constData());
        auto pages = QPDFPageDocumentHelper(pdf).getAllPages();
        QPDFObjectHandle annots = pages[0].getObjectHandle().getKey("/Annots");
        QVERIFY(annots.isArray());
        QCOMPARE(annots.getArrayNItems(), 1);
        QCOMPARE(annots.getArrayItem(0).getKey("/A").getKey("/URI").getUTF8Value(), std::string("https://example.org/x"));
    } catch (const std::exception &e) {
        QFAIL(e.what());
    }

    // The table's header row is bold, and its cells did not run into each other.
    QVERIFY(baseFontsOf(path, 0).contains(QStringLiteral("/Helvetica-Bold")));
    double jahr = -1.0;
    double ort = -1.0;
    for (const SetLine &line : setLinesOf(path, 0)) {
        if (line.text == QStringLiteral("Jahr")) {
            jahr = line.x;
        } else if (line.text == QStringLiteral("Ort")) {
            ort = line.x;
        }
    }
    QVERIFY(jahr > 0.0);
    QVERIFY(ort > jahr);
}

void TestTypeset::wordsTooWideForTheColumnAreBrokenAndReported()
{
    Typeset::Document document;
    document.pageSize = QSizeF(300, 400);
    document.marginLeft = document.marginRight = 100.0; // a very narrow column indeed
    Typeset::Report report;
    QString error;

    const QString monster = QStringLiteral("Donaudampfschifffahrtsgesellschaftskapitaenswitwe");
    const QString path = out(QStringLiteral("wide.pdf"));
    QVERIFY2(Typeset::fromPlainText(QStringLiteral("Titel ") + monster + QStringLiteral(" Ende"), path, document,
                                    &report, &error),
             qPrintable(error));

    bool named = false;
    for (const QString &note : report.overflows) {
        named = named || note.contains(monster);
    }
    QVERIFY2(named, qPrintable(report.overflows.join(QLatin1Char('|'))));

    // Broken, not dropped, and every piece stayed inside the column.
    QString rejoined;
    const double rightMargin = document.pageSize.width() - document.marginRight;
    for (int page = 0; page < report.pages; ++page) {
        for (const SetLine &line : setLinesOf(path, page)) {
            rejoined += line.text;
            QVERIFY2(rightEdgeOf(line) <= rightMargin + 0.5,
                     qPrintable(QStringLiteral("%1 ends at %2").arg(line.text).arg(rightEdgeOf(line))));
        }
    }
    QVERIFY(rejoined.remove(QLatin1Char(' ')).contains(monster));
}

void TestTypeset::textIsExtractableByAnOutsideTool()
{
    if (QStandardPaths::findExecutable(QStringLiteral("pdftotext")).isEmpty()) {
        QSKIP("pdftotext is not installed");
    }

    Typeset::Document document;
    document.body.alignment = Qt::AlignJustify;
    document.title = QStringLiteral("Ein Titel");
    document.header = QStringLiteral("{title}");
    Typeset::Report report;
    QString error;

    QString source;
    for (int i = 0; i < 25; ++i) {
        source += QStringLiteral("Der Text muss sich wieder herauslesen lassen, sonst ist es ein Bild. ");
    }
    source += QString::fromUtf8("\n\nMit Umlauten: Grüße aus Köln, 14 €.\n");

    const QString path = out(QStringLiteral("extract.pdf"));
    QVERIFY2(Typeset::fromPlainText(source, path, document, &report, &error), qPrintable(error));

    const QString text = extractedText(path);
    QVERIFY(!text.isEmpty());
    QVERIFY2(text.contains(QStringLiteral("Der Text muss sich wieder herauslesen lassen")), qPrintable(text.left(400)));
    QVERIFY2(text.contains(QString::fromUtf8("Grüße aus Köln")), qPrintable(text));
    QVERIFY2(text.contains(QString::fromUtf8("14 €")), qPrintable(text));
    QVERIFY(text.contains(QStringLiteral("Ein Titel")));
}

void TestTypeset::aTextFileOnDiskIsTypesetByItsSuffix()
{
    const QString source = out(QStringLiteral("note.md"));
    QFile file(source);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(QString::fromUtf8("# Überschrift\n\nEin Satz.\n").toUtf8());
    file.close();

    Typeset::Document document;
    Typeset::Report report;
    QString error;
    const QString path = out(QStringLiteral("fromfile.pdf"));
    QVERIFY2(Typeset::fromTextFile(source, path, document, &report, &error), qPrintable(error));
    QCOMPARE(report.pages, 1);

    // As Markdown, so the heading is bigger than the body rather than a line
    // beginning with a hash.
    const QVector<SetLine> lines = setLinesOf(path, 0);
    QCOMPARE(lines.size(), 2);
    QVERIFY(lines.at(0).size > lines.at(1).size);
    QVERIFY(!test::contentOf(path, 0).contains(QLatin1Char('#')));

    QVERIFY(!Typeset::fromTextFile(out(QStringLiteral("missing.txt")), out(QStringLiteral("none.pdf")), document,
                                   &report, &error));
    QVERIFY(!error.isEmpty());
}

void TestTypeset::twoColumnsFillLeftBeforeRight()
{
    Typeset::Document document;
    document.columns = 2;
    document.columnGap = 20.0;
    Typeset::Report report;
    QString error;

    const QString path = out(QStringLiteral("columns.pdf"));
    QVERIFY2(Typeset::fromPlainText(numberedWords(900), path, document, &report, &error), qPrintable(error));

    const QVector<SetLine> lines = setLinesOf(path, 0);
    QVERIFY(lines.size() > 20);
    const double middle = document.pageSize.width() / 2.0;

    // The first line of the page is in the left column, and somewhere later the
    // text starts again at the top of the right one.
    QVERIFY(lines.constFirst().x < middle);
    bool secondColumn = false;
    for (const SetLine &line : lines) {
        if (line.x > middle) {
            secondColumn = true;
            QVERIFY(line.y <= lines.constFirst().y);
        }
    }
    QVERIFY2(secondColumn, "nothing reached the second column");
    QCOMPARE(wordsOf(path, report.pages).split(QLatin1Char(' '), Qt::SkipEmptyParts).size(), 900);
}

void TestTypeset::runningHeadsGetTheirPlaceholdersFilled()
{
    Typeset::Document document;
    document.title = QStringLiteral("Bericht");
    document.header = QStringLiteral("{title}");
    document.footer = QStringLiteral("Seite {page} von {pages}");
    Typeset::Report report;
    QString error;

    const QString path = out(QStringLiteral("heads.pdf"));
    QVERIFY2(Typeset::fromPlainText(numberedWords(2000), path, document, &report, &error), qPrintable(error));
    QVERIFY(report.pages >= 2);

    for (int page = 0; page < report.pages; ++page) {
        const QString content = test::contentOf(path, page);
        QVERIFY(content.contains(QLatin1String("(Bericht) Tj")));
        QVERIFY2(content.contains(QStringLiteral("(Seite %1 von %2) Tj").arg(page + 1).arg(report.pages)),
                 qPrintable(QStringLiteral("page %1").arg(page)));
    }

    // Drawn outside the text area, so they never push the first line down.
    const QVector<SetLine> lines = setLinesOf(path, 0);
    double topOfBody = 0.0;
    double head = 0.0;
    for (const SetLine &line : lines) {
        if (line.text == QStringLiteral("Bericht")) {
            head = line.y;
        } else if (line.text.startsWith(QLatin1String("w1 "))) {
            topOfBody = line.y;
        }
    }
    QVERIFY(head > 0.0);
    QVERIFY(topOfBody > 0.0);
    QVERIFY(head > topOfBody);
    QVERIFY(head > document.pageSize.height() - document.marginTop);
}

void TestTypeset::nothingUsableIsRefusedWithAReason()
{
    Typeset::Document document;
    Typeset::Report report;
    QString error;

    QVERIFY(!Typeset::fromPlainText(QStringLiteral("   \n\n  \n"), out(QStringLiteral("empty.pdf")), document, &report,
                                    &error));
    QVERIFY(!error.isEmpty());

    error.clear();
    document.marginLeft = document.marginRight = 290.0;
    QVERIFY(!Typeset::fromPlainText(QStringLiteral("Text"), out(QStringLiteral("narrow.pdf")), document, &report,
                                    &error));
    QVERIFY(!error.isEmpty());

    error.clear();
    document = Typeset::Document();
    document.body.fontSize = 0.0;
    QVERIFY(!Typeset::fromPlainText(QStringLiteral("Text"), out(QStringLiteral("nosize.pdf")), document, &report,
                                    &error));
    QVERIFY(!error.isEmpty());
}

void TestTypeset::limitationsAreStated()
{
    const QStringList notes = Typeset::limitations();
    QVERIFY(notes.size() >= 5);
    for (const QString &note : notes) {
        QVERIFY(!note.trimmed().isEmpty());
    }
}

QTEST_MAIN(TestTypeset)

#include "tst_typeset.moc"
