/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "FormEditor.h"

#include "PageNavigation.h"

#include "FormFieldView.h"
#include "PageProcessor.h"
#include "core/Document.h"
#include "core/RenderBackend.h"

#include <KLocalizedString>

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QEvent>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QWidget>

namespace ps {

namespace {

QImage rotated(const QImage &image, int degrees)
{
    if (degrees == 0 || image.isNull()) {
        return image;
    }
    return image.transformed(QTransform().rotate(degrees), Qt::SmoothTransformation);
}

} // namespace

FormEditor::FormEditor(Document *document, RenderBackend *backend, const QVector<FormField> &fields,
                       PageProcessor *processor, QObject *parent)
    : EditorMode(parent)
    , m_document(document)
    , m_backend(backend)
    , m_processor(processor)
    , m_fields(fields)
    , m_view(new FormFieldView)
    , m_pageBox(new QSpinBox)
    , m_pageLabel(new QLabel)
    , m_flatten(new QCheckBox(
          i18nc("@option:check", "Make the answers permanent (the form can no longer be filled in)")))
    , m_summary(new QLabel)
{
    setWidgets(buildStage(), buildPanel());

    connect(m_view, &FormFieldView::fieldPicked, this, &FormEditor::focusField);

    // Start on the page the first fillable field is on, not on page one: an
    // official form's first page is often all instructions.
    int start = 0;
    for (const Editor &editor : std::as_const(m_editors)) {
        if (!editor.field.readOnly && editor.field.page >= 0) {
            start = editor.field.page;
            break;
        }
    }
    showPage(start);
}

QWidget *FormEditor::buildStage()
{
    auto *stage = new QWidget;

    auto *navigation = pageNavigation(stage, m_pageBox, m_document->pageCount());

    auto *hint = new QLabel(i18n("Click a box on the page to type into it. Dashed grey boxes are locked by the form."),
                            stage);
    hint->setWordWrap(true);

    auto *layout = new QVBoxLayout(stage);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addLayout(navigation);
    layout->addWidget(m_view, 1);
    layout->addWidget(hint);

    connect(m_pageBox, &QSpinBox::valueChanged, this, [this](int value) { showPage(value - 1); });

    return stage;
}

QWidget *FormEditor::buildPanel()
{
    auto *panel = new QWidget;

    auto *sheet = new QWidget;
    auto *form = new QFormLayout(sheet);
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

    int fillable = 0;
    for (const FormField &field : std::as_const(m_fields)) {
        if (field.kind == FormField::Kind::Button || field.kind == FormField::Kind::Signature) {
            continue; // Nothing to type into either of them.
        }

        Editor editor;
        editor.field = field;

        switch (field.kind) {
        case FormField::Kind::Checkbox:
        case FormField::Kind::Radio: {
            auto *box = new QCheckBox(sheet);
            box->setChecked(!field.value.isEmpty() && field.value != QLatin1String("/Off"));
            editor.widget = box;
            break;
        }
        case FormField::Kind::Choice: {
            auto *combo = new QComboBox(sheet);
            combo->setEditable(true); // Some lists accept a value of their own.
            combo->addItems(field.options);
            combo->setCurrentText(field.value);
            editor.widget = combo;
            break;
        }
        default:
            if (field.multiline) {
                auto *edit = new QPlainTextEdit(sheet);
                edit->setPlainText(field.value);
                edit->setMinimumHeight(70);
                editor.widget = edit;
            } else {
                auto *edit = new QLineEdit(field.value, sheet);
                editor.widget = edit;
            }
            break;
        }

        editor.widget->setEnabled(!field.readOnly);
        if (field.readOnly) {
            editor.widget->setToolTip(i18n("The form locks this field."));
        }

        QString label = field.label;
        if (field.required) {
            label += QStringLiteral(" *");
        }
        auto *caption = new QLabel(label, sheet);
        caption->setWordWrap(true);
        if (field.page >= 0) {
            caption->setToolTip(i18n("Page %1 · %2", field.page + 1, Forms::describe(field.kind)));
        }

        form->addRow(caption, editor.widget);

        // Typing into a field in the panel turns the page to it and lights it
        // up, so the two halves always agree about what is being filled in.
        // A filter rather than a per-widget signal because the four kinds of
        // editor have four different ones, and "got the focus" is the same
        // event in all of them.
        editor.widget->setProperty("psFieldIndex", int(m_editors.size()));
        editor.widget->installEventFilter(this);

        m_editors.append(editor);
        if (!field.readOnly) {
            ++fillable;
        }
    }

    auto *scroll = new QScrollArea(panel);
    scroll->setWidget(sheet);
    scroll->setWidgetResizable(true);

    m_summary->setWordWrap(true);
    m_summary->setText(fillable == m_editors.size()
                           ? i18np("%1 field. Fields marked * are required.",
                                   "%1 fields. Fields marked * are required.", m_editors.size())
                           : i18n("%1 of %2 fields can be filled in; the rest are locked by the form. "
                                  "Fields marked * are required.",
                                  fillable, m_editors.size()));

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, panel);
    buttons->button(QDialogButtonBox::Ok)->setText(i18nc("@action:button", "Fill In"));
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        Q_EMIT finished(true);
    });
    connect(buttons, &QDialogButtonBox::rejected, this, [this] {
        Q_EMIT finished(false);
    });

    auto *layout = new QVBoxLayout(panel);
    layout->addWidget(m_summary);
    layout->addWidget(scroll, 1);
    layout->addWidget(m_flatten);
    layout->addWidget(buttons);

    return panel;
}

bool FormEditor::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::FocusIn) {
        const QVariant index = watched->property("psFieldIndex");
        if (index.isValid()) {
            focusField(index.toInt());
        }
    }
    return EditorMode::eventFilter(watched, event);
}

void FormEditor::showPage(int page)
{
    if (page < 0 || page >= m_document->pageCount()) {
        return;
    }
    m_page = page;

    const PageRef ref = m_document->pageAt(page);
    m_view->setPage(rotated(m_backend->renderPage(ref.sourceId, ref.sourcePage, 1400), ref.rotation),
                    m_document->pageSizePoints(page));

    QVector<FormField> here;
    QVector<int> indices;
    for (int i = 0; i < m_editors.size(); ++i) {
        if (m_editors.at(i).field.page == page) {
            here.append(m_editors.at(i).field);
            indices.append(i);
        }
    }
    m_view->setFields(here, indices);

    if (m_pageBox->value() != page + 1) {
        QSignalBlocker blocker(m_pageBox);
        m_pageBox->setValue(page + 1);
    }
}

void FormEditor::focusField(int index)
{
    if (index < 0 || index >= m_editors.size()) {
        return;
    }
    const Editor &editor = m_editors.at(index);
    if (editor.field.page >= 0 && editor.field.page != m_page) {
        showPage(editor.field.page);
    }
    m_view->setCurrentField(index);

    if (editor.widget && !editor.widget->hasFocus() && editor.widget->isEnabled()) {
        editor.widget->setFocus(Qt::OtherFocusReason);
    }
}

QString FormEditor::title() const
{
    return i18nc("@title:window", "Fill In Form");
}

QString FormEditor::iconName() const
{
    return QStringLiteral("document-edit-sign");
}

QString FormEditor::valueOf(const Editor &editor) const
{
    if (auto *box = qobject_cast<QCheckBox *>(editor.widget)) {
        // The engine works out which name this box's appearance uses.
        return box->isChecked() ? QStringLiteral("on") : QStringLiteral("off");
    }
    if (auto *combo = qobject_cast<QComboBox *>(editor.widget)) {
        return combo->currentText();
    }
    if (auto *edit = qobject_cast<QLineEdit *>(editor.widget)) {
        return edit->text();
    }
    if (auto *edit = qobject_cast<QPlainTextEdit *>(editor.widget)) {
        return edit->toPlainText();
    }
    return {};
}

QHash<QString, QString> FormEditor::changedValues() const
{
    QHash<QString, QString> changed;
    for (const Editor &editor : m_editors) {
        if (editor.field.readOnly) {
            continue;
        }
        const QString now = valueOf(editor);

        // Only what actually moved. Writing every field back would set values on
        // fields the user never touched, and a form full of empty strings is not
        // the same document as a form full of nothing.
        if (editor.field.kind == FormField::Kind::Checkbox || editor.field.kind == FormField::Kind::Radio) {
            const bool was = !editor.field.value.isEmpty() && editor.field.value != QLatin1String("/Off");
            const bool is = now == QLatin1String("on");
            if (was != is) {
                changed.insert(editor.field.name, now);
            }
            continue;
        }
        if (now != editor.field.value) {
            changed.insert(editor.field.name, now);
        }
    }
    return changed;
}

bool FormEditor::shouldFlatten() const
{
    return m_flatten->isChecked();
}

bool FormEditor::isUnchanged() const
{
    return changedValues().isEmpty() && !shouldFlatten();
}

bool FormEditor::commit(QString *error)
{
    const QHash<QString, QString> values = changedValues();
    if (values.isEmpty() && !shouldFlatten()) {
        return true;
    }
    if (!m_processor) {
        if (error) {
            *error = i18n("This editor has nothing to fill the form in with.");
        }
        return false;
    }

    int filled = 0;
    QStringList notes;
    if (!m_processor->fillForm(values, shouldFlatten(), &filled, &notes, error)) {
        return false;
    }

    notes += shouldFlatten() ? i18ncp("@info:status", "Filled in %1 field and made the answers permanent.",
                                      "Filled in %1 fields and made the answers permanent.", filled)
                             : i18ncp("@info:status", "Filled in %1 field.", "Filled in %1 fields.", filled);
    Q_EMIT statusMessage(notes.join(u' '));
    return true;
}

} // namespace ps
