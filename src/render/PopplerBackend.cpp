/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "PopplerBackend.h"

#include <QMutexLocker>
#include <QThread>
#include <QWaitCondition>

#include <KLocalizedString>

#include <poppler-annotation.h>
#include <poppler-qt6.h>

#include <algorithm>

namespace ps {

namespace {

/**
 * How the pages are drawn, on every handle of every file.
 *
 * Thin hairlines and small type in scanned documents are unreadable at
 * thumbnail size without antialiasing. Hinting stays off so that glyph
 * positions match the printed result rather than snapping to the pixel grid.
 *
 * The backend is left at Splash deliberately. Poppler's QPainter backend was
 * measured against it and is not faster (within a tenth either way on files
 * where it works at all), and on a real magazine it fails to rasterise a glyph
 * and takes the process down with it. Splash is the engine Poppler tests.
 */
void configure(Poppler::Document *document, bool draft)
{
    document->setRenderHint(Poppler::Document::Antialiasing, !draft);
    document->setRenderHint(Poppler::Document::TextAntialiasing, !draft);
    document->setRenderHint(Poppler::Document::TextHinting, false);
}

/**
 * The page's form fields, taken off the sheet for as long as this stands.
 *
 * Poppler offers `Document::HideAnnotations`, and it is the wrong tool: it hides
 * *every* annotation, so a form laid out over a marked-up contract would render
 * with the markup gone. What is wanted is narrower (the widgets only), and the
 * way to say it is per annotation. Setting the hidden flag writes through to the
 * document's own object, which is what makes the rasteriser skip it; measured on
 * a page of ct.26.16-form.pdf carrying three fields and five comments, hiding
 * the widgets changes 1851 pixels and the render hint changes 29548.
 *
 * Writing through is also why this puts the flags back. A handle is leased for
 * one render and then returned to the pool, so a flag left set would take the
 * fields off every later render made through that same handle, including the
 * ones the reader asked for.
 */
class WidgetsHidden
{
public:
    explicit WidgetsHidden(Poppler::Page *page)
        : m_widgets(page->annotations({ Poppler::Annotation::AWidget }))
    {
        m_was.reserve(m_widgets.size());
        for (const auto &widget : m_widgets) {
            m_was.push_back(widget->flags());
            widget->setFlags(widget->flags() | Poppler::Annotation::Hidden);
        }
    }

    WidgetsHidden(const WidgetsHidden &) = delete;
    WidgetsHidden &operator=(const WidgetsHidden &) = delete;

    ~WidgetsHidden()
    {
        for (size_t i = 0; i < m_widgets.size() && i < m_was.size(); ++i) {
            m_widgets[i]->setFlags(m_was[i]);
        }
    }

private:
    std::vector<std::unique_ptr<Poppler::Annotation>> m_widgets;
    std::vector<Poppler::Annotation::Flags> m_was;
};

} // namespace

/**
 * One open file: the path it came from, and the handles on it.
 *
 * The mutex guards the free list and the count, never a render: that is the
 * whole point of the pool. A thread that finds the list empty and the count at
 * its ceiling waits on the condition rather than opening a handle nobody asked
 * for, so a hundred queued renders still cost only as many handles as there are
 * cores to run them on.
 */
struct PopplerBackend::Entry {
    QString path;
    QMutex mutex;
    QWaitCondition freed;
    std::vector<std::unique_ptr<Poppler::Document>> idle;
    int open = 0;
    int ceiling = 1;
};

PopplerBackend::Lease::Lease(std::shared_ptr<Entry> entry, std::unique_ptr<Poppler::Document> document)
    : m_entry(std::move(entry))
    , m_document(std::move(document))
{
}

PopplerBackend::Lease::Lease(Lease &&other) noexcept
    : m_entry(std::move(other.m_entry))
    , m_document(std::move(other.m_document))
{
}

PopplerBackend::Lease::~Lease()
{
    if (!m_entry || !m_document) {
        return;
    }
    QMutexLocker locker(&m_entry->mutex);
    m_entry->idle.push_back(std::move(m_document));
    m_entry->freed.wakeOne();
}

int PopplerBackend::handlesPerDocument()
{
    // One per core is what makes the renders overlap; the ceiling is there
    // because each handle carries Poppler's object and font caches for the
    // file, and a machine with sixty-four cores does not want sixty-four
    // copies of a magazine's cross-reference table.
    return std::clamp(QThread::idealThreadCount(), 1, 8);
}

PopplerBackend::PopplerBackend() = default;
PopplerBackend::~PopplerBackend() = default;

bool PopplerBackend::addDocument(int sourceId, const QString &path, QString *error)
{
    std::unique_ptr<Poppler::Document> document = Poppler::Document::load(path);

    if (!document) {
        if (error) {
            *error = i18n("“%1” could not be opened for display.", path);
        }
        return false;
    }
    if (document->isLocked()) {
        if (error) {
            *error = i18n("“%1” is password protected.", path);
        }
        return false;
    }
    configure(document.get(), false);

    auto entry = std::make_shared<Entry>();
    entry->path = path;
    entry->ceiling = handlesPerDocument();
    entry->open = 1;
    entry->idle.push_back(std::move(document));

    QMutexLocker locker(&m_registryMutex);
    m_documents.insert(sourceId, entry);
    return true;
}

void PopplerBackend::removeDocument(int sourceId)
{
    QMutexLocker locker(&m_registryMutex);
    m_documents.remove(sourceId);
}

std::shared_ptr<PopplerBackend::Entry> PopplerBackend::entryFor(int sourceId)
{
    QMutexLocker locker(&m_registryMutex);
    return m_documents.value(sourceId);
}

PopplerBackend::Lease PopplerBackend::lease(int sourceId)
{
    const auto entry = entryFor(sourceId);
    if (!entry) {
        return {};
    }

    // One attempt at opening a fresh handle per lease. A file that has just
    // refused to open will refuse again, and retrying it round the loop would
    // turn a deleted file into a spin.
    bool mayOpen = true;

    QMutexLocker locker(&entry->mutex);
    for (;;) {
        if (!entry->idle.empty()) {
            std::unique_ptr<Poppler::Document> handle = std::move(entry->idle.back());
            entry->idle.pop_back();
            return { entry, std::move(handle) };
        }
        if (mayOpen && entry->open < entry->ceiling) {
            // Counted before the file is read and given back on failure, so two
            // threads arriving together open two handles rather than racing
            // past the ceiling or both deciding the other will do it.
            mayOpen = false;
            ++entry->open;
            const QString path = entry->path;
            locker.unlock();

            std::unique_ptr<Poppler::Document> handle = Poppler::Document::load(path);
            if (handle && !handle->isLocked()) {
                configure(handle.get(), false);
                return { entry, std::move(handle) };
            }

            locker.relock();
            --entry->open;
        }
        if (entry->open <= 0) {
            return {}; // nothing is out on loan, so waiting would be for ever
        }
        entry->freed.wait(&entry->mutex);
    }
}

QImage PopplerBackend::renderPage(int sourceId, int page, int widthPx)
{
    Request request;
    request.widthPx = widthPx;
    return render(sourceId, page, request);
}

QImage PopplerBackend::render(int sourceId, int page, const Request &request)
{
    if (request.widthPx <= 0) {
        return {};
    }

    const Lease handle = lease(sourceId);
    if (!handle) {
        return {};
    }

    const std::unique_ptr<Poppler::Page> pdfPage = handle->page(page);
    if (!pdfPage) {
        return {};
    }

    const QSizeF sizePoints = pdfPage->pageSizeF();
    if (sizePoints.width() <= 0.0 || sizePoints.height() <= 0.0) {
        return {};
    }

    // The hints live on the handle rather than on the call, and the handle is
    // ours alone until the lease ends, so a draft cannot blur somebody else's
    // accurate render.
    configure(handle.get(), request.draft);

    // For the length of this render and no longer; see WidgetsHidden.
    const std::unique_ptr<WidgetsHidden> without
        = request.omit == Request::Omit::FormFields ? std::make_unique<WidgetsHidden>(pdfPage.get()) : nullptr;

    // Poppler thinks in DPI, callers think in pixels. 72 points to the inch.
    const double dpi = static_cast<double>(request.widthPx) * 72.0 / sizePoints.width();
    if (request.tile.isEmpty()) {
        return pdfPage->renderToImage(dpi, dpi);
    }

    // Asked of Poppler rather than cut out afterwards: this is the whole saving.
    // The clamp is against the page's own pixel size so that a tile hanging off
    // the bottom of the sheet asks for paper that exists.
    const int height = int(std::lround(sizePoints.height() * dpi / 72.0));
    const QRect tile = request.tile.intersected(QRect(0, 0, request.widthPx, std::max(1, height)));
    if (tile.isEmpty()) {
        return {};
    }
    return pdfPage->renderToImage(dpi, dpi, tile.x(), tile.y(), tile.width(), tile.height());
}

QSizeF PopplerBackend::pageSizePoints(int sourceId, int page)
{
    const Lease handle = lease(sourceId);
    if (!handle) {
        return {};
    }

    const std::unique_ptr<Poppler::Page> pdfPage = handle->page(page);
    return pdfPage ? pdfPage->pageSizeF() : QSizeF();
}

QString PopplerBackend::extractText(int sourceId, int page)
{
    const Lease handle = lease(sourceId);
    if (!handle) {
        return {};
    }

    const std::unique_ptr<Poppler::Page> pdfPage = handle->page(page);
    // A null rectangle means "the whole page" in Poppler's API.
    return pdfPage ? pdfPage->text(QRectF()) : QString();
}

QStringList PopplerBackend::wordsInside(int sourceId, int page, const QRectF &rectInPoints)
{
    const Lease handle = lease(sourceId);
    if (!handle) {
        return {};
    }

    const std::unique_ptr<Poppler::Page> pdfPage = handle->page(page);
    if (!pdfPage) {
        return {};
    }

    // Poppler measures word boxes from the top of the page; callers think in
    // PDF coordinates, which start at the bottom.
    const double height = pdfPage->pageSizeF().height();
    const QRectF wanted(rectInPoints.x(), height - rectInPoints.y() - rectInPoints.height(), rectInPoints.width(),
                        rectInPoints.height());

    QStringList words;
    for (const auto &box : pdfPage->textList()) {
        if (box->boundingBox().intersects(wanted)) {
            words << box->text();
        }
    }
    return words;
}

QVector<RenderBackend::Word> PopplerBackend::words(int sourceId, int page)
{
    const Lease handle = lease(sourceId);
    if (!handle) {
        return {};
    }

    const std::unique_ptr<Poppler::Page> pdfPage = handle->page(page);
    if (!pdfPage) {
        return {};
    }

    // Poppler runs its own reading-order analysis over the content stream, so
    // this comes back in the order a person reads rather than the order the
    // producer happened to draw in, which for a two-column page is the whole
    // difference between a usable selection and nonsense.
    const std::vector<std::unique_ptr<Poppler::TextBox>> boxes = pdfPage->textList();

    const double height = pdfPage->pageSizeF().height();

    QVector<Word> found;
    found.reserve(int(boxes.size()));
    for (const auto &box : boxes) {
        const QRectF top = box->boundingBox();
        Word word;
        word.text = box->text();
        // Poppler measures from the top of the page; everything above this
        // layer thinks in PDF coordinates, which start at the bottom.
        word.rect = QRectF(top.x(), height - top.y() - top.height(), top.width(), top.height());
        word.joinedToNext = !box->hasSpaceAfter();
        found.append(word);
    }
    return found;
}

} // namespace ps
