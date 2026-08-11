/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#pragma once

#include <QColor>
#include <QRectF>
#include <QString>
#include <QStringList>
#include <QVector>

namespace ps {

/**
 * Making forms, rather than filling in someone else's.
 *
 * The other half of ps::Forms, and the half that is normally locked behind a
 * subscription: taking a letter, a contract or a club membership sheet and
 * putting real fields on it, so that whoever receives it can type instead of
 * print, sign and scan.
 *
 * ## The one thing that goes wrong
 *
 * A field with no appearance stream is invisible. Not subtly wrong, but invisible,
 * in about half the readers in use and in most of the ones that print, because
 * the specification lets a reader draw a widget from its own idea of what a
 * field looks like and a good number of readers decline. Every widget built here
 * therefore gets an `/AP` `/N` form XObject of its own with its border, its
 * background and, for a tick box, its tick; and `/NeedAppearances` is left true
 * afterwards so that a reader which does draw its own gets invited to.
 *
 * The frame is deliberately drawn *outside* the `/Tx BMC` … `EMC` pair that
 * holds the value, because that pair is the one part of the stream a reader
 * replaces when the value changes. A border drawn inside it disappears the first
 * time anybody types.
 *
 * ## Names
 *
 * @ref Field::name is the fully qualified name: the name filling addresses. A
 * full stop in it means a group: "Address.Street" and "Address.Town" become two
 * fields under one parent, which is what lets a reader's "clear this section"
 * work and what a spreadsheet column should be called.
 *
 * ## Where it refuses
 *
 * XFA. A document with an `/XFA` entry carries a second, incompatible form
 * description written in XML, and readers disagree about which of the two wins:
 * Acrobat honours the XFA and ignores the fields, everything else does the
 * opposite. Adding an AcroForm field to such a document produces a file that
 * behaves differently for every reader, so this refuses instead and says why.
 */
class FormBuilder
{
public:
    struct Field {
        enum class Kind { Text, Checkbox, Radio, Dropdown, ListBox, PushButton, Signature };
        Kind kind = Kind::Text;
        QString name; //!< the fully qualified name filling will address
        QString label; //!< /TU, what a person sees next to it
        int page = 0;
        QRectF rect; //!< display points, origin bottom-left

        bool required = false;
        bool readOnly = false;
        bool multiline = false;
        bool password = false;
        bool comb = false;
        int maxLength = 0; //!< 0 for no limit
        bool multiSelect = false; //!< ListBox
        bool editable = false; //!< Dropdown that also accepts typing

        QStringList options; //!< what may be chosen
        QStringList exportValues; //!< what gets stored for each option
        QString defaultValue;

        QString fontName = QStringLiteral("Helv");
        double fontSize = 0.0; //!< 0 means auto-fit
        QColor textColour = Qt::black;
        QColor backgroundColour; //!< invalid for none
        QColor borderColour;
        double borderWidth = 1.0;
        QString borderStyle = QStringLiteral("solid"); //!< solid, dashed, beveled, inset, underline
        Qt::Alignment alignment = Qt::AlignLeft;

        /** For a radio button: which group it belongs to. */
        QString radioGroup;
        /** The on-state name for a checkbox or radio, e.g. "/Yes". */
        QString onState = QStringLiteral("/Yes");
    };

    /**
     * Adds fields to a document, creating /AcroForm when it has none.
     *
     * A radio group is several entries sharing one @ref Field::radioGroup, each
     * with its own @ref Field::rect and its own @ref Field::onState: one PDF
     * field with one value and several widgets, which is the only arrangement in
     * which the buttons take it in turns rather than toggling as one.
     */
    static bool addFields(const QString &in, const QString &out, const QVector<Field> &fields, int *added,
                          QString *error);

    /**
     * Changes the properties of fields that are already there, by name.
     *
     * Every entry is taken as a complete description, so anything left at its
     * default is applied as that default; read a field with ps::Forms first if
     * the intention is to change one property and keep the rest. Names that are
     * not in the document are simply not counted, which is what makes @p updated
     * worth comparing against the number asked for.
     */
    static bool updateFields(const QString &in, const QString &out, const QVector<Field> &fields, int *updated,
                             QString *error);

    static bool removeFields(const QString &in, const QString &out, const QStringList &names, int *removed,
                             QString *error);
    static bool renameField(const QString &in, const QString &out, const QString &from, const QString &to,
                            QString *error);

    /** The order the tab key walks the fields of a page. */
    static bool setTabOrder(const QString &in, const QString &out, int page, const QStringList &names, QString *error);

    enum class Format { None, Number, Currency, Percent, Date, Time, ZipCode, Phone };
    /**
     * Attaches format, validation or calculation behaviour.
     *
     * These are JavaScript actions, which is how every PDF form does it, and which
     * therefore comes with two things worth saying out loud: PDF/A forbids scripts,
     * and a good number of readers do not run them. The report says both.
     */
    static bool setFormat(const QString &in, const QString &out, const QString &fieldName, Format format, int decimals,
                          const QString &currencySymbol, QStringList *warnings, QString *error);
    static bool setValidation(const QString &in, const QString &out, const QString &fieldName, double minimum,
                              double maximum, QStringList *warnings, QString *error);
    enum class Calculation { Sum, Product, Average, Minimum, Maximum };
    static bool setCalculation(const QString &in, const QString &out, const QString &fieldName, Calculation how,
                               const QStringList &sourceFields, QStringList *warnings, QString *error);

    enum class ButtonAction { ResetForm, SubmitForm, GoToPage, OpenUrl };
    static bool setButtonAction(const QString &in, const QString &out, const QString &fieldName, ButtonAction action,
                                const QString &target, QString *error);

    /** Copies an empty form's fields onto another document of the same layout. */
    static bool copyFieldsFrom(const QString &templatePdf, const QString &in, const QString &out, int *copied,
                               QString *error);

    /** Field data in and out, for filling many copies from a spreadsheet. */
    static bool exportData(const QString &pdf, const QString &toFile, QString *error); //!< by extension: .fdf, .xfdf, .csv
    static bool importData(const QString &in, const QString &out, const QString &fromFile, int *filled, QString *error);
    /** One row per document: collects many filled copies into a table. */
    static bool collect(const QStringList &pdfs, const QString &toCsv, QString *error);

    /** Plain-language notes about what this cannot promise. */
    static QStringList limitations();
};

} // namespace ps
