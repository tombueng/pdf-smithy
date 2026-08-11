/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "RedactEditor.h"

#include "PageNavigation.h"

#include "PageProcessor.h"
#include "RedactionView.h"
#include "core/Document.h"
#include "core/PdfGeometry.h"
#include "core/Redaction.h"
#include "core/RenderBackend.h"

#include <KLocalizedString>

#include <QDialogButtonBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QWidget>

namespace ps {

namespace {

/** Turns a page image by the rotation the user has applied in the organiser. */
QImage rotated(const QImage &image, int degrees)
{
    if (degrees == 0 || image.isNull()) {
        return image;
    }
    QTransform turn;
    turn.rotate(degrees);
    return image.transformed(turn, Qt::SmoothTransformation);
}

} // namespace

RedactEditor::RedactEditor(Document *document, RenderBackend *backend, int startRow, PageProcessor *processor,
                           QObject *parent)
    : EditorMode(parent)
    , m_document(document)
    , m_backend(backend)
    , m_processor(processor)
    , m_row(qBound(0, startRow, qMax(0, document->pageCount() - 1)))
    , m_view(new RedactionView)
    , m_pageBox(new QSpinBox)
    , m_pageLabel(new QLabel)
    , m_list(new QListWidget)
    , m_remove(new QPushButton(i18nc("@action:button", "Remove")))
    , m_removeAll(new QPushButton(i18nc("@action:button", "Remove All")))
    , m_affected(new QLabel)
{
    setWidgets(buildStage(), buildPanel());

    connect(m_view, &RedactionView::areasChanged, this, [this] {
        storeCurrent();
        refreshList();
        refreshAffectedText();
    });
    connect(m_view, &RedactionView::selectionChanged, this, [this] {
        m_remove->setEnabled(m_view->selectedIndex() >= 0);
        refreshAffectedText();
    });
    connect(m_remove, &QPushButton::clicked, m_view, &RedactionView::removeSelected);
    connect(m_removeAll, &QPushButton::clicked, this, [this] {
        m_areas.clear();
        m_view->clear();
        refreshList();
        refreshAffectedText();
        m_apply->setEnabled(false);
    });
    connect(m_list, &QListWidget::currentRowChanged, this, &RedactEditor::goToArea);

    showRow(m_row);
    refreshAffectedText();
}

QWidget *RedactEditor::buildStage()
{
    auto *stage = new QWidget;

    m_pageBox->setValue(m_row + 1);
    auto *navigation = pageNavigation(stage, m_pageBox, m_document->pageCount());

    auto *hint = new QLabel(i18n("Drag across anything that has to go. Click a box to select it, "
                                 "Delete to take it back."),
                            stage);
    hint->setWordWrap(true);

    auto *layout = new QVBoxLayout(stage);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addLayout(navigation);
    layout->addWidget(m_view, 1);
    layout->addWidget(hint);

    connect(m_pageBox, &QSpinBox::valueChanged, this, [this](int value) { showRow(value - 1); });

    return stage;
}

QWidget *RedactEditor::buildPanel()
{
    auto *panel = new QWidget;

    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_remove->setEnabled(false);

    auto *listButtons = new QHBoxLayout;
    listButtons->addWidget(m_remove);
    listButtons->addWidget(m_removeAll);
    listButtons->addStretch(1);

    auto *marked = new QGroupBox(i18nc("@title:group", "Marked areas"), panel);
    auto *markedLayout = new QVBoxLayout(marked);
    markedLayout->addWidget(m_list, 1);
    markedLayout->addLayout(listButtons);

    // Showing the words is the point: a box drawn a few points short looks
    // right and is not, and this is where that shows up.
    m_affected->setWordWrap(true);
    m_affected->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto *affectedBox = new QGroupBox(i18nc("@title:group", "Text that will be destroyed"), panel);
    auto *affectedLayout = new QVBoxLayout(affectedBox);
    affectedLayout->addWidget(m_affected);

    auto *notes = new QLabel(panel);
    notes->setWordWrap(true);
    QString noteText = QStringLiteral("<ul>");
    for (const QString &limit : Redaction::limitations()) {
        noteText += QStringLiteral("<li>%1</li>").arg(limit.toHtmlEscaped());
    }
    noteText += QStringLiteral("</ul>");
    notes->setText(noteText);
    auto *notesBox = new QGroupBox(i18nc("@title:group", "What this does and does not do"), panel);
    auto *notesLayout = new QVBoxLayout(notesBox);
    notesLayout->addWidget(notes);

    auto *warning = new QLabel(i18n("<b>This cannot be undone in the finished file.</b> The marked content is taken "
                                    "out of the document, not covered over."),
                               panel);
    warning->setWordWrap(true);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, panel);
    m_apply = buttons->button(QDialogButtonBox::Ok);
    m_apply->setText(i18nc("@action:button", "Redact"));
    m_apply->setEnabled(false);
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        Q_EMIT finished(true);
    });
    connect(buttons, &QDialogButtonBox::rejected, this, [this] {
        Q_EMIT finished(false);
    });

    auto *layout = new QVBoxLayout(panel);
    layout->addWidget(marked, 1);
    layout->addWidget(affectedBox);
    layout->addWidget(notesBox);
    layout->addWidget(warning);
    layout->addWidget(buttons);

    return panel;
}

QString RedactEditor::title() const
{
    return i18nc("@title:window", "Redact");
}

QString RedactEditor::iconName() const
{
    return QStringLiteral("edit-delete-shred");
}

bool RedactEditor::isUnchanged() const
{
    return areaCount() == 0;
}

bool RedactEditor::commit(QString *error)
{
    storeCurrent();
    if (m_areas.isEmpty()) {
        return true;
    }
    if (!m_processor) {
        if (error) {
            *error = i18n("This editor has nothing to redact with.");
        }
        return false;
    }

    Redaction::Report report;
    if (!m_processor->redact(m_areas, &report, error)) {
        return false;
    }

    // Anything the engine had to compromise on is said out loud rather than
    // left for the reader to discover in the published file.
    QStringList said = report.warnings;
    if (!report.pagesRasterised.isEmpty()) {
        said += i18ncp("@info",
                       "One page held a picture that could not be edited, so it became an image and lost "
                       "its selectable text.",
                       "%1 pages held pictures that could not be edited, so they became images and lost their "
                       "selectable text.",
                       report.pagesRasterised.size());
    }
    said += i18ncp("@info:status", "Redacted %1 area: the content is gone, not covered.",
                   "Redacted %1 areas: the content is gone, not covered.", report.areasApplied);
    Q_EMIT statusMessage(said.join(u' '));
    return true;
}

void RedactEditor::showRow(int row)
{
    if (row < 0 || row >= m_document->pageCount()) {
        return;
    }

    storeCurrent();
    m_row = row;

    const PageRef ref = m_document->pageAt(row);
    m_view->setPage(rotated(m_backend->renderPage(ref.sourceId, ref.sourcePage, 1400), ref.rotation),
                    m_document->pageSizePoints(row));
    m_view->setAreasInPoints(m_areas.value(row));

    if (m_pageBox->value() != row + 1) {
        QSignalBlocker blocker(m_pageBox);
        m_pageBox->setValue(row + 1);
    }
    refreshList();
    refreshAffectedText();
}

void RedactEditor::storeCurrent()
{
    const QVector<QRectF> areas = m_view->areasInPoints();
    if (areas.isEmpty()) {
        m_areas.remove(m_row);
    } else {
        m_areas.insert(m_row, areas);
    }
    m_apply->setEnabled(areaCount() > 0);
}

int RedactEditor::areaCount() const
{
    int total = 0;
    for (auto it = m_areas.constBegin(); it != m_areas.constEnd(); ++it) {
        total += it.value().size();
    }
    return total;
}

QHash<int, QVector<QRectF>> RedactEditor::areasByRow() const
{
    return m_areas;
}

void RedactEditor::refreshList()
{
    QSignalBlocker blocker(m_list);
    m_list->clear();
    m_listTargets.clear();

    QList<int> rows = m_areas.keys();
    std::sort(rows.begin(), rows.end());
    for (int row : std::as_const(rows)) {
        const QVector<QRectF> areas = m_areas.value(row);
        for (int i = 0; i < areas.size(); ++i) {
            const QRectF &area = areas.at(i);
            m_list->addItem(i18nc("@item marked redaction area: page number and size in points", "Page %1: %2 × %3 pt",
                                  row + 1, qRound(area.width()), qRound(area.height())));
            m_listTargets.append({ row, i });
            if (row == m_row && i == m_view->selectedIndex()) {
                m_list->setCurrentRow(m_list->count() - 1);
            }
        }
    }
    m_removeAll->setEnabled(m_list->count() > 0);
}

void RedactEditor::goToArea(int listIndex)
{
    if (listIndex < 0 || listIndex >= m_listTargets.size()) {
        return;
    }
    const int row = m_listTargets.at(listIndex).first;
    if (row != m_row) {
        showRow(row);
    }
}

void RedactEditor::refreshAffectedText()
{
    const QVector<QRectF> areas = m_view->areasInPoints();
    if (areas.isEmpty()) {
        m_affected->setText(i18n("Nothing marked on this page."));
        return;
    }

    // Areas are measured on the page as the organiser shows it, which may be
    // turned relative to the page the renderer knows about.
    const PageRef ref = m_document->pageAt(m_row);
    const QSizeF source = m_backend->pageSizePoints(ref.sourceId, ref.sourcePage);
    const QTransform toSource = PdfGeometry::displayToPageTransform(ref.rotation, source.width(), source.height());

    QStringList words;
    for (const QRectF &area : areas) {
        words << m_backend->wordsInside(ref.sourceId, ref.sourcePage, toSource.mapRect(area));
    }
    words.removeAll(QString());

    if (words.isEmpty()) {
        m_affected->setText(i18n("No selectable text here. On a scan that is expected: the picture itself is edited, "
                                 "and any recognised text over it goes with it."));
        return;
    }
    m_affected->setText(QStringLiteral("<i>%1</i>").arg(words.join(QLatin1Char(' ')).toHtmlEscaped()));
}

} // namespace ps
