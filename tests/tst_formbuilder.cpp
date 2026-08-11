/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#include "TestPdf.h"

#include "core/FormBuilder.h"
#include "core/Forms.h"
#include "core/PdfFile.h"

#include <KLocalizedString>

#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>

#include <qpdf/Buffer.hh>
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>
#include <qpdf/QPDFWriter.hh>

using namespace ps;

namespace {

QByteArray streamBytes(QPDFObjectHandle stream)
{
    std::shared_ptr<Buffer> data = stream.getStreamData(qpdf_dl_all);
    return QByteArray(reinterpret_cast<const char *>(data->getBuffer()), qsizetype(data->getSize()));
}

/** The field dictionary a name addresses, found the same way a reader finds it. */
QPDFObjectHandle findField(QPDFObjectHandle container, const QString &prefix, const QString &wanted)
{
    for (int i = 0; container.isArray() && i < container.getArrayNItems(); ++i) {
        QPDFObjectHandle item = container.getArrayItem(i);
        if (!item.isDictionary()) {
            continue;
        }
        const QString partial
            = item.getKey("/T").isString() ? QString::fromStdString(item.getKey("/T").getUTF8Value()) : QString();
        const QString full = partial.isEmpty() ? prefix
            : prefix.isEmpty()                 ? partial
                                               : prefix + QLatin1Char('.') + partial;
        if (full == wanted) {
            return item;
        }
        QPDFObjectHandle kids = item.getKey("/Kids");
        for (int k = 0; kids.isArray() && k < kids.getArrayNItems(); ++k) {
            if (kids.getArrayItem(k).getKey("/T").isString()) {
                QPDFObjectHandle deeper = findField(kids, full, wanted);
                if (deeper.isDictionary()) {
                    return deeper;
                }
                break;
            }
        }
    }
    return QPDFObjectHandle::newNull();
}

/**
 * How many widgets the document has, and how many of those carry a drawing.
 *
 * The pair is the whole point: a widget without an /AP /N stream is a field that
 * is in the document, has a name and a value, and is invisible on the page.
 */
void appearanceTally(const QString &path, int *widgets, int *drawn)
{
    *widgets = 0;
    *drawn = 0;
    QPDF pdf;
    PdfFile::open(pdf, path);
    for (QPDFPageObjectHelper &page : QPDFPageDocumentHelper(pdf).getAllPages()) {
        QPDFObjectHandle annots = page.getObjectHandle().getKey("/Annots");
        for (int a = 0; annots.isArray() && a < annots.getArrayNItems(); ++a) {
            QPDFObjectHandle annot = annots.getArrayItem(a);
            if (!annot.isDictionary() || !annot.getKey("/Subtype").isName()
                || annot.getKey("/Subtype").getName() != "/Widget") {
                continue;
            }
            ++(*widgets);
            QPDFObjectHandle normal = annot.getKey("/AP").isDictionary() ? annot.getKey("/AP").getKey("/N")
                                                                        : QPDFObjectHandle::newNull();
            if (normal.isStream()) {
                ++(*drawn);
                continue;
            }
            if (!normal.isDictionary() || normal.getDictAsMap().empty()) {
                continue;
            }
            bool everyState = true;
            for (const auto &[state, appearance] : normal.getDictAsMap()) {
                Q_UNUSED(state)
                everyState = everyState && appearance.isStream();
            }
            if (everyState) {
                ++(*drawn);
            }
        }
    }
}

/** The states a field's first widget can draw, sorted so the order cannot matter. */
QStringList appearanceStatesOf(const QString &path, const QString &name)
{
    QPDF pdf;
    PdfFile::open(pdf, path);
    QPDFObjectHandle field = findField(pdf.getRoot().getKey("/AcroForm").getKey("/Fields"), QString(), name);
    if (!field.isDictionary()) {
        return {};
    }
    QPDFObjectHandle widget = field;
    if (field.getKey("/Kids").isArray() && field.getKey("/Kids").getArrayNItems() > 0) {
        widget = field.getKey("/Kids").getArrayItem(0);
    }
    QStringList states;
    QPDFObjectHandle normal = widget.getKey("/AP").getKey("/N");
    if (normal.isDictionary()) {
        for (const auto &[state, appearance] : normal.getDictAsMap()) {
            Q_UNUSED(appearance)
            states << QString::fromStdString(state);
        }
    }
    states.sort();
    return states;
}

/** The on-state of every kid of a radio group, and how many kids there are. */
QStringList kidStatesOf(const QString &path, const QString &name, int *kidCount)
{
    *kidCount = 0;
    QPDF pdf;
    PdfFile::open(pdf, path);
    QPDFObjectHandle field = findField(pdf.getRoot().getKey("/AcroForm").getKey("/Fields"), QString(), name);
    QPDFObjectHandle kids = field.isDictionary() ? field.getKey("/Kids") : QPDFObjectHandle::newNull();
    QStringList states;
    for (int k = 0; kids.isArray() && k < kids.getArrayNItems(); ++k) {
        ++(*kidCount);
        QPDFObjectHandle normal = kids.getArrayItem(k).getKey("/AP").getKey("/N");
        if (!normal.isDictionary()) {
            continue;
        }
        for (const auto &[state, appearance] : normal.getDictAsMap()) {
            Q_UNUSED(appearance)
            if (state != "/Off") {
                states << QString::fromStdString(state);
            }
        }
    }
    return states;
}

/** The /T of every widget on a page, in the order /Annots gives them. */
QStringList widgetOrderOf(const QString &path, int page)
{
    QPDF pdf;
    PdfFile::open(pdf, path);
    std::vector<QPDFPageObjectHelper> pages = QPDFPageDocumentHelper(pdf).getAllPages();
    QStringList names;
    if (size_t(page) >= pages.size()) {
        return names;
    }
    QPDFObjectHandle annots = pages[size_t(page)].getObjectHandle().getKey("/Annots");
    for (int a = 0; annots.isArray() && a < annots.getArrayNItems(); ++a) {
        QPDFObjectHandle annot = annots.getArrayItem(a);
        if (annot.isDictionary() && annot.getKey("/T").isString()) {
            names << QString::fromStdString(annot.getKey("/T").getUTF8Value());
        }
    }
    return names;
}

QStringList calculationOrderOf(const QString &path)
{
    QPDF pdf;
    PdfFile::open(pdf, path);
    QPDFObjectHandle order = pdf.getRoot().getKey("/AcroForm").getKey("/CO");
    QStringList names;
    for (int i = 0; order.isArray() && i < order.getArrayNItems(); ++i) {
        QPDFObjectHandle item = order.getArrayItem(i);
        if (item.isDictionary() && item.getKey("/T").isString()) {
            names << QString::fromStdString(item.getKey("/T").getUTF8Value());
        }
    }
    return names;
}

/** One entry of a field's /AA: the kind of action, and the script it runs. */
QPair<QString, QString> additionalActionOf(const QString &path, const QString &name, const char *which)
{
    QPDF pdf;
    PdfFile::open(pdf, path);
    QPDFObjectHandle field = findField(pdf.getRoot().getKey("/AcroForm").getKey("/Fields"), QString(), name);
    if (!field.isDictionary() || !field.getKey("/AA").isDictionary()) {
        return {};
    }
    QPDFObjectHandle action = field.getKey("/AA").getKey(which);
    if (!action.isDictionary()) {
        return {};
    }
    const QString kind
        = action.getKey("/S").isName() ? QString::fromStdString(action.getKey("/S").getName()) : QString();
    const QString script
        = action.getKey("/JS").isString() ? QString::fromStdString(action.getKey("/JS").getUTF8Value()) : QString();
    return { kind, script };
}

QString valueNameOf(const QString &path, const QString &name)
{
    QPDF pdf;
    PdfFile::open(pdf, path);
    QPDFObjectHandle field = findField(pdf.getRoot().getKey("/AcroForm").getKey("/Fields"), QString(), name);
    QPDFObjectHandle value = field.isDictionary() ? field.getKey("/V") : QPDFObjectHandle::newNull();
    return value.isName() ? QString::fromStdString(value.getName()) : QString();
}

/** The drawn appearance of a field's only widget, as content-stream text. */
QByteArray drawingOf(const QString &path, const QString &name)
{
    QPDF pdf;
    PdfFile::open(pdf, path);
    QPDFObjectHandle field = findField(pdf.getRoot().getKey("/AcroForm").getKey("/Fields"), QString(), name);
    QPDFObjectHandle normal = field.isDictionary() ? field.getKey("/AP").getKey("/N") : QPDFObjectHandle::newNull();
    return normal.isStream() ? streamBytes(normal) : QByteArray();
}

FormBuilder::Field textField(const QString &name, const QRectF &rect)
{
    FormBuilder::Field field;
    field.kind = FormBuilder::Field::Kind::Text;
    field.name = name;
    field.label = name;
    field.rect = rect;
    field.borderColour = QColor(Qt::darkGray);
    field.backgroundColour = QColor(240, 240, 240);
    return field;
}

/** A document whose form is XFA, which is the one shape this must refuse. */
bool writeXfaPdf(const QString &path)
{
    try {
        QPDF pdf;
        pdf.emptyPDF();
        QPDFPageDocumentHelper documents(pdf);
        QPDFObjectHandle page = QPDFObjectHandle::parse("<< /Type /Page /MediaBox [0 0 612 792] >>");
        page.replaceKey("/Resources", QPDFObjectHandle::parse("<< >>"));
        page.replaceKey("/Contents", QPDFObjectHandle::newStream(&pdf, "\n"));
        documents.addPage(QPDFPageObjectHelper(pdf.makeIndirectObject(page)), false);

        QPDFObjectHandle acroForm = QPDFObjectHandle::newDictionary();
        acroForm.replaceKey("/Fields", QPDFObjectHandle::newArray());
        QPDFObjectHandle xfa = QPDFObjectHandle::newArray();
        xfa.appendItem(QPDFObjectHandle::newString("template"));
        xfa.appendItem(pdf.makeIndirectObject(QPDFObjectHandle::newStream(&pdf, "<template/>")));
        acroForm.replaceKey("/XFA", xfa);
        pdf.getRoot().replaceKey("/AcroForm", pdf.makeIndirectObject(acroForm));

        QPDFWriter writer(pdf, QFile::encodeName(path).constData());
        writer.write();
        return true;
    } catch (const std::exception &) {
        return false;
    }
}

} // namespace

/**
 * Making a form, checked by reading the result back out of the file.
 *
 * The failure this guards against is the quiet one: a field that is in the
 * document, has a name and a value, and draws nothing. Every case therefore
 * asserts two things: that the structure is right, and that there is an
 * appearance stream to show it.
 */
class TestFormbuilder : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();

    void makesOneOfEveryKindAndReadsThemBack();
    void everyFieldItMakesIsDrawn();
    void aTickBoxKnowsExactlyTwoStates();
    void aRadioGroupIsOneFieldWithSeveralKids();
    void putsAFieldWhereAskedOnATurnedPage();
    void groupsFieldsWhoseNamesAreDotted();
    void changesAFieldWithoutMovingIt();
    void removesFieldsFromBothPlacesTheyLive();
    void renamesInPlaceButRefusesToMove();
    void setsTheOrderTheTabKeyWalks();
    void formattingIsJavaScriptAndSaysSo();
    void validationIsWrittenWithoutAskingTheLocale();
    void calculationsRunAfterWhatTheyDependOn();
    void givesAButtonSomethingToDo();
    void copiesAFormOntoAnotherDocument();
    void takesDataOutAndPutsItBack();
    void collectsManyFilledCopiesIntoATable();
    void refusesADocumentWithAnXfaForm();
    void saysWhatItCannotPromise();

private:
    QVector<FormBuilder::Field> everyKind() const;

    QTemporaryDir m_dir;
    QString m_blank;
    QString m_form; //!< one of every kind, built once
};

void TestFormbuilder::initTestCase()
{
    KLocalizedString::setApplicationDomain("pdf-smithy");
    QVERIFY(m_dir.isValid());

    m_blank = m_dir.filePath(QStringLiteral("blank.pdf"));
    QVERIFY(test::writeSamplePdf(m_blank, 2));

    m_form = m_dir.filePath(QStringLiteral("built.pdf"));
    int added = 0;
    QString error;
    QVERIFY2(FormBuilder::addFields(m_blank, m_form, everyKind(), &added, &error), qPrintable(error));
    QCOMPARE(added, int(everyKind().size()));
}

QVector<FormBuilder::Field> TestFormbuilder::everyKind() const
{
    QVector<FormBuilder::Field> fields;

    FormBuilder::Field name = textField(QStringLiteral("Name"), QRectF(72, 700, 300, 24));
    name.label = QStringLiteral("Ihr Name");
    name.required = true;
    fields << name;

    FormBuilder::Field remark = textField(QStringLiteral("Bemerkung"), QRectF(72, 600, 300, 80));
    remark.multiline = true;
    fields << remark;

    FormBuilder::Field locked = textField(QStringLiteral("Aktenzeichen"), QRectF(72, 560, 200, 24));
    locked.readOnly = true;
    locked.defaultValue = QStringLiteral("AZ-2026-001");
    fields << locked;

    FormBuilder::Field tick;
    tick.kind = FormBuilder::Field::Kind::Checkbox;
    tick.name = QStringLiteral("Einverstanden");
    tick.label = QStringLiteral("Ich bin einverstanden");
    tick.rect = QRectF(72, 520, 18, 18);
    tick.onState = QStringLiteral("/Ja");
    tick.borderColour = QColor(Qt::black);
    fields << tick;

    for (int i = 0; i < 2; ++i) {
        FormBuilder::Field option;
        option.kind = FormBuilder::Field::Kind::Radio;
        option.name = i == 0 ? QStringLiteral("Herr") : QStringLiteral("Frau");
        option.label = option.name;
        option.radioGroup = QStringLiteral("Anrede");
        option.rect = QRectF(72 + i * 30, 480, 18, 18);
        option.onState = option.name;
        option.borderColour = QColor(Qt::black);
        fields << option;
    }

    FormBuilder::Field country;
    country.kind = FormBuilder::Field::Kind::Dropdown;
    country.name = QStringLiteral("Land");
    country.label = QStringLiteral("Land");
    country.rect = QRectF(72, 440, 200, 24);
    country.options = { QStringLiteral("Schweiz"), QStringLiteral("Deutschland"), QStringLiteral("Österreich") };
    country.borderColour = QColor(Qt::darkGray);
    fields << country;

    FormBuilder::Field colours;
    colours.kind = FormBuilder::Field::Kind::ListBox;
    colours.name = QStringLiteral("Farben");
    colours.label = QStringLiteral("Farben");
    colours.rect = QRectF(72, 360, 200, 60);
    colours.options = { QStringLiteral("Rot"), QStringLiteral("Grün") };
    colours.multiSelect = true;
    colours.borderColour = QColor(Qt::darkGray);
    fields << colours;

    FormBuilder::Field press;
    press.kind = FormBuilder::Field::Kind::PushButton;
    press.name = QStringLiteral("Zuruecksetzen");
    press.label = QStringLiteral("Leeren");
    press.rect = QRectF(72, 320, 90, 24);
    press.borderColour = QColor(Qt::black);
    press.backgroundColour = QColor(220, 220, 220);
    press.borderStyle = QStringLiteral("beveled");
    fields << press;

    FormBuilder::Field signature;
    signature.kind = FormBuilder::Field::Kind::Signature;
    signature.name = QStringLiteral("Unterschrift");
    signature.label = QStringLiteral("Unterschrift");
    signature.rect = QRectF(72, 250, 200, 50);
    signature.borderColour = QColor(Qt::black);
    signature.borderStyle = QStringLiteral("dashed");
    fields << signature;

    return fields;
}

void TestFormbuilder::makesOneOfEveryKindAndReadsThemBack()
{
    QVERIFY(Forms::hasForm(m_form));

    QString error;
    const QVector<FormField> fields = Forms::read(m_form, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));

    const auto byName = [&fields](const QString &name) -> FormField {
        for (const FormField &field : fields) {
            if (field.name == name) {
                return field;
            }
        }
        return {};
    };

    const FormField name = byName(QStringLiteral("Name"));
    QCOMPARE(name.kind, FormField::Kind::Text);
    QCOMPARE(name.label, QStringLiteral("Ihr Name"));
    QVERIFY2(name.required, "the required flag did not survive");
    QVERIFY(!name.readOnly);
    QCOMPARE(name.page, 0);

    QVERIFY2(byName(QStringLiteral("Bemerkung")).multiline, "the multi-line flag did not survive");

    const FormField locked = byName(QStringLiteral("Aktenzeichen"));
    QVERIFY2(locked.readOnly, "the locked flag did not survive");
    QCOMPARE(locked.value, QStringLiteral("AZ-2026-001"));

    QCOMPARE(byName(QStringLiteral("Einverstanden")).kind, FormField::Kind::Checkbox);
    QCOMPARE(byName(QStringLiteral("Einverstanden")).value, QStringLiteral("/Off"));
    QCOMPARE(byName(QStringLiteral("Anrede")).kind, FormField::Kind::Radio);
    QCOMPARE(byName(QStringLiteral("Unterschrift")).kind, FormField::Kind::Signature);
    QCOMPARE(byName(QStringLiteral("Zuruecksetzen")).kind, FormField::Kind::Button);

    const FormField country = byName(QStringLiteral("Land"));
    QCOMPARE(country.kind, FormField::Kind::Choice);
    QCOMPARE(country.options.size(), 3);
    QVERIFY(country.options.contains(QStringLiteral("Österreich")));
    QCOMPARE(byName(QStringLiteral("Farben")).kind, FormField::Kind::Choice);

    // The label a person sees is /TU, and every one of them came back.
    for (const FormField &field : fields) {
        QVERIFY2(!field.label.isEmpty(), qPrintable(field.name));
    }
}

void TestFormbuilder::everyFieldItMakesIsDrawn()
{
    int widgets = 0;
    int drawn = 0;
    appearanceTally(m_form, &widgets, &drawn);
    QCOMPARE(widgets, int(everyKind().size()));
    QCOMPARE(drawn, widgets);

    QPDF pdf;
    PdfFile::open(pdf, m_form);
    // A reader with an appearance generator of its own is still invited to use it.
    QPDFObjectHandle needed = pdf.getRoot().getKey("/AcroForm").getKey("/NeedAppearances");
    QVERIFY2(needed.isBool() && needed.getBoolValue(), "/NeedAppearances was not left set");

    // /DR has to carry the font that /DA names, or the text draws with no font.
    QPDFObjectHandle resources = pdf.getRoot().getKey("/AcroForm").getKey("/DR");
    QVERIFY(resources.getKey("/Font").isDictionary());
    QVERIFY(resources.getKey("/Font").getKey("/Helv").isDictionary());

    // And the other half of the proof: the field really is fillable.
    const QString filled = m_dir.filePath(QStringLiteral("filled.pdf"));
    int count = 0;
    QString error;
    QVERIFY2(Forms::fill(m_form, filled, { { QStringLiteral("Name"), QStringLiteral("Tom Bueng") } }, &count, nullptr,
                         &error),
             qPrintable(error));
    QCOMPARE(count, 1);

    bool found = false;
    for (const FormField &field : Forms::read(filled, &error)) {
        if (field.name == QLatin1String("Name")) {
            QCOMPARE(field.value, QStringLiteral("Tom Bueng"));
            found = true;
        }
    }
    QVERIFY(found);

    // Not only in the field dictionary: in the drawing, which is what prints.
    const QByteArray drawing = drawingOf(filled, QStringLiteral("Name"));
    QVERIFY2(drawing.contains(QByteArrayLiteral("Tom Bueng")), drawing.constData());
    // The frame was drawn outside the marked content, so a reader regenerating
    // the value did not take the border with it.
    QVERIFY2(drawing.contains(QByteArrayLiteral(" re")), drawing.constData());
}

void TestFormbuilder::aTickBoxKnowsExactlyTwoStates()
{
    // Exactly the on-state and /Off. Anything else and the box holds a value it
    // has no drawing for, which looks like a broken reader.
    QCOMPARE(appearanceStatesOf(m_form, QStringLiteral("Einverstanden")),
             QStringList({ QStringLiteral("/Ja"), QStringLiteral("/Off") }));

    const QString ticked = m_dir.filePath(QStringLiteral("ticked.pdf"));
    int count = 0;
    QString error;
    QVERIFY2(Forms::fill(m_form, ticked, { { QStringLiteral("Einverstanden"), QStringLiteral("yes") } }, &count, nullptr,
                         &error),
             qPrintable(error));
    QCOMPARE(count, 1);
    QCOMPARE(valueNameOf(ticked, QStringLiteral("Einverstanden")), QStringLiteral("/Ja"));

    for (const FormField &field : Forms::read(ticked, &error)) {
        if (field.name == QLatin1String("Einverstanden")) {
            QCOMPARE(field.value, QStringLiteral("/Ja"));
        }
    }
}

void TestFormbuilder::aRadioGroupIsOneFieldWithSeveralKids()
{
    const QString out = m_dir.filePath(QStringLiteral("radios.pdf"));
    QVector<FormBuilder::Field> fields;
    const QStringList states { QStringLiteral("/Herr"), QStringLiteral("/Frau"), QStringLiteral("/Divers") };
    for (int i = 0; i < 3; ++i) {
        FormBuilder::Field option;
        option.kind = FormBuilder::Field::Kind::Radio;
        option.name = states.at(i).mid(1);
        option.label = option.name;
        option.radioGroup = QStringLiteral("Anrede");
        option.rect = QRectF(72, 700 - i * 30, 18, 18);
        option.onState = states.at(i);
        option.borderColour = QColor(Qt::black);
        fields << option;
    }

    int added = 0;
    QString error;
    QVERIFY2(FormBuilder::addFields(m_blank, out, fields, &added, &error), qPrintable(error));
    QCOMPARE(added, 3);

    // One field…
    QPDF pdf;
    PdfFile::open(pdf, out);
    QCOMPARE(pdf.getRoot().getKey("/AcroForm").getKey("/Fields").getArrayNItems(), 1);

    // …with three kids, each with an on-state of its own. Three separate fields
    // instead would give three buttons that all toggle together.
    int kids = 0;
    QStringList onStates = kidStatesOf(out, QStringLiteral("Anrede"), &kids);
    QCOMPARE(kids, 3);
    onStates.sort();
    QCOMPARE(onStates, QStringList({ QStringLiteral("/Divers"), QStringLiteral("/Frau"), QStringLiteral("/Herr") }));

    int widgets = 0;
    int drawn = 0;
    appearanceTally(out, &widgets, &drawn);
    QCOMPARE(widgets, 3);
    QCOMPARE(drawn, 3);

    // And one button can be chosen by name.
    const QString chosen = m_dir.filePath(QStringLiteral("radios-chosen.pdf"));
    int count = 0;
    QVERIFY2(
        Forms::fill(out, chosen, { { QStringLiteral("Anrede"), QStringLiteral("/Frau") } }, &count, nullptr, &error),
        qPrintable(error));
    QCOMPARE(valueNameOf(chosen, QStringLiteral("Anrede")), QStringLiteral("/Frau"));
}

void TestFormbuilder::putsAFieldWhereAskedOnATurnedPage()
{
    const QString turned = m_dir.filePath(QStringLiteral("turned.pdf"));
    QVERIFY(test::writeRotatedPdf(turned, 1, 90));

    const QString out = m_dir.filePath(QStringLiteral("turned-form.pdf"));
    int added = 0;
    QString error;
    QVERIFY2(FormBuilder::addFields(turned, out, { textField(QStringLiteral("Name"), QRectF(100, 200, 180, 20)) },
                                    &added, &error),
             qPrintable(error));
    QCOMPARE(added, 1);

    // Forms::read maps a widget back into displayed coordinates, so asking for a
    // box in display points and getting the same box back is the whole contract.
    bool seen = false;
    for (const FormField &field : Forms::read(out, &error)) {
        if (field.name != QLatin1String("Name")) {
            continue;
        }
        seen = true;
        QVERIFY2(qAbs(field.rect.x() - 100.0) < 1.0, qPrintable(QString::number(field.rect.x())));
        QVERIFY2(qAbs(field.rect.y() - 200.0) < 1.0, qPrintable(QString::number(field.rect.y())));
        QVERIFY2(qAbs(field.rect.width() - 180.0) < 1.0, qPrintable(QString::number(field.rect.width())));
    }
    QVERIFY(seen);

    int widgets = 0;
    int drawn = 0;
    appearanceTally(out, &widgets, &drawn);
    QCOMPARE(drawn, 1);
}

void TestFormbuilder::groupsFieldsWhoseNamesAreDotted()
{
    const QString out = m_dir.filePath(QStringLiteral("grouped.pdf"));
    QVector<FormBuilder::Field> fields;
    fields << textField(QStringLiteral("Adresse.Strasse"), QRectF(72, 700, 200, 20));
    fields << textField(QStringLiteral("Adresse.Ort"), QRectF(72, 660, 200, 20));

    int added = 0;
    QString error;
    QVERIFY2(FormBuilder::addFields(m_blank, out, fields, &added, &error), qPrintable(error));
    QCOMPARE(added, 2);

    // One parent in /Fields, two fields underneath, and the names that address
    // them are the dotted ones.
    QPDF pdf;
    PdfFile::open(pdf, out);
    QCOMPARE(pdf.getRoot().getKey("/AcroForm").getKey("/Fields").getArrayNItems(), 1);

    QStringList names;
    for (const FormField &field : Forms::read(out, &error)) {
        names << field.name;
    }
    names.sort();
    QCOMPARE(names, QStringList({ QStringLiteral("Adresse.Ort"), QStringLiteral("Adresse.Strasse") }));

    const QString filled = m_dir.filePath(QStringLiteral("grouped-filled.pdf"));
    int count = 0;
    QVERIFY2(Forms::fill(out, filled, { { QStringLiteral("Adresse.Ort"), QStringLiteral("Zürich") } }, &count, nullptr,
                         &error),
             qPrintable(error));
    QCOMPARE(count, 1);
}

void TestFormbuilder::changesAFieldWithoutMovingIt()
{
    FormBuilder::Field changed = textField(QStringLiteral("Name"), QRectF());
    changed.label = QStringLiteral("Vor- und Nachname");
    changed.readOnly = true;
    changed.maxLength = 40;

    const QString out = m_dir.filePath(QStringLiteral("changed.pdf"));
    int updated = 0;
    QString error;
    QVERIFY2(FormBuilder::updateFields(m_form, out, { changed }, &updated, &error), qPrintable(error));
    QCOMPARE(updated, 1);

    bool seen = false;
    for (const FormField &field : Forms::read(out, &error)) {
        if (field.name != QLatin1String("Name")) {
            continue;
        }
        seen = true;
        QCOMPARE(field.label, QStringLiteral("Vor- und Nachname"));
        QVERIFY(field.readOnly);
        QVERIFY(!field.required);
        // An empty rectangle means "leave it where it is", not "move it nowhere".
        QVERIFY2(qAbs(field.rect.x() - 72.0) < 1.0, qPrintable(QString::number(field.rect.x())));
        QVERIFY2(qAbs(field.rect.width() - 300.0) < 1.0, qPrintable(QString::number(field.rect.width())));
    }
    QVERIFY(seen);

    // A name nobody has is not counted, which is what makes the count worth
    // comparing against the number asked for.
    FormBuilder::Field missing = textField(QStringLiteral("Naem"), QRectF(1, 1, 10, 10));
    updated = 0;
    QVERIFY2(FormBuilder::updateFields(m_form, m_dir.filePath(QStringLiteral("nothing.pdf")), { missing }, &updated,
                                       &error),
             qPrintable(error));
    QCOMPARE(updated, 0);
}

void TestFormbuilder::removesFieldsFromBothPlacesTheyLive()
{
    const QString out = m_dir.filePath(QStringLiteral("removed.pdf"));
    int removed = 0;
    QString error;
    QVERIFY2(FormBuilder::removeFields(m_form, out, { QStringLiteral("Bemerkung"), QStringLiteral("Anrede") }, &removed,
                                       &error),
             qPrintable(error));
    QCOMPARE(removed, 2);

    QStringList names;
    for (const FormField &field : Forms::read(out, &error)) {
        names << field.name;
    }
    QVERIFY(!names.contains(QStringLiteral("Bemerkung")));
    QVERIFY(!names.contains(QStringLiteral("Anrede")));
    QVERIFY(names.contains(QStringLiteral("Name")));

    // Out of the pages too: a widget left in /Annots with no field behind it is
    // what makes a reader offer to repair the document.
    int widgets = 0;
    int drawn = 0;
    appearanceTally(out, &widgets, &drawn);
    QCOMPARE(widgets, int(everyKind().size()) - 3); // the remark, and both radio buttons
    QCOMPARE(drawn, widgets);
}

void TestFormbuilder::renamesInPlaceButRefusesToMove()
{
    const QString out = m_dir.filePath(QStringLiteral("renamed.pdf"));
    QString error;
    QVERIFY2(FormBuilder::renameField(m_form, out, QStringLiteral("Name"), QStringLiteral("Nachname"), &error),
             qPrintable(error));

    QStringList names;
    for (const FormField &field : Forms::read(out, &error)) {
        names << field.name;
    }
    QVERIFY(names.contains(QStringLiteral("Nachname")));
    QVERIFY(!names.contains(QStringLiteral("Name")));

    int count = 0;
    QVERIFY2(Forms::fill(out, m_dir.filePath(QStringLiteral("renamed-filled.pdf")),
                         { { QStringLiteral("Nachname"), QStringLiteral("Bueng") } }, &count, nullptr, &error),
             qPrintable(error));
    QCOMPARE(count, 1);

    // Into another group is a move, not a rename, and is refused rather than
    // guessed at.
    error.clear();
    QVERIFY(!FormBuilder::renameField(m_form, m_dir.filePath(QStringLiteral("moved.pdf")), QStringLiteral("Name"),
                                      QStringLiteral("Person.Name"), &error));
    QVERIFY(!error.isEmpty());

    // And onto a name that is taken.
    error.clear();
    QVERIFY(!FormBuilder::renameField(m_form, m_dir.filePath(QStringLiteral("clash.pdf")), QStringLiteral("Name"),
                                      QStringLiteral("Land"), &error));
    QVERIFY(!error.isEmpty());
}

void TestFormbuilder::setsTheOrderTheTabKeyWalks()
{
    const QStringList before = widgetOrderOf(m_form, 0);
    QVERIFY(before.contains(QStringLiteral("Name")));
    QVERIFY(before.indexOf(QStringLiteral("Name")) < before.indexOf(QStringLiteral("Land")));

    const QString out = m_dir.filePath(QStringLiteral("tabbed.pdf"));
    QString error;
    QVERIFY2(FormBuilder::setTabOrder(
                 m_form, out, 0,
                 { QStringLiteral("Land"), QStringLiteral("Aktenzeichen"), QStringLiteral("Name") }, &error),
             qPrintable(error));

    const QStringList after = widgetOrderOf(out, 0);
    QCOMPARE(after.mid(0, 3),
             QStringList({ QStringLiteral("Land"), QStringLiteral("Aktenzeichen"), QStringLiteral("Name") }));
    // Nothing was dropped on the way: the rest follow in the order they had.
    QCOMPARE(after.size(), before.size());

    error.clear();
    QVERIFY(!FormBuilder::setTabOrder(m_form, m_dir.filePath(QStringLiteral("nope.pdf")), 0,
                                      { QStringLiteral("Naem") }, &error));
    QVERIFY(!error.isEmpty());
}

void TestFormbuilder::formattingIsJavaScriptAndSaysSo()
{
    const QString out = m_dir.filePath(QStringLiteral("formatted.pdf"));
    QStringList warnings;
    QString error;
    QVERIFY2(FormBuilder::setFormat(m_form, out, QStringLiteral("Name"), FormBuilder::Format::Currency, 2,
                                    QStringLiteral("EUR"), &warnings, &error),
             qPrintable(error));

    const QPair<QString, QString> format = additionalActionOf(out, QStringLiteral("Name"), "/F");
    QCOMPARE(format.first, QStringLiteral("/JavaScript"));
    QVERIFY2(format.second.contains(QStringLiteral("AFNumber_Format")), qPrintable(format.second));
    QVERIFY2(format.second.contains(QStringLiteral("EUR")), qPrintable(format.second));

    // Formatting that only tidies up when focus leaves fools nobody, so the
    // keystroke half has to be there too.
    QCOMPARE(additionalActionOf(out, QStringLiteral("Name"), "/K").first, QStringLiteral("/JavaScript"));

    QVERIFY2(!warnings.isEmpty(), "a script was added without a word about what it costs");
    QVERIFY2(warnings.join(QLatin1Char(' ')).contains(QStringLiteral("PDF/A")),
             qPrintable(warnings.join(QLatin1Char('|'))));
    QCOMPARE(warnings.size(), 2); // what it costs an archive, and that readers vary

    // And taking it off again leaves nothing behind.
    const QString plain = m_dir.filePath(QStringLiteral("unformatted.pdf"));
    warnings.clear();
    QVERIFY2(FormBuilder::setFormat(out, plain, QStringLiteral("Name"), FormBuilder::Format::None, 0, QString(),
                                    &warnings, &error),
             qPrintable(error));
    QCOMPARE(additionalActionOf(plain, QStringLiteral("Name"), "/F").first, QString());
    QVERIFY(warnings.isEmpty());
}

void TestFormbuilder::validationIsWrittenWithoutAskingTheLocale()
{
    const QString out = m_dir.filePath(QStringLiteral("validated.pdf"));
    QStringList warnings;
    QString error;
    QVERIFY2(FormBuilder::setValidation(m_form, out, QStringLiteral("Name"), 1.5, 99.25, &warnings, &error),
             qPrintable(error));

    const QPair<QString, QString> validate = additionalActionOf(out, QStringLiteral("Name"), "/V");
    QCOMPARE(validate.first, QStringLiteral("/JavaScript"));
    // A comma for the decimal point would be a script with two extra arguments
    // that refuses everything, which is the locale bug this project has had once.
    QVERIFY2(validate.second.contains(QStringLiteral("1.5000")), qPrintable(validate.second));
    QVERIFY2(validate.second.contains(QStringLiteral("99.2500")), qPrintable(validate.second));
    QCOMPARE(validate.second.count(QLatin1Char(',')), 3);
    QVERIFY(!warnings.isEmpty());

    error.clear();
    QVERIFY(!FormBuilder::setValidation(m_form, m_dir.filePath(QStringLiteral("nope2.pdf")), QStringLiteral("Name"),
                                        10.0, 1.0, nullptr, &error));
    QVERIFY(!error.isEmpty());
}

void TestFormbuilder::calculationsRunAfterWhatTheyDependOn()
{
    const QString sheet = m_dir.filePath(QStringLiteral("sheet.pdf"));
    QVector<FormBuilder::Field> fields;
    fields << textField(QStringLiteral("A"), QRectF(72, 700, 80, 20));
    fields << textField(QStringLiteral("B"), QRectF(72, 670, 80, 20));
    fields << textField(QStringLiteral("Summe"), QRectF(72, 640, 80, 20));
    fields << textField(QStringLiteral("Doppelt"), QRectF(72, 610, 80, 20));

    int added = 0;
    QString error;
    QVERIFY2(FormBuilder::addFields(m_blank, sheet, fields, &added, &error), qPrintable(error));

    const QString first = m_dir.filePath(QStringLiteral("sum.pdf"));
    QStringList warnings;
    QVERIFY2(FormBuilder::setCalculation(sheet, first, QStringLiteral("Summe"), FormBuilder::Calculation::Sum,
                                         { QStringLiteral("A"), QStringLiteral("B") }, &warnings, &error),
             qPrintable(error));
    QVERIFY(!warnings.isEmpty());

    const QString second = m_dir.filePath(QStringLiteral("sum2.pdf"));
    QVERIFY2(FormBuilder::setCalculation(first, second, QStringLiteral("Doppelt"), FormBuilder::Calculation::Product,
                                         { QStringLiteral("Summe"), QStringLiteral("A") }, nullptr, &error),
             qPrintable(error));

    // /CO is the order the calculations run in. A field that works from another
    // calculated field has to come after it, or it works from last time's answer.
    QCOMPARE(calculationOrderOf(second), QStringList({ QStringLiteral("Summe"), QStringLiteral("Doppelt") }));

    const QPair<QString, QString> calculate = additionalActionOf(second, QStringLiteral("Summe"), "/C");
    QCOMPARE(calculate.first, QStringLiteral("/JavaScript"));
    QVERIFY2(calculate.second.contains(QStringLiteral("AFSimple_CALC(\"SUM\"")), qPrintable(calculate.second));

    // A source that is not in the document would be a calculation that silently
    // produces nothing.
    error.clear();
    QVERIFY(!FormBuilder::setCalculation(second, m_dir.filePath(QStringLiteral("nope3.pdf")), QStringLiteral("Summe"),
                                         FormBuilder::Calculation::Sum, { QStringLiteral("C") }, nullptr, &error));
    QVERIFY(!error.isEmpty());
}

void TestFormbuilder::givesAButtonSomethingToDo()
{
    const QString out = m_dir.filePath(QStringLiteral("button.pdf"));
    QString error;
    QVERIFY2(FormBuilder::setButtonAction(m_form, out, QStringLiteral("Zuruecksetzen"),
                                          FormBuilder::ButtonAction::ResetForm, QString(), &error),
             qPrintable(error));

    QPDF pdf;
    PdfFile::open(pdf, out);
    QPDFObjectHandle field
        = findField(pdf.getRoot().getKey("/AcroForm").getKey("/Fields"), QString(), QStringLiteral("Zuruecksetzen"));
    QVERIFY(field.isDictionary());
    QCOMPARE(QString::fromStdString(field.getKey("/A").getKey("/S").getName()), QStringLiteral("/ResetForm"));

    const QString linked = m_dir.filePath(QStringLiteral("button-url.pdf"));
    QVERIFY2(FormBuilder::setButtonAction(m_form, linked, QStringLiteral("Zuruecksetzen"),
                                          FormBuilder::ButtonAction::OpenUrl,
                                          QStringLiteral("https://example.invalid/form"), &error),
             qPrintable(error));
    QPDF second;
    PdfFile::open(second, linked);
    QPDFObjectHandle button
        = findField(second.getRoot().getKey("/AcroForm").getKey("/Fields"), QString(), QStringLiteral("Zuruecksetzen"));
    QCOMPARE(QString::fromStdString(button.getKey("/A").getKey("/URI").getUTF8Value()),
             QStringLiteral("https://example.invalid/form"));

    // A page that is not there is a button that does nothing, so it is refused.
    error.clear();
    QVERIFY(!FormBuilder::setButtonAction(m_form, m_dir.filePath(QStringLiteral("nope4.pdf")),
                                          QStringLiteral("Zuruecksetzen"), FormBuilder::ButtonAction::GoToPage,
                                          QStringLiteral("99"), &error));
    QVERIFY(!error.isEmpty());
}

void TestFormbuilder::copiesAFormOntoAnotherDocument()
{
    const QString plain = m_dir.filePath(QStringLiteral("plain.pdf"));
    QVERIFY(test::writeSamplePdf(plain, 2));

    const QString out = m_dir.filePath(QStringLiteral("copied.pdf"));
    int copied = 0;
    QString error;
    QVERIFY2(FormBuilder::copyFieldsFrom(m_form, plain, out, &copied, &error), qPrintable(error));
    QVERIFY(copied > 0);

    // The pages did not come with them, which is what stripping /P before the
    // copy is for.
    QCOMPARE(test::pageCountOf(out), 2);

    QStringList names;
    for (const FormField &field : Forms::read(out, &error)) {
        names << field.name;
    }
    QVERIFY(names.contains(QStringLiteral("Name")));
    QVERIFY(names.contains(QStringLiteral("Anrede")));

    int widgets = 0;
    int drawn = 0;
    appearanceTally(out, &widgets, &drawn);
    QCOMPARE(widgets, int(everyKind().size()));
    QCOMPARE(drawn, widgets);

    int count = 0;
    QVERIFY2(Forms::fill(out, m_dir.filePath(QStringLiteral("copied-filled.pdf")),
                         { { QStringLiteral("Name"), QStringLiteral("Tom") } }, &count, nullptr, &error),
             qPrintable(error));
    QCOMPARE(count, 1);

    // Copying the same form twice would give two fields of one name, which is a
    // form nobody can fill in predictably.
    error.clear();
    QVERIFY(!FormBuilder::copyFieldsFrom(m_form, out, m_dir.filePath(QStringLiteral("twice.pdf")), nullptr, &error));
    QVERIFY(!error.isEmpty());
}

void TestFormbuilder::takesDataOutAndPutsItBack()
{
    const QString filled = m_dir.filePath(QStringLiteral("data-filled.pdf"));
    int count = 0;
    QString error;
    QVERIFY2(Forms::fill(m_form, filled,
                         { { QStringLiteral("Name"), QStringLiteral("Tom Bueng") },
                           { QStringLiteral("Land"), QStringLiteral("Österreich") },
                           { QStringLiteral("Einverstanden"), QStringLiteral("ja") } },
                         &count, nullptr, &error),
             qPrintable(error));
    QCOMPARE(count, 3);

    const QStringList kinds { QStringLiteral("csv"), QStringLiteral("xfdf"), QStringLiteral("fdf") };
    for (const QString &kind : kinds) {
        const QString data = m_dir.filePath(QStringLiteral("data.") + kind);
        QVERIFY2(FormBuilder::exportData(filled, data, &error), qPrintable(error));
        QVERIFY(QFileInfo(data).size() > 0);

        const QString back = m_dir.filePath(QStringLiteral("back-") + kind + QStringLiteral(".pdf"));
        int imported = 0;
        QVERIFY2(FormBuilder::importData(m_form, back, data, &imported, &error), qPrintable(error));
        QVERIFY2(imported >= 3, qPrintable(kind + QLatin1Char(' ') + QString::number(imported)));

        bool sawName = false;
        bool sawCountry = false;
        bool sawTick = false;
        for (const FormField &field : Forms::read(back, &error)) {
            if (field.name == QLatin1String("Name")) {
                QCOMPARE(field.value, QStringLiteral("Tom Bueng"));
                sawName = true;
            }
            if (field.name == QLatin1String("Land")) {
                QCOMPARE(field.value, QStringLiteral("Österreich"));
                sawCountry = true;
            }
            if (field.name == QLatin1String("Einverstanden")) {
                // A tick box's exported value is its own state name, and it has to
                // come back as that state rather than as "not one of the words
                // meaning yes, so off".
                QCOMPARE(field.value, QStringLiteral("/Ja"));
                sawTick = true;
            }
        }
        QVERIFY2(sawName, qPrintable(kind));
        QVERIFY2(sawCountry, qPrintable(kind));
        QVERIFY2(sawTick, qPrintable(kind));
    }

    // A file kind nobody can read is said so rather than half-written.
    error.clear();
    QVERIFY(!FormBuilder::exportData(filled, m_dir.filePath(QStringLiteral("data.xlsx")), &error));
    QVERIFY(!error.isEmpty());
}

void TestFormbuilder::collectsManyFilledCopiesIntoATable()
{
    QStringList copies;
    QString error;
    for (int i = 0; i < 3; ++i) {
        const QString copy = m_dir.filePath(QStringLiteral("copy-%1.pdf").arg(i));
        int count = 0;
        QVERIFY2(Forms::fill(m_form, copy,
                             { { QStringLiteral("Name"), QStringLiteral("Person %1").arg(i) },
                               { QStringLiteral("Land"), QStringLiteral("Schweiz") } },
                             &count, nullptr, &error),
                 qPrintable(error));
        copies << copy;
    }

    const QString table = m_dir.filePath(QStringLiteral("table.csv"));
    QVERIFY2(FormBuilder::collect(copies, table, &error), qPrintable(error));

    QFile file(table);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QStringList lines = QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    QCOMPARE(lines.size(), 4); // a heading and one row per document
    QVERIFY2(lines.constFirst().contains(QStringLiteral("Name")), qPrintable(lines.constFirst()));
    QVERIFY2(lines.at(1).contains(QStringLiteral("Person 0")), qPrintable(lines.at(1)));
    QVERIFY2(lines.at(3).contains(QStringLiteral("Person 2")), qPrintable(lines.at(3)));
}

void TestFormbuilder::refusesADocumentWithAnXfaForm()
{
    const QString xfa = m_dir.filePath(QStringLiteral("xfa.pdf"));
    QVERIFY(writeXfaPdf(xfa));

    QString error;
    QVERIFY2(!FormBuilder::addFields(xfa, m_dir.filePath(QStringLiteral("xfa-out.pdf")),
                                     { textField(QStringLiteral("Name"), QRectF(72, 700, 100, 20)) }, nullptr, &error),
             "a field was added to an XFA form");
    QVERIFY2(!error.isEmpty(), "an XFA form was refused without saying why");

    error.clear();
    QVERIFY(!FormBuilder::setFormat(xfa, m_dir.filePath(QStringLiteral("xfa-out2.pdf")), QStringLiteral("Name"),
                                    FormBuilder::Format::Number, 2, QString(), nullptr, &error));
    QVERIFY(!error.isEmpty());
}

void TestFormbuilder::saysWhatItCannotPromise()
{
    const QStringList limits = FormBuilder::limitations();
    QVERIFY(limits.size() >= 5);
    for (const QString &limit : limits) {
        QVERIFY(!limit.isEmpty());
    }
    QVERIFY2(limits.join(QLatin1Char(' ')).contains(QStringLiteral("PDF/A")),
             "the limitations do not say what a script costs an archive");
    QVERIFY2(limits.join(QLatin1Char(' ')).contains(QStringLiteral("XFA")),
             "the limitations do not mention the one form it refuses");
}

QTEST_MAIN(TestFormbuilder)

#include "tst_formbuilder.moc"
