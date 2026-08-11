/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later

    This file, and only this file plus its implementation, links against
    Poppler, which is GPL. Everything in src/core/ is MIT and stays that way.
    See LICENSING.md for why the split exists and what it buys.
*/
#pragma once

#include "core/RenderBackend.h"

#include <QHash>
#include <QMutex>
#include <memory>

namespace Poppler {
class Document;
}

namespace ps {

/**
 * RenderBackend on top of Poppler.
 *
 * ## Why one file has several handles
 *
 * A `Poppler::Document` is not thread-safe, so the obvious arrangement (one
 * handle per file behind a mutex) makes the pages of a document render one
 * after another. Measured on a 180-page magazine at reading size that is about
 * 150 ms a page: eight pages take 1.2 seconds serially and 0.34 seconds when
 * eight handles run at once on the same file. The rasteriser is not the
 * bottleneck; being allowed to use only one core is.
 *
 * So every open file keeps a small pool of handles instead. A render leases one
 * for the length of the call and puts it back afterwards, and the pool grows
 * lazily up to a ceiling near the core count: a file nobody is looking hard at
 * costs one handle, and a file being scrolled through costs as many as there
 * are cores to run them. Opening an extra handle is cheap (Poppler reads the
 * cross-reference table and little else, about three milliseconds on that
 * magazine), but it is not free in memory, which is why they are shared and
 * pooled rather than made per render or per thread.
 */
class PopplerBackend : public RenderBackend
{
public:
    PopplerBackend();
    ~PopplerBackend() override;

    bool addDocument(int sourceId, const QString &path, QString *error) override;
    void removeDocument(int sourceId) override;
    QImage renderPage(int sourceId, int page, int widthPx) override;
    QImage render(int sourceId, int page, const Request &request) override;
    QSizeF pageSizePoints(int sourceId, int page) override;
    QString extractText(int sourceId, int page) override;
    QStringList wordsInside(int sourceId, int page, const QRectF &rectInPoints) override;
    QVector<Word> words(int sourceId, int page) override;

    /** How many handles one file may hold at once. Sized to the machine. */
    static int handlesPerDocument();

private:
    struct Entry;

    /**
     * One handle taken out of a file's pool, and given back when it goes away.
     *
     * Every call into Poppler goes through one of these, so "which thread may
     * touch this document" has exactly one answer (the one holding it), and no
     * caller has to remember to release anything on the paths that fail.
     */
    class Lease
    {
    public:
        Lease() = default;
        Lease(std::shared_ptr<Entry> entry, std::unique_ptr<Poppler::Document> document);
        Lease(Lease &&other) noexcept;
        Lease(const Lease &) = delete;
        Lease &operator=(const Lease &) = delete;
        Lease &operator=(Lease &&) = delete;
        ~Lease();

        Poppler::Document *operator->() const { return m_document.get(); }

        Poppler::Document *get() const { return m_document.get(); }

        explicit operator bool() const { return m_document != nullptr; }

    private:
        std::shared_ptr<Entry> m_entry;
        std::unique_ptr<Poppler::Document> m_document;
    };

    /**
     * A handle on @p sourceId, waiting for a free one if the pool is at its
     * ceiling. False-y when there is no such document.
     */
    Lease lease(int sourceId);

    /**
     * Looked up under m_registryMutex. Returns a shared_ptr rather than a raw
     * pointer so that a document closed on the GUI thread cannot be destroyed
     * out from under a render still running on a worker.
     */
    std::shared_ptr<Entry> entryFor(int sourceId);

    QMutex m_registryMutex;
    QHash<int, std::shared_ptr<Entry>> m_documents;
};

} // namespace ps
