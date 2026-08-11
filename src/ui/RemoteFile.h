/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#pragma once

#include <QString>
#include <QTemporaryDir>
#include <QUrl>

class QWidget;

namespace ps {

/**
 * Documents that live somewhere other than this disk.
 *
 * The desktop file advertises sftp, smb and webdav, so a PDF on a network
 * share has to actually open: a protocol list that lies is worse than one
 * that is short.
 *
 * The engine works on local paths by design: QPDF and Poppler both want a file
 * they can seek in, and streaming a PDF over SMB page by page would be
 * miserable. So a remote document is fetched once into a scratch directory,
 * edited locally, and copied back on save. Nothing about that is visible to
 * the rest of the application, which keeps dealing in plain paths.
 */
class RemoteFile
{
public:
    /**
     * Makes @p url available locally, downloading it if necessary.
     *
     * Returns the local path, or an empty string with @p error set. Blocks
     * behind a progress dialog parented to @p parent; a document the user just
     * asked for is worth waiting for, and KIO shows what it is doing.
     */
    static QString fetch(const QUrl &url, QWidget *parent, QString *error);

    /** Copies @p localPath back to @p url, overwriting what is there. */
    static bool store(const QString &localPath, const QUrl &url, QWidget *parent, QString *error);

    /** True when the URL needs no fetching at all. */
    static bool isLocal(const QUrl &url);

    /** Where fetched copies live for the lifetime of the process. */
    static QString scratchDirectory();
};

} // namespace ps
