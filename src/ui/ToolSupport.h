/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#pragma once

#include <QString>
#include <QStringList>

#include <functional>

class QWidget;

namespace ps {

class Document;

/**
 * What the editing tools need from the window, without needing the window.
 *
 * The engine modules (preflight, colour, fonts, images, layout, forms, objects)
 * all work on a **file**, because that is what a PDF is and what a script has
 * to be able to hand them. The window works on a Document, which is pages
 * gathered from possibly several files and not necessarily written down
 * anywhere. Bridging the two is the same three steps every time: make sure
 * there is a file, run the thing, put the result back in front of the user.
 *
 * Doing that three-step in each dialog would mean eight chances to forget the
 * middle one, and the failure mode of forgetting it is a tool that silently
 * works on the version before the user's last edit.
 */
namespace tools {

/**
 * Ensures @p document exists on disk, saving it first if need be.
 *
 * @returns the path, or an empty string when the user declined to save. An
 * unsaved document is not an error worth a message box: the user was asked and
 * said no.
 */
QString savedPath(Document *document, QWidget *parent);

/**
 * Runs @p work, which writes a new file, and offers the result to the user.
 *
 * @p work is given an output path to write to and returns false on failure,
 * having filled the error. On success the user is asked whether to open the
 * result, because these tools produce a *new* document, and quietly replacing
 * what someone has open is the kind of helpfulness that loses work.
 *
 * @p work also fills a summary, which is shown alongside, so "what did it
 * actually do" is answered before the question is asked rather than after. It
 * is an out-parameter rather than something the caller passes in because what a
 * tool *did* and what it was *asked* to do are not the same list: a fix can
 * find nothing to mend, and saying "embedded the fonts" when none were missing
 * teaches the user to distrust the report.
 */
bool runProducing(Document *document, QWidget *parent, const QString &title, const QString &suffix,
                  const std::function<bool(const QString &output, QStringList *summary, QString *error)> &work);

/** Shows @p lines as a report, with a monospaced body when they are tabular. */
void showReport(QWidget *parent, const QString &title, const QStringList &lines, bool monospaced = false);

} // namespace tools

} // namespace ps
