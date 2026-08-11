/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "SignEditor.h"

#include "PagePlacementView.h"
#include "PageProcessor.h"
#include "SignatureCanvas.h"
#include "core/Document.h"
#include "core/RenderBackend.h"

#include <KLocalizedString>
#include <KMessageBox>

#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QPushButton>
#include <QSlider>
#include <QVBoxLayout>
#include <QWidget>

#include <cmath>
#include <numeric>

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

SignEditor::SignEditor(Document *document, RenderBackend *backend, int row, PageProcessor *processor, QObject *parent)
    : EditorMode(parent)
    , m_document(document)
    , m_backend(backend)
    , m_processor(processor)
    , m_row(qBound(0, row, qMax(0, document->pageCount() - 1)))
    , m_list(new QListWidget)
    , m_view(new PagePlacementView)
    , m_size(new QSlider(Qt::Horizontal))
    , m_allPages(new QCheckBox)
    , m_hint(new QLabel)
{
    setWidgets(buildStage(), buildPanel());

    const PageRef ref = m_document->pageAt(m_row);
    m_view->setPage(rotated(m_backend->renderPage(ref.sourceId, ref.sourcePage, 1200), ref.rotation),
                    m_document->pageSizePoints(m_row));

    connect(m_list, &QListWidget::currentRowChanged, this, &SignEditor::applySelection);
    connect(m_size, &QSlider::valueChanged, this, [this](int value) {
        m_view->setOverlayRelativeWidth(value / 100.0);
    });
    connect(m_view, &PagePlacementView::overlayChanged, this, [this] {
        m_placed = true;
        const int percent = static_cast<int>(std::lround(m_view->overlayRelativeWidth() * 100));
        if (m_size->value() != percent) {
            QSignalBlocker blocker(m_size);
            m_size->setValue(percent);
        }
    });

    refreshList();
}

QWidget *SignEditor::buildStage()
{
    auto *stage = new QWidget;

    m_size->setRange(3, 100);
    m_size->setValue(28);

    auto *sizeRow = new QHBoxLayout;
    sizeRow->addWidget(new QLabel(i18nc("@label:slider", "Size:"), stage));
    sizeRow->addWidget(m_size, 1);

    m_hint->setWordWrap(true);
    m_hint->setText(i18n("Drag the signature to where it belongs, or click anywhere on the page. "
                         "Pull the corner handle to resize it."));

    auto *layout = new QVBoxLayout(stage);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_view, 1);
    layout->addLayout(sizeRow);
    layout->addWidget(m_hint);

    return stage;
}

QWidget *SignEditor::buildPanel()
{
    auto *panel = new QWidget;

    m_list->setIconSize(QSize(150, 60));
    m_list->setSpacing(3);

    auto *importButton = new QPushButton(QIcon::fromTheme(QStringLiteral("document-open")),
                                         i18nc("@action:button", "From File…"), panel);
    auto *drawButton
        = new QPushButton(QIcon::fromTheme(QStringLiteral("draw-freehand")), i18nc("@action:button", "Draw…"), panel);
    m_deleteButton
        = new QPushButton(QIcon::fromTheme(QStringLiteral("edit-delete")), i18nc("@action:button", "Remove"), panel);

    importButton->setToolTip(i18nc("@info:tooltip",
                                   "A photo or scan of your signature. The paper around it is made "
                                   "transparent automatically."));
    drawButton->setToolTip(i18nc("@info:tooltip", "Sign with the mouse, a trackpad or a pen tablet."));

    m_allPages->setText(i18nc("@option:check", "Put it on every page"));

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, panel);
    m_okButton = buttons->button(QDialogButtonBox::Ok);
    m_okButton->setText(i18nc("@action:button", "Sign"));
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        Q_EMIT finished(true);
    });
    connect(buttons, &QDialogButtonBox::rejected, this, [this] {
        Q_EMIT finished(false);
    });

    auto *layout = new QVBoxLayout(panel);
    layout->addWidget(new QLabel(i18nc("@label", "Your signatures:"), panel));
    layout->addWidget(m_list, 1);
    layout->addWidget(importButton);
    layout->addWidget(drawButton);
    layout->addWidget(m_deleteButton);
    layout->addWidget(m_allPages);
    layout->addWidget(buttons);

    connect(importButton, &QPushButton::clicked, this, &SignEditor::importFromFile);
    connect(drawButton, &QPushButton::clicked, this, &SignEditor::drawNew);
    connect(m_deleteButton, &QPushButton::clicked, this, &SignEditor::deleteSelected);

    return panel;
}

QString SignEditor::title() const
{
    return i18nc("@title:window", "Sign Document");
}

QString SignEditor::iconName() const
{
    return QStringLiteral("document-edit-sign");
}

bool SignEditor::isUnchanged() const
{
    // Choosing a signature and never moving it is still a decision, but it is
    // one that costs nothing to make again, so only a placement counts.
    return !m_placed || selectedImage().isNull();
}

QVector<int> SignEditor::targetPages() const
{
    if (!m_allPages->isChecked()) {
        return { m_row };
    }
    QVector<int> pages(m_document->pageCount());
    std::iota(pages.begin(), pages.end(), 0);
    return pages;
}

bool SignEditor::commit(QString *error)
{
    const QImage image = selectedImage();
    const QVector<int> pages = targetPages();
    if (image.isNull() || pages.isEmpty()) {
        return true;
    }
    if (!m_processor) {
        if (error) {
            *error = i18n("This editor has nothing to sign with.");
        }
        return false;
    }

    if (!m_processor->stamp(pages, image, m_view->overlayRectInPoints(), error)) {
        return false;
    }
    Q_EMIT statusMessage(i18ncp("@info:status", "Signed %1 page.", "Signed %1 pages.", pages.size()));
    return true;
}

void SignEditor::refreshList(const QString &selectId)
{
    m_entries = SignatureStore::load();

    m_list->clear();
    int selectRow = -1;
    for (int i = 0; i < m_entries.size(); ++i) {
        const SignatureStore::Entry &entry = m_entries.at(i);

        // Drawn on a light plate: a dark signature on a dark theme background
        // would be invisible in the list.
        QPixmap plate(150, 60);
        plate.fill(QColor(250, 250, 252));
        {
            QPainter painter(&plate);
            painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
            const QImage scaled = entry.image.scaled(plate.size().shrunkBy(QMargins(4, 4, 4, 4)), Qt::KeepAspectRatio,
                                                     Qt::SmoothTransformation);
            painter.drawImage((plate.width() - scaled.width()) / 2, (plate.height() - scaled.height()) / 2, scaled);
        }

        auto *item = new QListWidgetItem(QIcon(plate), entry.name, m_list);
        item->setData(Qt::UserRole, entry.id);
        if (entry.id == selectId) {
            selectRow = i;
        }
    }

    const bool any = !m_entries.isEmpty();
    m_deleteButton->setEnabled(any);
    m_okButton->setEnabled(any);

    if (!any) {
        m_hint->setText(i18n("No signature yet. Add a photo or scan of one, or draw it here. "
                             "It is kept on this machine and reused next time."));
        m_view->setOverlayImage(QImage());
        return;
    }

    m_list->setCurrentRow(selectRow >= 0 ? selectRow : 0);
}

QImage SignEditor::selectedImage() const
{
    const int row = m_list->currentRow();
    if (row < 0 || row >= m_entries.size()) {
        return {};
    }
    return m_entries.at(row).image;
}

void SignEditor::applySelection()
{
    const QImage image = selectedImage();
    m_view->setOverlayImage(image);
    m_okButton->setEnabled(!image.isNull());
    if (!image.isNull()) {
        m_hint->setText(i18n("Drag the signature to where it belongs, or click anywhere on the page. "
                             "Pull the corner handle to resize it."));
    }
}

void SignEditor::importFromFile()
{
    const QString path = QFileDialog::getOpenFileName(panel(), i18nc("@title:window", "Signature Image"), QString(),
                                                      i18n("Images (*.png *.jpg *.jpeg *.webp *.tif *.tiff);;"
                                                           "All files (*)"));
    if (path.isEmpty()) {
        return;
    }

    QImage image(path);
    if (image.isNull()) {
        KMessageBox::error(panel(), i18n("“%1” could not be read as an image.", QFileInfo(path).fileName()));
        return;
    }

    // A photographed signature is ink on paper; without this it stamps a white
    // rectangle over the document.
    const QImage prepared = SignatureStore::trim(SignatureStore::removeBackground(image));

    bool ok = false;
    const QString name
        = QInputDialog::getText(panel(), i18nc("@title:window", "Name This Signature"), i18nc("@label", "Name:"),
                                QLineEdit::Normal, QFileInfo(path).completeBaseName(), &ok);
    if (!ok) {
        return;
    }

    const QString id = SignatureStore::save(
        prepared, name.trimmed().isEmpty() ? QFileInfo(path).completeBaseName() : name.trimmed());
    if (id.isEmpty()) {
        KMessageBox::error(panel(), i18n("The signature could not be stored."));
        return;
    }
    refreshList(id);
}

void SignEditor::drawNew()
{
    QDialog sheet(panel());
    sheet.setWindowTitle(i18nc("@title:window", "Draw Your Signature"));

    auto *canvas = new SignatureCanvas(&sheet);
    auto *clearButton = new QPushButton(QIcon::fromTheme(QStringLiteral("edit-clear")),
                                        i18nc("@action:button", "Start Again"), &sheet);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &sheet);
    QPushButton *accept = buttons->button(QDialogButtonBox::Ok);
    accept->setText(i18nc("@action:button", "Use This"));
    accept->setEnabled(false);

    QObject::connect(canvas, &SignatureCanvas::changed, &sheet,
                     [canvas, accept] { accept->setEnabled(!canvas->isEmpty()); });
    QObject::connect(clearButton, &QPushButton::clicked, canvas, &SignatureCanvas::clearCanvas);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &sheet, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &sheet, &QDialog::reject);

    auto *layout = new QVBoxLayout(&sheet);
    layout->addWidget(new QLabel(i18n("Sign in the box below."), &sheet));
    layout->addWidget(canvas, 1);
    layout->addWidget(clearButton);
    layout->addWidget(buttons);

    if (sheet.exec() != QDialog::Accepted || canvas->isEmpty()) {
        return;
    }

    bool ok = false;
    const QString name
        = QInputDialog::getText(panel(), i18nc("@title:window", "Name This Signature"), i18nc("@label", "Name:"),
                                QLineEdit::Normal, i18nc("@item default signature name", "My signature"), &ok);
    if (!ok) {
        return;
    }

    const QString id = SignatureStore::save(canvas->signature(), name.trimmed());
    if (!id.isEmpty()) {
        refreshList(id);
    }
}

void SignEditor::deleteSelected()
{
    const int row = m_list->currentRow();
    if (row < 0 || row >= m_entries.size()) {
        return;
    }

    const SignatureStore::Entry entry = m_entries.at(row);
    if (KMessageBox::warningContinueCancel(panel(), i18n("Remove the signature “%1” from this machine?", entry.name),
                                           i18nc("@title:window", "Remove Signature"), KStandardGuiItem::del())
        != KMessageBox::Continue) {
        return;
    }

    SignatureStore::remove(entry.id);
    refreshList();
}

} // namespace ps
