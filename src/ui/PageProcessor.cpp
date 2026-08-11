/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "PageProcessor.h"

#include "core/Document.h"
#include "core/DocumentWriter.h"
#include "core/RenderBackend.h"
#include "core/Source.h"
#include "core/commands/PageCommands.h"

#include <QApplication>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QProgressDialog>
#include <QUndoStack>
#include <QtConcurrent>

#include <KLocalizedString>

#include <numeric>

namespace ps {

PageProcessor::PageProcessor(Document *document, RenderBackend *backend, QWidget *dialogParent)
    : QObject(dialogParent)
    , m_document(document)
    , m_backend(backend)
    , m_dialogParent(dialogParent)
{
}

QString PageProcessor::extractRows(const QVector<int> &rows, QString *error)
{
    if (!m_scratch.isValid()) {
        if (error) {
            *error = i18n("No scratch space is available for processing.");
        }
        return {};
    }

    const QString path = m_scratch.filePath(QStringLiteral("in-%1.pdf").arg(++m_counter));
    if (!DocumentWriter::writeSelection(*m_document, rows, path, {}, error)) {
        return {};
    }
    return path;
}

bool PageProcessor::adoptResult(const QVector<int> &rows, const QString &processedFile, const QString &undoText,
                                QString *error)
{
    const int sourceId = m_document->addSource(processedFile, error);
    if (sourceId < 0) {
        return false;
    }

    Source *source = m_document->source(sourceId);
    if (!source || source->pageCount() != rows.size()) {
        if (error) {
            *error = i18n("The processed document came back with a different number of pages.");
        }
        return false;
    }

    QVector<PageRef> replacements;
    replacements.reserve(rows.size());
    for (int i = 0; i < rows.size(); ++i) {
        // Rotation was baked into the written file, so the new references start
        // upright rather than turning twice.
        replacements.append(PageRef { sourceId, i, 0 });
    }

    m_document->undoStack()->push(new ReplacePagesCommand(m_document, rows, replacements, undoText));
    return true;
}

#ifdef PS_WITH_OCR
bool PageProcessor::recognizeText(const QVector<int> &rows, const ScanProcessor::Options &options, QString *error)
{
    if (rows.isEmpty()) {
        return false;
    }

    const QString input = extractRows(rows, error);
    if (input.isEmpty()) {
        return false;
    }
    const QString output = m_scratch.filePath(QStringLiteral("ocr-%1.pdf").arg(m_counter));

    ScanProcessor processor;

    QProgressDialog progress(i18n("Recognising text…"), i18n("Cancel"), 0, rows.size(), m_dialogParent);
    progress.setWindowTitle(i18nc("@title:window", "Text Recognition"));
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(300);
    progress.setAutoClose(false);
    progress.setAutoReset(false);

    connect(&processor, &ScanProcessor::progress, &progress, [&progress](int page, int count) {
        progress.setMaximum(count);
        progress.setValue(page);
        progress.setLabelText(i18n("Recognising text on page %1 of %2…", page, count));
    });
    connect(&progress, &QProgressDialog::canceled, &processor, &ScanProcessor::cancel);

    ScanProcessor::Report report;
    QString workerError;
    QFutureWatcher<bool> watcher;
    QEventLoop loop;
    connect(&watcher, &QFutureWatcher<bool>::finished, &loop, &QEventLoop::quit);

    watcher.setFuture(
        QtConcurrent::run([&] { return processor.process(input, output, m_backend, options, &report, &workerError); }));
    loop.exec();
    progress.close();

    if (!watcher.result()) {
        if (error) {
            *error = workerError;
        }
        return false;
    }

    return adoptResult(
        rows, output,
        i18ncp("@action:inmenu undo step", "Recognise text on page", "Recognise text on %1 pages", rows.size()), error);
}
#endif

bool PageProcessor::compress(const QVector<int> &rows, const Compressor::Options &options, Compressor::Report *report,
                             QString *error)
{
    if (rows.isEmpty()) {
        return false;
    }

    const QString input = extractRows(rows, error);
    if (input.isEmpty()) {
        return false;
    }
    const QString output = m_scratch.filePath(QStringLiteral("small-%1.pdf").arg(m_counter));

    // Ghostscript reports no progress it is worth relaying, so this is a busy
    // indicator rather than a lie about how far along it is.
    QProgressDialog progress(i18n("Making pages smaller…"), QString(), 0, 0, m_dialogParent);
    progress.setWindowTitle(i18nc("@title:window", "Compressing"));
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(300);

    Compressor::Report local;
    QString workerError;
    QFutureWatcher<bool> watcher;
    QEventLoop loop;
    connect(&watcher, &QFutureWatcher<bool>::finished, &loop, &QEventLoop::quit);

    watcher.setFuture(
        QtConcurrent::run([&] { return Compressor::compress(input, output, options, &local, &workerError); }));
    loop.exec();
    progress.close();

    if (!watcher.result()) {
        if (error) {
            *error = workerError;
        }
        return false;
    }

    if (report) {
        *report = local;
    }

    // Nothing was gained, so nothing is changed. Swapping in an identical copy
    // would only add a pointless undo step and a second file to carry around.
    if (local.outcome == Compressor::Outcome::AlreadyOptimal) {
        return true;
    }

    return adoptResult(rows, output,
                       i18ncp("@action:inmenu undo step", "Compress page", "Compress %1 pages", rows.size()), error);
}

bool PageProcessor::estimateCompression(const QVector<int> &rows, const Compressor::Options &options,
                                        Compressor::Estimate *estimate, QString *error)
{
    if (rows.isEmpty()) {
        return false;
    }

    const QString whole = extractRows(rows, error);
    if (whole.isEmpty()) {
        return false;
    }

    // The sampler writes one page at a time; the estimator decides which.
    int serial = 0;
    const auto extractOne = [this, &serial](int row) -> QString {
        const QString path = m_scratch.filePath(QStringLiteral("sample-%1-%2.pdf").arg(m_counter).arg(++serial));
        if (!DocumentWriter::writeSelection(*m_document, { row }, path, {}, nullptr)) {
            return {};
        }
        return path;
    };

    return Compressor::estimate(whole, rows, extractOne, options, estimate, error);
}

bool PageProcessor::stamp(const QVector<int> &rows, const QImage &image, const QRectF &rectInPoints, QString *error)
{
    if (rows.isEmpty() || image.isNull()) {
        return false;
    }

    const QString input = extractRows(rows, error);
    if (input.isEmpty()) {
        return false;
    }
    const QString output = m_scratch.filePath(QStringLiteral("signed-%1.pdf").arg(m_counter));

    // The extracted file holds the chosen pages as 0..n-1, so the stamp map is
    // keyed by position within the extract rather than by document page.
    QHash<int, QVector<Overlay::ImageStamp>> stamps;
    Overlay::ImageStamp item;
    item.image = image;
    item.rect = rectInPoints;
    for (int i = 0; i < rows.size(); ++i) {
        stamps.insert(i, { item });
    }

    if (!Overlay::stampImages(input, output, stamps, error)) {
        return false;
    }

    return adoptResult(rows, output, i18ncp("@action:inmenu undo step", "Sign page", "Sign %1 pages", rows.size()),
                       error);
}

bool PageProcessor::watermark(const QVector<int> &rows, const Overlay::TextStamp &stamp, QString *error)
{
    if (rows.isEmpty()) {
        return false;
    }

    const QString input = extractRows(rows, error);
    if (input.isEmpty()) {
        return false;
    }
    const QString output = m_scratch.filePath(QStringLiteral("marked-%1.pdf").arg(m_counter));

    QVector<int> all(rows.size());
    std::iota(all.begin(), all.end(), 0);

    if (!Overlay::stampText(input, output, all, stamp, error)) {
        return false;
    }

    return adoptResult(rows, output,
                       i18ncp("@action:inmenu undo step", "Watermark page", "Watermark %1 pages", rows.size()), error);
}

bool PageProcessor::imageWatermark(const QVector<int> &rows, const QImage &image, Overlay::Anchor anchor,
                                   double relativeWidth, double rotation, double opacity, QString *error)
{
    if (rows.isEmpty() || image.isNull()) {
        return false;
    }

    const QString input = extractRows(rows, error);
    if (input.isEmpty()) {
        return false;
    }
    const QString output = m_scratch.filePath(QStringLiteral("logo-%1.pdf").arg(m_counter));

    QVector<int> all(rows.size());
    std::iota(all.begin(), all.end(), 0);

    if (!Overlay::stampImageOnPages(input, output, all, image, anchor, relativeWidth, rotation, opacity, 24.0, error)) {
        return false;
    }

    return adoptResult(rows, output,
                       i18ncp("@action:inmenu undo step", "Watermark page", "Watermark %1 pages", rows.size()), error);
}

bool PageProcessor::numberPages(const QVector<int> &rows, const PageNumbering::Options &options, QString *error)
{
    if (rows.isEmpty()) {
        return false;
    }

    const QString input = extractRows(rows, error);
    if (input.isEmpty()) {
        return false;
    }
    const QString output = m_scratch.filePath(QStringLiteral("numbered-%1.pdf").arg(m_counter));

    // The extract holds the chosen pages as 0..n-1, so the numbering runs over
    // all of them: the number is the position in the run, as the dialog shows.
    QVector<int> all(rows.size());
    std::iota(all.begin(), all.end(), 0);

    if (!PageNumbering::apply(input, output, all, options, error)) {
        return false;
    }

    return adoptResult(rows, output, i18ncp("@action:inmenu undo step", "Number page", "Number %1 pages", rows.size()),
                       error);
}

bool PageProcessor::crop(const QVector<int> &rows, const PageCrop::Margins &margins, bool reset, QString *error)
{
    if (rows.isEmpty()) {
        return false;
    }

    const QString input = extractRows(rows, error);
    if (input.isEmpty()) {
        return false;
    }
    const QString output = m_scratch.filePath(QStringLiteral("trimmed-%1.pdf").arg(m_counter));

    QVector<int> all(rows.size());
    std::iota(all.begin(), all.end(), 0);

    const bool done
        = reset ? PageCrop::reset(input, output, all, error) : PageCrop::crop(input, output, all, margins, error);
    if (!done) {
        return false;
    }

    return adoptResult(rows, output,
                       reset ? i18ncp("@action:inmenu undo step", "Restore page", "Restore %1 pages", rows.size())
                             : i18ncp("@action:inmenu undo step", "Trim page", "Trim %1 pages", rows.size()),
                       error);
}

bool PageProcessor::redact(const QHash<int, QVector<QRectF>> &areasByRow, Redaction::Report *report, QString *error)
{
    QVector<int> rows;
    for (auto it = areasByRow.constBegin(); it != areasByRow.constEnd(); ++it) {
        if (!it.value().isEmpty()) {
            rows.append(it.key());
        }
    }
    if (rows.isEmpty()) {
        return false;
    }
    std::sort(rows.begin(), rows.end());

    const QString input = extractRows(rows, error);
    if (input.isEmpty()) {
        return false;
    }
    const QString output = m_scratch.filePath(QStringLiteral("redacted-%1.pdf").arg(m_counter));

    // In the extracted file the pages sit in the order of `rows`, so a row
    // number becomes a page index by position.
    QVector<Redaction::Area> areas;
    for (int i = 0; i < rows.size(); ++i) {
        for (const QRectF &rect : areasByRow.value(rows.at(i))) {
            areas.append({ i, rect });
        }
    }

    // Well clear of the ids the document hands out for its own sources.
    constexpr int scratchSourceId = 1'000'000;

    Redaction::Options options;
    if (m_backend && m_backend->addDocument(scratchSourceId, input, nullptr)) {
        // The renderer is right here, so a picture the core cannot decode costs
        // the page its selectable text rather than its appearance.
        options.pageRasteriser = [this](int page, double dpi) {
            const QSizeF size = m_backend->pageSizePoints(scratchSourceId, page);
            return m_backend->renderPage(scratchSourceId, page, qRound(size.width() * dpi / 72.0));
        };
    }

    const bool done = Redaction::apply(input, output, areas, options, report, error);
    m_backend->removeDocument(scratchSourceId);
    if (!done) {
        return false;
    }

    return adoptResult(rows, output, i18ncp("@action:inmenu undo step", "Redact page", "Redact %1 pages", rows.size()),
                       error);
}

bool PageProcessor::arrangeOnSheets(const QVector<int> &rows, const NUp::Options &options, int *sheetCount,
                                    QString *error)
{
    if (rows.size() < 2) {
        if (error) {
            *error = i18n("Arranging needs at least two pages.");
        }
        return false;
    }

    const QString input = extractRows(rows, error);
    if (input.isEmpty()) {
        return false;
    }
    const QString output = m_scratch.filePath(QStringLiteral("sheets-%1.pdf").arg(m_counter));

    if (!NUp::apply(input, output, options, error)) {
        return false;
    }

    const int sourceId = m_document->addSource(output, error);
    if (sourceId < 0) {
        return false;
    }
    Source *source = m_document->source(sourceId);
    if (!source || source->pageCount() == 0) {
        if (error) {
            *error = i18n("The arranged document came back empty.");
        }
        return false;
    }

    QVector<PageRef> sheets;
    sheets.reserve(source->pageCount());
    for (int i = 0; i < source->pageCount(); ++i) {
        sheets.append(PageRef { sourceId, i, 0 });
    }

    QVector<int> ascending = rows;
    std::sort(ascending.begin(), ascending.end());
    const int insertAt = ascending.constFirst();

    // Two commands, one step: undo has to put the pages back, not half of them.
    QUndoStack *stack = m_document->undoStack();
    stack->beginMacro(
        i18ncp("@action:inmenu undo step", "Arrange %1 page on sheets", "Arrange %1 pages on sheets", rows.size()));
    stack->push(new RemovePagesCommand(m_document, ascending));
    stack->push(
        new InsertPagesCommand(m_document, insertAt, sheets, i18nc("@action:inmenu undo step", "Insert sheets")));
    stack->endMacro();

    if (sheetCount) {
        *sheetCount = sheets.size();
    }
    return true;
}

bool PageProcessor::annotate(const QHash<int, QVector<Annotation>> &annotationsByRow, QString *error)
{
    QVector<int> rows = annotationsByRow.keys();
    if (rows.isEmpty()) {
        return false;
    }
    std::sort(rows.begin(), rows.end());

    const QString input = extractRows(rows, error);
    if (input.isEmpty()) {
        return false;
    }
    const QString stripped = m_scratch.filePath(QStringLiteral("uncommented-%1.pdf").arg(m_counter));
    const QString output = m_scratch.filePath(QStringLiteral("commented-%1.pdf").arg(m_counter));

    // Clear first, then write the list the dialog ended up with. Adding on top
    // of what is there would duplicate every comment the user merely looked at.
    int removed = 0;
    if (!Annotations::remove(input, stripped, {}, {}, &removed, error)) {
        return false;
    }

    QVector<Annotation> flat;
    for (int i = 0; i < rows.size(); ++i) {
        for (Annotation annotation : annotationsByRow.value(rows.at(i))) {
            annotation.page = i; // position in the extracted file
            flat.append(annotation);
        }
    }

    if (flat.isEmpty()) {
        // Everything was deleted, which is a perfectly good outcome.
        return adoptResult(
            rows, stripped,
            i18ncp("@action:inmenu undo step", "Clear comments on page", "Clear comments on %1 pages", rows.size()),
            error);
    }

    if (!Annotations::add(stripped, output, flat, error)) {
        return false;
    }

    return adoptResult(
        rows, output, i18ncp("@action:inmenu undo step", "Comment on page", "Comment on %1 pages", rows.size()), error);
}

bool PageProcessor::exportComments(const QString &xfdfPath, QString *error)
{
    QVector<int> rows(m_document->pageCount());
    std::iota(rows.begin(), rows.end(), 0);
    if (rows.isEmpty()) {
        return false;
    }

    // Written out of the document as it stands, not as it was last saved: the
    // comments being exported are the ones on screen.
    const QString input = extractRows(rows, error);
    if (input.isEmpty()) {
        return false;
    }
    return Annotations::exportXfdf(input, xfdfPath, {}, QFileInfo(m_document->filePath()).fileName(), error);
}

bool PageProcessor::importComments(const QString &xfdfPath, int *added, QStringList *warnings, QString *error)
{
    QVector<int> rows(m_document->pageCount());
    std::iota(rows.begin(), rows.end(), 0);
    if (rows.isEmpty()) {
        return false;
    }

    const QString input = extractRows(rows, error);
    if (input.isEmpty()) {
        return false;
    }
    const QString output = m_scratch.filePath(QStringLiteral("imported-%1.pdf").arg(m_counter));

    if (!Annotations::importXfdf(input, xfdfPath, output, added, warnings, error)) {
        return false;
    }
    return adoptResult(rows, output, i18nc("@action:inmenu undo step", "Bring in comments"), error);
}

QString PageProcessor::writeScratchCopy(const QVector<int> &rows, QString *error)
{
    return extractRows(rows, error);
}

bool PageProcessor::fillForm(const QHash<QString, QString> &values, bool flatten, int *filled, QStringList *notes,
                             QString *error)
{
    QVector<int> rows(m_document->pageCount());
    std::iota(rows.begin(), rows.end(), 0);
    if (rows.isEmpty()) {
        return false;
    }

    const QString input = extractRows(rows, error);
    if (input.isEmpty()) {
        return false;
    }
    const QString filledPath = m_scratch.filePath(QStringLiteral("filled-%1.pdf").arg(m_counter));
    const QString flatPath = m_scratch.filePath(QStringLiteral("flatform-%1.pdf").arg(m_counter));

    QString result = input;
    if (!values.isEmpty()) {
        if (!Forms::fill(input, filledPath, values, filled, notes, error)) {
            return false;
        }
        result = filledPath;
    }

    if (flatten) {
        int count = 0;
        if (!Forms::flatten(result, flatPath, &count, error)) {
            return false;
        }
        result = flatPath;
    }

    return adoptResult(rows, result,
                       flatten ? i18nc("@action:inmenu undo step", "Fill in and finalise form")
                               : i18nc("@action:inmenu undo step", "Fill in form"),
                       error);
}

bool PageProcessor::buildForm(const QStringList &removed, const QVector<QPair<QString, QString>> &renamed,
                              const QVector<FormBuilder::Field> &changed, const QVector<FormBuilder::Field> &added,
                              QString *error)
{
    QVector<int> rows(m_document->pageCount());
    std::iota(rows.begin(), rows.end(), 0);
    if (rows.isEmpty()) {
        return false;
    }

    QString result = extractRows(rows, error);
    if (result.isEmpty()) {
        return false;
    }

    // Each engine call reads one file and writes another, so the four stages
    // chain through temporaries. The order is not arbitrary: removals first so
    // a name they free is available again, then renames, then the changes
    // (which name their fields as they are *after* the renames) and additions
    // last, for the same freed-name reason.
    int step = 0;
    const auto next = [&] { return m_scratch.filePath(QStringLiteral("form-%1-%2.pdf").arg(m_counter).arg(++step)); };

    int count = 0;
    if (!removed.isEmpty()) {
        const QString out = next();
        if (!FormBuilder::removeFields(result, out, removed, &count, error)) {
            return false;
        }
        result = out;
    }
    for (const auto &rename : renamed) {
        const QString out = next();
        if (!FormBuilder::renameField(result, out, rename.first, rename.second, error)) {
            return false;
        }
        result = out;
    }
    if (!changed.isEmpty()) {
        const QString out = next();
        if (!FormBuilder::updateFields(result, out, changed, &count, error)) {
            return false;
        }
        result = out;
    }
    if (!added.isEmpty()) {
        const QString out = next();
        if (!FormBuilder::addFields(result, out, added, &count, error)) {
            return false;
        }
        result = out;
    }

    if (step == 0) {
        return true; // nothing was waiting after all
    }
    return adoptResult(rows, result, i18nc("@action:inmenu undo step", "Change the form"), error);
}

bool PageProcessor::correctText(const QVector<TextEdit::Replacement> &replacements, const TextEdit::Options &options,
                                TextEdit::Report *report, QString *error)
{
    QVector<int> rows;
    for (const TextEdit::Replacement &replacement : replacements) {
        if (!rows.contains(replacement.page)) {
            rows.append(replacement.page);
        }
    }
    if (rows.isEmpty()) {
        return false;
    }
    std::sort(rows.begin(), rows.end());

    const QString input = extractRows(rows, error);
    if (input.isEmpty()) {
        return false;
    }
    const QString output = m_scratch.filePath(QStringLiteral("corrected-%1.pdf").arg(m_counter));

    // Renumber onto the extracted file, where the rows sit in order.
    QVector<TextEdit::Replacement> renumbered;
    renumbered.reserve(replacements.size());
    for (const TextEdit::Replacement &replacement : replacements) {
        TextEdit::Replacement moved = replacement;
        moved.page = int(rows.indexOf(replacement.page));
        renumbered.append(moved);
    }

    if (!TextEdit::apply(input, output, renumbered, options, report, error)) {
        return false;
    }

    return adoptResult(
        rows, output,
        i18ncp("@action:inmenu undo step", "Correct text on page", "Correct text on %1 pages", rows.size()), error);
}

bool PageProcessor::composeObjects(const QVector<PageEdit> &edits, const QVector<NewContent> &additions,
                                   PageComposer::Report *report, QString *error)
{
    QVector<int> rows;
    for (const PageEdit &edit : edits) {
        if (!rows.contains(edit.page)) {
            rows.append(edit.page);
        }
    }
    for (const NewContent &addition : additions) {
        if (!rows.contains(addition.page)) {
            rows.append(addition.page);
        }
    }
    if (rows.isEmpty()) {
        return false;
    }
    std::sort(rows.begin(), rows.end());

    const QString input = extractRows(rows, error);
    if (input.isEmpty()) {
        return false;
    }
    const QString output = m_scratch.filePath(QStringLiteral("composed-%1.pdf").arg(m_counter));

    // Renumbered onto the extracted file, where the rows sit in order: the
    // same dance correctText does, and for the same reason: an edit names its
    // page by row in the document, and the file handed to the engine holds
    // only the rows being touched.
    QVector<PageEdit> renumbered;
    renumbered.reserve(edits.size());
    for (const PageEdit &edit : edits) {
        PageEdit moved = edit;
        moved.page = int(rows.indexOf(edit.page));
        renumbered.append(moved);
    }

    QVector<NewContent> movedAdditions;
    movedAdditions.reserve(additions.size());
    for (const NewContent &addition : additions) {
        NewContent moved = addition;
        moved.page = int(rows.indexOf(addition.page));
        movedAdditions.append(moved);
    }

    if (!PageComposer::apply(input, output, renumbered, movedAdditions, report, error)) {
        return false;
    }

    return adoptResult(
        rows, output,
        i18ncp("@action:inmenu undo step", "Change what is on the page", "Change what is on %1 pages", rows.size()),
        error);
}

bool PageProcessor::writeArchival(const QString &path, const Archival::Options &options, Archival::Report *report,
                                  QString *error)
{
    QVector<int> rows(m_document->pageCount());
    std::iota(rows.begin(), rows.end(), 0);
    if (rows.isEmpty()) {
        return false;
    }

    // From the document as it stands, so that pages reordered but not yet saved
    // are what gets archived.
    const QString input = extractRows(rows, error);
    if (input.isEmpty()) {
        return false;
    }
    return Archival::convert(input, path, options, report, error);
}

} // namespace ps
