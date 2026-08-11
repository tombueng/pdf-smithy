/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#include "core/Compare.h"
#include "render/PopplerBackend.h"

#include <KLocalizedString>

#include <QImage>
#include <QSet>
#include <QTemporaryDir>
#include <QTest>

#include <qpdf/QPDF.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>
#include <qpdf/QPDFWriter.hh>

#include <string>

using namespace ps;

namespace {

/** One page of a fixture: lines of text from the top, and an optional black box. */
struct PageSpec {
    QStringList lines;

    /** In points from the bottom left. Empty for a page with no box on it. */
    QRect box;
};

/**
 * A document whose text and shapes are dictated line by line.
 *
 * Comparison fixtures have to differ from one another in exactly one respect at
 * a time (one word, one line break, one rectangle), which is what the generated
 * documents elsewhere in the suite cannot offer. Every number written here is an
 * integer, so no fixture can be the reason a test fails under a comma locale.
 */
bool writePdf(const QString &path, const QVector<PageSpec> &pages)
{
    try {
        QPDF pdf;
        pdf.emptyPDF();
        QPDFPageDocumentHelper helper(pdf);

        for (const PageSpec &spec : pages) {
            std::string body = "BT /F1 14 Tf\n";
            int baseline = 720;
            for (const QString &line : spec.lines) {
                body += "1 0 0 1 72 " + std::to_string(baseline) + " Tm (" + line.toStdString() + ") Tj\n";
                baseline -= 24;
            }
            body += "ET\n";

            if (!spec.box.isEmpty()) {
                body += std::to_string(spec.box.x()) + " " + std::to_string(spec.box.y()) + " "
                    + std::to_string(spec.box.width()) + " " + std::to_string(spec.box.height()) + " re f\n";
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

            helper.addPage(QPDFPageObjectHelper(pdf.makeIndirectObject(page)), false);
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

// The ids Compare registers its two documents under. Named here so that the
// tests can prove they were handed back.
constexpr int leftSourceId = 900'001;
constexpr int rightSourceId = 900'002;

const PageSpec firstPage { { QStringLiteral("Alpha Beta Gamma"), QStringLiteral("Delta Epsilon") }, {} };
const PageSpec secondPage { { QStringLiteral("Zeta Eta Theta"), QStringLiteral("Iota Kappa") }, {} };

} // namespace

/**
 * Telling two versions of a document apart.
 *
 * The cases that earn their keep here are the ones a naive comparison gets
 * wrong: words that only changed places, a page that reads the same and prints
 * differently, and a line break mistaken for an edit. Every verdict is read back
 * out of the report rather than assumed from how the fixtures were built.
 */
class TestCompare : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();

    void callsAFileComparedWithItselfIdentical();
    void namesTheWordThatWasReplaced();
    void noticesWordsThatOnlySwappedPlaces();
    void callsAPageThatMerelyLooksDifferentAVisualChange();
    void treatsALineBreakAsAnEditOnlyWhenWhitespaceCounts();
    void reportsSurplusPagesAsAddedOrRemoved();
    void putsTheTwoPagesSideBySideAndMarksTheDifference();
    void refusesWhatItCannotOpenAndTidiesUpAfterItself();
    void describesEveryOutcomeInWords();

private:
    QTemporaryDir m_dir;
    PopplerBackend m_backend;

    QString m_base; //!< Two pages of plain text
    QString m_edited; //!< As m_base, with one word on page two replaced
    QString m_reordered; //!< As m_base, with two words on page one swapped
    QString m_boxed; //!< As m_base, with a black rectangle on page one
    QString m_wrapped; //!< As m_base, with page one's first line broken in two
    QString m_longer; //!< m_base with two further pages after it
};

void TestCompare::initTestCase()
{
    KLocalizedString::setApplicationDomain("pdf-smithy");
    QVERIFY(m_dir.isValid());

    m_base = m_dir.filePath(QStringLiteral("base.pdf"));
    QVERIFY(writePdf(m_base, { firstPage, secondPage }));

    PageSpec editedSecond = secondPage;
    editedSecond.lines[0] = QStringLiteral("Zeta Eta Omega");
    m_edited = m_dir.filePath(QStringLiteral("edited.pdf"));
    QVERIFY(writePdf(m_edited, { firstPage, editedSecond }));

    PageSpec reorderedFirst = firstPage;
    reorderedFirst.lines[0] = QStringLiteral("Alpha Gamma Beta");
    m_reordered = m_dir.filePath(QStringLiteral("reordered.pdf"));
    QVERIFY(writePdf(m_reordered, { reorderedFirst, secondPage }));

    // Well below the text, so the words on the page are untouched by it.
    PageSpec boxedFirst = firstPage;
    boxedFirst.box = QRect(72, 300, 200, 100);
    m_boxed = m_dir.filePath(QStringLiteral("boxed.pdf"));
    QVERIFY(writePdf(m_boxed, { boxedFirst, secondPage }));

    PageSpec wrappedFirst;
    wrappedFirst.lines = { QStringLiteral("Alpha"), QStringLiteral("Beta Gamma"), QStringLiteral("Delta Epsilon") };
    m_wrapped = m_dir.filePath(QStringLiteral("wrapped.pdf"));
    QVERIFY(writePdf(m_wrapped, { wrappedFirst, secondPage }));

    const PageSpec third { { QStringLiteral("Lambda Mu Nu") }, {} };
    const PageSpec fourth { { QStringLiteral("Xi Omicron Pi") }, {} };
    m_longer = m_dir.filePath(QStringLiteral("longer.pdf"));
    QVERIFY(writePdf(m_longer, { firstPage, secondPage, third, fourth }));
}

void TestCompare::callsAFileComparedWithItselfIdentical()
{
    QString error;
    const Compare::Report report = Compare::run(&m_backend, m_base, m_base, Compare::Options(), &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));

    QCOMPARE(report.pages.size(), 2);
    QCOMPARE(report.changedPages, 0);
    QVERIFY(report.identical);

    for (int index = 0; index < report.pages.size(); ++index) {
        const Compare::PageResult &page = report.pages.at(index);
        QCOMPARE(page.leftPage, index);
        QCOMPARE(page.rightPage, index);
        QCOMPARE(page.change, Compare::Change::Same);
        QVERIFY(page.removedWords.isEmpty());
        QVERIFY(page.addedWords.isEmpty());
        // The same bytes through the same renderer: not "nearly" the same page.
        QVERIFY2(page.pixelDifference == 0.0, qPrintable(QString::number(page.pixelDifference, 'g', 6)));
    }

    // Both documents have to be off the renderer again. Were one left behind, a
    // later comparison would register over it or, worse, quietly render the
    // wrong file under that id.
    QVERIFY(m_backend.renderPage(leftSourceId, 0, 100).isNull());
    QVERIFY(m_backend.renderPage(rightSourceId, 0, 100).isNull());
}

void TestCompare::namesTheWordThatWasReplaced()
{
    QString error;
    const Compare::Report report = Compare::run(&m_backend, m_base, m_edited, Compare::Options(), &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));

    QCOMPARE(report.pages.size(), 2);
    QCOMPARE(report.changedPages, 1);
    QVERIFY(!report.identical);

    QCOMPARE(report.pages.constFirst().change, Compare::Change::Same);

    const Compare::PageResult &second = report.pages.at(1);
    QCOMPARE(second.leftPage, 1);
    QCOMPARE(second.rightPage, 1);
    QCOMPARE(second.change, Compare::Change::TextChanged);
    QCOMPARE(second.removedWords, QStringList { QStringLiteral("Theta") });
    QCOMPARE(second.addedWords, QStringList { QStringLiteral("Omega") });

    // The words it does not mention matter as much as the ones it does: a
    // comparison that lists the whole line has told the reader nothing.
    QVERIFY(!second.removedWords.contains(QStringLiteral("Zeta")));
    QVERIFY(!second.addedWords.contains(QStringLiteral("Eta")));
}

void TestCompare::noticesWordsThatOnlySwappedPlaces()
{
    QString error;
    const Compare::Report report = Compare::run(&m_backend, m_base, m_reordered, Compare::Options(), &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(report.pages.size(), 2);

    const Compare::PageResult &first = report.pages.constFirst();

    // "Alpha Beta Gamma" against "Alpha Gamma Beta". Both pages hold exactly the
    // same words, so any comparison built on sets or word counts calls this pair
    // identical, which is how a negation ends up on the wrong side of a clause
    // without anybody being told.
    QCOMPARE(first.change, Compare::Change::TextChanged);
    QCOMPARE(report.changedPages, 1);
    QVERIFY(!report.identical);

    const QSet<QString> removed(first.removedWords.begin(), first.removedWords.end());
    const QSet<QString> added(first.addedWords.begin(), first.addedWords.end());
    QCOMPARE(removed, added);
    QCOMPARE(removed.size(), 1);
    QVERIFY2(removed.contains(QStringLiteral("Beta")) || removed.contains(QStringLiteral("Gamma")),
             qPrintable(first.removedWords.join(QLatin1Char(' '))));

    // One word moved, so one word is reported on each side, not all three.
    QCOMPARE(first.removedWords.size(), 1);
    QCOMPARE(first.addedWords.size(), 1);
}

void TestCompare::callsAPageThatMerelyLooksDifferentAVisualChange()
{
    QString error;
    const Compare::Report report = Compare::run(&m_backend, m_base, m_boxed, Compare::Options(), &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(report.pages.size(), 2);
    QCOMPARE(report.changedPages, 1);

    const Compare::PageResult &first = report.pages.constFirst();
    QCOMPARE(first.change, Compare::Change::VisualOnly);
    QVERIFY(first.removedWords.isEmpty());
    QVERIFY(first.addedWords.isEmpty());

    // The rectangle is 200 x 100 points on a 612 x 792 page, a shade over four
    // per cent of it. A fraction far from that means the wrong thing was
    // measured, not that the tolerance was unlucky.
    QVERIFY2(first.pixelDifference > 0.03 && first.pixelDifference < 0.06,
             qPrintable(QString::number(first.pixelDifference, 'g', 6)));

    QCOMPARE(report.pages.at(1).change, Compare::Change::Same);
    QCOMPARE(report.pages.at(1).pixelDifference, 0.0);
}

void TestCompare::treatsALineBreakAsAnEditOnlyWhenWhitespaceCounts()
{
    // m_wrapped says the same five words on page one, with the first line broken
    // in two. The words are unchanged and in the same order; only the layout
    // moved, which is what re-saving a document through another writer does.
    QString error;
    Compare::Options options;
    const Compare::Report ignoring = Compare::run(&m_backend, m_base, m_wrapped, options, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(ignoring.pages.size(), 2);

    QCOMPARE(ignoring.pages.constFirst().change, Compare::Change::VisualOnly);
    QVERIFY2(ignoring.pages.constFirst().removedWords.isEmpty(),
             qPrintable(ignoring.pages.constFirst().removedWords.join(QLatin1Char(' '))));
    QVERIFY(ignoring.pages.constFirst().addedWords.isEmpty());

    options.ignoreWhitespace = false;
    const Compare::Report counting = Compare::run(&m_backend, m_base, m_wrapped, options, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(counting.pages.size(), 2);

    QCOMPARE(counting.pages.constFirst().change, Compare::Change::TextChanged);
    QVERIFY(!counting.pages.constFirst().removedWords.isEmpty());

    // And with whitespace significant, a page that genuinely did not change must
    // still come back unchanged: the separators travel with the words, so it
    // would be easy for every page to start reporting differences.
    QCOMPARE(counting.pages.at(1).change, Compare::Change::Same);
    QVERIFY(counting.pages.at(1).removedWords.isEmpty());
    QVERIFY(counting.pages.at(1).addedWords.isEmpty());
}

void TestCompare::reportsSurplusPagesAsAddedOrRemoved()
{
    QString error;
    const Compare::Report grown = Compare::run(&m_backend, m_base, m_longer, Compare::Options(), &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));

    QCOMPARE(grown.pages.size(), 4);
    QCOMPARE(grown.changedPages, 2);
    QVERIFY(!grown.identical);
    QCOMPARE(grown.pages.constFirst().change, Compare::Change::Same);
    QCOMPARE(grown.pages.at(1).change, Compare::Change::Same);

    const Compare::PageResult &third = grown.pages.at(2);
    QCOMPARE(third.leftPage, -1);
    QCOMPARE(third.rightPage, 2);
    QCOMPARE(third.change, Compare::Change::Added);
    QVERIFY(third.removedWords.isEmpty());
    QCOMPARE(third.addedWords, QStringList({ QStringLiteral("Lambda"), QStringLiteral("Mu"), QStringLiteral("Nu") }));
    // A new page with words on it is not a blank sheet, and the report says so.
    QVERIFY2(third.pixelDifference > 0.0, qPrintable(QString::number(third.pixelDifference, 'g', 6)));

    QCOMPARE(grown.pages.at(3).change, Compare::Change::Added);
    QCOMPARE(grown.pages.at(3).rightPage, 3);

    // The same pair the other way round: what was added is now missing.
    const Compare::Report shrunk = Compare::run(&m_backend, m_longer, m_base, Compare::Options(), &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(shrunk.pages.size(), 4);
    QCOMPARE(shrunk.changedPages, 2);

    const Compare::PageResult &lost = shrunk.pages.at(2);
    QCOMPARE(lost.leftPage, 2);
    QCOMPARE(lost.rightPage, -1);
    QCOMPARE(lost.change, Compare::Change::Removed);
    QVERIFY(lost.addedWords.isEmpty());
    QCOMPARE(lost.removedWords, QStringList({ QStringLiteral("Lambda"), QStringLiteral("Mu"), QStringLiteral("Nu") }));
}

void TestCompare::putsTheTwoPagesSideBySideAndMarksTheDifference()
{
    const QImage picture = Compare::visualise(&m_backend, m_base, m_boxed, 0, 0, Compare::Options());
    QVERIFY(!picture.isNull());

    // A 612 x 792 page at a hundred dots to the inch is 850 x 1100, and there
    // are two of them beside each other.
    QVERIFY2(picture.width() > 1700, qPrintable(QString::number(picture.width())));
    QVERIFY2(picture.height() >= 1100, qPrintable(QString::number(picture.height())));

    int marked = 0;
    QRect extent;
    for (int y = 0; y < picture.height(); ++y) {
        for (int x = 0; x < picture.width(); ++x) {
            const QColor colour = picture.pixelColor(x, y);
            if (colour.red() - colour.green() > 30 && colour.red() - colour.blue() > 30) {
                ++marked;
                extent = extent.isNull() ? QRect(x, y, 1, 1) : extent.united(QRect(x, y, 1, 1));
            }
        }
    }

    // The rectangle is on the page, so it must be marked; and the left-hand page
    // is the original, which nothing has any business tinting.
    QVERIFY2(marked > 1000, qPrintable(QString::number(marked)));
    QVERIFY2(extent.left() > picture.width() / 2,
             qPrintable(QStringLiteral("marks reach into the left page at x=%1").arg(extent.left())));

    // Where the marks are, in the right-hand page: the box spans 300 to 400
    // points up a 792-point page, which is a little under half way down to a
    // little under two thirds down, and 72 to 272 points across it.
    QVERIFY2(extent.top() > picture.height() * 0.4 && extent.bottom() < picture.height() * 0.7,
             qPrintable(QStringLiteral("marks run from y=%1 to y=%2").arg(extent.top()).arg(extent.bottom())));
    QVERIFY2(extent.right() < picture.width() * 0.8,
             qPrintable(QStringLiteral("marks reach x=%1").arg(extent.right())));

    // A page that only one side has is drawn against blank paper rather than
    // refused.
    const QImage added = Compare::visualise(&m_backend, m_base, m_longer, -1, 2, Compare::Options());
    QVERIFY(!added.isNull());
    QVERIFY(added.width() > 1700);

    QVERIFY(m_backend.renderPage(leftSourceId, 0, 100).isNull());
    QVERIFY(m_backend.renderPage(rightSourceId, 0, 100).isNull());
}

void TestCompare::refusesWhatItCannotOpenAndTidiesUpAfterItself()
{
    QString error;
    const Compare::Report missing
        = Compare::run(&m_backend, m_dir.filePath(QStringLiteral("nothing.pdf")), m_base, Compare::Options(), &error);
    QVERIFY(missing.pages.isEmpty());
    QCOMPARE(missing.changedPages, 0);
    // Nothing was established, so nothing may be claimed.
    QVERIFY(!missing.identical);
    QVERIFY(!error.isEmpty());

    // Whichever side failed, neither document may be left on the renderer.
    QVERIFY(m_backend.renderPage(leftSourceId, 0, 100).isNull());
    QVERIFY(m_backend.renderPage(rightSourceId, 0, 100).isNull());

    error.clear();
    QVERIFY(Compare::run(nullptr, m_base, m_base, Compare::Options(), &error).pages.isEmpty());
    QVERIFY(!error.isEmpty());

    QVERIFY(Compare::visualise(nullptr, m_base, m_base, 0, 0, Compare::Options()).isNull());
    QVERIFY(Compare::visualise(&m_backend, m_base, m_base, -1, -1, Compare::Options()).isNull());

    // A page the document does not have is a failure, not a blank half: drawing
    // it empty would say the page is blank.
    QVERIFY(Compare::visualise(&m_backend, m_base, m_base, 0, 99, Compare::Options()).isNull());
    QVERIFY(m_backend.renderPage(leftSourceId, 0, 100).isNull());
    QVERIFY(m_backend.renderPage(rightSourceId, 0, 100).isNull());
}

void TestCompare::describesEveryOutcomeInWords()
{
    const QVector<Compare::Change> outcomes { Compare::Change::Same, Compare::Change::TextChanged,
                                              Compare::Change::VisualOnly, Compare::Change::Added,
                                              Compare::Change::Removed };

    QSet<QString> descriptions;
    for (const Compare::Change change : outcomes) {
        const QString text = Compare::describe(change);
        QVERIFY(!text.trimmed().isEmpty());
        descriptions.insert(text);
    }

    // Five outcomes, five sentences: two verdicts that read alike are two
    // verdicts nobody can act on.
    QCOMPARE(descriptions.size(), outcomes.size());
}

QTEST_MAIN(TestCompare)

#include "tst_compare.moc"
