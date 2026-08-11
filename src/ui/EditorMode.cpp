/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "EditorMode.h"

#include <QWidget>

namespace ps {

EditorMode::~EditorMode()
{
    // Only what is still standing. If the window went first it took these with
    // it, and the guarded pointers are already empty.
    delete m_stage.data();
    delete m_panel.data();
}

void EditorMode::setWidgets(QWidget *stage, QWidget *panel)
{
    m_stage = stage;
    m_panel = panel;
}

QWidget *EditorMode::stage() const
{
    return m_stage;
}

QWidget *EditorMode::panel() const
{
    return m_panel;
}

} // namespace ps
