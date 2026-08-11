/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#pragma once

#include "Inspector.h"

#include <QDockWidget>
#include <QVector>

class QLabel;
class QScrollArea;

namespace ps {

/**
 * The panel beside the document, showing the properties of what is selected.
 *
 * It holds no tools and offers no commands of its own: it names the thing the
 * user has just clicked and lists its attributes, so that the panel can be
 * glanced at rather than read. Everything in it comes from the overlays through
 * Inspectable, which means a new kind of selectable thing needs no change here.
 *
 * Several overlays can have something selected at once (a picture inside a
 * page that also has a text run marked), and the panel shows whichever one the
 * user touched last, because that is the one they are thinking about. The window
 * says which by calling refreshFrom(); a plain refresh() works out the same
 * answer by noticing which source changed its mind.
 */
class InspectorDock : public QDockWidget
{
    Q_OBJECT

public:
    explicit InspectorDock(QWidget *parent = nullptr);

    /**
     * Listens to one more overlay. The source is not owned and must outlive the
     * dock or be withdrawn first; passing @p owner (any object that dies with
     * the source, since overlays are not themselves QObjects) does that
     * withdrawal automatically.
     */
    void addSource(InspectionSource *source, QObject *owner = nullptr);

    /** Stops asking @p source, and clears the panel if it was showing its answer. */
    void removeSource(InspectionSource *source);

    /**
     * Forgets every source and answers nothing more.
     *
     * Has to be called before the window it belongs to starts letting go of
     * things. A source is a QObject and this dock watches it for destruction,
     * so tearing the window down makes each dying overlay ask the panel to
     * rebuild itself, against a document whose own file has already been
     * closed. That is a crash on exit, and it is the same shape of trap as the
     * one the render cache has its own stop() for: a QObject child is destroyed
     * after its parent's members, so "the window is still standing" is not the
     * same as "the window's document is still readable".
     */
    void stop();

public Q_SLOTS:
    /** Asks every source again. Call this whenever a selection changed. */
    void refresh();

    /** As refresh(), but @p source is the one the user was just working in. */
    void refreshFrom(InspectionSource *source);

private:
    /** Replaces the contents with @p thing's editor, or with the hint for null. */
    void build(Inspectable *thing);

    /** A source and the answer it last gave, so that a change can be spotted. */
    struct Entry {
        InspectionSource *source = nullptr;
        Inspectable *seen = nullptr;
    };

    /** Most recently changed first: the front of this is what the panel shows. */
    QVector<Entry> m_sources;
    bool m_stopped = false;

    /** Named by refreshFrom() and consumed by the next refresh(). */
    InspectionSource *m_announced = nullptr;

    /** Only ever compared, never followed: the source that gave it may be gone. */
    Inspectable *m_shown = nullptr;

    /** Set while building, so that an editor cannot restart its own creation. */
    bool m_building = false;

    QLabel *m_heading;
    QLabel *m_hint;
    QScrollArea *m_scroll;
};

} // namespace ps
