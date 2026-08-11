/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#pragma once

#include "EditorMode.h"
#include "core/Forms.h"

#include <QHash>
#include <QVector>

class QCheckBox;
class QLabel;
class QSpinBox;
class QWidget;

namespace ps {

class Document;
class FormFieldView;
class PageProcessor;
class RenderBackend;

/**
 * Filling in a form, with the form in front of you.
 *
 * A list of labels alone is not enough on a real form: three fields called
 * "Date", a row of initials, a heading four rows above the box it belongs to.
 * So the page is shown with every field outlined, clicking a box picks it, and
 * picking one in the list moves the page to it. Locked fields are drawn too,
 * dashed and grey, because "why can I not type here" deserves an answer on the page
 * rather than a hunt.
 */
class FormEditor : public EditorMode
{
    Q_OBJECT

public:
    FormEditor(Document *document, RenderBackend *backend, const QVector<FormField> &fields,
               PageProcessor *processor = nullptr, QObject *parent = nullptr);

    QString title() const override;
    QString iconName() const override;
    bool isUnchanged() const override;
    bool commit(QString *error) override;

    /** Only the fields whose value the user actually moved. */
    QHash<QString, QString> changedValues() const;

    bool shouldFlatten() const;

protected:
    /** Watches the panel's editors so that focus follows to the page. */
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    struct Editor {
        FormField field;
        QWidget *widget = nullptr;
    };

    QWidget *buildStage();
    QWidget *buildPanel();
    QString valueOf(const Editor &editor) const;
    void showPage(int page);
    void focusField(int index);

    Document *m_document;
    RenderBackend *m_backend;
    PageProcessor *m_processor;
    QVector<FormField> m_fields;

    FormFieldView *m_view = nullptr;
    QSpinBox *m_pageBox = nullptr;
    QLabel *m_pageLabel = nullptr;
    QCheckBox *m_flatten = nullptr;
    QLabel *m_summary = nullptr;

    QVector<Editor> m_editors;
    int m_page = 0;
};

} // namespace ps
