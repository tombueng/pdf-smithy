/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#pragma once

#include "core/Encryption.h"
#include "core/ImageIO.h"
#include "core/Metadata.h"

#include <QDialog>

class QCheckBox;
class QComboBox;
class QDateTimeEdit;
class QLabel;
class QLineEdit;
class QRadioButton;
class QSpinBox;

namespace ps {

/** Title, author and the rest of what a document says about itself. */
class MetadataDialog : public QDialog
{
    Q_OBJECT

public:
    MetadataDialog(const Metadata::Fields &fields, QWidget *parent = nullptr);

    Metadata::Fields fields() const;

private:
    QLineEdit *m_title;
    QLineEdit *m_author;
    QLineEdit *m_subject;
    QLineEdit *m_keywords;
    QLineEdit *m_creator;
    QLineEdit *m_producer;
    QDateTimeEdit *m_created;
    QDateTimeEdit *m_modified;
};

// ══════════════════════════════════════════════════════════════════════════

/**
 * What is in the document that the user might not want to hand over.
 *
 * Shown before anything is removed, because "we cleaned it up" without saying
 * what was there is not an answer anybody can check.
 */
class SanitizeDialog : public QDialog
{
    Q_OBJECT

public:
    SanitizeDialog(const Metadata::Findings &findings, const Metadata::Fields &fields, QWidget *parent = nullptr);

    bool removeMetadata() const;
    bool removeInteractivity() const;

private:
    QCheckBox *m_metadata;
    QCheckBox *m_interactivity;
};

// ══════════════════════════════════════════════════════════════════════════

/** Setting or lifting a password, and the honest small print about permissions. */
class PasswordDialog : public QDialog
{
    Q_OBJECT

public:
    explicit PasswordDialog(bool documentIsAlreadyEncrypted, QWidget *parent = nullptr);

    bool removesProtection() const;
    QString password() const;
    Encryption::Permissions permissions() const;

private:
    void updateState();

    QRadioButton *m_set;
    QRadioButton *m_remove;
    QLineEdit *m_password;
    QLineEdit *m_repeat;
    QCheckBox *m_noPrinting;
    QCheckBox *m_noCopying;
    QLabel *m_message;
    QPushButton *m_okButton;
};

// ══════════════════════════════════════════════════════════════════════════

/** Resolution and format for rendering pages out as pictures. */
class ExportImagesDialog : public QDialog
{
    Q_OBJECT

public:
    ExportImagesDialog(int selectedCount, int totalCount, QWidget *parent = nullptr);

    bool appliesToSelectionOnly() const;
    ImageIO::ExportOptions options() const;

private:
    QRadioButton *m_selectionOnly;
    QRadioButton *m_wholeDocument;
    QComboBox *m_format;
    QComboBox *m_resolution;
    QSpinBox *m_quality;
};

} // namespace ps
