/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#pragma once

#include "core/Metadata.h"

#include <QPointer>
#include <QUndoCommand>

namespace ps {

class Document;
class MainWindow;

/**
 * The two edits that are about the document rather than about its pages.
 *
 * They live here rather than beside the page commands because one of them
 * reaches into the window: what a clean-up leaves out of the file is a decision
 * the writer is told about, and that decision has to travel with undo like
 * every other. Both used to mark the document dirty by hand and push nothing at
 * all, which meant the next Ctrl+Z undid whatever the user had done *before*.
 * A rotation vanishing when someone changed the title is the kind of surprise
 * that teaches people not to trust undo.
 */

/** Sets what the document says about itself: title, author, subject, keywords. */
class SetMetadataCommand : public QUndoCommand
{
public:
    SetMetadataCommand(Document *document, const Metadata::Fields &fields, QUndoCommand *parent = nullptr);

    void redo() override;
    void undo() override;

private:
    Document *m_document;
    Metadata::Fields m_before;
    Metadata::Fields m_after;
};

/**
 * Clean-up: what the document says about itself, and what a save leaves behind.
 *
 * @p window is held weakly because the undo stack belongs to the document,
 * which the window owns, so during teardown the stack outlives the window by
 * exactly the length of one destructor.
 */
class CleanUpCommand : public QUndoCommand
{
public:
    CleanUpCommand(Document *document, MainWindow *window, bool removeMetadata, bool stripInteractivity,
                   QUndoCommand *parent = nullptr);

    void redo() override;
    void undo() override;

private:
    void apply(const Metadata::Fields &fields, bool strip);

    Document *m_document;
    QPointer<MainWindow> m_window;
    Metadata::Fields m_before;
    Metadata::Fields m_after;
    bool m_stripBefore;
    bool m_stripAfter;
};

} // namespace ps
