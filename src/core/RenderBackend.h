/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#pragma once

#include <QImage>
#include <QRect>
#include <QRectF>
#include <QSizeF>
#include <QString>
#include <QStringList>
#include <QVector>

namespace ps {

/**
 * Everything the core needs from a rasteriser, and nothing more.
 *
 * This interface exists for a licensing reason as much as a technical one.
 * Poppler, the only mature Qt-native PDF renderer packaged by distributions,
 * is GPL, while every other dependency in the stack is permissive. Keeping the
 * renderer behind an interface means the MIT-licensed core never links against
 * it, and the day a permissive engine (pdfium) becomes packageable the swap is
 * one class, not a rewrite.
 *
 * Implementations must be safe to call from worker threads, and are expected to
 * let several pages of one file render at the same time. A rasteriser whose own
 * objects are not thread-safe (Poppler's are not) holds several of them per
 * open file rather than serialising, because a viewer that draws one page at a
 * time on a machine with sixteen cores is the slow viewer people complain
 * about, and page renders are the one workload that parallelises perfectly.
 */
class RenderBackend
{
public:
    virtual ~RenderBackend() = default;

    /**
     * What one render is being asked for, beyond "this page, this wide".
     *
     * Two things belong here that a width alone cannot say. A **tile**, because
     * at 400% zoom a whole A4 sheet is a two-hundred-megapixel image of which
     * the reader sees a twentieth, and rendering the other nineteen twentieths
     * is the single largest waste a page viewer can commit. And a **draft**
     * flag, because the first thing a reader wants is *something* in the right
     * place, and a stand-in that arrives in a tenth of the time is worth more
     * than the accurate one it will be replaced by.
     */
    struct Request {
        /**
         * What the render is to leave off the page, so that a layer above can
         * draw it instead.
         *
         * A form field's appearance is part of the page: it is painted into the
         * bitmap the reader sees, and nothing drawn on top can take it away
         * again. So a layer that lets someone move a field has two choices:
         * cover the old one with a patch of paper that never quite matches, or
         * ask for a page that never had it. This is the second, and it is what
         * makes a field being dragged correct by construction rather than by
         * repair.
         *
         * Only the fields, never the comments. A page can carry both, and a
         * form being laid out on a marked-up contract must not lose the
         * markup.
         */
        enum class Omit {
            Nothing, //!< the page as any reader would show it
            FormFields, //!< the widgets; every other annotation is drawn as usual
        };

        Omit omit = Omit::Nothing;

        /** Device pixels across the whole page, as though it were drawn entire. */
        int widthPx = 0;

        /**
         * The wanted part of that whole-page image, in its pixels, or an empty
         * rectangle for all of it. The returned image is the size of the tile,
         * not of the page.
         */
        QRect tile;

        /**
         * Speed before fidelity: antialiasing may be dropped. Only ever asked
         * for images that something better will replace.
         */
        bool draft = false;
    };

    /**
     * One render, as @p request describes it. Null on failure.
     *
     * The default is the honest, slow answer (draw the whole page and cut the
     * tile out of it), so that a backend which cannot do better still works;
     * one that can override this and save the cut pixels entirely. That default
     * draws the whole page, annotations and all: leaving part of it off is
     * something only the rasteriser can do, so a backend that means to honour
     * @ref Request::omit has to override this.
     */
    virtual QImage render(int sourceId, int page, const Request &request)
    {
        const QImage whole = renderPage(sourceId, page, request.widthPx);
        if (whole.isNull() || request.tile.isEmpty()) {
            return whole;
        }
        return whole.copy(request.tile.intersected(whole.rect()));
    }

    /** Makes a file available for rendering under the given id. */
    virtual bool addDocument(int sourceId, const QString &path, QString *error) = 0;

    virtual void removeDocument(int sourceId) = 0;

    /**
     * Renders a page scaled to @p widthPx pixels wide, preserving aspect ratio.
     * Rotation is deliberately *not* a parameter: callers rotate the result at
     * paint time so that turning a page does not invalidate the thumbnail cache.
     * Returns a null image on failure.
     */
    virtual QImage renderPage(int sourceId, int page, int widthPx) = 0;

    /** Page size in PostScript points (1/72 inch), with /Rotate applied. */
    virtual QSizeF pageSizePoints(int sourceId, int page) = 0;

    /** Extracted text, empty for pages that carry none (e.g. plain scans). */
    virtual QString extractText(int sourceId, int page) = 0;

    /**
     * The words that fall inside @p rectInPoints, in reading order.
     *
     * @p rectInPoints is measured on the page as displayed, origin at the
     * bottom left. Used to tell someone what a redaction is about to destroy
     * before it destroys it; an empty result means the area holds no
     * selectable text, which for a scan is expected rather than reassuring.
     */
    virtual QStringList wordsInside(int sourceId, int page, const QRectF &rectInPoints) = 0;

    /** One word on the page, with the box it occupies. */
    struct Word {
        QString text;

        /** In display points, origin at the bottom left. */
        QRectF rect;

        /**
         * True when no space follows this word before the next one.
         *
         * The renderer knows this and a caller cannot work it out: two words
         * whose boxes touch may be one word split across a kern or two words
         * set tight. Getting it wrong is how copied text comes out with
         * spaces inside words or none between them.
         */
        bool joinedToNext = false;
    };

    /**
     * Every word on the page, in reading order, for selecting and copying.
     *
     * Reading order rather than drawing order, because a selection dragged
     * across a two-column page has to pick up the column the user dragged
     * through and not everything between the two points in the content
     * stream. Empty on a page with no selectable text (a scan without
     * recognition), which is a fact about the document rather than a failure.
     */
    virtual QVector<Word> words(int sourceId, int page) = 0;
};

} // namespace ps
