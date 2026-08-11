/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#pragma once

#include "core/Compressor.h"
#include "core/Overlay.h"

#ifdef PS_WITH_OCR
#include "core/ScanProcessor.h"
#endif

#include <QDialog>
#include <QImage>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QSlider;
class QSpinBox;
class QRadioButton;

namespace ps {

/**
 * Where inserted pages should land.
 *
 * Appending to the end is only right about half the time; the rest of the time
 * the user has a page selected precisely because that is where the new material
 * belongs.
 */
class InsertPositionDialog : public QDialog
{
    Q_OBJECT

public:
    enum class Position {
        Start,
        BeforeSelection,
        AfterSelection,
        End,
    };
    Q_ENUM(Position)

    /**
     * @p selectedPage is the 1-based page the choice is relative to, or 0 when
     * nothing is selected, in which case those options are unavailable rather
     * than silently meaning something else.
     */
    InsertPositionDialog(int selectedPage, int fileCount, QWidget *parent = nullptr);

    Position position() const;

    /** Remembered between invocations so a repeated workflow stays quick. */
    void setPosition(Position position);

private:
    QRadioButton *m_start;
    QRadioButton *m_before;
    QRadioButton *m_after;
    QRadioButton *m_end;
};

// ══════════════════════════════════════════════════════════════════════════

/**
 * How many empty pages to add, and how big.
 *
 * Defaults to matching the document it is being added to, because a blank page
 * that is a different size from its neighbours is nearly always a mistake, and
 * when it is not, it is a deliberate act worth two clicks.
 */
class BlankPagesDialog : public QDialog
{
    Q_OBJECT

public:
    /**
     * @p matchSize is the size of the page the new ones will sit beside, or an
     * empty size when the document has no pages to match, in which case that
     * option is not offered rather than quietly meaning something else.
     */
    explicit BlankPagesDialog(const QSizeF &matchSize, QWidget *parent = nullptr);

    int pageCount() const;

    /** In points, as the reader will see it: a landscape choice is a wide size. */
    QSizeF sizePoints() const;

private:
    QSizeF m_matchSize;
    QSpinBox *m_count;
    QComboBox *m_size;
    QRadioButton *m_upright;
    QRadioButton *m_sideways;
};

// ══════════════════════════════════════════════════════════════════════════

/** What to shrink, how hard, and an honest word about what it costs. */
class CompressDialog : public QDialog
{
    Q_OBJECT

public:
    CompressDialog(int selectedCount, int totalCount, QWidget *parent = nullptr);

    bool appliesToSelectionOnly() const;
    Compressor::Options options() const;

    /**
     * Called when the user asks what it would achieve. Given the chosen
     * options, it must return an estimate; the dialog has no document.
     */
    using Estimator = std::function<bool(const Compressor::Options &, Compressor::Estimate *, QString *)>;
    void setEstimator(Estimator estimator);

private Q_SLOTS:
    void runEstimate();

private:
    QRadioButton *m_selectionOnly;
    QRadioButton *m_wholeDocument;
    QComboBox *m_level;
    QCheckBox *m_grayscale;
    QLabel *m_note;
    QPushButton *m_estimateButton;
    QLabel *m_estimateResult;
    Estimator m_estimator;
};

// ══════════════════════════════════════════════════════════════════════════

#ifdef PS_WITH_OCR
/** Languages, resolution and what to do about crooked pages. */
class ScanDialog : public QDialog
{
    Q_OBJECT

public:
    ScanDialog(int selectedCount, int totalCount, QWidget *parent = nullptr);

    bool appliesToSelectionOnly() const;
    ScanProcessor::Options options() const;

    /** False when Tesseract has no language data at all. */
    static bool isUsable();

private:
    QRadioButton *m_selectionOnly;
    QRadioButton *m_wholeDocument;
    QListWidget *m_languages;
    QComboBox *m_resolution;
    QCheckBox *m_straighten;
    QCheckBox *m_skipPagesWithText;
    QCheckBox *m_evenOutLighting;
    QSpinBox *m_despeckle;
};
#endif

} // namespace ps
