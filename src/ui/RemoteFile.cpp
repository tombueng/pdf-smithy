/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "RemoteFile.h"

#include <QDir>
#include <QFileInfo>
#include <QWidget>

#include <KIO/FileCopyJob>
#include <KIO/JobUiDelegate>
#include <KIO/JobUiDelegateFactory>
#include <KJobWidgets>
#include <KLocalizedString>

namespace ps {

namespace {

/**
 * One scratch directory for the whole process.
 *
 * Fetched documents have to outlive the operation that downloaded them (the
 * document keeps the file open until it is closed), so this deliberately lives
 * as long as the application does.
 */
QTemporaryDir &scratch()
{
    static QTemporaryDir directory(QDir::tempPath() + QLatin1String("/pdf-smithy-remote-XXXXXX"));
    return directory;
}

} // namespace

bool RemoteFile::isLocal(const QUrl &url)
{
    return url.isLocalFile() || url.scheme().isEmpty();
}

QString RemoteFile::scratchDirectory()
{
    return scratch().path();
}

QString RemoteFile::fetch(const QUrl &url, QWidget *parent, QString *error)
{
    if (isLocal(url)) {
        return url.toLocalFile();
    }

    if (!scratch().isValid()) {
        if (error) {
            *error = i18n("No scratch space is available for downloading.");
        }
        return {};
    }

    // Keep the visible name: the title bar, the recent-files list and any
    // "save as" default all read better as "contract.pdf" than as a hash.
    QString name = QFileInfo(url.path()).fileName();
    if (name.isEmpty()) {
        name = QStringLiteral("document.pdf");
    }

    QString target = scratch().filePath(name);
    for (int attempt = 1; QFileInfo::exists(target); ++attempt) {
        target = scratch().filePath(QStringLiteral("%1-%2").arg(attempt).arg(name));
    }

    KIO::FileCopyJob *job = KIO::file_copy(url, QUrl::fromLocalFile(target), -1, KIO::Overwrite);
    KJobWidgets::setWindow(job, parent);
    job->setUiDelegate(KIO::createDefaultJobUiDelegate(KJobUiDelegate::AutoErrorHandlingEnabled, parent));

    if (!job->exec()) {
        if (error) {
            *error = job->errorString().isEmpty() ? i18n("“%1” could not be downloaded.", url.toDisplayString())
                                                  : job->errorString();
        }
        return {};
    }

    return target;
}

bool RemoteFile::store(const QString &localPath, const QUrl &url, QWidget *parent, QString *error)
{
    if (isLocal(url)) {
        return true; // written in place already
    }

    KIO::FileCopyJob *job = KIO::file_copy(QUrl::fromLocalFile(localPath), url, -1, KIO::Overwrite);
    KJobWidgets::setWindow(job, parent);
    job->setUiDelegate(KIO::createDefaultJobUiDelegate(KJobUiDelegate::AutoErrorHandlingEnabled, parent));

    if (!job->exec()) {
        if (error) {
            *error = job->errorString().isEmpty() ? i18n("“%1” could not be written.", url.toDisplayString())
                                                  : job->errorString();
        }
        return false;
    }

    return true;
}

} // namespace ps
