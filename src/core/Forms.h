/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#pragma once

#include <QColor>
#include <QHash>
#include <QRectF>
#include <QString>
#include <QStringList>
#include <QVector>

namespace ps {

/**
 * One fillable field of a form, with the styling and the rules it carries.
 *
 * Everything below the geometry is read straight out of the field or its widget
 * and reported as the document states it. Where the document states nothing, the
 * member says nothing: an invalid QColor, a maximum length of -1, an empty
 * script. That distinction matters, because a form that sets its background to
 * black and a form that sets no background at all are different documents, and a
 * view that cannot tell them apart draws the second one wrong.
 */
struct FormField {
    enum class Kind {
        Text,
        Checkbox,
        Radio,
        Choice, //!< A drop-down or a list
        Button, //!< A push button; nothing to fill in
        Signature,
        Unknown,
    };

    /** How the border is drawn, from /BS /S. */
    enum class BorderStyle {
        Solid, //!< /S, and what a field with no /BS gets
        Dashed, //!< /D
        Beveled, //!< /B, a raised edge
        Inset, //!< /I, a sunken one
        Underline, //!< /U, a rule along the bottom only
    };

    /** The fully qualified name, which is what filling addresses. */
    QString name;

    /** What the form's author wrote next to it, when they wrote anything. */
    QString label;

    Kind kind = Kind::Unknown;

    QString value;

    /** For a drop-down, a list or a radio group: what may be chosen. */
    QStringList options;

    bool readOnly = false;
    bool required = false;
    bool multiline = false;

    /**
     * Zero-based page the first of the field's widgets sits on, or -1.
     *
     * The first of them, not the only one: see @ref widgets.
     */
    int page = -1;

    /** The first widget's box, in display points, origin at the bottom left. */
    QRectF rect;

    /** /MaxLen: how many characters a text field takes, or -1 for no limit. */
    int maxLength = -1;

    /**
     * /Ff bit 25: one character per cell, the boxes on a paper form.
     *
     * Only ever true when the field is also a plain text field with a /MaxLen,
     * because the specification says the flag means nothing without one and a
     * view that drew cells anyway would have no idea how many to draw.
     */
    bool comb = false;

    /** /MK /BG, the fill behind the field. Invalid when the document sets none. */
    QColor background;

    /** /MK /BC, the colour of the border. Invalid when the document sets none. */
    QColor border;

    /** /MK /CA, the caption written on a push button. */
    QString caption;

    /** /BS /W in points; 0 means the field asks for no border at all. */
    double borderWidth = 1.0;

    BorderStyle borderStyle = BorderStyle::Solid;

    /**
     * The font /DA names, without its slash, to be looked up in /AcroForm /DR.
     *
     * Empty when neither the field nor the form states a /DA, which is a broken
     * form rather than a styled one.
     */
    QString fontResource;

    /**
     * The size /DA asks for, in points.
     *
     * **0 means "fit the text to the box"** and is kept as 0 on purpose: it is
     * the legal, widely honoured way of leaving the size to whoever draws the
     * field, and replacing it with a guess here would hide that instruction from
     * the one piece of code able to measure the box.
     */
    double fontSize = 0.0;

    /** The text colour from /DA. Invalid when /DA states no colour operator. */
    QColor textColour;

    /** /Q: 0 left, 1 centred, 2 right. */
    int alignment = 0;

    /**
     * The JavaScript of /AA /F, /K, /V and /C, exactly as the document wrote it.
     *
     * Reported, never run. These are the scripts that reformat a number as
     * currency, refuse a letter in a date, check a value on the way out and add
     * a column of figures up, and knowing that a field has one is what lets a
     * view say so. Interpreting them is a different job with a different risk;
     * pretending to have done it would be worse than not doing it.
     */
    QString formatScript;
    QString keystrokeScript;
    QString validateScript;
    QString calculateScript;

    /**
     * Whether the first widget carries an appearance stream at all, from /AP /N.
     *
     * A field without one is invisible in about half the readers in use, so a
     * view has to know whether it is drawing on top of an appearance or standing
     * in for one that was never made.
     */
    bool hasAppearance = false;

    /**
     * One place on a page where the field is drawn and can be clicked.
     *
     * A field and a box on the page are the same thing only for a text field. A
     * group of radio buttons is *one* field, with one name and one value, and
     * several widgets, one per choice, each with its own rectangle and its own
     * name for "chosen". A field whose widgets sit on several pages, a reference
     * number repeated at the head of every sheet, is legal and not rare either.
     */
    struct Widget {
        /** In display points, origin at the bottom left. Empty when @ref page is -1. */
        QRectF rect;

        /** Zero-based page, or -1 when no page's /Annots lists this widget. */
        int page = -1;

        /**
         * What choosing this one stores in the field's value, e.g. "/Ja".
         *
         * Empty for anything that is not a tick box or a radio button. For a
         * radio group it is the one thing that is this button's alone: every
         * member shares the field's name and the field's value, and it is the
         * state name that says which of them the value refers to.
         */
        QString onState;

        bool hasAppearance = false;
    };

    /**
     * Every widget of the field, in the order the document lists them.
     *
     * Empty only for a field the document draws nowhere at all. @ref page,
     * @ref rect and @ref hasAppearance repeat the first widget that a page
     * actually lists, for the callers that want one box and no more; anything
     * that draws the form or answers a click has to work from this, because a
     * radio group drawn as one box selects all of its choices at once.
     */
    QVector<Widget> widgets;
};

/**
 * Filling in forms, and making the answers permanent.
 *
 * The part of PDF that most often sends people back to a website: an official
 * form arrives, and the only tool that will fill it in wants the document
 * uploaded. Everything here happens on the machine it is running on.
 *
 * Filling sets the field's value and asks for its appearance to be regenerated,
 * because a value with no appearance shows up blank in about half the readers in
 * use, including, awkwardly, the ones that print.
 */
class Forms
{
public:
    /** True when the document has an interactive form at all. */
    static bool hasForm(const QString &pdf);

    /** Every field, in the order the form declares them. */
    static QVector<FormField> read(const QString &pdf, QString *error);

    /**
     * Sets the fields named in @p values, leaving the rest alone.
     *
     * Names that are not in the document are reported through @p unknown rather
     * than ignored: a typo in a field name would otherwise look like a form
     * that filled in silently and wrongly.
     *
     * A checkbox takes "on"/"off", "yes"/"no", "true"/"false" or "1"/"0".
     */
    static bool fill(const QString &inputPdf, const QString &outputPdf, const QHash<QString, QString> &values,
                     int *filled, QStringList *unknown, QString *error);

    /**
     * Draws the fields as they stand and removes the interactivity.
     *
     * What a form looks like when its values have no appearance streams is up to
     * the reader; flattening ends that, which is why it is what to do before
     * sending a filled form back. It cannot be undone in the finished file.
     */
    static bool flatten(const QString &inputPdf, const QString &outputPdf, int *flattened, QString *error);

    /** A one-line description of a field's kind, for lists and reports. */
    static QString describe(FormField::Kind kind);
};

} // namespace ps
