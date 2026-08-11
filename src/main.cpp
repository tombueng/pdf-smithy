/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "config.h"
#include "ui/MainWindow.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QIcon>
#include <QTextStream>

#include <KAboutData>
#include <KCrash>
#include <KLocalizedString>

#include <clocale>

int main(int argc, char **argv)
{
    // PDF is written in the C locale and QPDF parses it with strtod, so a
    // German or French system would read every fractional number in every
    // document as zero. Our own reads go through PdfGeometry::numericValue and
    // are safe either way; QPDF's internal arithmetic (page placement,
    // matrices) is not, so the process is put into the C locale for numbers
    // only. Qt formats through QLocale and is unaffected.
    std::setlocale(LC_NUMERIC, "C");

    QApplication application(argc, argv);

    KLocalizedString::setApplicationDomain(PS_NAME);

    KAboutData about(QStringLiteral(PS_NAME), QStringLiteral(PS_DISPLAY), QStringLiteral(PS_VERSION),
                     i18n("Edit, merge, split, sign and OCR PDF documents, entirely on your own machine."),
                     KAboutLicense::GPL_V3, i18n("© 2026 Tom Bueng"), QString(),
                     QStringLiteral("https://github.com/tombueng/pdf-smithy"));

    about.addAuthor(QStringLiteral("Tom Bueng"), i18nc("@info:credit", "Author and maintainer"),
                    QStringLiteral("tombueng@gmail.com"));

    // Ties the running process to its .desktop file, which is what lets Plasma
    // group windows, show the right icon and offer jump-list actions.
    about.setDesktopFileName(QStringLiteral(PS_APPID));
    about.setOrganizationDomain(QByteArrayLiteral("github.io"));

    KAboutData::setApplicationData(about);
    KCrash::initialize();

    QApplication::setWindowIcon(QIcon::fromTheme(QStringLiteral(PS_APPID)));

    QCommandLineParser parser;
    about.setupCommandLine(&parser);
    parser.addPositionalArgument(QStringLiteral("file"), i18n("PDF documents to open."), QStringLiteral("[file…]"));

    // Parsed rather than processed, because process() answers --help and
    // --version by calling ::exit() from inside itself. That tears down the
    // static objects while the platform plugin still holds its thread-local
    // storage, and Qt says so on the way out: two "QThreadStorage: entry
    // destroyed before end of thread" lines on every `--version`. Returning
    // from main() instead lets the application come apart in the right order.
    QTextStream output(stdout);
    if (!parser.parse(QApplication::arguments())) {
        QTextStream(stderr) << parser.errorText() << Qt::endl << Qt::endl << parser.helpText();
        return 1;
    }
    if (parser.isSet(QStringLiteral("help"))) {
        output << parser.helpText();
        return 0;
    }
    if (parser.isSet(QStringLiteral("version"))) {
        output << QStringLiteral(PS_NAME) << u' ' << QStringLiteral(PS_VERSION) << Qt::endl;
        return 0;
    }
    about.processCommandLine(&parser);

    auto *window = new ps::MainWindow;
    window->show();

    const QStringList files = parser.positionalArguments();
    if (!files.isEmpty()) {
        window->openFile(files.constFirst());
    }

    return QApplication::exec();
}
