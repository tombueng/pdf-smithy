/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#pragma once

#include "EditorMode.h"
#include "core/PageNumbering.h"

#include <QVector>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QRadioButton;
class QSpinBox;
class QWidget;

namespace ps {

class Document;
class PageProcessor;
class RenderBackend;
class WatermarkView;

/**
 * Page numbers, headers, footers and Bates stamping, shown where they land.
 *
 * The two things that go wrong with a numbering run are both invisible in a
 * settings dialog: the number sits on top of something the page already has
 * there, and the run starts at the wrong page. Both are obvious the moment the
 * number is drawn on the actual page, and turning to the page where the run
 * begins answers the second one outright.
 *
 * The preview borrows the watermark's page view, because a page number is a
 * text stamp with the angle set to nothing.
 */
class NumberEditor : public EditorMode
{
    Q_OBJECT

public:
    NumberEditor(Document *document, RenderBackend *backend, const QVector<int> &selection,
                 PageProcessor *processor = nullptr, QObject *parent = nullptr);

    QString title() const override;
    QString iconName() const override;
    bool isUnchanged() const override;
    bool commit(QString *error) override;

private:
    QWidget *buildStage();
    QWidget *buildPanel();
    void showRow(int row);
    void refreshPreview();

    PageNumbering::Options options() const;

    /** The rows the run covers, given the scope chosen. */
    QVector<int> targetRows() const;

    Document *m_document;
    RenderBackend *m_backend;
    PageProcessor *m_processor;
    QVector<int> m_selection;
    QString m_fileName;
    int m_row = 0;

    WatermarkView *m_view = nullptr;
    QSpinBox *m_pageBox = nullptr;
    QLabel *m_rendered = nullptr;

    QRadioButton *m_selectionOnly = nullptr;
    QRadioButton *m_wholeDocument = nullptr;
    QComboBox *m_preset = nullptr;
    QLineEdit *m_text = nullptr;
    QComboBox *m_position = nullptr;
    QSpinBox *m_fontSize = nullptr;
    QSpinBox *m_margin = nullptr;
    QSpinBox *m_startNumber = nullptr;
    QLineEdit *m_batesPrefix = nullptr;
    QSpinBox *m_batesDigits = nullptr;
    QPushButton *m_colourButton = nullptr;
    QColor m_colour = QColor(60, 60, 60);

    /** Whether the user has actually decided anything here yet. */
    bool m_touched = false;
};

} // namespace ps
