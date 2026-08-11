/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#pragma once

class QWidget;

namespace ps {

class Document;
class RenderBackend;

/**
 * The editing tools, as one entry point each.
 *
 * Declared here rather than as eight headers of eight classes because that is
 * all the window needs to know: it has a document, a parent and a rendering
 * backend, and it wants the tool put in front of the user. Whether the tool is
 * one dialog, three tabs or a wizard is the tool's own business, and keeping
 * that out of MainWindow is what stops a 1700-line file becoming a 4000-line
 * one.
 *
 * @p page is the page the user has selected, or 0. The tools that work on one
 * page start there; the rest ignore it.
 */
namespace tools {

void showPreflight(Document *document, QWidget *parent);
void showColour(Document *document, QWidget *parent);
void showFonts(Document *document, QWidget *parent);
void showImages(Document *document, QWidget *parent);
void showLayout(Document *document, QWidget *parent);
void showFormBuilder(Document *document, RenderBackend *backend, int page, QWidget *parent);
void showObjects(Document *document, RenderBackend *backend, int page, QWidget *parent);
void showTypeset(Document *document, QWidget *parent);

} // namespace tools

} // namespace ps
