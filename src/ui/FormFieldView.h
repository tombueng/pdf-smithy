/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#pragma once

#include "PageCanvas.h"
#include "core/Forms.h"

#include <QVector>

namespace ps {

/**
 * The page with its form fields drawn on it.
 *
 * Filling in a form from a list of labels works right up until the labels are
 * ambiguous, which on a real form they always are: three boxes called "Date",
 * a row of initials, a table where the heading is four rows above the field.
 * Showing where each one actually sits answers that without the user having to
 * guess, and clicking a box on the page is how anyone would expect to choose
 * which field they are typing into.
 */
class FormFieldView : public PageCanvas
{
    Q_OBJECT

public:
    explicit FormFieldView(QWidget *parent = nullptr);

    /**
     * The fields on the page currently shown.
     *
     * Indices are into the caller's own list, so that picking one says
     * something the caller can act on rather than something it has to look up.
     */
    void setFields(const QVector<FormField> &fields, const QVector<int> &indices);

    /** Draws @p index as the one being edited; -1 for none. */
    void setCurrentField(int index);

Q_SIGNALS:
    void fieldPicked(int index);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;

private:
    QVector<FormField> m_fields;
    QVector<int> m_indices;
    int m_current = -1;
};

} // namespace ps
