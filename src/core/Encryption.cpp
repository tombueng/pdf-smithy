/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#include "Encryption.h"
#include "PdfFile.h"

#include <QFile>
#include <QFileInfo>
#include <QTemporaryFile>

#include <KLocalizedString>

#include <qpdf/QPDF.hh>
#include <qpdf/QPDFWriter.hh>

#include <cstdio>

namespace ps {

namespace {

qpdf_r3_print_e printingFlag(Encryption::Printing printing)
{
    switch (printing) {
    case Encryption::Printing::Allowed:
        return qpdf_r3p_full;
    case Encryption::Printing::LowResolutionOnly:
        return qpdf_r3p_low;
    case Encryption::Printing::Forbidden:
        return qpdf_r3p_none;
    }
    return qpdf_r3p_full;
}

QString scratchBeside(const QString &target, QString *error)
{
    QTemporaryFile temp(QFileInfo(target).absolutePath() + QLatin1String("/.pdf-smithy-lock-XXXXXX.pdf"));
    temp.setAutoRemove(false);
    if (!temp.open()) {
        if (error) {
            *error = i18n("Cannot write to “%1”.", QFileInfo(target).absolutePath());
        }
        return {};
    }
    const QString path = temp.fileName();
    temp.close();
    return path;
}

bool finish(const QString &tempPath, const QString &target, QString *error)
{
    QFile::setPermissions(tempPath, QFile::ReadOwner | QFile::WriteOwner | QFile::ReadGroup | QFile::ReadOther);
    if (::rename(QFile::encodeName(tempPath).constData(), QFile::encodeName(target).constData()) != 0) {
        QFile::remove(tempPath);
        if (error) {
            *error = i18n("Could not write “%1”.", target);
        }
        return false;
    }
    return true;
}

} // namespace

bool Encryption::isEncrypted(const QString &path)
{
    try {
        QPDF pdf;
        PdfFile::open(pdf, path);
        return pdf.isEncrypted();
    } catch (const std::exception &) {
        // Refusing to open without a password is itself the answer.
        return true;
    }
}

bool Encryption::canOpen(const QString &path, const QString &password)
{
    try {
        QPDF pdf;
        PdfFile::open(pdf, path, password);
        return true;
    } catch (const std::exception &) {
        return false;
    }
}

int Encryption::printingFlagFor(Printing printing)
{
    return static_cast<int>(printingFlag(printing));
}

bool Encryption::encrypt(const QString &inputPath, const QString &outputPath, const QString &userPassword,
                         const QString &ownerPassword, const Permissions &permissions, const QString &openPassword,
                         QString *error)
{
    if (userPassword.isEmpty() && ownerPassword.isEmpty()) {
        if (error) {
            *error = i18n("A password is required.");
        }
        return false;
    }

    const QString tempPath = scratchBeside(outputPath, error);
    if (tempPath.isEmpty()) {
        return false;
    }

    // An empty owner password leaves the permissions trivially removable, so
    // it falls back to the user password rather than to nothing.
    const QByteArray user = userPassword.toUtf8();
    const QByteArray owner = (ownerPassword.isEmpty() ? userPassword : ownerPassword).toUtf8();

    try {
        QPDF pdf;
        PdfFile::open(pdf, inputPath, openPassword);

        QPDFWriter writer(pdf);
        writer.setOutputFilename(QFile::encodeName(tempPath).constData());
        // No deterministic ID here, deliberately. The file ID feeds into the
        // encryption key, so making it reproducible would make the key
        // reproducible. QPDF refuses outright, and it is right to. Encrypted
        // output is therefore the one thing this project does not write
        // byte-identically twice.
        writer.setR6EncryptionParameters(user.constData(), owner.constData(), permissions.allowAccessibility,
                                         permissions.allowExtractText, permissions.allowAssemble,
                                         permissions.allowAnnotate, permissions.allowFillForms, permissions.allowModify,
                                         printingFlag(permissions.printing),
                                         /*encrypt_metadata_aes=*/true);
        writer.write();
    } catch (const std::exception &e) {
        QFile::remove(tempPath);
        if (error) {
            *error = QString::fromUtf8(e.what());
        }
        return false;
    }

    return finish(tempPath, outputPath, error);
}

bool Encryption::decrypt(const QString &inputPath, const QString &outputPath, const QString &openPassword,
                         QString *error)
{
    const QString tempPath = scratchBeside(outputPath, error);
    if (tempPath.isEmpty()) {
        return false;
    }

    try {
        QPDF pdf;
        PdfFile::open(pdf, inputPath, openPassword);

        QPDFWriter writer(pdf);
        writer.setOutputFilename(QFile::encodeName(tempPath).constData());
        // Explicitly, because the default is the opposite: QPDFWriter carries
        // the source document's encryption over unless told not to. Saying
        // nothing here would produce a file that is still locked.
        writer.setPreserveEncryption(false);
        writer.setObjectStreamMode(qpdf_o_generate);
        writer.setCompressStreams(true);
        writer.setDeterministicID(true);
        writer.write();
    } catch (const std::exception &e) {
        QFile::remove(tempPath);
        if (error) {
            *error
                = i18n("“%1” could not be unlocked: %2", QFileInfo(inputPath).fileName(), QString::fromUtf8(e.what()));
        }
        return false;
    }

    return finish(tempPath, outputPath, error);
}

} // namespace ps
