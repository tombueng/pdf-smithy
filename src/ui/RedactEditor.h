/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#pragma once

#include "EditorMode.h"

#include <QHash>
#include <QRectF>
#include <QVector>

class QLabel;
class QListWidget;
class QPushButton;
class QSpinBox;
class QWidget;

namespace ps {

class Document;
class PageProcessor;
class RedactionView;
class RenderBackend;

/**
 * Marking what has to go, and saying plainly what that means.
 *
 * Two things make this different from the other page tools. It moves between
 * pages itself, because a redaction usually spans several and having to start
 * over per page would guarantee a missed one. And it shows the words inside
 * the marked areas, so the decision is made against the actual text rather
 * than against a black rectangle that may be a few points off.
 *
 * It also states its own limits in the panel. Someone about to publish a
 * document deserves to know that an embedded drawing is removed whole and that
 * a fax-coded scan costs the page its selectable text, before they find out
 * from the result.
 */
class RedactEditor : public EditorMode
{
    Q_OBJECT

public:
    RedactEditor(Document *document, RenderBackend *backend, int startRow, PageProcessor *processor = nullptr,
                 QObject *parent = nullptr);

    QString title() const override;
    QString iconName() const override;
    bool isUnchanged() const override;
    bool commit(QString *error) override;

    /** Marked rectangles in display points, keyed by document row. */
    QHash<int, QVector<QRectF>> areasByRow() const;

    /** Total number of marked rectangles. */
    int areaCount() const;

private:
    QWidget *buildStage();
    QWidget *buildPanel();
    void showRow(int row);
    void storeCurrent();
    void refreshList();
    void refreshAffectedText();
    void goToArea(int listIndex);

    Document *m_document;
    RenderBackend *m_backend;
    PageProcessor *m_processor;
    int m_row = 0;

    RedactionView *m_view = nullptr;
    QSpinBox *m_pageBox = nullptr;
    QLabel *m_pageLabel = nullptr;
    QListWidget *m_list = nullptr;
    QPushButton *m_remove = nullptr;
    QPushButton *m_removeAll = nullptr;
    QLabel *m_affected = nullptr;
    QPushButton *m_apply = nullptr;

    QHash<int, QVector<QRectF>> m_areas;
    QVector<QPair<int, int>> m_listTargets;
};

} // namespace ps
