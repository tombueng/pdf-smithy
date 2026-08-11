/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#pragma once

#include <QCache>
#include <QHash>
#include <QImage>
#include <QMutex>
#include <QObject>
#include <QSet>
#include <QThreadPool>
#include <QVector>

namespace ps {

class RenderBackend;

/**
 * Thumbnails, rendered off the GUI thread and kept in a bounded cache.
 *
 * Three decisions shape this class:
 *
 * 1. Entries are keyed by source, page **and width**, but not by rotation.
 *    Callers rotate the finished image when they paint it, so spinning a page
 *    through all four orientations costs nothing and never re-renders.
 *
 *    The width used to be left out of the key, and that was expensive in a way
 *    nobody could see. Two views share one cache (the page grid and the
 *    thumbnail strip) at quite different sizes, so whichever set the width
 *    last decided what *both* got, and changing it emptied the cache. A saved
 *    configuration with a wide grid had the strip rendering every page a
 *    thousand pixels across to draw it a hundred and eighty wide, and every
 *    change of grid zoom threw all of it away.
 *
 * 2. A render already in hand is used rather than repeated. Asked for a width
 *    it does not have, the cache scales down a wider one it does (which is
 *    instant, looks right, and beats rendering by two orders of magnitude),
 *    and shows a narrower one blurred while the proper size is on its way,
 *    because a soft picture reads as the same page and an empty cell reads as
 *    a broken program.
 *
 * 3. Small renders are kept apart from large ones, each under its own ceiling.
 *    A sweep of a three-hundred-page document at grid size is a gigabyte of
 *    pixels; sharing one ceiling would let it evict every thumbnail the strip
 *    is drawing, and the strip would then render them all again. The small ones
 *    are cheap enough to keep hundreds of.
 *
 * The cache owns its thread pool and drains it in the destructor, which is what
 * makes it safe for worker tasks to hand results back by queued call.
 */
class RenderCache : public QObject
{
    Q_OBJECT

public:
    explicit RenderCache(QObject *parent = nullptr);
    ~RenderCache() override;

    /** Not owned; must outlive the cache. */
    void setBackend(RenderBackend *backend);

    /**
     * Waits for every render in flight and takes no more.
     *
     * Has to be called before whatever owns the backend lets go of it. The
     * destructor drains the pool too, but by then it is already too late in one
     * case that matters: a QObject child is destroyed by its parent *after* the
     * parent's own members, so a cache parented to the window outlives the
     * window's backend by exactly the window of time a task needs to call into
     * it. That is a use-after-free on every exit that happens while a page is
     * still being drawn, and it is why this exists rather than being left to
     * the destructor.
     */
    void stop();

    int thumbnailWidth() const { return m_width; }

    /**
     * The width @ref thumbnail() renders at when no width is given.
     *
     * Changing it no longer empties anything: the old size stays in hand and
     * the new one is scaled out of it where that is possible.
     */
    void setThumbnailWidth(int pixels);

    /** Ceiling in kilobytes for the full-size renders. Defaults to 192 MB. */
    void setCacheLimitKb(int kilobytes);

    /**
     * The thumbnail if it is ready, otherwise a null image and a queued render
     * whose completion is announced by thumbnailReady().
     *
     * "Ready" includes a render of another size that can stand in: the answer
     * is always the best picture of that page the cache can produce without
     * making anybody wait.
     */
    QImage thumbnail(int sourceId, int sourcePage);

    /**
     * The same, at a width of the caller's own choosing.
     *
     * This is what lets two views of different sizes share one cache: a strip
     * drawing at 180 pixels asks for 180 and is not made to pay for the grid's
     * thousand.
     */
    QImage thumbnail(int sourceId, int sourcePage, int widthPx);

    /** True when the image is already in memory, so no render will be started. */
    bool isCached(int sourceId, int sourcePage) const;
    bool isCached(int sourceId, int sourcePage, int widthPx) const;

    void clear();

    /** Drops queued work that has not started yet, e.g. after fast scrolling. */
    void cancelPending();

Q_SIGNALS:
    void thumbnailReady(int sourceId, int sourcePage);

private:
    friend class RenderTask;

    /** One page at one size. Rotation is deliberately absent; see the class note. */
    struct Key {
        int sourceId = 0;
        int page = 0;
        int width = 0;

        bool operator==(const Key &other) const = default;
    };

    friend size_t qHash(const Key &key, size_t seed) noexcept
    {
        return qHashMulti(seed, key.sourceId, key.page, key.width);
    }

    static quint64 pageKey(int sourceId, int sourcePage)
    {
        return (static_cast<quint64>(static_cast<quint32>(sourceId)) << 32) | static_cast<quint32>(sourcePage);
    }

    /** Small renders live in their own cache; see the class note. */
    QCache<Key, QImage> &shelfFor(int width) { return width <= ProxyWidth ? m_proxies : m_cache; }

    const QCache<Key, QImage> &shelfFor(int width) const { return width <= ProxyWidth ? m_proxies : m_cache; }

    /** Called with m_mutex held. Null when that exact size is not in hand. */
    const QImage *lookup(const Key &key) const;

    /** Called with m_mutex held. Puts @p image away and records its width. */
    void keep(const Key &key, const QImage &image);

    /** Called from worker threads. */
    void storeResult(int sourceId, int sourcePage, int width, const QImage &image);

    /** At or below this a render counts as a proxy rather than a thumbnail. */
    static constexpr int ProxyWidth = 320;

    mutable QMutex m_mutex;
    QCache<Key, QImage> m_cache;
    QCache<Key, QImage> m_proxies;

    /**
     * Which widths of a page are believed to be held, smallest first.
     *
     * A guess rather than a record: QCache drops entries without saying so, so
     * every use of this checks the cache and forgets what is no longer there.
     */
    mutable QHash<quint64, QVector<int>> m_widths;

    QSet<Key> m_pending;
    RenderBackend *m_backend = nullptr;
    int m_width = 180;
    QThreadPool m_pool;
};

} // namespace ps
