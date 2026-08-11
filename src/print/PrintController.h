/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#pragma once

#include <QObject>
#include <QString>
#include <QVector>

class QPrinter;
class QPainter;

namespace ps {

class Document;
class RenderBackend;

/**
 * Everything between "I want this on paper" and the printer.
 *
 * Most PDF viewers hand the file to CUPS and call it printing. That works
 * until you want two pages on a sheet, or a booklet, or only the odd sides for
 * manual duplex, which are the things people actually print for. So the imposition
 * happens here, and the printer only ever sees finished sheets.
 */
class PrintController : public QObject
{
    Q_OBJECT

public:
    enum class Layout {
        Normal, //!< one page per sheet
        TwoUp, //!< two pages side by side
        FourUp,
        SixUp,
        NineUp,
        SixteenUp,
        Booklet, //!< folded in the middle and stapled
    };
    Q_ENUM(Layout)

    enum class Scaling {
        FitToPage, //!< shrink or grow to fill the printable area
        ShrinkToFit, //!< only ever shrink; small pages keep their size
        ActualSize, //!< exact physical dimensions, may clip
        Custom,
    };
    Q_ENUM(Scaling)

    enum class Subset {
        All,
        OddOnly, //!< the two halves of manual duplex on a simplex printer
        EvenOnly,
    };
    Q_ENUM(Subset)

    struct Options {
        /** Empty means every page. Uses PageRange syntax. */
        QString range;

        Layout layout = Layout::Normal;
        Scaling scaling = Scaling::ShrinkToFit;

        /** Percent, only consulted for Scaling::Custom. */
        int customScalePercent = 100;

        Subset subset = Subset::All;

        bool reverseOrder = false;
        bool grayscale = false;

        /** Draw a hairline around each imposed page. Useful for cut marks. */
        bool pageBorders = false;

        /** Millimetres of white kept around the sheet's printable area. */
        qreal marginMm = 0.0;
    };

    explicit PrintController(QObject *parent = nullptr);

    void setDocument(Document *document);
    void setRenderBackend(RenderBackend *backend);

    /** How many pages fit on one sheet in the given layout. */
    static int pagesPerSheet(Layout layout);

    static QString layoutName(Layout layout);

    /**
     * The page indexes in the order they will be placed, already imposed:
     * for a booklet this is the folding order, for N-up it is reading order,
     * and -1 marks a deliberately blank slot.
     *
     * Exposed so the preview and the tests can check the ordering without
     * touching a printer.
     */
    QVector<int> impose(const Options &options, QString *error) const;

    /** Sheets the job will consume, before copies and duplex. */
    int sheetCount(const Options &options) const;

    /** Renders the job onto @p printer. */
    bool print(QPrinter *printer, const Options &options, QString *error);

    /** Draws sheet @p sheetIndex with @p painter, shared with the preview. */
    bool paintSheet(QPainter *painter, const QVector<int> &imposed, int sheetIndex, const Options &options,
                    const QRectF &sheetRect, int resolutionDpi);

Q_SIGNALS:
    void progress(int sheet, int sheetCount);

private:
    Document *m_document = nullptr;
    RenderBackend *m_backend = nullptr;
};

} // namespace ps
