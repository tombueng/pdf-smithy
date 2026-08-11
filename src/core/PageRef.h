/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#pragma once

#include <QtGlobal>

namespace ps {

/**
 * One page of the document under construction.
 *
 * A PageRef never owns pixels or PDF objects; it only points at a page inside
 * one of the Document's loaded Sources and records the edits the user layered
 * on top. That indirection is what makes reordering a 2000-page file instant
 * and undo trivial: nothing is destroyed until the document is written out.
 */
struct PageRef {
    /** Index into Document::sources(). */
    int sourceId = -1;

    /** Zero-based page index inside that source. */
    int sourcePage = 0;

    /**
     * Clockwise rotation the user added, in degrees: 0, 90, 180 or 270.
     * This is a delta on top of whatever /Rotate the source page already
     * carries, so a page that is upside-down in the original stays that way
     * until the user says otherwise.
     */
    int rotation = 0;

    bool operator==(const PageRef &other) const = default;
};

/** Normalises any angle onto the 0/90/180/270 grid, including negatives. */
inline int normalizeRotation(int degrees)
{
    int r = degrees % 360;
    if (r < 0) {
        r += 360;
    }
    return (r / 90) * 90;
}

} // namespace ps
