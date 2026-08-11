/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#pragma once

#include <QObject>
#include <QPointer>
#include <QString>

class QWidget;

namespace ps {

/**
 * One way of editing a page, shown in the window rather than in a dialog.
 *
 * ## Why not a dialog
 *
 * Marking up a page, blacking something out, correcting a line of text: all of
 * them are things a person does *to a document they are looking at*. A modal
 * dialog takes the document away and hands back a smaller copy of it, which
 * means the window behind is useless while the work is happening: no
 * scrolling to the page before to check a heading, no undo history, no
 * comparing against what a different page did. Worse, it makes every one of
 * these tools its own little application with its own page navigation, its own
 * zoom and its own idea of which page you are on.
 *
 * So an editing mode gives the window two widgets and lets the window place
 * them: the stage goes where the page grid was, and the panel goes into a dock
 * the user can drag to another edge, tear off, or close.
 *
 * ## The contract
 *
 * Both widgets belong to the mode for as long as the mode lives, whoever they
 * happen to be parented to in the meantime, which is why they are held here
 * and not in each subclass. The window borrows them, gives them back, and the
 * mode takes them with it when it goes. A mode is used once: entered, then
 * either committed or abandoned. That keeps the "did the user mean it"
 * question in one place instead of in every tool.
 */
class EditorMode : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;
    ~EditorMode() override;

    /** The page itself: goes where the grid was. */
    QWidget *stage() const;

    /** The tools: go into a dock at the edge of the window. */
    QWidget *panel() const;

    /** Names the dock, and says in the window what is being done. */
    virtual QString title() const = 0;

    /** An icon name for the dock, or empty. */
    virtual QString iconName() const
    {
        return {};
    }

    /**
     * True when leaving now would lose nothing.
     *
     * Asked before the "keep your changes?" question, so that a mode entered
     * and left again does not nag.
     */
    virtual bool isUnchanged() const
    {
        return false;
    }

    /**
     * Writes what was done into the document.
     *
     * Pushes onto the undo stack like any other edit (that is the whole point
     * of doing this in the window) and returns false with @p error set when
     * it could not.
     */
    virtual bool commit(QString *error) = 0;

Q_SIGNALS:
    /** Asks the window to leave this mode, committing or not. */
    void finished(bool commit);

    /** A line for the status bar while the mode is up. */
    void statusMessage(const QString &text);

protected:
    /**
     * Hands the two widgets over, once, from the subclass's constructor.
     *
     * They are held as guarded pointers rather than plain ones: the window
     * parents them to a stack and a dock while the mode is up, and if the
     * window itself goes first, Qt destroys them from that side. Deleting them
     * again from here is exactly the double free that guard is for.
     */
    void setWidgets(QWidget *stage, QWidget *panel);

private:
    QPointer<QWidget> m_stage;
    QPointer<QWidget> m_panel;
};

} // namespace ps
