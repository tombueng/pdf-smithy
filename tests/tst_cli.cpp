/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "TestPdf.h"

#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QTest>

using namespace Qt::Literals::StringLiterals;

/**
 * The command line as a whole, rather than any one command.
 *
 * Every area of the command line now lives in its own translation unit and
 * registers its own options, which keeps the areas from having to edit one
 * shared table, and moves the failure mode. Two areas can now claim the same
 * option name, and Qt's answer to that is a warning on standard error and an
 * option that silently belongs to whichever registered first. Nothing else in
 * the build notices. So the checks here are deliberately about the seams:
 * that no name is claimed twice, that every verb the help advertises is a verb
 * the dispatcher understands, and that the whole thing still says something
 * useful when given nothing.
 */
class TestCli : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();

    void startsWithoutComplainingAboutItself();
    void everyAdvertisedVerbIsUnderstood();
    void refusesAnUnknownVerbAndSaysSo();
    void reportsOnADocumentItWasGiven();
    void survivesAVerbWithoutItsArguments();
    void survivesAVerbWithoutItsArguments_data();

private:
    /** Runs the command line, returning its exit code and filling the streams. */
    int run(const QStringList &arguments, QString *standardOut = nullptr, QString *standardError = nullptr);

    /** The verbs the help text advertises. */
    QStringList advertisedVerbs();

    QString m_binary;
    QTemporaryDir m_directory;
};

void TestCli::initTestCase()
{
    QVERIFY(m_directory.isValid());

    // Beside the test binary, which is where the build puts it. Looking it up
    // rather than hard-coding a path keeps this working from ctest, from the
    // build directory and from an installed tree alike.
    const QString here = QCoreApplication::applicationDirPath();
    m_binary = here + u"/pdf-smithy-cli"_s;
    if (!QFileInfo::exists(m_binary)) {
        QSKIP("this build has no command-line companion");
    }
}

int TestCli::run(const QStringList &arguments, QString *standardOut, QString *standardError)
{
    QProcess process;
    // The command line is answered in whatever language the tester runs in,
    // and these cases read the answers. Pinning the language keeps a German
    // desktop from failing a test that an English one passes.
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(u"LC_ALL"_s, u"C"_s);
    environment.insert(u"LANGUAGE"_s, u"en"_s);
    process.setProcessEnvironment(environment);

    process.start(m_binary, arguments);
    if (!process.waitForFinished(30000)) {
        process.kill();
        return -1;
    }
    if (standardOut) {
        *standardOut = QString::fromUtf8(process.readAllStandardOutput());
    }
    if (standardError) {
        *standardError = QString::fromUtf8(process.readAllStandardError());
    }
    return process.exitCode();
}

QStringList TestCli::advertisedVerbs()
{
    QString text;
    QString errors;
    run({ u"--help"_s }, &text, &errors);
    text += errors;

    // Only the block under "Commands:", which is also the block
    // tools/generate-manpages.py reads. Scanning the whole help would pick up
    // the "command" positional argument and the option descriptions, and then
    // this test would report a verb that was never claimed to be one.
    const qsizetype start = text.indexOf(u"Commands:"_s);
    if (start < 0) {
        return {};
    }
    const QString block = text.mid(start + 9).section(u"\n\n"_s, 0, 0);

    static const QRegularExpression line(u"^\\s{2,}([a-z][a-z-]{1,})\\s"_s, QRegularExpression::MultilineOption);
    QStringList verbs;
    QRegularExpressionMatchIterator matches = line.globalMatch(block);
    while (matches.hasNext()) {
        const QString verb = matches.next().captured(1);
        if (!verbs.contains(verb)) {
            verbs.append(verb);
        }
    }
    return verbs;
}

void TestCli::startsWithoutComplainingAboutItself()
{
    QString text;
    QString errors;
    QCOMPARE(run({ u"--help"_s }, &text, &errors), 0);

    // Qt reports a name claimed twice on standard error and then carries on, so
    // the only symptom of two areas colliding is this line. It is the reason
    // this test exists.
    QVERIFY2(!errors.contains(u"already having an option"_s),
             qPrintable(u"an option name is registered twice: %1"_s.arg(errors)));
    QVERIFY2(!errors.contains(u"QCommandLineParser"_s), qPrintable(errors));
    QVERIFY(text.contains(u"Usage"_s) || text.contains(u"usage"_s));
}

void TestCli::everyAdvertisedVerbIsUnderstood()
{
    const QStringList verbs = advertisedVerbs();
    QVERIFY2(verbs.size() > 10, qPrintable(u"only %1 verbs found in the help"_s.arg(verbs.size())));

    QStringList unreachable;
    for (const QString &verb : verbs) {
        QString errors;
        run({ verb }, nullptr, &errors);
        // Called with no arguments a verb should complain about its arguments,
        // never about itself. "Unknown command" here means the help promises
        // something the dispatcher was never told about.
        if (errors.contains(u"Unknown command"_s)) {
            unreachable.append(verb);
        }
    }
    QVERIFY2(unreachable.isEmpty(), qPrintable(u"advertised but not dispatched: %1"_s.arg(unreachable.join(u", "_s))));
}

void TestCli::refusesAnUnknownVerbAndSaysSo()
{
    QString errors;
    const int code = run({ u"definitely-not-a-command"_s }, nullptr, &errors);
    QCOMPARE(code, 2); // UsageError
    QVERIFY(errors.contains(u"Unknown command"_s));
}

void TestCli::reportsOnADocumentItWasGiven()
{
    const QString document = m_directory.filePath(u"three.pdf"_s);
    QVERIFY(ps::test::writeSamplePdf(document, 3));

    QString text;
    QCOMPARE(run({ u"info"_s, document }, &text), 0);
    QVERIFY2(text.contains(u"3"_s), qPrintable(text));
}

void TestCli::survivesAVerbWithoutItsArguments_data()
{
    QTest::addColumn<QString>("verb");
    for (const QString &verb : advertisedVerbs()) {
        QTest::newRow(qPrintable(verb)) << verb;
    }
}

void TestCli::survivesAVerbWithoutItsArguments()
{
    QFETCH(QString, verb);

    QString errors;
    const int code = run({ verb }, nullptr, &errors);

    // A missing argument is a usage error, not a crash and not a success. -1 is
    // this test's own marker for a process that had to be killed; a negative or
    // above-128 code from the process itself is a signal, which is the crash
    // this case is looking for.
    QVERIFY2(code >= 0, qPrintable(u"“%1” never finished"_s.arg(verb)));
    QVERIFY2(code < 128, qPrintable(u"“%1” died on a signal (%2)"_s.arg(verb).arg(code)));
    QVERIFY2(code != 0 || !errors.isEmpty(),
             qPrintable(u"“%1” reported success without being given anything to do"_s.arg(verb)));
}

QTEST_MAIN(TestCli)

#include "tst_cli.moc"
