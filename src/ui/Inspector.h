/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#pragma once

#include <QObject>
#include <QString>

class QWidget;

namespace ps {

/**
 * What the properties panel shows: the attributes of whatever is selected.
 *
 * The panel is not a tool box and must never become one. A tool box asks the
 * user to remember which of twelve panels holds the setting they want; a
 * properties panel shows the settings of the thing they have just clicked, and
 * is empty when they have clicked nothing. Every attribute of the selected
 * text, comment, form field or picture belongs here, and nothing else does,
 * because the moment something unrelated appears the user has to start reading
 * the panel instead of glancing at it.
 *
 * Editing happens in the document. This panel is for the attributes that have
 * no natural gesture (a field's validation rule, a comment's author, the exact
 * point size), not for the ones that do.
 */
class Inspectable
{
public:
    virtual ~Inspectable() = default;

    /** What kind of thing this is, for the panel's heading: "Form field". */
    virtual QString kindName() const = 0;

    /** Which one, when that can be said: a field's name, a picture's size. */
    virtual QString description() const { return {}; }

    /**
     * The controls for this thing's attributes.
     *
     * Built fresh each time something is selected and destroyed with the
     * panel's previous contents. A selection that stays put must therefore not
     * rely on the widget surviving: write changes through as they are made.
     */
    virtual QWidget *buildEditor(QWidget *parent) = 0;
};

/**
 * Something that can tell the window what is selected in it.
 *
 * Implemented by the overlays. The window listens to whichever ones are
 * active and puts the latest answer in the panel, so an overlay never has to
 * know that the panel exists.
 */
class InspectionSource
{
public:
    virtual ~InspectionSource() = default;

    /** Null when nothing is selected. Stays valid until the next change. */
    virtual Inspectable *inspected() = 0;
};

} // namespace ps
