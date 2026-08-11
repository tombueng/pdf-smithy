/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#pragma once

#include "EditorMode.h"
#include "core/Overlay.h"

#include <QImage>
#include <QVector>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QRadioButton;
class QSlider;
class QSpinBox;
class QWidget;

namespace ps {

class Document;
class PageProcessor;
class RenderBackend;
class WatermarkView;

/**
 * A watermark, previewed on the page it is going onto.
 *
 * Every setting here (the angle, how faint, solid or outlined, where it sits)
 * is a judgement about how it looks against *this* document. Deciding that
 * against a white rectangle with the word DRAFT on it, which is what a settings
 * dialog can offer, means applying it, looking, undoing and starting again. So
 * the page is on the stage and the settings are in the panel, and every change
 * shows up immediately.
 */
class WatermarkEditor : public EditorMode
{
    Q_OBJECT

public:
    WatermarkEditor(Document *document, RenderBackend *backend, const QVector<int> &selection,
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
    void chooseImage();

    Overlay::TextStamp stamp() const;
    Overlay::Anchor chosenAnchor() const;

    /** The rows the watermark would go on, given the scope chosen. */
    QVector<int> targetRows() const;

    Document *m_document;
    RenderBackend *m_backend;
    PageProcessor *m_processor;
    QVector<int> m_selection;
    int m_row = 0;

    WatermarkView *m_view = nullptr;
    QSpinBox *m_pageBox = nullptr;

    QRadioButton *m_selectionOnly = nullptr;
    QRadioButton *m_wholeDocument = nullptr;

    QRadioButton *m_useText = nullptr;
    QRadioButton *m_useImage = nullptr;

    QComboBox *m_preset = nullptr;
    QLineEdit *m_text = nullptr;
    QSpinBox *m_size = nullptr;
    QSlider *m_opacity = nullptr;
    QSlider *m_angle = nullptr;
    QCheckBox *m_outlineOnly = nullptr;
    QPushButton *m_colourButton = nullptr;
    QColor m_colour = QColor(200, 30, 30);

    QPushButton *m_imageButton = nullptr;
    QComboBox *m_position = nullptr;
    QSlider *m_imageSize = nullptr;
    QLabel *m_imageName = nullptr;
    QImage m_image;

    /** Whether the user has actually decided anything here yet. */
    bool m_touched = false;
};

} // namespace ps
