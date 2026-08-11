/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "ToolSupport.h"

#include "MainWindow.h"
#include "core/Document.h"

#include <KLocalizedString>
#include <KMessageBox>

#include <QDialog>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QFontDatabase>
#include <QPlainTextEdit>
#include <QVBoxLayout>
#include <QWidget>

using namespace Qt::Literals::StringLiterals;

namespace ps::tools {

namespace {

/** The window @p widget lives in, so a tool can ask it to save or open. */
MainWindow *windowOf(QWidget *widget)
{
    for (QWidget *step = widget; step; step = step->parentWidget()) {
        if (auto *window = qobject_cast<MainWindow *>(step)) {
            return window;
        }
    }
    return nullptr;
}

} // namespace

QString savedPath(Document *document, QWidget *parent)
{
    if (!document || document->pageCount() == 0) {
        return {};
    }

    // The window is asked rather than the document, because the document's own
    // answer is only half of one: the layers over the page collect their work
    // and hand it over in a single step, so a page's worth of comments and
    // corrections leaves the undo stack clean and the document looking saved.
    // Every tool here reads a **file**, and a file written before that step is
    // the version from before the user's last hour.
    MainWindow *window = windowOf(parent);
    const bool waiting = window ? window->hasUnsavedWork() : document->isModified();
    if (!waiting && !document->filePath().isEmpty()) {
        return document->filePath();
    }
    if (!window) {
        return {};
    }

    // These tools read the file, not the model, so an unsaved change would
    // simply not be there, and the result would look like the tool ignoring
    // what the user just did.
    if (KMessageBox::questionTwoActions(
            parent, i18n("This has to be saved before the tool can work on it, because the tool reads the file."),
            i18nc("@title:window", "Save First?"), KStandardGuiItem::save(), KStandardGuiItem::cancel())
        != KMessageBox::PrimaryAction) {
        return {};
    }
    return window->saveForTools();
}

bool runProducing(Document *document, QWidget *parent, const QString &title, const QString &suffix,
                  const std::function<bool(const QString &output, QStringList *summary, QString *error)> &work)
{
    const QString input = savedPath(document, parent);
    if (input.isEmpty()) {
        return false;
    }

    const QFileInfo file(input);
    const QString output = file.absolutePath() + u'/' + file.completeBaseName() + suffix;

    QStringList text;
    QString error;
    if (!work(output, &text, &error)) {
        KMessageBox::error(parent, error.isEmpty() ? i18n("The tool did not say what went wrong.") : error, title);
        return false;
    }

    text += i18n("Written to %1.", QFileInfo(output).fileName());

    MainWindow *window = windowOf(parent);
    if (!window) {
        showReport(parent, title, text);
        return true;
    }

    if (KMessageBox::questionTwoActions(parent, text.join(u"\n\n"_s), title,
                                        KGuiItem(i18nc("@action:button", "Open the Result")),
                                        KGuiItem(i18nc("@action:button", "Leave It on Disk")))
        == KMessageBox::PrimaryAction) {
        window->openFile(output);
    }
    return true;
}

void showReport(QWidget *parent, const QString &title, const QStringList &lines, bool monospaced)
{
    QDialog dialog(parent);
    dialog.setWindowTitle(title);
    dialog.resize(760, 520);

    auto *layout = new QVBoxLayout(&dialog);
    auto *body = new QPlainTextEdit(&dialog);
    body->setReadOnly(true);
    body->setPlainText(lines.join(u'\n'));
    if (monospaced) {
        // A table of resolutions or widths only reads as a table in a font
        // whose digits are all one width.
        body->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        body->setLineWrapMode(QPlainTextEdit::NoWrap);
    }
    layout->addWidget(body);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);

    dialog.exec();
}

} // namespace ps::tools
