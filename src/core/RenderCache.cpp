/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#include "RenderCache.h"

#include "RenderBackend.h"

#include <QMutexLocker>
#include <QRunnable>
#include <QThread>

#include <algorithm>

namespace ps {

/**
 * One page render, handed to the cache's own pool.
 *
 * Holding a bare RenderCache* is safe because ~RenderCache drains the pool
 * before it finishes destructing, so no task can outlive its cache.
 */
class RenderTask : public QRunnable
{
public:
    RenderTask(RenderCache *cache, RenderBackend *backend, int sourceId, int page, int width)
        : m_cache(cache)
        , m_backend(backend)
        , m_sourceId(sourceId)
        , m_page(page)
        , m_width(width)
    {
        setAutoDelete(true);
    }

    void run() override
    {
        // A task already queued when the cache was stopped still runs; it just
        // has nothing left to render into.
        if (!m_backend || !m_cache) {
            return;
        }
        const QImage image = m_backend->renderPage(m_sourceId, m_page, m_width);
        m_cache->storeResult(m_sourceId, m_page, m_width, image);
    }

private:
    RenderCache *m_cache;
    RenderBackend *m_backend;
    int m_sourceId;
    int m_page;
    int m_width;
};

RenderCache::RenderCache(QObject *parent)
    : QObject(parent)
    , m_cache(192 * 1024) // kilobytes
    , m_proxies(32 * 1024)
{
    // Leave a core for the GUI thread so scrolling stays smooth while a large
    // document is being swept, and no more threads than a rasteriser will let
    // work at once: past that they only queue inside it, where nothing can see
    // how long they have been waiting or drop them when the reader moves on.
    m_pool.setMaxThreadCount(std::clamp(QThread::idealThreadCount() - 1, 1, 8));
}

RenderCache::~RenderCache()
{
    stop();
}

void RenderCache::stop()
{
    // Cleared first so that nothing queued starts after the wait begins, and
    // only then waited on: the other order lets a task slip in between.
    m_pool.clear();
    m_pool.waitForDone();

    QMutexLocker locker(&m_mutex);
    m_backend = nullptr;
    m_pending.clear();
}

void RenderCache::setBackend(RenderBackend *backend)
{
    // Drained before the pointer moves, for the same reason stop() exists: a
    // task queued against the old rasteriser holds a raw pointer to it, and
    // whoever is swapping the backend out is usually about to destroy it.
    m_pool.clear();
    m_pool.waitForDone();

    QMutexLocker locker(&m_mutex);
    m_backend = backend;
    m_cache.clear();
    m_proxies.clear();
    m_widths.clear();
    m_pending.clear();
}

void RenderCache::setThumbnailWidth(int pixels)
{
    pixels = std::clamp(pixels, 32, 2048);

    QMutexLocker locker(&m_mutex);
    // Nothing is thrown away. What is held at the old size either serves the
    // new one by being scaled down or waits until it is asked for again, and
    // either is better than rendering a document over because a slider moved.
    m_width = pixels;
}

void RenderCache::setCacheLimitKb(int kilobytes)
{
    QMutexLocker locker(&m_mutex);
    m_cache.setMaxCost(std::max(1024, kilobytes));
}

const QImage *RenderCache::lookup(const Key &key) const
{
    return shelfFor(key.width).object(key);
}

void RenderCache::keep(const Key &key, const QImage &image)
{
    const int costKb = std::max(1, static_cast<int>(image.sizeInBytes() / 1024));
    if (!shelfFor(key.width).insert(key, new QImage(image), costKb)) {
        return; // bigger than the whole shelf; QCache has already dropped it
    }

    QVector<int> &widths = m_widths[pageKey(key.sourceId, key.page)];
    const auto at = std::lower_bound(widths.begin(), widths.end(), key.width);
    if (at == widths.end() || *at != key.width) {
        widths.insert(at, key.width);
    }
}

QImage RenderCache::thumbnail(int sourceId, int sourcePage)
{
    QMutexLocker locker(&m_mutex);
    const int width = m_width;
    locker.unlock();
    return thumbnail(sourceId, sourcePage, width);
}

QImage RenderCache::thumbnail(int sourceId, int sourcePage, int widthPx)
{
    widthPx = std::clamp(widthPx, 32, 4096);
    const Key key { sourceId, sourcePage, widthPx };

    QMutexLocker locker(&m_mutex);
    if (const QImage *hit = lookup(key)) {
        return *hit;
    }

    // What else is held of this page, and can any of it answer instead? The
    // list is only a belief about the caches, so each candidate is probed and
    // the ones that have been evicted are forgotten here.
    QVector<int> &widths = m_widths[pageKey(sourceId, sourcePage)];
    const QImage *narrower = nullptr;
    QImage wider;
    for (int i = 0; i < widths.size();) {
        const QImage *held = lookup({ sourceId, sourcePage, widths.at(i) });
        if (!held) {
            widths.remove(i);
            continue;
        }
        if (widths.at(i) < widthPx) {
            narrower = held; // the widths are ascending, so this keeps the best
        } else if (wider.isNull()) {
            // Scaling a good picture down is instant and looks right, which is
            // the whole reason several widths are allowed to coexist.
            wider = held->scaledToWidth(widthPx, Qt::SmoothTransformation);
        }
        ++i;
    }

    if (!wider.isNull()) {
        keep(key, wider);
        return wider;
    }

    if (m_backend && !m_pending.contains(key)) {
        m_pending.insert(key);
        RenderBackend *backend = m_backend;
        const QImage standIn = narrower ? *narrower : QImage();
        locker.unlock();
        m_pool.start(new RenderTask(this, backend, sourceId, sourcePage, widthPx));
        // Blurred but in the right place, and replaced the moment the proper
        // size lands. An empty cell is what makes a viewer feel slow.
        return standIn;
    }
    return narrower ? *narrower : QImage();
}

bool RenderCache::isCached(int sourceId, int sourcePage) const
{
    QMutexLocker locker(&m_mutex);
    return shelfFor(m_width).contains({ sourceId, sourcePage, m_width });
}

bool RenderCache::isCached(int sourceId, int sourcePage, int widthPx) const
{
    QMutexLocker locker(&m_mutex);
    return shelfFor(widthPx).contains({ sourceId, sourcePage, widthPx });
}

void RenderCache::clear()
{
    QMutexLocker locker(&m_mutex);
    m_cache.clear();
    m_proxies.clear();
    m_widths.clear();
    m_pending.clear();
}

void RenderCache::cancelPending()
{
    m_pool.clear();
    QMutexLocker locker(&m_mutex);
    m_pending.clear();
}

void RenderCache::storeResult(int sourceId, int sourcePage, int width, const QImage &image)
{
    {
        QMutexLocker locker(&m_mutex);
        m_pending.remove({ sourceId, sourcePage, width });

        if (image.isNull()) {
            return;
        }
        // Kept whatever the current width has become since this was asked for:
        // it was wanted once, it costs nothing to hold, and the next zoom back
        // is answered out of it instead of by rendering the page again.
        keep({ sourceId, sourcePage, width }, image);
    }

    // Hop to the cache's own thread before touching signal machinery.
    QMetaObject::invokeMethod(
        this, [this, sourceId, sourcePage] { Q_EMIT thumbnailReady(sourceId, sourcePage); }, Qt::QueuedConnection);
}

} // namespace ps
