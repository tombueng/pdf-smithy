/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "Commands.h"

#include <KLocalizedString>

#include <QFileInfo>
#include <QLocale>
#include <QTextStream>

namespace ps::cli {

QTextStream &out()
{
    static QTextStream stream(stdout);
    return stream;
}

QTextStream &err()
{
    static QTextStream stream(stderr);
    return stream;
}

int fail(const QString &message, int code)
{
    err() << i18nc("@info error prefix on the command line", "Error: ") << message << Qt::endl;
    return code;
}

QString humanSize(qint64 bytes)
{
    return QLocale().formattedDataSize(bytes, 1, QLocale::DataSizeTraditionalFormat);
}

bool oneInputFile(const QStringList &arguments, const QString &verb, QString *path, int *code)
{
    if (arguments.size() != 1) {
        *code = fail(i18n("“%1” works on one document at a time.", verb), UsageError);
        return false;
    }
    const QFileInfo file(arguments.constFirst());
    if (!file.exists()) {
        *code = fail(i18n("“%1” does not exist.", arguments.constFirst()));
        return false;
    }
    if (!file.isReadable()) {
        *code = fail(i18n("“%1” cannot be read.", arguments.constFirst()));
        return false;
    }
    *path = file.absoluteFilePath();
    return true;
}

QString outputOr(const QString &given, const QString &input, const QString &suffix)
{
    if (!given.isEmpty()) {
        return given;
    }
    // Beside the original rather than over it: a command line that overwrites
    // its input by default is one bad flag away from losing the only copy.
    const QFileInfo file(input);
    return file.absolutePath() + u'/' + file.completeBaseName() + suffix;
}

} // namespace ps::cli
