/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#pragma once

#include "Forms.h"

#include <QHash>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QVector>

namespace ps {

/**
 * The things you keep typing into forms, kept once.
 *
 * Every form asks for the same handful of facts — a name, an address, an email,
 * a phone number — and typing them again is the tax a reader pays for a document
 * that was handed to them unfilled. This remembers them, locally, and works out
 * which stored fact a given form field is asking for, so filling one is a matter
 * of confirming rather than copying.
 *
 * ## Local, and only local
 *
 * The vault is a plain JSON file on the machine, nowhere else. It is never sent
 * anywhere; there is no account and no sync. That is the whole point: the data a
 * person reuses across forms is exactly the data they would least like to see
 * leave their computer.
 *
 * ## How a field is matched
 *
 * A field carries a name the form's author chose and, often, a label they wrote
 * beside it. @ref keyFor folds both to one of a small, fixed set of meanings —
 * "email", "given-name", "postal-code" — recognising the words several languages
 * use for each. It is deliberately cautious: a field it cannot place with
 * confidence is left for the person, not guessed at.
 */
class FormVault
{
public:
    /** The stored vault at @p path, or an empty one when there is no file yet. */
    static FormVault load(const QString &path);

    /** Writes the vault to @p path, creating the folder if need be. */
    bool save(const QString &path, QString *error) const;

    /** Where the vault lives by default: a per-user data file. */
    static QString defaultPath();

    void set(const QString &key, const QString &value);
    QString value(const QString &key) const;
    bool remove(const QString &key);
    QStringList keys() const;
    bool isEmpty() const { return m_entries.isEmpty(); }

    /**
     * The fields of @p form matched against what is stored, as a map from each
     * field's own name to the value to put in it. Only text and drop-down fields
     * that are not read-only, and for a drop-down only when the stored value is
     * one it offers.
     */
    QHash<QString, QString> fillFor(const QVector<FormField> &form) const;

    /**
     * Stores the filled-in values of @p form under the meanings they match,
     * overwriting what was there. Returns how many it learned.
     */
    int learnFrom(const QVector<FormField> &form);

    /**
     * The meaning a field is asking for — "email", "given-name", … — or empty
     * when it cannot be placed with confidence. Static so it can be tested on its
     * own and reused by the interface.
     */
    static QString keyFor(const FormField &field);

    /** Every meaning @ref keyFor can assign, for help and for the interface. */
    static QStringList knownKeys();

private:
    QMap<QString, QString> m_entries;
};

} // namespace ps
