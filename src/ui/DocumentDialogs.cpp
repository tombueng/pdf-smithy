/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "DocumentDialogs.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTimeEdit>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include <KLocalizedString>

namespace ps {

namespace {

QGroupBox *scopeBox(int selectedCount, int totalCount, QRadioButton **selectionOnly, QRadioButton **wholeDocument,
                    QWidget *parent)
{
    auto *box = new QGroupBox(i18nc("@title:group", "Apply to"), parent);
    *selectionOnly
        = new QRadioButton(i18ncp("@option:radio", "The selected page", "The %1 selected pages", selectedCount), box);
    *wholeDocument = new QRadioButton(
        i18ncp("@option:radio", "The whole document (%1 page)", "The whole document (%1 pages)", totalCount), box);

    (*selectionOnly)->setEnabled(selectedCount > 0);
    ((selectedCount > 0) ? *selectionOnly : *wholeDocument)->setChecked(true);

    auto *layout = new QVBoxLayout(box);
    layout->addWidget(*selectionOnly);
    layout->addWidget(*wholeDocument);
    return box;
}

} // namespace

// ══ Metadata ══════════════════════════════════════════════════════════════

MetadataDialog::MetadataDialog(const Metadata::Fields &fields, QWidget *parent)
    : QDialog(parent)
    , m_title(new QLineEdit(fields.title, this))
    , m_author(new QLineEdit(fields.author, this))
    , m_subject(new QLineEdit(fields.subject, this))
    , m_keywords(new QLineEdit(fields.keywords, this))
    , m_creator(new QLineEdit(fields.creator, this))
    , m_producer(new QLineEdit(fields.producer, this))
    , m_created(new QDateTimeEdit(this))
    , m_modified(new QDateTimeEdit(this))
{
    setWindowTitle(i18nc("@title:window", "Document Properties"));
    resize(560, 420);

    for (QDateTimeEdit *edit : { m_created, m_modified }) {
        edit->setCalendarPopup(true);
        edit->setDisplayFormat(QLocale().dateTimeFormat(QLocale::ShortFormat));
        edit->setSpecialValueText(i18nc("@item no date set", "not recorded"));
        edit->setMinimumDateTime(QDateTime::fromSecsSinceEpoch(0));
    }
    m_created->setDateTime(fields.created.isValid() ? fields.created : m_created->minimumDateTime());
    m_modified->setDateTime(fields.modified.isValid() ? fields.modified : m_modified->minimumDateTime());

    m_keywords->setPlaceholderText(i18nc("@info:placeholder", "Separated by commas"));
    m_producer->setToolTip(i18nc("@info:tooltip",
                                 "The software that wrote the file. Kept as it was rather than overwritten, so that "
                                 "saving does not announce which tool you used."));

    auto *form = new QFormLayout;
    form->addRow(i18nc("@label:textbox", "Title:"), m_title);
    form->addRow(i18nc("@label:textbox", "Author:"), m_author);
    form->addRow(i18nc("@label:textbox", "Subject:"), m_subject);
    form->addRow(i18nc("@label:textbox", "Keywords:"), m_keywords);
    form->addRow(i18nc("@label:textbox", "Created with:"), m_creator);
    form->addRow(i18nc("@label:textbox", "Written by:"), m_producer);
    form->addRow(i18nc("@label:textbox", "Created:"), m_created);
    form->addRow(i18nc("@label:textbox", "Last changed:"), m_modified);

    auto *hint = new QLabel(i18n("Emptying a field removes it from the document entirely."), this);
    hint->setWordWrap(true);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(hint);
    layout->addStretch();
    layout->addWidget(buttons);
}

Metadata::Fields MetadataDialog::fields() const
{
    Metadata::Fields fields;
    fields.title = m_title->text().trimmed();
    fields.author = m_author->text().trimmed();
    fields.subject = m_subject->text().trimmed();
    fields.keywords = m_keywords->text().trimmed();
    fields.creator = m_creator->text().trimmed();
    fields.producer = m_producer->text().trimmed();
    if (m_created->dateTime() > m_created->minimumDateTime()) {
        fields.created = m_created->dateTime();
    }
    if (m_modified->dateTime() > m_modified->minimumDateTime()) {
        fields.modified = m_modified->dateTime();
    }
    return fields;
}

// ══ Sanitising ════════════════════════════════════════════════════════════

SanitizeDialog::SanitizeDialog(const Metadata::Findings &findings, const Metadata::Fields &fields, QWidget *parent)
    : QDialog(parent)
    , m_metadata(new QCheckBox(i18nc("@option:check", "Document information"), this))
    , m_interactivity(new QCheckBox(i18nc("@option:check", "JavaScript, attachments and link actions"), this))
{
    setWindowTitle(i18nc("@title:window", "Clean Up Document"));
    resize(520, 380);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(new QLabel(i18n("This document currently carries:"), this));

    // Naming what is actually in there, rather than offering an abstract
    // checkbox. "Author: Tom Bueng" is a fact somebody can act on; "metadata"
    // is not.
    auto *found = new QLabel(this);
    found->setTextFormat(Qt::PlainText);
    found->setWordWrap(true);
    found->setStyleSheet(QStringLiteral("padding: 8px;"));

    QStringList lines;
    if (!fields.author.isEmpty()) {
        lines << i18n("Author: %1", fields.author);
    }
    if (!fields.title.isEmpty()) {
        lines << i18n("Title: %1", fields.title);
    }
    if (!fields.creator.isEmpty()) {
        lines << i18n("Created with: %1", fields.creator);
    }
    if (fields.created.isValid()) {
        lines << i18n("Created: %1", QLocale().toString(fields.created, QLocale::ShortFormat));
    }
    if (findings.hasXmp) {
        lines << i18n("An XMP metadata packet");
    }
    if (findings.hasJavaScript) {
        lines << i18n("JavaScript that runs when the document opens");
    }
    if (findings.embeddedFileCount > 0) {
        lines << i18np("%1 embedded file", "%1 embedded files", findings.embeddedFileCount);
    }
    if (lines.isEmpty()) {
        lines << i18n("Nothing worth removing: this document is already clean.");
    }
    found->setText(lines.join(QLatin1Char('\n')));
    layout->addWidget(found);

    m_metadata->setChecked(true);
    m_interactivity->setChecked(true);
    layout->addWidget(new QLabel(i18n("Remove:"), this));
    layout->addWidget(m_metadata);
    layout->addWidget(m_interactivity);

    auto *note
        = new QLabel(i18n("The pages themselves are not touched. Nothing changes on disk until you save."), this);
    note->setWordWrap(true);
    layout->addSpacing(8);
    layout->addWidget(note);
    layout->addStretch();

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(i18nc("@action:button", "Clean Up"));
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

bool SanitizeDialog::removeMetadata() const
{
    return m_metadata->isChecked();
}

bool SanitizeDialog::removeInteractivity() const
{
    return m_interactivity->isChecked();
}

// ══ Passwords ═════════════════════════════════════════════════════════════

PasswordDialog::PasswordDialog(bool documentIsAlreadyEncrypted, QWidget *parent)
    : QDialog(parent)
    , m_set(new QRadioButton(i18nc("@option:radio", "Ask for a password to open it"), this))
    , m_remove(new QRadioButton(i18nc("@option:radio", "Remove the password"), this))
    , m_password(new QLineEdit(this))
    , m_repeat(new QLineEdit(this))
    , m_noPrinting(new QCheckBox(i18nc("@option:check", "Ask readers not to print"), this))
    , m_noCopying(new QCheckBox(i18nc("@option:check", "Ask readers not to copy text"), this))
    , m_message(new QLabel(this))
{
    setWindowTitle(i18nc("@title:window", "Password"));
    resize(500, 380);

    m_password->setEchoMode(QLineEdit::Password);
    m_repeat->setEchoMode(QLineEdit::Password);
    m_remove->setEnabled(documentIsAlreadyEncrypted);
    m_set->setChecked(true);

    auto *form = new QFormLayout;
    form->addRow(i18nc("@label:textbox", "Password:"), m_password);
    form->addRow(i18nc("@label:textbox", "Repeat:"), m_repeat);

    auto *permissions = new QGroupBox(i18nc("@title:group", "Permissions"), this);
    auto *permissionLayout = new QVBoxLayout(permissions);
    permissionLayout->addWidget(m_noPrinting);
    permissionLayout->addWidget(m_noCopying);
    auto *caveat = new QLabel(i18n("Permission settings are a request, not a lock. Any reader may ignore them, "
                                   "so the password is what actually protects the document."),
                              permissions);
    caveat->setWordWrap(true);
    permissionLayout->addWidget(caveat);

    m_message->setWordWrap(true);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(m_set);
    layout->addLayout(form);
    layout->addWidget(permissions);
    layout->addSpacing(6);
    layout->addWidget(m_remove);
    layout->addSpacing(6);
    layout->addWidget(m_message);
    layout->addStretch();

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    m_okButton = buttons->button(QDialogButtonBox::Ok);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    const auto refresh = [this] { updateState(); };
    connect(m_set, &QRadioButton::toggled, this, refresh);
    connect(m_remove, &QRadioButton::toggled, this, refresh);
    connect(m_password, &QLineEdit::textChanged, this, refresh);
    connect(m_repeat, &QLineEdit::textChanged, this, refresh);
    updateState();
}

void PasswordDialog::updateState()
{
    const bool setting = m_set->isChecked();
    m_password->setEnabled(setting);
    m_repeat->setEnabled(setting);
    m_noPrinting->setEnabled(setting);
    m_noCopying->setEnabled(setting);

    if (!setting) {
        m_message->setText(i18n("The document will be saved without a password."));
        m_okButton->setEnabled(true);
        return;
    }

    if (m_password->text().isEmpty()) {
        m_message->setText(i18n("Encryption is AES-256. Nothing weaker is offered, because the older schemes in the "
                                "PDF specification are broken."));
        m_okButton->setEnabled(false);
        return;
    }
    if (m_password->text() != m_repeat->text()) {
        m_message->setText(i18n("The two passwords do not match."));
        m_okButton->setEnabled(false);
        return;
    }

    // Worth saying out loud once: there is no recovery path.
    m_message->setText(i18n("Keep the password somewhere safe. Without it the document cannot be opened again, "
                            "not by this application and not by any other."));
    m_okButton->setEnabled(true);
}

bool PasswordDialog::removesProtection() const
{
    return m_remove->isChecked();
}

QString PasswordDialog::password() const
{
    return m_password->text();
}

Encryption::Permissions PasswordDialog::permissions() const
{
    Encryption::Permissions permissions;
    permissions.printing = m_noPrinting->isChecked() ? Encryption::Printing::Forbidden : Encryption::Printing::Allowed;
    permissions.allowExtractText = !m_noCopying->isChecked();
    return permissions;
}

// ══ Image export ══════════════════════════════════════════════════════════

ExportImagesDialog::ExportImagesDialog(int selectedCount, int totalCount, QWidget *parent)
    : QDialog(parent)
    , m_format(new QComboBox(this))
    , m_resolution(new QComboBox(this))
    , m_quality(new QSpinBox(this))
{
    setWindowTitle(i18nc("@title:window", "Export as Images"));

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(scopeBox(selectedCount, totalCount, &m_selectionOnly, &m_wholeDocument, this));

    m_format->addItem(i18nc("@item image format", "PNG (sharp, larger files)"), QStringLiteral("png"));
    m_format->addItem(i18nc("@item image format", "JPEG (smaller, for photographs)"), QStringLiteral("jpeg"));
    m_format->addItem(i18nc("@item image format", "TIFF (for print workflows)"), QStringLiteral("tiff"));

    m_resolution->addItem(i18nc("@item resolution", "96 dpi (screen)"), 96);
    m_resolution->addItem(i18nc("@item resolution", "150 dpi (everyday)"), 150);
    m_resolution->addItem(i18nc("@item resolution", "300 dpi (print)"), 300);
    m_resolution->addItem(i18nc("@item resolution", "600 dpi (archival)"), 600);
    m_resolution->setCurrentIndex(1);

    m_quality->setRange(1, 100);
    m_quality->setValue(90);
    m_quality->setSuffix(i18nc("@item percent suffix in a spin box", " %"));

    auto *form = new QFormLayout;
    form->addRow(i18nc("@label:listbox", "Format:"), m_format);
    form->addRow(i18nc("@label:listbox", "Detail:"), m_resolution);
    form->addRow(i18nc("@label:spinbox", "Quality:"), m_quality);
    layout->addLayout(form);

    const auto refresh = [this] {
        // Quality means nothing for a lossless format, so it goes quiet
        // instead of sitting there implying it does something.
        m_quality->setEnabled(m_format->currentData().toString() != QLatin1String("png"));
    };
    connect(m_format, &QComboBox::currentIndexChanged, this, refresh);
    refresh();

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(i18nc("@action:button", "Export"));
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

bool ExportImagesDialog::appliesToSelectionOnly() const
{
    return m_selectionOnly->isChecked();
}

ImageIO::ExportOptions ExportImagesDialog::options() const
{
    ImageIO::ExportOptions options;
    options.format = m_format->currentData().toString();
    options.dpi = m_resolution->currentData().toInt();
    options.quality = m_quality->value();
    return options;
}

} // namespace ps
