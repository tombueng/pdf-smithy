/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#pragma once

#include "EditorMode.h"
#include "core/PageCrop.h"

#include <QVector>

class QCheckBox;
class QDoubleSpinBox;
class QLabel;
class QRadioButton;
class QSpinBox;
class QWidget;

namespace ps {

class CropView;
class Document;
class PageProcessor;
class RenderBackend;

/**
 * Setting the visible area of a page by dragging its edges.
 *
 * Four numbers in points is how a trim is *recorded*; it is not how anyone
 * decides one. So the page is on the stage with the part being kept bright and
 * the rest dimmed, the edges are dragged, and the numbers follow, and can be
 * typed, which is how the same trim is repeated on another document.
 *
 * Nothing is cut away either way: trimming sets the crop box, so it can be
 * taken back at any time and the page keeps its content.
 */
class CropEditor : public EditorMode
{
    Q_OBJECT

public:
    CropEditor(Document *document, RenderBackend *backend, const QVector<int> &selection,
               PageProcessor *processor = nullptr, QObject *parent = nullptr);

    QString title() const override;
    QString iconName() const override;
    bool isUnchanged() const override;
    bool commit(QString *error) override;

private:
    QWidget *buildStage();
    QWidget *buildPanel();
    void showRow(int row);
    void detectMargins();
    void pushToBoxes(const PageCrop::Margins &margins);
    PageCrop::Margins fromBoxes() const;

    /** The rows the trim would apply to, given the scope chosen. */
    QVector<int> targetRows() const;

    Document *m_document;
    RenderBackend *m_backend;
    PageProcessor *m_processor;
    QVector<int> m_selection;
    int m_row = 0;

    CropView *m_view = nullptr;
    QSpinBox *m_pageBox = nullptr;
    QRadioButton *m_selectionOnly = nullptr;
    QRadioButton *m_wholeDocument = nullptr;
    QDoubleSpinBox *m_left = nullptr;
    QDoubleSpinBox *m_right = nullptr;
    QDoubleSpinBox *m_top = nullptr;
    QDoubleSpinBox *m_bottom = nullptr;
    QCheckBox *m_reset = nullptr;
    QLabel *m_note = nullptr;
};

} // namespace ps
