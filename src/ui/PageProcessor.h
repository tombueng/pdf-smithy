/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#pragma once

#include "core/Compressor.h"
#include "core/Overlay.h"
#include "core/PageCrop.h"
#include "core/Annotation.h"
#include "core/Forms.h"
#include "core/Archival.h"
#include "core/FormBuilder.h"
#include "core/PageObjects.h"
#include "core/TextEdit.h"
#include "core/NUp.h"
#include "core/PageNumbering.h"
#include "core/Redaction.h"

#ifdef PS_WITH_OCR
#include "core/ScanProcessor.h"
#endif

#include <QHash>
#include <QObject>
#include <QTemporaryDir>
#include <QVector>

class QWidget;

namespace ps {

class Document;
class RenderBackend;

/**
 * Applies the heavy operations to whichever pages the user picked.
 *
 * Text recognition and compression both work on files rather than on single
 * pages, so this does the obvious thing: writes the chosen pages out, runs the
 * operation on that little document, and swaps the results back into the same
 * slots. Doing it that way means "OCR this one page" costs the same code as
 * "OCR everything", and both land on the undo stack as a single step.
 *
 * The work runs on a worker thread behind a cancellable progress dialog, so a
 * two-hundred-page scan does not freeze the window.
 */
class PageProcessor : public QObject
{
    Q_OBJECT

public:
    PageProcessor(Document *document, RenderBackend *backend, QWidget *dialogParent);

#ifdef PS_WITH_OCR
    /** Returns false on failure or cancellation; @p error explains why. */
    bool recognizeText(const QVector<int> &rows, const ScanProcessor::Options &options, QString *error);
#endif

    bool compress(const QVector<int> &rows, const Compressor::Options &options, Compressor::Report *report,
                  QString *error);

    /** Estimates what compressing @p rows would achieve, without doing it. */
    bool estimateCompression(const QVector<int> &rows, const Compressor::Options &options,
                             Compressor::Estimate *estimate, QString *error);

    /**
     * Stamps @p image at @p rectInPoints onto every row in @p rows.
     *
     * The rectangle is in the display points of the page the user was looking
     * at; applying it to several pages puts the signature in the same place on
     * each, which is what "sign every page" means.
     */
    bool stamp(const QVector<int> &rows, const QImage &image, const QRectF &rectInPoints, QString *error);

    /** Lays a text watermark over every row in @p rows. */
    bool watermark(const QVector<int> &rows, const Overlay::TextStamp &stamp, QString *error);

    /** Lays a picture over every row in @p rows, anchored and sized relatively. */
    bool imageWatermark(const QVector<int> &rows, const QImage &image, Overlay::Anchor anchor, double relativeWidth,
                        double rotation, double opacity, QString *error);

    /** Stamps page numbers, headers or Bates identifiers onto @p rows. */
    bool numberPages(const QVector<int> &rows, const PageNumbering::Options &options, QString *error);

    /** Sets or clears the visible area of @p rows. */
    bool crop(const QVector<int> &rows, const PageCrop::Margins &margins, bool reset, QString *error);

    /**
     * Takes the marked areas out of the pages they were marked on.
     *
     * @p areasByRow maps a document row to rectangles in that page's display
     * points. Unlike the other operations here this one is not reversible in
     * the finished file: undo puts the old pages back in the organiser, but
     * once the document has been saved the content is gone, which is the whole
     * point of it.
     */
    bool redact(const QHash<int, QVector<QRectF>> &areasByRow, Redaction::Report *report, QString *error);

    /**
     * Rearranges @p rows onto sheets, several pages to each.
     *
     * The only operation here that changes how many pages there are, so it
     * cannot swap results in one for one like the others. It takes the pages
     * out and puts the sheets in their place, wrapped in one undo step.
     *
     * @p sheetCount receives how many sheets came out.
     */
    bool arrangeOnSheets(const QVector<int> &rows, const NUp::Options &options, int *sheetCount, QString *error);

    /**
     * Replaces the comments on every page mentioned in @p annotationsByRow.
     *
     * Whole-page replacement rather than a list of additions, because the
     * dialog lets someone delete a comment that was already there, and "add
     * these and also remove those" is two operations pretending to be one.
     * Pages not mentioned keep whatever they had.
     */
    bool annotate(const QHash<int, QVector<Annotation>> &annotationsByRow, QString *error);

    /** Writes every comment in the document to an XFDF file. */
    bool exportComments(const QString &xfdfPath, QString *error);

    /**
     * Writes @p rows to a scratch file and hands back its path.
     *
     * For the operations that have to look at a whole file before deciding
     * anything, such as reading a form. Public because a dialog needs to
     * see the fields before it can offer to fill them in.
     */
    QString writeScratchCopy(const QVector<int> &rows, QString *error);

    /**
     * Writes the document as PDF/A, for keeping rather than for editing.
     *
     * Not an undo step: the result is a separate file, because converting for an
     * archive and carrying on editing are different intentions.
     */
    bool writeArchival(const QString &path, const Archival::Options &options, Archival::Report *report, QString *error);

    /** Replaces text runs on the given rows, as one undo step. */
    bool correctText(const QVector<TextEdit::Replacement> &replacements, const TextEdit::Options &options,
                     TextEdit::Report *report, QString *error);

    /**
     * Applies what the user moved, resized, recoloured or removed on a page.
     *
     * One undo step for the whole batch, because the page is rewritten in a
     * single pass: undoing a drag that also recoloured two things and deleted a
     * third one item at a time would put the document through states it was
     * never in.
     */
    bool composeObjects(const QVector<PageEdit> &edits, const QVector<NewContent> &additions,
                        PageComposer::Report *report, QString *error);

    /**
     * Applies what was drawn, moved, renamed and deleted on the form.
     *
     * One undo step for all four stages, because they are one intention: a
     * user who moves a field and renames it has done one thing, and undoing
     * half of it would leave the document in a state they never asked for.
     */
    bool buildForm(const QStringList &removed, const QVector<QPair<QString, QString>> &renamed,
                   const QVector<FormBuilder::Field> &changed, const QVector<FormBuilder::Field> &added,
                   QString *error);

    /** Fills in form fields, optionally making the answers permanent. */
    bool fillForm(const QHash<QString, QString> &values, bool flatten, int *filled, QStringList *notes, QString *error);

    /** Adds the comments an XFDF file holds, as one undo step. */
    bool importComments(const QString &xfdfPath, int *added, QStringList *warnings, QString *error);

private:
    /** Writes @p rows to a scratch file and returns its path, or empty. */
    QString extractRows(const QVector<int> &rows, QString *error);

    /**
     * Points @p rows at the pages of @p processedFile, one for one, as a
     * single undoable step.
     */
    bool adoptResult(const QVector<int> &rows, const QString &processedFile, const QString &undoText, QString *error);

    Document *m_document;
    RenderBackend *m_backend;
    QWidget *m_dialogParent;

    /**
     * Processed pages live here for the session. The document keeps them open,
     * so they must outlive the operation: they are the pages now.
     */
    QTemporaryDir m_scratch;
    int m_counter = 0;
};

} // namespace ps
