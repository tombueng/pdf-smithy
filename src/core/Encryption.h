/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#pragma once

#include <QString>

namespace ps {

/**
 * Passwords and permissions.
 *
 * Only AES-256 is offered. The older schemes in the PDF specification are
 * broken to the point where offering them would be dressing a document up as
 * protected when it is not, and a choice between "secure" and "insecure" is a
 * choice a file dialog has no business asking anyone to make.
 *
 * Permission flags are worth being blunt about: they are a request to the
 * reader, not a lock. Any viewer may ignore them, and plenty do. They stop an
 * honest colleague from printing by accident; they stop nobody who does not
 * want to be stopped. The dialog says so.
 */
class Encryption
{
public:
    enum class Printing {
        Allowed,
        LowResolutionOnly,
        Forbidden,
    };

    struct Permissions {
        bool allowExtractText = true;
        bool allowModify = true;
        bool allowAnnotate = true;
        bool allowFillForms = true;
        bool allowAssemble = true;

        /** Screen readers. Denying this is rarely defensible; it defaults on. */
        bool allowAccessibility = true;

        Printing printing = Printing::Allowed;
    };

    /**
     * How @p printing maps onto QPDF's printing flag.
     *
     * An int rather than the enum itself so that this header stays free of
     * QPDF, which every user of Encryption would otherwise have to pull in.
     * Public because the writer encrypts too, and two copies of this mapping
     * would be one copy too many.
     */
    static int printingFlagFor(Printing printing);

    /** True when @p path needs a password to be read at all. */
    static bool isEncrypted(const QString &path);

    /** True when @p password opens @p path. An empty password tests the user password. */
    static bool canOpen(const QString &path, const QString &password);

    /**
     * Writes @p outputPath encrypted with AES-256.
     *
     * @p userPassword is what a reader must type. @p ownerPassword lifts the
     * permission restrictions; when left empty it is set to the user password,
     * because an owner password of "" is no protection at all.
     */
    static bool encrypt(const QString &inputPath, const QString &outputPath, const QString &userPassword,
                        const QString &ownerPassword, const Permissions &permissions, const QString &openPassword,
                        QString *error);

    /** Writes @p outputPath with no encryption, given a password that opens it. */
    static bool decrypt(const QString &inputPath, const QString &outputPath, const QString &openPassword,
                        QString *error);
};

} // namespace ps
