/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "DocumentCommands.h"

#include "MainWindow.h"
#include "core/Document.h"

#include <KLocalizedString>

namespace ps {

SetMetadataCommand::SetMetadataCommand(Document *document, const Metadata::Fields &fields, QUndoCommand *parent)
    : QUndoCommand(i18nc("@action:inmenu undo step", "Change the document properties"), parent)
    , m_document(document)
    , m_before(document->metadata())
    , m_after(fields)
{
}

void SetMetadataCommand::redo()
{
    m_document->setMetadata(m_after);
}

void SetMetadataCommand::undo()
{
    m_document->setMetadata(m_before);
}

CleanUpCommand::CleanUpCommand(Document *document, MainWindow *window, bool removeMetadata, bool stripInteractivity,
                               QUndoCommand *parent)
    : QUndoCommand(i18nc("@action:inmenu undo step", "Clean up the document"), parent)
    , m_document(document)
    , m_window(window)
    , m_before(document->metadata())
    , m_after(removeMetadata ? Metadata::Fields {} : document->metadata())
    , m_stripBefore(window && window->stripsInteractivity())
    , m_stripAfter(stripInteractivity)
{
}

void CleanUpCommand::redo()
{
    apply(m_after, m_stripAfter);
}

void CleanUpCommand::undo()
{
    apply(m_before, m_stripBefore);
}

void CleanUpCommand::apply(const Metadata::Fields &fields, bool strip)
{
    m_document->setMetadata(fields);
    if (m_window) {
        m_window->setStripInteractivity(strip);
    }
}

} // namespace ps
