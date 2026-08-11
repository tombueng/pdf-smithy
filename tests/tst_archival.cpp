/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#include "TestPdf.h"

#include "core/Archival.h"
#include "core/PdfFile.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#include <functional>

#include <KLocalizedString>

#include <qpdf/Buffer.hh>
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDFWriter.hh>

using namespace ps;

namespace {

/**
 * Copies @p input to @p output with @p work applied to the open document.
 *
 * The fixtures this test needs are all "an ordinary PDF, plus one thing PDF/A
 * forbids", so they are built by editing a sample rather than by writing five
 * near-identical builders.
 */
bool rewrite(const QString &input, const QString &output, const std::function<void(QPDF &)> &work, bool encrypt = false)
{
    try {
        QPDF pdf;
        PdfFile::open(pdf, input);
        work(pdf);

        QPDFWriter writer(pdf);
        writer.setOutputFilename(QFile::encodeName(output).constData());
        if (encrypt) {
            // A file ID feeds the encryption key, so QPDF refuses to make
            // encrypted output deterministic. Nothing here depends on that.
            writer.setR6EncryptionParameters("", "owner", true, true, true, true, true, true, qpdf_r3p_full, true);
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

/** Carries a file along inside the document, which only PDF/A-3 permits. */
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

/** Stamps an XMP packet claiming a PDF/A part, without making the file conform. */
bool claimLevel(const QString &input, const QString &output, const QString &part, const QString &conformance)
{
    const std::string packet = "<?xpacket begin='' id='W5M0MpCehiHzreSzNTczkc9d'?>\n"
                               "<x:xmpmeta xmlns:x='adobe:ns:meta/'><rdf:RDF "
                               "xmlns:rdf='http://www.w3.org/1999/02/22-rdf-syntax-ns#'>\n"
                               "<rdf:Description rdf:about='' xmlns:pdfaid='http://www.aiim.org/pdfa/ns/id/' "
                               "pdfaid:part='"
        + part.toStdString() + "' pdfaid:conformance='" + conformance.toStdString()
        + "'/></rdf:RDF></x:xmpmeta>\n<?xpacket end='w'?>\n";

    return rewrite(input, output, [&packet](QPDF &pdf) {
        QPDFObjectHandle metadata = QPDFObjectHandle::newStream(&pdf, packet);
        metadata.getDict().replaceKey("/Type", QPDFObjectHandle::newName("/Metadata"));
        metadata.getDict().replaceKey("/Subtype", QPDFObjectHandle::newName("/XML"));
        pdf.getRoot().replaceKey("/Metadata", metadata);
    });
}

/** The raw XMP packet of @p path, or empty when it has none. */
QString xmpOf(const QString &path)
{
    try {
        QPDF pdf;
        PdfFile::open(pdf, path);
        QPDFObjectHandle metadata = pdf.getRoot().getKey("/Metadata");
        if (!metadata.isStream()) {
            return {};
        }
        std::shared_ptr<Buffer> buffer = metadata.getStreamData();
        return QString::fromUtf8(reinterpret_cast<const char *>(buffer->getBuffer()), qsizetype(buffer->getSize()));
    } catch (const std::exception &) {
        return {};
    }
}

/** One /Info entry of @p path, as text. */
QString infoOf(const QString &path, const char *key)
{
    try {
        QPDF pdf;
        PdfFile::open(pdf, path);
        QPDFObjectHandle info = pdf.getTrailer().getKey("/Info");
        if (!info.isDictionary() || !info.hasKey(key)) {
            return {};
        }
        QPDFObjectHandle value = info.getKey(key);
        return value.isString() ? QString::fromStdString(value.getUTF8Value()) : QString();
    } catch (const std::exception &) {
        return {};
    }
}

/** Problems mentioning @p needle, which is how a font name is looked for. */
int problemsMentioning(const Archival::Findings &findings, const QString &needle)
{
    int count = 0;
    for (const QString &problem : findings.problems) {
        if (problem.contains(needle)) {
            ++count;
        }
    }
    return count;
}

} // namespace

class TestArchival : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();

    void describesEveryLevel();
    void availabilityMatchesTheSystem();
    void admitsWhatItCannotCheck();

    void findsAnUnembeddedFont();
    void findsEncryption();
    void findsJavaScript();
    void findsAnAttachmentBelowPdfA3();
    void allowsAnAttachmentInPdfA3();
    void readsTheClaimedLevelFromXmp();
    void doesNotTrustAClaimOnItsOwn();
    void reportsAnUnreadableFile();

    void refusesMissingInput();
    void refusesAnUnknownColourProfile();
    void reportsMissingGhostscript();

    void convertsAndStampsTheLevel();
    void convertsToEachLevel();
    void convertEmbedsTheFonts();
    void convertKeepsTheGivenTitleAndAuthor();
    void leavesNoTemporaryFiles();

private:
    /** Skips the current test unless Ghostscript is there to do the work. */
    bool requireGhostscript();

    QTemporaryDir m_dir;
    QString m_sample;
    QString m_textHeavy;
};

void TestArchival::initTestCase()
{
    KLocalizedString::setApplicationDomain("pdf-smithy");

    QVERIFY(m_dir.isValid());
    m_sample = m_dir.filePath(QStringLiteral("sample.pdf"));
    QVERIFY(test::writeSamplePdf(m_sample, 3));

    // Base-14 Helvetica with no font file anywhere in the document: the
    // commonest reason a perfectly ordinary PDF is not archival.
    m_textHeavy = m_dir.filePath(QStringLiteral("text.pdf"));
    QVERIFY(test::writeTextHeavyPdf(m_textHeavy, 2));
}

bool TestArchival::requireGhostscript()
{
    if (Archival::isAvailable()) {
        return true;
    }
    QTest::qSkip("Ghostscript is not installed", __FILE__, __LINE__);
    return false;
}

void TestArchival::describesEveryLevel()
{
    for (const Archival::Level level : { Archival::Level::PdfA1b, Archival::Level::PdfA2b, Archival::Level::PdfA3b }) {
        QVERIFY(!Archival::describe(level).isEmpty());
    }
    // The three descriptions have to be distinguishable, or the choice is not a
    // choice.
    QVERIFY(Archival::describe(Archival::Level::PdfA1b) != Archival::describe(Archival::Level::PdfA2b));
    QVERIFY(Archival::describe(Archival::Level::PdfA2b) != Archival::describe(Archival::Level::PdfA3b));
}

void TestArchival::availabilityMatchesTheSystem()
{
    QCOMPARE(Archival::isAvailable(), test::haveGhostscript());
}

void TestArchival::admitsWhatItCannotCheck()
{
    // The honesty is part of the feature: an inspection that let a user believe
    // it was a validation would be worse than no inspection at all.
    QVERIFY(!Archival::inspectionLimitations().isEmpty());
    for (const QString &note : Archival::inspectionLimitations()) {
        QVERIFY(!note.isEmpty());
    }
}

void TestArchival::findsAnUnembeddedFont()
{
    QString error;
    const Archival::Findings findings = Archival::inspect(m_textHeavy, &error);

    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(!findings.problems.isEmpty());
    // The font's name has to appear, because "a font is not embedded" leaves
    // the user with nothing to go and fix.
    QCOMPARE(problemsMentioning(findings, QStringLiteral("Helvetica")), 1);
    QVERIFY(!findings.looksArchival);
    QVERIFY(findings.claimedLevel.isEmpty());
}

void TestArchival::findsEncryption()
{
    const QString locked = m_dir.filePath(QStringLiteral("locked.pdf"));
    QVERIFY(rewrite(m_sample, locked, [](QPDF &) { }, /*encrypt=*/true));

    // Counted rather than matched on wording: the suite runs under a German
    // locale too, and asserting on English sentences would fail there for the
    // wrong reason.
    const qsizetype plain = Archival::inspect(m_sample, nullptr).problems.size();
    const Archival::Findings findings = Archival::inspect(locked, nullptr);

    QCOMPARE(findings.problems.size(), plain + 1);
    QVERIFY(!findings.looksArchival);
}

void TestArchival::findsJavaScript()
{
    const QString scripted = m_dir.filePath(QStringLiteral("scripted.pdf"));
    QVERIFY(addJavaScript(m_sample, scripted));

    const qsizetype plain = Archival::inspect(m_sample, nullptr).problems.size();
    QCOMPARE(Archival::inspect(scripted, nullptr).problems.size(), plain + 1);
}

void TestArchival::findsAnAttachmentBelowPdfA3()
{
    const QString withFile = m_dir.filePath(QStringLiteral("attached.pdf"));
    QVERIFY(addAttachment(m_sample, withFile));

    const qsizetype plain = Archival::inspect(m_sample, nullptr).problems.size();
    QCOMPARE(Archival::inspect(withFile, nullptr).problems.size(), plain + 1);
}

void TestArchival::allowsAnAttachmentInPdfA3()
{
    const QString withFile = m_dir.filePath(QStringLiteral("attached-plain.pdf"));
    QVERIFY(addAttachment(m_sample, withFile));

    const QString asTwo = m_dir.filePath(QStringLiteral("attached-a2.pdf"));
    const QString asThree = m_dir.filePath(QStringLiteral("attached-a3.pdf"));
    QVERIFY(claimLevel(withFile, asTwo, QStringLiteral("2"), QStringLiteral("B")));
    QVERIFY(claimLevel(withFile, asThree, QStringLiteral("3"), QStringLiteral("B")));

    // PDF/A-3 exists so that a document can carry its source data along, so the
    // very same file is a fault at one level and correct at the next.
    QCOMPARE(Archival::inspect(asThree, nullptr).problems.size(),
             Archival::inspect(asTwo, nullptr).problems.size() - 1);
}

void TestArchival::readsTheClaimedLevelFromXmp()
{
    const QString claiming = m_dir.filePath(QStringLiteral("claiming.pdf"));
    QVERIFY(claimLevel(m_sample, claiming, QStringLiteral("2"), QStringLiteral("b")));

    // Lower case in the packet, upper case in the designation: PDF/A-2B is how
    // the standard writes it.
    QCOMPARE(Archival::inspect(claiming, nullptr).claimedLevel, QStringLiteral("PDF/A-2B"));
}

void TestArchival::doesNotTrustAClaimOnItsOwn()
{
    // The sample has an unembedded font. Stamping the XMP does not change that,
    // and a file that claims the standard while breaking it must not be waved
    // through.
    const QString lying = m_dir.filePath(QStringLiteral("lying.pdf"));
    QVERIFY(claimLevel(m_textHeavy, lying, QStringLiteral("1"), QStringLiteral("B")));

    const Archival::Findings findings = Archival::inspect(lying, nullptr);
    QCOMPARE(findings.claimedLevel, QStringLiteral("PDF/A-1B"));
    QVERIFY(!findings.problems.isEmpty());
    QVERIFY(!findings.looksArchival);
}

void TestArchival::reportsAnUnreadableFile()
{
    QString error;
    const Archival::Findings findings = Archival::inspect(m_dir.filePath(QStringLiteral("nope.pdf")), &error);

    QVERIFY(!error.isEmpty());
    QVERIFY(!findings.looksArchival);
    QVERIFY(findings.problems.isEmpty());
}

void TestArchival::refusesMissingInput()
{
    QString error;
    QVERIFY(!Archival::convert(m_dir.filePath(QStringLiteral("nope.pdf")), m_dir.filePath(QStringLiteral("out.pdf")),
                               Archival::Options {}, nullptr, &error));
    QVERIFY(!error.isEmpty());
}

void TestArchival::refusesAnUnknownColourProfile()
{
    if (!requireGhostscript()) {
        return;
    }

    Archival::Options options;
    options.iccProfilePath = m_dir.filePath(QStringLiteral("not-a-profile.icc"));

    QString error;
    QVERIFY(!Archival::convert(m_sample, m_dir.filePath(QStringLiteral("bad-icc.pdf")), options, nullptr, &error));
    QVERIFY(!error.isEmpty());
    // A silently ignored profile would mean the user believes their colours are
    // pinned down when they are not.
    QVERIFY(error.contains(QStringLiteral("not-a-profile.icc")));
}

void TestArchival::reportsMissingGhostscript()
{
    if (Archival::isAvailable()) {
        QSKIP("Ghostscript is installed, so the missing-tool path cannot be exercised here");
    }

    QString error;
    QVERIFY(
        !Archival::convert(m_sample, m_dir.filePath(QStringLiteral("x.pdf")), Archival::Options {}, nullptr, &error));
    // The message has to name the package rather than only say "failed".
    QVERIFY(error.contains(QStringLiteral("ghostscript"), Qt::CaseInsensitive));
}

void TestArchival::convertsAndStampsTheLevel()
{
    if (!requireGhostscript()) {
        return;
    }

    const QString out = m_dir.filePath(QStringLiteral("archival.pdf"));

    Archival::Options options;
    options.level = Archival::Level::PdfA2b;

    Archival::Report report;
    QString error;
    QVERIFY2(Archival::convert(m_textHeavy, out, options, &report, &error), qPrintable(error));
    QVERIFY(report.converted);
    QCOMPARE(test::pageCountOf(out), 2);

    // Read out of the file itself, not out of the report: the identification is
    // the one thing an archive looks at, and a report saying it happened is not
    // the same as it having happened.
    const QString xmp = xmpOf(out);
    QVERIFY2(!xmp.isEmpty(), "the converted file has no XMP packet at all");
    QVERIFY2(xmp.contains(QStringLiteral("pdfaid:part")), qPrintable(xmp.left(400)));
    QVERIFY(xmp.contains(QStringLiteral("2")));

    const Archival::Findings findings = Archival::inspect(out, nullptr);
    QCOMPARE(findings.claimedLevel, QStringLiteral("PDF/A-2B"));
    QVERIFY2(findings.looksArchival, qPrintable(findings.problems.join(QLatin1Char('\n'))));
}

void TestArchival::convertsToEachLevel()
{
    if (!requireGhostscript()) {
        return;
    }

    const QVector<std::pair<Archival::Level, QString>> wanted {
        { Archival::Level::PdfA1b, QStringLiteral("PDF/A-1B") },
        { Archival::Level::PdfA2b, QStringLiteral("PDF/A-2B") },
        { Archival::Level::PdfA3b, QStringLiteral("PDF/A-3B") },
    };

    for (const auto &[level, designation] : wanted) {
        const QString out = m_dir.filePath(QStringLiteral("level-%1.pdf").arg(designation.right(2)));

        Archival::Options options;
        options.level = level;

        QString error;
        QVERIFY2(Archival::convert(m_sample, out, options, nullptr, &error), qPrintable(error));
        QCOMPARE(Archival::inspect(out, nullptr).claimedLevel, designation);
    }
}

void TestArchival::convertEmbedsTheFonts()
{
    if (!requireGhostscript()) {
        return;
    }

    const Archival::Findings before = Archival::inspect(m_textHeavy, nullptr);
    QCOMPARE(problemsMentioning(before, QStringLiteral("Helvetica")), 1);

    const QString out = m_dir.filePath(QStringLiteral("embedded.pdf"));
    Archival::Report report;
    QString error;
    QVERIFY2(Archival::convert(m_textHeavy, out, Archival::Options {}, &report, &error), qPrintable(error));

    // The whole point of the exercise: the glyphs now travel with the document.
    const Archival::Findings after = Archival::inspect(out, nullptr);
    QCOMPARE(problemsMentioning(after, QStringLiteral("Helvetica")), 0);
    QVERIFY(after.problems.isEmpty());

    // And the report has to say what was done, or the user has no way of
    // knowing their document was rewritten.
    QVERIFY(!report.changes.isEmpty());
}

void TestArchival::convertKeepsTheGivenTitleAndAuthor()
{
    if (!requireGhostscript()) {
        return;
    }

    Archival::Options options;
    // Umlauts and a dash outside Latin-1 on purpose: the document information
    // travels through a PostScript prologue, and anything that writes it as
    // plain bytes mangles exactly this.
    options.title = QStringLiteral("Größenänderung – Bericht");
    options.author = QStringLiteral("Tom Büng");

    const QString out = m_dir.filePath(QStringLiteral("titled.pdf"));
    QString error;
    QVERIFY2(Archival::convert(m_sample, out, options, nullptr, &error), qPrintable(error));

    QCOMPARE(infoOf(out, "/Title"), options.title);
    QCOMPARE(infoOf(out, "/Author"), options.author);
}

void TestArchival::leavesNoTemporaryFiles()
{
    if (!requireGhostscript()) {
        return;
    }

    const QString out = m_dir.filePath(QStringLiteral("clean.pdf"));
    QString error;
    QVERIFY2(Archival::convert(m_sample, out, Archival::Options {}, nullptr, &error), qPrintable(error));

    const QStringList leftovers
        = QDir(m_dir.path()).entryList({ QStringLiteral(".pdf-smithy-*") }, QDir::Files | QDir::Hidden);
    QVERIFY2(leftovers.isEmpty(), qPrintable(leftovers.join(QLatin1Char(' '))));
}

QTEST_GUILESS_MAIN(TestArchival)

#include "tst_archival.moc"
