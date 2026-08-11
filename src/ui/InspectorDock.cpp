/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "InspectorDock.h"

#include <KLocalizedString>

#include <QApplication>
#include <QFont>
#include <QLabel>
#include <QScrollArea>
#include <QVBoxLayout>

#include <utility>

namespace ps {

InspectorDock::InspectorDock(QWidget *parent)
    : QDockWidget(i18nc("@title:window", "Properties"), parent)
    , m_heading(new QLabel(this))
    , m_hint(new QLabel(this))
    , m_scroll(new QScrollArea(this))
{
    setObjectName(QStringLiteral("inspector_dock"));
    setAllowedAreas(Qt::AllDockWidgetAreas);
    setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable | QDockWidget::DockWidgetClosable);

    QFont headingFont = m_heading->font();
    headingFont.setBold(true);
    m_heading->setFont(headingFont);
    m_heading->setWordWrap(true);
    // A field's name is what its author has to type into a script elsewhere, so
    // the heading is worth copying out of.
    m_heading->setTextInteractionFlags(Qt::TextSelectableByMouse);

    m_hint->setWordWrap(true);
    // Stays at the top of whatever room the layout gives it, rather than
    // drifting into the middle of an otherwise empty panel.
    m_hint->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    m_hint->setForegroundRole(QPalette::PlaceholderText);
    m_hint->setText(i18n("Nothing is selected. Click a word, a form field, a comment or a picture in the document, "
                         "and its properties appear here."));

    // The editor decides its own height and the panel scrolls it; an editor that
    // scrolled internally would put a second scrollbar inside this one.
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);

    auto *body = new QWidget(this);
    auto *layout = new QVBoxLayout(body);
    layout->addWidget(m_heading);
    layout->addWidget(m_hint);
    layout->addWidget(m_scroll, 1);
    setWidget(body);

    build(nullptr);
}

// ── Which overlays it asks ────────────────────────────────────────────────

void InspectorDock::addSource(InspectionSource *source, QObject *owner)
{
    if (!source) {
        return;
    }
    for (const Entry &entry : std::as_const(m_sources)) {
        if (entry.source == source) {
            return;
        }
    }

    // Recorded as having answered nothing so far, so that an overlay registered
    // with something already selected counts as a change and is shown at once.
    m_sources.append(Entry { source, nullptr });

    if (owner) {
        connect(owner, &QObject::destroyed, this, [this, source] { removeSource(source); });
    }
    refresh();
}

void InspectorDock::removeSource(InspectionSource *source)
{
    for (int i = 0; i < m_sources.size(); ++i) {
        if (m_sources[i].source != source) {
            continue;
        }
        const bool wasShowing = m_shown && m_sources[i].seen == m_shown;
        m_sources.removeAt(i);
        if (m_announced == source) {
            m_announced = nullptr;
        }
        if (wasShowing) {
            // Its answer goes with it, so the editor built from that answer has
            // to go before anything can ask the remaining sources.
            build(nullptr);
        }
        refresh();
        return;
    }
}

// ── What it shows ─────────────────────────────────────────────────────────

void InspectorDock::refreshFrom(InspectionSource *source)
{
    m_announced = source;
    refresh();
}

void InspectorDock::refresh()
{
    InspectionSource *const announced = std::exchange(m_announced, nullptr);

    // A source answering differently from last time is the one the user has just
    // been working in, so it moves ahead of the ones that have not moved. Doing
    // it by comparison rather than by signal keeps the panel independent of what
    // the overlays choose to emit.
    QVector<Entry> reordered;
    reordered.reserve(m_sources.size());
    int front = 0;
    for (const Entry &entry : std::as_const(m_sources)) {
        const Entry updated { entry.source, entry.source->inspected() };
        if (updated.seen != entry.seen) {
            reordered.insert(front++, updated);
        } else {
            reordered.append(updated);
        }
    }
    m_sources = reordered;

    // A source the window named outranks that guess: it knows which overlay took
    // the click even when the answer happens to be the same object as before.
    if (announced) {
        for (int i = 0; i < m_sources.size(); ++i) {
            if (m_sources[i].source == announced) {
                m_sources.move(i, 0);
                break;
            }
        }
    }

    if (m_stopped) {
        return;
    }

    Inspectable *winner = nullptr;
    for (const Entry &entry : std::as_const(m_sources)) {
        if (entry.seen) {
            winner = entry.seen;
            break;
        }
    }

    // An editor writes its changes through as they are made, so typing into one
    // can come straight back here as a refresh. Rebuilding then would take the
    // widget out from under the user mid-word; every other refresh rebuilds,
    // because a source is free to answer with the same object each time and only
    // its contents to have moved on.
    if (winner && winner == m_shown && m_scroll->widget()
        && m_scroll->widget()->isAncestorOf(QApplication::focusWidget())) {
        return;
    }
    build(winner);
}

void InspectorDock::stop()
{
    // The flag rather than a sweep of disconnects: the connections are to each
    // source's own destruction, and the panel is about to be destroyed itself.
    // What matters is that nothing rebuilds between here and then.
    m_stopped = true;
    m_sources.clear();
    m_shown = nullptr;
    if (QWidget *previous = m_scroll->takeWidget()) {
        previous->deleteLater();
    }
}

void InspectorDock::build(Inspectable *thing)
{
    // Building an editor can make it emit, and what it emits can land back here
    // before the first build has finished. Letting the outer one win leaves the
    // panel with a complete editor rather than half of two.
    if (m_building) {
        return;
    }
    m_building = true;

    // The editor may be what asked for this rebuild, and one of its widgets may
    // still be on the stack, so it is retired rather than deleted outright.
    if (QWidget *previous = m_scroll->takeWidget()) {
        previous->deleteLater();
    }
    m_shown = thing;

    if (!thing) {
        m_heading->hide();
        m_scroll->hide();
        m_hint->show();
        m_building = false;
        return;
    }

    const QString kind = thing->kindName();
    const QString which = thing->description();
    m_heading->setText(which.isEmpty() ? kind
                                       : i18nc("@info:status the kind of thing selected, then which one of them: "
                                               "\"Form field: Address.Street\"",
                                               "%1: %2", kind, which));
    m_heading->show();
    m_hint->hide();

    QWidget *editor = thing->buildEditor(m_scroll);
    if (editor) {
        m_scroll->setWidget(editor);
        // The scroll area is already visible by the time a second thing is
        // selected, and a widget added to a visible one has to be shown by hand.
        editor->show();
        m_scroll->show();
    } else {
        // A thing with a name but no attributes worth editing: the heading alone
        // still tells the user what they have hold of.
        m_scroll->hide();
    }

    m_building = false;
}

} // namespace ps
