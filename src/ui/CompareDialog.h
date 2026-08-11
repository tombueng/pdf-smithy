/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#pragma once

#include "core/Compare.h"

#include <QDialog>

class QLabel;
class QListWidget;
class QScrollArea;

namespace ps {

class RenderBackend;

/**
 * What changed between two documents.
 *
 * The list on the left says what changed in words; the panel on the right shows
 * the two pages side by side with the differences marked. Both are needed: a
 * changed figure in a table is obvious in the picture and invisible in a word
 * list, and a swapped pair of words is the other way round.
 */
class CompareDialog : public QDialog
{
    Q_OBJECT

public:
    CompareDialog(RenderBackend *backend, const QString &leftPdf, const QString &rightPdf,
                  const Compare::Report &report, QWidget *parent = nullptr);

private:
    void showEntry(int index);

    RenderBackend *m_backend;
    QString m_left;
    QString m_right;
    Compare::Report m_report;

    QListWidget *m_list;
    QLabel *m_sheet;
    QLabel *m_words;
    QScrollArea *m_scroll;

    /** Which report entry each list row stands for. */
    QVector<int> m_rows;
};

} // namespace ps
