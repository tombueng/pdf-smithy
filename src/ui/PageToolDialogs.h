/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#pragma once

#include "core/NUp.h"
#include "core/PageCrop.h"
#include "core/PageNumbering.h"
#include "core/Splitter.h"

#include <QDialog>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QRadioButton;
class QSpinBox;

namespace ps {

class Document;
class RenderBackend;

/**
 * Breaking a document apart.
 *
 * Four ways of saying where the cuts go, and, importantly, a list of the
 * files that would come out, updated as the settings change. Writing twenty
 * files is not something anyone should have to do speculatively to find out
 * what they get.
 */
class SplitDialog : public QDialog
{
    Q_OBJECT

public:
    SplitDialog(const Document *document, const QString &sourcePath, QWidget *parent = nullptr);

    Splitter::Options options() const;

private:
    void updatePreview();

    const Document *m_document;
    QString m_sourcePath;
    QVector<Splitter::Chapter> m_chapters;

    QRadioButton *m_everyN;
    QRadioButton *m_ranges;
    QRadioButton *m_bySize;
    QRadioButton *m_byBookmarks;
    QSpinBox *m_pagesPerFile;
    QLineEdit *m_rangeText;
    QSpinBox *m_maxMegabytes;
    QLineEdit *m_outputTemplate;
    QListWidget *m_preview;
    QLabel *m_summary;
};

// ══════════════════════════════════════════════════════════════════════════

/**
 * Several pages onto one sheet, as a file rather than as a print job.
 *
 * Deliberately short: how many to a sheet, how much air around them, and
 * whether to draw a line between. The preview to the right shows the
 * arrangement rather than the pages, because the arrangement is the only thing
 * that is being decided here.
 */
class NUpDialog : public QDialog
{
    Q_OBJECT

public:
    NUpDialog(int selectedCount, int totalCount, bool pagesAreLandscape, QWidget *parent = nullptr);

    bool appliesToSelectionOnly() const;
    NUp::Options options() const;

private:
    void refresh();

    bool m_pagesAreLandscape;

    QRadioButton *m_selectionOnly;
    QRadioButton *m_wholeDocument;
    QComboBox *m_perSheet;
    QDoubleSpinBox *m_margin;
    QDoubleSpinBox *m_gutter;
    QCheckBox *m_borders;
    QCheckBox *m_downFirst;
    QLabel *m_summary;
    QWidget *m_preview;
};

} // namespace ps
