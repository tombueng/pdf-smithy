/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#pragma once

#include "EditorMode.h"
#include "SignatureStore.h"

#include <QImage>
#include <QVector>

class QCheckBox;
class QLabel;
class QListWidget;
class QPushButton;
class QSlider;
class QWidget;

namespace ps {

class Document;
class PageProcessor;
class PagePlacementView;
class RenderBackend;

/**
 * Pick a signature, put it where it belongs, decide which pages get it.
 *
 * The page is shown at a readable size and the signature is dragged onto it,
 * because "bottom right, about a third across" is something people point at
 * rather than measure. The stored signatures sit in the panel, so choosing a
 * different one does not cover up the page it is going onto.
 */
class SignEditor : public EditorMode
{
    Q_OBJECT

public:
    SignEditor(Document *document, RenderBackend *backend, int row, PageProcessor *processor = nullptr,
               QObject *parent = nullptr);

    QString title() const override;
    QString iconName() const override;
    bool isUnchanged() const override;
    bool commit(QString *error) override;

private:
    QWidget *buildStage();
    QWidget *buildPanel();

    void importFromFile();
    void drawNew();
    void deleteSelected();
    void refreshList(const QString &selectId = {});
    void applySelection();
    QImage selectedImage() const;

    /** Zero-based pages the signature would go on. */
    QVector<int> targetPages() const;

    Document *m_document;
    RenderBackend *m_backend;
    PageProcessor *m_processor;
    int m_row = 0;

    QVector<SignatureStore::Entry> m_entries;
    QListWidget *m_list = nullptr;
    PagePlacementView *m_view = nullptr;
    QSlider *m_size = nullptr;
    QCheckBox *m_allPages = nullptr;
    QPushButton *m_deleteButton = nullptr;
    QPushButton *m_okButton = nullptr;
    QLabel *m_hint = nullptr;

    bool m_placed = false;
};

} // namespace ps
