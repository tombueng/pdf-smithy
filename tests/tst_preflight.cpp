/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#include "TestPdf.h"

#include "core/PdfFile.h"
#include "core/PdfGeometry.h"
#include "core/Preflight.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>

#include <functional>

#include <KLocalizedString>

#include <qpdf/Buffer.hh>
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFWriter.hh>

using namespace ps;

namespace {

/** Copies @p input to @p output with @p work applied to the open document. */
bool rewrite(const QString &input, const QString &output, const std::function<void(QPDF &)> &work,
             bool restrictive = false)
{
    try {
        QPDF pdf;
        PdfFile::open(pdf, input);
        work(pdf);

        QPDFWriter writer(pdf);
        writer.setOutputFilename(QFile::encodeName(output).constData());
        if (restrictive) {
            // An empty user password with an owner password is the "protected"
            // file people actually send: it opens without being asked for
            // anything and then refuses to be printed.
            writer.setR6EncryptionParameters("", "owner", true, false, false, false, false, false, qpdf_r3p_none, true);
        } else {
            writer.setDeterministicID(true);
        }
        writer.write();
    } catch (const std::exception &) {
        return false;
    }
    return true;
}

/** Puts a document-level JavaScript name tree in the catalogue. */
bool addJavaScript(const QString &input, const QString &output)
{
    return rewrite(input, output, [](QPDF &pdf) {
        QPDFObjectHandle action
            = pdf.makeIndirectObject(QPDFObjectHandle::parse("<< /S /JavaScript /JS (app.alert\\(1\\);) >>"));
        QPDFObjectHandle entries = QPDFObjectHandle::newArray();
        entries.appendItem(QPDFObjectHandle::newString("Greeting"));
        entries.appendItem(action);

        QPDFObjectHandle tree = QPDFObjectHandle::newDictionary();
        tree.replaceKey("/Names", entries);

        QPDFObjectHandle names = QPDFObjectHandle::newDictionary();
        names.replaceKey("/JavaScript", pdf.makeIndirectObject(tree));
        pdf.getRoot().replaceKey("/Names", pdf.makeIndirectObject(names));
    });
}

/** An action that merely jumps somewhere, which is not a script. */
bool addPlainOpenAction(const QString &input, const QString &output)
{
    return rewrite(input, output, [](QPDF &pdf) {
        QPDFObjectHandle destination = QPDFObjectHandle::newArray();
        destination.appendItem(QPDFPageDocumentHelper(pdf).getAllPages().front().getObjectHandle());
        destination.appendItem(QPDFObjectHandle::newName("/Fit"));
        pdf.getRoot().replaceKey("/OpenAction", destination);
    });
}

/** Carries a spreadsheet along inside the document. */
bool addAttachment(const QString &input, const QString &output)
{
    return rewrite(input, output, [](QPDF &pdf) {
        QPDFObjectHandle contents = QPDFObjectHandle::newStream(&pdf, std::string("Datum;Betrag\n2026-07-01;12,50\n"));
        QPDFObjectHandle embedded = QPDFObjectHandle::newDictionary();
        embedded.replaceKey("/F", contents);

        QPDFObjectHandle specification = QPDFObjectHandle::newDictionary();
        specification.replaceKey("/Type", QPDFObjectHandle::newName("/Filespec"));
        specification.replaceKey("/F", QPDFObjectHandle::newString("zahlen.csv"));
        specification.replaceKey("/EF", embedded);

        QPDFObjectHandle entries = QPDFObjectHandle::newArray();
        entries.appendItem(QPDFObjectHandle::newString("zahlen.csv"));
        entries.appendItem(pdf.makeIndirectObject(specification));

        QPDFObjectHandle tree = QPDFObjectHandle::newDictionary();
        tree.replaceKey("/Names", entries);

        QPDFObjectHandle names = QPDFObjectHandle::newDictionary();
        names.replaceKey("/EmbeddedFiles", pdf.makeIndirectObject(tree));
        pdf.getRoot().replaceKey("/Names", pdf.makeIndirectObject(names));
    });
}

/**
 * A TrueType font file whose OS/2 table forbids embedding.
 *
 * Only the header, the table directory and the first ten bytes of OS/2 are real;
 * nothing here renders the font, and the licence check reads exactly one field.
 */
QByteArray restrictedFontProgram()
{
    const auto be16 = [](int value) {
        QByteArray out(2, '\0');
        out[0] = char((value >> 8) & 0xff);
        out[1] = char(value & 0xff);
        return out;
    };
    const auto be32 = [](quint32 value) {
        QByteArray out(4, '\0');
        out[0] = char((value >> 24) & 0xff);
        out[1] = char((value >> 16) & 0xff);
        out[2] = char((value >> 8) & 0xff);
        out[3] = char(value & 0xff);
        return out;
    };

    QByteArray font;
    font += be32(0x00010000u); // sfnt version
    font += be16(1); // one table
    font += be16(16) + be16(0) + be16(0); // search hints, which nothing reads

    const int tableOffset = 28;
    font += QByteArrayLiteral("OS/2");
    font += be32(0); // checksum
    font += be32(quint32(tableOffset));
    font += be32(96); // length

    QByteArray os2;
    os2 += be16(4); // version
    os2 += be16(500); // xAvgCharWidth
    os2 += be16(400); // usWeightClass
    os2 += be16(5); // usWidthClass
    os2 += be16(0x0002); // fsType: restricted licence
    os2 += QByteArray(96 - os2.size(), '\0');

    font += os2;
    return font;
}

/**
 * One page carrying most of what a printer complains about.
 *
 * Built in one fixture rather than six because the rules are meant to be
 * independent: if a hairline stops being found the moment a spot colour is on
 * the same page, that is exactly the bug worth catching.
 */
bool writeAwkwardPdf(const QString &path)
{
    try {
        QPDF pdf;
        pdf.emptyPDF();
        QPDFPageDocumentHelper pages(pdf);

        QPDFObjectHandle helvetica
            = pdf.makeIndirectObject(QPDFObjectHandle::parse("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>"));

        QPDFObjectHandle type3 = pdf.makeIndirectObject(QPDFObjectHandle::parse(
            "<< /Type /Font /Subtype /Type3 /FontBBox [0 0 1000 1000] /FontMatrix [0.001 0 0 0.001 0 0]"
            " /CharProcs << >> /Encoding << /Type /Encoding >> /FirstChar 97 /LastChar 97 /Widths [500] >>"));

        QPDFObjectHandle program = QPDFObjectHandle::newStream(
            &pdf, std::string(restrictedFontProgram().constData(), size_t(restrictedFontProgram().size())));
        QPDFObjectHandle descriptor = pdf.makeIndirectObject(QPDFObjectHandle::parse(
            "<< /Type /FontDescriptor /FontName /ABCDEF+Gesperrt /Flags 4 /ItalicAngle 0 /Ascent 700"
            " /Descent -200 /CapHeight 700 /StemV 80 /FontBBox [0 -200 1000 900] >>"));
        descriptor.replaceKey("/FontFile2", program);
        QPDFObjectHandle restricted = pdf.makeIndirectObject(QPDFObjectHandle::parse(
            "<< /Type /Font /Subtype /TrueType /BaseFont /ABCDEF+Gesperrt /FirstChar 65 /LastChar 65"
            " /Widths [500] /Encoding /WinAnsiEncoding >>"));
        restricted.replaceKey("/FontDescriptor", descriptor);

        // Ten pixels stretched over two hundred points is 3.6 dpi, which no
        // threshold in any profile forgives.
        QPDFObjectHandle image = QPDFObjectHandle::newStream(&pdf, std::string(10 * 10 * 3, '\x40'));
        image.getDict().replaceKey("/Type", QPDFObjectHandle::newName("/XObject"));
        image.getDict().replaceKey("/Subtype", QPDFObjectHandle::newName("/Image"));
        image.getDict().replaceKey("/Width", QPDFObjectHandle::newInteger(10));
        image.getDict().replaceKey("/Height", QPDFObjectHandle::newInteger(10));
        image.getDict().replaceKey("/BitsPerComponent", QPDFObjectHandle::newInteger(8));
        image.getDict().replaceKey("/ColorSpace", QPDFObjectHandle::newName("/DeviceRGB"));

        QPDFObjectHandle spot
            = QPDFObjectHandle::parse("[/Separation /PANTONE#20185#20C /DeviceCMYK"
                                      " << /FunctionType 2 /Domain [0 1] /C0 [0 0 0 0] /C1 [0 0.9 0.8 0] /N 1 >>]");

        QPDFObjectHandle state = QPDFObjectHandle::parse("<< /Type /ExtGState /BM /Multiply /ca 0.5 >>");

        QPDFObjectHandle fonts = QPDFObjectHandle::newDictionary();
        fonts.replaceKey("/F1", helvetica);
        fonts.replaceKey("/F3", type3);
        fonts.replaceKey("/F4", restricted);

        QPDFObjectHandle xobjects = QPDFObjectHandle::newDictionary();
        xobjects.replaceKey("/Im1", image);

        QPDFObjectHandle spaces = QPDFObjectHandle::newDictionary();
        spaces.replaceKey("/Spot", spot);

        QPDFObjectHandle states = QPDFObjectHandle::newDictionary();
        states.replaceKey("/GS1", pdf.makeIndirectObject(state));

        QPDFObjectHandle resources = QPDFObjectHandle::newDictionary();
        resources.replaceKey("/Font", fonts);
        resources.replaceKey("/XObject", xobjects);
        resources.replaceKey("/ColorSpace", spaces);
        resources.replaceKey("/ExtGState", states);

        const std::string content =
            // A line of width zero: the classic hairline.
            "q 0 w 40 40 m 500 40 l S Q\n"
            // Three-point text, which no profile considers legible.
            "BT /F1 3 Tf 1 0 0 1 40 700 Tm (winzig) Tj ET\n"
            "BT /F3 12 Tf 1 0 0 1 40 660 Tm (a) Tj ET\n"
            "BT /F4 12 Tf 1 0 0 1 40 620 Tm (A) Tj ET\n"
            "q /GS1 gs /Spot cs 0.5 scn 40 300 200 100 re f Q\n"
            "q 200 0 0 200 300 60 cm /Im1 Do Q\n";

        QPDFObjectHandle page = QPDFObjectHandle::newDictionary();
        page.replaceKey("/Type", QPDFObjectHandle::newName("/Page"));
        page.replaceKey("/MediaBox", QPDFObjectHandle::parse("[0 0 612 792]"));
        page.replaceKey("/Resources", resources);
        page.replaceKey("/Contents", QPDFObjectHandle::newStream(&pdf, content));
        pages.addPage(QPDFPageObjectHelper(pdf.makeIndirectObject(page)), false);

        QPDFWriter writer(pdf);
        writer.setOutputFilename(QFile::encodeName(path).constData());
        writer.setDeterministicID(true);
        writer.write();
    } catch (const std::exception &) {
        return false;
    }
    return true;
}

/** A document that says it is tagged and has a figure nobody described. */
bool addUndescribedFigure(const QString &input, const QString &output, bool withAlt)
{
    return rewrite(input, output, [withAlt](QPDF &pdf) {
        QPDFObjectHandle page = QPDFPageDocumentHelper(pdf).getAllPages().front().getObjectHandle();

        QPDFObjectHandle figure = QPDFObjectHandle::newDictionary();
        figure.replaceKey("/Type", QPDFObjectHandle::newName("/StructElem"));
        figure.replaceKey("/S", QPDFObjectHandle::newName("/Figure"));
        figure.replaceKey("/Pg", page);
        if (withAlt) {
            figure.replaceKey("/Alt", QPDFObjectHandle::newUnicodeString("Ein Diagramm der Umsätze"));
        }

        QPDFObjectHandle root = QPDFObjectHandle::newDictionary();
        root.replaceKey("/Type", QPDFObjectHandle::newName("/StructTreeRoot"));
        root.replaceKey("/K", pdf.makeIndirectObject(figure));
        pdf.getRoot().replaceKey("/StructTreeRoot", pdf.makeIndirectObject(root));

        QPDFObjectHandle markInfo = QPDFObjectHandle::newDictionary();
        markInfo.replaceKey("/Marked", QPDFObjectHandle::newBool(true));
        pdf.getRoot().replaceKey("/MarkInfo", markInfo);
        pdf.getRoot().replaceKey("/Lang", QPDFObjectHandle::newString("de-DE"));
    });
}

/** An XMP packet whose title disagrees with the one in /Info. */
bool addDisagreeingXmp(const QString &input, const QString &output, const QString &infoTitle, const QString &xmpTitle)
{
    const std::string packet = "<?xpacket begin='' id='W5M0MpCehiHzreSzNTczkc9d'?>\n"
                               "<x:xmpmeta xmlns:x='adobe:ns:meta/'><rdf:RDF "
                               "xmlns:rdf='http://www.w3.org/1999/02/22-rdf-syntax-ns#'>\n"
                               "<rdf:Description rdf:about='' xmlns:dc='http://purl.org/dc/elements/1.1/'>\n"
                               "<dc:title><rdf:Alt><rdf:li xml:lang='x-default'>"
        + xmpTitle.toStdString()
        + "</rdf:li></rdf:Alt></dc:title>\n"
          "</rdf:Description></rdf:RDF></x:xmpmeta>\n<?xpacket end='w'?>\n";

    return rewrite(input, output, [&packet, &infoTitle](QPDF &pdf) {
        QPDFObjectHandle metadata = QPDFObjectHandle::newStream(&pdf, packet);
        metadata.getDict().replaceKey("/Type", QPDFObjectHandle::newName("/Metadata"));
        metadata.getDict().replaceKey("/Subtype", QPDFObjectHandle::newName("/XML"));
        pdf.getRoot().replaceKey("/Metadata", metadata);

        QPDFObjectHandle info = pdf.makeIndirectObject(QPDFObjectHandle::newDictionary());
        info.replaceKey("/Title", QPDFObjectHandle::newUnicodeString(infoTitle.toStdString()));
        pdf.getTrailer().replaceKey("/Info", info);
    });
}

/** Whether the catalogue still holds anything that could run. */
bool catalogueHasScripts(const QString &path)
{
    try {
        QPDF pdf;
        PdfFile::open(pdf, path);
        QPDFObjectHandle root = pdf.getRoot();
        if (root.hasKey("/AA")) {
            return true;
        }
        QPDFObjectHandle names = root.getKey("/Names");
        if (names.isDictionary() && names.hasKey("/JavaScript")) {
            return true;
        }
        QPDFObjectHandle open = root.getKey("/OpenAction");
        return open.isDictionary() && open.hasKey("/JS");
    } catch (const std::exception &) {
        return true;
    }
}

/** The /Title of a file as it now stands on disc. */
QString titleOf(const QString &path)
{
    try {
        QPDF pdf;
        PdfFile::open(pdf, path);
        QPDFObjectHandle info = pdf.getTrailer().getKey("/Info");
        if (!info.isDictionary() || !info.hasKey("/Title")) {
            return {};
        }
        QPDFObjectHandle title = info.getKey("/Title");
        return title.isString() ? QString::fromStdString(title.getUTF8Value()) : QString();
    } catch (const std::exception &) {
        return {};
    }
}

/** How many pages of @p path carry a /TrimBox. */
int pagesWithTrimBox(const QString &path)
{
    try {
        QPDF pdf;
        PdfFile::open(pdf, path);
        int count = 0;
        for (QPDFPageObjectHelper &page : QPDFPageDocumentHelper(pdf).getAllPages()) {
            if (page.getObjectHandle().hasKey("/TrimBox")) {
                ++count;
            }
        }
        return count;
    } catch (const std::exception &) {
        return -1;
    }
}

/** The findings for one rule, so a test can say what it expects rather than count. */
QVector<Preflight::Finding> findingsFor(const Preflight::Report &report, const QString &ruleId)
{
    QVector<Preflight::Finding> found;
    for (const Preflight::Finding &finding : report.findings) {
        if (finding.ruleId == ruleId) {
            found.append(finding);
        }
    }
    return found;
}

bool mentions(const Preflight::Report &report, const QString &ruleId)
{
    return !findingsFor(report, ruleId).isEmpty();
}

/** A profile that asks about exactly the rules named, all as errors. */
Preflight::Profile profileOf(const QStringList &rules)
{
    Preflight::Profile profile;
    profile.id = QStringLiteral("test");
    profile.name = QStringLiteral("Test");
    for (const QString &rule : rules) {
        profile.rules.insert(rule, Preflight::Severity::Error);
    }
    return profile;
}

/**
 * A real document from `testdata/`, or empty when it was never downloaded.
 *
 * The fixtures this suite builds itself are deliberately small and deliberately
 * odd; a hundred-year-old scan is the only thing that proves the walker survives
 * what a real generator emits.
 */
QString realDocument(const QString &name)
{
    QDir directory(QCoreApplication::applicationDirPath());
    for (int i = 0; i < 6; ++i) {
        const QString candidate = directory.filePath(QStringLiteral("testdata/") + name);
        if (QFileInfo::exists(candidate)) {
            return candidate;
        }
        if (!directory.cdUp()) {
            break;
        }
    }
    return {};
}

} // namespace

class TestPreflight : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();

    void everyShippedProfileNamesOnlyRulesThatExist();
    void everyRuleAndFixCanBeDescribed();
    void admitsWhatItCannotCheck();

    void checksARealScanAgainstThePrintProfile();
    void countsMatchTheSeveritiesOfTheFindings();
    void saysWhichRulesItCouldNotJudge();

    void findsAnUnembeddedFont();
    void findsATypeThreeFont();
    void findsASubsetWithoutAToUnicodeTable();
    void findsAFontWhoseLicenceForbidsEmbedding();
    void findsAPicturePlacedTooCoarsely();
    void findsSpotColoursAndBlendModes();
    void findsAHairlineAndTinyText();
    void ignoresInvisibleOcrText();
    void findsAnEmptyPage();
    void findsMixedSizesAndRotations();
    void findsAnAnnotationOffThePage();
    void findsEncryptionAndWithheldPermissions();
    void findsAnUndescribedFigure();
    void acceptsAFigureThatIsDescribed();
    void findsTwoTitlesThatDisagree();
    void ignoresRulesTheProfileDoesNotName();

    void findsAndRemovesJavaScript();
    void findsAndRemovesAnAttachment();
    void findsAndRemovesAnOpenAction();
    void findsAndSetsAMissingTrimBox();
    void findsAndSetsAMissingTitle();
    void linearisesOnRequest();
    void embedsFontsThroughGhostscript();
    void downsamplesAPictureThatIsTooFine();
    void appliesSeveralFixesInOnePass();
    void refusesAFixItDoesNotHave();

    void roundTripsAProfileThroughJson();
    void refusesAProfileNamingARuleThatDoesNotExist();

    void writesAReportThatOpens();
    void reportNamesTheRulesItFound();

private:
    /** Skips the current test unless Ghostscript is there to do the work. */
    bool requireGhostscript();

    QTemporaryDir m_dir;
    QString m_sample;
    QString m_awkward;
    QString path(const QString &name) const { return m_dir.filePath(name); }
};

bool TestPreflight::requireGhostscript()
{
    if (test::haveGhostscript()) {
        return true;
    }
    QTest::qSkip("Ghostscript is not installed", __FILE__, __LINE__);
    return false;
}

void TestPreflight::initTestCase()
{
    KLocalizedString::setApplicationDomain("pdf-smithy");

    QVERIFY(m_dir.isValid());
    m_sample = path(QStringLiteral("sample.pdf"));
    QVERIFY(test::writeSamplePdf(m_sample, 3));

    m_awkward = path(QStringLiteral("awkward.pdf"));
    QVERIFY(writeAwkwardPdf(m_awkward));
}

void TestPreflight::everyShippedProfileNamesOnlyRulesThatExist()
{
    const QVector<Preflight::Profile> profiles = Preflight::builtinProfiles();
    QCOMPARE(profiles.size(), 9);

    const QStringList known = Preflight::knownRules();
    QStringList ids;
    for (const Preflight::Profile &profile : profiles) {
        QVERIFY(!profile.id.isEmpty());
        QVERIFY(!profile.name.isEmpty());
        QVERIFY(!profile.rules.isEmpty());
        ids.append(profile.id);

        const QStringList named = profile.rules.keys();
        for (const QString &rule : named) {
            // A profile naming a rule that does not exist would switch a check
            // off without anyone noticing, which is the one failure a preflight
            // tool must not have.
            QVERIFY2(known.contains(rule), qPrintable(profile.id + QLatin1String(" names ") + rule));
        }
        const QStringList thresholds = profile.thresholds.keys();
        for (const QString &rule : thresholds) {
            QVERIFY2(known.contains(rule), qPrintable(profile.id + QLatin1String(" thresholds ") + rule));
        }

        QCOMPARE(Preflight::profileById(profile.id).name, profile.name);
    }

    for (const QString &wanted :
         { QStringLiteral("pdfa-1b"), QStringLiteral("pdfa-2b"), QStringLiteral("pdfa-3b"), QStringLiteral("pdfx-1a"),
           QStringLiteral("pdfx-3"), QStringLiteral("pdfx-4"), QStringLiteral("print-ready"), QStringLiteral("web"),
           QStringLiteral("accessible") }) {
        QVERIFY2(ids.contains(wanted), qPrintable(wanted));
    }

    QVERIFY(Preflight::profileById(QStringLiteral("no-such-profile")).id.isEmpty());
}

void TestPreflight::everyRuleAndFixCanBeDescribed()
{
    const QStringList known = Preflight::knownRules();
    QCOMPARE(known.size(), 34);
    for (const QString &rule : known) {
        QVERIFY2(!Preflight::describeRule(rule).isEmpty(), qPrintable(rule));
    }
    // Every implemented rule is a known rule, and there is at least one of each.
    const QStringList implemented = Preflight::implementedRules();
    QVERIFY(!implemented.isEmpty());
    QVERIFY(implemented.size() < known.size());
    for (const QString &rule : implemented) {
        QVERIFY(known.contains(rule));
    }

    const QStringList fixes = Preflight::knownFixes();
    QCOMPARE(fixes.size(), 9);
    for (const QString &fix : fixes) {
        QVERIFY2(!Preflight::describeFix(fix).isEmpty(), qPrintable(fix));
    }
    QVERIFY(Preflight::describeFix(QStringLiteral("fix.invent-pixels")).isEmpty());
}

void TestPreflight::admitsWhatItCannotCheck()
{
    QVERIFY(!Preflight::limitations().isEmpty());
    for (const QString &note : Preflight::limitations()) {
        QVERIFY(!note.isEmpty());
    }
}

void TestPreflight::checksARealScanAgainstThePrintProfile()
{
    const QString document = realDocument(QStringLiteral("brief-1902.pdf"));
    if (document.isEmpty()) {
        QSKIP("testdata/brief-1902.pdf has not been downloaded");
    }

    QString error;
    const Preflight::Report report
        = Preflight::run(document, Preflight::profileById(QStringLiteral("print-ready")), &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(!report.findings.isEmpty());

    for (const Preflight::Finding &finding : report.findings) {
        QVERIFY(!finding.message.isEmpty());
        QVERIFY(!finding.ruleId.isEmpty());
        QVERIFY(Preflight::knownRules().contains(finding.ruleId));
        // A fix that is offered has to say what it would do, or the offer is a
        // dare rather than a choice.
        if (!finding.fixId.isEmpty()) {
            QVERIFY(Preflight::knownFixes().contains(finding.fixId));
            QVERIFY(!finding.fixDescription.isEmpty());
        }
    }

    // A hundred-year-old scan has no trim box and no output intent; if those two
    // stop being found on a real document, something has broken quietly.
    QVERIFY(mentions(report, QStringLiteral("page.missing-trimbox")));
    QVERIFY(mentions(report, QStringLiteral("colour.no-output-intent")));
}

void TestPreflight::countsMatchTheSeveritiesOfTheFindings()
{
    const QVector<Preflight::Profile> profiles = Preflight::builtinProfiles();
    for (const Preflight::Profile &profile : profiles) {
        QString error;
        const Preflight::Report report = Preflight::run(m_awkward, profile, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));

        int errors = 0;
        int warnings = 0;
        for (const Preflight::Finding &finding : report.findings) {
            if (finding.severity == Preflight::Severity::Error) {
                ++errors;
            } else if (finding.severity == Preflight::Severity::Warning) {
                ++warnings;
            }
            // Severity comes from the profile, never from the rule.
            QCOMPARE(profile.rules.value(finding.ruleId, finding.severity), finding.severity);
        }
        QCOMPARE(report.errors, errors);
        QCOMPARE(report.warnings, warnings);
        QCOMPARE(report.passed, errors == 0);
    }
}

void TestPreflight::saysWhichRulesItCouldNotJudge()
{
    QString error;
    const Preflight::Report report
        = Preflight::run(m_awkward, Preflight::profileById(QStringLiteral("print-ready")), &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));

    // The honesty has to be real: the list is not decoration, it names rules the
    // profile asked about and nothing was said about.
    QVERIFY(!report.notChecked.isEmpty());
    const QStringList known = Preflight::knownRules();
    const QStringList implemented = Preflight::implementedRules();
    for (const QString &rule : report.notChecked) {
        QVERIFY2(known.contains(rule), qPrintable(rule));
        QVERIFY2(!implemented.contains(rule), qPrintable(rule));
        QVERIFY(!mentions(report, rule));
    }

    // A profile that never names the unchecked rule must not have it foisted on
    // its report either.
    const Preflight::Report accessible
        = Preflight::run(m_awkward, Preflight::profileById(QStringLiteral("accessible")), &error);
    QVERIFY(!accessible.notChecked.contains(QStringLiteral("content.outside-trimbox")));
}

void TestPreflight::findsAnUnembeddedFont()
{
    QString error;
    const Preflight::Report report
        = Preflight::run(m_sample, profileOf({ QStringLiteral("font.not-embedded") }), &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));

    const QVector<Preflight::Finding> found = findingsFor(report, QStringLiteral("font.not-embedded"));
    QCOMPARE(found.size(), 1);
    QCOMPARE(found.first().object, QStringLiteral("Helvetica"));
    QCOMPARE(found.first().fixId, QStringLiteral("fix.embed-fonts"));
    QCOMPARE(report.errors, 1);
    QVERIFY(!report.passed);
}

void TestPreflight::findsATypeThreeFont()
{
    QString error;
    const Preflight::Report report = Preflight::run(m_awkward, profileOf({ QStringLiteral("font.type3") }), &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(findingsFor(report, QStringLiteral("font.type3")).size(), 1);

    // A Type 3 font's glyphs are in the document already, so it is never also
    // reported as unembedded.
    const Preflight::Report both
        = Preflight::run(m_awkward, profileOf({ QStringLiteral("font.not-embedded") }), &error);
    for (const Preflight::Finding &finding : findingsFor(both, QStringLiteral("font.not-embedded"))) {
        QVERIFY(finding.object != QStringLiteral("Gesperrt"));
    }
}

void TestPreflight::findsASubsetWithoutAToUnicodeTable()
{
    QString error;
    const Preflight::Report report
        = Preflight::run(m_awkward, profileOf({ QStringLiteral("font.subset-without-tounicode") }), &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));

    const QVector<Preflight::Finding> found = findingsFor(report, QStringLiteral("font.subset-without-tounicode"));
    QCOMPARE(found.size(), 1);
    // The six-letter subset tag means nothing to a reader and is stripped.
    QCOMPARE(found.first().object, QStringLiteral("Gesperrt"));
}

void TestPreflight::findsAFontWhoseLicenceForbidsEmbedding()
{
    QString error;
    const Preflight::Report report
        = Preflight::run(m_awkward, profileOf({ QStringLiteral("font.licence-restricted") }), &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(findingsFor(report, QStringLiteral("font.licence-restricted")).size(), 1);

    // The plain Helvetica on the sample has no font program at all, so nothing
    // can be said about its licence and nothing is.
    const Preflight::Report quiet
        = Preflight::run(m_sample, profileOf({ QStringLiteral("font.licence-restricted") }), &error);
    QVERIFY(!mentions(quiet, QStringLiteral("font.licence-restricted")));
}

void TestPreflight::findsAPicturePlacedTooCoarsely()
{
    Preflight::Profile profile = profileOf({ QStringLiteral("image.low-resolution") });
    profile.thresholds.insert(QStringLiteral("image.low-resolution"), 300.0);

    QString error;
    const Preflight::Report report = Preflight::run(m_awkward, profile, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(findingsFor(report, QStringLiteral("image.low-resolution")).size(), 1);
    QCOMPARE(findingsFor(report, QStringLiteral("image.low-resolution")).first().page, 0);

    // Ten pixels over two hundred points is 3.6 dpi, so a threshold below that
    // has to fall silent: the resolution is measured, not assumed.
    profile.thresholds.insert(QStringLiteral("image.low-resolution"), 2.0);
    const Preflight::Report lenient = Preflight::run(m_awkward, profile, &error);
    QVERIFY(!mentions(lenient, QStringLiteral("image.low-resolution")));

    Preflight::Profile excessive = profileOf({ QStringLiteral("image.excessive-resolution") });
    excessive.thresholds.insert(QStringLiteral("image.excessive-resolution"), 2.0);
    const Preflight::Report over = Preflight::run(m_awkward, excessive, &error);
    QCOMPARE(findingsFor(over, QStringLiteral("image.excessive-resolution")).size(), 1);
    QCOMPARE(findingsFor(over, QStringLiteral("image.excessive-resolution")).first().fixId,
             QStringLiteral("fix.downsample-images"));
}

void TestPreflight::findsSpotColoursAndBlendModes()
{
    QString error;
    const Preflight::Report report
        = Preflight::run(m_awkward,
                         profileOf({ QStringLiteral("colour.spot-colours"), QStringLiteral("transparency.blend-modes"),
                                     QStringLiteral("transparency.present"), QStringLiteral("colour.rgb-in-print"),
                                     QStringLiteral("colour.no-icc"), QStringLiteral("colour.no-output-intent") }),
                         &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));

    QCOMPARE(findingsFor(report, QStringLiteral("colour.spot-colours")).size(), 1);
    QCOMPARE(findingsFor(report, QStringLiteral("transparency.blend-modes")).size(), 1);
    QCOMPARE(findingsFor(report, QStringLiteral("transparency.present")).size(), 1);
    // The picture is DeviceRGB, which is what a press cannot use directly.
    QCOMPARE(findingsFor(report, QStringLiteral("colour.rgb-in-print")).size(), 1);
    QCOMPARE(findingsFor(report, QStringLiteral("colour.no-icc")).size(), 1);
    QCOMPARE(findingsFor(report, QStringLiteral("colour.no-output-intent")).size(), 1);

    // The spot colour's name is quoted, because a printer needs to know which
    // ink is being asked for.
    QVERIFY(findingsFor(report, QStringLiteral("colour.spot-colours"))
                .first()
                .message.contains(QStringLiteral("PANTONE 185 C")));
}

void TestPreflight::findsAHairlineAndTinyText()
{
    Preflight::Profile profile = profileOf({ QStringLiteral("stroke.hairline"), QStringLiteral("text.tiny") });
    profile.thresholds.insert(QStringLiteral("stroke.hairline"), 0.25);
    profile.thresholds.insert(QStringLiteral("text.tiny"), 5.0);

    QString error;
    const Preflight::Report report = Preflight::run(m_awkward, profile, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(findingsFor(report, QStringLiteral("stroke.hairline")).size(), 1);
    QCOMPARE(findingsFor(report, QStringLiteral("text.tiny")).size(), 1);

    // The sample sets 24 pt text and strokes nothing, so neither rule fires.
    const Preflight::Report clean = Preflight::run(m_sample, profile, &error);
    QVERIFY(!mentions(clean, QStringLiteral("stroke.hairline")));
    QVERIFY(!mentions(clean, QStringLiteral("text.tiny")));
}

void TestPreflight::ignoresInvisibleOcrText()
{
    // Three-point text in render mode 3 is what a scanner's OCR leaves behind.
    // Flagging it would flag every scanned book ever made.
    const QString ocr = path(QStringLiteral("ocr.pdf"));
    QVERIFY(rewrite(m_sample, ocr, [](QPDF &pdf) {
        QPDFPageObjectHelper page = QPDFPageDocumentHelper(pdf).getAllPages().front();
        page.addPageContents(QPDFObjectHandle::newStream(&pdf,
                                                         std::string("BT 3 Tr /F1 2 Tf 1 0 0 1 40 40 Tm "
                                                                     "(unsichtbar) Tj ET\n")),
                             false);
    }));

    Preflight::Profile profile = profileOf({ QStringLiteral("text.tiny") });
    profile.thresholds.insert(QStringLiteral("text.tiny"), 6.0);

    QString error;
    const Preflight::Report report = Preflight::run(ocr, profile, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(!mentions(report, QStringLiteral("text.tiny")));
}

void TestPreflight::findsAnEmptyPage()
{
    const QString withBlank = path(QStringLiteral("blank.pdf"));
    QVERIFY(rewrite(m_sample, withBlank, [](QPDF &pdf) {
        QPDFObjectHandle page = QPDFObjectHandle::newDictionary();
        page.replaceKey("/Type", QPDFObjectHandle::newName("/Page"));
        page.replaceKey("/MediaBox", QPDFObjectHandle::parse("[0 0 612 792]"));
        page.replaceKey("/Contents", QPDFObjectHandle::newStream(&pdf, std::string("q Q\n")));
        QPDFPageDocumentHelper(pdf).addPage(QPDFPageObjectHelper(pdf.makeIndirectObject(page)), false);
    }));

    QString error;
    const Preflight::Report report = Preflight::run(withBlank, profileOf({ QStringLiteral("page.empty") }), &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));

    const QVector<Preflight::Finding> found = findingsFor(report, QStringLiteral("page.empty"));
    QCOMPARE(found.size(), 1);
    QCOMPARE(found.first().page, 3);
}

void TestPreflight::findsMixedSizesAndRotations()
{
    const QString mixed = path(QStringLiteral("mixed.pdf"));
    QVERIFY(rewrite(m_sample, mixed, [](QPDF &pdf) {
        std::vector<QPDFPageObjectHelper> pages = QPDFPageDocumentHelper(pdf).getAllPages();
        pages[1].getObjectHandle().replaceKey("/MediaBox", QPDFObjectHandle::parse("[0 0 842 1191]"));
        pages[2].getObjectHandle().replaceKey("/Rotate", QPDFObjectHandle::newInteger(90));
    }));

    QString error;
    const Preflight::Report report = Preflight::run(
        mixed, profileOf({ QStringLiteral("page.mixed-sizes"), QStringLiteral("page.mixed-rotation") }), &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));

    QCOMPARE(findingsFor(report, QStringLiteral("page.mixed-sizes")).size(), 1);
    QCOMPARE(findingsFor(report, QStringLiteral("page.mixed-sizes")).first().page, 1);
    QCOMPARE(findingsFor(report, QStringLiteral("page.mixed-rotation")).size(), 1);
    QCOMPARE(findingsFor(report, QStringLiteral("page.mixed-rotation")).first().page, 2);

    // One size, one rotation, nothing said.
    const Preflight::Report even = Preflight::run(
        m_sample, profileOf({ QStringLiteral("page.mixed-sizes"), QStringLiteral("page.mixed-rotation") }), &error);
    QVERIFY(even.findings.isEmpty());
}

void TestPreflight::findsAnAnnotationOffThePage()
{
    const QString stray = path(QStringLiteral("stray-annot.pdf"));
    QVERIFY(rewrite(m_sample, stray, [](QPDF &pdf) {
        QPDFObjectHandle annotation = QPDFObjectHandle::newDictionary();
        annotation.replaceKey("/Type", QPDFObjectHandle::newName("/Annot"));
        annotation.replaceKey("/Subtype", QPDFObjectHandle::newName("/Square"));
        annotation.replaceKey("/Rect", QPDFObjectHandle::parse("[580 700 700 760]"));

        QPDFObjectHandle hidden = QPDFObjectHandle::newDictionary();
        hidden.replaceKey("/Type", QPDFObjectHandle::newName("/Annot"));
        hidden.replaceKey("/Subtype", QPDFObjectHandle::newName("/Square"));
        hidden.replaceKey("/Rect", QPDFObjectHandle::parse("[900 900 1000 1000]"));
        // Bit 2 is Hidden: it will never appear on paper, so its position is
        // nobody's problem.
        hidden.replaceKey("/F", QPDFObjectHandle::newInteger(2));

        QPDFObjectHandle annotations = QPDFObjectHandle::newArray();
        annotations.appendItem(pdf.makeIndirectObject(annotation));
        annotations.appendItem(pdf.makeIndirectObject(hidden));
        QPDFPageDocumentHelper(pdf).getAllPages().front().getObjectHandle().replaceKey("/Annots", annotations);
    }));

    QString error;
    const Preflight::Report report
        = Preflight::run(stray, profileOf({ QStringLiteral("interactive.annotations-outside-page") }), &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));

    const QVector<Preflight::Finding> found
        = findingsFor(report, QStringLiteral("interactive.annotations-outside-page"));
    QCOMPARE(found.size(), 1);
    QCOMPARE(found.first().page, 0);
    // One of the two annotations reaches off the page; the hidden one does not
    // count, so the sentence has to be about one and not two.
    QVERIFY(found.first().message.contains(QStringLiteral("Square")));
}

void TestPreflight::findsEncryptionAndWithheldPermissions()
{
    const QString locked = path(QStringLiteral("locked.pdf"));
    QVERIFY(rewrite(m_sample, locked, [](QPDF &) { }, true));

    QString error;
    const Preflight::Report report = Preflight::run(
        locked, profileOf({ QStringLiteral("security.encrypted"), QStringLiteral("security.permissions-restricted") }),
        &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));

    QCOMPARE(findingsFor(report, QStringLiteral("security.encrypted")).size(), 1);
    QCOMPARE(findingsFor(report, QStringLiteral("security.encrypted")).first().fixId, QStringLiteral("fix.decrypt"));
    QCOMPARE(findingsFor(report, QStringLiteral("security.permissions-restricted")).size(), 1);

    // And the fix really removes it, judged by reopening the result.
    const QString freed = path(QStringLiteral("freed.pdf"));
    QStringList applied;
    QVERIFY2(Preflight::applyFixes(locked, freed, { QStringLiteral("fix.decrypt") }, &applied, &error),
             qPrintable(error));
    QVERIFY(!applied.isEmpty());

    QPDF check;
    PdfFile::open(check, freed);
    QVERIFY(!check.isEncrypted());
}

void TestPreflight::findsAnUndescribedFigure()
{
    const QString tagged = path(QStringLiteral("tagged.pdf"));
    QVERIFY(addUndescribedFigure(m_sample, tagged, false));

    QString error;
    const Preflight::Report report
        = Preflight::run(tagged,
                         profileOf({ QStringLiteral("structure.no-tags"), QStringLiteral("structure.no-language"),
                                     QStringLiteral("structure.images-without-alt") }),
                         &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));

    // Tagged and in German, so only the figure is wrong.
    QVERIFY(!mentions(report, QStringLiteral("structure.no-tags")));
    QVERIFY(!mentions(report, QStringLiteral("structure.no-language")));
    const QVector<Preflight::Finding> found = findingsFor(report, QStringLiteral("structure.images-without-alt"));
    QCOMPARE(found.size(), 1);
    QCOMPARE(found.first().page, 0);
}

void TestPreflight::acceptsAFigureThatIsDescribed()
{
    const QString described = path(QStringLiteral("described.pdf"));
    QVERIFY(addUndescribedFigure(m_sample, described, true));

    QString error;
    const Preflight::Report report
        = Preflight::run(described, profileOf({ QStringLiteral("structure.images-without-alt") }), &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(report.findings.isEmpty());
    QVERIFY(report.passed);
}

void TestPreflight::findsTwoTitlesThatDisagree()
{
    const QString disagreeing = path(QStringLiteral("disagreeing.pdf"));
    QVERIFY(addDisagreeingXmp(m_sample, disagreeing, QStringLiteral("Angebot 2026"), QStringLiteral("Angebot 2025")));

    QString error;
    const Preflight::Report report = Preflight::run(
        disagreeing,
        profileOf({ QStringLiteral("metadata.xmp-disagrees-with-info"), QStringLiteral("metadata.no-title") }), &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));

    QCOMPARE(findingsFor(report, QStringLiteral("metadata.xmp-disagrees-with-info")).size(), 1);
    // There is a title, so the missing-title rule stays quiet.
    QVERIFY(!mentions(report, QStringLiteral("metadata.no-title")));

    // Two spellings of the same title are not a disagreement.
    const QString agreeing = path(QStringLiteral("agreeing.pdf"));
    QVERIFY(addDisagreeingXmp(m_sample, agreeing, QStringLiteral("Angebot 2026"), QStringLiteral("Angebot 2026")));
    const Preflight::Report quiet
        = Preflight::run(agreeing, profileOf({ QStringLiteral("metadata.xmp-disagrees-with-info") }), &error);
    QVERIFY(quiet.findings.isEmpty());
}

void TestPreflight::ignoresRulesTheProfileDoesNotName()
{
    // The awkward fixture breaks a dozen rules; a profile that names one of them
    // has to report exactly that one and stay silent about the rest.
    QString error;
    const Preflight::Report report = Preflight::run(m_awkward, profileOf({ QStringLiteral("font.type3") }), &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(report.findings.size(), 1);
    QCOMPARE(report.findings.first().ruleId, QStringLiteral("font.type3"));

    // And an empty profile checks nothing at all rather than everything.
    const Preflight::Report nothing = Preflight::run(m_awkward, Preflight::Profile {}, &error);
    QVERIFY(nothing.findings.isEmpty());
    QVERIFY(nothing.passed);
}

void TestPreflight::findsAndRemovesJavaScript()
{
    const QString scripted = path(QStringLiteral("scripted.pdf"));
    QVERIFY(addJavaScript(m_sample, scripted));
    QVERIFY(catalogueHasScripts(scripted));

    QString error;
    const Preflight::Report before
        = Preflight::run(scripted, profileOf({ QStringLiteral("interactive.javascript") }), &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(findingsFor(before, QStringLiteral("interactive.javascript")).size(), 1);
    QCOMPARE(findingsFor(before, QStringLiteral("interactive.javascript")).first().fixId,
             QStringLiteral("fix.remove-javascript"));

    const QString cleaned = path(QStringLiteral("unscripted.pdf"));
    QStringList applied;
    QVERIFY2(Preflight::applyFixes(scripted, cleaned, { QStringLiteral("fix.remove-javascript") }, &applied, &error),
             qPrintable(error));
    QCOMPARE(applied.size(), 1);

    // Read the catalogue back out of the produced file rather than trusting the
    // report of what was done.
    QVERIFY(!catalogueHasScripts(cleaned));

    const Preflight::Report after
        = Preflight::run(cleaned, profileOf({ QStringLiteral("interactive.javascript") }), &error);
    QVERIFY(after.findings.isEmpty());
    QVERIFY(after.passed);
    QCOMPARE(test::pageCountOf(cleaned), 3);
}

void TestPreflight::findsAndRemovesAnAttachment()
{
    const QString carrying = path(QStringLiteral("carrying.pdf"));
    QVERIFY(addAttachment(m_sample, carrying));

    QString error;
    const Preflight::Report before
        = Preflight::run(carrying, profileOf({ QStringLiteral("interactive.embedded-files") }), &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(findingsFor(before, QStringLiteral("interactive.embedded-files")).size(), 1);

    const QString stripped = path(QStringLiteral("stripped.pdf"));
    QStringList applied;
    QVERIFY2(
        Preflight::applyFixes(carrying, stripped, { QStringLiteral("fix.remove-embedded-files") }, &applied, &error),
        qPrintable(error));

    const Preflight::Report after
        = Preflight::run(stripped, profileOf({ QStringLiteral("interactive.embedded-files") }), &error);
    QVERIFY(after.findings.isEmpty());
}

void TestPreflight::findsAndRemovesAnOpenAction()
{
    const QString jumping = path(QStringLiteral("jumping.pdf"));
    QVERIFY(addPlainOpenAction(m_sample, jumping));

    QString error;
    const Preflight::Report before = Preflight::run(
        jumping, profileOf({ QStringLiteral("interactive.open-action"), QStringLiteral("interactive.javascript") }),
        &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(findingsFor(before, QStringLiteral("interactive.open-action")).size(), 1);
    // A destination is not a script, and calling one the other would cry wolf.
    QVERIFY(!mentions(before, QStringLiteral("interactive.javascript")));

    const QString still = path(QStringLiteral("still.pdf"));
    QStringList applied;
    QVERIFY2(Preflight::applyFixes(jumping, still, { QStringLiteral("fix.remove-open-action") }, &applied, &error),
             qPrintable(error));

    const Preflight::Report after
        = Preflight::run(still, profileOf({ QStringLiteral("interactive.open-action") }), &error);
    QVERIFY(after.findings.isEmpty());
}

void TestPreflight::findsAndSetsAMissingTrimBox()
{
    QString error;
    const Preflight::Profile profile = profileOf({ QStringLiteral("page.missing-trimbox") });
    const Preflight::Report before = Preflight::run(m_sample, profile, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(findingsFor(before, QStringLiteral("page.missing-trimbox")).size(), 3);
    QCOMPARE(pagesWithTrimBox(m_sample), 0);

    const QString trimmed = path(QStringLiteral("trimmed.pdf"));
    QStringList applied;
    QVERIFY2(Preflight::applyFixes(m_sample, trimmed, { QStringLiteral("fix.set-trimbox") }, &applied, &error),
             qPrintable(error));
    QCOMPARE(pagesWithTrimBox(trimmed), 3);

    const Preflight::Report after = Preflight::run(trimmed, profile, &error);
    QVERIFY(after.findings.isEmpty());
    QVERIFY(after.passed);

    // And the box is the crop box, which for this fixture is the media box.
    QPDF check;
    PdfFile::open(check, trimmed);
    QPDFObjectHandle box = QPDFPageDocumentHelper(check).getAllPages().front().getObjectHandle().getKey("/TrimBox");
    QVERIFY(box.isArray());
    QCOMPARE(box.getArrayNItems(), 4);
    // Read through PdfGeometry, which is the only reader that ignores the
    // locale; anything else returns 612 as 612 and 595.276 as zero.
    QCOMPARE(PdfGeometry::boxValue(box, 2, -1.0), 612.0);
    QCOMPARE(PdfGeometry::boxValue(box, 3, -1.0), 792.0);
}

void TestPreflight::findsAndSetsAMissingTitle()
{
    QString error;
    const Preflight::Profile profile = profileOf({ QStringLiteral("metadata.no-title") });
    const Preflight::Report before = Preflight::run(m_sample, profile, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(findingsFor(before, QStringLiteral("metadata.no-title")).size(), 1);
    QCOMPARE(findingsFor(before, QStringLiteral("metadata.no-title")).first().fixId, QStringLiteral("fix.set-title"));

    const QString titled = path(QStringLiteral("Jahresbericht_2026.pdf"));
    QVERIFY(QFile::copy(m_sample, titled));

    const QString out = path(QStringLiteral("titled-out.pdf"));
    QStringList applied;
    QVERIFY2(Preflight::applyFixes(titled, out, { QStringLiteral("fix.set-title") }, &applied, &error),
             qPrintable(error));

    // Read the title back out of the file, and check the underscore became a
    // space: a title is read by people.
    QCOMPARE(titleOf(out), QStringLiteral("Jahresbericht 2026"));

    const Preflight::Report after = Preflight::run(out, profile, &error);
    QVERIFY(after.findings.isEmpty());
    QVERIFY(after.passed);
}

void TestPreflight::linearisesOnRequest()
{
    const QString fast = path(QStringLiteral("fast.pdf"));
    QString error;
    QStringList applied;
    QVERIFY2(Preflight::applyFixes(m_sample, fast, { QStringLiteral("fix.linearise") }, &applied, &error),
             qPrintable(error));
    QCOMPARE(applied.size(), 1);

    // QPDF is the judge of whether a file really is linearised.
    QPDF check;
    PdfFile::open(check, fast);
    QVERIFY(check.isLinearized());
    QCOMPARE(test::pageCountOf(fast), 3);
}

void TestPreflight::embedsFontsThroughGhostscript()
{
    if (!requireGhostscript()) {
        return;
    }

    const Preflight::Profile profile = profileOf({ QStringLiteral("font.not-embedded") });
    QString error;
    QVERIFY(mentions(Preflight::run(m_sample, profile, &error), QStringLiteral("font.not-embedded")));

    const QString embedded = path(QStringLiteral("embedded.pdf"));
    QStringList applied;
    QVERIFY2(Preflight::applyFixes(m_sample, embedded, { QStringLiteral("fix.embed-fonts") }, &applied, &error),
             qPrintable(error));
    QVERIFY(!applied.isEmpty());

    // Judged by reading the produced file back, not by what the fix claimed.
    const Preflight::Report after = Preflight::run(embedded, profile, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(!mentions(after, QStringLiteral("font.not-embedded")));
    QCOMPARE(test::pageCountOf(embedded), 3);
}

void TestPreflight::downsamplesAPictureThatIsTooFine()
{
    if (!requireGhostscript()) {
        return;
    }

    const QString onePage = path(QStringLiteral("one.pdf"));
    QVERIFY(test::writeSamplePdf(onePage, 1));
    const QString fine = path(QStringLiteral("fine.pdf"));
    if (!test::rasterizePdf(onePage, fine, 600)) {
        QSKIP("Ghostscript could not rasterise the fixture");
    }

    Preflight::Profile profile = profileOf({ QStringLiteral("image.excessive-resolution") });
    profile.thresholds.insert(QStringLiteral("image.excessive-resolution"), 400.0);

    QString error;
    QVERIFY(mentions(Preflight::run(fine, profile, &error), QStringLiteral("image.excessive-resolution")));

    const QString coarser = path(QStringLiteral("coarser.pdf"));
    QStringList applied;
    QVERIFY2(Preflight::applyFixes(fine, coarser, { QStringLiteral("fix.downsample-images") }, &applied, &error),
             qPrintable(error));
    QVERIFY(!applied.isEmpty());

    // 300 dpi is what the fix aims at, so a 400 dpi ceiling has to be satisfied
    // afterwards, and the measurement is taken from the produced file.
    const Preflight::Report after = Preflight::run(coarser, profile, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(!mentions(after, QStringLiteral("image.excessive-resolution")));
    QCOMPARE(test::pageCountOf(coarser), 1);
}

void TestPreflight::appliesSeveralFixesInOnePass()
{
    const QString messy = path(QStringLiteral("Vertrag_2026.pdf"));
    QVERIFY(addJavaScript(m_sample, messy));

    const Preflight::Profile profile
        = profileOf({ QStringLiteral("interactive.javascript"), QStringLiteral("page.missing-trimbox"),
                      QStringLiteral("metadata.no-title") });
    QString error;
    const Preflight::Report before = Preflight::run(messy, profile, &error);
    QCOMPARE(before.errors, 5); // one script, three trim boxes, one title

    const QString mended = path(QStringLiteral("mended.pdf"));
    QStringList applied;
    QVERIFY2(Preflight::applyFixes(messy, mended,
                                   { QStringLiteral("fix.set-title"), QStringLiteral("fix.remove-javascript"),
                                     QStringLiteral("fix.set-trimbox"), QStringLiteral("fix.linearise") },
                                   &applied, &error),
             qPrintable(error));
    // Four fixes, four sentences about what each one did.
    QCOMPARE(applied.size(), 4);

    const Preflight::Report after = Preflight::run(mended, profile, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(after.findings.isEmpty());
    QVERIFY(after.passed);

    // Every one of them really landed, read back out of the file.
    QVERIFY(!catalogueHasScripts(mended));
    QCOMPARE(pagesWithTrimBox(mended), 3);
    QCOMPARE(titleOf(mended), QStringLiteral("Vertrag 2026"));
    QPDF check;
    PdfFile::open(check, mended);
    QVERIFY(check.isLinearized());
}

void TestPreflight::refusesAFixItDoesNotHave()
{
    QString error;
    QStringList applied;
    const QString out = path(QStringLiteral("never.pdf"));
    QVERIFY(!Preflight::applyFixes(m_sample, out, { QStringLiteral("fix.make-it-nice") }, &applied, &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!QFileInfo::exists(out));

    // And a missing input is refused before anything is written.
    error.clear();
    QVERIFY(!Preflight::applyFixes(path(QStringLiteral("absent.pdf")), out, { QStringLiteral("fix.linearise") },
                                   &applied, &error));
    QVERIFY(!error.isEmpty());
}

void TestPreflight::roundTripsAProfileThroughJson()
{
    const Preflight::Profile original = Preflight::profileById(QStringLiteral("print-ready"));
    QVERIFY(!original.rules.isEmpty());
    QVERIFY(!original.thresholds.isEmpty());

    const QString file = path(QStringLiteral("print-ready.json"));
    QString error;
    QVERIFY2(Preflight::saveProfile(original, file, &error), qPrintable(error));

    const Preflight::Profile loaded = Preflight::loadProfile(file, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(loaded.id, original.id);
    QCOMPARE(loaded.name, original.name);
    QCOMPARE(loaded.rules, original.rules);
    QCOMPARE(loaded.thresholds, original.thresholds);

    // A threshold with a fractional part is where a locale bug would show: on a
    // German system a comma would either be written or read as one.
    Preflight::Profile fractional = profileOf({ QStringLiteral("stroke.hairline") });
    fractional.thresholds.insert(QStringLiteral("stroke.hairline"), 0.125);
    const QString second = path(QStringLiteral("fractional.json"));
    QVERIFY2(Preflight::saveProfile(fractional, second, &error), qPrintable(error));
    const Preflight::Profile back = Preflight::loadProfile(second, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(back.thresholds.value(QStringLiteral("stroke.hairline")), 0.125);
    QCOMPARE(back.rules, fractional.rules);

    // A saved profile is stable, so it can be kept in version control.
    const QString third = path(QStringLiteral("again.json"));
    QVERIFY(Preflight::saveProfile(original, third, &error));
    QFile first(file);
    QFile repeat(third);
    QVERIFY(first.open(QIODevice::ReadOnly));
    QVERIFY(repeat.open(QIODevice::ReadOnly));
    QCOMPARE(first.readAll(), repeat.readAll());
}

void TestPreflight::refusesAProfileNamingARuleThatDoesNotExist()
{
    const QString file = path(QStringLiteral("wrong.json"));
    QFile out(file);
    QVERIFY(out.open(QIODevice::WriteOnly));
    out.write(QByteArrayLiteral("{\"id\":\"x\",\"name\":\"X\",\"rules\":{\"font.not-embeded\":\"error\"}}"));
    out.close();

    QString error;
    const Preflight::Profile loaded = Preflight::loadProfile(file, &error);
    QVERIFY(loaded.rules.isEmpty());
    QVERIFY(!error.isEmpty());
    // The typo itself is named, because "invalid profile" sends nobody anywhere.
    QVERIFY(error.contains(QStringLiteral("font.not-embeded")));

    // A file that is not JSON at all is refused too, without crashing.
    const QString rubbish = path(QStringLiteral("rubbish.json"));
    QFile bad(rubbish);
    QVERIFY(bad.open(QIODevice::WriteOnly));
    bad.write(QByteArrayLiteral("not a profile"));
    bad.close();
    error.clear();
    QVERIFY(Preflight::loadProfile(rubbish, &error).rules.isEmpty());
    QVERIFY(!error.isEmpty());
}

void TestPreflight::writesAReportThatOpens()
{
    QString error;
    const Preflight::Report report
        = Preflight::run(m_awkward, Preflight::profileById(QStringLiteral("print-ready")), &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(!report.findings.isEmpty());

    const QString out = path(QStringLiteral("report.pdf"));
    QVERIFY2(Preflight::writeReport(m_awkward, report, out, &error), qPrintable(error));
    QVERIFY(QFileInfo(out).size() > 0);

    // QPDF is the judge of whether the produced file is a PDF at all.
    QVERIFY(test::pageCountOf(out) >= 1);

    // A report on a document with nothing wrong still has to be a document.
    const QString cleanOut = path(QStringLiteral("clean-report.pdf"));
    const Preflight::Report empty = Preflight::run(m_sample, Preflight::Profile {}, &error);
    QVERIFY2(Preflight::writeReport(m_sample, empty, cleanOut, &error), qPrintable(error));
    QCOMPARE(test::pageCountOf(cleanOut), 1);
}

void TestPreflight::reportNamesTheRulesItFound()
{
    QString error;
    const Preflight::Report report
        = Preflight::run(m_sample, profileOf({ QStringLiteral("page.missing-trimbox") }), &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));

    const QString out = path(QStringLiteral("named-report.pdf"));
    QVERIFY2(Preflight::writeReport(m_sample, report, out, &error), qPrintable(error));

    // Rule ids are untranslated on purpose, so the report can be searched for
    // one whatever language it was written in.
    QString text;
    for (int page = 0; page < test::pageCountOf(out); ++page) {
        text += test::contentOf(out, page);
    }
    QVERIFY(text.contains(QStringLiteral("page.missing-trimbox")));
    QVERIFY(text.contains(QStringLiteral("sample.pdf")));
}

QTEST_MAIN(TestPreflight)

#include "tst_preflight.moc"
